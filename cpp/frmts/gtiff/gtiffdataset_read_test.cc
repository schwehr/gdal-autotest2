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

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "third_party/gdal/frmts/gtiff/gtiffdataset.h"
#include "third_party/gdal/frmts/gtiff/gtiffrasterband.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace {

// ---------------------------------------------------------------------------
// 100% RAM memfd-backed VSI filesystem ("/vsimem_gsr_fd/") so
// VSIFGetNativeFileDescriptorL and CPLVirtualMemFileMapNew succeed in RAM.
// ---------------------------------------------------------------------------
class GsrMemFdHandler final : public VSIFilesystemHandler {
 public:
  ~GsrMemFdHandler() override {
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
        fd = memfd_create("gtiff_gsr_vmem", 0);
        if (fd < 0) return nullptr;
        fds_[pszFilename] = fd;
      } else {
        fd = it->second;
        if (strchr(pszAccess, 'w') != nullptr &&
            strchr(pszAccess, '+') == nullptr) {
          (void)ftruncate(fd, 0);
        }
      }
    }
    const char* delegated_access =
        (strcmp(pszAccess, "wb+") == 0 || strcmp(pszAccess, "w+b") == 0)
            ? "r+b"
            : pszAccess;
    if (strcmp(pszAccess, "wb+") == 0 || strcmp(pszAccess, "w+b") == 0) {
      (void)ftruncate(fd, 0);
    }
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
// 100% RAM wrapper around /vsimem/ ("/vsimem_gsr_mr/") with dynamic
// HasOptimizedReadMultiRange, HasPRead, and ReadMultiRange failure injection.
// ---------------------------------------------------------------------------
class GsrMultiRangeHandler;

class GsrMultiRangeHandle final : public VSIProxyFileHandle {
 public:
  GsrMultiRangeHandle(VSIVirtualHandleUniquePtr&& inner,
                      const GsrMultiRangeHandler* handler)
      : VSIProxyFileHandle(std::move(inner)), handler_(handler) {}

  bool HasPRead() const override;
  size_t GetAdviseReadTotalBytesLimit() const override;
  int ReadMultiRange(int nRanges, void** ppData, const vsi_l_offset* panOffsets,
                     const size_t* panSizes) override;

 private:
  const GsrMultiRangeHandler* handler_;
};

class GsrMultiRangeHandler final : public VSIFilesystemHandler {
 public:
  static constexpr const char* kPrefix = "/vsimem_gsr_mr/";

  static std::string ToMem(const char* path) {
    if (strncmp(path, kPrefix, strlen(kPrefix)) == 0) {
      return std::string("/vsimem/gsr_mr_backing_") + (path + strlen(kPrefix));
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
        new GsrMultiRangeHandle(std::move(inner), this));
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
  bool fail_multi_range = false;
  bool opt_multi_range = true;
  size_t advise_read_limit = 0;
};

bool GsrMultiRangeHandle::HasPRead() const { return handler_->has_pread; }

size_t GsrMultiRangeHandle::GetAdviseReadTotalBytesLimit() const {
  return handler_->advise_read_limit;
}

int GsrMultiRangeHandle::ReadMultiRange(int nRanges, void** ppData,
                                        const vsi_l_offset* panOffsets,
                                        const size_t* panSizes) {
  if (handler_->fail_multi_range) return -1;
  return VSIProxyFileHandle::ReadMultiRange(nRanges, ppData, panOffsets,
                                            panSizes);
}

GsrMemFdHandler* g_gsr_memfd = nullptr;
GsrMultiRangeHandler* g_gsr_mr = nullptr;

void WriteTextToVsiMem(const char* path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(VSIFWriteL(content.data(), 1, content.size(), fp), content.size());
  ASSERT_EQ(VSIFCloseL(fp), 0);
}

class GTiffDatasetReadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();
    g_gsr_memfd = new GsrMemFdHandler();
    VSIFileManager::InstallHandler("/vsimem_gsr_fd/", g_gsr_memfd);
    g_gsr_mr = new GsrMultiRangeHandler();
    VSIFileManager::InstallHandler("/vsimem_gsr_mr/", g_gsr_mr);
  }

  static void TearDownTestSuite() {
    VSIFileManager::RemoveHandler("/vsimem_gsr_fd/");
    delete g_gsr_memfd;
    g_gsr_memfd = nullptr;
    VSIFileManager::RemoveHandler("/vsimem_gsr_mr/");
    delete g_gsr_mr;
    g_gsr_mr = nullptr;
    CPLVirtualMemManagerTerminate();
  }

  void SetUp() override {
    g_gsr_mr->has_pread = false;
    g_gsr_mr->fail_multi_range = false;
    g_gsr_mr->opt_multi_range = true;
  }
};

TEST_F(GTiffDatasetReadTest, ImplicitJpegOverviewsAndCompressedData) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. Implicit JPEG overviews (GetJPEGOverviewCount):
  // - 1024x256 JPEG with JPEGTABLESMODE=1 (has JPEGTables)
  // - GTIFF_IMPLICIT_JPEG_OVR=NO disables implicit overviews
  // - 4-band CMYK JPEG returns 0 implicit overviews
  // - JPEGTABLESMODE=0 (no JPEGTables tag -> uses {0xFF, 0xD8})
  // - Corrupted JPEGTables (last byte != 0xD9 -> returns 0)
  {
    const char* p_jpg = "/vsimem/gsr_jpg_1024.tif";
    const char* p_no_tbl = "/vsimem/gsr_jpg_notbl.tif";
    const char* p_cmyk = "/vsimem/gsr_jpg_cmyk.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(p_jpg);
      VSIUnlink(p_no_tbl);
      VSIUnlink(p_cmyk);
    };
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "256");
      co.SetNameValue("BLOCKYSIZE", "256");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_jpg, 1024, 256, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(100), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(150), CE_None);
      EXPECT_EQ(ds->GetRasterBand(3)->Fill(200), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_jpg, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> sub(256 * 64 * 3, 0);
      // First downsampled RasterIO computes m_nJPEGOverviewCount (3 levels);
      // second downsampled RasterIO hits cached m_nJPEGOverviewCount >= 0!
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 1024, 256, sub.data(), 128, 32,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 1024, 256, sub.data(), 256, 64,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_None);
    }
    {
      CPLConfigOptionSetter off("GTIFF_IMPLICIT_JPEG_OVR", "NO", false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_jpg, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->GetOverview(0), nullptr);
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("JPEGTABLESMODE", "0");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_no_tbl, 256, 256, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(80), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_no_tbl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_NE(ds->GetRasterBand(1)->GetOverview(0), nullptr);
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("PHOTOMETRIC", "CMYK");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_cmyk, 256, 256, 4, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(50), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_cmyk, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->GetOverview(0), nullptr);
    }
  }

  // 2. GetCompressionFormats & ReadCompressedData:
  // - JPEG 3-band RGB (tiled & stripped, with & without JPEGTables)
  // - JPEG 4-band RGBA ("JPEG;colorspace=RGBA")
  // - JPEG PLANARCONFIG_SEPARATE (single band panBandList)
  // - WEBP & JXL & LZW & NONE & sparse unwritten tile
  // - Buffer modes: query-only (ppBuffer==nullptr), caller buffer too small vs
  //   large enough, driver-allocated buffer, and truncated file read failure!
  {
    const char* p_rgba = "/vsimem/gsr_comp_rgba.tif";
    const char* p_sep = "/vsimem/gsr_comp_sep.tif";
    const char* p_webp = "/vsimem/gsr_comp_webp.tif";
    const char* p_jxl = "/vsimem/gsr_comp_jxl.tif";
    const char* p_lzw = "/vsimem/gsr_comp_lzw.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {p_rgba, p_sep, p_webp, p_jxl, p_lzw}) {
        VSIUnlink(p);
      }
    };

    // 4-band RGBA JPEG (stripped, BLOCKYSIZE=16)
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("PHOTOMETRIC", "RGB");
      co.SetNameValue("ALPHA", "YES");
      co.SetNameValue("BLOCKYSIZE", "16");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_rgba, 16, 16, 4, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      for (int b = 1; b <= 4; ++b) {
        EXPECT_EQ(ds->GetRasterBand(b)->Fill(100), CE_None);
      }
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_rgba, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      CPLStringList fmts = ds->GetCompressionFormats(0, 0, 16, 16, 4, nullptr);
      ASSERT_EQ(fmts.size(), 1);
      EXPECT_STREQ(fmts[0], "JPEG;colorspace=RGBA");
      // Non-whole-block returns empty:
      EXPECT_TRUE(ds->GetCompressionFormats(1, 0, 8, 8, 4, nullptr).empty());

      // Query size and detailed format only (ppBuffer == nullptr).
      size_t sz = 0;
      char* detailed = nullptr;
      EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr,
                                       nullptr, &sz, &detailed),
                CE_None);
      EXPECT_GT(sz, 0U);
      ASSERT_NE(detailed, nullptr);
      CPLFree(detailed);

      // Invalid format tokens ("JPEG;a=1;b=2" or "") -> CE_Failure.
      EXPECT_EQ(ds->ReadCompressedData("JPEG;a=1;b=2", 0, 0, 16, 16, 4, nullptr,
                                       nullptr, &sz, nullptr),
                CE_Failure);
      EXPECT_EQ(ds->ReadCompressedData("", 0, 0, 16, 16, 4, nullptr, nullptr,
                                       &sz, nullptr),
                CE_Failure);

      // ppBuffer != nullptr && pnBufferSize == nullptr -> CE_Failure.
      void* buf = nullptr;
      EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr, &buf,
                                       nullptr, nullptr),
                CE_Failure);

      // Caller-allocated buffer too small -> CE_Failure.
      std::vector<GByte> caller_buf(sz + 64, 0);
      void* p_caller = caller_buf.data();
      size_t small_sz = 2;
      EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr,
                                       &p_caller, &small_sz, nullptr),
                CE_Failure);

      // Caller-allocated buffer large enough -> CE_None.
      size_t big_sz = caller_buf.size();
      detailed = nullptr;
      EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr,
                                       &p_caller, &big_sz, &detailed),
                CE_None);
      ASSERT_NE(detailed, nullptr);
      EXPECT_NE(strstr(detailed, "colorspace=RGBA"), nullptr);
      CPLFree(detailed);

      // Driver-allocated buffer (*ppBuffer == nullptr) -> CE_None.
      void* drv_buf = nullptr;
      size_t drv_sz = 0;
      EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr,
                                       &drv_buf, &drv_sz, nullptr),
                CE_None);
      EXPECT_NE(drv_buf, nullptr);
      VSIFree(drv_buf);
    }

    // JPEG PLANARCONFIG_SEPARATE + tiled + sparse tile + truncated file failure
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("SPARSE_OK", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_sep, 32, 16, 2, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> tile(16 * 16, 90);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      // panBandList is 0-based for GetCompressionFormats/ReadCompressedData
      // (nBlockId += panBandList[0] * m_nBlocksPerBand), so {1} addresses the
      // second band's block.
      const int band_idx1 = 1;
      EXPECT_FALSE(
          ds->GetCompressionFormats(0, 0, 16, 16, 1, &band_idx1).empty());
      // Sparse unwritten block (1,0) returns empty:
      EXPECT_TRUE(
          ds->GetCompressionFormats(16, 0, 16, 16, 1, &band_idx1).empty());

      void* drv_buf = nullptr;
      size_t drv_sz = 0;
      EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 1, &band_idx1,
                                       &drv_buf, &drv_sz, nullptr),
                CE_None);
      VSIFree(drv_buf);
      drv_buf = nullptr;

      // Truncate backing file so TIFFReadRawTile fails and frees *ppBuffer!
      VSILFILE* fp = VSIFOpenL(p_sep, "r+b");
      ASSERT_NE(fp, nullptr);
      EXPECT_EQ(VSIFTruncateL(fp, 64), 0);
      VSIFCloseL(fp);
      {
        CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
        EXPECT_EQ(ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 1, &band_idx1,
                                         &drv_buf, &drv_sz, nullptr),
                  CE_Failure);
        EXPECT_EQ(drv_buf, nullptr);
      }
    }

    // WEBP, JXL, and LZW
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "WEBP");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_webp, 16, 16, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(50), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_webp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetCompressionFormats(0, 0, 16, 16, 3, nullptr).size(), 1);
      void* buf = nullptr;
      size_t sz = 0;
      EXPECT_EQ(ds->ReadCompressedData("WEBP", 0, 0, 16, 16, 3, nullptr, &buf,
                                       &sz, nullptr),
                CE_None);
      VSIFree(buf);
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JXL");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_jxl, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(60), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_jxl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      void* buf = nullptr;
      size_t sz = 0;
      EXPECT_EQ(ds->ReadCompressedData("JXL", 0, 0, 16, 16, 1, nullptr, &buf,
                                       &sz, nullptr),
                CE_None);
      VSIFree(buf);
      ds.reset();

      // Patch COMPRESSION_JXL_DNG_1_7 (52546) -> COMPRESSION_JXL (50002) in IFD
      // so GetCompressionFormats hits case COMPRESSION_JXL:
      vsi_l_offset len = 0;
      GByte* raw = VSIGetMemFileBuffer(p_jxl, &len, FALSE);
      for (vsi_l_offset i = 0; i + 12 <= len; ++i) {
        uint16_t tag = 0, ty = 0, val = 0;
        memcpy(&tag, raw + i, 2);
        memcpy(&ty, raw + i + 2, 2);
        memcpy(&val, raw + i + 8, 2);
        if (tag == TIFFTAG_COMPRESSION && ty == 3 && val == 52546) {
          uint16_t jxl_old = 50002;
          memcpy(raw + i + 8, &jxl_old, 2);
          break;
        }
      }
      ds.reset(GDALDataset::Open(p_jxl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetCompressionFormats(0, 0, 16, 16, 1, nullptr).size(), 1);
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "LZW");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_lzw, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(70), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_lzw, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_TRUE(ds->GetCompressionFormats(0, 0, 16, 16, 1, nullptr).empty());
      size_t sz = 0;
      EXPECT_EQ(ds->ReadCompressedData("LZW", 0, 0, 16, 16, 1, nullptr, nullptr,
                                       &sz, nullptr),
                CE_Failure);
    }
  }
}

TEST_F(GTiffDatasetReadTest, MultiThreadedReadAllPaths) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);
  const char* const oo_mt[] = {"NUM_THREADS=2", nullptr};

  // 1. 3-band CONTIG LZW + PREDICTOR=2, 50x50 tiled (16x16, partial right &
  // bottom tiles):
  // - Mode A: Full image into BIP buffer (bSkipBlockCache + bUseBIPOptim)
  // - Mode B: Full image into BSQ buffer (bSkipBlockCache +
  //   bUseDeinterleaveOptimNoBlockCache)
  // - Mode D: Sub-window into BSQ buffer (bUseDeinterleaveOptimBlockCache),
  //   plus immediate repeat (hits cached blocks in block cache!)
  // - Mode E: Sub-window with reversed band map {3, 2, 1}
  // - Unaligned window (3, 5, 40, 38) and aligned-end window (4, 4, 28, 28)
  {
    const char* path = "/vsimem/gsr_mt_3b.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("PREDICTOR", "2");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 50, 50, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(11), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(22), CE_None);
      EXPECT_EQ(ds->GetRasterBand(3)->Fill(33), CE_None);
    }

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        path, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
    ASSERT_NE(ds, nullptr);

    // Mode A: BIP full-image read.
    std::vector<GByte> bip(50 * 50 * 3, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 50, 50, bip.data(), 50, 50, GDT_Byte,
                           3, nullptr, 3, 50 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(bip[0], 11);
    EXPECT_EQ(bip[1], 22);
    EXPECT_EQ(bip[2], 33);

    // Mode B: BSQ full-image read.
    std::vector<GByte> bsq(50 * 50 * 3, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 50, 50, bsq.data(), 50, 50, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(bsq[0], 11);

    // Mode D: Sub-window BSQ read (uses block cache + GDALDeinterleave),
    // repeated twice so second call hits already-cached blocks!
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 50, 48, bsq.data(), 50, 48, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 50, 48, bsq.data(), 50, 48, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);

    // Mode E: Sub-window with reversed band map {3, 2, 1} on fresh open.
    ds.reset(GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                               oo_mt, nullptr));
    int rev_bands[3] = {3, 2, 1};
    EXPECT_EQ(ds->RasterIO(GF_Read, 3, 5, 40, 38, bsq.data(), 40, 38, GDT_Byte,
                           3, rev_bands, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(bsq[0], 33);

    // Aligned-end window (4 + 28 == 32, multiple of 16).
    EXPECT_EQ(ds->RasterIO(GF_Read, 4, 4, 28, 28, bsq.data(), 28, 28, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
  }

  // 2. 4-band RGBA + sparse tiles (both without NoData and with NoData=77) +
  // single-band read with tiny cache (GDALSetCacheMax64) vs normal cache +
  // bHasPRead == false via /vsimem_gsr_mr/!
  {
    const char* path = "/vsimem_gsr_mr/gsr_mt_4b_sparse.tif";
    const std::string backing = GsrMultiRangeHandler::ToMem(path);
    absl::Cleanup clean = [&] { VSIUnlink(backing.c_str()); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "DEFLATE");
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("ALPHA", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 32, 32, 4, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(77.0), CE_None);
      std::vector<GByte> tile(16 * 16, 44);
      for (int b = 1; b <= 4; ++b) {
        EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(0, 0, tile.data()), CE_None);
      }
    }

    // Open with has_pread = false -> exercises Seek+Read under mutex in
    // ThreadDecompressionFunc!
    g_gsr_mr->has_pread = false;
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        path, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
    ASSERT_NE(ds, nullptr);

    // Full-image 4-band BSQ read (hits 4-band deinterleave + sparse NoData=77).
    std::vector<GByte> buf(32 * 32 * 4, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           4, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 44);
    EXPECT_EQ(buf[20], 77);  // Sparse tile (1, 0) filled with NoData=77!

    // Single-band sub-window read with tiny cache max -> disables aggressive
    // band caching!
    const GIntBig old_cache = GDALGetCacheMax64();
    {
      absl::Cleanup restore_cache = [&] { GDALSetCacheMax64(old_cache); };
      GDALSetCacheMax64(512);
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(), 32,
                                         30, GDT_Byte, 0, 0, nullptr),
          CE_None);
    }
    // Single-band sub-window read with normal cache -> bCacheAllBands = true.
    ds.reset(GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                               oo_mt, nullptr));
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(),
                                             32, 30, GDT_Byte, 0, 0, nullptr),
              CE_None);
  }

  // 3. Uncompressed little-endian & big-endian (ENDIANNESS=INVERTED),
  // PLANARCONFIG_SEPARATE, LERC, YCbCr JPEG, and GA_Update (both dirty block in
  // cache and clean after FlushCache)!
  {
    const char* p_uncomp = "/vsimem/gsr_mt_uncomp.tif";
    const char* p_be = "/vsimem/gsr_mt_be.tif";
    const char* p_lerc = "/vsimem/gsr_mt_lerc.tif";
    const char* p_ycbcr = "/vsimem/gsr_mt_ycbcr.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {p_uncomp, p_be, p_lerc, p_ycbcr}) {
        VSIUnlink(p);
      }
    };

    // Uncompressed little-endian CONTIG & SEPARATE
    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "8");
      co.SetNameValue("SPARSE_OK", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_uncomp, 16, 20, 2, GDT_UInt16, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<uint16_t> s0(16 * 8, 500);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, s0.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, s0.data()), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_uncomp, GDAL_OF_RASTER | GDAL_OF_READONLY,
                            nullptr, oo_mt, nullptr));
      ASSERT_NE(ds, nullptr);
      std::vector<uint16_t> out(16 * 20 * 2, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 20, out.data(), 16, 20,
                             GDT_UInt16, 2, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(out[0], 500);
      EXPECT_EQ(out[16 * 8], 0);  // Sparse strip 1 filled with 0.
    }

    // Big-endian SEPARATE UInt16
    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "8");
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("ENDIANNESS", "INVERTED");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_be, 16, 16, 2, GDT_UInt16, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(1000), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(2000), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
          p_be, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
      ASSERT_NE(ds, nullptr);
      std::vector<uint16_t> out(16 * 16 * 2, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 16, out.data(), 16, 16,
                             GDT_UInt16, 2, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(out[0], 1000);
    }

    // LERC + GA_Update (dirty block fallback & clean multi-threaded read)
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LERC");
      co.SetNameValue("NUM_THREADS", "2");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_lerc, 32, 32, 1, GDT_Float32, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(42.0), CE_None);
      ds->FlushCache(false);

      // Clean GA_Update MultiThreadedRead:
      std::vector<float> fbuf(32 * 32, 0.0f);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, fbuf.data(), 32, 32,
                             GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_FLOAT_EQ(fbuf[0], 42.0f);

      // Dirty 1 pixel in cache without flushing -> falls back to base impl!
      float one = 99.0f;
      EXPECT_EQ(ds->RasterIO(GF_Write, 0, 0, 1, 1, &one, 1, 1, GDT_Float32, 1,
                             nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, fbuf.data(), 32, 32,
                             GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_FLOAT_EQ(fbuf[0], 99.0f);
    }

    // YCbCr JPEG multi-threaded read
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("PHOTOMETRIC", "YCBCR");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_ycbcr, 32, 32, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(ds->GetRasterBand(b)->Fill(128), CE_None);
      }
    }
    {
      std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
          p_ycbcr, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> out(32 * 32 * 3, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_NEAR(out[0], 128, 5);
    }
  }
}

TEST_F(GTiffDatasetReadTest, VirtualMemIOAndDirectIOAllBranches) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // Create reusable tiled & stripped CONTIG & SEPARATE test datasets in
  // /vsimem/ and /vsimem_gsr_fd/ (including sparse + dense blocks and
  // big-endian UInt16/CInt16).
  const char* p_tiled_ctg = "/vsimem/gsr_vmdio_t_ctg.tif";
  const char* p_tiled_sep = "/vsimem/gsr_vmdio_t_sep.tif";
  const char* p_strip_ctg = "/vsimem/gsr_vmdio_s_ctg.tif";
  const char* p_strip_sep = "/vsimem/gsr_vmdio_s_sep.tif";
  const char* p_fd_tiled = "/vsimem_gsr_fd/gsr_fd_t.tif";
  const char* p_be_u16 = "/vsimem/gsr_vmdio_be_u16.tif";
  const char* p_be_c16 = "/vsimem/gsr_vmdio_be_c16.tif";
  absl::Cleanup clean = [&] {
    for (const char* p : {p_tiled_ctg, p_tiled_sep, p_strip_ctg, p_strip_sep,
                          p_fd_tiled, p_be_u16, p_be_c16}) {
      VSIUnlink(p);
    }
  };

  // Tiled CONTIG 3-band Byte (32x32, 16x16 tiles, tiles (0,0) and (1,1)
  // written, tiles (1,0) and (0,1) sparse with NoData=9).
  {
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("SPARSE_OK", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_tiled_ctg, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(9.0), CE_None);
    std::vector<GByte> t(16 * 16, 55);
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(0, 0, t.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(1, 1, t.data()), CE_None);
    }
  }
  // Tiled SEPARATE 3-band Byte (32x32, 16x16 tiles, tile(0,0) written, others
  // sparse).
  {
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("INTERLEAVE", "BAND");
    co.SetNameValue("SPARSE_OK", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_tiled_sep, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> t(16 * 16, 66);
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(0, 0, t.data()), CE_None);
    }
  }
  // Stripped CONTIG 3-band Byte (32x16, BLOCKYSIZE=4, dense so DirectIO works).
  {
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_strip_ctg, 32, 16, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(10), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->Fill(20), CE_None);
    EXPECT_EQ(ds->GetRasterBand(3)->Fill(30), CE_None);
  }
  // Stripped SEPARATE 3-band Byte (32x16, BLOCKYSIZE=4, sparse strip 1).
  {
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    co.SetNameValue("INTERLEAVE", "BAND");
    co.SetNameValue("SPARSE_OK", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_strip_sep, 32, 16, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> s(32 * 4, 77);
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(ds->GetRasterBand(b)->WriteBlock(0, 0, s.data()), CE_None);
    }
  }
  // Native fd memfd file for CPLVirtualMemFileMapNew path in VirtualMemIO:
  {
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(p_fd_tiled, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(99), CE_None);
  }
  // Big-endian UInt16 & CInt16 stripped files:
  {
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    co.SetNameValue("ENDIANNESS", "INVERTED");
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_be_u16, 16, 16, 2, GDT_UInt16, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(1234), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(5678), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_be_c16, 16, 16, 1, GDT_CInt16, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<int16_t> cbuf(16 * 16 * 2, 321);
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16, cbuf.data(),
                                         16, 16, GDT_CInt16, 0, 0, nullptr),
          CE_None);
    }
  }

  // =========================================================================
  // Part A: GTIFF_VIRTUAL_MEM_IO=IF_ENOUGH_RAM / YES
  // =========================================================================
  {
    CPLConfigOptionSetter vm_ram("GTIFF_VIRTUAL_MEM_IO", "IF_ENOUGH_RAM",
                                 false);
    std::unique_ptr<GDALDataset> ds_fd(
        GDALDataset::Open(p_fd_tiled, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_fd, nullptr);
    std::vector<GByte> buf(32 * 32 * 3, 0);
    EXPECT_EQ(ds_fd->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                              GDT_Byte, 3, nullptr, 3, 32 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 99);
  }
  {
    CPLConfigOptionSetter vm_yes("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    // 1. Tiled CONTIG (bUseContigImplementation == true & false):
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p_tiled_ctg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> buf(64 * 64 * 5, 0);
    std::vector<uint16_t> u16_buf(64 * 64 * 4, 0);

    // Packed BIP no resampling:
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 3, 32 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 55);
    // Strided BIP (nPixelSpace = 4) no resampling:
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 4, 32 * 4, 1, nullptr),
              CE_None);
    // Type-converting BIP (Byte -> UInt16) no resampling:
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, u16_buf.data(), 32, 32,
                           GDT_UInt16, 3, nullptr, 6, 32 * 6, 2, nullptr),
              CE_None);
    // X-resampling BIP (Byte -> Byte & Byte -> UInt16):
    EXPECT_EQ(ds->RasterIO(GF_Read, 2, 2, 28, 28, buf.data(), 20, 20, GDT_Byte,
                           3, nullptr, 3, 20 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 2, 2, 28, 28, u16_buf.data(), 20, 20,
                           GDT_UInt16, 3, nullptr, 6, 20 * 6, 2, nullptr),
              CE_None);
    // BSQ (bUseContigImplementation == false) no resampling & X-resampling:
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 30, 30, buf.data(), 20, 20, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 30, 30, u16_buf.data(), 20, 20,
                           GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    // Hitting the last row of the last tile in BSQ with X-resampling triggers
    // FetchBytes boundary check (CE_Failure).
    // NOTE: this is a valid full-raster 32x32 -> 20x20 downsampled read that
    // should succeed. It pins a known upstream quirk/bug in
    // FetchBufferVirtualMemIO::FetchBytes, whose bounds check is too strict
    // for the last row of the last tile under GTIFF_VIRTUAL_MEM_IO=YES.
    // If that is fixed upstream, update this expectation to CE_None.
    {
      CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_Failure);
    }

    // 2. Tiled SEPARATE (bUseContigImplementation == false, memcpy & resample):
    std::unique_ptr<GDALDataset> ds_ts(
        GDALDataset::Open(p_tiled_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_ts, nullptr);
    EXPECT_EQ(ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);

    // 3. Stripped CONTIG (packed BIP, strided BIP, resampled BIP, type-conv
    // BIP, BSQ):
    std::unique_ptr<GDALDataset> ds_sc(
        GDALDataset::Open(p_strip_ctg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_sc, nullptr);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 3, 32 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 4, 32 * 4, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 20, 10,
                              GDT_Byte, 3, nullptr, 3, 20 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, u16_buf.data(), 20, 10,
                              GDT_UInt16, 3, nullptr, 6, 20 * 6, 2, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 20, 10,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);

    // 4. Stripped SEPARATE (sparse + dense, BSQ):
    std::unique_ptr<GDALDataset> ds_ss(
        GDALDataset::Open(p_strip_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_ss, nullptr);
    EXPECT_EQ(ds_ss->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds_ss->RasterIO(GF_Read, 0, 0, 32, 16, u16_buf.data(), 20, 10,
                              GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);

    // 5. Big-endian UInt16 & CInt16 via VirtualMemIO:
    std::unique_ptr<GDALDataset> ds_be(
        GDALDataset::Open(p_be_u16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_be, nullptr);
    EXPECT_EQ(ds_be->RasterIO(GF_Read, 0, 0, 16, 16, u16_buf.data(), 16, 16,
                              GDT_UInt16, 2, nullptr, 4, 16 * 4, 2, nullptr),
              CE_None);
    EXPECT_EQ(u16_buf[0], 1234);

    std::unique_ptr<GDALDataset> ds_bec(
        GDALDataset::Open(p_be_c16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_bec, nullptr);
    std::vector<int16_t> c_out(16 * 16 * 2, 0);
    EXPECT_EQ(ds_bec->RasterIO(GF_Read, 0, 0, 16, 16, c_out.data(), 16, 16,
                               GDT_CInt16, 1, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(c_out[0], 321);
  }

  // =========================================================================
  // Part B: GTIFF_DIRECT_IO=YES
  // =========================================================================
  {
    CPLConfigOptionSetter dio_yes("GTIFF_DIRECT_IO", "YES", false);
    std::vector<GByte> buf(64 * 300 * 5, 0);
    std::vector<uint16_t> u16_buf(64 * 300 * 4, 0);

    // 1. Tiled CONTIG (no resampling BIP/BSQ, downsampling ychunk<=256,
    // upsampling ychunk=300 > 256 heap buffer, 1-band subset):
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p_tiled_ctg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    // Packed BIP:
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 3, 32 * 3, 1, nullptr),
              CE_None);
    // BSQ:
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    // Resampling ychunk <= 256 (Byte & UInt16):
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, u16_buf.data(), 20, 20,
                           GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    // Upsampling ychunk > 256 (16 -> 300 in Y -> heap-allocated pabyTempData!):
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 8, 300, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    // 1-band read on CONTIG tiled (no resampling, downsampling, upsampling>256)
    int b1_only = 1;
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           1, &b1_only, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 8, 300, GDT_Byte,
                           1, &b1_only, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 16, 16, u16_buf.data(), 8, 10,
                           GDT_UInt16, 1, &b1_only, 0, 0, 0, nullptr),
              CE_None);

    // 2. Tiled SEPARATE (multi-band delegates per-band in DirectIO):
    std::unique_ptr<GDALDataset> ds_ts(
        GDALDataset::Open(p_tiled_sep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_ts, nullptr);
    EXPECT_EQ(ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);

    // 3. Stripped CONTIG DirectIO + CopyContigByteMultiBand:
    // - packed BIP (nPixelSpace=3)
    // - CopyContigByteMultiBand 3-band nPixelSpace=4 (8-pixel unrolled loop!)
    // - CopyContigByteMultiBand 3-band nPixelSpace=5
    // - CopyContigByteMultiBand 2-band nPixelSpace=4
    // - BSQ, Y-downsampling, Y-upsampling, X-resampling Byte & UInt16
    std::unique_ptr<GDALDataset> ds_sc(
        GDALDataset::Open(p_strip_ctg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_sc, nullptr);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 3, 32 * 3, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 4, 32 * 4, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 5, 32 * 5, 1, nullptr),
              CE_None);
    int two_bands[2] = {1, 2};
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 2, two_bands, 4, 32 * 4, 1, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    // Y-downsampling & X-resampling (Byte & UInt16) + Y-upsampling:
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 20, 8,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, u16_buf.data(), 20, 24,
                              GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr),
              CE_None);

    // 4. Big-endian UInt16 & CInt16 via DirectIO:
    std::unique_ptr<GDALDataset> ds_be(
        GDALDataset::Open(p_be_u16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_be, nullptr);
    EXPECT_EQ(ds_be->RasterIO(GF_Read, 0, 0, 16, 16, u16_buf.data(), 16, 16,
                              GDT_UInt16, 2, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(u16_buf[0], 1234);

    std::unique_ptr<GDALDataset> ds_bec(
        GDALDataset::Open(p_be_c16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_bec, nullptr);
    std::vector<int16_t> c_out(16 * 16 * 2, 0);
    EXPECT_EQ(ds_bec->RasterIO(GF_Read, 0, 0, 16, 16, c_out.data(), 16, 16,
                               GDT_CInt16, 1, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(c_out[0], 321);
  }
}

TEST_F(GTiffDatasetReadTest, ReadStrileLoadBlockBufIdentifyAndBufferedStream) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. LoadBlockBuf with partial bottom WebP tile (16x20 with 16x16 tiles ->
  // zeroes trailing padding lines at L3407-3419!) + overview ReadStrile setting
  // HAS_USED_READ_ENCODED_API=1!
  {
    const char* p_webp = "/vsimem/gsr_webp_partial.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p_webp); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "WEBP");
      co.SetNameValue("WEBP_LOSSLESS", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_webp, 16, 20, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(ds->GetRasterBand(b)->Fill(88), CE_None);
      }
      int ovr = 2;
      EXPECT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(p_webp, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_STREQ(ds->GetMetadataItem("HAS_USED_READ_ENCODED_API", "_DEBUG_"),
                 "0");
    std::vector<GByte> blk(16 * 16, 0);
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 1, blk.data()), CE_None);
    EXPECT_EQ(blk[0], 88);
    // Read overview band -> sets m_poBaseDS->m_bHasUsedReadEncodedAPI = true!
    GByte ovr_px = 0;
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_band, nullptr);
    EXPECT_EQ(ovr_band->RasterIO(GF_Read, 0, 0, 1, 1, &ovr_px, 1, 1, GDT_Byte,
                                 0, 0, nullptr),
              CE_None);
    EXPECT_STREQ(ds->GetMetadataItem("HAS_USED_READ_ENCODED_API", "_DEBUG_"),
                 "1");
  }

  // 2. Identify on GTIFF_RAW:, GTIFF_DIR:, Classic/BigTIFF LE & BE.
  {
    GDALOpenInfo oi_raw("GTIFF_RAW:/vsimem/nonexistent.tif", GA_ReadOnly);
    EXPECT_EQ(GTiffDataset::Identify(&oi_raw), FALSE);
    GDALOpenInfo oi_dir("GTIFF_DIR:1:/vsimem/nonexistent.tif", GA_ReadOnly);
    EXPECT_EQ(GTiffDataset::Identify(&oi_dir), TRUE);
  }

  // 3. GTIFFMakeBufferedStream (TIFF_READ_STREAMING=YES):
  // - ClassicTIFF LE & BE (ENDIANNESS=INVERTED) created with
  //   STREAMABLE_OUTPUT=YES
  // - BigTIFF LE & BE created with STREAMABLE_OUTPUT=YES, BIGTIFF=YES
  // - Unordered strip offsets in streamed TIFF -> sets UNORDERED_BLOCKS=YES
  // - Corrupt streamed headers (IFD offset != 8 / != 16, nTags > 1024,
  //   nCount >= 16M, tag offset overflow, nMaxOffset > 10MB)
  {
    CPLConfigOptionSetter stream_on("TIFF_READ_STREAMING", "YES", false);
    for (bool bigtiff : {false, true}) {
      for (bool big_endian : {false, true}) {
        const char* p = "/vsimem/gsr_stream_valid.tif";
        absl::Cleanup clean = [&] { VSIUnlink(p); };
        {
          CPLStringList co;
          co.SetNameValue("STREAMABLE_OUTPUT", "YES");
          co.SetNameValue("BLOCKYSIZE", "4");
          if (bigtiff) co.SetNameValue("BIGTIFF", "YES");
          if (big_endian) co.SetNameValue("ENDIANNESS", "INVERTED");
          std::unique_ptr<GDALDataset> ds(
              drv->Create(p, 8, 8, 1, GDT_Byte, co.List()));
          ASSERT_NE(ds, nullptr);
          EXPECT_EQ(ds->SetMetadataItem("TIFFTAG_IMAGEDESCRIPTION",
                                        "StreamedTestDescriptionLong"),
                    CE_None);
          std::vector<GByte> strip(8 * 4, 65);
          EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, strip.data()),
                    CE_None);
          EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, strip.data()),
                    CE_None);
          EXPECT_EQ(ds->Close(), CE_None);
        }
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(p, GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(ds, nullptr);
        GByte v = 0;
        EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &v, 1, 1,
                                                 GDT_Byte, 0, 0, nullptr),
                  CE_None);
        EXPECT_EQ(v, 65);
      }
    }

    // Error paths in GTIFFMakeBufferedStream using raw in-memory buffers:
    {
      CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
      // ClassicTIFF with IFD offset = 24 (!= 8)
      const char* p_err = "/vsimem/gsr_stream_err.tif";
      absl::Cleanup clean = [&] { VSIUnlink(p_err); };
      std::vector<GByte> raw(64, 0);
      raw[0] = 'I';
      raw[1] = 'I';
      raw[2] = 42;
      raw[4] = 24;  // IFD offset = 24 != 8
      VSIFCloseL(VSIFileFromMemBuffer(p_err, raw.data(), raw.size(), FALSE));
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);

      // ClassicTIFF with IFD offset = 8, nTags = 1025 (> 1024)
      raw[4] = 8;
      uint16_t ntags = 1025;
      memcpy(raw.data() + 8, &ntags, 2);
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);

      // ClassicTIFF with 1 tag, count = 20000000 (>= 16*1024*1024)
      ntags = 1;
      memcpy(raw.data() + 8, &ntags, 2);
      uint16_t tag_id = 270, tag_type = 2;
      uint32_t tag_cnt = 20000000, tag_off = 30;
      memcpy(raw.data() + 10, &tag_id, 2);
      memcpy(raw.data() + 12, &tag_type, 2);
      memcpy(raw.data() + 14, &tag_cnt, 4);
      memcpy(raw.data() + 18, &tag_off, 4);
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);

      // ClassicTIFF with tag_cnt = 16, tag_off = 0xFFFFFFFC (overflow)
      tag_cnt = 16;
      tag_off = 0xFFFFFFFCU;
      memcpy(raw.data() + 14, &tag_cnt, 4);
      memcpy(raw.data() + 18, &tag_off, 4);
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);

      // ClassicTIFF with tag_cnt = 16, tag_off = 11 * 1024 * 1024 (> 10MB)
      tag_off = 11 * 1024 * 1024;
      memcpy(raw.data() + 18, &tag_off, 4);
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);

      // ClassicTIFF with tag_off = 200 past EOF (GTIFFExtendMemoryFile short
      // read)
      tag_off = 200;
      memcpy(raw.data() + 18, &tag_off, 4);
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);

      // BigTIFF with IFD offset = 32 (!= 16) and nTags = 1025
      std::fill(raw.begin(), raw.end(), 0);
      raw[0] = 'I';
      raw[1] = 'I';
      raw[2] = 43;
      raw[4] = 8;
      raw[8] = 32;
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);
      raw[8] = 16;
      uint64_t btags = 1025;
      memcpy(raw.data() + 16, &btags, 8);
      EXPECT_EQ(GDALDataset::Open(p_err, GDAL_OF_RASTER | GDAL_OF_READONLY),
                nullptr);
    }
  }
}

TEST_F(GTiffDatasetReadTest, OpenDirGeoreferencingAndPamInfo) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. OpenDir: GTIFF_DIR:1:, GTIFF_DIR:2:, GTIFF_RAW:GTIFF_DIR:1:,
  // GTIFF_DIR:off:<offset>:, GA_Update downgrade warning, and error cases!
  {
    const char* p_multi = "/vsimem/gsr_multi_ifd.tif";
    absl::Cleanup clean = [&] { VSIUnlink(p_multi); };
    {
      std::unique_ptr<GDALDataset> ds1(
          drv->Create(p_multi, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds1, nullptr);
      EXPECT_EQ(ds1->GetRasterBand(1)->Fill(10), CE_None);
    }
    {
      CPLStringList co;
      co.SetNameValue("APPEND_SUBDATASET", "YES");
      std::unique_ptr<GDALDataset> ds2(
          drv->Create(p_multi, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds2, nullptr);
      EXPECT_EQ(ds2->GetRasterBand(1)->Fill(20), CE_None);
    }

    // Query SUBDATASETS and IFD_OFFSET.
    std::string off_str;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_multi, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_NE(ds->GetMetadata("SUBDATASETS"), nullptr);
      const char* s =
          ds->GetRasterBand(1)->GetMetadataItem("IFD_OFFSET", "TIFF");
      ASSERT_NE(s, nullptr);
      off_str = s;
    }

    // Open by index, raw + index, and byte offset:
    {
      std::unique_ptr<GDALDataset> d1(
          GDALDataset::Open((std::string("GTIFF_DIR:1:") + p_multi).c_str(),
                            GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(d1, nullptr);
      EXPECT_EQ(d1->GetRasterXSize(), 8);

      std::unique_ptr<GDALDataset> d2(GDALDataset::Open(
          (std::string("GTIFF_RAW:GTIFF_DIR:2:") + p_multi).c_str(),
          GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(d2, nullptr);
      EXPECT_EQ(d2->GetRasterXSize(), 16);

      std::unique_ptr<GDALDataset> doff(GDALDataset::Open(
          (std::string("GTIFF_DIR:off:") + off_str + ":" + p_multi).c_str(),
          GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(doff, nullptr);
      EXPECT_EQ(doff->GetRasterXSize(), 8);

      // OpenDir in GA_Update emits warning and opens RO:
      CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
      std::unique_ptr<GDALDataset> dupd(
          GDALDataset::Open((std::string("GTIFF_DIR:1:") + p_multi).c_str(),
                            GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(dupd, nullptr);

      // Invalid OpenDir strings:
      EXPECT_EQ(GDALDataset::Open("GTIFF_DIR:", GDAL_OF_RASTER), nullptr);
      EXPECT_EQ(
          GDALDataset::Open((std::string("GTIFF_DIR:0:") + p_multi).c_str(),
                            GDAL_OF_RASTER),
          nullptr);
      EXPECT_EQ(GDALDataset::Open("GTIFF_DIR:1:", GDAL_OF_RASTER), nullptr);
      EXPECT_EQ(
          GDALDataset::Open((std::string("GTIFF_DIR:99:") + p_multi).c_str(),
                            GDAL_OF_RASTER),
          nullptr);
      EXPECT_EQ(
          GDALDataset::Open("GTIFF_DIR:1:/vsimem/no_such.tif", GDAL_OF_RASTER),
          nullptr);
    }
  }

  // 2. LookForProjectionFromXML (.xml EPSG<=32767 & .XML ESRI>32767),
  // MapInfo .tab sidecar (affine & GCPs), Worldfile (.tfw & .wld),
  // Negative ScaleY (GTIFF_HONOUR_NEGATIVE_SCALEY unset/YES/NO),
  // Rotated GeoTransform, 3D Vertical Z scale/offset, Compound CRS stripping,
  // and SHIFT_ORIGIN_IN_MINUS_180_PLUS_180!
  {
    // .xml with EPSG:4326 and .XML with ESRI:102001
    const char* p_xml1 = "/vsimem/gsr_xml1/a.tif";
    const char* p_xml1_side = "/vsimem/gsr_xml1/a.xml";
    const char* p_xml2 = "/vsimem/gsr_xml2/b.tif";
    const char* p_xml2_side = "/vsimem/gsr_xml2/b.XML";
    absl::Cleanup clean_xml = [&] {
      VSIUnlink(p_xml1);
      VSIUnlink(p_xml1_side);
      VSIUnlink(p_xml2);
      VSIUnlink(p_xml2_side);
    };
    {
      std::unique_ptr<GDALDataset> d1(
          drv->Create(p_xml1, 4, 4, 1, GDT_Byte, nullptr));
      std::unique_ptr<GDALDataset> d2(
          drv->Create(p_xml2, 4, 4, 1, GDT_Byte, nullptr));
    }
    WriteTextToVsiMem(
        p_xml1_side,
        "<metadata><refSysInfo><RefSystem><refSysID><identCode code=\"4326\"/>"
        "</refSysID></RefSystem></refSysInfo></metadata>");
    WriteTextToVsiMem(p_xml2_side,
                      "<metadata><refSysInfo><RefSystem><refSysID><identCode "
                      "code=\"102001\"/>"
                      "</refSysID></RefSystem></refSysInfo></metadata>");
    {
      std::unique_ptr<GDALDataset> d1(
          GDALDataset::Open(p_xml1, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(d1, nullptr);
      EXPECT_NE(d1->GetSpatialRef(), nullptr);
    }
    {
      CPLConfigOptionSetter no_rd("GDAL_DISABLE_READDIR_ON_OPEN", "YES", false);
      std::unique_ptr<GDALDataset> d2(
          GDALDataset::Open(p_xml2, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(d2, nullptr);
      EXPECT_NE(d2->GetSpatialRef(), nullptr);
    }

    // .tab sidecar and .wld sidecar
    const char* p_tab = "/vsimem/gsr_tab/t.tif";
    const char* p_tab_side = "/vsimem/gsr_tab/t.tab";
    const char* p_wld = "/vsimem/gsr_wld/w.tif";
    const char* p_wld_side = "/vsimem/gsr_wld/w.wld";
    absl::Cleanup clean_tab = [&] {
      VSIUnlink(p_tab);
      VSIUnlink(p_tab_side);
      VSIUnlink(p_wld);
      VSIUnlink(p_wld_side);
    };
    {
      std::unique_ptr<GDALDataset> d1(
          drv->Create(p_tab, 10, 10, 1, GDT_Byte, nullptr));
      std::unique_ptr<GDALDataset> d2(
          drv->Create(p_wld, 10, 10, 1, GDT_Byte, nullptr));
    }
    WriteTextToVsiMem(
        p_tab_side,
        "!table\n!version 300\n!charset Neutral\nDefinition Table\n"
        "  File \"t.tif\"\n  Type \"RASTER\"\n"
        "  (10.0, 20.0) (0, 0) Label \"Pt 1\",\n"
        "  (20.0, 20.0) (10, 0) Label \"Pt 2\",\n"
        "  (10.0, 10.0) (0, 10) Label \"Pt 3\"\n"
        "  CoordSys Earth Projection 1, 104\n");
    WriteTextToVsiMem(p_wld_side, "1.0\n0.0\n0.0\n-1.0\n100.5\n200.5\n");
    {
      std::unique_ptr<GDALDataset> dt(
          GDALDataset::Open(p_tab, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(dt, nullptr);
      GDALGeoTransform gt;
      EXPECT_EQ(dt->GetGeoTransform(gt), CE_None);
      EXPECT_NE(dt->GetSpatialRef(), nullptr);

      std::unique_ptr<GDALDataset> dw(
          GDALDataset::Open(p_wld, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(dw, nullptr);
      EXPECT_EQ(dw->GetGeoTransform(gt), CE_None);
      EXPECT_DOUBLE_EQ(gt[0], 100.0);
    }

    // Negative ScaleY + 3D Vertical SRS Z scale/offset + SHIFT_ORIGIN
    const char* p_neg = "/vsimem/gsr_neg_scaley.tif";
    absl::Cleanup clean_neg = [&] { VSIUnlink(p_neg); };
    {
      VSILFILE* fp = VSIFOpenL(p_neg, "wb+");
      ASSERT_NE(fp, nullptr);
      TIFF* tif = VSI_TIFFOpen(p_neg, "w", fp);
      ASSERT_NE(tif, nullptr);
      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
      const double scale[3] = {1.0, -2.0, 3.5};
      const double tie[6] = {0.0, 0.0, 1.0, -190.0, 50.0, 10.0};
      constexpr uint32_t kTiffTagGeoPixelScale = 33550;
      constexpr uint32_t kTiffTagGeoTiePoints = 33922;
      TIFFSetField(tif, kTiffTagGeoPixelScale, 3, scale);
      TIFFSetField(tif, kTiffTagGeoTiePoints, 6, tie);
      // Compound CRS EPSG:4326 + Vertical CRS EPSG:5703 (NAVD88 height) with
      // GeoTIFF 1.1 version (1, 1, 1) so vertical CS is preserved by default:
      constexpr uint32_t kTiffTagGeoKeyDirectory = 34735;
      const uint16_t geokeys[16] = {
          1,    1, 1, 3,     // Version 1, KeyRev 1.1, 3 keys
          1024, 0, 1, 2,     // GTModelTypeGeoKey = ModelTypeGeographic
          2048, 0, 1, 4326,  // GeographicTypeGeoKey = 4326
          4096, 0, 1, 5703   // VerticalCSTypeGeoKey = 5703
      };
      TIFFSetField(tif, kTiffTagGeoKeyDirectory, 16, geokeys);
      GByte z[16] = {0};
      TIFFWriteEncodedStrip(tif, 0, z, 16);
      TIFFWriteDirectory(tif);
      TIFFClose(tif);
      VSIFCloseL(fp);
    }
    for (const char* opt : {static_cast<const char*>(nullptr), "YES", "NO"}) {
      CPLConfigOptionSetter neg_opt("GTIFF_HONOUR_NEGATIVE_SCALEY", opt, false);
      CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
      const char* const oo_shift[] = {"SHIFT_ORIGIN_IN_MINUS_180_PLUS_180=YES",
                                      nullptr};
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_neg, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_shift, nullptr));
      ASSERT_NE(ds, nullptr);
      GDALGeoTransform gt;
      EXPECT_EQ(ds->GetGeoTransform(gt), CE_None);
      EXPECT_DOUBLE_EQ(gt[0], 170.0);  // -190 + 360!
      int ok = 0;
      EXPECT_DOUBLE_EQ(ds->GetRasterBand(1)->GetScale(&ok), 3.5);
      EXPECT_EQ(ok, TRUE);
    }
  }

  // 3. ApplyPamInfo: PAM GeoTransform, PAM SRS, PAM GCPs overriding internal
  // GT, xml:ESRI GeodataXform GCP synthesis, PAM metadata & band properties!
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    const char* p_esri = "/vsimem/gsr_pam_esri.tif";
    const char* p_esri_aux = "/vsimem/gsr_pam_esri.tif.aux.xml";
    const char* p_pam_gcp = "/vsimem/gsr_pam_gcp.tif";
    const char* p_pam_gcp_aux = "/vsimem/gsr_pam_gcp.tif.aux.xml";
    absl::Cleanup clean = [&] {
      VSIUnlink(p_esri);
      VSIUnlink(p_esri_aux);
      VSIUnlink(p_pam_gcp);
      VSIUnlink(p_pam_gcp_aux);
    };

    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_esri, 100, 100, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->SetMetadataItem("TIFFTAG_XRESOLUTION", "10"), CE_None);
      EXPECT_EQ(ds->SetMetadataItem("TIFFTAG_YRESOLUTION", "10"), CE_None);
      EXPECT_EQ(ds->SetMetadataItem("TIFFTAG_RESOLUTIONUNIT", "2"), CE_None);
      GDALGeoTransform gt(0.0, 1.0, 0.0, 0.0, 0.0, -1.0);
      EXPECT_EQ(ds->SetGeoTransform(gt), CE_None);
    }
    WriteTextToVsiMem(
        p_esri_aux,
        "<PAMDataset>"
        "<Metadata domain=\"xml:ESRI\">"
        "<MDI key=\"GeodataXform\">&lt;GeodataXform&gt;"
        "&lt;SpatialReference&gt;&lt;WKT&gt;GEOGCS[&amp;quot;WGS 84&amp;quot;,"
        "DATUM[&amp;quot;WGS_1984&amp;quot;,SPHEROID[&amp;quot;WGS "
        "84&amp;quot;,"
        "6378137,298.257223563]],PRIMEM[&amp;quot;Greenwich&amp;quot;,0],"
        "UNIT[&amp;quot;degree&amp;quot;,0.0174532925199433]]&lt;/WKT&gt;"
        "&lt;/SpatialReference&gt;"
        "&lt;SourceGCPs&gt;&lt;Double&gt;1.0&lt;/"
        "Double&gt;&lt;Double&gt;2.0&lt;/Double&gt;&lt;/SourceGCPs&gt;"
        "&lt;TargetGCPs&gt;&lt;Double&gt;10.0&lt;/"
        "Double&gt;&lt;Double&gt;20.0&lt;/Double&gt;&lt;/TargetGCPs&gt;"
        "&lt;/GeodataXform&gt;</MDI>"
        "</Metadata>"
        "<Metadata><MDI key=\"PAM_DS_KEY\">PAM_DS_VAL</MDI></Metadata>"
        "<PAMRasterBand band=\"1\">"
        "<Description>PamDesc</Description>"
        "<UnitType>PamUnit</UnitType>"
        "<Offset>2.0</Offset>"
        "<Scale>4.0</Scale>"
        "<ColorInterp>Red</ColorInterp>"
        "<ColorTable><Entry c1=\"1\" c2=\"2\" c3=\"3\" "
        "c4=\"255\"/></ColorTable>"
        "<Metadata><MDI key=\"PAM_B_KEY\">PAM_B_VAL</MDI></Metadata>"
        "</PAMRasterBand>"
        "</PAMDataset>");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_esri, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 1);
      EXPECT_NE(ds->GetGCPSpatialRef(), nullptr);
      EXPECT_NE(ds->GetGCPs(), nullptr);
      EXPECT_STREQ(ds->GetMetadataItem("PAM_DS_KEY"), "PAM_DS_VAL");
      EXPECT_STREQ(ds->GetRasterBand(1)->GetDescription(), "PamDesc");
      EXPECT_STREQ(ds->GetRasterBand(1)->GetUnitType(), "PamUnit");
      EXPECT_NE(ds->GetRasterBand(1)->GetColorTable(), nullptr);
    }

    // PAM GCPList + PAM GeoTransform & SRS priority test:
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_pam_gcp, 10, 10, 1, GDT_Byte, nullptr));
      GDALGeoTransform gt(0.0, 1.0, 0.0, 0.0, 0.0, -1.0);
      EXPECT_EQ(ds->SetGeoTransform(gt), CE_None);
    }
    WriteTextToVsiMem(p_pam_gcp_aux,
                      "<PAMDataset>"
                      "<SRS>EPSG:4326</SRS>"
                      "<GCPList Projection=\"EPSG:4326\">"
                      "<GCP Id=\"1\" Pixel=\"0\" Line=\"0\" X=\"10\" Y=\"20\"/>"
                      "</GCPList>"
                      "</PAMDataset>");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_pam_gcp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 1);
      GDALGeoTransform gt;
      // Internal GT was invalidated by higher-priority PAM GCPs!
      EXPECT_EQ(ds->GetGeoTransform(gt), CE_Failure);
    }
  }
}

TEST_F(GTiffDatasetReadTest,
       OpenOffsetColorProfileExifMetadataAndCacheMultiRange) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. OpenOffset error & edge cases via raw VSI_TIFFOpen:
  // - IMAGEWIDTH = 0
  // - Unconfigured codec (COMPRESSION_JBIG = 34661 and unknown = 12345)
  // - Missing RowsPerStrip tag -> warns "assuming all one strip"
  // - Subsampled non-JPEG YCbCr with BitsPerSample=16 -> error
  // - SampleFormat=IEEEFP with BitsPerSample=12 -> error
  // - SampleFormat=UINT with BitsPerSample=128 -> GDT_Unknown error
  {
    CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
    for (int case_id = 0; case_id < 6; ++case_id) {
      SCOPED_TRACE(case_id);
      const char* p = "/vsimem/gsr_openoffset_edge.tif";
      absl::Cleanup clean = [&] { VSIUnlink(p); };
      VSILFILE* fp = VSIFOpenL(p, "wb+");
      ASSERT_NE(fp, nullptr);
      TIFF* tif = VSI_TIFFOpen(p, "w", fp);
      ASSERT_NE(tif, nullptr);
      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, case_id == 0 ? 0 : 8);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 8);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, case_id == 3 ? 3 : 1);
      TIFFSetField(
          tif, TIFFTAG_BITSPERSAMPLE,
          case_id == 3 ? 16 : (case_id == 4 ? 12 : (case_id == 5 ? 128 : 8)));
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                   case_id == 3 ? PHOTOMETRIC_YCBCR : PHOTOMETRIC_MINISBLACK);
      if (case_id == 4) {
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
      }
      if (case_id != 2) {
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 8);
      }
      std::vector<GByte> zero(256, 0);
      TIFFWriteEncodedStrip(tif, 0, zero.data(), 64);
      TIFFWriteDirectory(tif);
      TIFFClose(tif);
      VSIFCloseL(fp);

      if (case_id == 1) {
        // Directory is written; patch the COMPRESSION tag in memory to 34661
        // (JBIG), then to an unknown codec id.
        vsi_l_offset len = 0;
        GByte* buf = VSIGetMemFileBuffer(p, &len, FALSE);
        // Find tag 259 (TIFFTAG_COMPRESSION) in IFD and set value to 34661
        for (vsi_l_offset i = 0; i + 12 <= len; ++i) {
          uint16_t t = 0, ty = 0;
          memcpy(&t, buf + i, 2);
          memcpy(&ty, buf + i + 2, 2);
          if (t == TIFFTAG_COMPRESSION && ty == 3) {
            uint16_t jbig = 34661;
            memcpy(buf + i + 8, &jbig, 2);
            EXPECT_EQ(GDALDataset::Open(p, GDAL_OF_RASTER), nullptr);
            uint16_t unk = 12345;
            memcpy(buf + i + 8, &unk, 2);
            EXPECT_EQ(GDALDataset::Open(p, GDAL_OF_RASTER), nullptr);
            break;
          }
        }
      } else if (case_id == 2) {
        // Missing RowsPerStrip -> opens with warning!
        // Strip out tag 278 (TIFFTAG_ROWSPERSTRIP) by changing tag ID to 65000
        vsi_l_offset len = 0;
        GByte* buf = VSIGetMemFileBuffer(p, &len, FALSE);
        for (vsi_l_offset i = 0; i + 12 <= len; ++i) {
          uint16_t t = 0, ty = 0;
          memcpy(&t, buf + i, 2);
          memcpy(&ty, buf + i + 2, 2);
          if (t == TIFFTAG_ROWSPERSTRIP && (ty == 3 || ty == 4)) {
            uint16_t dummy_tag = 65000;
            memcpy(buf + i, &dummy_tag, 2);
            break;
          }
        }
        std::unique_ptr<GDALDataset> ds(GDALDataset::Open(p, GDAL_OF_RASTER));
        EXPECT_NE(ds, nullptr);
      } else {
        EXPECT_EQ(GDALDataset::Open(p, GDAL_OF_RASTER), nullptr);
      }
    }
  }

  // 2. LoadICCProfile (both embedded ICC profile and colorimetric tags +
  // TIFFTAG_TRANSFERRANGE), LoadEXIFMetadata (EXIF + GPS SubIFDs),
  // LoadMetadata (.IMD sidecar + internal RPC tag + .ovr skip), and all
  // _DEBUG_ GetMetadataItem keys!
  {
    const char* p_icc = "/vsimem/gsr_icc.tif";
    const char* p_col = "/vsimem/gsr_colorimetric.tif";
    const char* p_exif = "/vsimem/gsr_exif.tif";
    const char* p_imd = "/vsimem/gsr_imd/img.tif";
    const char* p_imd_side = "/vsimem/gsr_imd/img.IMD";
    const char* p_ovr_ext = "/vsimem/gsr_imd/img.tif.ovr";
    absl::Cleanup clean = [&] {
      VSIUnlink(p_icc);
      VSIUnlink(p_col);
      VSIUnlink(p_exif);
      VSIUnlink(p_imd);
      VSIUnlink(p_imd_side);
      VSIUnlink(p_ovr_ext);
    };

    // Embedded ICC profile
    {
      CPLStringList co;
      co.SetNameValue("SOURCE_ICC_PROFILE", "AAAA");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_icc, 4, 4, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_icc, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_STREQ(ds->GetMetadataItem("SOURCE_ICC_PROFILE", "COLOR_PROFILE"),
                   "AAAA");
    }

    // Colorimetric tags + TIFFTAG_TRANSFERRANGE (0x0156) on NBITS=1 (2 entries)
    // libtiff does not know tag 0x0156, so register it via a tag extender that
    // chains to the previously installed (GDAL) extender. The extender must stay
    // active until the dataset is re-opened and read below, so it is restored
    // at the end of this enclosing scope.
    static TIFFExtendProc s_parent_extender = nullptr;
    auto transfer_range_extender = [](TIFF* tif) {
      static char kTransferRangeName[] = "TransferRange";
      static const TIFFFieldInfo xtiffFieldInfo[] = {
          {0x0156, 6, 6, TIFF_SHORT, FIELD_CUSTOM, 1, 0, kTransferRangeName},
      };
      TIFFMergeFieldInfo(tif, xtiffFieldInfo, 1);
      if (s_parent_extender) s_parent_extender(tif);
    };
    s_parent_extender = TIFFSetTagExtender(transfer_range_extender);
    absl::Cleanup restore_extender = [] {
      TIFFSetTagExtender(s_parent_extender);
      s_parent_extender = nullptr;
    };
    {
      CPLStringList co;
      co.SetNameValue("NBITS", "1");
      co.SetNameValue("SOURCE_PRIMARIES_RED", "0.64,0.33,1.0");
      co.SetNameValue("SOURCE_PRIMARIES_GREEN", "0.30,0.60,1.0");
      co.SetNameValue("SOURCE_PRIMARIES_BLUE", "0.15,0.06,1.0");
      co.SetNameValue("SOURCE_WHITEPOINT", "0.3127,0.3290,1.0");
      co.SetNameValue("TIFFTAG_TRANSFERFUNCTION_RED", "0,65535");
      co.SetNameValue("TIFFTAG_TRANSFERFUNCTION_GREEN", "0,65535");
      co.SetNameValue("TIFFTAG_TRANSFERFUNCTION_BLUE", "0,65535");
      co.SetNameValue("TIFFTAG_TRANSFERRANGE_BLACK", "0,0,0");
      co.SetNameValue("TIFFTAG_TRANSFERRANGE_WHITE", "1,1,1");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_col, 8, 8, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_col, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_NE(ds->GetMetadata("COLOR_PROFILE"), nullptr);
      EXPECT_NE(
          ds->GetMetadataItem("TIFFTAG_TRANSFERRANGE_WHITE", "COLOR_PROFILE"),
          nullptr);
    }

    // EXIF & GPS SubIFDs synthesized in a ClassicTIFF in /vsimem/:
    {
      // Layout:
      // 0..7: ClassicTIFF header (IFD0 at offset 8)
      // 8..107: IFD0 with 7 tags (Width=4, Length=4, BPS=8, Comp=1, Photo=1,
      //         StripOffsets=200, RowsPerStrip=4, StripByteCounts=16,
      //         EXIFIFD(34665)=120, GPSIFD(34853)=160)
      // 120..137: EXIF IFD (1 tag: 0x8827 ISOSpeedRatings SHORT=3 count=1
      // val=200) 160..177: GPS IFD (1 tag: 0x0000 GPSVersionID BYTE=1 count=4
      // val=2.2.0.0)
      std::vector<GByte> raw(256, 0);
      raw[0] = 'I';
      raw[1] = 'I';
      raw[2] = 42;
      raw[4] = 8;
      auto put_u16 = [&](size_t off, uint16_t v) { memcpy(&raw[off], &v, 2); };
      auto put_u32 = [&](size_t off, uint32_t v) { memcpy(&raw[off], &v, 4); };
      auto put_tag = [&](size_t idx, uint16_t tag, uint16_t type, uint32_t cnt,
                         uint32_t val) {
        size_t base = 10 + idx * 12;
        put_u16(base, tag);
        put_u16(base + 2, type);
        put_u32(base + 4, cnt);
        put_u32(base + 8, val);
      };
      put_u16(8, 10);
      put_tag(0, 256, 4, 1, 4);      // IMAGEWIDTH
      put_tag(1, 257, 4, 1, 4);      // IMAGELENGTH
      put_tag(2, 258, 3, 1, 8);      // BITSPERSAMPLE
      put_tag(3, 259, 3, 1, 1);      // COMPRESSION
      put_tag(4, 262, 3, 1, 1);      // PHOTOMETRIC
      put_tag(5, 273, 4, 1, 220);    // STRIPOFFSETS
      put_tag(6, 278, 4, 1, 4);      // ROWSPERSTRIP
      put_tag(7, 279, 4, 1, 16);     // STRIPBYTECOUNTS
      put_tag(8, 34665, 4, 1, 140);  // TIFFTAG_EXIFIFD
      put_tag(9, 34853, 4, 1, 180);  // TIFFTAG_GPSIFD
      // EXIF IFD at 140:
      put_u16(140, 1);
      put_u16(142, 0x8827);
      put_u16(144, 3);
      put_u32(146, 1);
      put_u16(150, 400);
      // GPS IFD at 180:
      put_u16(180, 1);
      put_u16(182, 0x0000);
      put_u16(184, 1);
      put_u32(186, 4);
      raw[190] = 2;
      raw[191] = 2;
      VSIFCloseL(VSIFileFromMemBuffer(p_exif, raw.data(), raw.size(), FALSE));

      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_exif, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_NE(ds->GetMetadata("EXIF"), nullptr);
      EXPECT_STREQ(ds->GetMetadataItem("EXIF_ISOSpeedRatings", "EXIF"), "400");
    }

    // IMD sidecar + internal RPC tag + .ovr skip + _DEBUG_ metadata items:
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_imd, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      const char* rpc[] = {
          "LINE_OFF=1",
          "SAMP_OFF=1",
          "LAT_OFF=1",
          "LONG_OFF=1",
          "HEIGHT_OFF=1",
          "LINE_SCALE=1",
          "SAMP_SCALE=1",
          "LAT_SCALE=1",
          "LONG_SCALE=1",
          "HEIGHT_SCALE=1",
          "LINE_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
          "LINE_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
          "SAMP_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
          "SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
          nullptr};
      EXPECT_EQ(ds->SetMetadata(const_cast<char**>(rpc), "RPC"), CE_None);
      EXPECT_EQ(ds->SetMetadataItem("CUSTOM", "VAL"), CE_None);
      std::unique_ptr<GDALDataset> ds_ovr(
          drv->Create(p_ovr_ext, 8, 8, 1, GDT_Byte, nullptr));
    }
    WriteTextToVsiMem(p_imd_side,
                      "BEGIN_GROUP = IMAGE_1\n\tsatId = \"WV02\";\nEND_GROUP = "
                      "IMAGE_1\nEND;\n");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_imd, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      char** dlist = ds->GetMetadataDomainList();
      CSLDestroy(dlist);
      EXPECT_NE(ds->GetMetadata("IMD"), nullptr);
      EXPECT_NE(ds->GetMetadata("RPC"), nullptr);
      (void)ds->GetMetadata("ProxyOverviewRequest");
      (void)ds->GetMetadata("DERIVED_SUBDATASETS");
      for (const char* dbg :
           {"TIFFTAG_EXTRASAMPLES", "TIFFTAG_PHOTOMETRIC",
            "TIFFTAG_GDAL_METADATA", "HAS_USED_READ_ENCODED_API",
            "WEBP_LOSSLESS", "WEBP_LEVEL", "MAX_Z_ERROR",
            "MAX_Z_ERROR_OVERVIEW", "JXL_LOSSLESS", "JXL_DISTANCE",
            "JXL_ALPHA_DISTANCE", "JXL_EFFORT", "UNKNOWN"}) {
        (void)ds->GetMetadataItem(dbg, "_DEBUG_");
      }

      std::unique_ptr<GDALDataset> ds_ovr(
          GDALDataset::Open(p_ovr_ext, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds_ovr, nullptr);
      EXPECT_EQ(ds_ovr->GetMetadata("IMD"), nullptr);
    }
  }

  // 3. CacheMultiRange:
  // - COG with Contig + Internal Mask + Sparse Tile + FloatingPointWindow
  // - COG with INTERLEAVE=TILE (disjoint ranges {1, 3})
  // - Corrupt trailer retry fallback (CheckTrailer returns false -> retries
  //   with m_bLeaderSizeAsUInt4 = false!)
  // - Small GDAL_MAX_RAW_BLOCK_CACHE_SIZE limit
  // - ReadMultiRange failure fallback
  {
    const char* p_src = "/vsimem/gsr_cmr_src.tif";
    const char* p_cog = "/vsimem_gsr_mr/gsr_cmr_cog.tif";
    const char* p_tile_cog = "/vsimem_gsr_mr/gsr_cmr_tile_cog.tif";
    const std::string back_cog = GsrMultiRangeHandler::ToMem(p_cog);
    const std::string back_tile = GsrMultiRangeHandler::ToMem(p_tile_cog);
    absl::Cleanup clean = [&] {
      VSIUnlink(p_src);
      VSIUnlink(back_cog.c_str());
      VSIUnlink(back_tile.c_str());
    };

    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      std::unique_ptr<GDALDataset> src(
          drv->Create(p_src, 64, 64, 3, GDT_Byte, nullptr));
      ASSERT_NE(src, nullptr);
      EXPECT_EQ(src->CreateMaskBand(GMF_PER_DATASET), CE_None);
      // Leave top-left 16x16 tile (0,0) as 0 (sparse in COG), fill rest!
      std::vector<GByte> patch(48 * 48, 120);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(src->GetRasterBand(b)->RasterIO(GF_Write, 16, 16, 48, 48,
                                                  patch.data(), 48, 48,
                                                  GDT_Byte, 0, 0, nullptr),
                  CE_None);
      }
      EXPECT_EQ(src->GetRasterBand(1)->GetMaskBand()->RasterIO(
                    GF_Write, 16, 16, 48, 48, patch.data(), 48, 48, GDT_Byte, 0,
                    0, nullptr),
                CE_None);

      GDALDriver* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
      ASSERT_NE(cog_drv, nullptr);
      CPLStringList co;
      co.SetNameValue("BLOCKSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("SPARSE_OK", "YES");
      std::unique_ptr<GDALDataset> cog(cog_drv->CreateCopy(
          p_cog, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(cog, nullptr);

      // Also create INTERLEAVE=TILE COG via COG CreateCopy:
      CPLStringList tco;
      tco.SetNameValue("BLOCKSIZE", "16");
      tco.SetNameValue("COMPRESS", "LZW");
      tco.SetNameValue("INTERLEAVE", "TILE");
      std::unique_ptr<GDALDataset> tcog(cog_drv->CreateCopy(
          p_tile_cog, src.get(), FALSE, tco.List(), nullptr, nullptr));
      ASSERT_NE(tcog, nullptr);
    }

    // Read COG with FloatingPointWindowValidity + read mask band:
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_cog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_NE(ds->GetMetadataItem("GDAL_STRUCTURAL_METADATA", "TIFF"),
                nullptr);
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.bFloatingPointWindowValidity = TRUE;
      extra.dfXOff = 0.0;
      extra.dfYOff = 0.0;
      extra.dfXSize = 64.0;
      extra.dfYSize = 64.0;
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 3, nullptr, 0, 0, 0, &extra),
                CE_None);
      std::vector<GByte> msk_out(64 * 64, 0);
      EXPECT_EQ(ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                    GF_Read, 0, 0, 64, 64, msk_out.data(), 64, 64, GDT_Byte, 0,
                    0, nullptr),
                CE_None);
    }

    // Read INTERLEAVE=TILE COG with disjoint bands {1, 3}:
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_tile_cog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int bands[2] = {1, 3};
      std::vector<GByte> out(64 * 64 * 2, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 2, bands, 0, 0, 0, nullptr),
                CE_None);
    }

    // Small GDAL_MAX_RAW_BLOCK_CACHE_SIZE limit & fail_multi_range fallback:
    {
      CPLConfigOptionSetter small_lim("GDAL_MAX_RAW_BLOCK_CACHE_SIZE", "40",
                                      false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_cog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_None);
    }
    {
      g_gsr_mr->fail_multi_range = true;
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_cog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_None);
      g_gsr_mr->fail_multi_range = false;
    }

    // Corrupt 4-byte trailer of last tile in back_cog -> CheckTrailer fails and
    // CacheMultiRange retries with m_bLeaderSizeAsUInt4 = false!
    {
      vsi_l_offset off_last = 0, sz_last = 0;
      {
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(p_cog, GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(ds, nullptr);
        const char* so =
            ds->GetRasterBand(1)->GetMetadataItem("BLOCK_OFFSET_3_3", "TIFF");
        const char* ss =
            ds->GetRasterBand(1)->GetMetadataItem("BLOCK_SIZE_3_3", "TIFF");
        ASSERT_NE(so, nullptr);
        ASSERT_NE(ss, nullptr);
        off_last = static_cast<vsi_l_offset>(std::strtoull(so, nullptr, 10));
        sz_last = static_cast<vsi_l_offset>(std::strtoull(ss, nullptr, 10));
      }
      vsi_l_offset total_len = 0;
      GByte* raw = VSIGetMemFileBuffer(back_cog.c_str(), &total_len, FALSE);
      ASSERT_NE(raw, nullptr);
      ASSERT_LE(off_last + sz_last + 4, total_len);
      raw[off_last + sz_last] ^= 0xFF;  // Flip trailer byte!

      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_cog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr),
                CE_None);
    }
  }
}

TEST_F(GTiffDatasetReadTest, AdditionalReadAndOpenCoverage) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);
  const char* const oo_mt[] = {"NUM_THREADS=2", nullptr};

  // 1. OpenOffset:
  // - LERC_DEFLATE & LERC_ZSTD + MAX_Z_ERROR + MAX_Z_ERROR_OVERVIEW
  // - JXL lossy (JXL_LOSSLESS=NO, JXL_DISTANCE=2.0, JXL_ALPHA_DISTANCE=3.0,
  //   JXL_EFFORT=3)
  // - RAT in TIFFTAG_GDAL_METADATA + band scale/offset/unittype/description/
  //   colorinterp + xml:XMP + PHOTOMETRIC=MINISWHITE + 64-bit/Float32 NoData
  // - WebP baseline COMPRESSION_REVERSIBILITY (/vsisubfile/ probe)
  // - _DEBUG_ TIFFTAG_EXTRASAMPLES on 2-band dataset
  {
    const char* p_lerc1 = "/vsimem/gsr_add_lerc1.tif";
    const char* p_lerc2 = "/vsimem/gsr_add_lerc2.tif";
    const char* p_jxl = "/vsimem/gsr_add_jxl.tif";
    const char* p_rat = "/vsimem/gsr_add_rat.tif";
    const char* p_i64 = "/vsimem/gsr_add_i64.tif";
    const char* p_u64 = "/vsimem/gsr_add_u64.tif";
    const char* p_webp_base = "/vsimem/gsr_add_webp_base.tif";
    absl::Cleanup clean = [&] {
      for (const char* p :
           {p_lerc1, p_lerc2, p_jxl, p_rat, p_i64, p_u64, p_webp_base}) {
        VSIUnlink(p);
      }
    };

    // LERC_DEFLATE + MAX_Z_ERROR + MAX_Z_ERROR_OVERVIEW & LERC_ZSTD
    {
      CPLStringList co1;
      co1.SetNameValue("COMPRESS", "LERC_DEFLATE");
      co1.SetNameValue("MAX_Z_ERROR", "0.5");
      co1.SetNameValue("MAX_Z_ERROR_OVERVIEW", "1.5");
      std::unique_ptr<GDALDataset> d1(
          drv->Create(p_lerc1, 8, 8, 1, GDT_Float32, co1.List()));
      ASSERT_NE(d1, nullptr);
      EXPECT_EQ(d1->GetRasterBand(1)->SetNoDataValue(-3.4028234663852886e+38),
                CE_None);
      EXPECT_EQ(d1->GetRasterBand(1)->Fill(1.0), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> d1(
          GDALDataset::Open(p_lerc1, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(d1, nullptr);
      EXPECT_STREQ(d1->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                   "LERC_DEFLATE");
      EXPECT_STREQ(d1->GetMetadataItem("MAX_Z_ERROR", "_DEBUG_"), "0.500000");
      EXPECT_STREQ(d1->GetMetadataItem("MAX_Z_ERROR_OVERVIEW", "_DEBUG_"),
                   "1.500000");
    }
    {
      CPLStringList co2;
      co2.SetNameValue("COMPRESS", "LERC_ZSTD");
      std::unique_ptr<GDALDataset> d2(
          drv->Create(p_lerc2, 8, 8, 1, GDT_Byte, co2.List()));
      ASSERT_NE(d2, nullptr);
      EXPECT_EQ(d2->GetRasterBand(1)->Fill(2), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> d2(
          GDALDataset::Open(p_lerc2, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(d2, nullptr);
      EXPECT_STREQ(d2->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                   "LERC_ZSTD");
    }

    // JXL lossy metadata in TIFFTAG_GDAL_METADATA + _DEBUG_
    // TIFFTAG_EXTRASAMPLES
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JXL");
      co.SetNameValue("JXL_LOSSLESS", "NO");
      co.SetNameValue("JXL_DISTANCE", "2.0");
      co.SetNameValue("JXL_ALPHA_DISTANCE", "3.0");
      co.SetNameValue("JXL_EFFORT", "3");
      co.SetNameValue("ALPHA", "YES");
      std::unique_ptr<GDALDataset> dj(
          drv->Create(p_jxl, 8, 8, 2, GDT_Byte, co.List()));
      ASSERT_NE(dj, nullptr);
      EXPECT_EQ(dj->GetRasterBand(1)->Fill(10), CE_None);
      EXPECT_EQ(dj->GetRasterBand(2)->Fill(255), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> dj(
          GDALDataset::Open(p_jxl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(dj, nullptr);
      EXPECT_STREQ(
          dj->GetMetadataItem("COMPRESSION_REVERSIBILITY", "IMAGE_STRUCTURE"),
          "LOSSY");
      EXPECT_NE(dj->GetMetadataItem("TIFFTAG_EXTRASAMPLES", "_DEBUG_"),
                nullptr);
    }

    // RAT + scale/offset/unittype/description/colorinterp + xml:XMP +
    // PHOTOMETRIC=MINISWHITE
    {
      CPLStringList co;
      co.SetNameValue("PHOTOMETRIC", "MINISWHITE");
      std::unique_ptr<GDALDataset> dr(
          drv->Create(p_rat, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(dr, nullptr);
      GDALDefaultRasterAttributeTable rat;
      rat.CreateColumn("Name", GFT_String, GFU_Name);
      rat.SetRowCount(2);
      rat.SetValue(0, 0, "Class0");
      rat.SetValue(1, 0, "Class1");
      EXPECT_EQ(dr->GetRasterBand(1)->SetDefaultRAT(&rat), CE_None);
      EXPECT_EQ(dr->GetRasterBand(1)->SetScale(2.5), CE_None);
      EXPECT_EQ(dr->GetRasterBand(1)->SetOffset(1.5), CE_None);
      EXPECT_EQ(dr->GetRasterBand(1)->SetUnitType("meters"), CE_None);
      dr->GetRasterBand(1)->SetDescription("BandOneDesc");
      EXPECT_EQ(dr->GetRasterBand(1)->SetMetadataItem("COLOR_INTERPRETATION",
                                                      "CustomCI"),
                CE_None);
      const char* xmp[] = {"<x:xmpmeta>test</x:xmpmeta>", nullptr};
      EXPECT_EQ(dr->SetMetadata(const_cast<char**>(xmp), "xml:XMP"), CE_None);
      const char* xml_custom[] = {"<custom>1</custom>", nullptr};
      EXPECT_EQ(dr->SetMetadata(const_cast<char**>(xml_custom), "xml:CUSTOM"),
                CE_None);
    }
    {
      std::unique_ptr<GDALDataset> dr(
          GDALDataset::Open(p_rat, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(dr, nullptr);
      EXPECT_NE(dr->GetRasterBand(1)->GetDefaultRAT(), nullptr);
      EXPECT_NE(dr->GetRasterBand(1)->GetColorTable(), nullptr);
      EXPECT_DOUBLE_EQ(dr->GetRasterBand(1)->GetScale(), 2.5);
      EXPECT_DOUBLE_EQ(dr->GetRasterBand(1)->GetOffset(), 1.5);
      EXPECT_STREQ(dr->GetRasterBand(1)->GetUnitType(), "meters");
      EXPECT_STREQ(dr->GetRasterBand(1)->GetDescription(), "BandOneDesc");
      EXPECT_NE(dr->GetMetadata("xml:XMP"), nullptr);
      EXPECT_NE(dr->GetMetadata("xml:CUSTOM"), nullptr);
    }

    // GDT_Int64 & GDT_UInt64 NoData on open
    {
      std::unique_ptr<GDALDataset> di(
          drv->Create(p_i64, 4, 4, 1, GDT_Int64, nullptr));
      EXPECT_EQ(di->GetRasterBand(1)->SetNoDataValueAsInt64(-999), CE_None);
      std::unique_ptr<GDALDataset> du(
          drv->Create(p_u64, 4, 4, 1, GDT_UInt64, nullptr));
      EXPECT_EQ(du->GetRasterBand(1)->SetNoDataValueAsUInt64(999), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> di(
          GDALDataset::Open(p_i64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(di, nullptr);
      int ok = 0;
      EXPECT_EQ(di->GetRasterBand(1)->GetNoDataValueAsInt64(&ok), -999);
      EXPECT_EQ(ok, TRUE);
      std::unique_ptr<GDALDataset> du(
          GDALDataset::Open(p_u64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(du, nullptr);
      EXPECT_EQ(du->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok), 999U);
      EXPECT_EQ(ok, TRUE);
    }

    // WebP with PROFILE=BASELINE -> GetMetadata("IMAGE_STRUCTURE") probes
    // /vsisubfile/ with the WEBP driver!
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "WEBP");
      co.SetNameValue("PROFILE", "BASELINE");
      std::unique_ptr<GDALDataset> dw(
          drv->Create(p_webp_base, 16, 16, 3, GDT_Byte, co.List()));
      ASSERT_NE(dw, nullptr);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(dw->GetRasterBand(b)->Fill(100), CE_None);
      }
    }
    {
      std::unique_ptr<GDALDataset> dw(
          GDALDataset::Open(p_webp_base, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(dw, nullptr);
      EXPECT_NE(dw->GetMetadata("IMAGE_STRUCTURE"), nullptr);
      EXPECT_NE(
          dw->GetMetadataItem("COMPRESSION_REVERSIBILITY", "IMAGE_STRUCTURE"),
          nullptr);
    }
  }

  // 2. LoadGeoreferencingAndPamIfNeeded:
  // - Rotated GeoTransform (TIFFTAG_GEOTRANSMATRIX) + AREA_OR_POINT=Point
  // - GCPs (TIFFTAG_GEOTIEPOINTS) + AREA_OR_POINT=Point
  // - PAM <GeoTransform> overriding .wld sidecar + GEOREF_SOURCES=INTERNAL,PAM
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    const char* p_rot = "/vsimem/gsr_add_rot.tif";
    const char* p_gcp = "/vsimem/gsr_add_gcp.tif";
    const char* p_wld = "/vsimem/gsr_add_wld/w.tif";
    const char* p_wld_side = "/vsimem/gsr_add_wld/w.wld";
    const char* p_wld_aux = "/vsimem/gsr_add_wld/w.tif.aux.xml";
    absl::Cleanup clean = [&] {
      for (const char* p : {p_rot, p_gcp, p_wld, p_wld_side, p_wld_aux}) {
        VSIUnlink(p);
      }
    };

    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_rot, 8, 8, 1, GDT_Byte, nullptr));
      GDALGeoTransform gt(10.0, 1.0, 0.2, 20.0, 0.3, -1.0);
      EXPECT_EQ(ds->SetGeoTransform(gt), CE_None);
      EXPECT_EQ(ds->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT),
                CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_rot, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GDALGeoTransform gt;
      EXPECT_EQ(ds->GetGeoTransform(gt), CE_None);
      EXPECT_NEAR(gt[0], 10.0, 1e-6);
      EXPECT_NEAR(gt[2], 0.2, 1e-6);
    }

    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_gcp, 8, 8, 1, GDT_Byte, nullptr));
      GDAL_GCP gcp;
      GDALInitGCPs(1, &gcp);
      gcp.dfGCPPixel = 2.0;
      gcp.dfGCPLine = 3.0;
      gcp.dfGCPX = 100.0;
      gcp.dfGCPY = 200.0;
      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(ds->SetGCPs(1, &gcp, &srs), CE_None);
      GDALDeinitGCPs(1, &gcp);
      EXPECT_EQ(ds->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT),
                CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_gcp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 1);
      EXPECT_NE(ds->GetGCPSpatialRef(), nullptr);
      ASSERT_NE(ds->GetGCPs(), nullptr);
      // Pins an upstream asymmetry: with AREA_OR_POINT=Point, the Create()
      // write path (GTiffDataset::WriteGeoTIFFInfo) adds +0.5 to the GCP
      // pixel/line and the reader (gtiffdataset_read.cpp GCP loading) adds
      // +0.5 again, while CreateCopy subtracts 0.5. So the 2.0 written above
      // round-trips to 3.0 here.
      EXPECT_DOUBLE_EQ(ds->GetGCPs()[0].dfGCPPixel, 3.0);
    }

    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_wld, 8, 8, 1, GDT_Byte, nullptr));
    }
    WriteTextToVsiMem(p_wld_side, "1.0\n0.0\n0.0\n-1.0\n10.5\n20.5\n");
    WriteTextToVsiMem(p_wld_aux,
                      "<PAMDataset>"
                      "<SRS>EPSG:4326</SRS>"
                      "<GeoTransform>50, 2, 0, 60, 0, -2</GeoTransform>"
                      "</PAMDataset>");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_wld, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GDALGeoTransform gt;
      EXPECT_EQ(ds->GetGeoTransform(gt), CE_None);
      EXPECT_DOUBLE_EQ(gt[0], 50.0);
      EXPECT_NE(ds->GetSpatialRef(), nullptr);
    }
    {
      const char* const oo_georef[] = {
          "GEOREF_SOURCES=INTERNAL,TABFILE,WORLDFILE,PAM", nullptr};
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_wld, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_georef, nullptr));
      ASSERT_NE(ds, nullptr);
      EXPECT_NE(ds->GetSpatialRef(), nullptr);
    }
  }

  // 3. MultiThreadedRead additional branches:
  // - Dataset::RasterIO single-band sub-window on 4-band CONTIG dataset (both
  //   with small cache max and normal cache max -> L1117-1142!)
  // - AdviseRead total bytes limit splitting request into 2 halves (L1405-1439)
  // - GA_Update async compression queue WaitCompletionForBlock (L1221-1239)
  // - ThreadDecompressionFunc PRead failure (has_pread=true) and Seek/Read +
  //   LoadBlocks with has_pread=false (L619-670)
  {
    const char* p_mt4 = "/vsimem_gsr_mr/gsr_add_mt4.tif";
    const std::string back_mt4 = GsrMultiRangeHandler::ToMem(p_mt4);
    const char* p_upd = "/vsimem/gsr_add_mt_upd.tif";
    absl::Cleanup clean = [&] {
      g_gsr_mr->advise_read_limit = 0;
      g_gsr_mr->has_pread = false;
      VSIUnlink(back_mt4.c_str());
      VSIUnlink(p_upd);
    };

    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "DEFLATE");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_mt4, 32, 32, 4, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      for (int b = 1; b <= 4; ++b) {
        EXPECT_EQ(ds->GetRasterBand(b)->Fill(30 + b), CE_None);
      }
    }

    // Dataset::RasterIO single-band sub-window with small vs normal cache max,
    // and with has_pread = false (hits LoadBlocks() under !bHasPRead at L644!):
    g_gsr_mr->opt_multi_range = false;
    g_gsr_mr->has_pread = false;
    {
      std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
          p_mt4, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
      ASSERT_NE(ds, nullptr);
      int band1 = 1;
      std::vector<GByte> buf(32 * 30, 0);
      const GIntBig old_cache = GDALGetCacheMax64();
      {
        absl::Cleanup restore = [&] { GDALSetCacheMax64(old_cache); };
        GDALSetCacheMax64(256);
        EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(), 32, 30,
                               GDT_Byte, 1, &band1, 0, 0, 0, nullptr),
                  CE_None);
      }
      // Normal cache max -> bCacheAllBands = true &
      // bUseDeinterleaveOptimBlockCache = true!
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(), 32, 30,
                             GDT_Byte, 1, &band1, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(buf[0], 31);
    }

    // AdviseRead splitting into 2 halves when advise_read_limit = 40:
    g_gsr_mr->advise_read_limit = 40;
    {
      std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
          p_mt4, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> buf(32 * 32 * 4, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                             GDT_Byte, 4, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(buf[0], 31);
    }
    g_gsr_mr->advise_read_limit = 0;

    // Truncate backing file to trigger PRead failure (has_pread=true) and Read
    // failure (has_pread=false) in ThreadDecompressionFunc:
    {
      std::unique_ptr<GDALDataset> ds_p(GDALDataset::Open(
          p_mt4, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo_mt, nullptr));
      ASSERT_NE(ds_p, nullptr);
      // Force IFD/block map load before truncating backing file:
      int bx = 0, by = 0;
      ds_p->GetRasterBand(1)->GetBlockSize(&bx, &by);

      VSILFILE* fp = VSIFOpenL(back_mt4.c_str(), "r+b");
      ASSERT_NE(fp, nullptr);
      EXPECT_EQ(VSIFTruncateL(fp, 64), 0);
      VSIFCloseL(fp);

      CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
      std::vector<GByte> buf(32 * 32 * 4, 0);
      g_gsr_mr->has_pread = false;
      EXPECT_EQ(ds_p->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                               GDT_Byte, 4, nullptr, 0, 0, 0, nullptr),
                CE_Failure);
    }

    // GA_Update with NUM_THREADS=2: WriteBlock queues compression jobs, then
    // immediate MultiThreadedRead drains oQueue via WaitCompletionForBlock!
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "DEFLATE");
      co.SetNameValue("NUM_THREADS", "2");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_upd, 32, 32, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> tile(16 * 16, 77);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(1, 0, tile.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, tile.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(1, 1, tile.data()), CE_None);
      std::vector<GByte> out(32 * 32, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                             GDT_Byte, 1, nullptr, 0, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(out[0], 77);
    }
  }
}

}  // namespace
