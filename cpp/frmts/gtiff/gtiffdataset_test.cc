// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests GeoTIFF dataset lifecycle, directory scanning, virtual/direct/threaded
// raster I/O, volatile codec parameters, and raw binary layout
// (gtiffdataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffdataset.cpp

#include "third_party/gdal/frmts/gtiff/gtiffdataset.h"

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif  // __linux__

#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

#ifdef __linux__
// ---------------------------------------------------------------------------
// 100% RAM memfd-backed VSI filesystem ("/vsimem_ds_fd/") for VirtualMemIO.
// ---------------------------------------------------------------------------
class DsMemFdHandler final : public VSIFilesystemHandler {
 public:
  ~DsMemFdHandler() override {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& kv : fds_) {
      close(kv.second);
    }
  }

  static std::string ProcPath(int fd) {
    return std::format("/proc/self/fd/{}", fd);
  }

  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    int fd = -1;
    std::string_view access(pszAccess);
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = fds_.find(pszFilename);
      const bool create = access.find('w') != std::string_view::npos ||
                          access.find('a') != std::string_view::npos;
      if (it == fds_.end()) {
        if (!create) return nullptr;
        fd = memfd_create("gtiff_ds_vmem", 0);
        if (fd < 0) return nullptr;
        fds_[pszFilename] = fd;
      } else {
        fd = it->second;
      }
      // Any "w" mode truncates (a no-op on a freshly created memfd).
      if (access.find('w') != std::string_view::npos) {
        (void)ftruncate(fd, 0);
      }
    }
    const char* delegated_access =
        (access == "wb+" || access == "w+b") ? "r+b" : pszAccess;
    return VSIFileManager::GetHandler("")->Open(
        ProcPath(fd).c_str(), delegated_access, bSetError, papszOptions);
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int nFlags) override {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = fds_.find(pszFilename);
      if (it == fds_.end()) return -1;
      fd = it->second;
    }
    return VSIFileManager::GetHandler("")->Stat(ProcPath(fd).c_str(), pStatBuf,
                                                nFlags);
  }

  int Unlink(const char* pszFilename) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = fds_.find(pszFilename);
    if (it == fds_.end()) return -1;
    close(it->second);
    fds_.erase(it);
    return 0;
  }

 private:
  std::mutex mu_;
  std::map<std::string, int> fds_;
};
#endif  // __linux__

// ---------------------------------------------------------------------------
// 100% RAM wrapper around /vsimem/ ("/vsimem_ds_mr/") with configurable
// HasOptimizedReadMultiRange, HasPRead, and Close() error injection.
// ---------------------------------------------------------------------------
class DsMultiRangeHandler;

class DsMultiRangeHandle final : public VSIProxyFileHandle {
 public:
  DsMultiRangeHandle(VSIVirtualHandleUniquePtr&& inner,
                     const DsMultiRangeHandler* handler)
      : VSIProxyFileHandle(std::move(inner)), handler_(handler) {}

  bool HasPRead() const override;
  int Close() override;

 private:
  const DsMultiRangeHandler* handler_;
};

class DsMultiRangeHandler final : public VSIFilesystemHandler {
 public:
  static constexpr std::string_view kPrefix = "/vsimem_ds_mr/";

  static std::string ToMem(std::string_view path) {
    if (path.starts_with(kPrefix)) {
      return std::format("/vsimem/ds_mr_backing_{}",
                         path.substr(kPrefix.size()));
    }
    return std::string(path);
  }

  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    const std::string mem = ToMem(pszFilename);
    auto inner = VSIFileManager::GetHandler("/vsimem/")
                     ->Open(mem.c_str(), pszAccess, bSetError, papszOptions);
    if (!inner) return nullptr;
    return VSIVirtualHandleUniquePtr(
        new DsMultiRangeHandle(std::move(inner), this));
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int nFlags) override {
    const std::string mem = ToMem(pszFilename);
    return VSIFileManager::GetHandler("/vsimem/")
        ->Stat(mem.c_str(), pStatBuf, nFlags);
  }

  int Unlink(const char* pszFilename) override {
    const std::string mem = ToMem(pszFilename);
    return VSIFileManager::GetHandler("/vsimem/")->Unlink(mem.c_str());
  }

  int HasOptimizedReadMultiRange(const char*) override {
    return opt_multi_range ? TRUE : FALSE;
  }

  bool has_pread = false;
  bool fail_close = false;
  bool opt_multi_range = true;
};

bool DsMultiRangeHandle::HasPRead() const { return handler_->has_pread; }

int DsMultiRangeHandle::Close() {
  const int rc = VSIProxyFileHandle::Close();
  return handler_->fail_close ? -1 : rc;
}

class GTiffDatasetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_COG();
    GDALRegister_JPEG();
    GTiffOneTimeInit();
#ifdef __linux__
    memfd_handler_ = std::make_unique<DsMemFdHandler>();
    VSIFileManager::InstallHandler("/vsimem_ds_fd/", memfd_handler_.get());
#endif  // __linux__
    mr_handler_ = std::make_unique<DsMultiRangeHandler>();
    VSIFileManager::InstallHandler("/vsimem_ds_mr/", mr_handler_.get());
  }

  static void TearDownTestSuite() {
#ifdef __linux__
    VSIFileManager::RemoveHandler("/vsimem_ds_fd/");
    memfd_handler_.reset();
#endif  // __linux__
    VSIFileManager::RemoveHandler("/vsimem_ds_mr/");
    mr_handler_.reset();
  }

  void SetUp() override {
    CPLErrorReset();
    gtiff_driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, gtiff_driver_);
    mr_handler_->has_pread = false;
    mr_handler_->fail_close = false;
    mr_handler_->opt_multi_range = true;
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* gtiff_driver_ = nullptr;
#ifdef __linux__
  static inline std::unique_ptr<DsMemFdHandler> memfd_handler_;
#endif  // __linux__
  static inline std::unique_ptr<DsMultiRangeHandler> mr_handler_;
};

TEST_F(GTiffDatasetTest, CtorHelpersHandlesAndComputeBlocks) {
  // 1. GetTIFFTags, GetGTIFFKeysFlavor, GetGeoTIFFVersion.
  EXPECT_NE(nullptr, GTiffDataset::GetTIFFTags());
  EXPECT_STREQ("TIFFTAG_DOCUMENTNAME",
               GTiffDataset::GetTIFFTags()[0].pszTagName);

  CPLStringList opts;
  EXPECT_EQ(GEOTIFF_KEYS_STANDARD, GetGTIFFKeysFlavor(opts.List()));
  opts.SetNameValue("GEOTIFF_KEYS_FLAVOR", "ESRI_PE");
  EXPECT_EQ(GEOTIFF_KEYS_ESRI_PE, GetGTIFFKeysFlavor(opts.List()));

  EXPECT_EQ(GEOTIFF_VERSION_AUTO, GetGeoTIFFVersion(opts.List()));
  opts.SetNameValue("GEOTIFF_VERSION", "1.0");
  EXPECT_EQ(GEOTIFF_VERSION_1_0, GetGeoTIFFVersion(opts.List()));
  opts.SetNameValue("GEOTIFF_VERSION", "1.1");
  EXPECT_EQ(GEOTIFF_VERSION_1_1, GetGeoTIFFVersion(opts.List()));

  // 2. Ctor config options: GTIFF_VIRTUAL_MEM_IO=IF_ENOUGH_RAM and YES.
  {
    CPLConfigOptionSetter vm_ram("GTIFF_VIRTUAL_MEM_IO", "IF_ENOUGH_RAM",
                                 false);
    GTiffDataset ds1;
    EXPECT_EQ(CE_None, ds1.Close());
  }
  {
    CPLConfigOptionSetter vm_yes("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    GTiffDataset ds2;
    EXPECT_EQ(nullptr, ds2.GetInternalHandle("TIFF_HANDLE"));
    EXPECT_EQ(nullptr, ds2.GetInternalHandle("OTHER"));
    EXPECT_EQ(nullptr, ds2.GetInternalHandle(nullptr));
  }

  // 3. ComputeBlocksPerColRowAndBand overflow branches via OpenOffset on raw
  // in-memory TIFFs!
  auto create_oversized_tiled_tiff = [&](const char* path, int bands,
                                         const char* interleave,
                                         uint32_t target_dim) {
    {
      CPLStringList opts;
      opts.AddNameValue("TILED", "YES");
      opts.AddNameValue("BLOCKXSIZE", "16");
      opts.AddNameValue("BLOCKYSIZE", "16");
      opts.AddNameValue("INTERLEAVE", interleave);
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(path, 16, 16, bands, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->Close());
    }
    static_assert(std::endian::native == std::endian::little);
    vsi_l_offset sz = 0;
    GByte* raw = VSIGetMemFileBuffer(path, &sz, FALSE);
    ASSERT_NE(nullptr, raw);
    uint32_t ifd_off = 0;
    std::memcpy(&ifd_off, raw + 4, 4);
    uint16_t nentries = 0;
    std::memcpy(&nentries, raw + ifd_off, 2);
    for (uint16_t i = 0; i < nentries; ++i) {
      const size_t pos = ifd_off + 2 + i * 12;
      uint16_t tag = 0;
      uint16_t type = 0;
      std::memcpy(&tag, raw + pos, 2);
      std::memcpy(&type, raw + pos + 2, 2);
      if (tag == TIFFTAG_IMAGEWIDTH || tag == TIFFTAG_IMAGELENGTH) {
        if (type == 3) {
          uint16_t new_type = 4;  // TIFF_LONG
          std::memcpy(raw + pos + 2, &new_type, 2);
        }
        std::memcpy(raw + pos + 8, &target_dim, 4);
      }
    }
  };
  {
    // 3a. m_nBlocksPerColumn > INT_MAX / m_nBlocksPerRow:
    // e.g. 1000000 x 1000000 tiled 16x16 -> 62500 x 62500 = 3.9e9 > INT_MAX!
    constexpr char kP1[] = "/vsimem/ds_overflow_rc.tif";
    absl::Cleanup c1 = [&] { VSIUnlink(kP1); };
    ASSERT_NO_FATAL_FAILURE(
        create_oversized_tiled_tiff(kP1, 1, "PIXEL", 1000000));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kP1, GDAL_OF_RASTER | GDAL_OF_READONLY));
      EXPECT_EQ(nullptr, ds);
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many blocks"));
      CPLErrorReset();
    }
  }
  {
    // 3b. PLANARCONFIG_SEPARATE && m_nBlocksPerBand > INT_MAX / l_nBands:
    // e.g. 500000 x 500000 tiled 16x16 (31250 x 31250 = 976,562,500 < INT_MAX)
    // with 3 bands SEPARATE (976,562,500 * 3 > INT_MAX)!
    constexpr char kP2[] = "/vsimem/ds_overflow_bands.tif";
    absl::Cleanup c2 = [&] { VSIUnlink(kP2); };
    ASSERT_NO_FATAL_FAILURE(
        create_oversized_tiled_tiff(kP2, 3, "BAND", 500000));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kP2, GDAL_OF_RASTER | GDAL_OF_READONLY));
      EXPECT_EQ(nullptr, ds);
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many blocks"));
      CPLErrorReset();
    }
  }

  // 4. GTiffDatasetLibGeotiffErrorCallback via malformed GeoKeys in memory.
  {
    constexpr char kGkPath[] = "/vsimem/ds_bad_geokey.tif";
    absl::Cleanup c_gk = [&] { VSIUnlink(kGkPath); };
    {
      VSILFILE* fp = VSIFOpenL(kGkPath, "wb+");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
      TIFF* tif = VSI_TIFFOpen(kGkPath, "w", fp);
      ASSERT_NE(nullptr, tif);
      absl::Cleanup close_tif = [tif] { TIFFClose(tif); };

      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
      GByte zero[16] = {0};
      TIFFWriteEncodedStrip(tif, 0, zero, 16);
      // Write a GeoKeyDirectoryTag (34735) claiming 10 keys when array only has
      // 4 shorts!
      constexpr uint32_t kTiffTagGeoKeyDirectory = 34735;
      const uint16_t bad_geokeys[4] = {1, 1, 0, 10};
      TIFFSetField(tif, kTiffTagGeoKeyDirectory, 4, bad_geokeys);
      TIFFWriteDirectory(tif);
    }

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kGkPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetInternalHandle("TIFF_HANDLE"));
      EXPECT_EQ(nullptr, ds->GetSpatialRef());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("GeoTIFF tags apparently corrupt"));
      CPLErrorReset();
    }
  }
}

TEST_F(GTiffDatasetTest, FinalizeCloseAndCloseDependentDatasets) {
  // 1. COLOR_PROFILE metadata change + ESRI_XML_PAM=YES +
  // m_bFillEmptyTilesAtClosing + NUM_THREADS=2 compression queue +
  // CloseDependentDatasets()!
  {
    constexpr char kPath[] = "/vsimem/ds_finalize_full.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/ds_finalize_full.tif.aux.xml");
    };
    CPLConfigOptionSetter esri_pam("ESRI_XML_PAM", "YES", false);
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);

    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("COMPRESS", "LZW");
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
    // Upstream GTiffDataset::FlushCacheInternal() clears bit-field
    // m_bLoadedBlockDirty before WaitCompletion(), racing with worker threads
    // reading sibling bit-fields in RestoreVolatileParameters()
    // (gtiffdataset.h:256, gtiffdataset_write.cpp:2188).
    co.SetNameValue("NUM_THREADS", "1");
#else
    co.SetNameValue("NUM_THREADS", "2");
#endif
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 32, 32, 1, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);

    // Write only 1 of 4 tiles so m_bFillEmptyTilesAtClosing remains true!
    std::vector<GByte> tile(16 * 16, 42);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()));

    // Set COLOR_PROFILE metadata and xml:ESRI metadata!
    EXPECT_EQ(CE_None, ds->SetMetadataItem("SOURCE_WHITEPOINT", "0.3127,0.3290",
                                           "COLOR_PROFILE"));
    CPLStringList esri_md{"<ESRI>test</ESRI>"};
    EXPECT_EQ(CE_None, ds->SetMetadata(esri_md.List(), "xml:ESRI"));

    // Add internal mask and internal overview so CloseDependentDatasets()
    // returns TRUE (bDroppedRef == true) and overview/mask
    // CloseDependentDatasets returns FALSE (m_poBaseDS != nullptr)!
    EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    int ovr = 2;
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                          nullptr, nullptr));
    EXPECT_EQ(CE_None, ds->Close());
  }

  // Separate check for CloseDependentDatasets() on an already-crystalized
  // dataset with an internal overview and internal mask!
  {
    constexpr char kCloseDepPath[] = "/vsimem/ds_close_dep.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kCloseDepPath); };
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kCloseDepPath, 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(1));
      EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      int ovr = 2;
      EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
    }
    struct AccessCloseDep : public GDALDataset {
      using GDALDataset::CloseDependentDatasets;
    };
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kCloseDepPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_band);
    GDALDataset* ovr_ds = ovr_band->GetDataset();
    ASSERT_NE(nullptr, ovr_ds);
    EXPECT_EQ(FALSE, (ovr_ds->*(&AccessCloseDep::CloseDependentDatasets))());
    EXPECT_EQ(TRUE, (ds.get()->*(&AccessCloseDep::CloseDependentDatasets))());
    // Second Close() exercises m_bIsFinalized == true early return!
    EXPECT_EQ(CE_None, ds->Close());
  }

  // 2. Implicit JPEG overviews cleanup in Finalize() + GA_ReadOnly
  // SetMetadata pushing to PAM on Close().
  {
    constexpr char kPath[] = "/vsimem/ds_finalize_jpg.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/ds_finalize_jpg.tif.aux.xml");
    };
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 256, 256, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(80));
    }
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetOverview(0));
    EXPECT_EQ(CE_None, ds->SetMetadataItem("RO_KEY", "RO_VAL"));
    EXPECT_EQ(CE_None, ds->Close());
  }

  // 3. STREAMABLE_OUTPUT=YES (sets m_pszTmpFilename & m_fpToWrite) +
  // @SUPPRESS_ASAP=YES (calls CancelCreation()).
  {
    constexpr char kStreamPath[] = "/vsimem/ds_stream_out.tif";
    constexpr char kSupPath[] = "/vsimem/ds_suppress.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kStreamPath);
      VSIUnlink(kSupPath);
    };
    {
      CPLStringList co;
      co.SetNameValue("STREAMABLE_OUTPUT", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kStreamPath, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> strip(8 * 4, 11);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, strip.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 1, strip.data()));
      EXPECT_EQ(CE_None, ds->Close());
    }
    {
      CPLStringList co;
      co.SetNameValue("@SUPPRESS_ASAP", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kSupPath, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->Close());
    }
  }

  // 4. COG reopened in GA_Update with IGNORE_COG_LAYOUT_BREAK=YES -> sets
  // m_bWriteKnownIncompatibleEdition = true -> patches header in Finalize()!
  {
    constexpr char kSrcPath[] = "/vsimem/ds_cog_src.tif";
    constexpr char kCogPath[] = "/vsimem/ds_cog_edit.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kSrcPath);
      VSIUnlink(kCogPath);
    };
    {
      GDALDatasetUniquePtr src(
          gtiff_driver_->Create(kSrcPath, 32, 32, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      EXPECT_EQ(CE_None, src->GetRasterBand(1)->Fill(10));
      GDALDriver* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
      ASSERT_NE(nullptr, cog_drv);
      CPLStringList co;
      co.SetNameValue("COMPRESS", "LZW");
      GDALDatasetUniquePtr cog(cog_drv->CreateCopy(
          kCogPath, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, cog);
    }
    const CPLStringList oo{"IGNORE_COG_LAYOUT_BREAK=YES"};
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kCogPath, GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
                          oo.List(), nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->SetMetadataItem("EDIT_COG", "1"));
    std::vector<GByte> random_data(32 * 32);
    for (int i = 0; i < 1024; ++i) random_data[i] = static_cast<GByte>(i * 37);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 32, 32, random_data.data(), 32, 32,
                             GDT_Byte, 0, 0, nullptr));
      CPLErrorReset();
    }
    EXPECT_EQ(CE_None, ds->Close());

    // Reopen RO and verify KNOWN_INCOMPATIBLE_EDITION warning / layout removal.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      std::unique_ptr<GDALDataset> ds_ro(
          GDALDataset::Open(kCogPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds_ro);
      EXPECT_EQ(nullptr, ds_ro->GetMetadataItem("LAYOUT", "IMAGE_STRUCTURE"));
      CPLErrorReset();
    }
  }

  // 5. Close() failure paths: ||maxlength=900 write error AND VSIFCloseL error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kMaxPath[] = "/vsimem/ds_close_err.tif||maxlength=900";
    absl::Cleanup clean = [&] {
      VSIUnlink(kMaxPath);
      VSIUnlink("/vsimem/ds_close_err.tif");
    };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "64");
    co.SetNameValue("BLOCKYSIZE", "64");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kMaxPath, 128, 128, 1, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> tile(64 * 64, 55);
    (void)ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data());
    EXPECT_EQ(CE_Failure, ds->Close());
    CPLErrorReset();
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kFcPath[] = "/vsimem_ds_mr/ds_fail_close.tif";
    const std::string backing = DsMultiRangeHandler::ToMem(kFcPath);
    absl::Cleanup clean = [&] {
      mr_handler_->fail_close = false;
      VSIUnlink(backing.c_str());
    };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kFcPath, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
    }
    std::unique_ptr<GDALDataset> ds_ro(
        GDALDataset::Open(kFcPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_ro);
    mr_handler_->fail_close = true;
    EXPECT_EQ(CE_Failure, ds_ro->Close());
    CPLErrorReset();
  }
}

TEST_F(GTiffDatasetTest, IRasterIOAllBranches) {
  // 1. TryOverviewRasterIO delegation (downsampling on dataset with overviews).
  {
    constexpr char kPath[] = "/vsimem/ds_irio_ovr.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 64, 64, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(10));
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(20));
    EXPECT_EQ(CE_None, ds->GetRasterBand(3)->Fill(30));
    int ovr = 2;
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                          nullptr, nullptr));
    std::vector<GByte> sub(16 * 16 * 3, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, sub.data(), 16, 16,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(10, sub[0]);
  }

#ifdef __linux__
  // 2. VirtualMemIO via /vsimem_ds_fd/ (GTIFF_VIRTUAL_MEM_IO=YES).
  {
    CPLConfigOptionSetter vm_yes("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    constexpr char kPath[] = "/vsimem_ds_fd/ds_irio_vmem.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 2, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(33));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(66));
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> out(16 * 16 * 2, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 16, 16, out.data(), 16, 16,
                                    GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(33, out[0]);
    EXPECT_EQ(66, out[256]);
  }
#endif  // __linux__

  // 3. DirectIO via GTIFF_DIRECT_IO=YES.
  {
    CPLConfigOptionSetter dio_yes("GTIFF_DIRECT_IO", "YES", false);
    constexpr char kPath[] = "/vsimem/ds_irio_dio.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 2, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(44));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(88));
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> out(16 * 16 * 2, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 16, 16, out.data(), 16, 16,
                                    GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(44, out[0]);
  }

  // 4. MultiThreadedRead (NUM_THREADS=2 on /vsimem/, which has PRead=true) and
  // CacheMultiRange (on /vsimem_ds_mr/ with HasPRead=false, both CONTIG &
  // SEPARATE!).
  for (bool separate : {false, true}) {
    const char* p_mt =
        separate ? "/vsimem/ds_mt_sep.tif" : "/vsimem/ds_mt_ctg.tif";
    const char* p_mr = separate ? "/vsimem_ds_mr/ds_mr_sep.tif"
                                : "/vsimem_ds_mr/ds_mr_ctg.tif";
    const std::string backing = DsMultiRangeHandler::ToMem(p_mr);
    absl::Cleanup clean = [&] {
      VSIUnlink(p_mt);
      VSIUnlink(backing.c_str());
    };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("COMPRESS", "LZW");
    co.SetNameValue("INTERLEAVE", separate ? "BAND" : "PIXEL");
    for (const char* path : {p_mt, p_mr}) {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(path, 32, 32, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(12));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(34));
    }

    // MultiThreadedRead on /vsimem/.
    {
      const CPLStringList oo{"NUM_THREADS=2"};
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_mt, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> out(32 * 32 * 2, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                                      GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(12, out[0]);
    }

    // CacheMultiRange on /vsimem_ds_mr/ (HasPRead=false).
    {
      mr_handler_->opt_multi_range = true;
      mr_handler_->has_pread = false;
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_mr, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> out(32 * 32 * 2, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                                      GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(12, out[0]);
    }
  }

  // 5. Whole-block write optimization in IRasterIO:
  // - Single block BIP fast path (no temp buffer)
  // - Multi-block BIP with partial right/bottom edge tiles (20x20 on 16x16)
  // - Multi-block BSQ / reversed band map ("random" spacing loop)
  // - Fallback when HasBlockCache() is true
  // - GTIFF_DONT_WRITE_BLOCKS=YES debug short-circuit!
  {
    constexpr char kWbPath[] = "/vsimem/ds_wb_write.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kWbPath); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kWbPath, 20, 20, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);

    // 5a. Single full block (0,0,16,16) BIP -> direct WriteEncodedTileOrStrip!
    std::vector<GByte> bip16(16 * 16 * 3, 50);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 16, 16, bip16.data(), 16, 16,
                           GDT_Byte, 3, nullptr, 3, 16 * 3, 1, nullptr));

    // 5b. Full image (0,0,20,20) BIP -> multi-block + partial edge tile zeroing
    std::vector<GByte> bip20(20 * 20 * 3, 60);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 20, 20, bip20.data(), 20, 20,
                           GDT_Byte, 3, nullptr, 3, 20 * 3, 1, nullptr));

    // 5c. Full image (0,0,20,20) with reversed bandMap + BSQ spacing!
    int rev_map[3] = {3, 2, 1};
    std::vector<GByte> bsq20(20 * 20 * 3, 70);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 20, 20, bsq20.data(), 20, 20,
                           GDT_Byte, 3, rev_map, 1, 20, 400, nullptr));

    // 5d. Trigger block cache on band 1 so bOptimOK becomes false -> fallback!
    std::vector<GByte> tmp_blk(16 * 16);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, tmp_blk.data()));
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 16, 16, bip16.data(), 16, 16,
                           GDT_Byte, 3, nullptr, 3, 16 * 3, 1, nullptr));
  }
  {
    CPLConfigOptionSetter no_write("GTIFF_DONT_WRITE_BLOCKS", "YES", false);
    constexpr char kNwPath[] = "/vsimem/ds_dont_write.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kNwPath); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kNwPath, 16, 16, 2, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> bip(16 * 16 * 2, 99);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Write, 0, 0, 16, 16, bip.data(), 16, 16,
                                    GDT_Byte, 2, nullptr, 2, 32, 1, nullptr));
  }
}

TEST_F(GTiffDatasetTest, SetReloadDirectoryAndRestoreVolatileParameters) {
  // Exercise RestoreVolatileParameters across codecs in GA_Update by building
  // an internal overview on each dataset so SetDirectory() switches IFDs!
  struct CodecCase {
    std::string_view name;
    std::string_view compress;
    std::string_view extra_k;
    std::string_view extra_v;
  };
  constexpr CodecCase kCases[] = {
      {.name = "jpeg_ycbcr",
       .compress = "JPEG",
       .extra_k = "PHOTOMETRIC",
       .extra_v = "YCBCR"},
      {.name = "deflate",
       .compress = "DEFLATE",
       .extra_k = "ZLEVEL",
       .extra_v = "6"},
      {.name = "lzma",
       .compress = "LZMA",
       .extra_k = "LZMA_PRESET",
       .extra_v = "2"},
      {.name = "zstd",
       .compress = "ZSTD",
       .extra_k = "ZSTD_LEVEL",
       .extra_v = "3"},
      {.name = "lerc_zstd",
       .compress = "LERC_ZSTD",
       .extra_k = "MAX_Z_ERROR",
       .extra_v = "0.5"},
      {.name = "webp_lossless",
       .compress = "WEBP",
       .extra_k = "WEBP_LOSSLESS",
       .extra_v = "YES"},
      {.name = "jxl",
       .compress = "JXL",
       .extra_k = "JXL_EFFORT",
       .extra_v = "3"},
  };

  CPLConfigOptionSetter deflate_zlib("GDAL_TIFF_DEFLATE_SUBCODEC", "ZLIB",
                                     false);
  for (const auto& c : kCases) {
    SCOPED_TRACE(std::string(c.name));
    const std::string path = std::format("/vsimem/ds_vol_{}.tif", c.name);
    absl::Cleanup clean = [&] { VSIUnlink(path.c_str()); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("COMPRESS", std::string(c.compress).c_str());
    if (!c.extra_k.empty()) {
      co.SetNameValue(std::string(c.extra_k).c_str(),
                      std::string(c.extra_v).c_str());
    }
    const int bands =
        (c.name == "jpeg_ycbcr" || c.name == "webp_lossless") ? 3 : 1;
    GDALDatasetUniquePtr ds(gtiff_driver_->Create(path.c_str(), 32, 32, bands,
                                                  GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);

    if (c.compress == "JPEG") {
      GTIFFSetJpegQuality(ds.get(), 80);
      GTIFFSetJpegTablesMode(ds.get(), 1);
    } else if (c.compress == "DEFLATE") {
      GTIFFSetZLevel(ds.get(), 6);
    } else if (c.compress == "ZSTD") {
      GTIFFSetZSTDLevel(ds.get(), 3);
    } else if (c.compress == "LERC_ZSTD") {
      GTIFFSetZSTDLevel(ds.get(), 3);
      GTIFFSetMaxZError(ds.get(), 0.5);
    } else if (c.compress == "WEBP") {
      GTIFFSetWebPLevel(ds.get(), 100);
      GTIFFSetWebPLossless(ds.get(), true);
    } else if (c.compress == "JXL") {
      GTIFFSetJXLLossless(ds.get(), false);
      GTIFFSetJXLEffort(ds.get(), 3);
      GTIFFSetJXLDistance(ds.get(), 1.5f);
      GTIFFSetJXLAlphaDistance(ds.get(), 1.5f);
    }

    for (int b = 1; b <= bands; ++b) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->Fill(100));
    }
    int ovr = 2;
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                          nullptr, nullptr));
    // Write to overview then write to base band -> forces SetDirectory() &
    // RestoreVolatileParameters() in GA_Update!
    GDALRasterBand* ovr_b = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_b);
    EXPECT_EQ(CE_None, ovr_b->Fill(50));
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(120));
  }
}

TEST_F(GTiffDatasetTest, ScanDirectoriesSubIFDsMasksAndSubdatasets) {
  // Build rich multi-IFD TIFFs in memory using VSI_TIFFOpen to cover every
  // branch of ScanDirectories():
  // 1. SubIFDs on IFD 0 (valid overview SubIFD + invalid offset SubIFD) +
  //    OVERVIEW_RESAMPLING propagation + mismatched band count overview IFD +
  //    invalid 16-bit mask IFD + valid 8-bit main mask IFD + valid overview
  //    mask IFD + orphan overview mask IFD!
  {
    constexpr char kPath[] = "/vsimem/ds_scan_complex.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      VSILFILE* fp = VSIFOpenL(kPath, "wb+");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
      TIFF* tif = VSI_TIFFOpen(kPath, "w", fp);
      ASSERT_NE(nullptr, tif);
      absl::Cleanup close_tif = [tif] { TIFFClose(tif); };

      auto write_simple_ifd = [&](uint32_t w, uint32_t h, uint16_t spp,
                                  uint16_t bps, uint32_t subfiletype,
                                  const char* gdal_md = nullptr) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, h);
        if (subfiletype != 0) {
          TIFFSetField(tif, TIFFTAG_SUBFILETYPE, subfiletype);
        }
        if (gdal_md != nullptr) {
          TIFFSetField(tif, TIFFTAG_GDAL_METADATA, gdal_md);
        }
        std::vector<GByte> buf(static_cast<size_t>(w) * h * spp * (bps / 8), 1);
        TIFFWriteEncodedStrip(tif, 0, buf.data(), buf.size());
      };

      // Main IFD 0: 16x16x2 with 2 SubIFDs (one valid 8x8x2 overview, one bogus
      // offset) + OVERVIEW_RESAMPLING in GDAL_METADATA!
      uint64_t subifd_offsets[2] = {0, 0};
      write_simple_ifd(
          16, 16, 2, 8, 0,
          "<GDALMetadata><Item name=\"OVERVIEW_RESAMPLING\" "
          "domain=\"IMAGE_STRUCTURE\">BILINEAR</Item></GDALMetadata>");
      TIFFSetField(tif, TIFFTAG_SUBIFD, 2, subifd_offsets);
      TIFFWriteDirectory(tif);

      // SubIFD 0: valid 8x8x2 overview!
      write_simple_ifd(8, 8, 2, 8, FILETYPE_REDUCEDIMAGE);
      TIFFWriteDirectory(tif);  // libtiff patches subifd_offsets[0]

      // SubIFD 1: write valid IFD first, then we'll also append chained IFDs:
      write_simple_ifd(4, 4, 1, 8, FILETYPE_REDUCEDIMAGE);  // wrong band count!
      TIFFWriteDirectory(tif);

      // Chained IFD 1: invalid main mask (16-bit -> rejected & deleted).
      write_simple_ifd(16, 16, 1, 16, FILETYPE_MASK);
      TIFFWriteDirectory(tif);

      // Chained IFD 2: valid main mask (16x16x1 8-bit).
      write_simple_ifd(16, 16, 1, 8, FILETYPE_MASK);
      TIFFWriteDirectory(tif);

      // Chained IFD 3: invalid overview mask (16-bit -> rejected & deleted).
      write_simple_ifd(8, 8, 1, 16, FILETYPE_REDUCEDIMAGE | FILETYPE_MASK);
      TIFFWriteDirectory(tif);

      // Chained IFD 4: orphan overview mask (2x2 -> no matching overview ->
      // deleted).
      write_simple_ifd(2, 2, 1, 8, FILETYPE_REDUCEDIMAGE | FILETYPE_MASK);
      TIFFWriteDirectory(tif);

      // Chained IFD 5: matching overview mask for 8x8 overview -> attached to
      // main mask's overviews!
      write_simple_ifd(8, 8, 1, 8, FILETYPE_REDUCEDIMAGE | FILETYPE_MASK);
      TIFFWriteDirectory(tif);

      // Chained IFD 6: second multipage image with GTG `grid_name` in
      // TIFFTAG_GDAL_METADATA -> populates SUBDATASETS!
      write_simple_ifd(
          16, 16, 2, 8, FILETYPE_PAGE,
          "<GDALMetadata><Item name=\"other\">x</Item>"
          "<Item name=\"grid_name\">vertical_shift_grid</Item></GDALMetadata>");
      TIFFWriteDirectory(tif);

      // Chained IFD 7: directory with width > INT_MAX (skipped with CPLDebug).
      // Use a single strip (ROWSPERSTRIP=1) so libtiff allocates only 1 strip
      // offset entry while writing the directory.
      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 3000000000U);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
      TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);
      TIFFWriteCheck(tif, 0, "test");
      TIFFWriteDirectory(tif);
    }

    std::unique_ptr<GDALDataset> ds;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ds.reset(GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      CPLErrorReset();
    }
    ASSERT_NE(nullptr, ds);
    char** subds = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, subds);
    // Find the SUBDATASET_*_DESC containing "vertical_shift_grid" (its index
    // equals iDirIndex of the FILETYPE_PAGE IFD, i.e. 7).
    bool found_grid_name = false;
    for (int i = 0; subds[i] != nullptr; ++i) {
      if (std::string_view(subds[i]).find("vertical_shift_grid") !=
          std::string_view::npos) {
        found_grid_name = true;
        break;
      }
    }
    EXPECT_TRUE(found_grid_name);

    EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
    GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr);
    EXPECT_STREQ("BILINEAR", ovr->GetMetadataItem("RESAMPLING"));

    GDALRasterBand* msk = ds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(nullptr, msk);
    EXPECT_EQ(1, msk->GetOverviewCount());
  }
}

TEST_F(GTiffDatasetTest, GetFileListAndGetRawBinaryLayout) {
  // 1. GetFileList:
  // - Overview dataset (m_poBaseDS != nullptr) returns nullptr
  // - Main dataset with .IMD, .RPB, .tfw, ESRI .xml (EPSG<=32767), and .vat.dbf
  {
    constexpr char kBase[] = "/vsimem/ds_filelist.tif";
    constexpr char kImd[] = "/vsimem/ds_filelist.IMD";
    constexpr char kRpb[] = "/vsimem/ds_filelist.RPB";
    constexpr char kTfw[] = "/vsimem/ds_filelist.tfw";
    constexpr char kXml[] = "/vsimem/ds_filelist.xml";
    constexpr char kVat[] = "/vsimem/ds_filelist.tif.vat.dbf";
    absl::Cleanup clean = [&] { VSIUnlink(kBase); };

    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kBase, 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      int ovr = 2;
      EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
    }

    VsiMemTempWrapper imd_file(
        kImd,
        "BEGIN_GROUP = IMAGE_1\n\tsatId = \"QB02\";\nEND_GROUP = "
        "IMAGE_1\nEND;\n");
    VsiMemTempWrapper rpb_file(
        kRpb,
        "satId = \"QB02\";\nBEGIN_GROUP = IMAGE\n"
        "\terrBias = 0.0;\n\terrRand = 0.0;\n\tlineOffset = 0;\n"
        "\tsampOffset = 0;\n\tlatOffset = 0;\n\tlongOffset = 0;\n"
        "\theightOffset = 0;\n\tlineScale = 1;\n\tsampScale = 1;\n"
        "\tlatScale = 1;\n\tlongScale = 1;\n\theightScale = 1;\n"
        "\tlineNumCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "\tlineDenCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "\tsampNumCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "\tsampDenCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "END_GROUP = IMAGE\nEND;\n");
    VsiMemTempWrapper tfw_file(kTfw, "1.0\n0.0\n0.0\n-1.0\n100.0\n200.0\n");
    VsiMemTempWrapper xml_file(
        kXml,
        "<metadata><refSysInfo><RefSystem><refSysID><identCode code=\"4326\"/>"
        "</refSysID></RefSystem></refSysInfo></metadata>");
    VsiMemTempWrapper vat_file(kVat, "dummy");

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kBase, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));

    // Overview GetFileList returns nullptr.
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_band);
    GDALDataset* ovr_ds = ovr_band->GetDataset();
    ASSERT_NE(nullptr, ovr_ds);
    StringListPtr ovr_fl(ovr_ds->GetFileList(), CSLDestroy);
    EXPECT_EQ(nullptr, ovr_fl);

    StringListPtr fl(ds->GetFileList(), CSLDestroy);
    ASSERT_NE(nullptr, fl);
    const std::vector<std::string> files = CslToVector(fl.get());
    EXPECT_THAT(files, Contains(kImd));
    EXPECT_THAT(files, Contains(kTfw));
    EXPECT_THAT(files, Contains(kXml));
    EXPECT_THAT(files, Contains(kVat));
    EXPECT_THAT(files, Contains(kRpb));
  }

  // 2. GetRawBinaryLayout all branches:
  // - Compressed -> false
  // - Odd NBITS (<8 or non-power-of-two) -> false
  // - Complex data type -> false
  // - Single-tile CONTIG & single-tile 3-band SEPARATE -> true; multi-tile ->
  //   false; non-uniform band spacing in single-tile SEPARATE -> false
  // - Stripped 1-band (UNKNOWN), CONTIG (BIP), 3-band SEPARATE (BSQ), inverted
  //   endianness -> true; non-contiguous strips / non-uniform band spacing ->
  //   false!
  GDALDataset::RawBinaryLayout layout;

  // 2a. Compressed, NBITS=12, Complex -> false.
  {
    constexpr char kLzwPath[] = "/vsimem/ds_rbl_lzw.tif";
    constexpr char kNbPath[] = "/vsimem/ds_rbl_nb12.tif";
    constexpr char kCxPath[] = "/vsimem/ds_rbl_cx.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kLzwPath);
      VSIUnlink(kNbPath);
      VSIUnlink(kCxPath);
    };
    CPLStringList co_lzw;
    co_lzw.SetNameValue("COMPRESS", "LZW");
    GDALDatasetUniquePtr ds1(
        gtiff_driver_->Create(kLzwPath, 8, 8, 1, GDT_Byte, co_lzw.List()));
    ASSERT_NE(nullptr, ds1);
    EXPECT_FALSE(ds1->GetRawBinaryLayout(layout));

    CPLStringList co_nb;
    co_nb.SetNameValue("NBITS", "12");
    GDALDatasetUniquePtr ds2(
        gtiff_driver_->Create(kNbPath, 8, 8, 1, GDT_UInt16, co_nb.List()));
    ASSERT_NE(nullptr, ds2);
    EXPECT_FALSE(ds2->GetRawBinaryLayout(layout));

    GDALDatasetUniquePtr ds3(
        gtiff_driver_->Create(kCxPath, 8, 8, 1, GDT_CInt16, nullptr));
    ASSERT_NE(nullptr, ds3);
    EXPECT_FALSE(ds3->GetRawBinaryLayout(layout));
  }

  // 2b. Tiled: multi-tile (false), single-tile CONTIG (true), single-tile
  // 3-band SEPARATE (true), and sparse/non-uniform single-tile SEPARATE (false)
  {
    constexpr char kMtPath[] = "/vsimem/ds_rbl_mt.tif";
    constexpr char kStCtgPath[] = "/vsimem/ds_rbl_st_ctg.tif";
    constexpr char kStSepPath[] = "/vsimem/ds_rbl_st_sep.tif";
    constexpr char kStBadPath[] = "/vsimem/ds_rbl_st_bad.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {kMtPath, kStCtgPath, kStSepPath, kStBadPath}) {
        VSIUnlink(p);
      }
    };

    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");

    GDALDatasetUniquePtr d_mt(
        gtiff_driver_->Create(kMtPath, 32, 32, 1, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, d_mt);
    EXPECT_FALSE(d_mt->GetRawBinaryLayout(layout));

    GDALDatasetUniquePtr d_ctg(
        gtiff_driver_->Create(kStCtgPath, 16, 16, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, d_ctg);
    EXPECT_EQ(CE_None, d_ctg->GetRasterBand(1)->Fill(1));
    EXPECT_TRUE(d_ctg->GetRawBinaryLayout(layout));
    EXPECT_EQ(GDALDataset::RawBinaryLayout::Interleaving::BIP,
              layout.eInterleaving);

    co.SetNameValue("INTERLEAVE", "BAND");
    {
      GDALDatasetUniquePtr d_sep_create(
          gtiff_driver_->Create(kStSepPath, 16, 16, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, d_sep_create);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(CE_None, d_sep_create->GetRasterBand(b)->Fill(b));
      }
    }
    std::unique_ptr<GDALDataset> d_sep(
        GDALDataset::Open(kStSepPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, d_sep);
    EXPECT_TRUE(d_sep->GetRawBinaryLayout(layout));
    EXPECT_EQ(GDALDataset::RawBinaryLayout::Interleaving::BSQ,
              layout.eInterleaving);

    // Non-uniform band spacing in single-tile 3-band SEPARATE: write band 1,
    // band 3, then band 2 with SPARSE_OK=YES!
    co.SetNameValue("SPARSE_OK", "YES");
    {
      GDALDatasetUniquePtr d_bad_create(
          gtiff_driver_->Create(kStBadPath, 16, 16, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, d_bad_create);
      std::vector<GByte> blk(256, 1);
      EXPECT_EQ(CE_None,
                d_bad_create->GetRasterBand(1)->WriteBlock(0, 0, blk.data()));
      d_bad_create->FlushCache(false);
      EXPECT_EQ(CE_None,
                d_bad_create->GetRasterBand(3)->WriteBlock(0, 0, blk.data()));
      d_bad_create->FlushCache(false);
      EXPECT_EQ(CE_None,
                d_bad_create->GetRasterBand(2)->WriteBlock(0, 0, blk.data()));
    }
    std::unique_ptr<GDALDataset> d_bad(
        GDALDataset::Open(kStBadPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, d_bad);
    EXPECT_FALSE(d_bad->GetRawBinaryLayout(layout));
  }

  // 2c. Stripped: 1-band inverted endianness (true), 3-band SEPARATE BSQ
  // multi-strip (true), non-contiguous CONTIG strips (false), and non-uniform
  // SEPARATE strips (false).
  {
    constexpr char kBePath[] = "/vsimem/ds_rbl_be.tif";
    constexpr char kSepPath[] = "/vsimem/ds_rbl_sep_str.tif";
    constexpr char kBadCtgPath[] = "/vsimem/ds_rbl_bad_ctg.tif";
    constexpr char kBadSepPath[] = "/vsimem/ds_rbl_bad_sep.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {kBePath, kSepPath, kBadCtgPath, kBadSepPath}) {
        VSIUnlink(p);
      }
    };

    {
      CPLStringList co;
      co.SetNameValue("ENDIANNESS", "INVERTED");
      co.SetNameValue("BLOCKYSIZE", "4");
      {
        GDALDatasetUniquePtr ds_create(
            gtiff_driver_->Create(kBePath, 8, 8, 1, GDT_UInt16, co.List()));
        ASSERT_NE(nullptr, ds_create);
        EXPECT_EQ(CE_None, ds_create->GetRasterBand(1)->Fill(100));
      }
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kBePath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_TRUE(ds->GetRawBinaryLayout(layout));
      EXPECT_FALSE(layout.bLittleEndianOrder);
    }

    {
      CPLStringList co;
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("BLOCKYSIZE", "4");
      {
        GDALDatasetUniquePtr ds_create(
            gtiff_driver_->Create(kSepPath, 8, 8, 3, GDT_Byte, co.List()));
        ASSERT_NE(nullptr, ds_create);
        for (int b = 1; b <= 3; ++b) {
          EXPECT_EQ(CE_None, ds_create->GetRasterBand(b)->Fill(b));
        }
      }
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kSepPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_TRUE(ds->GetRawBinaryLayout(layout));
      EXPECT_EQ(GDALDataset::RawBinaryLayout::Interleaving::BSQ,
                layout.eInterleaving);
    }

    // Out-of-order strips in CONTIG -> false!
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      {
        GDALDatasetUniquePtr ds_create(
            gtiff_driver_->Create(kBadCtgPath, 8, 8, 1, GDT_Byte, co.List()));
        ASSERT_NE(nullptr, ds_create);
        std::vector<GByte> s(32, 1);
        EXPECT_EQ(CE_None,
                  ds_create->GetRasterBand(1)->WriteBlock(0, 1, s.data()));
        ds_create->FlushCache(false);
        EXPECT_EQ(CE_None,
                  ds_create->GetRasterBand(1)->WriteBlock(0, 0, s.data()));
      }
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kBadCtgPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_FALSE(ds->GetRawBinaryLayout(layout));
    }

    // Out-of-order bands/strips in SEPARATE -> false!
    {
      CPLStringList co;
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      {
        GDALDatasetUniquePtr ds_create(
            gtiff_driver_->Create(kBadSepPath, 8, 8, 3, GDT_Byte, co.List()));
        ASSERT_NE(nullptr, ds_create);
        std::vector<GByte> s(32, 1);
        // Write band 1 strips contiguous, band 3 strips contiguous, then band
        // 2!
        for (int b : {1, 3, 2}) {
          EXPECT_EQ(CE_None,
                    ds_create->GetRasterBand(b)->WriteBlock(0, 0, s.data()));
          ds_create->FlushCache(false);
          EXPECT_EQ(CE_None,
                    ds_create->GetRasterBand(b)->WriteBlock(0, 1, s.data()));
          ds_create->FlushCache(false);
        }
      }
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kBadSepPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_FALSE(ds->GetRawBinaryLayout(layout));
    }
  }
}

// TODO: b/335317901 - Cover remaining directory-scanning edge cases and low-
// probability I/O failure branches in gtiffdataset.cpp.

}  // namespace
}  // namespace autotest2
