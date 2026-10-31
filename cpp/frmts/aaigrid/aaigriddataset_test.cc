// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests ASCII Arc/Info (AAI), GRASS ASCII, and ISG raster drivers.
//
// See also:
//   https://gdal.org/drivers/raster/aaigrid.html
//   https://gdal.org/drivers/raster/grassasciigrid.html
//   https://gdal.org/drivers/raster/isg.html

#include "third_party/gdal/frmts/aaigrid/aaigriddataset.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::FloatEq;
using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::NotNull;

// RAII wrapper that writes data to a /vsimem/ file and unlinks it on exit.
class VsiMemFile {
 public:
  VsiMemFile(std::string path, std::string_view content)
      : path_(std::move(path)) {
    VSILFILE* fp = VSIFOpenL(path_.c_str(), "wb");
    if (fp != nullptr) {
      if (!content.empty()) {
        VSIFWriteL(content.data(), 1, content.size(), fp);
      }
      VSIFCloseL(fp);
    }
  }
  VsiMemFile(const VsiMemFile&) = delete;
  VsiMemFile& operator=(const VsiMemFile&) = delete;

  ~VsiMemFile() { VSIUnlink(path_.c_str()); }

  const char* path() const { return path_.c_str(); }

 private:
  std::string path_;
};

// RAII wrapper that suppresses GDAL error logging within a scope.
class QuietErrorHandler {
 public:
  QuietErrorHandler() { CPLPushErrorHandler(CPLQuietErrorHandler); }
  QuietErrorHandler(const QuietErrorHandler&) = delete;
  QuietErrorHandler& operator=(const QuietErrorHandler&) = delete;
  ~QuietErrorHandler() { CPLPopErrorHandler(); }
};

// RAII wrapper that captures GDAL warnings/errors even across CPLErrorReset().
class CapturingErrorHandler {
 public:
  CapturingErrorHandler() {
    CPLPushErrorHandlerEx(&CapturingErrorHandler::Callback, this);
  }
  CapturingErrorHandler(const CapturingErrorHandler&) = delete;
  CapturingErrorHandler& operator=(const CapturingErrorHandler&) = delete;
  ~CapturingErrorHandler() { CPLPopErrorHandler(); }

  CPLErr last_type() const { return last_type_; }
  const std::string& last_msg() const { return last_msg_; }

  void Reset() {
    last_type_ = CE_None;
    last_msg_.clear();
  }

 private:
  static void CPL_STDCALL Callback(CPLErr err_class, int /*err_no*/,
                                   const char* msg) {
    auto* self =
        static_cast<CapturingErrorHandler*>(CPLGetErrorHandlerUserData());
    if (self != nullptr) {
      self->last_type_ = err_class;
      self->last_msg_ = msg ? msg : "";
    }
  }

  CPLErr last_type_ = CE_None;
  std::string last_msg_;
};

// RAII wrapper that temporarily sets a GDAL config option and restores it.
class ConfigOptionSaver {
 public:
  ConfigOptionSaver(const char* key, const char* value) : key_(key) {
    const char* prev = CPLGetConfigOption(key, nullptr);
    if (prev != nullptr) {
      had_prev_ = true;
      prev_value_ = prev;
    }
    CPLSetConfigOption(key, value);
  }
  ConfigOptionSaver(const ConfigOptionSaver&) = delete;
  ConfigOptionSaver& operator=(const ConfigOptionSaver&) = delete;

  ~ConfigOptionSaver() {
    CPLSetConfigOption(key_.c_str(), had_prev_ ? prev_value_.c_str() : nullptr);
  }

 private:
  std::string key_;
  bool had_prev_ = false;
  std::string prev_value_;
};

// Custom virtual filesystem handler to test low-level Seek/Write/Close
// failures and non-reopenable write-only targets.
class ControllableVsiHandler final : public VSIFilesystemHandler {
 public:
  class Handle final : public VSIVirtualHandle {
   public:
    Handle(ControllableVsiHandler* handler, bool read_mode)
        : handler_(handler), read_mode_(read_mode) {}

    int Seek(vsi_l_offset offset, int whence) override {
      ++handler_->seek_calls_;
      if (handler_->fail_seek_after_ > 0 &&
          handler_->seek_calls_ > handler_->fail_seek_after_) {
        return -1;
      }
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
      if (!read_mode_ || offset_ >= handler_->buffer_.size()) {
        eof_ = true;
        return 0;
      }
      const size_t total = size * count;
      const size_t avail = handler_->buffer_.size() - offset_;
      const size_t to_copy = total < avail ? total : avail;
      if (to_copy < total) {
        eof_ = true;
      }
      if (to_copy > 0) {
        memcpy(buffer, handler_->buffer_.data() + offset_, to_copy);
        offset_ += to_copy;
      }
      return size > 0 ? to_copy / size : 0;
    }

    size_t Write(const void* buffer, size_t size, size_t count) override {
      ++handler_->write_calls_;
      const size_t total = size * count;
      if (handler_->fail_write_on_call_ > 0 &&
          handler_->write_calls_ == handler_->fail_write_on_call_) {
        return 0;
      }
      const char* chars = static_cast<const char*>(buffer);
      handler_->buffer_.append(chars, total);
      offset_ += total;
      return count;
    }

    void ClearErr() override { eof_ = false; }
    int Eof() override { return eof_ ? 1 : 0; }
    int Error() override { return 0; }

    int Close() override {
      if (handler_->fail_close_) {
        return -1;
      }
      return 0;
    }

   private:
    ControllableVsiHandler* handler_;
    bool read_mode_;
    size_t offset_ = 0;
    bool eof_ = false;
  };

  VSIVirtualHandleUniquePtr Open(const char* filename, const char* access,
                                 bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    const std::string_view fname(filename);
    if (fail_prj_open_ && fname.size() >= 4 &&
        fname.substr(fname.size() - 4) == ".prj") {
      return nullptr;
    }
    const bool is_read = strchr(access, 'r') != nullptr;
    if (is_read && write_only_) {
      return nullptr;
    }
    if (!is_read) {
      buffer_.clear();
    }
    return VSIVirtualHandleUniquePtr(new Handle(this, is_read));
  }

  int Stat(const char* /*pszFilename*/, VSIStatBufL* stat_buf,
           int /*nFlags*/) override {
    if (write_only_) {
      return -1;
    }
    memset(stat_buf, 0, sizeof(*stat_buf));
    stat_buf->st_mode = S_IFREG;
    stat_buf->st_size = buffer_.size();
    return 0;
  }

  void Reset(std::string data = "") {
    buffer_ = std::move(data);
    virtual_size_ = 0;
    seek_calls_ = 0;
    fail_seek_after_ = 0;
    write_calls_ = 0;
    fail_write_on_call_ = 0;
    fail_close_ = false;
    fail_prj_open_ = false;
    write_only_ = false;
  }

  std::string buffer_;
  size_t virtual_size_ = 0;
  int seek_calls_ = 0;
  int fail_seek_after_ = 0;
  int write_calls_ = 0;
  int fail_write_on_call_ = 0;
  bool fail_close_ = false;
  bool fail_prj_open_ = false;
  bool write_only_ = false;
};

ControllableVsiHandler* GetControllableHandler() {
  static ControllableVsiHandler* handler = []() {
    auto* h = new ControllableVsiHandler();
    VSIFileManager::InstallHandler("/vsictrl_aaig/", h);
    return h;
  }();
  return handler;
}

std::string ReadVsiFileText(const char* path) {
  vsi_l_offset len = 0;
  GByte* data = VSIGetMemFileBuffer(path, &len, FALSE);
  if (data == nullptr) {
    return "";
  }
  return std::string(reinterpret_cast<const char*>(data),
                     static_cast<size_t>(len));
}

int AbortImmediatelyProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                             void* /*pProgressArg*/) {
  return FALSE;
}

int AbortAfterFirstLineProgress(double dfComplete, const char* /*pszMessage*/,
                                void* /*pProgressArg*/) {
  return dfComplete > 0.0 ? FALSE : TRUE;
}

// ============================================================================
// Driver registration and metadata tests
// ============================================================================

TEST(AaigridDriverTest, RegisterDriversAndCheckMetadata) {
  // Call registration twice to cover both fresh registration and idempotence.
  GDALRegister_AAIGrid();
  GDALRegister_AAIGrid();
  GDALRegister_GRASSASCIIGrid();
  GDALRegister_GRASSASCIIGrid();
  GDALRegister_ISG();
  GDALRegister_ISG();

  GDALDriver* aaig = GetGDALDriverManager()->GetDriverByName("AAIGrid");
  ASSERT_THAT(aaig, NotNull());
  EXPECT_STREQ("Arc/Info ASCII Grid", aaig->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("asc", aaig->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("YES", aaig->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_THAT(aaig->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST),
              HasSubstr("FORCE_CELLSIZE"));
  EXPECT_THAT(aaig->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST),
              HasSubstr("DATATYPE"));

  GDALDriver* grass = GetGDALDriverManager()->GetDriverByName("GRASSASCIIGrid");
  ASSERT_THAT(grass, NotNull());
  EXPECT_STREQ("GRASS ASCII Grid", grass->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("YES", grass->GetMetadataItem(GDAL_DCAP_VIRTUALIO));

  GDALDriver* isg = GetGDALDriverManager()->GetDriverByName("ISG");
  ASSERT_THAT(isg, NotNull());
  EXPECT_STREQ("International Service for the Geoid",
               isg->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("isg", isg->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("YES", isg->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
}

// ============================================================================
// AAIGDataset Identify & Open tests
// ============================================================================

TEST(AaigridDatasetTest, IdentifyDoesNotExistAndTooShortAndUnknown) {
  auto missing = std::make_unique<GDALOpenInfo>("/vsimem/does_not_exist.asc",
                                                GDAL_OF_READONLY, nullptr);
  EXPECT_FALSE(AAIGDataset::Identify(missing.get()));
  EXPECT_THAT(AAIGDataset::Open(missing.get()), IsNull());

  // Header < 40 bytes must fail Identify.
  VsiMemFile short_file("/vsimem/aaig_short.asc", "ncols 1\nnrows 1\n1\n");
  auto short_info = std::make_unique<GDALOpenInfo>(short_file.path(),
                                                   GDAL_OF_READONLY, nullptr);
  EXPECT_FALSE(AAIGDataset::Identify(short_info.get()));
  EXPECT_THAT(AAIGDataset::Open(short_info.get()), IsNull());

  // Header >= 40 bytes with unrecognized first token must fail Identify.
  VsiMemFile unknown_file("/vsimem/aaig_unknown.asc",
                          "foobar 10\nnrows 10\ncellsize 1\nxllcorner 0\n"
                          "yllcorner 0\n1 2 3\n");
  auto unknown_info = std::make_unique<GDALOpenInfo>(unknown_file.path(),
                                                     GDAL_OF_READONLY, nullptr);
  EXPECT_FALSE(AAIGDataset::Identify(unknown_info.get()));
  EXPECT_THAT(AAIGDataset::Open(unknown_info.get()), IsNull());
}

TEST(AaigridDatasetTest, IdentifyAllSupportedStartingKeywords) {
  const char* kKeywords[] = {
      "ncols",     "NROWS", "XllCorner", "yllcorner", "xllcenter",
      "YLLCENTER", "dx",    "DY",        "CellSize",
  };
  for (const char* kw : kKeywords) {
    const std::string content =
        absl::StrCat(kw, " 2\npadding_line_for_forty_bytes_minimum\n1 2\n");
    VsiMemFile mem("/vsimem/aaig_kw.asc", content);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    EXPECT_TRUE(AAIGDataset::Identify(open_info.get()))
        << "Keyword failed: " << kw;
  }
}

TEST(AaigridDatasetTest, OpenFailsWhenFpLIsNull) {
  // Construct GDALOpenInfo with in-memory header bytes and no open file handle.
  const std::string header =
      "ncols 2\nnrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n3 4\n";
  GDALOpenInfo open_info("/vsimem/no_fpl.asc", GDAL_OF_READONLY);
  open_info.nHeaderBytes = static_cast<int>(header.size());
  open_info.pabyHeader =
      reinterpret_cast<GByte*>(const_cast<char*>(header.data()));
  open_info.fpL = nullptr;

  EXPECT_TRUE(AAIGDataset::Identify(&open_info));
  EXPECT_THAT(AAIGDataset::Open(&open_info), IsNull());
  open_info.pabyHeader = nullptr;
}

TEST(AaigridDatasetTest, ParseHeaderMissingOrInvalidFields) {
  QuietErrorHandler quiet;

  const char* kBadHeaders[] = {
      // Missing ncols value at end of header.
      "nrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\nncols",
      // Started with nrows, missing ncols completely.
      "nrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n",
      // Missing nrows completely.
      "ncols 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n",
      // nrows at end with no value.
      "ncols 2\ncellsize 1\nxllcorner 0\nyllcorner 0\nnrows",
      // Invalid dimensions (0 and negative and overflow).
      "ncols 0\nnrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n",
      "ncols 2\nnrows -2\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n",
      "ncols 100000\nnrows 100000\ncellsize 1\nxllcorner 0\nyllcorner 0\n1\n",
      // Missing cellsize and dx/dy.
      "ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\n1 2\n3 4\n",
      // cellsize at end with no value.
      "ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize",
      // dx present without dy.
      "ncols 2\nnrows 2\ndx 1.0\nxllcorner 0\nyllcorner 0\n1 2\n3 4\n",
      // dy at end with no value.
      "ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\ndx 1.0\ndy",
      // Header valid, but no data values before NUL terminator.
      "ncols 2\nnrows 2\ncellsize 1.0\nxllcorner 0.0\nyllcorner 0.0\n",
  };

  for (const char* content : kBadHeaders) {
    VsiMemFile mem("/vsimem/aaig_bad_hdr.asc", content);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    EXPECT_THAT(AAIGDataset::Open(open_info.get()), IsNull())
        << "Expected Open failure for:\n"
        << content;
  }
}

TEST(AaigridDatasetTest, GeoreferencingCornerCenterDefaultAndCcafsHack) {
  // 1. xllcorner / yllcorner with cellsize.
  {
    VsiMemFile mem("/vsimem/aaig_corner.asc",
                   "ncols 3\nnrows 2\nxllcorner 100.0\nyllcorner 200.0\n"
                   "cellsize 10.0\n1 2 3\n4 5 6\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(100.0, gt[0]);
    EXPECT_DOUBLE_EQ(10.0, gt[1]);
    EXPECT_DOUBLE_EQ(0.0, gt[2]);
    EXPECT_DOUBLE_EQ(220.0, gt[3]);
    EXPECT_DOUBLE_EQ(0.0, gt[4]);
    EXPECT_DOUBLE_EQ(-10.0, gt[5]);
    EXPECT_THAT(ds->GetMetadataItem(GDALMD_AREA_OR_POINT), IsNull());
    EXPECT_THAT(ds->GetSpatialRef(), IsNull());
  }

  // 2. xllcenter / yllcenter with dx and dy.
  {
    VsiMemFile mem("/vsimem/aaig_center.asc",
                   "ncols 2\nnrows 2\nxllcenter 10.0\nyllcenter 20.0\n"
                   "dx 2.0\ndy 4.0\n1 2\n3 4\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(9.0, gt[0]);
    EXPECT_DOUBLE_EQ(2.0, gt[1]);
    EXPECT_DOUBLE_EQ(0.0, gt[2]);
    EXPECT_DOUBLE_EQ(26.0, gt[3]);
    EXPECT_DOUBLE_EQ(0.0, gt[4]);
    EXPECT_DOUBLE_EQ(-4.0, gt[5]);
    EXPECT_STREQ(GDALMD_AOP_POINT, ds->GetMetadataItem(GDALMD_AREA_OR_POINT));
  }

  // 3. No xllcorner/yllcorner or xllcenter/yllcenter -> defaults to origin 0,0.
  {
    VsiMemFile mem("/vsimem/aaig_no_origin.asc",
                   "ncols 2\nnrows 2\ncellsize 5.0\n"
                   "                    \n1 2\n3 4\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(0.0, gt[0]);
    EXPECT_DOUBLE_EQ(5.0, gt[1]);
    EXPECT_DOUBLE_EQ(0.0, gt[3]);
    EXPECT_DOUBLE_EQ(-5.0, gt[5]);
  }

  // 4. CCAFS climate precision hack (ncols % 360 == 0, xllcorner == -180,
  // cellsize within 1e-9 of 360 / ncols).
  {
    std::string data =
        "ncols 360\nnrows 1\nxllcorner -180.0\nyllcorner -90.0\n"
        "cellsize 1.0000000001\n";
    for (int i = 0; i < 360; ++i) {
      absl::StrAppend(&data, "1 ");
    }
    VsiMemFile mem("/vsimem/aaig_ccafs.asc", data);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(-180.0, gt[0]);
    EXPECT_DOUBLE_EQ(1.0, gt[1]);
    EXPECT_DOUBLE_EQ(-89.0, gt[3]);
    EXPECT_DOUBLE_EQ(-1.0, gt[5]);
  }
}

TEST(AaigridDatasetTest, DataTypesNoDataAndConfigAndOpenOptions) {
  QuietErrorHandler quiet;

  // 1. Int32 without NODATA_value, then SetNoDataValue.
  {
    VsiMemFile mem("/vsimem/aaig_int.asc",
                   "ncols 2\nnrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
                   "10 -20\n30 40");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());
    EXPECT_EQ(GDT_Int32, band->GetRasterDataType());

    int has_nodata = TRUE;
    EXPECT_DOUBLE_EQ(-9999.0, band->GetNoDataValue(&has_nodata));
    EXPECT_FALSE(has_nodata);
    EXPECT_DOUBLE_EQ(-9999.0, band->GetNoDataValue(nullptr));

    EXPECT_EQ(CE_None, band->SetNoDataValue(-555.0));
    EXPECT_DOUBLE_EQ(-555.0, band->GetNoDataValue(&has_nodata));
    EXPECT_TRUE(has_nodata);

    int32_t pixels[4] = {};
    ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, pixels, 2, 2,
                                      GDT_Int32, 0, 0, nullptr));
    EXPECT_THAT(pixels, ElementsAre(10, -20, 30, 40));
  }

  // 2. Float32 auto-detected via '.', ',', 'e', 'E', and >100KB second chunk.
  {
    std::string big_int_prefix(1024 * 100 + 16, ' ');
    const std::string content =
        absl::StrCat("ncols 2\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n",
                     big_int_prefix, "1 2.5\n");
    VsiMemFile mem("/vsimem/aaig_big_float.asc", content);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 3. NODATA_value variants: comma, NaN, inf, -inf, out of int32 range,
  // very small (< FLT_MIN -> Float64), very large (> FLT_MAX -> Float64),
  // and clamping to FLT_MAX / -FLT_MAX when DATATYPE=Float32.
  {
    VsiMemFile mem_comma(
        "/vsimem/aaig_nd_comma.asc",
        "ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value -99,5\n1\n");
    auto oi_comma = std::make_unique<GDALOpenInfo>(mem_comma.path(),
                                                   GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_comma =
        absl::WrapUnique(AAIGDataset::Open(oi_comma.get()));
    ASSERT_THAT(ds_comma, NotNull());
    EXPECT_EQ(GDT_Float32, ds_comma->GetRasterBand(1)->GetRasterDataType());
    EXPECT_FLOAT_EQ(-99.5f,
                    static_cast<float>(
                        ds_comma->GetRasterBand(1)->GetNoDataValue(nullptr)));

    VsiMemFile mem_nan(
        "/vsimem/aaig_nd_nan.asc",
        "ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value NaN\n1\n");
    auto oi_nan = std::make_unique<GDALOpenInfo>(mem_nan.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_nan =
        absl::WrapUnique(AAIGDataset::Open(oi_nan.get()));
    ASSERT_THAT(ds_nan, NotNull());
    EXPECT_EQ(GDT_Float32, ds_nan->GetRasterBand(1)->GetRasterDataType());
    EXPECT_TRUE(std::isnan(ds_nan->GetRasterBand(1)->GetNoDataValue(nullptr)));

    VsiMemFile mem_inf(
        "/vsimem/aaig_nd_inf.asc",
        "ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value inf\n1\n");
    auto oi_inf = std::make_unique<GDALOpenInfo>(mem_inf.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_inf =
        absl::WrapUnique(AAIGDataset::Open(oi_inf.get()));
    ASSERT_THAT(ds_inf, NotNull());
    EXPECT_TRUE(std::isinf(ds_inf->GetRasterBand(1)->GetNoDataValue(nullptr)));

    VsiMemFile mem_tiny(
        "/vsimem/aaig_nd_tiny.asc",
        "ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value 1.0e-45\n1\n");
    auto oi_tiny = std::make_unique<GDALOpenInfo>(mem_tiny.path(),
                                                  GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_tiny =
        absl::WrapUnique(AAIGDataset::Open(oi_tiny.get()));
    ASSERT_THAT(ds_tiny, NotNull());
    EXPECT_EQ(GDT_Float64, ds_tiny->GetRasterBand(1)->GetRasterDataType());

    // Huge NODATA_value (1e100) with DATATYPE=Float32 clamps to FLT_MAX,
    // and -1e100 clamps to -FLT_MAX; pixel values 1e300 / -1e300 clamp via
    // DoubleToFloatClamp.
    VsiMemFile mem_clamp(
        "/vsimem/aaig_clamp.asc",
        "ncols 2\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value 1e100\n1e300 -1e300\n");
    char** open_opts = CSLAddString(nullptr, "DATATYPE=Float32");
    auto clean_opts = absl::MakeCleanup([&] { CSLDestroy(open_opts); });
    auto oi_clamp = std::make_unique<GDALOpenInfo>(mem_clamp.path(),
                                                   GDAL_OF_READONLY, nullptr);
    oi_clamp->papszOpenOptions = open_opts;
    std::unique_ptr<GDALDataset> ds_clamp =
        absl::WrapUnique(AAIGDataset::Open(oi_clamp.get()));
    oi_clamp->papszOpenOptions = nullptr;
    ASSERT_THAT(ds_clamp, NotNull());
    EXPECT_EQ(GDT_Float32, ds_clamp->GetRasterBand(1)->GetRasterDataType());
    EXPECT_DOUBLE_EQ(std::numeric_limits<float>::max(),
                     ds_clamp->GetRasterBand(1)->GetNoDataValue(nullptr));
    float fvals[2] = {};
    ASSERT_EQ(CE_None, ds_clamp->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 1, fvals, 2, 1, GDT_Float32, 0, 0,
                           nullptr));
    EXPECT_FLOAT_EQ(std::numeric_limits<float>::max(), fvals[0]);
    EXPECT_FLOAT_EQ(std::numeric_limits<float>::lowest(), fvals[1]);

    VsiMemFile mem_neg_clamp(
        "/vsimem/aaig_neg_clamp.asc",
        "ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value -1e100\n1.5\n");
    auto oi_neg = std::make_unique<GDALOpenInfo>(mem_neg_clamp.path(),
                                                 GDAL_OF_READONLY, nullptr);
    oi_neg->papszOpenOptions = open_opts;
    std::unique_ptr<GDALDataset> ds_neg =
        absl::WrapUnique(AAIGDataset::Open(oi_neg.get()));
    oi_neg->papszOpenOptions = nullptr;
    ASSERT_THAT(ds_neg, NotNull());
    EXPECT_DOUBLE_EQ(-std::numeric_limits<float>::max(),
                     ds_neg->GetRasterBand(1)->GetNoDataValue(nullptr));
  }

  // 4. "null" NODATA_value and "null" / "nan" pixel tokens in Float32 &
  // Float64.
  {
    VsiMemFile mem_null(
        "/vsimem/aaig_null.asc",
        "ncols 3\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
        "NODATA_value null\nnull nan 42.5\n");
    auto oi_f32 = std::make_unique<GDALOpenInfo>(mem_null.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_f32 =
        absl::WrapUnique(AAIGDataset::Open(oi_f32.get()));
    ASSERT_THAT(ds_f32, NotNull());
    EXPECT_EQ(GDT_Float32, ds_f32->GetRasterBand(1)->GetRasterDataType());
    EXPECT_DOUBLE_EQ(-std::numeric_limits<float>::max(),
                     ds_f32->GetRasterBand(1)->GetNoDataValue(nullptr));
    float f32[3] = {};
    ASSERT_EQ(CE_None,
              ds_f32->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 3, 1, f32, 3, 1,
                                                 GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(-std::numeric_limits<float>::max(), f32[0]);
    EXPECT_TRUE(std::isnan(f32[1]));
    EXPECT_FLOAT_EQ(42.5f, f32[2]);

    ConfigOptionSaver save_dt("AAIGRID_DATATYPE", "Float64");
    auto oi_f64 = std::make_unique<GDALOpenInfo>(mem_null.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_f64 =
        absl::WrapUnique(AAIGDataset::Open(oi_f64.get()));
    ASSERT_THAT(ds_f64, NotNull());
    EXPECT_EQ(GDT_Float64, ds_f64->GetRasterBand(1)->GetRasterDataType());
    EXPECT_DOUBLE_EQ(-std::numeric_limits<double>::max(),
                     ds_f64->GetRasterBand(1)->GetNoDataValue(nullptr));
    double f64[3] = {};
    ASSERT_EQ(CE_None,
              ds_f64->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 3, 1, f64, 3, 1,
                                                 GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(-std::numeric_limits<double>::max(), f64[0]);
    EXPECT_TRUE(std::isnan(f64[1]));
    EXPECT_DOUBLE_EQ(42.5, f64[2]);
  }

  // 5. Unsupported DATATYPE open option emits warning and falls back to Int32.
  {
    VsiMemFile mem("/vsimem/aaig_bad_dt.asc",
                   "ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
                   "7\n");
    char** open_opts = CSLAddString(nullptr, "DATATYPE=Byte");
    auto clean_opts = absl::MakeCleanup([&] { CSLDestroy(open_opts); });
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    oi->papszOpenOptions = open_opts;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(oi.get()));
    oi->papszOpenOptions = nullptr;
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(GDT_Int32, ds->GetRasterBand(1)->GetRasterDataType());
  }
}

TEST(AaigridDatasetTest, BlockReadingBufferRefillAndReadErrors) {
  QuietErrorHandler quiet;

  // 1. Multi-row raster > 300 bytes of pixel data to exercise Getc() 256-byte
  // buffer refill, out-of-order block reading (skipping rows with pImage ==
  // nullptr), and invalid ReadBlock offsets.
  {
    std::string content =
        "ncols 40\nnrows 4\ncellsize 1\nxllcorner 0\nyllcorner 0\n";
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 40; ++c) {
        absl::StrAppend(&content, r * 100 + c, (r == 3 && c == 39) ? "" : " ");
      }
      if (r < 3) {
        content.push_back('\n');
      }
    }
    VsiMemFile mem("/vsimem/aaig_large_buf.asc", content);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());

    // Invalid block offsets.
    std::vector<int32_t> row(40);
    EXPECT_EQ(CE_Failure, band->ReadBlock(-1, 0, row.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(1, 0, row.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(0, -1, row.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(0, 4, row.data()));

    // Read row 3 first -> forces recursive skip of rows 0, 1, 2.
    ASSERT_EQ(CE_None, band->ReadBlock(0, 3, row.data()));
    EXPECT_EQ(300, row[0]);
    EXPECT_EQ(339, row[39]);

    // Read row 1 afterwards.
    ASSERT_EQ(CE_None, band->ReadBlock(0, 1, row.data()));
    EXPECT_EQ(100, row[0]);
    EXPECT_EQ(139, row[39]);
  }

  // 2. Truncated file: sequential read fails, and out-of-order read of row 2
  // fails when row 0 is short.
  {
    VsiMemFile mem("/vsimem/aaig_short_data.asc",
                   "ncols 3\nnrows 3\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
                   "1 2\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    int32_t row[3] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 2, row));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("File short"));
  }

  // 3. Token too long (>= 499 characters in a single pixel token).
  {
    const std::string long_token(500, '1');
    const std::string content =
        absl::StrCat("ncols 1\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n",
                     long_token, "\n");
    VsiMemFile mem("/vsimem/aaig_long_tok.asc", content);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    int32_t val = 0;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 0, &val));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Token too long"));
  }

  // 4. Huge raster dimensions compared to small file size.
  {
    VsiMemFile mem("/vsimem/aaig_huge_dims.asc",
                   "ncols 10000001\nnrows 1\ncellsize 1\n"
                   "xllcorner 0\nyllcorner 0\n1 2\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    CPLErrorReset();
    EXPECT_THAT(AAIGDataset::Open(open_info.get()), IsNull());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too large raster dimension"));
  }

  // 5. Low-level Seek failure in IReadBlock and Close failure in ~AAIGDataset.
  {
    ControllableVsiHandler* handler = GetControllableHandler();
    handler->Reset(
        "ncols 2\nnrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n3 4\n");
    auto open_info = std::make_unique<GDALOpenInfo>(
        "/vsictrl_aaig/seek_fail.asc", GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    auto* aaig_band = static_cast<AAIGRasterBand*>(ds->GetRasterBand(1));
    EXPECT_EQ(CE_Failure, aaig_band->IReadBlock(0, -1, nullptr));
    EXPECT_EQ(CE_Failure, aaig_band->IReadBlock(1, 0, nullptr));
    // Fail the next Seek() inside IReadBlock().
    handler->fail_seek_after_ = handler->seek_calls_;
    int32_t row[2] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 0, row));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Can't seek to offset"));

    // Fail Close() in ~AAIGDataset().
    handler->fail_close_ = true;
    CPLErrorReset();
    ds.reset();
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    handler->Reset();
  }

  // 6. Large-dimension file size check passes (VSIFSeekL to 0), then
  // VSIFSeekL(nStartOfData) fails during data-type scan.
  {
    ControllableVsiHandler* handler = GetControllableHandler();
    handler->Reset(
        "ncols 10000001\nnrows 1\ncellsize 1\nxllcorner 0\nyllcorner 0\n1 2\n");
    handler->virtual_size_ = 25000000;
    // Allow SEEK_END (1) and SEEK_SET(0) (2), fail SEEK_SET(nStartOfData) (3).
    handler->fail_seek_after_ = 2;
    auto open_info = std::make_unique<GDALOpenInfo>(
        "/vsictrl_aaig/seek_start_fail.asc", GDAL_OF_READONLY, nullptr);
    EXPECT_THAT(AAIGDataset::Open(open_info.get()), IsNull());
    handler->Reset();
  }
}

TEST(AaigridDatasetTest, ProjectionFilesLowercaseUppercaseAndArcSeconds) {
  // 1. Lowercase .prj file with projected WKT.
  {
    VsiMemFile asc("/vsimem/aaig_prj_test.asc",
                   "ncols 2\nnrows 2\ncellsize 10\nxllcorner 0\nyllcorner 0\n"
                   "1 2\n3 4\n");
    VsiMemFile prj(
        "/vsimem/aaig_prj_test.prj",
        "PROJCS[\"WGS 84 / UTM zone 10N\","
        "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
        "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
        "PROJECTION[\"Transverse_Mercator\"],"
        "PARAMETER[\"latitude_of_origin\",0],"
        "PARAMETER[\"central_meridian\",-123],"
        "PARAMETER[\"scale_factor\",0.9996],"
        "PARAMETER[\"false_easting\",500000],"
        "PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(asc.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    EXPECT_THAT(ds->GetSpatialRef(), NotNull());

    char** file_list = ds->GetFileList();
    auto clean_list = absl::MakeCleanup([&] { CSLDestroy(file_list); });
    EXPECT_EQ(2, CSLCount(file_list));
    EXPECT_NE(-1, CSLFindString(file_list, asc.path()));
    EXPECT_NE(-1, CSLFindString(file_list, prj.path()));
  }

  // 2. Uppercase .PRJ file on case-sensitive /vsimem/ with ESRI geographic
  // projection in arc-seconds (Units DS -> divides geotransform by 3600).
  {
    VsiMemFile asc("/vsimem/aaig_upper.ASC",
                   "ncols 2\nnrows 2\ncellsize 3600\n"
                   "xllcorner 36000\nyllcorner 72000\n1 2\n3 4\n");
    VsiMemFile prj("/vsimem/aaig_upper.PRJ",
                   "Projection    GEOGRAPHIC\n"
                   "Datum         NAD83\n"
                   "Spheroid      GRS80\n"
                   "Units         DS\n"
                   "Parameters\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(asc.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    ASSERT_THAT(ds->GetSpatialRef(), NotNull());
    EXPECT_TRUE(ds->GetSpatialRef()->IsGeographic());

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_DOUBLE_EQ(1.0, gt[1]);
    EXPECT_DOUBLE_EQ(22.0, gt[3]);
    EXPECT_DOUBLE_EQ(-1.0, gt[5]);
  }

  // 3. ESRI geographic .prj with bare "Units" token (no value) and without
  // "Units" keyword to cover all OSR_GDS branches.
  {
    VsiMemFile asc("/vsimem/aaig_bare_units.asc",
                   "ncols 2\nnrows 2\ncellsize 1\nxllcorner 0\nyllcorner 0\n"
                   "1 2\n3 4\n");
    VsiMemFile prj("/vsimem/aaig_bare_units.prj",
                   "Projection    GEOGRAPHIC\n"
                   "Datum         NAD83\n"
                   "Units\n"
                   "Parameters\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(asc.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(AAIGDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    ASSERT_THAT(ds->GetSpatialRef(), NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(1.0, gt[1]);
  }
}

// ============================================================================
// GRASSASCIIDataset tests
// ============================================================================

TEST(GrassAsciiDatasetTest, IdentifyKeywordsAndInvalid) {
  auto missing = std::make_unique<GDALOpenInfo>("/vsimem/does_not_exist.grass",
                                                GDAL_OF_READONLY, nullptr);
  EXPECT_FALSE(GRASSASCIIDataset::Identify(missing.get()));
  EXPECT_THAT(GRASSASCIIDataset::Open(missing.get()), IsNull());

  const char* kKeywords[] = {
      "north:", "SOUTH:", "East:", "west:", "rows:", "COLS:",
  };
  for (const char* kw : kKeywords) {
    const std::string content =
        absl::StrCat(kw, " 10\npadding_for_forty_byte_minimum_header\n1\n");
    VsiMemFile mem("/vsimem/grass_kw.txt", content);
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    EXPECT_TRUE(GRASSASCIIDataset::Identify(open_info.get()))
        << "Keyword failed: " << kw;
  }

  VsiMemFile bad("/vsimem/grass_bad.txt",
                 "unknown: 10\npadding_for_forty_byte_minimum_header\n1\n");
  auto bad_info =
      std::make_unique<GDALOpenInfo>(bad.path(), GDAL_OF_READONLY, nullptr);
  EXPECT_FALSE(GRASSASCIIDataset::Identify(bad_info.get()));
  EXPECT_THAT(GRASSASCIIDataset::Open(bad_info.get()), IsNull());
}

TEST(GrassAsciiDatasetTest, OpenValidTypesNullAndParseErrors) {
  QuietErrorHandler quiet;

  // 1. Valid GRASS ASCII grid with null float promotion and type: double.
  {
    VsiMemFile mem("/vsimem/grass_valid.txt",
                   "north: 50.0\n"
                   "south: 30.0\n"
                   "east:  20.0\n"
                   "west:  10.0\n"
                   "rows:  2\n"
                   "cols:  2\n"
                   "null:  -999.5\n"
                   "type:  double\n"
                   "1.25 -999.5\n"
                   "3.75 4.5\n");
    auto open_info =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(GRASSASCIIDataset::Open(open_info.get()));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(2, ds->GetRasterXSize());
    EXPECT_EQ(2, ds->GetRasterYSize());

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_DOUBLE_EQ(5.0, gt[1]);
    EXPECT_DOUBLE_EQ(50.0, gt[3]);
    EXPECT_DOUBLE_EQ(-10.0, gt[5]);

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());
    EXPECT_EQ(GDT_Float64, band->GetRasterDataType());
    int has_nodata = FALSE;
    EXPECT_DOUBLE_EQ(-999.5, band->GetNoDataValue(&has_nodata));
    EXPECT_TRUE(has_nodata);

    double vals[4] = {};
    ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, vals, 2, 2,
                                      GDT_Float64, 0, 0, nullptr));
    EXPECT_THAT(vals, ElementsAre(DoubleEq(1.25), DoubleEq(-999.5),
                                  DoubleEq(3.75), DoubleEq(4.5)));
  }

  // 2. type: int, type: float, and invalid type warning + config option.
  {
    VsiMemFile mem_int(
        "/vsimem/grass_int.txt",
        "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 1\ncols: 2\n"
        "type: int\n10 20\n");
    auto oi_int = std::make_unique<GDALOpenInfo>(mem_int.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_int =
        absl::WrapUnique(GRASSASCIIDataset::Open(oi_int.get()));
    ASSERT_THAT(ds_int, NotNull());
    EXPECT_EQ(GDT_Int32, ds_int->GetRasterBand(1)->GetRasterDataType());

    VsiMemFile mem_flt(
        "/vsimem/grass_flt.txt",
        "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 1\ncols: 2\n"
        "type: float\n10 20\n");
    auto oi_flt = std::make_unique<GDALOpenInfo>(mem_flt.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_flt =
        absl::WrapUnique(GRASSASCIIDataset::Open(oi_flt.get()));
    ASSERT_THAT(ds_flt, NotNull());
    EXPECT_EQ(GDT_Float32, ds_flt->GetRasterBand(1)->GetRasterDataType());

    VsiMemFile mem_bad_type(
        "/vsimem/grass_bad_type.txt",
        "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 1\ncols: 2\n"
        "null: -999.25\ntype: bogus\n10 20\n");
    CPLErrorReset();
    auto oi_bad_type = std::make_unique<GDALOpenInfo>(
        mem_bad_type.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_bad_type =
        absl::WrapUnique(GRASSASCIIDataset::Open(oi_bad_type.get()));
    ASSERT_THAT(ds_bad_type, NotNull());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(GDT_Float32, ds_bad_type->GetRasterBand(1)->GetRasterDataType());

    VsiMemFile mem_untyped(
        "/vsimem/grass_untyped.txt",
        "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 1\ncols: 2\n"
        "10 20\n");
    ConfigOptionSaver save_grass_dt("GRASSASCIIGRID_DATATYPE", "Float64");
    auto oi_cfg = std::make_unique<GDALOpenInfo>(mem_untyped.path(),
                                                 GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds_cfg =
        absl::WrapUnique(GRASSASCIIDataset::Open(oi_cfg.get()));
    ASSERT_THAT(ds_cfg, NotNull());
    EXPECT_EQ(GDT_Float64, ds_cfg->GetRasterBand(1)->GetRasterDataType());
  }

  // 2b. GRASS null values with comma, NaN, and out-of-int32-range promote to
  // Float32 when no type: or config option is given.
  {
    const char* kFloatNulls[] = {"-999,5", "NaN", "3000000000"};
    for (const char* null_tok : kFloatNulls) {
      const std::string content = absl::StrCat(
          "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 1\ncols: 2\nnull: ",
          null_tok, "\n10 20\n");
      VsiMemFile mem("/vsimem/grass_null_promote.txt", content);
      auto oi =
          std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
      std::unique_ptr<GDALDataset> ds =
          absl::WrapUnique(GRASSASCIIDataset::Open(oi.get()));
      ASSERT_THAT(ds, NotNull()) << "Failed for null: " << null_tok;
      EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());
    }
  }

  // 3. Invalid GRASS headers.
  const char* kBadGrass[] = {
      "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 1\n1 2\n",
      "north: 10\nsouth: 0\neast: 10\nwest: 0\ncols: 1\n1 2\n",
      "north: 10\nsouth: 0\neast: 10\nwest: 0\nrows: 0\ncols: 1\n1\n",
      "north: 10\nsouth: 0\neast: 10\nrows: 1\ncols: 1\n1\n",
  };
  for (const char* content : kBadGrass) {
    VsiMemFile mem("/vsimem/grass_err.txt", content);
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    EXPECT_THAT(GRASSASCIIDataset::Open(oi.get()), IsNull());
  }
}

// ============================================================================
// ISGDataset tests
// ============================================================================

TEST(IsgDatasetTest, IdentifyKeywordsAndLargeHeaderIngest) {
  auto missing = std::make_unique<GDALOpenInfo>("/vsimem/does_not_exist.isg",
                                                GDAL_OF_READONLY, nullptr);
  EXPECT_FALSE(ISGDataset::Identify(missing.get()));
  EXPECT_THAT(ISGDataset::Open(missing.get()), IsNull());

  // Large header > 1024 bytes before lat min/max/lon min/max/nrows/ncols
  // triggers TryToIngest(8192) inside ISGDataset::Identify.
  std::string large_header = "model name : TestGeoid\n";
  for (int i = 0; i < 30; ++i) {
    absl::StrAppend(&large_header,
                    "# Comment line padding header past 1024 bytes\n");
  }
  absl::StrAppend(&large_header,
                  "lat min = 10.0\n"
                  "lat max = 12.0\n"
                  "lon min = 20.0\n"
                  "lon max = 22.0\n"
                  "delta lat = 1.0\n"
                  "delta lon = 1.0\n"
                  "nrows = 2\n"
                  "ncols = 2\n"
                  "end_of_head\n"
                  "1.0 2.0\n3.0 4.0\n");
  VsiMemFile mem("/vsimem/isg_large_hdr.isg", large_header);
  auto open_info =
      std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
  EXPECT_TRUE(ISGDataset::Identify(open_info.get()));
  std::unique_ptr<GDALDataset> ds =
      absl::WrapUnique(ISGDataset::Open(open_info.get()));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(2, ds->GetRasterXSize());
  EXPECT_EQ(2, ds->GetRasterYSize());

  // Header already ingested >= 8192 bytes without nrows/ncols still identifies
  // if "model name" is present.
  std::string huge_comment_header = "model name : TestGeoid\n";
  huge_comment_header.append(9000, '#');
  VsiMemFile mem_huge("/vsimem/isg_huge_comment.isg", huge_comment_header);
  auto oi_huge = std::make_unique<GDALOpenInfo>(mem_huge.path(),
                                                GDAL_OF_READONLY, nullptr);
  oi_huge->TryToIngest(8192);
  EXPECT_TRUE(ISGDataset::Identify(oi_huge.get()));
}

TEST(IsgDatasetTest, Version1Version2DmsUnitsAndRounding) {
  QuietErrorHandler quiet;

  // 1. ISG 1.0 with metadata, units, nodata, and Float32 pixels.
  {
    VsiMemFile mem("/vsimem/isg_v1.isg",
                   "model name : GEOID_TEST\n"
                   "model type : gravimetric\n"
                   "units      : meters\n"
                   "ISG format = 1.0\n"
                   "data format = grid\n"
                   "data ordering = N-to-S, W-to-E\n"
                   "coord type = geodetic\n"
                   "coord units = deg\n"
                   "lat min = 40.0\n"
                   "lat max = 42.0\n"
                   "lon min = -10.0\n"
                   "lon max = -8.0\n"
                   "delta lat = 1.0\n"
                   "delta lon = 1.0\n"
                   "nrows = 2\n"
                   "ncols = 2\n"
                   "nodata = -9999.0\n"
                   "end_of_head\n"
                   "1.5 -9999.0\n"
                   "2.5 3.5\n");
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi.get()));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("GEOID_TEST", ds->GetMetadataItem("MODEL_NAME"));
    EXPECT_STREQ("gravimetric", ds->GetMetadataItem("MODEL_TYPE"));
    ASSERT_THAT(ds->GetSpatialRef(), NotNull());
    EXPECT_TRUE(ds->GetSpatialRef()->IsGeographic());

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_THAT(band, NotNull());
    EXPECT_STREQ("meters", band->GetUnitType());
    EXPECT_EQ(GDT_Float32, band->GetRasterDataType());
    int has_nodata = FALSE;
    EXPECT_DOUBLE_EQ(-9999.0, band->GetNoDataValue(&has_nodata));
    EXPECT_TRUE(has_nodata);

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(-10.0, gt[0]);
    EXPECT_DOUBLE_EQ(1.0, gt[1]);
    EXPECT_DOUBLE_EQ(42.0, gt[3]);
    EXPECT_DOUBLE_EQ(-1.0, gt[5]);

    float vals[4] = {};
    ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, vals, 2, 2,
                                      GDT_Float32, 0, 0, nullptr));
    EXPECT_THAT(vals, ElementsAre(FloatEq(1.5f), FloatEq(-9999.0f),
                                  FloatEq(2.5f), FloatEq(3.5f)));
  }

  // 2. ISG 2.0 with DMS coordinates (degree symbol '\xc2\xb0') and data units.
  {
    VsiMemFile mem("/vsimem/isg_v2_dms.isg",
                   "model name : GEOID_V2\r\n"
                   "data units : mm\r\n"
                   "ISG format = 2.0\r\n"
                   "coord units = dms\r\n"
                   "lat min = 10\xc2\xb0"
                   "30'00\"\r\n"
                   "lat max = 11\xc2\xb0"
                   "30'00\"\r\n"
                   "lon min = 20\xc2\xb0"
                   "00'00\"\r\n"
                   "lon max = 21\xc2\xb0"
                   "00'00\"\r\n"
                   "delta lat = 1\xc2\xb0"
                   "00'00\"\r\n"
                   "delta lon = 1\xc2\xb0"
                   "00'00\"\r\n"
                   "nrows = 2\r\n"
                   "ncols = 2\r\n"
                   "end_of_head\r\n"
                   "1.0 2.0\r\n3.0 4.0\r\n");
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi.get()));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("mm", ds->GetRasterBand(1)->GetUnitType());

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    // ISG 2.0 adjusts min/max outward by delta / 2.
    EXPECT_THAT(gt[0], DoubleNear(19.5, 1e-11));
    EXPECT_THAT(gt[1], DoubleNear(1.0, 1e-11));
    EXPECT_THAT(gt[3], DoubleNear(12.0, 1e-11));
    EXPECT_THAT(gt[5], DoubleNear(-1.0, 1e-11));
  }

  // 3. ISG rounding cases:
  // (a) Half-integer grid centers with 1/60 deg (0.0166667 rounded to 1/60).
  {
    VsiMemFile mem("/vsimem/isg_round_half.isg",
                   "model name : ROUND_HALF\n"
                   "lat min = -0.5083333\n"
                   "lat max = 0.5083333\n"
                   "lon min = -0.5083333\n"
                   "lon max = 0.5083333\n"
                   "delta lat = 0.0166667\n"
                   "delta lon = 0.0166667\n"
                   "nrows = 61\n"
                   "ncols = 61\n"
                   "end_of_head\n"
                   "1.0\n");
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_THAT(gt[1], DoubleNear(1.0 / 60.0, 1e-12));
  }

  // (b) Integer-multiple bounds with 0.0166667 rounded to 1/60.
  {
    VsiMemFile mem("/vsimem/isg_round_int.isg",
                   "model name : ROUND_INT\n"
                   "lat min = -0.5\n"
                   "lat max = 0.5\n"
                   "lon min = -0.5\n"
                   "lon max = 0.5\n"
                   "delta lat = 0.0166667\n"
                   "delta lon = 0.0166667\n"
                   "nrows = 60\n"
                   "ncols = 60\n"
                   "end_of_head\n"
                   "1.0\n");
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_THAT(gt[1], DoubleNear(1.0 / 60.0, 1e-12));
  }

  // (c) Special 0.0167 fallback to 0.016667.
  {
    VsiMemFile mem("/vsimem/isg_round_0167.isg",
                   "model name : ROUND_0167\n"
                   "lat min = -35.0003\n"
                   "lat max = -34.0003\n"
                   "lon min = -65.0003\n"
                   "lon max = -64.0003\n"
                   "delta lat = 0.0167\n"
                   "delta lon = 0.0167\n"
                   "nrows = 60\n"
                   "ncols = 60\n"
                   "end_of_head\n"
                   "1.0\n");
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi.get()));
    ASSERT_THAT(ds, NotNull());
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_THAT(gt[1], DoubleNear(0.016667, 1e-12));
  }

  // (d) Slightly approximate georeference within 0.005 * delta -> warning.
  {
    VsiMemFile mem("/vsimem/isg_approx.isg",
                   "model name : APPROX\n"
                   "lat min = 0.0\n"
                   "lat max = 10.02\n"
                   "lon min = 0.0\n"
                   "lon max = 10.02\n"
                   "delta lat = 1.0\n"
                   "delta lon = 1.0\n"
                   "nrows = 10\n"
                   "ncols = 10\n"
                   "end_of_head\n"
                   "1.0\n");
    CPLErrorReset();
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi.get()));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("slightly approximate"));
  }

  // (e) Inconsistent georeference fails by default, succeeds with
  // ISG_SKIP_GEOREF_CONSISTENCY_CHECK=YES.
  {
    VsiMemFile mem("/vsimem/isg_inconsistent.isg",
                   "model name : INCONSISTENT\n"
                   "lat min = 0.0\n"
                   "lat max = 15.0\n"
                   "lon min = 0.0\n"
                   "lon max = 15.0\n"
                   "delta lat = 1.0\n"
                   "delta lon = 1.0\n"
                   "nrows = 10\n"
                   "ncols = 10\n"
                   "end_of_head\n"
                   "1.0\n");
    CPLErrorReset();
    auto oi_fail =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    EXPECT_THAT(ISGDataset::Open(oi_fail.get()), IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    ConfigOptionSaver skip_check("ISG_SKIP_GEOREF_CONSISTENCY_CHECK", "YES");
    auto oi_ok =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds =
        absl::WrapUnique(ISGDataset::Open(oi_ok.get()));
    ASSERT_THAT(ds, NotNull());
  }
}

TEST(IsgDatasetTest, HeaderAndEndOfHeadErrors) {
  QuietErrorHandler quiet;

  const char* kBadIsg[] = {
      // Missing required fields (lon max missing).
      "model name : BAD1\nlat min = 0\nlat max = 1\nlon min = 0\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 1\nncols = 1\nend_of_head\n1\n",
      // Unsupported data format.
      "model name : BAD2\ndata format = sparse\nlat min = 0\nlat max = 1\n"
      "lon min = 0\nlon max = 1\ndelta lat = 1\ndelta lon = 1\n"
      "nrows = 1\nncols = 1\nend_of_head\n1\n",
      // Unsupported data ordering.
      "model name : BAD3\ndata ordering = S-to-N, W-to-E\n"
      "lat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 1\nncols = 1\nend_of_head\n1\n",
      // Unsupported coord type.
      "model name : BAD4\ncoord type = projected\n"
      "lat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 1\nncols = 1\nend_of_head\n1\n",
      // Unsupported coord units.
      "model name : BAD5\ncoord units = rad\n"
      "lat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 1\nncols = 1\nend_of_head\n1\n",
      // Invalid dimensions / deltas (nrows = 0, delta lat = 200, huge dims).
      "model name : BAD6\nlat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 0\nncols = 1\nend_of_head\n1\n",
      "model name : BAD7\nlat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 200\ndelta lon = 1\nnrows = 1\nncols = 1\nend_of_head\n1\n",
      "model name : BAD8\nlat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 0.00001\ndelta lon = 0.00001\nnrows = 100000\nncols = "
      "100000\n"
      "end_of_head\n1\n",
      // Missing end_of_head.
      "model name : BAD9\nlat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 1\nncols = 1\n1.0\n",
      // end_of_head without trailing newline.
      "model name : BAD10\nlat min = 0\nlat max = 1\nlon min = 0\nlon max = 1\n"
      "delta lat = 1\ndelta lon = 1\nnrows = 1\nncols = 1\nend_of_head",
  };

  for (const char* content : kBadIsg) {
    VsiMemFile mem("/vsimem/isg_err.isg", content);
    auto oi =
        std::make_unique<GDALOpenInfo>(mem.path(), GDAL_OF_READONLY, nullptr);
    EXPECT_THAT(ISGDataset::Open(oi.get()), IsNull())
        << "Expected ISG failure for:\n"
        << content;
  }
}

// ============================================================================
// AAIGDataset::CreateCopy tests
// ============================================================================

std::unique_ptr<GDALDataset> CreateMemDataset(int xsize, int ysize, int bands,
                                              GDALDataType dtype) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_drv == nullptr) {
    return nullptr;
  }
  return absl::WrapUnique(
      mem_drv->Create("", xsize, ysize, bands, dtype, nullptr));
}

TEST(AaigridCreateCopyTest, BandCountProgressAndFileCreationErrors) {
  QuietErrorHandler quiet;
  GDALRegister_AAIGrid();

  // 1. Multi-band source dataset (nBands != 1) must fail.
  std::unique_ptr<GDALDataset> two_band = CreateMemDataset(2, 2, 2, GDT_Byte);
  ASSERT_THAT(two_band, NotNull());
  CPLErrorReset();
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsimem/out_2band.asc", two_band.get(), TRUE,
                              nullptr, GDALDummyProgress, nullptr),
      IsNull());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("doesn't support 2 bands"));

  // 2. Progress callback aborting immediately at 0.0.
  std::unique_ptr<GDALDataset> one_band = CreateMemDataset(2, 2, 1, GDT_Int32);
  ASSERT_THAT(one_band, NotNull());
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsimem/out_abort0.asc", one_band.get(), TRUE,
                              nullptr, AbortImmediatelyProgress, nullptr),
      IsNull());

  // 3. Progress callback aborting after first scanline.
  const char* abort1_path = "/vsimem/out_abort1.asc";
  auto clean_abort1 = absl::MakeCleanup([&] { VSIUnlink(abort1_path); });
  CPLErrorReset();
  EXPECT_THAT(
      AAIGDataset::CreateCopy(abort1_path, one_band.get(), TRUE, nullptr,
                              AbortAfterFirstLineProgress, nullptr),
      IsNull());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated CreateCopy"));

  // 4. Invalid output path ("/vsimem/" directory) -> Unable to create file.
  CPLErrorReset();
  EXPECT_THAT(AAIGDataset::CreateCopy("/vsimem/", one_band.get(), TRUE, nullptr,
                                      GDALDummyProgress, nullptr),
              IsNull());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to create file"));
}

TEST(AaigridCreateCopyTest, IntFloatSouthUpNonSquarePrecisionAndProjection) {
  QuietErrorHandler quiet;
  GDALRegister_AAIGrid();

  // 1. Int32 with NoData and WGS84 projection -> writes .asc and .prj.
  {
    std::unique_ptr<GDALDataset> src = CreateMemDataset(2, 2, 1, GDT_Int32);
    ASSERT_THAT(src, NotNull());
    src->SetGeoTransform(GDALGeoTransform(10.0, 2.0, 0.0, 50.0, 0.0, -2.0));
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    char* wkt = nullptr;
    ASSERT_EQ(OGRERR_NONE, srs.exportToWkt(&wkt));
    src->SetProjection(wkt);
    CPLFree(wkt);

    src->GetRasterBand(1)->SetNoDataValue(-9999.0);
    int32_t pixels[4] = {1, -9999, 3, 4};
    ASSERT_EQ(CE_None,
              src->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, pixels, 2,
                                              2, GDT_Int32, 0, 0, nullptr));

    const char* out_asc = "/vsimem/cc_int.asc";
    const char* out_prj = "/vsimem/cc_int.prj";
    auto cleanup = absl::MakeCleanup([&] {
      VSIUnlink(out_asc);
      VSIUnlink(out_prj);
    });

    std::unique_ptr<GDALDataset> dst = absl::WrapUnique(AAIGDataset::CreateCopy(
        out_asc, src.get(), TRUE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(dst, NotNull());
    EXPECT_THAT(dst->GetSpatialRef(), NotNull());
    EXPECT_EQ(GDT_Int32, dst->GetRasterBand(1)->GetRasterDataType());
    EXPECT_DOUBLE_EQ(-9999.0, dst->GetRasterBand(1)->GetNoDataValue(nullptr));

    int32_t out_pixels[4] = {};
    ASSERT_EQ(CE_None,
              dst->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out_pixels,
                                              2, 2, GDT_Int32, 0, 0, nullptr));
    EXPECT_THAT(out_pixels, ElementsAre(1, -9999, 3, 4));
  }

  // 2. Float32 with integer-valued pixels and leading NaN -> appends '.0' to
  // first finite integer pixel so reopened dataset remains Float32.
  {
    std::unique_ptr<GDALDataset> src = CreateMemDataset(3, 1, 1, GDT_Float32);
    ASSERT_THAT(src, NotNull());
    src->GetRasterBand(1)->SetNoDataValue(-99.5);
    float pixels[3] = {std::numeric_limits<float>::quiet_NaN(), 5.0f, 6.0f};
    ASSERT_EQ(CE_None,
              src->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 3, 1, pixels, 3,
                                              1, GDT_Float32, 0, 0, nullptr));

    const char* out_asc = "/vsimem/cc_float_dot.asc";
    auto cleanup = absl::MakeCleanup([&] { VSIUnlink(out_asc); });
    std::unique_ptr<GDALDataset> dst = absl::WrapUnique(AAIGDataset::CreateCopy(
        out_asc, src.get(), TRUE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(dst, NotNull());
    EXPECT_EQ(GDT_Float32, dst->GetRasterBand(1)->GetRasterDataType());
    EXPECT_THAT(ReadVsiFileText(out_asc), HasSubstr("5.0 "));
  }

  // 3. Non-square pixels: default warning (DX/DY), FORCE_CELLSIZE=TRUE,
  // FORCE_CELLSIZE=FALSE, and south-up raster (gt[5] > 0).
  {
    std::unique_ptr<GDALDataset> src = CreateMemDataset(2, 2, 1, GDT_Float64);
    ASSERT_THAT(src, NotNull());
    // South-up and non-square (xscale = 2.0, yscale = 4.0 > 0).
    src->SetGeoTransform(GDALGeoTransform(0.0, 2.0, 0.0, 10.0, 0.0, 4.0));
    double pixels[4] = {1.123456, 2.234567, 3.345678, 4.456789};
    ASSERT_EQ(CE_None,
              src->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, pixels, 2,
                                              2, GDT_Float64, 0, 0, nullptr));

    // (a) No FORCE_CELLSIZE -> emits warning and writes dx / dy; south-up
    // flips row order.
    const char* out_dxdy = "/vsimem/cc_dxdy.asc";
    auto clean_dxdy = absl::MakeCleanup([&] { VSIUnlink(out_dxdy); });
    CapturingErrorHandler capture;
    std::unique_ptr<GDALDataset> dst_dxdy =
        absl::WrapUnique(AAIGDataset::CreateCopy(
            out_dxdy, src.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(dst_dxdy, NotNull());
    EXPECT_EQ(CE_Warning, capture.last_type());
    const std::string text_dxdy = ReadVsiFileText(out_dxdy);
    EXPECT_THAT(text_dxdy, HasSubstr("dx "));
    EXPECT_THAT(text_dxdy, HasSubstr("dy "));
    double flipped[4] = {};
    ASSERT_EQ(CE_None, dst_dxdy->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 2, flipped, 2, 2, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_THAT(
        flipped,
        ElementsAre(DoubleNear(3.345678, 1e-5), DoubleNear(4.456789, 1e-5),
                    DoubleNear(1.123456, 1e-5), DoubleNear(2.234567, 1e-5)));

    // (b) FORCE_CELLSIZE=TRUE + conflicting DECIMAL_PRECISION &
    // SIGNIFICANT_DIGITS -> emits conflict warning, uses DECIMAL_PRECISION.
    const char* out_force = "/vsimem/cc_force.asc";
    auto clean_force = absl::MakeCleanup([&] { VSIUnlink(out_force); });
    char** opts_conflict = nullptr;
    opts_conflict = CSLAddString(opts_conflict, "FORCE_CELLSIZE=TRUE");
    opts_conflict = CSLAddString(opts_conflict, "DECIMAL_PRECISION=2");
    opts_conflict = CSLAddString(opts_conflict, "SIGNIFICANT_DIGITS=5");
    auto free_conflict = absl::MakeCleanup([&] { CSLDestroy(opts_conflict); });
    capture.Reset();
    std::unique_ptr<GDALDataset> dst_force = absl::WrapUnique(
        AAIGDataset::CreateCopy(out_force, src.get(), FALSE, opts_conflict,
                                GDALDummyProgress, nullptr));
    ASSERT_THAT(dst_force, NotNull());
    EXPECT_EQ(CE_Warning, capture.last_type());
    EXPECT_THAT(capture.last_msg(),
                HasSubstr("Conflicting precision arguments"));
    const std::string text_force = ReadVsiFileText(out_force);
    EXPECT_THAT(text_force, HasSubstr("cellsize "));
    EXPECT_THAT(text_force, HasSubstr("1.12 "));

    // (c) SIGNIFICANT_DIGITS alone + FORCE_CELLSIZE=FALSE.
    const char* out_sig = "/vsimem/cc_sig.asc";
    auto clean_sig = absl::MakeCleanup([&] { VSIUnlink(out_sig); });
    char** opts_sig = nullptr;
    opts_sig = CSLAddString(opts_sig, "FORCE_CELLSIZE=FALSE");
    opts_sig = CSLAddString(opts_sig, "SIGNIFICANT_DIGITS=3");
    auto free_sig = absl::MakeCleanup([&] { CSLDestroy(opts_sig); });
    std::unique_ptr<GDALDataset> dst_sig =
        absl::WrapUnique(AAIGDataset::CreateCopy(
            out_sig, src.get(), FALSE, opts_sig, GDALDummyProgress, nullptr));
    ASSERT_THAT(dst_sig, NotNull());
    EXPECT_THAT(ReadVsiFileText(out_sig), HasSubstr("1.12 "));
  }

  // 4. Wide rasters (width = 1030 > 1024) for both Int32 and Float32 to hit
  // periodic 1024-pixel scanline buffer flush branches.
  {
    std::unique_ptr<GDALDataset> src_int =
        CreateMemDataset(1030, 1, 1, GDT_Int32);
    ASSERT_THAT(src_int, NotNull());
    const char* out_wide_int = "/vsimem/cc_wide_int.asc";
    auto clean_wi = absl::MakeCleanup([&] { VSIUnlink(out_wide_int); });
    std::unique_ptr<GDALDataset> dst_wi = absl::WrapUnique(
        AAIGDataset::CreateCopy(out_wide_int, src_int.get(), TRUE, nullptr,
                                GDALDummyProgress, nullptr));
    ASSERT_THAT(dst_wi, NotNull());
    EXPECT_EQ(1030, dst_wi->GetRasterXSize());

    std::unique_ptr<GDALDataset> src_flt =
        CreateMemDataset(1030, 1, 1, GDT_Float32);
    ASSERT_THAT(src_flt, NotNull());
    const char* out_wide_flt = "/vsimem/cc_wide_flt.asc";
    auto clean_wf = absl::MakeCleanup([&] { VSIUnlink(out_wide_flt); });
    std::unique_ptr<GDALDataset> dst_wf = absl::WrapUnique(
        AAIGDataset::CreateCopy(out_wide_flt, src_flt.get(), TRUE, nullptr,
                                GDALDummyProgress, nullptr));
    ASSERT_THAT(dst_wf, NotNull());
    EXPECT_EQ(1030, dst_wf->GetRasterXSize());
  }
}

TEST(AaigridCreateCopyTest, LowLevelWriteFailuresAndWriteOnlyFallback) {
  QuietErrorHandler quiet;
  GDALRegister_AAIGrid();
  ControllableVsiHandler* handler = GetControllableHandler();

  std::unique_ptr<GDALDataset> src_int = CreateMemDataset(2, 1, 1, GDT_Int32);
  ASSERT_THAT(src_int, NotNull());
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  char* wkt = nullptr;
  ASSERT_EQ(OGRERR_NONE, srs.exportToWkt(&wkt));
  src_int->SetProjection(wkt);
  CPLFree(wkt);

  // 1. Write-only target that cannot be reopened -> returns placeholder
  // AAIGDataset (lines 1590-1597).
  handler->Reset();
  handler->write_only_ = true;
  std::unique_ptr<GDALDataset> dst_wo = absl::WrapUnique(
      AAIGDataset::CreateCopy("/vsictrl_aaig/write_only.asc", src_int.get(),
                              TRUE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(dst_wo, NotNull());
  EXPECT_EQ(2, dst_wo->GetRasterXSize());
  EXPECT_EQ(1, dst_wo->GetRasterYSize());
  dst_wo.reset();

  // 2. Fail header write (call 1).
  handler->Reset();
  handler->fail_write_on_call_ = 1;
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsictrl_aaig/fail_hdr.asc", src_int.get(), TRUE,
                              nullptr, GDALDummyProgress, nullptr),
      IsNull());

  // 3. Fail int scanline write (call 2).
  handler->Reset();
  handler->fail_write_on_call_ = 2;
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsictrl_aaig/fail_int.asc", src_int.get(), TRUE,
                              nullptr, GDALDummyProgress, nullptr),
      IsNull());

  // 4. Fail trailing newline write (call 3).
  handler->Reset();
  handler->fail_write_on_call_ = 3;
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsictrl_aaig/fail_nl.asc", src_int.get(), TRUE,
                              nullptr, GDALDummyProgress, nullptr),
      IsNull());

  // 5. Fail Close() on output file.
  handler->Reset();
  handler->fail_close_ = true;
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsictrl_aaig/fail_close.asc", src_int.get(),
                              TRUE, nullptr, GDALDummyProgress, nullptr),
      IsNull());

  // 6. Fail .prj file creation -> emits CPLError but still returns dataset.
  handler->Reset();
  handler->fail_prj_open_ = true;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> dst_no_prj = absl::WrapUnique(
      AAIGDataset::CreateCopy("/vsictrl_aaig/fail_prj.asc", src_int.get(), TRUE,
                              nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(dst_no_prj, NotNull());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  dst_no_prj.reset();

  // 7. Fail float scanline write (call 2).
  std::unique_ptr<GDALDataset> src_flt = CreateMemDataset(2, 1, 1, GDT_Float32);
  ASSERT_THAT(src_flt, NotNull());
  handler->Reset();
  handler->fail_write_on_call_ = 2;
  EXPECT_THAT(
      AAIGDataset::CreateCopy("/vsictrl_aaig/fail_flt.asc", src_flt.get(), TRUE,
                              nullptr, GDALDummyProgress, nullptr),
      IsNull());
  handler->Reset();
}

}  // namespace
}  // namespace autotest2
