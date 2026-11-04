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
// Tests the SRTM HGT raster driver (srtmhgtdataset.cpp).
//
// Environment and architecture caveats:
//   1. PAM is disabled in Google3 builds (no .aux.xml generated; GetNoDataValue
//      on Byte band returns FALSE).
//   2. Host is assumed little-endian (CPL_LSB), so GDALSwapWords/pabyBuffer
//      endian conversion branches are compiled in.
//   3. SRTMHGTDataset and SRTMHGTRasterBand are declared locally in
//      srtmhgtdataset.cpp without an export header; tested via GDALDriver*
//      (pfnIdentify, pfnOpen, pfnCreateCopy, and GDALOpenEx).
//
// See also:
//   https://gdal.org/drivers/raster/srtmhgt.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/srtmhgt/srtmhgtdataset.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/srtmhgt.py

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/log/check.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::NotNull;

constexpr GInt16 kSrtmhgtNoDataValue = -32768;
constexpr char kCtrlPrefix[] = "/vsictrl_srtmhgt/";

// RAII wrapper that creates a sized file in /vsimem/ and unlinks it on exit.
class SizedVsiMemFile {
 public:
  SizedVsiMemFile(std::string path, vsi_l_offset size)
      : path_(std::move(path)) {
    VSILFILE* fp = VSIFOpenL(path_.c_str(), "wb");
    CHECK_NE(nullptr, fp) << "Failed to open " << path_;
    if (size > 0) {
      CHECK_EQ(0, VSIFSeekL(fp, size - 1, SEEK_SET));
      constexpr GByte kZero = 0;
      CHECK_EQ(1, VSIFWriteL(&kZero, 1, 1, fp));
    }
    CHECK_EQ(0, VSIFCloseL(fp));
  }
  SizedVsiMemFile(const SizedVsiMemFile&) = delete;
  SizedVsiMemFile& operator=(const SizedVsiMemFile&) = delete;
  ~SizedVsiMemFile() { CHECK_EQ(0, VSIUnlink(path_.c_str())); }

  const char* path() const { return path_.c_str(); }

 private:
  std::string path_;
};

// Creates a zip archive in /vsimem/ containing an inner file of specified size.
void CreateZipArchiveWithSizedFile(const char* zip_path, const char* inner_name,
                                   size_t size) {
  std::string vsi_zip_path =
      std::string("/vsizip/") + zip_path + "/" + inner_name;
  VSILFILE* fp = VSIFOpenL(vsi_zip_path.c_str(), "wb");
  ASSERT_NE(nullptr, fp) << "Failed to create " << vsi_zip_path;
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  const std::vector<char> zeros(size, 0);
  ASSERT_EQ(size, VSIFWriteL(zeros.data(), 1, size, fp));
}

// Scoped error handler that collects all emitted warning/error messages.
class ScopedErrorCollector {
 public:
  ScopedErrorCollector() {
    CPLPushErrorHandlerEx(&ScopedErrorCollector::Callback, this);
  }
  ScopedErrorCollector(const ScopedErrorCollector&) = delete;
  ScopedErrorCollector& operator=(const ScopedErrorCollector&) = delete;
  ~ScopedErrorCollector() { CPLPopErrorHandler(); }

  const std::vector<std::string>& messages() const { return messages_; }

 private:
  static void CPL_STDCALL Callback(CPLErr /*err_class*/, int /*err_no*/,
                                   const char* msg) {
    auto* self =
        static_cast<ScopedErrorCollector*>(CPLGetErrorHandlerUserData());
    if (self != nullptr) {
      self->messages_.emplace_back(msg != nullptr ? msg : "");
    }
  }

  std::vector<std::string> messages_;
};

// Custom virtual filesystem handler to test low-level Stat/Write failures and
// large virtual file sizes without allocating tens of megabytes of memory.
// Variant of aaigrid's ControllableVsiHandler;
// TODO: b/335317901 - Hoist to util/controllable_vsi_handler.h.
class ControllableVsiHandler final : public VSIFilesystemHandler {
 public:
  class Handle final : public VSIVirtualHandle {
   public:
    Handle(ControllableVsiHandler* handler, bool read_mode)
        : handler_(handler), read_mode_(read_mode) {}

    int Seek(vsi_l_offset offset, int whence) override {
      if (whence == SEEK_SET) {
        offset_ = static_cast<size_t>(offset);
      } else if (whence == SEEK_CUR) {
        offset_ += static_cast<size_t>(offset);
      } else if (whence == SEEK_END) {
        const size_t end_size = handler_->virtual_size_ > 0
                                    ? handler_->virtual_size_
                                    : handler_->buffer_.size();
        offset_ = end_size + static_cast<size_t>(offset);
      }
      return 0;
    }

    vsi_l_offset Tell() override { return offset_; }

    size_t Read(void* buffer, size_t size, size_t count) override {
      if (!read_mode_) return 0;
      const size_t total = size * count;
      if (handler_->virtual_size_ > 0) {
        if (offset_ >= handler_->virtual_size_) return 0;
        const size_t avail = handler_->virtual_size_ - offset_;
        const size_t to_copy = std::min(total, avail);
        std::memset(buffer, 0, to_copy);
        offset_ += to_copy;
        return size > 0 ? to_copy / size : 0;
      }
      if (offset_ >= handler_->buffer_.size()) return 0;
      const size_t avail = handler_->buffer_.size() - offset_;
      const size_t to_copy = std::min(total, avail);
      std::memcpy(buffer, handler_->buffer_.data() + offset_, to_copy);
      offset_ += to_copy;
      return size > 0 ? to_copy / size : 0;
    }

    size_t Write(const void* buffer, size_t size, size_t count) override {
      ++handler_->write_calls_;
      if (handler_->fail_write_on_call_ > 0 &&
          handler_->write_calls_ == handler_->fail_write_on_call_) {
        return 0;
      }
      const size_t total = size * count;
      const auto* chars = static_cast<const char*>(buffer);
      if (offset_ + total > handler_->buffer_.size()) {
        handler_->buffer_.resize(offset_ + total);
      }
      std::memcpy(handler_->buffer_.data() + offset_, chars, total);
      offset_ += total;
      return count;
    }

    void ClearErr() override {}
    int Eof() override { return 0; }
    int Error() override { return 0; }
    int Close() override { return 0; }

   private:
    ControllableVsiHandler* handler_;
    bool read_mode_;
    size_t offset_ = 0;
  };

  VSIVirtualHandleUniquePtr Open(const char* /*filename*/, const char* access,
                                 bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    const bool is_read = (std::strchr(access, 'r') != nullptr);
    if (!is_read) {
      buffer_.clear();
    }
    return VSIVirtualHandleUniquePtr(new Handle(this, is_read));
  }

  int Stat(const char* /*pszFilename*/, VSIStatBufL* stat_buf,
           int /*nFlags*/) override {
    ++stat_calls_;
    if (fail_stat_on_call_ > 0 && stat_calls_ == fail_stat_on_call_) {
      return -1;
    }
    std::memset(stat_buf, 0, sizeof(*stat_buf));
    stat_buf->st_mode = S_IFREG;
    if (second_stat_size_ > 0 && stat_calls_ == 2) {
      stat_buf->st_size = second_stat_size_;
    } else {
      stat_buf->st_size = virtual_size_ > 0 ? virtual_size_ : buffer_.size();
    }
    return 0;
  }

  void Reset() {
    buffer_.clear();
    virtual_size_ = 0;
    second_stat_size_ = 0;
    stat_calls_ = 0;
    fail_stat_on_call_ = 0;
    write_calls_ = 0;
    fail_write_on_call_ = 0;
  }

  std::string buffer_;
  size_t virtual_size_ = 0;
  size_t second_stat_size_ = 0;
  int stat_calls_ = 0;
  int fail_stat_on_call_ = 0;
  int write_calls_ = 0;
  int fail_write_on_call_ = 0;
};

// In-memory fake raster band for CreateCopy tests.
class FakeSourceBand final : public GDALRasterBand {
 public:
  FakeSourceBand(GDALDataset* ds, int band, GDALDataType dt, int block_x_size,
                 int block_y_size) {
    poDS = ds;
    nBand = band;
    eDataType = dt;
    nBlockXSize = block_x_size;
    nBlockYSize = block_y_size;
  }

  void SetNoData(bool has_nodata, double val) {
    has_nodata_ = has_nodata;
    nodata_value_ = val;
  }

  void SetFailRead(bool fail) { fail_read_ = fail; }

  void SetPixel(int x, int y, int16_t val) { pixels_[{x, y}] = val; }

  double GetNoDataValue(int* pbSuccess = nullptr) override {
    if (pbSuccess != nullptr) {
      *pbSuccess = has_nodata_ ? TRUE : FALSE;
    }
    return nodata_value_;
  }

  CPLErr IReadBlock(int /*nBlockXOff*/, int nBlockYOff, void* pImage) override {
    if (fail_read_) return CE_Failure;
    auto* dst = static_cast<int16_t*>(pImage);
    for (int x = 0; x < nBlockXSize; ++x) {
      auto it = pixels_.find({x, nBlockYOff});
      dst[x] = (it != pixels_.end()) ? it->second : 0;
    }
    return CE_None;
  }

 private:
  bool has_nodata_ = false;
  double nodata_value_ = kSrtmhgtNoDataValue;
  bool fail_read_ = false;
  std::map<std::pair<int, int>, int16_t> pixels_;
};

// In-memory fake dataset for CreateCopy tests without needing the MEM driver.
class FakeSourceDataset final : public GDALDataset {
 public:
  FakeSourceDataset(int xsize, int ysize, int nbands,
                    GDALDataType dt = GDT_Int16) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    for (int i = 1; i <= nbands; ++i) {
      SetBand(i, new FakeSourceBand(this, i, dt, xsize, 1));
    }
  }

  void SetFailGeoTransform(bool fail) { fail_gt_ = fail; }
  void SetCustomGeoTransform(const GDALGeoTransform& gt) {
    fail_gt_ = false;
    gt_ = gt;
  }
  void SetCustomSpatialRef(const OGRSpatialReference* srs) {
    if (srs != nullptr) {
      srs_ = *srs;
    } else {
      srs_.Clear();
    }
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    if (fail_gt_) return CE_Failure;
    gt = gt_;
    return CE_None;
  }

  const OGRSpatialReference* GetSpatialRef() const override {
    return srs_.IsEmpty() ? nullptr : &srs_;
  }

 private:
  bool fail_gt_ = false;
  GDALGeoTransform gt_{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  OGRSpatialReference srs_{};
};

int AbortImmediatelyProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                             void* /*pProgressArg*/) {
  return FALSE;
}

class SrtmhgtDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_SRTMHGT();
    driver_ = GetGDALDriverManager()->GetDriverByName("SRTMHGT");
    ASSERT_THAT(driver_, NotNull());
    ctrl_handler_ = std::make_unique<ControllableVsiHandler>();
    VSIFileManager::InstallHandler(kCtrlPrefix, ctrl_handler_.get());
  }

  void TearDown() override {
    VSIFileManager::RemoveHandler(kCtrlPrefix);
    ctrl_handler_.reset();
    CPLErrorReset();
  }

  GDALDriver* driver_ = nullptr;
  std::unique_ptr<ControllableVsiHandler> ctrl_handler_;
};

// ============================================================================
// Driver Registration & Metadata
// ============================================================================

TEST_F(SrtmhgtDatasetTest, DriverRegistrationAndMetadata) {
  // Test both deregistration/reregistration and idempotence.
  GDALDriverManager* dm = GetGDALDriverManager();
  GDALDriver* drv = dm->GetDriverByName("SRTMHGT");
  if (drv != nullptr) {
    dm->DeregisterDriver(drv);
    delete drv;
  }
  GDALRegister_SRTMHGT();
  // Second call must hit the early-return branch.
  GDALRegister_SRTMHGT();

  GDALDriver* driver = dm->GetDriverByName("SRTMHGT");
  ASSERT_THAT(driver, NotNull());
  EXPECT_STREQ("SRTMHGT", driver->GetDescription());
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("SRTMHGT File Format",
               driver->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("hgt", driver->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("drivers/raster/srtmhgt.html",
               driver->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("Byte Int16 UInt16",
               driver->GetMetadataItem(GDAL_DMD_CREATIONDATATYPES));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_VIRTUALIO));

  EXPECT_NE(nullptr, driver->pfnIdentify);
  EXPECT_NE(nullptr, driver->pfnOpen);
  EXPECT_NE(nullptr, driver->pfnCreateCopy);
}

// ============================================================================
// Identify: Rejections
// ============================================================================

struct IdentifyRejectionCase {
  const char* filename;
  vsi_l_offset file_size = 0;
  bool create_file = false;
};

TEST_F(SrtmhgtDatasetTest, IdentifyRejectsInvalidFilenamesExtensionsAndSizes) {
  constexpr IdentifyRejectionCase kCases[] = {
      // Non-existent path
      {.filename = "/vsimem/srtmhgt_rej/does_not_exist"},
      // Length < 11 chars
      {.filename = "/vsimem/srtmhgt_rej/N00E00.hgt",
       .file_size = 1201 * 1201 * 2,
       .create_file = true},
      // fileName[7] != '.'
      {.filename = "/vsimem/srtmhgt_rej/N00E000X.hgt",
       .file_size = 1201 * 1201 * 2,
       .create_file = true},
      // Neither 'n' nor 's' at index 0
      {.filename = "/vsimem/srtmhgt_rej/X00E000.hgt",
       .file_size = 1201 * 1201 * 2,
       .create_file = true},
      // Neither 'e' nor 'w' at index 3
      {.filename = "/vsimem/srtmhgt_rej/N00X000.hgt",
       .file_size = 1201 * 1201 * 2,
       .create_file = true},
      // Unsupported extension
      {.filename = "/vsimem/srtmhgt_rej/N00E000.tif",
       .file_size = 1201 * 1201 * 2,
       .create_file = true},
      // Valid filename format, but file does not exist on disk
      {.filename = "/vsimem/srtmhgt_rej/N00E000.hgt"},
      // Unsupported file size (100 bytes)
      {.filename = "/vsimem/srtmhgt_rej/N00E000.hgt",
       .file_size = 100,
       .create_file = true},
  };

  for (const auto& tc : kCases) {
    std::unique_ptr<SizedVsiMemFile> mem_file;
    if (tc.create_file) {
      mem_file = std::make_unique<SizedVsiMemFile>(tc.filename, tc.file_size);
    }
    GDALOpenInfo open_info(tc.filename, GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, driver_->pfnIdentify(&open_info))
        << "Failed rejection for: " << tc.filename;
    EXPECT_THAT(driver_->pfnOpen(&open_info), IsNull());
  }
}

// ============================================================================
// Identify & Open: All Supported Extensions and Unit Types
// ============================================================================

struct ExtensionTestCase {
  std::string_view extension;
  std::string_view expected_unit;
};

TEST_F(SrtmhgtDatasetTest, IdentifyAndOpenAllExtensionsAndUnitTypes) {
  constexpr ExtensionTestCase kCases[] = {
      {.extension = ".hgt", .expected_unit = "m"},
      {.extension = ".hgts", .expected_unit = "m"},
      {.extension = ".err", .expected_unit = ""},
      {.extension = ".img", .expected_unit = ""},
      {.extension = ".num", .expected_unit = ""},
      {.extension = ".raw", .expected_unit = "m"},
      {.extension = ".swb", .expected_unit = ""},
      // Note: .hgt.gz pins upstream behavior where the extension check is
      // string-based; opening a raw uncompressed file succeeds via standard
      // VSI.
      {.extension = ".hgt.gz", .expected_unit = "m"},
  };

  for (const auto& tc : kCases) {
    const std::string path =
        std::string("/vsimem/srtmhgt_ext/N43W080") + std::string(tc.extension);
    SizedVsiMemFile mem_file(path, 1201 * 1201 * 2);

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, driver_->pfnIdentify(&open_info))
        << "Failed identify for: " << path;

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull()) << "Failed to open " << path;
    ASSERT_EQ(1, ds->GetRasterCount());
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());
    EXPECT_EQ(tc.expected_unit, band->GetUnitType());
    EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());
  }
}

// ============================================================================
// Identify & Open: All Supported File Sizes, Dimensions, and Hemispheres
// ============================================================================

struct SizeTestCase {
  const char* filename;
  vsi_l_offset file_size;
  int expected_xsize;
  int expected_ysize;
  GDALDataType expected_datatype;
  double expected_origin_x;
  double expected_origin_y;
};

TEST_F(SrtmhgtDatasetTest, IdentifyAndOpenAllSupportedFileSizesAndHemispheres) {
  // Coordinate calculations mirror srtmhgtdataset.cpp:482-487 exactly.
  constexpr SizeTestCase kCases[] = {
      // 1201 x 1201 Int16 (North, West)
      {.filename = "/vsictrl_srtmhgt/N43W080.hgt",
       .file_size = 1201 * 1201 * 2,
       .expected_xsize = 1201,
       .expected_ysize = 1201,
       .expected_datatype = GDT_Int16,
       .expected_origin_x = -80.0 - 0.5 / 1200.0,
       .expected_origin_y = 44.0 + 0.5 / 1200.0},
      // 1801 x 3601 Int16 (lowercase north, east)
      {.filename = "/vsictrl_srtmhgt/n43e080.hgt",
       .file_size = 1801 * 3601 * 2,
       .expected_xsize = 1801,
       .expected_ysize = 3601,
       .expected_datatype = GDT_Int16,
       .expected_origin_x = 80.0 - 0.5 / 1800.0,
       .expected_origin_y = 44.0 + 0.5 / 3600.0},
      // 3601 x 3601 Byte (South, East)
      {.filename = "/vsictrl_srtmhgt/S12E034.swb",
       .file_size = 3601 * 3601,
       .expected_xsize = 3601,
       .expected_ysize = 3601,
       .expected_datatype = GDT_Byte,
       .expected_origin_x = 34.0 - 0.5 / 3600.0,
       .expected_origin_y = -12.0 + 1 + 0.5 / 3600.0},
      // 3601 x 3601 Int16 (lowercase south, west)
      {.filename = "/vsictrl_srtmhgt/s12w034.hgt",
       .file_size = 3601 * 3601 * 2,
       .expected_xsize = 3601,
       .expected_ysize = 3601,
       .expected_datatype = GDT_Int16,
       .expected_origin_x = -34.0 - 0.5 / 3600.0,
       .expected_origin_y = -12.0 + 1 + 0.5 / 3600.0},
      // 3601 x 3601 Float32 (.hgts)
      {.filename = "/vsictrl_srtmhgt/N10E020.hgts",
       .file_size = 3601 * 3601 * 4,
       .expected_xsize = 3601,
       .expected_ysize = 3601,
       .expected_datatype = GDT_Float32,
       .expected_origin_x = 20.0 - 0.5 / 3600.0,
       .expected_origin_y = 11.0 + 0.5 / 3600.0},
      // 7201 x 7201 Int16
      {.filename = "/vsictrl_srtmhgt/N00E000.hgt",
       .file_size = 7201 * 7201 * 2,
       .expected_xsize = 7201,
       .expected_ysize = 7201,
       .expected_datatype = GDT_Int16,
       .expected_origin_x = 0.0 - 0.5 / 7200.0,
       .expected_origin_y = 1.0 + 0.5 / 7200.0},
  };

  for (const auto& tc : kCases) {
    ctrl_handler_->Reset();
    ctrl_handler_->virtual_size_ = static_cast<size_t>(tc.file_size);

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tc.filename, GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull()) << "Failed to open " << tc.filename;
    EXPECT_EQ(tc.expected_xsize, ds->GetRasterXSize());
    EXPECT_EQ(tc.expected_ysize, ds->GetRasterYSize());
    EXPECT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(0, ds->GetGCPCount());

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(tc.expected_origin_x, gt[0]);
    EXPECT_DOUBLE_EQ(1.0 / (tc.expected_xsize - 1), gt[1]);
    EXPECT_DOUBLE_EQ(0.0, gt[2]);
    EXPECT_DOUBLE_EQ(tc.expected_origin_y, gt[3]);
    EXPECT_DOUBLE_EQ(0.0, gt[4]);
    EXPECT_DOUBLE_EQ(-1.0 / (tc.expected_ysize - 1), gt[5]);

    EXPECT_STREQ(GDALMD_AOP_POINT, ds->GetMetadataItem(GDALMD_AREA_OR_POINT));

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());
    EXPECT_EQ(tc.expected_datatype, band->GetRasterDataType());
    int block_x = 0;
    int block_y = 0;
    band->GetBlockSize(&block_x, &block_y);
    EXPECT_EQ(tc.expected_xsize, block_x);
    EXPECT_EQ(1, block_y);
    EXPECT_EQ(0, band->GetOverviewCount());
    EXPECT_THAT(band->GetColorTable(), IsNull());

    int has_nodata = FALSE;
    const double nodata_val = band->GetNoDataValue(&has_nodata);
    if (tc.expected_datatype == GDT_Byte) {
      // Byte datasets delegate to GDALPamRasterBand (nodata not set initially)
      EXPECT_EQ(FALSE, has_nodata);
    } else {
      EXPECT_EQ(TRUE, has_nodata);
      EXPECT_DOUBLE_EQ(kSrtmhgtNoDataValue, nodata_val);
      // Also test without pbSuccess
      EXPECT_DOUBLE_EQ(kSrtmhgtNoDataValue, band->GetNoDataValue(nullptr));
    }
  }
}

// ============================================================================
// Spatial Reference: Default WGS84 and Compound Coordinate System
// ============================================================================

TEST_F(SrtmhgtDatasetTest, SpatialReferenceDefaultAndCompoundCs) {
  ctrl_handler_->Reset();
  ctrl_handler_->virtual_size_ = 1201 * 1201 * 2;

  // 1. Default WGS84
  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        "/vsictrl_srtmhgt/N43W080.hgt", GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_THAT(srs, NotNull());
    OGRSpatialReference expected_wgs84;
    expected_wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    expected_wgs84.importFromWkt(SRS_WKT_WGS84_LAT_LONG);
    EXPECT_THAT(*srs, IsSameAs(expected_wgs84));
  }

  // 2. REPORT_COMPD_CS=YES
  {
    CPLSetConfigOption("REPORT_COMPD_CS", "YES");
    absl::Cleanup restore_opt = [] {
      CPLSetConfigOption("REPORT_COMPD_CS", nullptr);
    };

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        "/vsictrl_srtmhgt/N43W080.hgt", GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_THAT(srs, NotNull());
    EXPECT_TRUE(srs->IsCompound());
    EXPECT_STREQ("5773", srs->GetAuthorityCode("VERT_CS"));
    EXPECT_STREQ("4326", srs->GetAuthorityCode("GEOGCS"));
  }
}

// ============================================================================
// Zip Archive Variants and Low-Level Stat Edge Cases
// ============================================================================

TEST_F(SrtmhgtDatasetTest, IdentifyAndOpenZipVariantsAndStatEdgeCases) {
  // 1. In-memory .hgt.zip
  {
    constexpr char kZipPath[] = "/vsimem/srtmhgt_zip/N43W080.hgt.zip";
    auto cleanup_zip = absl::MakeCleanup([kZipPath] { VSIUnlink(kZipPath); });
    CreateZipArchiveWithSizedFile(kZipPath, "N43W080.hgt", 1201 * 1201 * 2);

    GDALOpenInfo open_info(kZipPath, GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, driver_->pfnIdentify(&open_info));

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kZipPath, GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ(kZipPath, ds->GetDescription());
    EXPECT_EQ(1201, ds->GetRasterXSize());
  }

  // 2. In-memory .srtmswbd.raw.zip
  {
    constexpr char kZipPath[] = "/vsimem/srtmhgt_zip/N43W080.srtmswbd.raw.zip";
    auto cleanup_zip = absl::MakeCleanup([kZipPath] { VSIUnlink(kZipPath); });
    CreateZipArchiveWithSizedFile(kZipPath, "N43W080.raw", 1201 * 1201 * 2);

    GDALOpenInfo open_info(kZipPath, GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, driver_->pfnIdentify(&open_info));

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kZipPath, GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ(kZipPath, ds->GetDescription());
    EXPECT_EQ(1201, ds->GetRasterXSize());
  }

  // 3. Stat failure on second call in OpenPAM (Identify's VSIStatL succeeds,
  // OpenPAM's fails).
  // Note: driver_->pfnOpen is called directly so that Identify's VSIStatL is
  // call #1 and OpenPAM's is call #2; GDALOpenEx would add a third stat call.
  {
    ctrl_handler_->Reset();
    ctrl_handler_->virtual_size_ = 1201 * 1201 * 2;
    ctrl_handler_->fail_stat_on_call_ = 2;

    GDALOpenInfo open_info("/vsictrl_srtmhgt/N43W080.hgt", GDAL_OF_READONLY);
    EXPECT_THAT(driver_->pfnOpen(&open_info), IsNull());
  }

  // 4. Stat returns unsupported size on second call in OpenPAM (upstream quirk:
  // switch default returns 0x0 raster rather than nullptr).
  {
    ctrl_handler_->Reset();
    ctrl_handler_->virtual_size_ = 1201 * 1201 * 2;
    ctrl_handler_->second_stat_size_ = 999;

    GDALOpenInfo open_info("/vsictrl_srtmhgt/N43W080.hgt", GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(driver_->pfnOpen(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(0, ds->GetRasterXSize());
    EXPECT_EQ(0, ds->GetRasterYSize());
  }
}

// ============================================================================
// Read and Write Blocks: Int16 and Byte Modes
// ============================================================================

TEST_F(SrtmhgtDatasetTest, ReadAndWriteBlocksInt16AndByteAndReadOnlyFailure) {
  // 1. Read-only write failure via dirty block
  {
    constexpr char kRoPath[] = "/vsimem/srtmhgt_rw/ro/N43W080.hgt";
    SizedVsiMemFile mem_file(kRoPath, 1201 * 1201 * 2);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(mem_file.path(), GDAL_OF_READONLY | GDAL_OF_RASTER));
    ASSERT_THAT(ds, NotNull());
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());

    GDALRasterBlock* block = band->GetLockedBlockRef(0, 0);
    ASSERT_THAT(block, NotNull());
    block->MarkDirty();
    EXPECT_EQ(CE_Failure, block->Write());
    block->DropLock();
  }

  // 2. Read and Write in Update mode (Int16)
  {
    constexpr char kUpPath[] = "/vsimem/srtmhgt_rw/up1/N43W080.hgt";
    SizedVsiMemFile mem_file(kUpPath, 1201 * 1201 * 2);

    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kUpPath, GDAL_OF_UPDATE | GDAL_OF_RASTER));
      ASSERT_THAT(ds, NotNull());
      GDALRasterBand* band = ds->GetRasterBand(1);
      ASSERT_THAT(band, NotNull());

      std::vector<int16_t> write_row(1201, 100);
      write_row[0] = 500;
      write_row[1] = -32768;
      write_row[1200] = 1200;
      ASSERT_EQ(CE_None, band->WriteBlock(0, 0, write_row.data()));
    }

    // Reopen read-only and verify values read back through IReadBlock
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kUpPath, GDAL_OF_READONLY | GDAL_OF_RASTER));
      ASSERT_THAT(ds, NotNull());
      GDALRasterBand* band = ds->GetRasterBand(1);
      ASSERT_THAT(band, NotNull());

      std::vector<int16_t> read_row(1201, 0);
      ASSERT_EQ(CE_None, band->ReadBlock(0, 0, read_row.data()));
      EXPECT_EQ(500, read_row[0]);
      EXPECT_EQ(-32768, read_row[1]);
      EXPECT_EQ(100, read_row[2]);
      EXPECT_EQ(1200, read_row[1200]);

      double min_max[2] = {};
      ASSERT_EQ(CE_None, band->ComputeRasterMinMax(FALSE, min_max));
      EXPECT_DOUBLE_EQ(0.0, min_max[0]);
      EXPECT_DOUBLE_EQ(1200.0, min_max[1]);

      // Verify big-endian byte layout in the underlying file
      VSILFILE* fp = VSIFOpenL(kUpPath, "rb");
      ASSERT_THAT(fp, NotNull());
      absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
      GByte bytes[4] = {};
      ASSERT_EQ(4, VSIFReadL(bytes, 1, 4, fp));
      // 500 is 0x01F4 in big-endian
      EXPECT_EQ(0x01, bytes[0]);
      EXPECT_EQ(0xF4, bytes[1]);
      // -32768 is 0x8000 in big-endian
      EXPECT_EQ(0x80, bytes[2]);
      EXPECT_EQ(0x00, bytes[3]);
    }
  }

  // 3. Read and Write in Update mode (Byte, .swb)
  {
    constexpr char kSwbPath[] = "/vsimem/srtmhgt_rw/up2/N43W080.swb";
    SizedVsiMemFile mem_file(kSwbPath, 3601 * 3601);

    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kSwbPath, GDAL_OF_UPDATE | GDAL_OF_RASTER));
      ASSERT_THAT(ds, NotNull());
      GDALRasterBand* band = ds->GetRasterBand(1);
      ASSERT_THAT(band, NotNull());

      std::vector<GByte> write_row(3601, 42);
      write_row[0] = 255;
      ASSERT_EQ(CE_None, band->WriteBlock(0, 0, write_row.data()));
    }

    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(kSwbPath, GDAL_OF_READONLY | GDAL_OF_RASTER));
      ASSERT_THAT(ds, NotNull());
      GDALRasterBand* band = ds->GetRasterBand(1);
      ASSERT_THAT(band, NotNull());

      std::vector<GByte> read_row(3601, 0);
      ASSERT_EQ(CE_None, band->ReadBlock(0, 0, read_row.data()));
      EXPECT_EQ(255, read_row[0]);
      EXPECT_EQ(42, read_row[1]);
    }
  }
}

// ============================================================================
// CreateCopy: Validations, Error Paths, and Warnings
// ============================================================================

TEST_F(SrtmhgtDatasetTest, CreateCopyValidationAndErrorPaths) {
  auto dir_cleanup =
      absl::MakeCleanup([] { VSIRmdirRecursive("/vsimem/srtmhgt_cc"); });

  // 1. Zero bands
  {
    FakeSourceDataset ds_0band(1201, 1201, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(driver_->CreateCopy("/vsimem/srtmhgt_cc/out0.hgt", &ds_0band,
                                    TRUE, nullptr, GDALDummyProgress, nullptr),
                IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("zero band"));
  }

  // 2. Two bands with strict = TRUE
  {
    FakeSourceDataset ds_2band(1201, 1201, 2);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(driver_->CreateCopy("/vsimem/srtmhgt_cc/out2.hgt", &ds_2band,
                                    TRUE, nullptr, GDALDummyProgress, nullptr),
                IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("only uses the first band"));
  }

  // 3. Missing geotransform
  {
    FakeSourceDataset ds_no_gt(1201, 1201, 1);
    ds_no_gt.SetFailGeoTransform(true);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(
        driver_->CreateCopy("/vsimem/srtmhgt_cc/out_nogt.hgt", &ds_no_gt, TRUE,
                            nullptr, GDALDummyProgress, nullptr),
        IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("geo transform matrix"));
  }

  // 4. Invalid dimensions (e.g., 100 x 100)
  {
    FakeSourceDataset ds_bad_dim(100, 100, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(
        driver_->CreateCopy("/vsimem/srtmhgt_cc/out_dim.hgt", &ds_bad_dim, TRUE,
                            nullptr, GDALDummyProgress, nullptr),
        IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Image dimensions should be"));
  }

  // 5. Output file creation failure ("/vsimem/")
  {
    FakeSourceDataset ds_ok(1201, 1201, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(driver_->CreateCopy("/vsimem/", &ds_ok, TRUE, nullptr,
                                    GDALDummyProgress, nullptr),
                IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot create file"));
  }

  // 6. Source RasterIO read failure (IReadBlock fails in gcore)
  {
    FakeSourceDataset ds_read_fail(1201, 1201, 1);
    auto* band = static_cast<FakeSourceBand*>(ds_read_fail.GetRasterBand(1));
    band->SetFailRead(true);
    constexpr char kPath[] = "/vsimem/srtmhgt_cc/out_rfail.hgt";
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(driver_->CreateCopy(kPath, &ds_read_fail, TRUE, nullptr,
                                    GDALDummyProgress, nullptr),
                IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 7. Write failure via ControllableVsiHandler
  {
    ctrl_handler_->Reset();
    ctrl_handler_->fail_write_on_call_ = 1;
    FakeSourceDataset ds_ok(1201, 1201, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(
        driver_->pfnCreateCopy("/vsictrl_srtmhgt/fail_write.hgt", &ds_ok, TRUE,
                               nullptr, GDALDummyProgress, nullptr),
        IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_FileIO, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed to write line 0"));
    ctrl_handler_->Reset();
  }

  // 8. User cancelled via progress callback
  {
    FakeSourceDataset ds_ok(1201, 1201, 1);
    constexpr char kPath[] = "/vsimem/srtmhgt_cc/out_abort.hgt";
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(driver_->CreateCopy(kPath, &ds_ok, TRUE, nullptr,
                                    AbortImmediatelyProgress, nullptr),
                IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated CreateCopy"));
  }

  // 9. Warnings: non-strict 2-band, non-WGS84 projection, misaligned
  // geotransform, southern/western origin, and mismatched output filename
  // (badname.hgt). Also exercises OpenPAM returning nullptr and the false
  // branch of `if (poDS)`.
  {
    FakeSourceDataset ds_warn(1201, 1201, 2);
    // Non-WGS84 projection
    OGRSpatialReference non_wgs84;
    non_wgs84.SetWellKnownGeogCS("NAD27");
    ds_warn.SetCustomSpatialRef(&non_wgs84);
    // South/West origin (-10 lat, -20 lon) with deliberate coordinate
    // misalignment
    GDALGeoTransform gt{-20.0, 1.0 / 1200.0, 0.0, -9.0, 0.0, -1.0 / 1200.0};
    ds_warn.SetCustomGeoTransform(gt);

    constexpr char kBadPath[] = "/vsimem/srtmhgt_cc/step9/badname.hgt";
    ScopedErrorCollector collector;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> copy_ds(driver_->CreateCopy(
        kBadPath, &ds_warn, FALSE, nullptr, GDALDummyProgress, nullptr));
    // Since badname.hgt is not an SRTMHGT filename, OpenPAM returns nullptr and
    // CreateCopy returns nullptr.
    EXPECT_THAT(copy_ds, IsNull());

    const auto& msgs = collector.messages();
    EXPECT_THAT(msgs, Contains(HasSubstr("only uses the first band")));
    EXPECT_THAT(msgs, Contains(HasSubstr("Only WGS 84 is supported")));
    EXPECT_THAT(msgs, Contains(HasSubstr("not properly aligned")));
    EXPECT_THAT(msgs,
                Contains(HasSubstr("Expected output filename is S10W020.HGT")));
  }
}

// ============================================================================
// CreateCopy: Successful Copy, Nodata Translation, and Clean Read-back
// ============================================================================

TEST_F(SrtmhgtDatasetTest, CreateCopySuccessAndNodataTranslation) {
  FakeSourceDataset src(1201, 1201, 1);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  src.SetCustomSpatialRef(&srs);

  // Aligned GeoTransform for N43W080
  GDALGeoTransform gt{-80.0 - 0.5 / 1200.0, 1.0 / 1200.0, 0.0,
                      44.0 + 0.5 / 1200.0,  0.0,          -1.0 / 1200.0};
  src.SetCustomGeoTransform(gt);

  auto* band = static_cast<FakeSourceBand*>(src.GetRasterBand(1));
  band->SetNoData(true, -9999.0);
  band->SetPixel(0, 0, 1234);
  band->SetPixel(1, 0,
                 -9999);  // Must be translated to kSrtmhgtNoDataValue (-32768)
  band->SetPixel(2, 0, 5678);

  constexpr char kOutPath[] = "/vsimem/srtmhgt_cc/success/N43W080.HGT";
  auto cleanup = absl::MakeCleanup(
      [] { VSIRmdirRecursive("/vsimem/srtmhgt_cc/success"); });

  CPLErrorReset();
  std::unique_ptr<GDALDataset> copied(driver_->CreateCopy(
      kOutPath, &src, TRUE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(copied, NotNull());
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(1201, copied->GetRasterXSize());
  EXPECT_EQ(1201, copied->GetRasterYSize());

  GDALRasterBand* copy_band = copied->GetRasterBand(1);
  ASSERT_THAT(copy_band, NotNull());
  EXPECT_DOUBLE_EQ(kSrtmhgtNoDataValue, copy_band->GetNoDataValue());

  std::vector<int16_t> row(1201, 0);
  ASSERT_EQ(CE_None, copy_band->ReadBlock(0, 0, row.data()));
  EXPECT_EQ(1234, row[0]);
  EXPECT_EQ(kSrtmhgtNoDataValue, row[1]);
  EXPECT_EQ(5678, row[2]);
}

// TODO: b/335317901 - Coverage notes: lines 412 and 419 in srtmhgtdataset.cpp
// (defensive else-branches of N/S and E/W hemisphere character checks) are
// unreachable because Identify already validated them. Float32 (.hgts)
// update-mode round-trip is omitted because it requires a ~52 MB buffer.

}  // namespace
}  // namespace autotest2
