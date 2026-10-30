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

#include "third_party/gdal/frmts/gtiff/gtiffdataset.h"

#include <sys/mman.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/gtiffrasterband.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace {

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
    return CPLSPrintf("/proc/self/fd/%d", fd);
  }

  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = fds_.find(pszFilename);
      const bool create = strchr(pszAccess, 'w') != nullptr ||
                          strchr(pszAccess, 'a') != nullptr;
      if (it == fds_.end()) {
        if (!create) return nullptr;
        fd = memfd_create("gtiff_ds_vmem", 0);
        if (fd < 0) return nullptr;
        fds_[pszFilename] = fd;
      } else {
        fd = it->second;
      }
      // Any "w" mode truncates (a no-op on a freshly created memfd).
      if (strchr(pszAccess, 'w') != nullptr) {
        (void)ftruncate(fd, 0);
      }
    }
    const char* delegated_access =
        (strcmp(pszAccess, "wb+") == 0 || strcmp(pszAccess, "w+b") == 0)
            ? "r+b"
            : pszAccess;
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
  static constexpr const char* kPrefix = "/vsimem_ds_mr/";

  static std::string ToMem(const char* path) {
    if (strncmp(path, kPrefix, strlen(kPrefix)) == 0) {
      return std::string("/vsimem/ds_mr_backing_") + (path + strlen(kPrefix));
    }
    return path;
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

DsMemFdHandler* g_ds_memfd = nullptr;
DsMultiRangeHandler* g_ds_mr = nullptr;

class GTiffDatasetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();
    g_ds_memfd = new DsMemFdHandler();
    VSIFileManager::InstallHandler("/vsimem_ds_fd/", g_ds_memfd);
    g_ds_mr = new DsMultiRangeHandler();
    VSIFileManager::InstallHandler("/vsimem_ds_mr/", g_ds_mr);
  }

  static void TearDownTestSuite() {
    VSIFileManager::RemoveHandler("/vsimem_ds_fd/");
    delete g_ds_memfd;
    g_ds_memfd = nullptr;
    VSIFileManager::RemoveHandler("/vsimem_ds_mr/");
    delete g_ds_mr;
    g_ds_mr = nullptr;
    CPLVirtualMemManagerTerminate();
  }

  void SetUp() override {
    g_ds_mr->has_pread = false;
    g_ds_mr->fail_close = false;
    g_ds_mr->opt_multi_range = true;
  }
};

TEST_F(GTiffDatasetTest, CtorHelpersHandlesAndComputeBlocks) {
  // 1. GetTIFFTags, GetGTIFFKeysFlavor, GetGeoTIFFVersion.
  EXPECT_NE(GTiffDataset::GetTIFFTags(), nullptr);
  EXPECT_STREQ(GTiffDataset::GetTIFFTags()[0].pszTagName,
               "TIFFTAG_DOCUMENTNAME");

  CPLStringList opts;
  EXPECT_EQ(GetGTIFFKeysFlavor(opts.List()), GEOTIFF_KEYS_STANDARD);
  opts.SetNameValue("GEOTIFF_KEYS_FLAVOR", "ESRI_PE");
  EXPECT_EQ(GetGTIFFKeysFlavor(opts.List()), GEOTIFF_KEYS_ESRI_PE);

  EXPECT_EQ(GetGeoTIFFVersion(opts.List()), GEOTIFF_VERSION_AUTO);
  opts.SetNameValue("GEOTIFF_VERSION", "1.0");
  EXPECT_EQ(GetGeoTIFFVersion(opts.List()), GEOTIFF_VERSION_1_0);
  opts.SetNameValue("GEOTIFF_VERSION", "1.1");
  EXPECT_EQ(GetGeoTIFFVersion(opts.List()), GEOTIFF_VERSION_1_1);

  // 2. Ctor config options: GTIFF_VIRTUAL_MEM_IO=IF_ENOUGH_RAM and YES.
  {
    CPLConfigOptionSetter vm_ram("GTIFF_VIRTUAL_MEM_IO", "IF_ENOUGH_RAM",
                                 false);
    GTiffDataset ds1;
    EXPECT_EQ(ds1.Close(), CE_None);
  }
  {
    CPLConfigOptionSetter vm_yes("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    GTiffDataset ds2;
    EXPECT_EQ(ds2.GetInternalHandle("TIFF_HANDLE"), nullptr);
    EXPECT_EQ(ds2.GetInternalHandle("OTHER"), nullptr);
    EXPECT_EQ(ds2.GetInternalHandle(nullptr), nullptr);
  }

  // 3. ComputeBlocksPerColRowAndBand overflow branches via OpenOffset on raw
  // in-memory TIFFs!
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    // 3a. m_nBlocksPerColumn > INT_MAX / m_nBlocksPerRow:
    // e.g. 1000000 x 1000000 tiled 16x16 -> 62500 x 62500 = 3.9e9 > INT_MAX!
    const char* p1 = "/vsimem/ds_overflow_rc.tif";
    absl::Cleanup c1 = [&] { VSIUnlink(p1); };
    VSILFILE* fp = VSIFOpenL(p1, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(p1, "w", fp);
    ASSERT_NE(tif, nullptr);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 1000000);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1000000);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, 16);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFWriteCheck(tif, 1, "test");
    TIFFWriteDirectory(tif);
    TIFFClose(tif);
    VSIFCloseL(fp);

    EXPECT_EQ(GDALDataset::Open(p1, GDAL_OF_RASTER | GDAL_OF_READONLY),
              nullptr);
  }
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    // 3b. PLANARCONFIG_SEPARATE && m_nBlocksPerBand > INT_MAX / l_nBands:
    // e.g. 500000 x 500000 tiled 16x16 (31250 x 31250 = 976,562,500 < INT_MAX)
    // with 3 bands SEPARATE (976,562,500 * 3 > INT_MAX)!
    const char* p2 = "/vsimem/ds_overflow_bands.tif";
    absl::Cleanup c2 = [&] { VSIUnlink(p2); };
    VSILFILE* fp = VSIFOpenL(p2, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(p2, "w", fp);
    ASSERT_NE(tif, nullptr);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 500000);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 500000);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, 16);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFWriteCheck(tif, 1, "test");
    TIFFWriteDirectory(tif);
    TIFFClose(tif);
    VSIFCloseL(fp);

    EXPECT_EQ(GDALDataset::Open(p2, GDAL_OF_RASTER | GDAL_OF_READONLY),
              nullptr);
  }

  // 4. GTiffDatasetLibGeotiffErrorCallback via malformed GeoKeys in memory.
  {
    const char* p_gk = "/vsimem/ds_bad_geokey.tif";
    absl::Cleanup c_gk = [&] { VSIUnlink(p_gk); };
    VSILFILE* fp = VSIFOpenL(p_gk, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(p_gk, "w", fp);
    ASSERT_NE(tif, nullptr);
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
    TIFFClose(tif);
    VSIFCloseL(fp);

    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p_gk, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_NE(ds->GetInternalHandle("TIFF_HANDLE"), nullptr);
    (void)ds->GetSpatialRef();
  }
}

TEST_F(GTiffDatasetTest, FinalizeCloseAndCloseDependentDatasets) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. COLOR_PROFILE metadata change + ESRI_XML_PAM=YES +
  // m_bFillEmptyTilesAtClosing + NUM_THREADS=2 compression queue +
  // CloseDependentDatasets()!
  {
    const char* path = "/vsimem/ds_finalize_full.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(path);
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
    co.SetNameValue("NUM_THREADS", "2");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 32, 32, 1, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);

    // Write only 1 of 4 tiles so m_bFillEmptyTilesAtClosing remains true!
    std::vector<GByte> tile(16 * 16, 42);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()), CE_None);

    // Set COLOR_PROFILE metadata and xml:ESRI metadata!
    EXPECT_EQ(ds->SetMetadataItem("SOURCE_WHITEPOINT", "0.3127,0.3290",
                                  "COLOR_PROFILE"),
              CE_None);
    const char* esri_md[] = {"<ESRI>test</ESRI>", nullptr};
    EXPECT_EQ(ds->SetMetadata(const_cast<char**>(esri_md), "xml:ESRI"),
              CE_None);

    // Add internal mask and internal overview so CloseDependentDatasets()
    // returns TRUE (bDroppedRef == true) and overview/mask
    // CloseDependentDatasets returns FALSE (m_poBaseDS != nullptr)!
    EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
    int ovr = 2;
    EXPECT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);
    EXPECT_EQ(ds->Close(), CE_None);
  }

  // Separate check for CloseDependentDatasets() on an already-crystalized
  // dataset with an internal overview and internal mask!
  {
    const char* p_cd = "/vsimem/ds_close_dep.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p_cd); };
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_cd, 16, 16, 1, GDT_Byte, nullptr));
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(1), CE_None);
      EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      int ovr = 2;
      EXPECT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }
    // GTiffDataset::CloseDependentDatasets() is protected, so cpl::down_cast
    // cannot be used from the test. This helper reaches the virtual method
    // through a GDALDataset-derived accessor; the static_cast is technically
    // an invalid downcast (the object is a GTiffDataset, not an
    // AccessCloseDep) but only a virtual call is made through it.
    struct AccessCloseDep : public GDALDataset {
      static int Call(GDALDataset* p) {
        return static_cast<AccessCloseDep*>(p)->CloseDependentDatasets();
      }
    };
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p_cd, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_band, nullptr);
    GDALDataset* ovr_ds = ovr_band->GetDataset();
    ASSERT_NE(ovr_ds, nullptr);
    EXPECT_EQ(AccessCloseDep::Call(ovr_ds), FALSE);
    EXPECT_EQ(AccessCloseDep::Call(ds.get()), TRUE);
    // Second Close() exercises m_bIsFinalized == true early return!
    EXPECT_EQ(ds->Close(), CE_None);
  }

  // 2. Implicit JPEG overviews cleanup in Finalize() + GA_ReadOnly
  // SetMetadata pushing to PAM on Close().
  {
    const char* path = "/vsimem/ds_finalize_jpg.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(path);
      VSIUnlink("/vsimem/ds_finalize_jpg.tif.aux.xml");
    };
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 256, 256, 1, GDT_Byte, co.List()));
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(80), CE_None);
    }
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_NE(ds->GetRasterBand(1)->GetOverview(0), nullptr);
    EXPECT_EQ(ds->SetMetadataItem("RO_KEY", "RO_VAL"), CE_None);
    EXPECT_EQ(ds->Close(), CE_None);
  }

  // 3. STREAMABLE_OUTPUT=YES (sets m_pszTmpFilename & m_fpToWrite) +
  // @SUPPRESS_ASAP=YES (calls CancelCreation()).
  {
    const char* p_stream = "/vsimem/ds_stream_out.tif";
    const char* p_sup = "/vsimem/ds_suppress.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(p_stream);
      VSIUnlink(p_sup);
    };
    {
      CPLStringList co;
      co.SetNameValue("STREAMABLE_OUTPUT", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_stream, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> strip(8 * 4, 11);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, strip.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, strip.data()), CE_None);
      EXPECT_EQ(ds->Close(), CE_None);
    }
    {
      CPLStringList co;
      co.SetNameValue("@SUPPRESS_ASAP", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_sup, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->Close(), CE_None);
    }
  }

  // 4. COG reopened in GA_Update with IGNORE_COG_LAYOUT_BREAK=YES -> sets
  // m_bWriteKnownIncompatibleEdition = true -> patches header in Finalize()!
  {
    const char* src_p = "/vsimem/ds_cog_src.tif";
    const char* cog_p = "/vsimem/ds_cog_edit.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(src_p);
      VSIUnlink(cog_p);
    };
    {
      std::unique_ptr<GDALDataset> src(
          drv->Create(src_p, 32, 32, 1, GDT_Byte, nullptr));
      EXPECT_EQ(src->GetRasterBand(1)->Fill(10), CE_None);
      GDALDriver* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
      CPLStringList co;
      co.SetNameValue("COMPRESS", "LZW");
      std::unique_ptr<GDALDataset> cog(cog_drv->CreateCopy(
          cog_p, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(cog, nullptr);
    }
    const char* const oo[] = {"IGNORE_COG_LAYOUT_BREAK=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        cog_p, GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr, oo, nullptr));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->SetMetadataItem("EDIT_COG", "1"), CE_None);
    std::vector<GByte> random_data(32 * 32);
    for (int i = 0; i < 1024; ++i) random_data[i] = static_cast<GByte>(i * 37);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 32, 32,
                                             random_data.data(), 32, 32,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->Close(), CE_None);

    // Reopen RO and verify KNOWN_INCOMPATIBLE_EDITION warning / layout removal.
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> ds_ro(
        GDALDataset::Open(cog_p, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_ro, nullptr);
    EXPECT_EQ(ds_ro->GetMetadataItem("LAYOUT", "IMAGE_STRUCTURE"), nullptr);
  }

  // 5. Close() failure paths: ||maxlength=900 write error AND VSIFCloseL error.
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    const char* p_max = "/vsimem/ds_close_err.tif||maxlength=900";
    absl::Cleanup clean = [&] { VSIUnlink("/vsimem/ds_close_err.tif"); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "64");
    co.SetNameValue("BLOCKYSIZE", "64");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_max, 128, 128, 1, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> tile(64 * 64, 55);
    (void)ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data());
    EXPECT_EQ(ds->Close(), CE_Failure);
  }
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    const char* p_fc = "/vsimem_ds_mr/ds_fail_close.tif";
    const std::string backing = DsMultiRangeHandler::ToMem(p_fc);
    absl::Cleanup clean = [&] {
      g_ds_mr->fail_close = false;
      VSIUnlink(backing.c_str());
    };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_fc, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
    }
    std::unique_ptr<GDALDataset> ds_ro(
        GDALDataset::Open(p_fc, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_ro, nullptr);
    g_ds_mr->fail_close = true;
    EXPECT_EQ(ds_ro->Close(), CE_Failure);
  }
}

TEST_F(GTiffDatasetTest, IRasterIOAllBranches) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. TryOverviewRasterIO delegation (downsampling on dataset with overviews).
  {
    const char* p = "/vsimem/ds_irio_ovr.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p); };
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p, 64, 64, 3, GDT_Byte, nullptr));
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(10), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->Fill(20), CE_None);
    EXPECT_EQ(ds->GetRasterBand(3)->Fill(30), CE_None);
    int ovr = 2;
    EXPECT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);
    std::vector<GByte> sub(16 * 16 * 3, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, sub.data(), 16, 16, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(sub[0], 10);
  }

  // 2. VirtualMemIO via /vsimem_ds_fd/ (GTIFF_VIRTUAL_MEM_IO=YES).
  {
    CPLConfigOptionSetter vm_yes("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    const char* p = "/vsimem_ds_fd/ds_irio_vmem.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p); };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p, 16, 16, 2, GDT_Byte, nullptr));
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(33), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(66), CE_None);
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> out(16 * 16 * 2, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 16, out.data(), 16, 16, GDT_Byte,
                           2, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out[0], 33);
    EXPECT_EQ(out[256], 66);
  }

  // 3. DirectIO via GTIFF_DIRECT_IO=YES.
  {
    CPLConfigOptionSetter dio_yes("GTIFF_DIRECT_IO", "YES", false);
    const char* p = "/vsimem/ds_irio_dio.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p); };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p, 16, 16, 2, GDT_Byte, nullptr));
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(44), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(88), CE_None);
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> out(16 * 16 * 2, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 16, out.data(), 16, 16, GDT_Byte,
                           2, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out[0], 44);
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
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 32, 32, 2, GDT_Byte, co.List()));
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(12), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(34), CE_None);
    }

    // MultiThreadedRead on /vsimem/.
    {
      const char* const oo[] = {"NUM_THREADS=2", nullptr};
      std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
          p_mt, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo, nullptr));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> out(32 * 32 * 2, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                             GDT_Byte, 2, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(out[0], 12);
    }

    // CacheMultiRange on /vsimem_ds_mr/ (HasPRead=false).
    {
      g_ds_mr->opt_multi_range = true;
      g_ds_mr->has_pread = false;
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_mr, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> out(32 * 32 * 2, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                             GDT_Byte, 2, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(out[0], 12);
    }
  }

  // 5. Whole-block write optimization in IRasterIO:
  // - Single block BIP fast path (no temp buffer)
  // - Multi-block BIP with partial right/bottom edge tiles (20x20 on 16x16)
  // - Multi-block BSQ / reversed band map ("random" spacing loop)
  // - Fallback when HasBlockCache() is true
  // - GTIFF_DONT_WRITE_BLOCKS=YES debug short-circuit!
  {
    const char* p_wb = "/vsimem/ds_wb_write.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p_wb); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_wb, 20, 20, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);

    // 5a. Single full block (0,0,16,16) BIP -> direct WriteEncodedTileOrStrip!
    std::vector<GByte> bip16(16 * 16 * 3, 50);
    EXPECT_EQ(ds->RasterIO(GF_Write, 0, 0, 16, 16, bip16.data(), 16, 16,
                           GDT_Byte, 3, nullptr, 3, 16 * 3, 1, nullptr),
              CE_None);

    // 5b. Full image (0,0,20,20) BIP -> multi-block + partial edge tile zeroing
    std::vector<GByte> bip20(20 * 20 * 3, 60);
    EXPECT_EQ(ds->RasterIO(GF_Write, 0, 0, 20, 20, bip20.data(), 20, 20,
                           GDT_Byte, 3, nullptr, 3, 20 * 3, 1, nullptr),
              CE_None);

    // 5c. Full image (0,0,20,20) with reversed bandMap + BSQ spacing!
    int rev_map[3] = {3, 2, 1};
    std::vector<GByte> bsq20(20 * 20 * 3, 70);
    EXPECT_EQ(ds->RasterIO(GF_Write, 0, 0, 20, 20, bsq20.data(), 20, 20,
                           GDT_Byte, 3, rev_map, 1, 20, 400, nullptr),
              CE_None);

    // 5d. Trigger block cache on band 1 so bOptimOK becomes false -> fallback!
    std::vector<GByte> tmp_blk(16 * 16);
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, tmp_blk.data()), CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Write, 0, 0, 16, 16, bip16.data(), 16, 16,
                           GDT_Byte, 3, nullptr, 3, 16 * 3, 1, nullptr),
              CE_None);
  }
  {
    CPLConfigOptionSetter no_write("GTIFF_DONT_WRITE_BLOCKS", "YES", false);
    const char* p_nw = "/vsimem/ds_dont_write.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p_nw); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_nw, 16, 16, 2, GDT_Byte, co.List()));
    std::vector<GByte> bip(16 * 16 * 2, 99);
    EXPECT_EQ(ds->RasterIO(GF_Write, 0, 0, 16, 16, bip.data(), 16, 16, GDT_Byte,
                           2, nullptr, 2, 32, 1, nullptr),
              CE_None);
  }
}

TEST_F(GTiffDatasetTest, SetReloadDirectoryAndRestoreVolatileParameters) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // Exercise RestoreVolatileParameters across codecs in GA_Update by building
  // an internal overview on each dataset so SetDirectory() switches IFDs!
  struct CodecCase {
    const char* name;
    const char* compress;
    const char* extra_k;
    const char* extra_v;
  };
  const CodecCase cases[] = {
      {"jpeg_ycbcr", "JPEG", "PHOTOMETRIC", "YCBCR"},
      {"deflate", "DEFLATE", "ZLEVEL", "6"},
      {"lzma", "LZMA", "LZMA_PRESET", "2"},
      {"zstd", "ZSTD", "ZSTD_LEVEL", "3"},
      {"lerc_zstd", "LERC_ZSTD", "MAX_Z_ERROR", "0.5"},
      {"webp_lossless", "WEBP", "WEBP_LOSSLESS", "YES"},
      {"jxl", "JXL", "JXL_EFFORT", "3"},
  };

  CPLConfigOptionSetter deflate_zlib("GDAL_TIFF_DEFLATE_SUBCODEC", "ZLIB",
                                     false);
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    std::string path = std::string("/vsimem/ds_vol_") + c.name + ".tif";
    absl::Cleanup clean = [&] { VSIUnlink(path.c_str()); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("COMPRESS", c.compress);
    if (c.extra_k) co.SetNameValue(c.extra_k, c.extra_v);
    const int bands = (strcmp(c.name, "jpeg_ycbcr") == 0 ||
                       strcmp(c.name, "webp_lossless") == 0)
                          ? 3
                          : 1;
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 32, 32, bands, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);

    if (strcmp(c.compress, "JPEG") == 0) {
      GTIFFSetJpegQuality(ds.get(), 80);
      GTIFFSetJpegTablesMode(ds.get(), 1);
    } else if (strcmp(c.compress, "DEFLATE") == 0) {
      GTIFFSetZLevel(ds.get(), 6);
    } else if (strcmp(c.compress, "ZSTD") == 0) {
      GTIFFSetZSTDLevel(ds.get(), 3);
    } else if (strcmp(c.compress, "LERC_ZSTD") == 0) {
      GTIFFSetZSTDLevel(ds.get(), 3);
      GTIFFSetMaxZError(ds.get(), 0.5);
    } else if (strcmp(c.compress, "WEBP") == 0) {
      GTIFFSetWebPLevel(ds.get(), 100);
      GTIFFSetWebPLossless(ds.get(), true);
    } else if (strcmp(c.compress, "JXL") == 0) {
      GTIFFSetJXLLossless(ds.get(), false);
      GTIFFSetJXLEffort(ds.get(), 3);
      GTIFFSetJXLDistance(ds.get(), 1.5f);
      GTIFFSetJXLAlphaDistance(ds.get(), 1.5f);
    }

    for (int b = 1; b <= bands; ++b) {
      EXPECT_EQ(ds->GetRasterBand(b)->Fill(100), CE_None);
    }
    int ovr = 2;
    EXPECT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);
    // Write to overview then write to base band -> forces SetDirectory() &
    // RestoreVolatileParameters() in GA_Update!
    GDALRasterBand* ovr_b = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_b, nullptr);
    EXPECT_EQ(ovr_b->Fill(50), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(120), CE_None);
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
    const char* path = "/vsimem/ds_scan_complex.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    VSILFILE* fp = VSIFOpenL(path, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(path, "w", fp);
    ASSERT_NE(tif, nullptr);

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
    TIFFWriteDirectory(tif);  // libtiff patches subifd_offsets[0] automatically

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
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 3000000000U);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, 16);
    TIFFWriteCheck(tif, 1, "test");
    TIFFWriteDirectory(tif);

    TIFFClose(tif);
    VSIFCloseL(fp);

    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    char** subds = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(subds, nullptr);
    // Find the SUBDATASET_*_DESC containing "vertical_shift_grid" (its index
    // equals iDirIndex of the FILETYPE_PAGE IFD, i.e. 7).
    bool found_grid_name = false;
    for (int i = 0; subds[i] != nullptr; ++i) {
      if (strstr(subds[i], "vertical_shift_grid") != nullptr) {
        found_grid_name = true;
        break;
      }
    }
    EXPECT_TRUE(found_grid_name);

    EXPECT_EQ(ds->GetRasterBand(1)->GetOverviewCount(), 1);
    GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr, nullptr);
    EXPECT_STREQ(ovr->GetMetadataItem("RESAMPLING"), "BILINEAR");

    GDALRasterBand* msk = ds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(msk, nullptr);
    EXPECT_EQ(msk->GetOverviewCount(), 1);
  }
}

TEST_F(GTiffDatasetTest, GetFileListAndGetRawBinaryLayout) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. GetFileList:
  // - Overview dataset (m_poBaseDS != nullptr) returns nullptr
  // - Main dataset with .IMD, .RPB, .tfw, ESRI .xml (EPSG<=32767), and .vat.dbf
  {
    const char* base = "/vsimem/ds_filelist.tif";
    const char* imd = "/vsimem/ds_filelist.IMD";
    const char* rpb = "/vsimem/ds_filelist.RPB";
    const char* tfw = "/vsimem/ds_filelist.tfw";
    const char* xml = "/vsimem/ds_filelist.xml";
    const char* vat = "/vsimem/ds_filelist.tif.vat.dbf";
    absl::Cleanup clean = [&] {
      for (const char* p : {base, imd, rpb, tfw, xml, vat}) VSIUnlink(p);
    };

    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(base, 16, 16, 1, GDT_Byte, nullptr));
      int ovr = 2;
      EXPECT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }

    auto write_text = [](const char* p, const char* txt) -> bool {
      VSILFILE* f = VSIFOpenL(p, "wb");
      if (f == nullptr) return false;
      VSIFWriteL(txt, 1, strlen(txt), f);
      VSIFCloseL(f);
      return true;
    };
    ASSERT_TRUE(write_text(imd,
                           "BEGIN_GROUP = IMAGE_1\n\tsatId = \"QB02\";\nEND_GROUP = "
                           "IMAGE_1\nEND;\n"));
    ASSERT_TRUE(write_text(
        rpb,
        "satId = \"QB02\";\nBEGIN_GROUP = IMAGE\n"
        "\terrBias = 0.0;\n\terrRand = 0.0;\n\tlineOffset = 0;\n"
        "\tsampOffset = 0;\n\tlatOffset = 0;\n\tlongOffset = 0;\n"
        "\theightOffset = 0;\n\tlineScale = 1;\n\tsampScale = 1;\n"
        "\tlatScale = 1;\n\tlongScale = 1;\n\theightScale = 1;\n"
        "\tlineNumCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "\tlineDenCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "\tsampNumCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "\tsampDenCoef = (1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);\n"
        "END_GROUP = IMAGE\nEND;\n"));
    ASSERT_TRUE(write_text(tfw, "1.0\n0.0\n0.0\n-1.0\n100.0\n200.0\n"));
    ASSERT_TRUE(write_text(
        xml,
        "<metadata><refSysInfo><RefSystem><refSysID><identCode code=\"4326\"/>"
        "</refSysID></RefSystem></refSysInfo></metadata>"));
    ASSERT_TRUE(write_text(vat, "dummy"));

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(base, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    GDALGeoTransform gt;
    EXPECT_EQ(ds->GetGeoTransform(gt), CE_None);

    // Overview GetFileList returns nullptr.
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_band, nullptr);
    GDALDataset* ovr_ds = ovr_band->GetDataset();
    ASSERT_NE(ovr_ds, nullptr);
    EXPECT_EQ(ovr_ds->GetFileList(), nullptr);

    char** fl = ds->GetFileList();
    ASSERT_NE(fl, nullptr);
    EXPECT_GE(CSLFindString(fl, tfw), 0);
    EXPECT_GE(CSLFindString(fl, xml), 0);
    EXPECT_GE(CSLFindString(fl, vat), 0);
    EXPECT_GE(CSLFindString(fl, rpb), 0);
    CSLDestroy(fl);
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
    const char* p_lzw = "/vsimem/ds_rbl_lzw.tif";
    const char* p_nb = "/vsimem/ds_rbl_nb12.tif";
    const char* p_cx = "/vsimem/ds_rbl_cx.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(p_lzw);
      VSIUnlink(p_nb);
      VSIUnlink(p_cx);
    };
    CPLStringList co_lzw;
    co_lzw.SetNameValue("COMPRESS", "LZW");
    std::unique_ptr<GDALDataset> ds1(
        drv->Create(p_lzw, 8, 8, 1, GDT_Byte, co_lzw.List()));
    EXPECT_FALSE(ds1->GetRawBinaryLayout(layout));

    CPLStringList co_nb;
    co_nb.SetNameValue("NBITS", "12");
    std::unique_ptr<GDALDataset> ds2(
        drv->Create(p_nb, 8, 8, 1, GDT_UInt16, co_nb.List()));
    EXPECT_FALSE(ds2->GetRawBinaryLayout(layout));

    std::unique_ptr<GDALDataset> ds3(
        drv->Create(p_cx, 8, 8, 1, GDT_CInt16, nullptr));
    EXPECT_FALSE(ds3->GetRawBinaryLayout(layout));
  }

  // 2b. Tiled: multi-tile (false), single-tile CONTIG (true), single-tile
  // 3-band SEPARATE (true), and sparse/non-uniform single-tile SEPARATE (false)
  {
    const char* p_mt = "/vsimem/ds_rbl_mt.tif";
    const char* p_st_ctg = "/vsimem/ds_rbl_st_ctg.tif";
    const char* p_st_sep = "/vsimem/ds_rbl_st_sep.tif";
    const char* p_st_bad = "/vsimem/ds_rbl_st_bad.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {p_mt, p_st_ctg, p_st_sep, p_st_bad}) VSIUnlink(p);
    };

    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");

    std::unique_ptr<GDALDataset> d_mt(
        drv->Create(p_mt, 32, 32, 1, GDT_Byte, co.List()));
    EXPECT_FALSE(d_mt->GetRawBinaryLayout(layout));

    std::unique_ptr<GDALDataset> d_ctg(
        drv->Create(p_st_ctg, 16, 16, 3, GDT_Byte, co.List()));
    EXPECT_EQ(d_ctg->GetRasterBand(1)->Fill(1), CE_None);
    EXPECT_TRUE(d_ctg->GetRawBinaryLayout(layout));
    EXPECT_EQ(layout.eInterleaving,
              GDALDataset::RawBinaryLayout::Interleaving::BIP);

    co.SetNameValue("INTERLEAVE", "BAND");
    std::unique_ptr<GDALDataset> d_sep(
        drv->Create(p_st_sep, 16, 16, 3, GDT_Byte, co.List()));
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(d_sep->GetRasterBand(b)->Fill(b), CE_None);
    }
    d_sep.reset();
    d_sep.reset(GDALDataset::Open(p_st_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(d_sep, nullptr);
    EXPECT_TRUE(d_sep->GetRawBinaryLayout(layout));
    EXPECT_EQ(layout.eInterleaving,
              GDALDataset::RawBinaryLayout::Interleaving::BSQ);

    // Non-uniform band spacing in single-tile 3-band SEPARATE: write band 1,
    // band 3, then band 2 with SPARSE_OK=YES!
    co.SetNameValue("SPARSE_OK", "YES");
    std::unique_ptr<GDALDataset> d_bad(
        drv->Create(p_st_bad, 16, 16, 3, GDT_Byte, co.List()));
    std::vector<GByte> blk(256, 1);
    EXPECT_EQ(d_bad->GetRasterBand(1)->WriteBlock(0, 0, blk.data()), CE_None);
    d_bad->FlushCache(false);
    EXPECT_EQ(d_bad->GetRasterBand(3)->WriteBlock(0, 0, blk.data()), CE_None);
    d_bad->FlushCache(false);
    EXPECT_EQ(d_bad->GetRasterBand(2)->WriteBlock(0, 0, blk.data()), CE_None);
    d_bad.reset();
    d_bad.reset(GDALDataset::Open(p_st_bad, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(d_bad, nullptr);
    EXPECT_FALSE(d_bad->GetRawBinaryLayout(layout));
  }

  // 2c. Stripped: 1-band inverted endianness (true), 3-band SEPARATE BSQ
  // multi-strip (true), non-contiguous CONTIG strips (false), and non-uniform
  // SEPARATE strips (false).
  {
    const char* p_be = "/vsimem/ds_rbl_be.tif";
    const char* p_sep = "/vsimem/ds_rbl_sep_str.tif";
    const char* p_bad_ctg = "/vsimem/ds_rbl_bad_ctg.tif";
    const char* p_bad_sep = "/vsimem/ds_rbl_bad_sep.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {p_be, p_sep, p_bad_ctg, p_bad_sep}) VSIUnlink(p);
    };

    {
      CPLStringList co;
      co.SetNameValue("ENDIANNESS", "INVERTED");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_be, 8, 8, 1, GDT_UInt16, co.List()));
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(100), CE_None);
      ds.reset();
      ds.reset(GDALDataset::Open(p_be, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_TRUE(ds->GetRawBinaryLayout(layout));
      EXPECT_FALSE(layout.bLittleEndianOrder);
    }

    {
      CPLStringList co;
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_sep, 8, 8, 3, GDT_Byte, co.List()));
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(ds->GetRasterBand(b)->Fill(b), CE_None);
      }
      ds.reset();
      ds.reset(GDALDataset::Open(p_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_TRUE(ds->GetRawBinaryLayout(layout));
      EXPECT_EQ(layout.eInterleaving,
                GDALDataset::RawBinaryLayout::Interleaving::BSQ);
    }

    // Out-of-order strips in CONTIG -> false!
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_bad_ctg, 8, 8, 1, GDT_Byte, co.List()));
      std::vector<GByte> s(32, 1);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, s.data()), CE_None);
      ds->FlushCache(false);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, s.data()), CE_None);
      ds.reset();
      ds.reset(GDALDataset::Open(p_bad_ctg, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_FALSE(ds->GetRawBinaryLayout(layout));
    }

    // Out-of-order bands/strips in SEPARATE -> false!
    {
      CPLStringList co;
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_bad_sep, 8, 8, 3, GDT_Byte, co.List()));
      std::vector<GByte> s(32, 1);
      // Write band 1 strips contiguous, band 3 strips contiguous, then band 2!
      for (int b : {1, 3, 2}) {
        EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(0, 0, s.data()), CE_None);
        ds->FlushCache(false);
        EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(0, 1, s.data()), CE_None);
        ds->FlushCache(false);
      }
      ds.reset();
      ds.reset(GDALDataset::Open(p_bad_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_FALSE(ds->GetRawBinaryLayout(layout));
    }
  }
}

}  // namespace
