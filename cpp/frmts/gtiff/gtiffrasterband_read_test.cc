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
// Tests GeoTIFF raster band read paths, RAT/histogram loading, DirectIO,
// VirtualMemAuto, coverage status, and block cache (gtiffrasterband_read.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffrasterband_read.cpp

#include "third_party/gdal/frmts/gtiff/gtiffrasterband.h"

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif  // __linux__

#include <algorithm>
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
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

#ifdef __linux__
// ---------------------------------------------------------------------------
// 100% RAM memfd-backed VSI filesystem ("/vsimem_fd/") so
// VSIFGetNativeFileDescriptorL and CPLVirtualMemFileMapNew succeed in RAM.
// ---------------------------------------------------------------------------
class MemFdHandler final : public VSIFilesystemHandler {
 public:
  ~MemFdHandler() override {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT
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
      std::lock_guard<std::mutex> lock(mu_);  // NOLINT
      auto it = fds_.find(pszFilename);
      const bool create = access.find('w') != std::string_view::npos ||
                          access.find('a') != std::string_view::npos;
      if (it == fds_.end()) {
        if (!create) return nullptr;
        fd = memfd_create("gtiff_rb_read_vmem", 0);
        if (fd < 0) return nullptr;
        fds_[pszFilename] = fd;
      } else {
        fd = it->second;
        if (access.find('w') != std::string_view::npos &&
            access.find('+') == std::string_view::npos) {
          (void)ftruncate(fd, 0);
        }
      }
    }
    const char* delegated_access =
        (access == "wb+" || access == "w+b") ? "r+b" : pszAccess;
    if (access == "wb+" || access == "w+b") {
      (void)ftruncate(fd, 0);
    }
    return VSIFileManager::GetHandler("")->Open(
        ProcPath(fd).c_str(), delegated_access, bSetError, papszOptions);
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int nFlags) override {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);  // NOLINT
      auto it = fds_.find(pszFilename);
      if (it == fds_.end()) return -1;
      fd = it->second;
    }
    return VSIFileManager::GetHandler("")->Stat(ProcPath(fd).c_str(), pStatBuf,
                                                nFlags);
  }

  int Unlink(const char* pszFilename) override {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT
    auto it = fds_.find(pszFilename);
    if (it == fds_.end()) return -1;
    close(it->second);
    fds_.erase(it);
    return 0;
  }

 private:
  std::mutex mu_;  // NOLINT
  std::map<std::string, int> fds_;
};
#endif  // __linux__

// ---------------------------------------------------------------------------
// 100% RAM wrapper around /vsimem/ ("/vsimem_mr/") with configurable
// HasOptimizedReadMultiRange, HasPRead, GetRangeStatus, and SiblingFiles.
// ---------------------------------------------------------------------------
class MultiRangeVsiMemHandler;

class MultiRangeHandle final : public VSIProxyFileHandle {
 public:
  MultiRangeHandle(VSIVirtualHandleUniquePtr&& inner,
                   const MultiRangeVsiMemHandler* handler)
      : VSIProxyFileHandle(std::move(inner)), handler_(handler) {}

  VSIRangeStatus GetRangeStatus(vsi_l_offset, vsi_l_offset) override;
  bool HasPRead() const override;
  int ReadMultiRange(int nRanges, void** ppData, const vsi_l_offset* panOffsets,
                     const size_t* panSizes) override;

 private:
  const MultiRangeVsiMemHandler* handler_;
};

class MultiRangeVsiMemHandler final : public VSIFilesystemHandler {
 public:
  static constexpr std::string_view kPrefix = "/vsimem_mr/";

  static std::string ToMem(std::string_view path) {
    if (path.starts_with(kPrefix)) {
      return std::format("/vsimem/mr_backing_{}", path.substr(kPrefix.size()));
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
        new MultiRangeHandle(std::move(inner), this));
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

  char** SiblingFiles(const char*) override {
    return return_siblings ? CSLDuplicate(siblings.List()) : nullptr;
  }

  VSIRangeStatus range_status = VSI_RANGE_STATUS_DATA;
  bool has_pread = false;
  bool fail_multi_range = false;
  bool opt_multi_range = true;
  bool return_siblings = false;
  CPLStringList siblings;
};

VSIRangeStatus MultiRangeHandle::GetRangeStatus(vsi_l_offset, vsi_l_offset) {
  return handler_->range_status;
}

bool MultiRangeHandle::HasPRead() const { return handler_->has_pread; }

int MultiRangeHandle::ReadMultiRange(int nRanges, void** ppData,
                                     const vsi_l_offset* panOffsets,
                                     const size_t* panSizes) {
  if (handler_->fail_multi_range) return -1;
  return VSIProxyFileHandle::ReadMultiRange(nRanges, ppData, panOffsets,
                                            panSizes);
}

class GTiffRasterBandReadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_COG();
    GDALRegister_JPEG();
    RegisterOGRShape();
    GTiffOneTimeInit();
#ifdef __linux__
    memfd_handler_ = std::make_unique<MemFdHandler>();
    VSIFileManager::InstallHandler("/vsimem_fd/", memfd_handler_.get());
#endif  // __linux__
    mr_handler_ = std::make_unique<MultiRangeVsiMemHandler>();
    VSIFileManager::InstallHandler("/vsimem_mr/", mr_handler_.get());
  }

  static void TearDownTestSuite() {
#ifdef __linux__
    VSIFileManager::RemoveHandler("/vsimem_fd/");
    memfd_handler_.reset();
#endif  // __linux__
    VSIFileManager::RemoveHandler("/vsimem_mr/");
    mr_handler_.reset();
  }

  void SetUp() override {
    CPLErrorReset();
    gtiff_driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, gtiff_driver_);
    mr_handler_->range_status = VSI_RANGE_STATUS_DATA;
    mr_handler_->has_pread = false;
    mr_handler_->fail_multi_range = false;
    mr_handler_->opt_multi_range = true;
    mr_handler_->return_siblings = false;
    mr_handler_->siblings.Clear();
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* gtiff_driver_ = nullptr;
#ifdef __linux__
  static inline std::unique_ptr<MemFdHandler> memfd_handler_;
#endif  // __linux__
  static inline std::unique_ptr<MultiRangeVsiMemHandler> mr_handler_;
};

// Helper to create an in-memory .vat.dbf file with OGR ESRI Shapefile driver.
void CreateInMemVatDbf(const std::string& dbf_path) {
  GDALDriver* shp_drv =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_drv);
  GDALDatasetUniquePtr ds(
      shp_drv->Create(dbf_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->CreateLayer("vat", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, lyr);
  OGRFieldDefn f_val("VALUE", OFTInteger);
  OGRFieldDefn f_cnt("COUNT", OFTInteger);
  OGRFieldDefn f_cls("CLASS_NAME", OFTString);
  f_cls.SetWidth(32);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_val));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_cnt));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_cls));

  OGRFeature feat(lyr->GetLayerDefn());
  feat.SetField("VALUE", 1);
  feat.SetField("COUNT", 10);
  feat.SetField("CLASS_NAME", "Water");
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
  ds.reset();

  // Remove any .shp/.shx created alongside .dbf, keeping .dbf intact.
  const std::string base = dbf_path.substr(0, dbf_path.size() - 4);
  VSIUnlink((base + ".shp").c_str());
  VSIUnlink((base + ".shx").c_str());
}

TEST_F(GTiffRasterBandReadTest, GetDefaultRATAndHistograms) {
  // 1. Overview band (m_poBaseDS != nullptr) returns m_poRAT directly (null).
  {
    constexpr char kPath[] = "/vsimem/rb_rat_ovr.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(5));
    int ovr = 2;
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                          nullptr, nullptr));
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_band);
    EXPECT_EQ(nullptr, ovr_band->GetDefaultRAT());
  }

  // 2. RAT from PAM takes priority, and second call hits m_bRATSet cache.
  {
    constexpr char kPath[] = "/vsimem/rb_rat_pam.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/rb_rat_pam.tif.aux.xml");
    };
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    CPLConfigOptionSetter rat_pam("GTIFF_WRITE_RAT_TO_PAM", "YES", false);
    {
      CPLStringList co;
      co.SetNameValue("PROFILE", "BASELINE");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      GDALDefaultRasterAttributeTable rat;
      rat.CreateColumn("VALUE", GFT_Integer, GFU_MinMax);
      rat.SetValue(0, 0, 42);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetDefaultRAT(&rat));
      GUIntBig hist[4] = {0};
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->GetHistogram(-0.5, 255.5, 4, hist, TRUE,
                                                   FALSE, nullptr, nullptr));
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetDefaultHistogram(0.0, 255.0, 4, hist));
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterAttributeTable* rat1 = ds->GetRasterBand(1)->GetDefaultRAT();
    ASSERT_NE(nullptr, rat1);
    EXPECT_EQ(42, rat1->GetValueAsInt(0, 0));
    // Call again after clearing PAM flag path: hits `if (m_bRATSet)`.
    EXPECT_EQ(rat1, ds->GetRasterBand(1)->GetDefaultRAT());

    // GetHistogram & GetDefaultHistogram.
    GUIntBig hist_out[4] = {0};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->GetHistogram(-0.5, 255.5, 4, hist_out, TRUE,
                                                 FALSE, nullptr, nullptr));
    double dfMin = 0.0;
    double dfMax = 0.0;
    int nBuckets = 0;
    GUIntBig* panHist = nullptr;
    const CPLErr hist_err = ds->GetRasterBand(1)->GetDefaultHistogram(
        &dfMin, &dfMax, &nBuckets, &panHist, FALSE, nullptr, nullptr);
    std::unique_ptr<GUIntBig, decltype(&VSIFree)> hist_ptr(panHist, VSIFree);
    EXPECT_EQ(CE_None, hist_err);
  }

  // 3. /vsisubfile/ rejects sidecar file (!GDALCanFileAcceptSidecarFile).
  {
    constexpr char kPath[] = "/vsimem/rb_rat_sub.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
    }
    VSIStatBufL st{};
    ASSERT_EQ(0, VSIStatL(kPath, &st));
    const std::string sub =
        std::format("/vsisubfile/0_{},{}", st.st_size, kPath);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
    // Second call hits m_bRATSet == true.
    EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
  }

  // 4. Sidecar .vat.dbf via SiblingFiles list (both present & absent) and
  // via VSIStatL fallback (GDAL_DISABLE_READDIR_ON_OPEN=YES).
  {
    constexpr char kMrPath[] = "/vsimem_mr/vat_sib.tif";
    const std::string backing_tif = MultiRangeVsiMemHandler::ToMem(kMrPath);
    const std::string backing_dbf = backing_tif + ".vat.dbf";
    absl::Cleanup clean = [&] {
      VSIUnlink(backing_tif.c_str());
      VSIUnlink(backing_dbf.c_str());
    };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kMrPath, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
    }
    CreateInMemVatDbf(backing_dbf);
    ASSERT_FALSE(HasFailure());

    // 4a. SiblingFiles returns matching "vat_sib.tif.vat.dbf".
    mr_handler_->return_siblings = true;
    mr_handler_->siblings.AddString("vat_sib.tif.vat.dbf");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMrPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GDALRasterAttributeTable* rat = ds->GetRasterBand(1)->GetDefaultRAT();
      ASSERT_NE(nullptr, rat);
      EXPECT_EQ(1, rat->GetRowCount());
    }

    // 4b. SiblingFiles returns list WITHOUT .vat.dbf -> returns nullptr.
    mr_handler_->siblings.Clear();
    mr_handler_->siblings.AddString("other_file.txt");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMrPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
    }

    // 4c. VSIStatL fallback when GDAL_DISABLE_READDIR_ON_OPEN=YES.
    mr_handler_->return_siblings = false;
    {
      CPLConfigOptionSetter no_readdir("GDAL_DISABLE_READDIR_ON_OPEN", "YES",
                                       false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMrPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GDALRasterAttributeTable* rat = ds->GetRasterBand(1)->GetDefaultRAT();
      ASSERT_NE(nullptr, rat);
      EXPECT_EQ(1, rat->GetRowCount());
    }
  }
}

TEST_F(GTiffRasterBandReadTest, DirectIOAllBranches) {
  CPLConfigOptionSetter direct_io("GTIFF_DIRECT_IO", "YES", false);

  // 1. Tiled uncompressed CONTIG in GA_Update -> exercises FlushCache +
  // CommonDirectIOClassic via single-band RasterIO!
  {
    constexpr char kPath[] = "/vsimem/rb_dio_tiled.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(11));
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(22));
    EXPECT_EQ(CE_None, ds->GetRasterBand(3)->Fill(33));

    // Single-band RasterIO calls GTiffRasterBand::IRasterIO -> DirectIO!
    std::vector<GByte> buf(16 * 16, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(22, buf[0]);

    // Non-nearest resampling falls back (DirectIO returns -1 -> block cache).
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<GByte> sub(8 * 8, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16, sub.data(),
                                             8, 8, GDT_Byte, 0, 0, &extra));
    EXPECT_EQ(11, sub[0]);
  }

  // 2. Compressed TIFF -> DirectIO returns -1 (unsupported) and falls back.
  {
    constexpr char kPath[] = "/vsimem/rb_dio_lzw.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    CPLStringList co;
    co.SetNameValue("COMPRESS", "LZW");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(77));
    ds->FlushCache(false);
    GByte v = 0;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &v, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(77, v);
  }

  // 3. Stripped uncompressed single-band Byte:
  // - exact read without pTmpBuffer (ppData points directly into pData)
  // - Y subsampling + X subsampling (bOneByteCopy == true)
  // - Y oversampling + X oversampling + type conversion (bOneByteCopy == false)
  {
    constexpr char kPath[] = "/vsimem/rb_dio_strip1.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> src(16 * 16);
      for (int i = 0; i < 256; ++i) src[i] = static_cast<GByte>(i);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 16, 16, src.data(), 16, 16,
                             GDT_Byte, 0, 0, nullptr));
    }

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    // Exact 1-band Byte read -> pTmpBuffer == nullptr!
    std::vector<GByte> exact(8 * 8, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 2, 2, 8, 8, exact.data(),
                                             8, 8, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(2 * 16 + 2, exact[0]);

    // Subsampling in X & Y with GDT_Byte (bOneByteCopy == true).
    std::vector<GByte> sub(4 * 4, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16, sub.data(),
                                             4, 4, GDT_Byte, 0, 0, nullptr));
    // Nearest neighbour picks source pixel floor((i + 0.5) * 4), i.e. 2 for
    // i == 0.
    EXPECT_EQ(2 * 16 + 2, sub[0]);

    // Oversampling in X & Y with GDT_UInt16 (bOneByteCopy == false).
    std::vector<uint16_t> over(8 * 8, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, over.data(),
                                             8, 8, GDT_UInt16, 0, 0, nullptr));
    // Output pixels 0 and 1 both map to source pixel 0, pixel 2 maps to 1.
    EXPECT_EQ(0, over[0]);
    EXPECT_EQ(0, over[1]);
    EXPECT_EQ(1, over[2]);

    // Same XSize, type conversion (nBufXSize == nXSize in pTmpBuffer loop).
    std::vector<uint16_t> conv(16 * 4, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 4, conv.data(),
                                             16, 4, GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(5, conv[5]);
  }

  // 4. Stripped multi-band CONTIG + big-endian (ENDIANNESS=INVERTED):
  // both non-complex (UInt16) and complex (CInt16) byte-swapping!
  {
    constexpr char kPathU16[] = "/vsimem/rb_dio_be_u16.tif";
    constexpr char kPathC16[] = "/vsimem/rb_dio_be_c16.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kPathU16);
      VSIUnlink(kPathC16);
    };
    CPLStringList co;
    co.SetNameValue("ENDIANNESS", "INVERTED");
    co.SetNameValue("BLOCKYSIZE", "4");
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPathU16, 8, 8, 2, GDT_UInt16, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(1000));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(2000));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPathU16, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<uint16_t> out(8 * 8, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                             GF_Read, 0, 0, 8, 8, out.data(), 8, 8, GDT_UInt16,
                             0, 0, nullptr));
      EXPECT_EQ(2000, out[0]);
    }
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPathC16, 8, 8, 1, GDT_CInt16, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<int16_t> cdata(8 * 8 * 2, 0);
      for (int i = 0; i < 64; ++i) {
        cdata[2 * i] = 300;
        cdata[2 * i + 1] = -400;
      }
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 8, 8, cdata.data(), 8, 8,
                             GDT_CInt16, 0, 0, nullptr));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPathC16, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<int16_t> cout(8 * 8 * 2, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 8, 8, cout.data(), 8, 8, GDT_CInt16,
                             0, 0, nullptr));
      EXPECT_EQ(300, cout[0]);
      EXPECT_EQ(-400, cout[1]);
    }
  }

  // 5. Sparse stripped file (panOffsets[iLine] == 0 -> DirectIO returns -1) and
  // ReadMultiRange failure (returns CE_Failure).
  {
    constexpr char kSparsePath[] = "/vsimem/rb_dio_sparse.tif";
    constexpr char kFailMrPath[] = "/vsimem_mr/rb_dio_failmr.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(kFailMrPath);
    absl::Cleanup clean = [&] {
      mr_handler_->fail_multi_range = false;
      VSIUnlink(kSparsePath);
      VSIUnlink(backing.c_str());
    };
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kSparsePath, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kSparsePath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GByte v = 99;
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &v, 1, 1,
                                               GDT_Byte, 0, 0, nullptr));
      EXPECT_EQ(0, v);
    }

    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kFailMrPath, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(55));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kFailMrPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      mr_handler_->fail_multi_range = true;
      GByte v = 0;
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure,
                  ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, &v, 1, 1,
                                                 GDT_Byte, 0, 0, nullptr));
        CPLErrorReset();
      }
    }
  }
}

#ifdef __linux__
TEST_F(GTiffRasterBandReadTest, GetVirtualMemAutoAllBranches) {
  // 1. CONTIG stripped file on /vsimem_fd/:
  // - Created with SPARSE_OK=YES (all zero offsets) and opened in GA_Update ->
  //   exercises zero-strip initialization + ftruncate + base mapping + derived
  //   mapping for band 1 & band 2 + DropReferenceVirtualMem (both before and
  //   after dataset destruction!).
  {
    constexpr char kPath[] = "/vsimem_fd/vmem_contig_sparse.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
    }

    // First open RO on sparse file -> "Sparse files not supported in file
    // mapping" + USE_DEFAULT_IMPLEMENTATION=NO returns nullptr.
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int px = 0;
      GIntBig ln = 0;
      CPLStringList opts;
      opts.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "NO");
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetVirtualMemAuto(
                             GF_Read, &px, &ln, opts.List()));
    }

    // Now open GA_Update -> initializes all strips via ftruncate and maps RW!
    CPLVirtualMem* vmem_outlive = nullptr;
    absl::Cleanup free_outlive = [&] {
      if (vmem_outlive != nullptr) CPLVirtualMemFree(vmem_outlive);
    };
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, ds);
      int px1 = 0;
      int px2 = 0;
      GIntBig ln1 = 0;
      GIntBig ln2 = 0;
      CPLVirtualMem* vm1 = ds->GetRasterBand(1)->GetVirtualMemAuto(
          GF_Write, &px1, &ln1, nullptr);
      ASSERT_NE(nullptr, vm1);
      absl::Cleanup free_vm1 = [&] {
        if (vm1 != nullptr) CPLVirtualMemFree(vm1);
      };
      EXPECT_EQ(2, px1);
      EXPECT_EQ(32, ln1);

      // Band 2 reuses m_pBaseMapping!
      vmem_outlive = ds->GetRasterBand(2)->GetVirtualMemAuto(GF_Write, &px2,
                                                             &ln2, nullptr);
      ASSERT_NE(nullptr, vmem_outlive);
      EXPECT_EQ(2, px2);

      auto* ptr1 = static_cast<GByte*>(CPLVirtualMemGetAddr(vm1));
      auto* ptr2 = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_outlive));
      ptr1[0] = 123;
      ptr2[0] = 234;

      // Free vm1 while dataset is STILL ALIVE -> DropReferenceVirtualMem
      // with *ppoSelf != nullptr!
      CPLVirtualMemFree(vm1);
      vm1 = nullptr;
    }
    // Free vmem_outlive AFTER dataset was destroyed -> DropReferenceVirtualMem
    // with *ppoSelf == nullptr!
    CPLVirtualMemFree(vmem_outlive);
    vmem_outlive = nullptr;
  }

  // 2. SEPARATE stripped file on /vsimem_fd/ with multiple strips per band,
  // plus USE_DEFAULT_IMPLEMENTATION=YES and fallback on /vsimem/ (no fd).
  {
    constexpr char kPath[] = "/vsimem_fd/vmem_separate.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      CPLStringList co;
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(42));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(84));
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    int px = 0;
    GIntBig ln = 0;
    CPLVirtualMem* vm =
        ds->GetRasterBand(2)->GetVirtualMemAuto(GF_Read, &px, &ln, nullptr);
    ASSERT_NE(nullptr, vm);
    absl::Cleanup free_vm = [vm] { CPLVirtualMemFree(vm); };
    EXPECT_EQ(1, px);
    EXPECT_EQ(16, ln);
    EXPECT_EQ(84, *static_cast<GByte*>(CPLVirtualMemGetAddr(vm)));

    // USE_DEFAULT_IMPLEMENTATION=YES.
    CPLStringList opts_yes;
    opts_yes.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "YES");
    CPLVirtualMem* vm_def = ds->GetRasterBand(1)->GetVirtualMemAuto(
        GF_Read, &px, &ln, opts_yes.List());
    ASSERT_NE(nullptr, vm_def);
    CPLVirtualMemFree(vm_def);
  }

  // 3. Incompatible spacing on /vsimem_fd/: strip 0 written, strip 1 missing
  // (panTIFFOffsets[1] == 0), and reversed strip offsets (strip 0 written after
  // strip 1) -> bCompatibleSpacing == false -> falls back to base impl!
  {
    constexpr char kPathHole[] = "/vsimem_fd/vmem_hole.tif";
    constexpr char kPathRev[] = "/vsimem_fd/vmem_rev.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kPathHole);
      VSIUnlink(kPathRev);
    };
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPathHole, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> blk(16 * 4, 7);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, blk.data()));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPathHole, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int px = 0;
      GIntBig ln = 0;
      CPLStringList opts_no;
      opts_no.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "NO");
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetVirtualMemAuto(
                             GF_Read, &px, &ln, opts_no.List()));
      // AUTO falls back to GDALRasterBand::GetVirtualMemAuto.
      CPLVirtualMem* vm_fb =
          ds->GetRasterBand(1)->GetVirtualMemAuto(GF_Read, &px, &ln, nullptr);
      ASSERT_NE(nullptr, vm_fb);
      CPLVirtualMemFree(vm_fb);
    }

    // Out-of-order strip offsets: write strip index 2 first, then 0, 1, 3.
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPathRev, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> blk(16 * 4, 9);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 2, blk.data()));
      ds->FlushCache(false);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, blk.data()));
      ds->FlushCache(false);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 1, blk.data()));
      ds->FlushCache(false);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 3, blk.data()));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPathRev, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int px = 0;
      GIntBig ln = 0;
      CPLStringList opts_no;
      opts_no.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "OFF");
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetVirtualMemAuto(
                             GF_Read, &px, &ln, opts_no.List()));
    }
  }
}
#endif  // __linux__

TEST_F(GTiffRasterBandReadTest, IGetDataCoverageStatusAllBranches) {
  // 1. SEPARATE tiled sparse TIFF on /vsimem_mr/:
  // - GA_Update FlushCache branch
  // - Empty tile + non-empty tile
  // - GetRangeStatus returning HOLE vs DATA
  // - nMaskFlagStop early exit setting *pdfDataPct = -1.0
  // - Int64 / UInt64 / Double NoData == 0 vs != 0 branches
  {
    constexpr char kMrPath[] = "/vsimem_mr/cov_status.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(kMrPath);
    absl::Cleanup clean = [&] { VSIUnlink(backing.c_str()); };

    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("INTERLEAVE", "BAND");
    co.SetNameValue("SPARSE_OK", "YES");
    std::unique_ptr<GDALDataset> ds(
        gtiff_driver_->Create(kMrPath, 32, 32, 2, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> tile(16 * 16, 42);
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()));

    // Query in GA_Update mode -> flushes cache; 1 of 4 tiles has data (25%).
    double pct = 0.0;
    int st = ds->GetRasterBand(2)->GetDataCoverageStatus(0, 0, 32, 32, 0, &pct);
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_DATA | GDAL_DATA_COVERAGE_STATUS_EMPTY,
              st);
    EXPECT_DOUBLE_EQ(25.0, pct);

    // Early stop via nMaskFlagStop -> pct = -1.0.
    st = ds->GetRasterBand(2)->GetDataCoverageStatus(
        0, 0, 32, 32, GDAL_DATA_COVERAGE_STATUS_DATA, &pct);
    EXPECT_NE(0, st & GDAL_DATA_COVERAGE_STATUS_DATA);
    EXPECT_DOUBLE_EQ(-1.0, pct);
    ds.reset();

    // Reopen RO with range_status = HOLE -> allocated uncompressed block
    // without nonzero NoData is reported as EMPTY!
    mr_handler_->range_status = VSI_RANGE_STATUS_HOLE;
    ds.reset(GDALDataset::Open(kMrPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    st = ds->GetRasterBand(2)->GetDataCoverageStatus(0, 0, 16, 16, 0, &pct);
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_EMPTY, st);
    EXPECT_DOUBLE_EQ(0.0, pct);

    // With range_status = DATA -> reported as DATA (100%).
    mr_handler_->range_status = VSI_RANGE_STATUS_DATA;
    st = ds->GetRasterBand(2)->GetDataCoverageStatus(0, 0, 16, 16, 0, &pct);
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_DATA, st);
    EXPECT_DOUBLE_EQ(100.0, pct);
  }

  // 2. NoData = 0 in Int64 and UInt64 RO datasets.
  for (GDALDataType dt : {GDT_Int64, GDT_UInt64}) {
    const char* mr_path =
        dt == GDT_Int64 ? "/vsimem_mr/cov_i64.tif" : "/vsimem_mr/cov_u64.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(mr_path);
    absl::Cleanup clean = [&] { VSIUnlink(backing.c_str()); };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(mr_path, 8, 8, 1, dt, nullptr));
      ASSERT_NE(nullptr, ds);
      if (dt == GDT_Int64) {
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsInt64(0));
      } else {
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsUInt64(0));
      }
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(1));
    }
    mr_handler_->range_status = VSI_RANGE_STATUS_DATA;
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    double pct = 0.0;
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_DATA,
              ds->GetRasterBand(1)->GetDataCoverageStatus(0, 0, 8, 8, 0, &pct));
    EXPECT_DOUBLE_EQ(100.0, pct);
  }
}

TEST_F(GTiffRasterBandReadTest, IReadBlockFillCacheAndInterleavedMask) {
  // 1. Partial bottom strip & partial bottom tile, SPARSE_OK missing block,
  // 3-band RO GDALDeinterleave fast path, 2-band CONTIG GDALCopyWords64 +
  // FillCacheForOtherBands, and corrupt strile error paths (both SEPARATE and
  // CONTIG).
  {
    // 1a. 3-band RO CONTIG with partial bottom strip (height 10, rowsperstrip
    // 4)
    constexpr char kPath3[] = "/vsimem/rb_read_3band.tif";
    absl::Cleanup clean3 = [&] { VSIUnlink(kPath3); };
    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath3, 8, 10, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(10));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(20));
      EXPECT_EQ(CE_None, ds->GetRasterBand(3)->Fill(30));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPath3, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> blk(8 * 4, 0);
      // Reading band 1 first triggers 3-band GDALDeinterleave fast path!
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 2, blk.data()));
      EXPECT_EQ(10, blk[0]);
      // Reading band 2 first on block 0 triggers GDALCopyWords64 +
      // FillCacheForOtherBands!
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->ReadBlock(0, 0, blk.data()));
      EXPECT_EQ(20, blk[0]);
    }
  }

  // 1b. Sparse missing block + corrupt LZW block in SEPARATE and CONTIG.
  for (bool separate : {true, false}) {
    const char* path =
        separate ? "/vsimem/rb_corrupt_sep.tif" : "/vsimem/rb_corrupt_ctg.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("INTERLEAVE", separate ? "BAND" : "PIXEL");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(path, 32, 20, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> tile(16 * 16, 77);
      // Write tile (0,0), leave tile (1,0) sparse!
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()));
    }
    // Corrupt the LZW payload of tile (0,0) in-memory.
    vsi_l_offset off0 = 0;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      const char* s_off =
          ds->GetRasterBand(1)->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF");
      ASSERT_NE(nullptr, s_off);
      off0 = static_cast<vsi_l_offset>(std::strtoull(s_off, nullptr, 10));
    }
    vsi_l_offset len = 0;
    GByte* raw = VSIGetMemFileBuffer(path, &len, FALSE);
    ASSERT_NE(nullptr, raw);
    ASSERT_LT(off0 + 4, len);
    std::fill_n(raw + off0, 4, static_cast<GByte>(0xFF));

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> buf(16 * 16, 5);
    // Sparse tile (1,0) -> NullBlock -> 0, CE_None!
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(1, 0, buf.data()));
    EXPECT_EQ(0, buf[0]);
    // Corrupt tile (0,0) -> CE_Failure!
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      CPLErrorReset();
    }
  }

  // 2. COG with internal mask on /vsimem_mr/ -> IRasterIO invokes
  // CacheMultiRange -> IReadBlock invokes CacheMaskForBlock and preloads mask!
  {
    constexpr char kSrcPath[] = "/vsimem/rb_cog_mask_src.tif";
    constexpr char kCogPath[] = "/vsimem_mr/rb_cog_mask.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(kCogPath);
    absl::Cleanup clean = [&] {
      VSIUnlink(kSrcPath);
      VSIUnlink(backing.c_str());
    };
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      GDALDatasetUniquePtr src(
          gtiff_driver_->Create(kSrcPath, 64, 64, 3, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      EXPECT_EQ(CE_None, src->GetRasterBand(1)->Fill(100));
      EXPECT_EQ(CE_None, src->GetRasterBand(2)->Fill(150));
      EXPECT_EQ(CE_None, src->GetRasterBand(3)->Fill(200));
      EXPECT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));
      EXPECT_EQ(CE_None, src->GetRasterBand(1)->GetMaskBand()->Fill(255));

      GDALDriver* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
      ASSERT_NE(nullptr, cog_drv);
      CPLStringList cog_co;
      cog_co.SetNameValue("BLOCKSIZE", "32");
      cog_co.SetNameValue("COMPRESS", "LZW");
      GDALDatasetUniquePtr cog(cog_drv->CreateCopy(
          kCogPath, src.get(), FALSE, cog_co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, cog);
    }

    mr_handler_->opt_multi_range = true;
    mr_handler_->has_pread = false;
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kCogPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> out(64 * 64 * 3, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(100, out[0]);
  }

  // 3. Streaming backward read rejection (m_bStreamingIn == true).
  {
    constexpr char kStreamTif[] = "/vsimem/rb_stream.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kStreamTif); };
    {
      CPLStringList co;
      co.SetNameValue("STREAMABLE_OUTPUT", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      GDALDatasetUniquePtr src(
          gtiff_driver_->Create(kStreamTif, 8, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, src);
      std::vector<GByte> strip(8 * 4, 88);
      for (int y_blk = 0; y_blk < 4; ++y_blk) {
        EXPECT_EQ(CE_None,
                  src->GetRasterBand(1)->WriteBlock(0, y_blk, strip.data()));
      }
      EXPECT_EQ(CE_None, src->Close());
    }

    CPLConfigOptionSetter stream_on("TIFF_READ_STREAMING", "YES", false);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kStreamTif, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> blk(8 * 4, 0);
    // Read later strip (0, 2) first so m_fpL advances past strip 0...
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 2, blk.data()));
    EXPECT_EQ(88, blk[0]);
    // Now read earlier strip (0, 0) -> backward read not supported!
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 0, blk.data()));
      CPLErrorReset();
    }
  }
}

TEST_F(GTiffRasterBandReadTest, MetadataPropertiesNoDataOverviewsAndMasks) {
  // 1. Description, Offset, Scale, UnitType (including 3D vertical SRS unit
  // fallback!), ColorInterp, ColorTable (band 1 vs band 2), Metadata domains,
  // JPEGTABLES, IFD_OFFSET, BLOCK_OFFSET/BLOCK_SIZE, _DEBUG_ HAS_BLOCK_CACHE,
  // and signed GDT_Byte legacy PIXELTYPE.
  {
    constexpr char kJpegPath[] = "/vsimem/rb_meta_jpeg.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kJpegPath); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kJpegPath, 32, 32, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      ds->GetRasterBand(1)->SetDescription("BandOne");
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetOffset(1.5));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetScale(2.5));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->SetUnitType("W/m2"));

      // Compound SRS with vertical units so band 1 (empty unit) falls back to
      // m_pszVertUnit!
      OGRSpatialReference comp;
      OGRSpatialReference horiz;
      OGRSpatialReference vert;
      ASSERT_EQ(OGRERR_NONE, horiz.importFromEPSG(4326));
      ASSERT_EQ(OGRERR_NONE, vert.importFromEPSG(5703));  // NAVD88 height (m)
      ASSERT_EQ(OGRERR_NONE, comp.SetCompoundCS("WGS84+NAVD88", &horiz, &vert));
      EXPECT_EQ(CE_None, ds->SetSpatialRef(&comp));

      std::vector<GByte> tile(16 * 16, 128);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()));
    }

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kJpegPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* b1 = ds->GetRasterBand(1);
    GDALRasterBand* b2 = ds->GetRasterBand(2);

    EXPECT_STREQ("BandOne", b1->GetDescription());
    int ok = 0;
    EXPECT_DOUBLE_EQ(1.5, b1->GetOffset(&ok));
    EXPECT_EQ(TRUE, ok);
    EXPECT_DOUBLE_EQ(2.5, b1->GetScale(&ok));
    EXPECT_EQ(TRUE, ok);
    EXPECT_STREQ("metre", b1->GetUnitType());
    EXPECT_STREQ("W/m2", b2->GetUnitType());
    EXPECT_EQ(GCI_GrayIndex, b1->GetColorInterpretation());
    EXPECT_EQ(nullptr, b1->GetColorTable());
    EXPECT_EQ(nullptr, b2->GetColorTable());

    StringListPtr domains(b1->GetMetadataDomainList(), CSLDestroy);
    EXPECT_EQ(nullptr, domains);
    EXPECT_EQ(nullptr, b1->GetMetadata("IMAGE_STRUCTURE"));
    EXPECT_EQ(nullptr, b1->GetMetadata(nullptr));

    EXPECT_NE(nullptr, b1->GetMetadataItem("JPEGTABLES", "TIFF"));
    EXPECT_NE(nullptr, b1->GetMetadataItem("IFD_OFFSET", "TIFF"));
    EXPECT_NE(nullptr, b2->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF"));
    EXPECT_NE(nullptr, b2->GetMetadataItem("BLOCK_SIZE_0_0", "TIFF"));
    // Sparse tile (1, 0) -> nullptr.
    EXPECT_EQ(nullptr, b1->GetMetadataItem("BLOCK_OFFSET_1_0", "TIFF"));
    EXPECT_EQ(nullptr, b1->GetMetadataItem("BLOCK_SIZE_1_0", "TIFF"));
    // Out-of-bounds block coordinates -> nullptr.
    EXPECT_EQ(nullptr, b1->GetMetadataItem("BLOCK_OFFSET_-1_0", "TIFF"));
    EXPECT_EQ(nullptr, b1->GetMetadataItem("BLOCK_OFFSET_99_0", "TIFF"));
    EXPECT_EQ(nullptr, b1->GetMetadataItem("BLOCK_SIZE_0_-1", "TIFF"));
    EXPECT_EQ(nullptr, b1->GetMetadataItem("BLOCK_SIZE_0_99", "TIFF"));

    // _DEBUG_ HAS_BLOCK_CACHE before and after reading a block.
    EXPECT_STREQ("0", b1->GetMetadataItem("HAS_BLOCK_CACHE", "_DEBUG_"));
    std::vector<GByte> tmp(16 * 16);
    EXPECT_EQ(CE_None, b1->ReadBlock(0, 0, tmp.data()));
    EXPECT_STREQ("1", b1->GetMetadataItem("HAS_BLOCK_CACHE", "_DEBUG_"));

    // PIXELTYPE on GDT_Byte.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, b1->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("PIXELTYPE"));
      CPLErrorReset();
    }

    // Unknown metadata domain -> nullptr.
    EXPECT_EQ(nullptr, b1->GetMetadataItem("JPEGTABLES", "NONEXISTENT"));
  }

  // JPEGTABLES on uncompressed TIFF returns nullptr, and ColorTable on Palette.
  {
    constexpr char kPalPath[] = "/vsimem/rb_meta_pal.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPalPath); };
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPalPath, 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALColorTable ct;
    GDALColorEntry ce = {10, 20, 30, 255};
    ct.SetColorEntry(0, &ce);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetColorTable(&ct));
    EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetColorTable());
    EXPECT_EQ(nullptr,
              ds->GetRasterBand(1)->GetMetadataItem("JPEGTABLES", "TIFF"));
  }

  // 2. GetNoDataValue, GetNoDataValueAsInt64, GetNoDataValueAsUInt64:
  // - PAM NoData override (BASELINE profile)
  // - Multi-band per-band NODATA_VALUES (m_bNoDataSet) vs dataset GDAL_NODATA
  // - Int64 & UInt64 dataset/band NoData + cross-type error paths!
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    constexpr char kPam[] = "/vsimem/rb_nd_pam.tif";
    constexpr char kMb[] = "/vsimem/rb_nd_mb.tif";
    constexpr char kI64[] = "/vsimem/rb_nd_i64.tif";
    constexpr char kU64[] = "/vsimem/rb_nd_u64.tif";
    constexpr char kI64Pam[] = "/vsimem/rb_nd_i64_pam.tif";
    constexpr char kU64Pam[] = "/vsimem/rb_nd_u64_pam.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {kPam, kMb, kI64, kU64, kI64Pam, kU64Pam}) {
        VSIUnlink(p);
        VSIUnlink((std::string(p) + ".aux.xml").c_str());
      }
    };

    // PAM NoData on Byte, Int64, UInt64 (PROFILE=BASELINE forces PAM).
    CPLStringList base_co;
    base_co.SetNameValue("PROFILE", "BASELINE");
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPam, 4, 4, 1, GDT_Byte, base_co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(77.0));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPam, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      EXPECT_DOUBLE_EQ(77.0, ds->GetRasterBand(1)->GetNoDataValue(&ok));
      EXPECT_EQ(TRUE, ok);
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        (void)ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok);
        EXPECT_EQ(FALSE, ok);
        (void)ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok);
        EXPECT_EQ(FALSE, ok);
        CPLErrorReset();
      }
    }

    // Per-band m_bNoDataSet (live on band 1 while band 2 has none) vs
    // dataset-level m_poGDS->m_bNoDataSet on reopen!
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kMb, 4, 4, 2, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(12.0));
      int ok = 0;
      EXPECT_DOUBLE_EQ(12.0, ds->GetRasterBand(1)->GetNoDataValue(&ok));
      EXPECT_EQ(TRUE, ok);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMb, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      EXPECT_DOUBLE_EQ(12.0, ds->GetRasterBand(1)->GetNoDataValue(&ok));
      EXPECT_EQ(TRUE, ok);
    }

    // Int64 TIFF with NoData & without NoData + PAM Int64 NoData.
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kI64, 4, 4, 1, GDT_Int64, nullptr));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_INT64,
                ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok));
      EXPECT_EQ(FALSE, ok);
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetNoDataValueAsInt64(-1234567890123LL));
      // Band-level m_bNoDataSetAsInt64 cast to double:
      EXPECT_NEAR(-1234567890123.0, ds->GetRasterBand(1)->GetNoDataValue(&ok),
                  1.0);
      EXPECT_EQ(TRUE, ok);
      EXPECT_EQ(-1234567890123LL,
                ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok));
      EXPECT_EQ(TRUE, ok);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kI64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      // Dataset-level m_poGDS->m_bNoDataSetAsInt64!
      EXPECT_EQ(-1234567890123LL,
                ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok));
      EXPECT_EQ(TRUE, ok);
      EXPECT_NEAR(-1234567890123.0, ds->GetRasterBand(1)->GetNoDataValue(&ok),
                  1.0);
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        (void)ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok);
        EXPECT_EQ(FALSE, ok);
        CPLErrorReset();
      }
    }
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kI64Pam, 4, 4, 1, GDT_Int64, base_co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsInt64(-999));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kI64Pam, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      EXPECT_EQ(-999, ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok));
      EXPECT_EQ(TRUE, ok);
    }

    // UInt64 TIFF with NoData & without NoData + PAM UInt64 NoData.
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kU64, 4, 4, 1, GDT_UInt64, nullptr));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      (void)ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok);
      EXPECT_EQ(FALSE, ok);
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetNoDataValueAsUInt64(9876543210ULL));
      EXPECT_NEAR(9876543210.0, ds->GetRasterBand(1)->GetNoDataValue(&ok), 1.0);
      EXPECT_EQ(9876543210ULL,
                ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kU64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      EXPECT_EQ(9876543210ULL,
                ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok));
      EXPECT_EQ(TRUE, ok);
      EXPECT_NEAR(9876543210.0, ds->GetRasterBand(1)->GetNoDataValue(&ok), 1.0);
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        (void)ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok);
        EXPECT_EQ(FALSE, ok);
        CPLErrorReset();
      }
    }
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kU64Pam, 4, 4, 1, GDT_UInt64, base_co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsUInt64(888ULL));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kU64Pam, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ok = 0;
      EXPECT_EQ(888ULL, ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok));
      EXPECT_EQ(TRUE, ok);
    }
  }

  // 3. Overviews (internal, external .ovr, implicit JPEG overviews, disabled
  // overviews) & Masks (internal 1-band & per-band, overview mask, external
  // MASK_OVERVIEW_DATASET, GetMaskValueRange 0_AND_255 vs 0_AND_1 vs UNKNOWN).
  {
    // 3a. Implicit JPEG overviews (256x256 JPEG) + downsampled RasterIO.
    constexpr char kJpg256[] = "/vsimem/rb_jpg_ovr.tif";
    absl::Cleanup clean_jpg = [&] { VSIUnlink(kJpg256); };
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kJpg256, 256, 256, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(100));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kJpg256, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
      EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetOverview(0));
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetOverview(99));
      // Downsampled RasterIO increments m_nJPEGOverviewVisibilityCounter ->
      // hits m_poGDS->GetJPEGOverviewCount() inside GetOverviewCount()!
      std::vector<GByte> sub(64 * 64, 0);
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 256, 256, sub.data(), 64, 64,
                             GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));

      // Disable overviews via open option OVERVIEW_LEVEL=NONE!
      const CPLStringList oo{"OVERVIEW_LEVEL=NONE"};
      std::unique_ptr<GDALDataset> ds_no_ovr(
          GDALDataset::Open(kJpg256, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo.List(), nullptr));
      ASSERT_NE(nullptr, ds_no_ovr);
      EXPECT_EQ(0, ds_no_ovr->GetRasterBand(1)->GetOverviewCount());
      EXPECT_EQ(CE_None,
                ds_no_ovr->RasterIO(GF_Read, 0, 0, 256, 256, sub.data(), 64, 64,
                                    GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));
    }

    // 3b. External .ovr + external .msk (created in RO mode).
    constexpr char kExtPath[] = "/vsimem/rb_ext_ovr.tif";
    absl::Cleanup clean_ext = [&] {
      VSIUnlink(kExtPath);
      VSIUnlink("/vsimem/rb_ext_ovr.tif.ovr");
      VSIUnlink("/vsimem/rb_ext_ovr.tif.msk");
    };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kExtPath, 32, 32, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(50));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kExtPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int ovr = 2;
      EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
      EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
      EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetOverview(0));
      EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      EXPECT_EQ(GMF_PER_DATASET, ds->GetRasterBand(1)->GetMaskFlags());
      GDALRasterBand* ext_msk = ds->GetRasterBand(1)->GetMaskBand();
      ASSERT_NE(nullptr, ext_msk);
      EXPECT_TRUE(ext_msk->IsMaskBand());
      EXPECT_EQ(GMVR_UNKNOWN, ext_msk->GetMaskValueRange());
    }

    // 3c. Internal per-dataset 1-bit mask + internal overviews -> tests
    // overview mask delegation, 8-bit promoted vs 1-bit GetMaskValueRange,
    // per-band internal mask, and MASK_OVERVIEW_DATASET open option!
    constexpr char kIntMskPath[] = "/vsimem/rb_int_msk.tif";
    constexpr char kPerbandMskPath[] = "/vsimem/rb_perband_msk.tif";
    constexpr char kExtMoPath[] = "/vsimem/rb_ext_mo.tif";
    absl::Cleanup clean_msk = [&] {
      VSIUnlink(kIntMskPath);
      VSIUnlink(kPerbandMskPath);
      VSIUnlink(kExtMoPath);
    };
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kIntMskPath, 32, 32, 2, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetMaskBand()->Fill(255));
      int ovr = 2;
      EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));

      // Per-band internal mask IFD (2-band main IFD + 2-band FILETYPE_MASK IFD)
      // so m_poMaskDS->GetRasterCount() == 2 -> GetMaskFlags() == 0!
      {
        {
          VSILFILE* fp = VSIFOpenL(kPerbandMskPath, "wb+");
          ASSERT_NE(nullptr, fp);
          absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
          TIFF* tif = VSI_TIFFOpen(kPerbandMskPath, "w", fp);
          ASSERT_NE(nullptr, tif);
          absl::Cleanup close_tif = [tif] { TIFFClose(tif); };

          std::vector<GByte> buf2(16 * 16 * 2, 100);
          TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
          TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
          TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
          TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
          TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
          TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
          TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
          TIFFWriteEncodedStrip(tif, 0, buf2.data(), buf2.size());
          TIFFWriteDirectory(tif);

          std::vector<GByte> msk2(16 * 16 / 8 * 2, 0xFF);
          TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_MASK);
          TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
          TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
          TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
          TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 1);
          TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
          TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MASK);
          TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
          TIFFWriteEncodedStrip(tif, 0, msk2.data(), msk2.size());
          TIFFWriteDirectory(tif);
        }

        std::unique_ptr<GDALDataset> ds_pb(GDALDataset::Open(
            kPerbandMskPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(nullptr, ds_pb);
        EXPECT_EQ(0, ds_pb->GetRasterBand(1)->GetMaskFlags());
        EXPECT_NE(nullptr, ds_pb->GetRasterBand(2)->GetMaskBand());
      }

      // Small 16x16 1-band TIFF to serve as MASK_OVERVIEW_DATASET for 16x16 ovr
      GDALDatasetUniquePtr ds_mo(
          gtiff_driver_->Create(kExtMoPath, 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds_mo);
      EXPECT_EQ(CE_None, ds_mo->GetRasterBand(1)->Fill(255));
    }

    // Open with GDAL_TIFF_INTERNAL_MASK_TO_8BIT=YES (default -> 0_AND_255)
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kIntMskPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GDALRasterBand* b1 = ds->GetRasterBand(1);
      EXPECT_FALSE(b1->IsMaskBand());
      EXPECT_EQ(GMVR_UNKNOWN, b1->GetMaskValueRange());
      EXPECT_EQ(nullptr, b1->GetOverview(-1));
      EXPECT_EQ(nullptr, b1->GetOverview(99));
      GDALRasterBand* msk = b1->GetMaskBand();
      ASSERT_NE(nullptr, msk);
      EXPECT_TRUE(msk->IsMaskBand());
      EXPECT_EQ(GMVR_0_AND_255_ONLY, msk->GetMaskValueRange());

      GDALRasterBand* ovr_b1 = b1->GetOverview(0);
      ASSERT_NE(nullptr, ovr_b1);
      EXPECT_EQ(GMF_PER_DATASET, ovr_b1->GetMaskFlags());
      EXPECT_NE(nullptr, ovr_b1->GetMaskBand());
    }

    // Open with GDAL_TIFF_INTERNAL_MASK_TO_8BIT=NO -> GMVR_0_AND_1_ONLY!
    {
      CPLConfigOptionSetter msk_1bit("GDAL_TIFF_INTERNAL_MASK_TO_8BIT", "NO",
                                     false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kIntMskPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(GMVR_0_AND_1_ONLY,
                ds->GetRasterBand(1)->GetMaskBand()->GetMaskValueRange());
    }

    // Overview without internal mask delegating to base band's external mask,
    // plus MASK_OVERVIEW_DATASET open option!
    constexpr char kOvrNoIntMsk[] = "/vsimem/rb_ovr_base_msk.tif";
    absl::Cleanup clean_obm = [&] {
      VSIUnlink(kOvrNoIntMsk);
      VSIUnlink("/vsimem/rb_ovr_base_msk.tif.msk");
    };
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kOvrNoIntMsk, 32, 32, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      int ovr = 2;
      EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
    }
    {
      // Create external .msk with its own 16x16 overview!
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kOvrNoIntMsk, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      int ovr = 2;
      EXPECT_EQ(
          CE_None,
          ds->GetRasterBand(1)->GetMaskBand()->GetDataset()->BuildOverviews(
              "NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kOvrNoIntMsk, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GDALRasterBand* ovr_b = ds->GetRasterBand(1)->GetOverview(0);
      ASSERT_NE(nullptr, ovr_b);
      EXPECT_EQ(GMF_PER_DATASET, ovr_b->GetMaskFlags());
      GDALRasterBand* ovr_m = ovr_b->GetMaskBand();
      ASSERT_NE(nullptr, ovr_m);
      EXPECT_EQ(16, ovr_m->GetXSize());

      // Also open with MASK_OVERVIEW_DATASET -> m_poExternalMaskDS != nullptr!
      CPLStringList oo;
      oo.SetNameValue("MASK_OVERVIEW_DATASET", kExtMoPath);
      std::unique_ptr<GDALDataset> ds_mo(
          GDALDataset::Open(kOvrNoIntMsk, GDAL_OF_RASTER | GDAL_OF_READONLY,
                            nullptr, oo.List(), nullptr));
      ASSERT_NE(nullptr, ds_mo);
      GDALRasterBand* ovr_b_mo = ds_mo->GetRasterBand(1)->GetOverview(0);
      ASSERT_NE(nullptr, ovr_b_mo);
      EXPECT_EQ(GMF_PER_DATASET, ovr_b_mo->GetMaskFlags());
      EXPECT_NE(nullptr, ovr_b_mo->GetMaskBand());
    }
  }
}

// TODO: b/335317901 - Cover remaining raster-band read edge cases and
// low-probability I/O failure branches in gtiffrasterband_read.cpp.

}  // namespace
}  // namespace autotest2
