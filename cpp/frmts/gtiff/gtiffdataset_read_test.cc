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
// Tests GeoTIFF dataset opening, subdatasets/IFDs, georeferencing, PAM,
// compressed data reading, and multi-threaded/direct/virtual-mem I/O
// (gtiffdataset_read.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffdataset_read.cpp

#include "third_party/gdal/frmts/gtiff/gtiffdataset.h"

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif  // __linux__

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
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
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

static_assert(std::endian::native == std::endian::little);

#ifdef __linux__
// ---------------------------------------------------------------------------
// 100% RAM memfd-backed VSI filesystem ("/vsimem_gsr_fd/") so
// VSIFGetNativeFileDescriptorL and CPLVirtualMemFileMapNew succeed in RAM.
// ---------------------------------------------------------------------------
class GsrMemFdHandler final : public VSIFilesystemHandler {
 public:
  ~GsrMemFdHandler() override {
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
      std::lock_guard<std::mutex> lock(mu_);
      auto it = fds_.find(pszFilename);
      const bool create = access.find('w') != std::string_view::npos ||
                          access.find('a') != std::string_view::npos;
      if (it == fds_.end()) {
        if (!create) return nullptr;
        fd = memfd_create("gtiff_gsr_vmem", 0);
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
  static constexpr std::string_view kPrefix = "/vsimem_gsr_mr/";

  static std::string ToMem(std::string_view path) {
    if (path.starts_with(kPrefix)) {
      return std::format("/vsimem/gsr_mr_backing_{}",
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

class GTiffDatasetReadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_COG();
    GDALRegister_JPEG();
    GDALRegister_WEBP();
    GTiffOneTimeInit();
#ifdef __linux__
    memfd_handler_ = std::make_unique<GsrMemFdHandler>();
    VSIFileManager::InstallHandler("/vsimem_gsr_fd/", memfd_handler_.get());
#endif  // __linux__
    mr_handler_ = std::make_unique<GsrMultiRangeHandler>();
    VSIFileManager::InstallHandler("/vsimem_gsr_mr/", mr_handler_.get());
  }

  static void TearDownTestSuite() {
#ifdef __linux__
    VSIFileManager::RemoveHandler("/vsimem_gsr_fd/");
    memfd_handler_.reset();
#endif  // __linux__
    VSIFileManager::RemoveHandler("/vsimem_gsr_mr/");
    mr_handler_.reset();
  }

  void SetUp() override {
    CPLErrorReset();
    gtiff_driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, gtiff_driver_);
    mr_handler_->has_pread = false;
    mr_handler_->fail_multi_range = false;
    mr_handler_->opt_multi_range = true;
    mr_handler_->advise_read_limit = 0;
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* gtiff_driver_ = nullptr;
#ifdef __linux__
  static inline std::unique_ptr<GsrMemFdHandler> memfd_handler_;
#endif  // __linux__
  static inline std::unique_ptr<GsrMultiRangeHandler> mr_handler_;
};

TEST_F(GTiffDatasetReadTest, ImplicitJpegOverviewsAndCompressedData) {
  // 1. Implicit JPEG overviews (GetJPEGOverviewCount):
  // - 1024x256 JPEG with JPEGTABLESMODE=1 (has JPEGTables)
  // - GTIFF_IMPLICIT_JPEG_OVR=NO disables implicit overviews
  // - 4-band CMYK JPEG returns 0 implicit overviews
  // - JPEGTABLESMODE=0 (no JPEGTables tag -> uses {0xFF, 0xD8})
  {
    constexpr char kJpg[] = "/vsimem/gsr_jpg_1024.tif";
    constexpr char kNoTbl[] = "/vsimem/gsr_jpg_notbl.tif";
    constexpr char kCmyk[] = "/vsimem/gsr_jpg_cmyk.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(kJpg);
      VSIUnlink(kNoTbl);
      VSIUnlink(kCmyk);
    };
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "256");
      co.SetNameValue("BLOCKYSIZE", "256");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kJpg, 1024, 256, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(100));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(150));
      EXPECT_EQ(CE_None, ds->GetRasterBand(3)->Fill(200));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kJpg, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> sub(256 * 64 * 3, 0);
      // First downsampled RasterIO computes m_nJPEGOverviewCount (3 levels);
      // second downsampled RasterIO hits cached m_nJPEGOverviewCount >= 0!
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 1024, 256, sub.data(), 128, 32,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 1024, 256, sub.data(), 256, 64,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }
    {
      CPLConfigOptionSetter off("GTIFF_IMPLICIT_JPEG_OVR", "NO", false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kJpg, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetOverview(0));
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("JPEGTABLESMODE", "0");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kNoTbl, 256, 256, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(80));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kNoTbl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetOverview(0));
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("PHOTOMETRIC", "CMYK");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kCmyk, 256, 256, 4, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(50));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kCmyk, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetOverview(0));
    }
  }

  // 2. GetCompressionFormats & ReadCompressedData:
  // - JPEG 4-band RGBA ("JPEG;colorspace=RGBA")
  // - JPEG PLANARCONFIG_SEPARATE (single band panBandList)
  // - WEBP & JXL & LZW & sparse unwritten tile
  // - Buffer modes: query-only (ppBuffer==nullptr), caller buffer too small vs
  //   large enough, driver-allocated buffer, and truncated file read failure!
  {
    constexpr char kRgba[] = "/vsimem/gsr_comp_rgba.tif";
    constexpr char kSep[] = "/vsimem/gsr_comp_sep.tif";
    constexpr char kWebp[] = "/vsimem/gsr_comp_webp.tif";
    constexpr char kJxl[] = "/vsimem/gsr_comp_jxl.tif";
    constexpr char kLzw[] = "/vsimem/gsr_comp_lzw.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {kRgba, kSep, kWebp, kJxl, kLzw}) {
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
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kRgba, 16, 16, 4, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      for (int b = 1; b <= 4; ++b) {
        EXPECT_EQ(CE_None, ds->GetRasterBand(b)->Fill(100));
      }
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kRgba, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      CPLStringList fmts = ds->GetCompressionFormats(0, 0, 16, 16, 4, nullptr);
      ASSERT_EQ(1, fmts.size());
      EXPECT_STREQ("JPEG;colorspace=RGBA", fmts[0]);
      // Non-whole-block returns empty:
      EXPECT_TRUE(ds->GetCompressionFormats(1, 0, 8, 8, 4, nullptr).empty());

      // Query size and detailed format only (ppBuffer == nullptr).
      size_t sz = 0;
      char* detailed_raw = nullptr;
      const CPLErr query_err = ds->ReadCompressedData(
          "JPEG", 0, 0, 16, 16, 4, nullptr, nullptr, &sz, &detailed_raw);
      std::unique_ptr<char, CplFreeDeleter> detailed(detailed_raw);
      EXPECT_EQ(CE_None, query_err);
      EXPECT_GT(sz, 0U);
      ASSERT_NE(nullptr, detailed);

      // Invalid format tokens ("JPEG;a=1;b=2" or "") -> CE_Failure.
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure,
                  ds->ReadCompressedData("JPEG;a=1;b=2", 0, 0, 16, 16, 4,
                                         nullptr, nullptr, &sz, nullptr));
        EXPECT_EQ(CE_Failure,
                  ds->ReadCompressedData("", 0, 0, 16, 16, 4, nullptr, nullptr,
                                         &sz, nullptr));
        // ppBuffer != nullptr && pnBufferSize == nullptr -> CE_Failure.
        void* buf = nullptr;
        EXPECT_EQ(CE_Failure,
                  ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr, &buf,
                                         nullptr, nullptr));
        CPLErrorReset();
      }

      // Caller-allocated buffer too small -> CE_Failure.
      std::vector<GByte> caller_buf(sz + 64, 0);
      void* p_caller = caller_buf.data();
      size_t small_sz = 2;
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure,
                  ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 4, nullptr,
                                         &p_caller, &small_sz, nullptr));
        CPLErrorReset();
      }

      // Caller-allocated buffer large enough -> CE_None.
      size_t big_sz = caller_buf.size();
      char* detailed2_raw = nullptr;
      const CPLErr caller_err = ds->ReadCompressedData(
          "JPEG", 0, 0, 16, 16, 4, nullptr, &p_caller, &big_sz, &detailed2_raw);
      std::unique_ptr<char, CplFreeDeleter> detailed2(detailed2_raw);
      EXPECT_EQ(CE_None, caller_err);
      ASSERT_NE(nullptr, detailed2);
      EXPECT_THAT(detailed2.get(), HasSubstr("colorspace=RGBA"));

      // Driver-allocated buffer (*ppBuffer == nullptr) -> CE_None.
      void* drv_buf_raw = nullptr;
      size_t drv_sz = 0;
      const CPLErr drv_err = ds->ReadCompressedData(
          "JPEG", 0, 0, 16, 16, 4, nullptr, &drv_buf_raw, &drv_sz, nullptr);
      std::unique_ptr<void, decltype(&VSIFree)> drv_buf(drv_buf_raw, VSIFree);
      EXPECT_EQ(CE_None, drv_err);
      EXPECT_NE(nullptr, drv_buf);
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
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kSep, 32, 16, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> tile(16 * 16, 90);
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kSep, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      // Upstream GetCompressionFormats/ReadCompressedData currently indexes
      // panBandList without 1-based GDAL band adjustment; TODO: b/335317901 -
      // update if upstream switches to 1-based band numbers.
      constexpr int kBandIdx1 = 1;
      EXPECT_FALSE(
          ds->GetCompressionFormats(0, 0, 16, 16, 1, &kBandIdx1).empty());
      // Sparse unwritten block (1,0) returns empty:
      EXPECT_TRUE(
          ds->GetCompressionFormats(16, 0, 16, 16, 1, &kBandIdx1).empty());

      void* drv_buf_raw = nullptr;
      size_t drv_sz = 0;
      {
        const CPLErr err =
            ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 1, &kBandIdx1,
                                   &drv_buf_raw, &drv_sz, nullptr);
        std::unique_ptr<void, decltype(&VSIFree)> drv_buf(drv_buf_raw, VSIFree);
        EXPECT_EQ(CE_None, err);
      }
      drv_buf_raw = nullptr;

      // Truncate backing file so TIFFReadRawTile fails and frees *ppBuffer!
      {
        VSILFILE* fp = VSIFOpenL(kSep, "r+b");
        ASSERT_NE(nullptr, fp);
        absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
        EXPECT_EQ(0, VSIFTruncateL(fp, 64));
      }
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure,
                  ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 1, &kBandIdx1,
                                         &drv_buf_raw, &drv_sz, nullptr));
        EXPECT_EQ(nullptr, drv_buf_raw);
        CPLErrorReset();
      }
    }

    // WEBP, JXL, and LZW
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "WEBP");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kWebp, 16, 16, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(50));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kWebp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(1, ds->GetCompressionFormats(0, 0, 16, 16, 3, nullptr).size());
      void* buf_raw = nullptr;
      size_t sz = 0;
      const CPLErr err = ds->ReadCompressedData(
          "WEBP", 0, 0, 16, 16, 3, nullptr, &buf_raw, &sz, nullptr);
      std::unique_ptr<void, decltype(&VSIFree)> buf(buf_raw, VSIFree);
      EXPECT_EQ(CE_None, err);
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JXL");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kJxl, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(60));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kJxl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      void* buf_raw = nullptr;
      size_t sz = 0;
      const CPLErr err = ds->ReadCompressedData("JXL", 0, 0, 16, 16, 1, nullptr,
                                                &buf_raw, &sz, nullptr);
      std::unique_ptr<void, decltype(&VSIFree)> buf(buf_raw, VSIFree);
      EXPECT_EQ(CE_None, err);
      ds.reset();

      // Patch COMPRESSION_JXL_DNG_1_7 (52546) -> COMPRESSION_JXL (50002) in IFD
      // so GetCompressionFormats hits case COMPRESSION_JXL:
      vsi_l_offset len = 0;
      GByte* raw = VSIGetMemFileBuffer(kJxl, &len, FALSE);
      ASSERT_NE(nullptr, raw);
      for (vsi_l_offset i = 0; i + 12 <= len; ++i) {
        uint16_t tag = 0;
        uint16_t ty = 0;
        uint16_t val = 0;
        std::memcpy(&tag, raw + i, 2);
        std::memcpy(&ty, raw + i + 2, 2);
        std::memcpy(&val, raw + i + 8, 2);
        if (tag == TIFFTAG_COMPRESSION && ty == 3 && val == 52546) {
          uint16_t jxl_old = 50002;
          std::memcpy(raw + i + 8, &jxl_old, 2);
          break;
        }
      }
      ds.reset(GDALDataset::Open(kJxl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(1, ds->GetCompressionFormats(0, 0, 16, 16, 1, nullptr).size());
    }
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "LZW");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kLzw, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(70));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kLzw, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_TRUE(ds->GetCompressionFormats(0, 0, 16, 16, 1, nullptr).empty());
      size_t sz = 0;
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure,
                  ds->ReadCompressedData("LZW", 0, 0, 16, 16, 1, nullptr,
                                         nullptr, &sz, nullptr));
        CPLErrorReset();
      }
    }
  }
}

TEST_F(GTiffDatasetReadTest, MultiThreadedReadAllPaths) {
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
  // Upstream GDALRasterBlock::Internalize() races on block cache linked list
  // under multi-threaded decompression.
  const CPLStringList oo_mt{"NUM_THREADS=1"};
#else
  const CPLStringList oo_mt{"NUM_THREADS=2"};
#endif

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
    constexpr char kPath[] = "/vsimem/gsr_mt_3b.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kPath); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("PREDICTOR", "2");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 50, 50, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(11));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(22));
      EXPECT_EQ(CE_None, ds->GetRasterBand(3)->Fill(33));
    }

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                          oo_mt.List(), nullptr));
    ASSERT_NE(nullptr, ds);

    // Mode A: BIP full-image read.
    std::vector<GByte> bip(50 * 50 * 3, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 50, 50, bip.data(), 50, 50, GDT_Byte,
                           3, nullptr, 3, 50 * 3, 1, nullptr));
    EXPECT_EQ(11, bip[0]);
    EXPECT_EQ(22, bip[1]);
    EXPECT_EQ(33, bip[2]);

    // Mode B: BSQ full-image read.
    std::vector<GByte> bsq(50 * 50 * 3, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 50, 50, bsq.data(), 50, 50,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(11, bsq[0]);

    // Mode D: Sub-window BSQ read (uses block cache + GDALDeinterleave),
    // repeated twice so second call hits already-cached blocks!
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 50, 48, bsq.data(), 50, 48,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 50, 48, bsq.data(), 50, 48,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));

    // Mode E: Sub-window with reversed band map {3, 2, 1} on fresh open.
    ds.reset(GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY,
                               nullptr, oo_mt.List(), nullptr));
    ASSERT_NE(nullptr, ds);
    int rev_bands[3] = {3, 2, 1};
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 3, 5, 40, 38, bsq.data(), 40, 38,
                                    GDT_Byte, 3, rev_bands, 0, 0, 0, nullptr));
    EXPECT_EQ(33, bsq[0]);

    // Aligned-end window (4 + 28 == 32, multiple of 16).
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 4, 4, 28, 28, bsq.data(), 28, 28,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
  }

  // 2. 4-band RGBA + sparse tiles (both without NoData and with NoData=77) +
  // single-band read with tiny cache (GDALSetCacheMax64) vs normal cache +
  // bHasPRead == false via /vsimem_gsr_mr/!
  {
    constexpr char kPath[] = "/vsimem_gsr_mr/gsr_mt_4b_sparse.tif";
    const std::string backing = GsrMultiRangeHandler::ToMem(kPath);
    absl::Cleanup clean = [&] { VSIUnlink(backing.c_str()); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "DEFLATE");
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("ALPHA", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 32, 32, 4, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(77.0));
      std::vector<GByte> tile(16 * 16, 44);
      for (int b = 1; b <= 4; ++b) {
        EXPECT_EQ(CE_None, ds->GetRasterBand(b)->WriteBlock(0, 0, tile.data()));
      }
    }

    // Open with has_pread = false -> exercises Seek+Read under mutex in
    // ThreadDecompressionFunc!
    mr_handler_->has_pread = false;
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                          oo_mt.List(), nullptr));
    ASSERT_NE(nullptr, ds);

    // Full-image 4-band BSQ read (hits 4-band deinterleave + sparse NoData=77).
    std::vector<GByte> buf(32 * 32 * 4, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                                    GDT_Byte, 4, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(44, buf[0]);
    EXPECT_EQ(77, buf[20]);  // Sparse tile (1, 0) filled with NoData=77!

    // Single-band sub-window read with tiny cache max -> disables aggressive
    // band caching!
    const GIntBig old_cache = GDALGetCacheMax64();
    {
      absl::Cleanup restore_cache = [&] { GDALSetCacheMax64(old_cache); };
      GDALSetCacheMax64(512);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 32, 30, buf.data(), 32, 30,
                             GDT_Byte, 0, 0, nullptr));
    }
    // Single-band sub-window read with normal cache -> bCacheAllBands = true.
    ds.reset(GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY,
                               nullptr, oo_mt.List(), nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(),
                                             32, 30, GDT_Byte, 0, 0, nullptr));
  }

  // 3. Uncompressed little-endian & big-endian (ENDIANNESS=INVERTED),
  // PLANARCONFIG_SEPARATE, LERC, YCbCr JPEG, and GA_Update (both dirty block in
  // cache and clean after FlushCache)!
  {
    constexpr char kUncomp[] = "/vsimem/gsr_mt_uncomp.tif";
    constexpr char kBe[] = "/vsimem/gsr_mt_be.tif";
    constexpr char kLerc[] = "/vsimem/gsr_mt_lerc.tif";
    constexpr char kYcbcr[] = "/vsimem/gsr_mt_ycbcr.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {kUncomp, kBe, kLerc, kYcbcr}) {
        VSIUnlink(p);
      }
    };

    // Uncompressed little-endian CONTIG & SEPARATE
    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "8");
      co.SetNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kUncomp, 16, 20, 2, GDT_UInt16, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint16_t> s0(16 * 8, 500);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, s0.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, s0.data()));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_mt.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      std::vector<uint16_t> out(16 * 20 * 2, 0);
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 16, 20, out.data(), 16, 20,
                             GDT_UInt16, 2, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(500, out[0]);
      EXPECT_EQ(0, out[16 * 8]);  // Sparse strip 1 filled with 0.
    }

    // Big-endian SEPARATE UInt16
    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "8");
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("ENDIANNESS", "INVERTED");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kBe, 16, 16, 2, GDT_UInt16, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(1000));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(2000));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kBe, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_mt.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      std::vector<uint16_t> out(16 * 16 * 2, 0);
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 16, 16, out.data(), 16, 16,
                             GDT_UInt16, 2, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(1000, out[0]);
    }

    // LERC + GA_Update (dirty block fallback & clean multi-threaded read)
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LERC");
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
          gtiff_driver_->Create(kLerc, 32, 32, 1, GDT_Float32, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(42.0));
      ds->FlushCache(false);

      // Clean GA_Update MultiThreadedRead:
      std::vector<float> fbuf(32 * 32, 0.0f);
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 32, 32, fbuf.data(), 32, 32,
                             GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));
      EXPECT_FLOAT_EQ(42.0f, fbuf[0]);

      // Dirty 1 pixel in cache without flushing -> falls back to base impl!
      float one = 99.0f;
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Write, 0, 0, 1, 1, &one, 1, 1, GDT_Float32, 1,
                             nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, 32, 32, fbuf.data(), 32, 32,
                             GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));
      EXPECT_FLOAT_EQ(99.0f, fbuf[0]);
    }

    // YCbCr JPEG multi-threaded read
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("PHOTOMETRIC", "YCBCR");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kYcbcr, 32, 32, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(CE_None, ds->GetRasterBand(b)->Fill(128));
      }
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kYcbcr, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_mt.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> out(32 * 32 * 3, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                                      GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
      EXPECT_NEAR(128, out[0], 5);
    }
  }
}

TEST_F(GTiffDatasetReadTest, VirtualMemIOAndDirectIOAllBranches) {
  // Create reusable tiled & stripped CONTIG & SEPARATE test datasets in
  // /vsimem/ and /vsimem_gsr_fd/ (including sparse + dense blocks and
  // big-endian UInt16/CInt16).
  constexpr char kTiledCtg[] = "/vsimem/gsr_vmdio_t_ctg.tif";
  constexpr char kTiledSep[] = "/vsimem/gsr_vmdio_t_sep.tif";
  constexpr char kStripCtg[] = "/vsimem/gsr_vmdio_s_ctg.tif";
  constexpr char kStripSep[] = "/vsimem/gsr_vmdio_s_sep.tif";
#ifdef __linux__
  constexpr char kFdTiled[] = "/vsimem_gsr_fd/gsr_fd_t.tif";
#endif  // __linux__
  constexpr char kBeU16[] = "/vsimem/gsr_vmdio_be_u16.tif";
  constexpr char kBeC16[] = "/vsimem/gsr_vmdio_be_c16.tif";
  absl::Cleanup clean = [&] {
    for (const char* p :
         {kTiledCtg, kTiledSep, kStripCtg, kStripSep, kBeU16, kBeC16}) {
      VSIUnlink(p);
    }
#ifdef __linux__
    VSIUnlink(kFdTiled);
#endif  // __linux__
  };

  // Tiled CONTIG 3-band Byte (32x32, 16x16 tiles, tiles (0,0) and (1,1)
  // written, tiles (1,0) and (0,1) sparse with NoData=9).
  {
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("SPARSE_OK", "YES");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kTiledCtg, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(9.0));
    std::vector<GByte> t(16 * 16, 55);
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->WriteBlock(0, 0, t.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->WriteBlock(1, 1, t.data()));
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
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kTiledSep, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> t(16 * 16, 66);
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->WriteBlock(0, 0, t.data()));
    }
  }
  // Stripped CONTIG 3-band Byte (32x16, BLOCKYSIZE=4, dense so DirectIO works).
  {
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kStripCtg, 32, 16, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(10));
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(20));
    EXPECT_EQ(CE_None, ds->GetRasterBand(3)->Fill(30));
  }
  // Stripped SEPARATE 3-band Byte (32x16, BLOCKYSIZE=4, sparse strip 1).
  {
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    co.SetNameValue("INTERLEAVE", "BAND");
    co.SetNameValue("SPARSE_OK", "YES");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kStripSep, 32, 16, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> s(32 * 4, 77);
    for (int b = 1; b <= 3; ++b) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(b)->WriteBlock(0, 0, s.data()));
    }
  }
#ifdef __linux__
  // Native fd memfd file for CPLVirtualMemFileMapNew path in VirtualMemIO:
  {
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kFdTiled, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(99));
  }
#endif  // __linux__
  // Big-endian UInt16 & CInt16 stripped files:
  {
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    co.SetNameValue("ENDIANNESS", "INVERTED");
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kBeU16, 16, 16, 2, GDT_UInt16, co.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(1234));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(5678));
    }
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kBeC16, 16, 16, 1, GDT_CInt16, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<int16_t> cbuf(16 * 16 * 2, 321);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 16, 16, cbuf.data(), 16, 16,
                             GDT_CInt16, 0, 0, nullptr));
    }
  }

  // =========================================================================
  // Part A: GTIFF_VIRTUAL_MEM_IO=IF_ENOUGH_RAM / YES
  // =========================================================================
#ifdef __linux__
  {
    CPLConfigOptionSetter vm_ram("GTIFF_VIRTUAL_MEM_IO", "IF_ENOUGH_RAM",
                                 false);
    std::unique_ptr<GDALDataset> ds_fd(
        GDALDataset::Open(kFdTiled, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_fd);
    std::vector<GByte> buf(32 * 32 * 3, 0);
    EXPECT_EQ(CE_None,
              ds_fd->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                              GDT_Byte, 3, nullptr, 3, 32 * 3, 1, nullptr));
    EXPECT_EQ(99, buf[0]);
  }
#endif  // __linux__
  {
    CPLConfigOptionSetter vm_yes("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    // 1. Tiled CONTIG (bUseContigImplementation == true & false):
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kTiledCtg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> buf(64 * 64 * 5, 0);
    std::vector<uint16_t> u16_buf(64 * 64 * 4, 0);

    // Packed BIP no resampling:
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 3, 32 * 3, 1, nullptr));
    EXPECT_EQ(55, buf[0]);
    // Strided BIP (nPixelSpace = 4) no resampling:
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 4, 32 * 4, 1, nullptr));
    // Type-converting BIP (Byte -> UInt16) no resampling:
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 32, 32, u16_buf.data(), 32, 32,
                           GDT_UInt16, 3, nullptr, 6, 32 * 6, 2, nullptr));
    // X-resampling BIP (Byte -> Byte & Byte -> UInt16):
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 2, 2, 28, 28, buf.data(), 20, 20, GDT_Byte,
                           3, nullptr, 3, 20 * 3, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 2, 2, 28, 28, u16_buf.data(), 20, 20,
                           GDT_UInt16, 3, nullptr, 6, 20 * 6, 2, nullptr));
    // BSQ (bUseContigImplementation == false) no resampling & X-resampling:
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 30, 30, buf.data(), 20, 20,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 30, 30, u16_buf.data(), 20, 20,
                           GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr));
    // Hitting the last row of the last tile in BSQ with X-resampling triggers
    // FetchBytes boundary check (CE_Failure).
    // NOTE: this is a valid full-raster 32x32 -> 20x20 downsampled read that
    // should succeed. It pins a known upstream quirk/bug in
    // FetchBufferVirtualMemIO::FetchBytes, whose bounds check is too strict
    // for the last row of the last tile under GTIFF_VIRTUAL_MEM_IO=YES.
    // If that is fixed upstream, update this expectation to CE_None.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
      CPLErrorReset();
    }

    // 2. Tiled SEPARATE (bUseContigImplementation == false, memcpy & resample):
    std::unique_ptr<GDALDataset> ds_ts(
        GDALDataset::Open(kTiledSep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_ts);
    EXPECT_EQ(CE_None,
              ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));

    // 3. Stripped CONTIG (packed BIP, strided BIP, resampled BIP, type-conv
    // BIP, BSQ):
    std::unique_ptr<GDALDataset> ds_sc(
        GDALDataset::Open(kStripCtg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_sc);
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 3, 32 * 3, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 4, 32 * 4, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 20, 10,
                              GDT_Byte, 3, nullptr, 3, 20 * 3, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, u16_buf.data(), 20, 10,
                              GDT_UInt16, 3, nullptr, 6, 20 * 6, 2, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 20, 10,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));

    // 4. Stripped SEPARATE (sparse + dense, BSQ):
    std::unique_ptr<GDALDataset> ds_ss(
        GDALDataset::Open(kStripSep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_ss);
    EXPECT_EQ(CE_None,
              ds_ss->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds_ss->RasterIO(GF_Read, 0, 0, 32, 16, u16_buf.data(), 20, 10,
                              GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr));

    // 5. Big-endian UInt16 & CInt16 via VirtualMemIO:
    std::unique_ptr<GDALDataset> ds_be(
        GDALDataset::Open(kBeU16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_be);
    EXPECT_EQ(CE_None,
              ds_be->RasterIO(GF_Read, 0, 0, 16, 16, u16_buf.data(), 16, 16,
                              GDT_UInt16, 2, nullptr, 4, 16 * 4, 2, nullptr));
    EXPECT_EQ(1234, u16_buf[0]);

    std::unique_ptr<GDALDataset> ds_bec(
        GDALDataset::Open(kBeC16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_bec);
    std::vector<int16_t> c_out(16 * 16 * 2, 0);
    EXPECT_EQ(CE_None,
              ds_bec->RasterIO(GF_Read, 0, 0, 16, 16, c_out.data(), 16, 16,
                               GDT_CInt16, 1, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(321, c_out[0]);
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
        GDALDataset::Open(kTiledCtg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    // Packed BIP:
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32, GDT_Byte,
                           3, nullptr, 3, 32 * 3, 1, nullptr));
    // BSQ:
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    // Resampling ychunk <= 256 (Byte & UInt16):
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 32, 32, u16_buf.data(), 20, 20,
                           GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr));
    // Upsampling ychunk > 256 (16 -> 300 in Y -> heap-allocated pabyTempData!):
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 8, 300,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    // 1-band read on CONTIG tiled (no resampling, downsampling, upsampling>256)
    int b1_only = 1;
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                                    GDT_Byte, 1, &b1_only, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 8, 300,
                                    GDT_Byte, 1, &b1_only, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 16, 16, u16_buf.data(), 8, 10,
                           GDT_UInt16, 1, &b1_only, 0, 0, 0, nullptr));

    // 2. Tiled SEPARATE (multi-band delegates per-band in DirectIO):
    std::unique_ptr<GDALDataset> ds_ts(
        GDALDataset::Open(kTiledSep, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_ts);
    EXPECT_EQ(CE_None,
              ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds_ts->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 20, 20,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));

    // 3. Stripped CONTIG DirectIO + CopyContigByteMultiBand:
    std::unique_ptr<GDALDataset> ds_sc(
        GDALDataset::Open(kStripCtg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_sc);
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 3, 32 * 3, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 4, 32 * 4, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 5, 32 * 5, 1, nullptr));
    int two_bands[2] = {1, 2};
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 2, two_bands, 4, 32 * 4, 1, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 32, 16,
                              GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    // Y-downsampling & X-resampling (Byte & UInt16) + Y-upsampling:
    EXPECT_EQ(CE_None, ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, buf.data(), 20, 8,
                                       GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds_sc->RasterIO(GF_Read, 0, 0, 32, 16, u16_buf.data(), 20, 24,
                              GDT_UInt16, 3, nullptr, 0, 0, 0, nullptr));

    // 4. Big-endian UInt16 & CInt16 via DirectIO:
    std::unique_ptr<GDALDataset> ds_be(
        GDALDataset::Open(kBeU16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_be);
    EXPECT_EQ(CE_None,
              ds_be->RasterIO(GF_Read, 0, 0, 16, 16, u16_buf.data(), 16, 16,
                              GDT_UInt16, 2, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(1234, u16_buf[0]);

    std::unique_ptr<GDALDataset> ds_bec(
        GDALDataset::Open(kBeC16, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds_bec);
    std::vector<int16_t> c_out(16 * 16 * 2, 0);
    EXPECT_EQ(CE_None,
              ds_bec->RasterIO(GF_Read, 0, 0, 16, 16, c_out.data(), 16, 16,
                               GDT_CInt16, 1, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(321, c_out[0]);
  }
}

TEST_F(GTiffDatasetReadTest, ReadStrileLoadBlockBufIdentifyAndBufferedStream) {
  // 1. LoadBlockBuf with partial bottom WebP tile (16x20 with 16x16 tiles ->
  // zeroes trailing padding lines) + overview ReadStrile setting
  // HAS_USED_READ_ENCODED_API=1!
  {
    constexpr char kWebp[] = "/vsimem/gsr_webp_partial.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kWebp); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "WEBP");
      co.SetNameValue("WEBP_LOSSLESS", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kWebp, 16, 20, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(CE_None, ds->GetRasterBand(b)->Fill(88));
      }
      int ovr = 2;
      EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kWebp, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("0",
                 ds->GetMetadataItem("HAS_USED_READ_ENCODED_API", "_DEBUG_"));
    std::vector<GByte> blk(16 * 16, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 1, blk.data()));
    EXPECT_EQ(88, blk[0]);
    // Read overview band -> sets m_poBaseDS->m_bHasUsedReadEncodedAPI = true!
    GByte ovr_px = 0;
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_band);
    EXPECT_EQ(CE_None, ovr_band->RasterIO(GF_Read, 0, 0, 1, 1, &ovr_px, 1, 1,
                                          GDT_Byte, 0, 0, nullptr));
    EXPECT_STREQ("1",
                 ds->GetMetadataItem("HAS_USED_READ_ENCODED_API", "_DEBUG_"));
  }

  // 2. Identify on GTIFF_RAW: and GTIFF_DIR:.
  {
    GDALOpenInfo oi_raw("GTIFF_RAW:/vsimem/nonexistent.tif", GA_ReadOnly);
    EXPECT_EQ(FALSE, GTiffDataset::Identify(&oi_raw));
    GDALOpenInfo oi_dir("GTIFF_DIR:1:/vsimem/nonexistent.tif", GA_ReadOnly);
    EXPECT_EQ(TRUE, GTiffDataset::Identify(&oi_dir));
  }

  // 3. GTIFFMakeBufferedStream (TIFF_READ_STREAMING=YES):
  // - ClassicTIFF LE & BE (ENDIANNESS=INVERTED) created with
  //   STREAMABLE_OUTPUT=YES
  // - BigTIFF LE & BE created with STREAMABLE_OUTPUT=YES, BIGTIFF=YES
  // - Corrupt streamed headers (IFD offset != 8 / != 16, nTags > 1024,
  //   nCount >= 16M, tag offset overflow, nMaxOffset > 10MB)
  {
    CPLConfigOptionSetter stream_on("TIFF_READ_STREAMING", "YES", false);
    for (bool bigtiff : {false, true}) {
      for (bool big_endian : {false, true}) {
        constexpr char kPath[] = "/vsimem/gsr_stream_valid.tif";
        absl::Cleanup clean = [&] { VSIUnlink(kPath); };
        {
          CPLStringList co;
          co.SetNameValue("STREAMABLE_OUTPUT", "YES");
          co.SetNameValue("BLOCKYSIZE", "4");
          if (bigtiff) co.SetNameValue("BIGTIFF", "YES");
          if (big_endian) co.SetNameValue("ENDIANNESS", "INVERTED");
          GDALDatasetUniquePtr ds(
              gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, co.List()));
          ASSERT_NE(nullptr, ds);
          EXPECT_EQ(CE_None,
                    ds->SetMetadataItem("TIFFTAG_IMAGEDESCRIPTION",
                                        "StreamedTestDescriptionLong"));
          std::vector<GByte> strip(8 * 4, 65);
          EXPECT_EQ(CE_None,
                    ds->GetRasterBand(1)->WriteBlock(0, 0, strip.data()));
          EXPECT_EQ(CE_None,
                    ds->GetRasterBand(1)->WriteBlock(0, 1, strip.data()));
          EXPECT_EQ(CE_None, ds->Close());
        }
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(nullptr, ds);
        GByte v = 0;
        EXPECT_EQ(CE_None,
                  ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &v, 1, 1,
                                                 GDT_Byte, 0, 0, nullptr));
        EXPECT_EQ(65, v);
      }
    }

    // Error paths in GTIFFMakeBufferedStream using raw in-memory buffers:
    {
      constexpr char kErrPath[] = "/vsimem/gsr_stream_err.tif";
      absl::Cleanup clean = [&] { VSIUnlink(kErrPath); };
      auto expect_stream_open_fails = [&] {
        CPLConfigOptionSetter stream_opt("TIFF_READ_STREAMING", "YES", false);
        WithQuietHandler quiet;
        CPLErrorReset();
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(kErrPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
        EXPECT_EQ(nullptr, ds);
        CPLErrorReset();
      };

      std::vector<GByte> raw(64, 0);
      raw[0] = 'I';
      raw[1] = 'I';
      raw[2] = 42;
      raw[4] = 24;  // IFD offset = 24 != 8
      {
        VSILFILE* fp =
            VSIFileFromMemBuffer(kErrPath, raw.data(), raw.size(), FALSE);
        ASSERT_NE(nullptr, fp);
        EXPECT_EQ(0, VSIFCloseL(fp));
      }
      expect_stream_open_fails();

      // ClassicTIFF with IFD offset = 8, nTags = 1025 (> 1024)
      raw[4] = 8;
      uint16_t ntags = 1025;
      std::memcpy(raw.data() + 8, &ntags, 2);
      expect_stream_open_fails();

      // ClassicTIFF with 1 tag, count = 20000000 (>= 16*1024*1024)
      ntags = 1;
      std::memcpy(raw.data() + 8, &ntags, 2);
      uint16_t tag_id = 270;
      uint16_t tag_type = 2;
      uint32_t tag_cnt = 20000000;
      uint32_t tag_off = 30;
      std::memcpy(raw.data() + 10, &tag_id, 2);
      std::memcpy(raw.data() + 12, &tag_type, 2);
      std::memcpy(raw.data() + 14, &tag_cnt, 4);
      std::memcpy(raw.data() + 18, &tag_off, 4);
      expect_stream_open_fails();

      // ClassicTIFF with tag_cnt = 16, tag_off = 0xFFFFFFFC (overflow)
      tag_cnt = 16;
      tag_off = 0xFFFFFFFCU;
      std::memcpy(raw.data() + 14, &tag_cnt, 4);
      std::memcpy(raw.data() + 18, &tag_off, 4);
      expect_stream_open_fails();

      // ClassicTIFF with tag_cnt = 16, tag_off = 11 * 1024 * 1024 (> 10MB)
      tag_off = 11 * 1024 * 1024;
      std::memcpy(raw.data() + 18, &tag_off, 4);
      expect_stream_open_fails();

      // ClassicTIFF with tag_off = 200 past EOF (GTIFFExtendMemoryFile short
      // read)
      tag_off = 200;
      std::memcpy(raw.data() + 18, &tag_off, 4);
      expect_stream_open_fails();

      // BigTIFF with IFD offset = 32 (!= 16) and nTags = 1025
      std::fill(raw.begin(), raw.end(), 0);
      raw[0] = 'I';
      raw[1] = 'I';
      raw[2] = 43;
      raw[4] = 8;
      raw[8] = 32;
      expect_stream_open_fails();

      raw[8] = 16;
      uint64_t btags = 1025;
      std::memcpy(raw.data() + 16, &btags, 8);
      expect_stream_open_fails();
    }
  }
}

TEST_F(GTiffDatasetReadTest, OpenDirGeoreferencingAndPamInfo) {
  // 1. OpenDir: GTIFF_DIR:1:, GTIFF_DIR:2:, GTIFF_RAW:GTIFF_DIR:1:,
  // GTIFF_DIR:off:<offset>:, GA_Update downgrade warning, and error cases!
  {
    constexpr char kMulti[] = "/vsimem/gsr_multi_ifd.tif";
    absl::Cleanup clean = [&] { VSIUnlink(kMulti); };
    {
      GDALDatasetUniquePtr ds1(
          gtiff_driver_->Create(kMulti, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds1);
      EXPECT_EQ(CE_None, ds1->GetRasterBand(1)->Fill(10));
    }
    {
      CPLStringList co;
      co.SetNameValue("APPEND_SUBDATASET", "YES");
      GDALDatasetUniquePtr ds2(
          gtiff_driver_->Create(kMulti, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds2);
      EXPECT_EQ(CE_None, ds2->GetRasterBand(1)->Fill(20));
    }

    // Query SUBDATASETS and IFD_OFFSET.
    std::string off_str;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMulti, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetMetadata("SUBDATASETS"));
      const char* s =
          ds->GetRasterBand(1)->GetMetadataItem("IFD_OFFSET", "TIFF");
      ASSERT_NE(nullptr, s);
      off_str = s;
    }

    // Open by index, raw + index, and byte offset:
    {
      const std::string p_dir1 = std::format("GTIFF_DIR:1:{}", kMulti);
      std::unique_ptr<GDALDataset> d1(
          GDALDataset::Open(p_dir1.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, d1);
      EXPECT_EQ(8, d1->GetRasterXSize());

      const std::string p_dir2 =
          std::format("GTIFF_RAW:GTIFF_DIR:2:{}", kMulti);
      std::unique_ptr<GDALDataset> d2(
          GDALDataset::Open(p_dir2.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, d2);
      EXPECT_EQ(16, d2->GetRasterXSize());

      const std::string p_off =
          std::format("GTIFF_DIR:off:{}:{}", off_str, kMulti);
      std::unique_ptr<GDALDataset> doff(
          GDALDataset::Open(p_off.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, doff);
      EXPECT_EQ(8, doff->GetRasterXSize());

      // OpenDir in GA_Update emits warning and opens RO:
      {
        std::unique_ptr<GDALDataset> dupd;
        {
          WithQuietHandler quiet;
          CPLErrorReset();
          dupd.reset(GDALDataset::Open(p_dir1.c_str(),
                                       GDAL_OF_RASTER | GDAL_OF_UPDATE));
          EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
          CPLErrorReset();
        }
        ASSERT_NE(nullptr, dupd);
      }

      // Invalid OpenDir strings:
      for (const std::string& bad_path :
           {std::string("GTIFF_DIR:"), std::format("GTIFF_DIR:0:{}", kMulti),
            std::string("GTIFF_DIR:1:"), std::format("GTIFF_DIR:99:{}", kMulti),
            std::string("GTIFF_DIR:1:/vsimem/no_such.tif")}) {
        WithQuietHandler quiet;
        CPLErrorReset();
        std::unique_ptr<GDALDataset> bad(
            GDALDataset::Open(bad_path.c_str(), GDAL_OF_RASTER));
        EXPECT_EQ(nullptr, bad);
        CPLErrorReset();
      }
    }
  }

  // 2. LookForProjectionFromXML (.xml EPSG<=32767 & .XML ESRI>32767),
  // MapInfo .tab sidecar (affine), Worldfile (.wld),
  // Negative ScaleY (GTIFF_HONOUR_NEGATIVE_SCALEY unset/YES/NO),
  // 3D Vertical Z scale/offset, and SHIFT_ORIGIN_IN_MINUS_180_PLUS_180!
  {
    // .xml with EPSG:4326 and .XML with ESRI:102001
    constexpr char kXml1[] = "/vsimem/gsr_xml1/a.tif";
    constexpr char kXml1Side[] = "/vsimem/gsr_xml1/a.xml";
    constexpr char kXml2[] = "/vsimem/gsr_xml2/b.tif";
    constexpr char kXml2Side[] = "/vsimem/gsr_xml2/b.XML";
    absl::Cleanup clean_xml = [&] {
      VSIUnlink(kXml1);
      VSIUnlink(kXml2);
    };
    {
      GDALDatasetUniquePtr d1(
          gtiff_driver_->Create(kXml1, 4, 4, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, d1);
      GDALDatasetUniquePtr d2(
          gtiff_driver_->Create(kXml2, 4, 4, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, d2);
    }
    VsiMemTempWrapper xml1_side(
        kXml1Side,
        "<metadata><refSysInfo><RefSystem><refSysID><identCode code=\"4326\"/>"
        "</refSysID></RefSystem></refSysInfo></metadata>");
    VsiMemTempWrapper xml2_side(
        kXml2Side,
        "<metadata><refSysInfo><RefSystem><refSysID><identCode "
        "code=\"102001\"/>"
        "</refSysID></RefSystem></refSysInfo></metadata>");
    {
      std::unique_ptr<GDALDataset> d1(
          GDALDataset::Open(kXml1, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, d1);
      EXPECT_NE(nullptr, d1->GetSpatialRef());
    }
    {
      CPLConfigOptionSetter no_rd("GDAL_DISABLE_READDIR_ON_OPEN", "YES", false);
      std::unique_ptr<GDALDataset> d2(
          GDALDataset::Open(kXml2, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, d2);
      EXPECT_NE(nullptr, d2->GetSpatialRef());
    }

    // .tab sidecar and .wld sidecar
    constexpr char kTab[] = "/vsimem/gsr_tab/t.tif";
    constexpr char kTabSide[] = "/vsimem/gsr_tab/t.tab";
    constexpr char kWld[] = "/vsimem/gsr_wld/w.tif";
    constexpr char kWldSide[] = "/vsimem/gsr_wld/w.wld";
    absl::Cleanup clean_tab = [&] {
      VSIUnlink(kTab);
      VSIUnlink(kWld);
    };
    {
      GDALDatasetUniquePtr d1(
          gtiff_driver_->Create(kTab, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, d1);
      GDALDatasetUniquePtr d2(
          gtiff_driver_->Create(kWld, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, d2);
    }
    VsiMemTempWrapper tab_side(
        kTabSide,
        "!table\n!version 300\n!charset Neutral\nDefinition Table\n"
        "  File \"t.tif\"\n  Type \"RASTER\"\n"
        "  (10.0, 20.0) (0, 0) Label \"Pt 1\",\n"
        "  (20.0, 20.0) (10, 0) Label \"Pt 2\",\n"
        "  (10.0, 10.0) (0, 10) Label \"Pt 3\"\n"
        "  CoordSys Earth Projection 1, 104\n");
    VsiMemTempWrapper wld_side(kWldSide, "1.0\n0.0\n0.0\n-1.0\n100.5\n200.5\n");
    {
      std::unique_ptr<GDALDataset> dt(
          GDALDataset::Open(kTab, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, dt);
      GDALGeoTransform gt;
      EXPECT_EQ(CE_None, dt->GetGeoTransform(gt));
      EXPECT_NE(nullptr, dt->GetSpatialRef());

      std::unique_ptr<GDALDataset> dw(
          GDALDataset::Open(kWld, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, dw);
      EXPECT_EQ(CE_None, dw->GetGeoTransform(gt));
      EXPECT_DOUBLE_EQ(100.0, gt[0]);
    }

    // Negative ScaleY + 3D Vertical SRS Z scale/offset + SHIFT_ORIGIN
    constexpr char kNegPath[] = "/vsimem/gsr_neg_scaley.tif";
    absl::Cleanup clean_neg = [&] { VSIUnlink(kNegPath); };
    {
      VSILFILE* fp = VSIFOpenL(kNegPath, "wb+");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
      TIFF* tif = VSI_TIFFOpen(kNegPath, "w", fp);
      ASSERT_NE(nullptr, tif);
      absl::Cleanup close_tif = [tif] { TIFFClose(tif); };

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
      constexpr uint16_t kGeoKeys[16] = {
          1,    1, 1, 3,     // Version 1, KeyRev 1.1, 3 keys
          1024, 0, 1, 2,     // GTModelTypeGeoKey = ModelTypeGeographic
          2048, 0, 1, 4326,  // GeographicTypeGeoKey = 4326
          4096, 0, 1, 5703   // VerticalCSTypeGeoKey = 5703
      };
      TIFFSetField(tif, kTiffTagGeoKeyDirectory, 16, kGeoKeys);
      GByte z[16] = {0};
      TIFFWriteEncodedStrip(tif, 0, z, 16);
      TIFFWriteDirectory(tif);
    }
    for (const char* opt : {static_cast<const char*>(nullptr), "YES", "NO"}) {
      SCOPED_TRACE(opt ? std::string(opt) : "<unset>");
      CPLConfigOptionSetter neg_opt("GTIFF_HONOUR_NEGATIVE_SCALEY", opt, false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kNegPath, GDAL_OF_RASTER | GDAL_OF_READONLY,
                            nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt;
      {
        std::optional<WithQuietHandler> quiet;
        if (opt == nullptr) {
          quiet.emplace();
        }
        CPLErrorReset();
        EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
        if (opt == nullptr) {
          EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
          EXPECT_THAT(CPLGetLastErrorMsg(),
                      HasSubstr("GTIFF_HONOUR_NEGATIVE_SCALEY"));
        } else {
          EXPECT_EQ(CE_None, CPLGetLastErrorType());
        }
        CPLErrorReset();
      }
      EXPECT_DOUBLE_EQ(-190.0, gt[0]);
      EXPECT_DOUBLE_EQ(
          (opt != nullptr && std::string_view(opt) == "YES") ? 2.0 : -2.0,
          gt[5]);
      int ok = 0;
      EXPECT_DOUBLE_EQ(3.5, ds->GetRasterBand(1)->GetScale(&ok));
      EXPECT_EQ(TRUE, ok);
    }
    {
      CPLConfigOptionSetter neg_opt("GTIFF_HONOUR_NEGATIVE_SCALEY", "NO",
                                    false);
      const CPLStringList oo_shift{"SHIFT_ORIGIN_IN_MINUS_180_PLUS_180=YES"};
      WithQuietHandler quiet;
      CPLErrorReset();
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kNegPath, GDAL_OF_RASTER | GDAL_OF_READONLY,
                            nullptr, oo_shift.List(), nullptr));
      CPLErrorReset();
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt;
      EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
      EXPECT_DOUBLE_EQ(170.0, gt[0]);  // -190 + 360!
    }
  }

  // 3. ApplyPamInfo: PAM GeoTransform, PAM SRS, PAM GCPs overriding internal
  // GT, xml:ESRI GeodataXform GCP synthesis, PAM metadata & band properties!
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    constexpr char kEsri[] = "/vsimem/gsr_pam_esri.tif";
    constexpr char kEsriAux[] = "/vsimem/gsr_pam_esri.tif.aux.xml";
    constexpr char kPamGcp[] = "/vsimem/gsr_pam_gcp.tif";
    constexpr char kPamGcpAux[] = "/vsimem/gsr_pam_gcp.tif.aux.xml";
    absl::Cleanup clean = [&] {
      VSIUnlink(kEsri);
      VSIUnlink(kPamGcp);
    };

    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kEsri, 100, 100, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_XRESOLUTION", "10"));
      EXPECT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_YRESOLUTION", "10"));
      EXPECT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_RESOLUTIONUNIT", "2"));
      GDALGeoTransform gt(0.0, 1.0, 0.0, 0.0, 0.0, -1.0);
      EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
    }
    VsiMemTempWrapper esri_aux(
        kEsriAux,
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
          GDALDataset::Open(kEsri, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(1, ds->GetGCPCount());
      EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
      EXPECT_NE(nullptr, ds->GetGCPs());
      EXPECT_STREQ("PAM_DS_VAL", ds->GetMetadataItem("PAM_DS_KEY"));
      EXPECT_STREQ("PamDesc", ds->GetRasterBand(1)->GetDescription());
      EXPECT_STREQ("PamUnit", ds->GetRasterBand(1)->GetUnitType());
      EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetColorTable());
    }

    // PAM GCPList + PAM GeoTransform & SRS priority test:
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPamGcp, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt(0.0, 1.0, 0.0, 0.0, 0.0, -1.0);
      EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
    }
    VsiMemTempWrapper pam_gcp_aux(
        kPamGcpAux,
        "<PAMDataset>"
        "<SRS>EPSG:4326</SRS>"
        "<GCPList Projection=\"EPSG:4326\">"
        "<GCP Id=\"1\" Pixel=\"0\" Line=\"0\" X=\"10\" Y=\"20\"/>"
        "</GCPList>"
        "</PAMDataset>");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kPamGcp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(1, ds->GetGCPCount());
      GDALGeoTransform gt;
      // Internal GT was invalidated by higher-priority PAM GCPs!
      EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
    }
  }
}

TEST_F(GTiffDatasetReadTest,
       OpenOffsetColorProfileExifMetadataAndCacheMultiRange) {
  // 1. OpenOffset error & edge cases via raw VSI_TIFFOpen:
  // - IMAGEWIDTH = 0
  // - Unconfigured codec (COMPRESSION_JBIG = 34661 and unknown = 12345)
  // - Missing RowsPerStrip tag -> warns "assuming all one strip"
  // - Subsampled non-JPEG YCbCr with BitsPerSample=16 -> error
  // - SampleFormat=IEEEFP with BitsPerSample=12 -> error
  // - SampleFormat=UINT with BitsPerSample=128 -> GDT_Unknown error
  {
    CPLErrorReset();
    for (int case_id = 0; case_id < 6; ++case_id) {
      SCOPED_TRACE(case_id);
      constexpr char kPath[] = "/vsimem/gsr_openoffset_edge.tif";
      absl::Cleanup clean = [&] { VSIUnlink(kPath); };
      {
        VSILFILE* fp = VSIFOpenL(kPath, "wb+");
        ASSERT_NE(nullptr, fp);
        absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
        TIFF* tif = VSI_TIFFOpen(kPath, "w", fp);
        ASSERT_NE(nullptr, tif);
        absl::Cleanup close_tif = [tif] { TIFFClose(tif); };

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
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 8);
        std::vector<GByte> zero(256, 0);
        TIFFWriteEncodedStrip(tif, 0, zero.data(), 64);
        TIFFWriteDirectory(tif);
      }

      if (case_id == 1) {
        // Directory is written; patch the COMPRESSION tag in memory to 34661
        // (JBIG), then to an unknown codec id.
        vsi_l_offset len = 0;
        GByte* buf = VSIGetMemFileBuffer(kPath, &len, FALSE);
        ASSERT_NE(nullptr, buf);
        for (vsi_l_offset i = 0; i + 12 <= len; ++i) {
          uint16_t t = 0;
          uint16_t ty = 0;
          std::memcpy(&t, buf + i, 2);
          std::memcpy(&ty, buf + i + 2, 2);
          if (t == TIFFTAG_COMPRESSION && ty == 3) {
            {
              uint16_t jbig = 34661;
              std::memcpy(buf + i + 8, &jbig, 2);
              WithQuietHandler quiet;
              CPLErrorReset();
              std::unique_ptr<GDALDataset> ds1(
                  GDALDataset::Open(kPath, GDAL_OF_RASTER));
              EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
              EXPECT_EQ(nullptr, ds1);
            }
            {
              uint16_t unk = 12345;
              std::memcpy(buf + i + 8, &unk, 2);
              WithQuietHandler quiet;
              CPLErrorReset();
              std::unique_ptr<GDALDataset> ds2(
                  GDALDataset::Open(kPath, GDAL_OF_RASTER));
              EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
              EXPECT_EQ(nullptr, ds2);
            }
            break;
          }
        }
      } else if (case_id == 2) {
        // Missing RowsPerStrip -> opens with warning!
        // Strip out tag 278 (TIFFTAG_ROWSPERSTRIP) by changing tag ID to 65000
        vsi_l_offset len = 0;
        GByte* buf = VSIGetMemFileBuffer(kPath, &len, FALSE);
        ASSERT_NE(nullptr, buf);
        for (vsi_l_offset i = 0; i + 12 <= len; ++i) {
          uint16_t t = 0;
          uint16_t ty = 0;
          std::memcpy(&t, buf + i, 2);
          std::memcpy(&ty, buf + i + 2, 2);
          if (t == TIFFTAG_ROWSPERSTRIP && (ty == 3 || ty == 4)) {
            uint16_t dummy_tag = 65000;
            std::memcpy(buf + i, &dummy_tag, 2);
            break;
          }
        }
        WithQuietHandler quiet;
        CPLErrorReset();
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(kPath, GDAL_OF_RASTER));
        EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
        EXPECT_NE(nullptr, ds);
      } else {
        WithQuietHandler quiet;
        CPLErrorReset();
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(kPath, GDAL_OF_RASTER));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
        EXPECT_EQ(nullptr, ds);
      }
    }
    CPLErrorReset();
  }

  // 2. LoadICCProfile (both embedded ICC profile and colorimetric tags +
  // TIFFTAG_TRANSFERRANGE), LoadEXIFMetadata (EXIF + GPS SubIFDs),
  // LoadMetadata (.IMD sidecar + internal RPC tag + .ovr skip), and all
  // _DEBUG_ GetMetadataItem keys!
  {
    constexpr char kIcc[] = "/vsimem/gsr_icc.tif";
    constexpr char kCol[] = "/vsimem/gsr_colorimetric.tif";
    constexpr char kExif[] = "/vsimem/gsr_exif.tif";
    constexpr char kImd[] = "/vsimem/gsr_imd/img.tif";
    constexpr char kImdSide[] = "/vsimem/gsr_imd/img.IMD";
    constexpr char kOvrExt[] = "/vsimem/gsr_imd/img.tif.ovr";
    absl::Cleanup clean = [&] {
      VSIUnlink(kIcc);
      VSIUnlink(kCol);
      VSIUnlink(kExif);
      VSIUnlink(kImd);
      VSIUnlink(kOvrExt);
    };

    // Embedded ICC profile
    {
      CPLStringList co;
      co.SetNameValue("SOURCE_ICC_PROFILE", "AAAA");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kIcc, 4, 4, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kIcc, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_STREQ("AAAA",
                   ds->GetMetadataItem("SOURCE_ICC_PROFILE", "COLOR_PROFILE"));
    }

    // Colorimetric tags + TIFFTAG_TRANSFERRANGE (0x0156) on NBITS=1 (2 entries)
    // libtiff does not know tag 0x0156, so register it via a tag extender that
    // chains to the previously installed (GDAL) extender. The extender must
    // stay active until the dataset is re-opened and read below, so it is
    // restored at the end of this enclosing scope.
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
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kCol, 8, 8, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kCol, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetMetadata("COLOR_PROFILE"));
      EXPECT_NE(nullptr, ds->GetMetadataItem("TIFFTAG_TRANSFERRANGE_WHITE",
                                             "COLOR_PROFILE"));
    }

    // EXIF & GPS SubIFDs synthesized in a ClassicTIFF in /vsimem/:
    {
      std::vector<GByte> raw(256, 0);
      raw[0] = 'I';
      raw[1] = 'I';
      raw[2] = 42;
      raw[4] = 8;
      auto put_u16 = [&](size_t off, uint16_t v) {
        std::memcpy(&raw[off], &v, 2);
      };
      auto put_u32 = [&](size_t off, uint32_t v) {
        std::memcpy(&raw[off], &v, 4);
      };
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
      {
        VSILFILE* fp =
            VSIFileFromMemBuffer(kExif, raw.data(), raw.size(), FALSE);
        ASSERT_NE(nullptr, fp);
        EXPECT_EQ(0, VSIFCloseL(fp));
      }

      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kExif, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetMetadata("EXIF"));
      EXPECT_STREQ("400", ds->GetMetadataItem("EXIF_ISOSpeedRatings", "EXIF"));
    }

    // IMD sidecar + internal RPC tag + .ovr skip + _DEBUG_ metadata items:
    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kImd, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      CPLStringList rpc{
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
          "SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"};
      EXPECT_EQ(CE_None, ds->SetMetadata(rpc.List(), "RPC"));
      EXPECT_EQ(CE_None, ds->SetMetadataItem("CUSTOM", "VAL"));
      GDALDatasetUniquePtr ds_ovr(
          gtiff_driver_->Create(kOvrExt, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds_ovr);
    }
    VsiMemTempWrapper imd_side(
        kImdSide,
        "BEGIN_GROUP = IMAGE_1\n\tsatId = \"WV02\";\nEND_GROUP = "
        "IMAGE_1\nEND;\n");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kImd, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      StringListPtr dlist(ds->GetMetadataDomainList(), CSLDestroy);
      ASSERT_NE(nullptr, dlist);
      EXPECT_NE(nullptr, ds->GetMetadata("IMD"));
      EXPECT_NE(nullptr, ds->GetMetadata("RPC"));
      EXPECT_EQ(nullptr, ds->GetMetadata("ProxyOverviewRequest"));
      EXPECT_NE(nullptr, ds->GetMetadata("DERIVED_SUBDATASETS"));
      EXPECT_EQ(nullptr,
                ds->GetMetadataItem("TIFFTAG_EXTRASAMPLES", "_DEBUG_"));
      EXPECT_STREQ("1", ds->GetMetadataItem("TIFFTAG_PHOTOMETRIC", "_DEBUG_"));
      EXPECT_NE(nullptr,
                ds->GetMetadataItem("TIFFTAG_GDAL_METADATA", "_DEBUG_"));
      EXPECT_STREQ("0",
                   ds->GetMetadataItem("HAS_USED_READ_ENCODED_API", "_DEBUG_"));
      for (const char* dbg :
           {"WEBP_LOSSLESS", "WEBP_LEVEL", "MAX_Z_ERROR",
            "MAX_Z_ERROR_OVERVIEW", "JXL_LOSSLESS", "JXL_DISTANCE",
            "JXL_ALPHA_DISTANCE", "JXL_EFFORT"}) {
        EXPECT_NE(nullptr, ds->GetMetadataItem(dbg, "_DEBUG_")) << dbg;
      }
      EXPECT_EQ(nullptr, ds->GetMetadataItem("UNKNOWN", "_DEBUG_"));

      std::unique_ptr<GDALDataset> ds_ovr(
          GDALDataset::Open(kOvrExt, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds_ovr);
      EXPECT_EQ(nullptr, ds_ovr->GetMetadata("IMD"));
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
    constexpr char kSrc[] = "/vsimem/gsr_cmr_src.tif";
    constexpr char kCog[] = "/vsimem_gsr_mr/gsr_cmr_cog.tif";
    constexpr char kTileCog[] = "/vsimem_gsr_mr/gsr_cmr_tile_cog.tif";
    const std::string back_cog = GsrMultiRangeHandler::ToMem(kCog);
    const std::string back_tile = GsrMultiRangeHandler::ToMem(kTileCog);
    absl::Cleanup clean = [&] {
      VSIUnlink(kSrc);
      VSIUnlink(back_cog.c_str());
      VSIUnlink(back_tile.c_str());
    };

    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      GDALDatasetUniquePtr src(
          gtiff_driver_->Create(kSrc, 64, 64, 3, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      EXPECT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));
      // Leave top-left 16x16 tile (0,0) as 0 (sparse in COG), fill rest!
      std::vector<GByte> patch(48 * 48, 120);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(CE_None, src->GetRasterBand(b)->RasterIO(
                               GF_Write, 16, 16, 48, 48, patch.data(), 48, 48,
                               GDT_Byte, 0, 0, nullptr));
      }
      EXPECT_EQ(CE_None, src->GetRasterBand(1)->GetMaskBand()->RasterIO(
                             GF_Write, 16, 16, 48, 48, patch.data(), 48, 48,
                             GDT_Byte, 0, 0, nullptr));

      GDALDriver* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
      ASSERT_NE(nullptr, cog_drv);
      CPLStringList co;
      co.SetNameValue("BLOCKSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr cog(cog_drv->CreateCopy(
          kCog, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, cog);

      // Also create INTERLEAVE=TILE COG via COG CreateCopy:
      CPLStringList tco;
      tco.SetNameValue("BLOCKSIZE", "16");
      tco.SetNameValue("COMPRESS", "LZW");
      tco.SetNameValue("INTERLEAVE", "TILE");
      GDALDatasetUniquePtr tcog(cog_drv->CreateCopy(
          kTileCog, src.get(), FALSE, tco.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, tcog);
    }

    // Read COG with FloatingPointWindowValidity + read mask band:
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kCog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr,
                ds->GetMetadataItem("GDAL_STRUCTURAL_METADATA", "TIFF"));
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.bFloatingPointWindowValidity = TRUE;
      extra.dfXOff = 0.0;
      extra.dfYOff = 0.0;
      extra.dfXSize = 64.0;
      extra.dfYSize = 64.0;
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                                      GDT_Byte, 3, nullptr, 0, 0, 0, &extra));
      std::vector<GByte> msk_out(64 * 64, 0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                             GF_Read, 0, 0, 64, 64, msk_out.data(), 64, 64,
                             GDT_Byte, 0, 0, nullptr));
    }

    // Read INTERLEAVE=TILE COG with disjoint bands {1, 3}:
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kTileCog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      int bands[2] = {1, 3};
      std::vector<GByte> out(64 * 64 * 2, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                                      GDT_Byte, 2, bands, 0, 0, 0, nullptr));
    }

    // Small GDAL_MAX_RAW_BLOCK_CACHE_SIZE limit & fail_multi_range fallback:
    {
      CPLConfigOptionSetter small_lim("GDAL_MAX_RAW_BLOCK_CACHE_SIZE", "40",
                                      false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kCog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                                      GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }
    {
      mr_handler_->fail_multi_range = true;
      absl::Cleanup reset_fail_mr = [] {
        GTiffDatasetReadTest::mr_handler_->fail_multi_range = false;
      };
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kCog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                                      GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }

    // Corrupt 4-byte trailer of last tile in back_cog -> CheckTrailer fails and
    // CacheMultiRange retries with m_bLeaderSizeAsUInt4 = false!
    {
      vsi_l_offset off_last = 0;
      vsi_l_offset sz_last = 0;
      {
        std::unique_ptr<GDALDataset> ds(
            GDALDataset::Open(kCog, GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(nullptr, ds);
        const char* so =
            ds->GetRasterBand(1)->GetMetadataItem("BLOCK_OFFSET_3_3", "TIFF");
        const char* ss =
            ds->GetRasterBand(1)->GetMetadataItem("BLOCK_SIZE_3_3", "TIFF");
        ASSERT_NE(nullptr, so);
        ASSERT_NE(nullptr, ss);
        off_last = static_cast<vsi_l_offset>(std::strtoull(so, nullptr, 10));
        sz_last = static_cast<vsi_l_offset>(std::strtoull(ss, nullptr, 10));
      }
      vsi_l_offset total_len = 0;
      GByte* raw = VSIGetMemFileBuffer(back_cog.c_str(), &total_len, FALSE);
      ASSERT_NE(nullptr, raw);
      ASSERT_LE(off_last + sz_last + 4, total_len);
      raw[off_last + sz_last] ^= 0xFF;  // Flip trailer byte!

      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kCog, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> out(64 * 64 * 3, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                                      GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }
  }
}

TEST_F(GTiffDatasetReadTest, AdditionalReadAndOpenCoverage) {
  const CPLStringList oo_mt{"NUM_THREADS=2"};

  // 1. OpenOffset:
  // - LERC_DEFLATE & LERC_ZSTD + MAX_Z_ERROR + MAX_Z_ERROR_OVERVIEW
  // - JXL lossy (JXL_LOSSLESS=NO, JXL_DISTANCE=2.0, JXL_ALPHA_DISTANCE=3.0,
  //   JXL_EFFORT=3)
  // - RAT in TIFFTAG_GDAL_METADATA + band scale/offset/unittype/description/
  //   colorinterp + xml:XMP + PHOTOMETRIC=MINISWHITE + 64-bit/Float32 NoData
  // - WebP baseline COMPRESSION_REVERSIBILITY (/vsisubfile/ probe)
  // - _DEBUG_ TIFFTAG_EXTRASAMPLES on 2-band dataset
  {
    constexpr char kLerc1[] = "/vsimem/gsr_add_lerc1.tif";
    constexpr char kLerc2[] = "/vsimem/gsr_add_lerc2.tif";
    constexpr char kJxl[] = "/vsimem/gsr_add_jxl.tif";
    constexpr char kRat[] = "/vsimem/gsr_add_rat.tif";
    constexpr char kI64[] = "/vsimem/gsr_add_i64.tif";
    constexpr char kU64[] = "/vsimem/gsr_add_u64.tif";
    constexpr char kWebpBase[] = "/vsimem/gsr_add_webp_base.tif";
    absl::Cleanup clean = [&] {
      for (const char* p :
           {kLerc1, kLerc2, kJxl, kRat, kI64, kU64, kWebpBase}) {
        VSIUnlink(p);
      }
    };

    // LERC_DEFLATE + MAX_Z_ERROR + MAX_Z_ERROR_OVERVIEW & LERC_ZSTD
    {
      CPLStringList co1;
      co1.SetNameValue("COMPRESS", "LERC_DEFLATE");
      co1.SetNameValue("MAX_Z_ERROR", "0.5");
      co1.SetNameValue("MAX_Z_ERROR_OVERVIEW", "1.5");
      GDALDatasetUniquePtr d1(
          gtiff_driver_->Create(kLerc1, 8, 8, 1, GDT_Float32, co1.List()));
      ASSERT_NE(nullptr, d1);
      EXPECT_EQ(CE_None,
                d1->GetRasterBand(1)->SetNoDataValue(-3.4028234663852886e+38));
      EXPECT_EQ(CE_None, d1->GetRasterBand(1)->Fill(1.0));
    }
    {
      std::unique_ptr<GDALDataset> d1(
          GDALDataset::Open(kLerc1, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, d1);
      EXPECT_STREQ("LERC_DEFLATE",
                   d1->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));
      EXPECT_STREQ("0.500000", d1->GetMetadataItem("MAX_Z_ERROR", "_DEBUG_"));
      EXPECT_STREQ("1.500000",
                   d1->GetMetadataItem("MAX_Z_ERROR_OVERVIEW", "_DEBUG_"));
    }
    {
      CPLStringList co2;
      co2.SetNameValue("COMPRESS", "LERC_ZSTD");
      GDALDatasetUniquePtr d2(
          gtiff_driver_->Create(kLerc2, 8, 8, 1, GDT_Byte, co2.List()));
      ASSERT_NE(nullptr, d2);
      EXPECT_EQ(CE_None, d2->GetRasterBand(1)->Fill(2));
    }
    {
      std::unique_ptr<GDALDataset> d2(
          GDALDataset::Open(kLerc2, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, d2);
      EXPECT_STREQ("LERC_ZSTD",
                   d2->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));
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
      GDALDatasetUniquePtr dj(
          gtiff_driver_->Create(kJxl, 8, 8, 2, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, dj);
      EXPECT_EQ(CE_None, dj->GetRasterBand(1)->Fill(10));
      EXPECT_EQ(CE_None, dj->GetRasterBand(2)->Fill(255));
    }
    {
      std::unique_ptr<GDALDataset> dj(
          GDALDataset::Open(kJxl, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, dj);
      EXPECT_STREQ("LOSSY", dj->GetMetadataItem("COMPRESSION_REVERSIBILITY",
                                                "IMAGE_STRUCTURE"));
      EXPECT_NE(nullptr,
                dj->GetMetadataItem("TIFFTAG_EXTRASAMPLES", "_DEBUG_"));
    }

    // RAT + scale/offset/unittype/description/colorinterp + xml:XMP +
    // PHOTOMETRIC=MINISWHITE
    {
      CPLStringList co;
      co.SetNameValue("PHOTOMETRIC", "MINISWHITE");
      GDALDatasetUniquePtr dr(
          gtiff_driver_->Create(kRat, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, dr);
      GDALDefaultRasterAttributeTable rat;
      rat.CreateColumn("Name", GFT_String, GFU_Name);
      rat.SetRowCount(2);
      rat.SetValue(0, 0, "Class0");
      rat.SetValue(1, 0, "Class1");
      EXPECT_EQ(CE_None, dr->GetRasterBand(1)->SetDefaultRAT(&rat));
      EXPECT_EQ(CE_None, dr->GetRasterBand(1)->SetScale(2.5));
      EXPECT_EQ(CE_None, dr->GetRasterBand(1)->SetOffset(1.5));
      EXPECT_EQ(CE_None, dr->GetRasterBand(1)->SetUnitType("meters"));
      dr->GetRasterBand(1)->SetDescription("BandOneDesc");
      EXPECT_EQ(CE_None, dr->GetRasterBand(1)->SetMetadataItem(
                             "COLOR_INTERPRETATION", "CustomCI"));
      CPLStringList xmp{"<x:xmpmeta>test</x:xmpmeta>"};
      EXPECT_EQ(CE_None, dr->SetMetadata(xmp.List(), "xml:XMP"));
      CPLStringList xml_custom{"<custom>1</custom>"};
      EXPECT_EQ(CE_None, dr->SetMetadata(xml_custom.List(), "xml:CUSTOM"));
    }
    {
      std::unique_ptr<GDALDataset> dr(
          GDALDataset::Open(kRat, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, dr);
      EXPECT_NE(nullptr, dr->GetRasterBand(1)->GetDefaultRAT());
      EXPECT_NE(nullptr, dr->GetRasterBand(1)->GetColorTable());
      EXPECT_DOUBLE_EQ(2.5, dr->GetRasterBand(1)->GetScale());
      EXPECT_DOUBLE_EQ(1.5, dr->GetRasterBand(1)->GetOffset());
      EXPECT_STREQ("meters", dr->GetRasterBand(1)->GetUnitType());
      EXPECT_STREQ("BandOneDesc", dr->GetRasterBand(1)->GetDescription());
      EXPECT_NE(nullptr, dr->GetMetadata("xml:XMP"));
      EXPECT_NE(nullptr, dr->GetMetadata("xml:CUSTOM"));
    }

    // GDT_Int64 & GDT_UInt64 NoData on open
    {
      GDALDatasetUniquePtr di(
          gtiff_driver_->Create(kI64, 4, 4, 1, GDT_Int64, nullptr));
      ASSERT_NE(nullptr, di);
      EXPECT_EQ(CE_None, di->GetRasterBand(1)->SetNoDataValueAsInt64(-999));
      GDALDatasetUniquePtr du(
          gtiff_driver_->Create(kU64, 4, 4, 1, GDT_UInt64, nullptr));
      ASSERT_NE(nullptr, du);
      EXPECT_EQ(CE_None, du->GetRasterBand(1)->SetNoDataValueAsUInt64(999));
    }
    {
      std::unique_ptr<GDALDataset> di(
          GDALDataset::Open(kI64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, di);
      int ok = 0;
      EXPECT_EQ(-999, di->GetRasterBand(1)->GetNoDataValueAsInt64(&ok));
      EXPECT_EQ(TRUE, ok);
      std::unique_ptr<GDALDataset> du(
          GDALDataset::Open(kU64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, du);
      EXPECT_EQ(999U, du->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok));
      EXPECT_EQ(TRUE, ok);
    }

    // WebP with PROFILE=BASELINE -> GetMetadata("IMAGE_STRUCTURE") probes
    // /vsisubfile/ with the WEBP driver!
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "WEBP");
      co.SetNameValue("PROFILE", "BASELINE");
      GDALDatasetUniquePtr dw(
          gtiff_driver_->Create(kWebpBase, 16, 16, 3, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, dw);
      for (int b = 1; b <= 3; ++b) {
        EXPECT_EQ(CE_None, dw->GetRasterBand(b)->Fill(100));
      }
    }
    {
      std::unique_ptr<GDALDataset> dw(
          GDALDataset::Open(kWebpBase, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, dw);
      EXPECT_NE(nullptr, dw->GetMetadata("IMAGE_STRUCTURE"));
      EXPECT_NE(nullptr, dw->GetMetadataItem("COMPRESSION_REVERSIBILITY",
                                             "IMAGE_STRUCTURE"));
    }
  }

  // 2. LoadGeoreferencingAndPamIfNeeded:
  // - Rotated GeoTransform (TIFFTAG_GEOTRANSMATRIX) + AREA_OR_POINT=Point
  // - GCPs (TIFFTAG_GEOTIEPOINTS) + AREA_OR_POINT=Point
  // - PAM <GeoTransform> overriding .wld sidecar + GEOREF_SOURCES=INTERNAL,PAM
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    constexpr char kRot[] = "/vsimem/gsr_add_rot.tif";
    constexpr char kGcp[] = "/vsimem/gsr_add_gcp.tif";
    constexpr char kWld[] = "/vsimem/gsr_add_wld/w.tif";
    constexpr char kWldSide[] = "/vsimem/gsr_add_wld/w.wld";
    constexpr char kWldAux[] = "/vsimem/gsr_add_wld/w.tif.aux.xml";
    absl::Cleanup clean = [&] {
      for (const char* p : {kRot, kGcp, kWld}) {
        VSIUnlink(p);
      }
    };

    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kRot, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt(10.0, 1.0, 0.2, 20.0, 0.3, -1.0);
      EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
      EXPECT_EQ(CE_None,
                ds->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kRot, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt;
      EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
      EXPECT_NEAR(10.0, gt[0], 1e-6);
      EXPECT_NEAR(0.2, gt[2], 1e-6);
    }

    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kGcp, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      GDAL_GCP gcp;
      GDALInitGCPs(1, &gcp);
      absl::Cleanup free_gcp = [&] { GDALDeinitGCPs(1, &gcp); };
      gcp.dfGCPPixel = 2.0;
      gcp.dfGCPLine = 3.0;
      gcp.dfGCPX = 100.0;
      gcp.dfGCPY = 200.0;
      OGRSpatialReference srs;
      ASSERT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
      EXPECT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs));
      EXPECT_EQ(CE_None,
                ds->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT));
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kGcp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(1, ds->GetGCPCount());
      EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
      ASSERT_NE(nullptr, ds->GetGCPs());
      // Pins an upstream asymmetry: with AREA_OR_POINT=Point, the Create()
      // write path (GTiffDataset::WriteGeoTIFFInfo) adds +0.5 to the GCP
      // pixel/line and the reader (gtiffdataset_read.cpp GCP loading) adds
      // +0.5 again, while CreateCopy subtracts 0.5. So the 2.0 written above
      // round-trips to 3.0 here.
      EXPECT_DOUBLE_EQ(3.0, ds->GetGCPs()[0].dfGCPPixel);
    }

    {
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kWld, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
    }
    VsiMemTempWrapper wld_side(kWldSide, "1.0\n0.0\n0.0\n-1.0\n10.5\n20.5\n");
    VsiMemTempWrapper wld_aux(kWldAux,
                              "<PAMDataset>"
                              "<SRS>EPSG:4326</SRS>"
                              "<GeoTransform>50, 2, 0, 60, 0, -2</GeoTransform>"
                              "</PAMDataset>");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kWld, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      GDALGeoTransform gt;
      EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
      EXPECT_DOUBLE_EQ(50.0, gt[0]);
      EXPECT_NE(nullptr, ds->GetSpatialRef());
    }
    {
      const CPLStringList oo_georef{
          "GEOREF_SOURCES=INTERNAL,TABFILE,WORLDFILE,PAM"};
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kWld, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_georef.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      EXPECT_NE(nullptr, ds->GetSpatialRef());
    }
  }

  // 3. MultiThreadedRead additional branches:
  // - Dataset::RasterIO single-band sub-window on 4-band CONTIG dataset (both
  //   with small cache max and normal cache max)
  // - AdviseRead total bytes limit splitting request into 2 halves
  // - GA_Update async compression queue WaitCompletionForBlock
  // - ThreadDecompressionFunc Read failure + LoadBlocks with has_pread=false
  {
    constexpr char kMt4[] = "/vsimem_gsr_mr/gsr_add_mt4.tif";
    const std::string back_mt4 = GsrMultiRangeHandler::ToMem(kMt4);
    constexpr char kUpd[] = "/vsimem/gsr_add_mt_upd.tif";
    absl::Cleanup clean = [&] {
      mr_handler_->advise_read_limit = 0;
      mr_handler_->has_pread = false;
      mr_handler_->opt_multi_range = true;
      VSIUnlink(back_mt4.c_str());
      VSIUnlink(kUpd);
    };

    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "DEFLATE");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kMt4, 32, 32, 4, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      for (int b = 1; b <= 4; ++b) {
        EXPECT_EQ(CE_None, ds->GetRasterBand(b)->Fill(30 + b));
      }
    }

    // Dataset::RasterIO single-band sub-window with small vs normal cache max,
    // and with has_pread = false (hits LoadBlocks() under !bHasPRead!):
    mr_handler_->opt_multi_range = false;
    mr_handler_->has_pread = false;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMt4, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_mt.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      int band1 = 1;
      std::vector<GByte> buf(32 * 30, 0);
      const GIntBig old_cache = GDALGetCacheMax64();
      {
        absl::Cleanup restore = [&] { GDALSetCacheMax64(old_cache); };
        GDALSetCacheMax64(256);
        EXPECT_EQ(CE_None,
                  ds->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(), 32, 30,
                               GDT_Byte, 1, &band1, 0, 0, 0, nullptr));
      }
      // Normal cache max -> bCacheAllBands = true &
      // bUseDeinterleaveOptimBlockCache = true!
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 30, buf.data(), 32, 30,
                                      GDT_Byte, 1, &band1, 0, 0, 0, nullptr));
      EXPECT_EQ(31, buf[0]);
    }

    // AdviseRead splitting into 2 halves when advise_read_limit = 40:
    mr_handler_->advise_read_limit = 40;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kMt4, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_mt.List(), nullptr));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> buf(32 * 32 * 4, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                                      GDT_Byte, 4, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(31, buf[0]);
    }
    mr_handler_->advise_read_limit = 0;

    // Truncate backing file to trigger Read failure (has_pread=false) in
    // ThreadDecompressionFunc:
    {
      std::unique_ptr<GDALDataset> ds_p(
          GDALDataset::Open(kMt4, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            oo_mt.List(), nullptr));
      ASSERT_NE(nullptr, ds_p);
      // Force IFD/block map load before truncating backing file:
      ASSERT_NE(nullptr, ds_p->GetRasterBand(1)->GetMetadataItem(
                             "BLOCK_OFFSET_0_0", "TIFF"));

      {
        VSILFILE* fp = VSIFOpenL(back_mt4.c_str(), "r+b");
        ASSERT_NE(nullptr, fp);
        absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
        EXPECT_EQ(0, VSIFTruncateL(fp, 64));
      }

      std::vector<GByte> buf(32 * 32 * 4, 0);
      mr_handler_->has_pread = false;
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure,
                  ds_p->RasterIO(GF_Read, 0, 0, 32, 32, buf.data(), 32, 32,
                                 GDT_Byte, 4, nullptr, 0, 0, 0, nullptr));
        EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot read"));
        CPLErrorReset();
      }
    }

    // GA_Update with NUM_THREADS=2: WriteBlock queues compression jobs, then
    // immediate MultiThreadedRead drains oQueue via WaitCompletionForBlock!
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "DEFLATE");
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
          gtiff_driver_->Create(kUpd, 32, 32, 1, GDT_Byte, co.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<GByte> tile(16 * 16, 77);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(1, 0, tile.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 1, tile.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(1, 1, tile.data()));
      std::vector<GByte> out(32 * 32, 0);
      EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 32, 32, out.data(), 32, 32,
                                      GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));
      EXPECT_EQ(77, out[0]);
    }
  }
}

// TODO: b/335317901 - Cover remaining dataset-opening edge cases and
// low-probability I/O failure branches in gtiffdataset_read.cpp.

}  // namespace
}  // namespace autotest2
