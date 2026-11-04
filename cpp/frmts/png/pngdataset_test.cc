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
// Tests PNG raster driver (pngdataset.cpp).
//
// Caveats:
//   - PNGDataset::Create() is guarded by #ifdef SUPPORT_CREATE, which is commented
//     out in pngdataset.cpp (creation supported only via CreateCopy).
//   - PAM (.aux.xml) persistence is compiled out (PAM_ENABLED undefined in BUILD).
//   - ENABLE_WHOLE_IMAGE_OPTIMIZATION is defined only when HAVE_SSE2 is present
//     (x86_64). On other architectures (ARM), scanline fallback is used.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/png/pngdataset.cpp
//   https://gdal.org/drivers/raster/png.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/png.py

#include "third_party/gdal/frmts/png/pngdataset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <span>
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
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/frmts/png/pngdrivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::NotNull;
using namespace std::string_view_literals;

constexpr char kVsiDir[] = "/vsimem/pngdataset_test";

// Builds a valid 400-byte grayscale ICC profile accepted by libpng.
// Includes a 256-byte tag payload with pseudo-random bytes so the compressed
// iCCP chunk exceeds libpng's 81 + LZ77Min byte minimum in png_handle_iCCP.
std::vector<uint8_t> MakeValidGrayIccProfile() {
  constexpr uint32_t kProfileLen = 400;  // 132 header + 12 tag table + 256 data
  std::vector<uint8_t> icc(kProfileLen, 0);
  icc[2] = 0x01;
  icc[3] = 0x90;  // 400
  icc[8] = 0x02;  // ICC v2
  icc[12] = 'm';
  icc[13] = 'n';
  icc[14] = 't';
  icc[15] = 'r';
  icc[16] = 'G';
  icc[17] = 'R';
  icc[18] = 'A';
  icc[19] = 'Y';
  icc[20] = 'X';
  icc[21] = 'Y';
  icc[22] = 'Z';
  icc[23] = ' ';
  icc[36] = 'a';
  icc[37] = 'c';
  icc[38] = 's';
  icc[39] = 'p';
  constexpr uint8_t kD50[] = {0x00, 0x00, 0xf6, 0xd6, 0x00, 0x01,
                              0x00, 0x00, 0x00, 0x00, 0xd3, 0x2d};
  std::memcpy(icc.data() + 68, kD50, sizeof(kD50));
  icc[131] = 1;  // 1 tag
  icc[132] = 'c';
  icc[133] = 'p';
  icc[134] = 'r';
  icc[135] = 't';
  icc[139] = 144;   // tag offset
  icc[142] = 0x01;  // tag size = 256
  uint32_t lcg = 0x12345678;
  for (size_t i = 144; i < kProfileLen; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    icc[i] = static_cast<uint8_t>(lcg >> 24);
  }
  return icc;
}

// Computes 32-bit CRC matching the PNG specification.
constexpr uint32_t PngCrc32(std::span<const uint8_t> data) {
  uint32_t crc = 0xffffffff;
  for (uint8_t byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
      crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return ~crc;
}

// Appends a PNG chunk with length, 4-char type, payload, and 32-bit CRC.
void AppendPngChunk(std::string& out, std::string_view type,
                    std::span<const uint8_t> payload) {
  const uint32_t len = static_cast<uint32_t>(payload.size());
  out.push_back(static_cast<char>((len >> 24) & 0xff));
  out.push_back(static_cast<char>((len >> 16) & 0xff));
  out.push_back(static_cast<char>((len >> 8) & 0xff));
  out.push_back(static_cast<char>(len & 0xff));

  std::vector<uint8_t> crc_data(type.begin(), type.end());
  crc_data.insert(crc_data.end(), payload.begin(), payload.end());
  out.append(type);
  out.append(reinterpret_cast<const char*>(payload.data()), payload.size());

  const uint32_t crc = PngCrc32(crc_data);
  out.push_back(static_cast<char>((crc >> 24) & 0xff));
  out.push_back(static_cast<char>((crc >> 16) & 0xff));
  out.push_back(static_cast<char>((crc >> 8) & 0xff));
  out.push_back(static_cast<char>(crc & 0xff));
}

// Synthesizes a valid PNG file in memory with optional custom chunks or raw IDATs.
std::string MakeCustomPng(
    uint32_t width, uint32_t height, uint8_t bit_depth, uint8_t color_type,
    uint8_t interlace, std::span<const uint8_t> uncompressed_scanlines,
    std::span<const std::pair<std::string_view, std::string_view>> extra_chunks =
        {},
    std::span<const std::string_view> raw_idats = {}) {
  std::string png = "\x89PNG\r\n\x1a\n";

  // IHDR payload (13 bytes)
  std::vector<uint8_t> ihdr = {
      static_cast<uint8_t>((width >> 24) & 0xff),
      static_cast<uint8_t>((width >> 16) & 0xff),
      static_cast<uint8_t>((width >> 8) & 0xff),
      static_cast<uint8_t>(width & 0xff),
      static_cast<uint8_t>((height >> 24) & 0xff),
      static_cast<uint8_t>((height >> 16) & 0xff),
      static_cast<uint8_t>((height >> 8) & 0xff),
      static_cast<uint8_t>(height & 0xff),
      bit_depth,
      color_type,
      0,  // compression
      0,  // filter
      interlace,
  };
  AppendPngChunk(png, "IHDR", ihdr);

  for (const auto& chunk : extra_chunks) {
    const auto payload = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(chunk.second.data()),
        chunk.second.size());
    AppendPngChunk(png, chunk.first, payload);
  }

  if (!raw_idats.empty()) {
    for (const auto& idat : raw_idats) {
      const auto payload = std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(idat.data()), idat.size());
      AppendPngChunk(png, "IDAT", payload);
    }
  } else if (!uncompressed_scanlines.empty()) {
    std::vector<uint8_t> compressed_buf(32 +
                                        uncompressed_scanlines.size() * 2);
    size_t compressed_size = 0;
    void* res = CPLZLibDeflate(
        uncompressed_scanlines.data(), uncompressed_scanlines.size(),
        /*nLevel=*/6, compressed_buf.data(), compressed_buf.size(),
        &compressed_size);
    if (res != nullptr && compressed_size > 0) {
      compressed_buf.resize(compressed_size);
      AppendPngChunk(png, "IDAT", compressed_buf);
    }
  }

  AppendPngChunk(png, "IEND", {});
  return png;
}

// Creates an in-memory dataset via the MEM driver for CreateCopy tests.
std::unique_ptr<GDALDataset> MakeMemDataset(int width, int height, int bands,
                                           GDALDataType data_type) {
  return std::unique_ptr<GDALDataset>(
      MEMDataset::Create("", width, height, bands, data_type, nullptr));
}

// Progress callback that fails immediately.
int AbortProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                  void* /*pProgressArg*/) {
  return FALSE;
}

// Controllable virtual filesystem handler to inject I/O errors.
class ControllableVsiHandler final : public VSIFilesystemHandler {
 public:
  class Handle final : public VSIVirtualHandle {
   public:
    Handle(ControllableVsiHandler* handler, bool is_read)
        : handler_(handler), is_read_(is_read) {}

    int Seek(vsi_l_offset offset, int whence) override {
      if (whence == SEEK_SET) {
        offset_ = static_cast<size_t>(offset);
      } else if (whence == SEEK_CUR) {
        offset_ += static_cast<size_t>(offset);
      } else if (whence == SEEK_END) {
        offset_ = handler_->buffer_.size() + static_cast<size_t>(offset);
      }
      return 0;
    }

    vsi_l_offset Tell() override { return offset_; }

    size_t Read(void* buffer, size_t size, size_t count) override {
      ++handler_->read_calls_;
      if (handler_->fail_read_on_call_ > 0 &&
          handler_->read_calls_ == handler_->fail_read_on_call_) {
        return 0;
      }
      if (!is_read_ || offset_ >= handler_->buffer_.size()) {
        return 0;
      }
      const size_t total = size * count;
      const size_t avail = handler_->buffer_.size() - offset_;
      const size_t to_copy = std::min(total, avail);
      if (to_copy > 0) {
        memcpy(buffer, handler_->buffer_.data() + offset_, to_copy);
        offset_ += to_copy;
      }
      return size > 0 ? to_copy / size : 0;
    }

    size_t Write(const void* buffer, size_t size, size_t count) override {
      ++handler_->write_calls_;
      if (handler_->fail_write_on_call_ > 0 &&
          handler_->write_calls_ == handler_->fail_write_on_call_) {
        return 0;
      }
      const size_t total = size * count;
      const char* chars = static_cast<const char*>(buffer);
      handler_->buffer_.append(chars, total);
      offset_ += total;
      return count;
    }

    void ClearErr() override {}
    int Eof() override { return offset_ >= handler_->buffer_.size() ? 1 : 0; }
    int Error() override { return 0; }

    int Close() override {
      if (handler_->fail_close_) {
        return -1;
      }
      return 0;
    }

   private:
    ControllableVsiHandler* handler_;
    bool is_read_;
    size_t offset_ = 0;
  };

  VSIVirtualHandleUniquePtr Open(const char* /*filename*/, const char* access,
                                 bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    if (fail_open_) {
      return nullptr;
    }
    const bool is_read = strchr(access, 'r') != nullptr;
    if (!is_read) {
      buffer_.clear();
    }
    return VSIVirtualHandleUniquePtr(new Handle(this, is_read));
  }

  int Stat(const char* /*pszFilename*/, VSIStatBufL* stat_buf,
           int /*nFlags*/) override {
    memset(stat_buf, 0, sizeof(*stat_buf));
    stat_buf->st_mode = S_IFREG;
    stat_buf->st_size = buffer_.size();
    return 0;
  }

  void Reset(std::string data = "") {
    buffer_ = std::move(data);
    fail_open_ = false;
    write_calls_ = 0;
    fail_write_on_call_ = 0;
    read_calls_ = 0;
    fail_read_on_call_ = 0;
    fail_close_ = false;
  }

  std::string buffer_;
  bool fail_open_ = false;
  int write_calls_ = 0;
  int fail_write_on_call_ = 0;
  int read_calls_ = 0;
  int fail_read_on_call_ = 0;
  bool fail_close_ = false;
};

// ============================================================================
// Driver Registration & Open Error Paths
// ============================================================================

TEST(PngDatasetTest, RegisterDriverTwice) {
  GDALRegister_PNG();
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());
  EXPECT_STREQ("PNG", driver->GetDescription());
}

TEST(PngDatasetTest, OpenDoesNotExistAndUnsupportedAccess) {
  GDALOpenInfo missing("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_THAT(PNGDataset::Open(&missing), IsNull());

  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  const std::string path = std::format("{}/update.png", kVsiDir);
  const std::vector<uint8_t> scanline = {0, 42};  // Filter None + 1 byte pixel
  VsiMemTempWrapper file(path, MakeCustomPng(1, 1, 8, 0, 0, scanline));

  WithQuietHandler quiet;
  GDALOpenInfo update_info(path.c_str(), GDAL_OF_UPDATE, nullptr);
  EXPECT_THAT(PNGDataset::Open(&update_info), IsNull());
}

TEST(PngDatasetTest, OpenCorruptHeaderAndWarningHandling) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  const std::string path = std::format("{}/corrupt.png", kVsiDir);

  // Valid PNG signature followed by truncated junk -> safe_png_* error handler
  VsiMemTempWrapper file(
      path, std::string("\x89PNG\r\n\x1a\n\x00\x00\x00\x01NOT_IHDR", 20));
  WithQuietHandler quiet;
  GDALOpenInfo info(path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_THAT(PNGDataset::Open(&info), IsNull());
}

// ============================================================================
// Color Types, Bit Depths, Palette, NoData & Background
// ============================================================================

TEST(PngDatasetTest, Gray8BitWithNoDataAndBackground) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 2, 1, GDT_Byte);
  ASSERT_THAT(mem_ds, NotNull());
  uint8_t pixels[4] = {10, 42, 30, 40};
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 2, pixels, 2, 2,
                                      GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));
  mem_ds->GetRasterBand(1)->SetNoDataValue(42);
  mem_ds->SetMetadataItem("BACKGROUND_COLOR", "123");

  const std::string path = std::format("{}/gray8.png", kVsiDir);
  GDALDatasetUniquePtr created_ds(
      driver->CreateCopy(path.c_str(), mem_ds.get(), FALSE, nullptr,
                         GDALDummyProgress, nullptr));
  ASSERT_THAT(created_ds, NotNull());
  created_ds.reset();

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(1, ds->GetRasterCount());
  EXPECT_EQ(2, ds->GetRasterXSize());
  EXPECT_EQ(2, ds->GetRasterYSize());
  EXPECT_EQ(0, ds->GetGCPCount());

  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_THAT(band, NotNull());
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());
  EXPECT_THAT(band->GetColorTable(), IsNull());
  EXPECT_EQ(GSBAP_TOP_TO_BOTTOM, band->GetSuggestedBlockAccessPattern());
  EXPECT_EQ(0, band->GetOverviewCount());

  int has_nodata = 0;
  EXPECT_DOUBLE_EQ(42.0, band->GetNoDataValue(&has_nodata));
  EXPECT_EQ(TRUE, has_nodata);
  EXPECT_DOUBLE_EQ(42.0, band->GetNoDataValue(nullptr));

  EXPECT_EQ(CE_None, band->SetNoDataValue(99.0));
  EXPECT_DOUBLE_EQ(99.0, band->GetNoDataValue(&has_nodata));

  EXPECT_STREQ("123", ds->GetMetadataItem("BACKGROUND_COLOR"));

  uint8_t read_pixels[4] = {};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, read_pixels, 2, 2,
                                    GDT_Byte, 0, 0, nullptr));
  EXPECT_THAT(read_pixels, ElementsAre(10, 42, 30, 40));
}

TEST(PngDatasetTest, GraySubByteDepths) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  const int depths[] = {1, 2, 4};
  for (int depth : depths) {
    SCOPED_TRACE(depth);
    auto mem_ds = MakeMemDataset(4, 1, 1, GDT_Byte);
    uint8_t val[4] = {0, 1, 0, 1};
    ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 4, 1, val, 4, 1,
                                        GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));

    const std::string path = std::format("{}/subbyte_{}.png", kVsiDir, depth);
    const std::string nbits_opt = std::format("NBITS={}", depth);
    const char* options[] = {nbits_opt.c_str(), nullptr};

    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ(std::format("{}", depth).c_str(),
                 ds->GetRasterBand(1)->GetMetadataItem("NBITS",
                                                       "IMAGE_STRUCTURE"));
  }

  // Invalid NBITS creation option emits warning and falls back to 8
  {
    auto mem_ds = MakeMemDataset(2, 1, 1, GDT_Byte);
    const std::string path = std::format("{}/invalid_nbits.png", kVsiDir);
    const char* options[] = {"NBITS=7", nullptr};
    WithQuietHandler quiet;
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
  }

  // Source band metadata NBITS is respected when option is absent
  {
    auto mem_ds = MakeMemDataset(2, 1, 1, GDT_Byte);
    mem_ds->GetRasterBand(1)->SetMetadataItem("NBITS", "2", "IMAGE_STRUCTURE");
    const std::string path = std::format("{}/source_nbits.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("2", ds->GetRasterBand(1)->GetMetadataItem("NBITS",
                                                           "IMAGE_STRUCTURE"));
  }
}

TEST(PngDatasetTest, GrayAlpha8Bit) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 1, 2, GDT_Byte);
  uint8_t data[4] = {128, 255, 64, 128};  // (G, A) x 2
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 1, data, 2, 1,
                                      GDT_Byte, 2, nullptr, 2, 4, 1, nullptr));
  mem_ds->SetMetadataItem("BACKGROUND_COLOR", "100");

  const std::string path = std::format("{}/gray_alpha.png", kVsiDir);
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(2, ds->GetRasterCount());
  EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());
  EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(2)->GetColorInterpretation());
  EXPECT_THAT(ds->GetRasterBand(2)->GetColorTable(), IsNull());
  EXPECT_STREQ("100", ds->GetMetadataItem("BACKGROUND_COLOR"));
}

TEST(PngDatasetTest, RgbAndRgba8BitAndNoData) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  // 1. RGB with NODATA_VALUES metadata item and BACKGROUND_COLOR
  {
    auto mem_ds = MakeMemDataset(1, 1, 3, GDT_Byte);
    mem_ds->SetMetadataItem("NODATA_VALUES", "10 20 30");
    mem_ds->SetMetadataItem("BACKGROUND_COLOR", "12,34,56");

    const std::string path = std::format("{}/rgb.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());

    EXPECT_STREQ("10 20 30", ds->GetMetadataItem("NODATA_VALUES"));
    EXPECT_STREQ("12,34,56", ds->GetMetadataItem("BACKGROUND_COLOR"));
    EXPECT_STREQ("PIXEL", ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));

    int success = 0;
    EXPECT_DOUBLE_EQ(10.0, ds->GetRasterBand(1)->GetNoDataValue(&success));
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(20.0, ds->GetRasterBand(2)->GetNoDataValue(&success));
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(30.0, ds->GetRasterBand(3)->GetNoDataValue(&success));
    EXPECT_TRUE(success);
  }

  // 2. RGB with per-band NoData values (without NODATA_VALUES metadata)
  {
    auto mem_ds = MakeMemDataset(1, 1, 3, GDT_Byte);
    mem_ds->GetRasterBand(1)->SetNoDataValue(11);
    mem_ds->GetRasterBand(2)->SetNoDataValue(22);
    mem_ds->GetRasterBand(3)->SetNoDataValue(33);

    const std::string path = std::format("{}/rgb_perband_nd.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("11 22 33", ds->GetMetadataItem("NODATA_VALUES"));
  }

  // 3. RGBA 4-band
  {
    auto mem_ds = MakeMemDataset(1, 1, 4, GDT_Byte);
    mem_ds->SetMetadataItem("BACKGROUND_COLOR", "1,2,3");

    const std::string path = std::format("{}/rgba.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(4, ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(4)->GetColorInterpretation());
    EXPECT_STREQ("1,2,3", ds->GetMetadataItem("BACKGROUND_COLOR"));
  }
}

TEST(PngDatasetTest, UInt16BitRoundTrip) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 2, 3, GDT_UInt16);
  uint16_t data[12] = {1000, 2000, 3000, 4000, 5000, 6000,
                       7000, 8000, 9000, 10000, 11000, 12000};
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 2, data, 2, 2,
                                      GDT_UInt16, 3, nullptr, 6, 12, 2,
                                      nullptr));

  const std::string path = std::format("{}/uint16.png", kVsiDir);
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  // 1. Default byte order
  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(3, ds->GetRasterCount());
  EXPECT_EQ(GDT_UInt16, ds->GetRasterBand(1)->GetRasterDataType());

  uint16_t read_data[12] = {};
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 2, 2, read_data, 2, 2,
                                  GDT_UInt16, 3, nullptr, 6, 12, 2, nullptr));
  EXPECT_EQ(1000, read_data[0]);
  EXPECT_EQ(2000, read_data[1]);
  EXPECT_EQ(3000, read_data[2]);
  EXPECT_EQ(12000, read_data[11]);

  // 2. BYTE_ORDER_LITTLE_ENDIAN=YES (used by JPEG FLIR)
  const char* open_opts[] = {"BYTE_ORDER_LITTLE_ENDIAN=YES", nullptr};
  GDALOpenInfo open_info2(path.c_str(), GDAL_OF_READONLY);
  open_info2.papszOpenOptions = const_cast<char**>(open_opts);
  std::unique_ptr<GDALDataset> ds2(PNGDataset::Open(&open_info2));
  ASSERT_THAT(ds2, NotNull());
  int bx = 0, by = 0;
  ds2->GetRasterBand(1)->GetBlockSize(&bx, &by);
  std::vector<uint16_t> read_le(bx * by);
  ASSERT_EQ(CE_None, ds2->GetRasterBand(1)->ReadBlock(0, 0, read_le.data()));
  EXPECT_EQ(CPL_SWAP16(1000), read_le[0]);
}

TEST(PngDatasetTest, PaletteTransparencyAndBackgroundVariants) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  // 1. Single transparent entry -> sets nodata on band 1
  {
    auto mem_ds = MakeMemDataset(2, 1, 1, GDT_Byte);
    GDALColorTable ct;
    GDALColorEntry e0 = {255, 0, 0, 255};
    GDALColorEntry e1 = {0, 255, 0, 0};  // transparent
    GDALColorEntry e2 = {0, 0, 255, 255};
    ct.SetColorEntry(0, &e0);
    ct.SetColorEntry(1, &e1);
    ct.SetColorEntry(2, &e2);
    mem_ds->GetRasterBand(1)->SetColorTable(&ct);
    mem_ds->SetMetadataItem("BACKGROUND_COLOR", "2");

    const std::string path = std::format("{}/palette_single_trans.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(GCI_PaletteIndex,
              ds->GetRasterBand(1)->GetColorInterpretation());
    GDALColorTable* read_ct = ds->GetRasterBand(1)->GetColorTable();
    ASSERT_THAT(read_ct, NotNull());
    EXPECT_EQ(3, read_ct->GetColorEntryCount());

    int has_nodata = 0;
    EXPECT_DOUBLE_EQ(1.0, ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
    EXPECT_TRUE(has_nodata);
    EXPECT_STREQ("2", ds->GetMetadataItem("BACKGROUND_COLOR"));
  }

  // 2. Multiple transparent entries -> nNoDataIndex == -2, no nodata
  {
    auto mem_ds = MakeMemDataset(2, 1, 1, GDT_Byte);
    GDALColorTable ct;
    GDALColorEntry e0 = {255, 0, 0, 0};  // transparent
    GDALColorEntry e1 = {0, 255, 0, 0};  // transparent
    ct.SetColorEntry(0, &e0);
    ct.SetColorEntry(1, &e1);
    mem_ds->GetRasterBand(1)->SetColorTable(&ct);

    const std::string path = std::format("{}/palette_multi_trans.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    int has_nodata = 0;
    ds->GetRasterBand(1)->GetNoDataValue(&has_nodata);
    EXPECT_FALSE(has_nodata);
  }

  // 3. Palette with more entries than 1 << nBitDepth is clamped
  {
    auto mem_ds = MakeMemDataset(2, 1, 1, GDT_Byte);
    GDALColorTable ct;
    for (int i = 0; i < 8; ++i) {
      GDALColorEntry e = {static_cast<short>(i * 30), 0, 0, 255};
      ct.SetColorEntry(i, &e);
    }
    mem_ds->GetRasterBand(1)->SetColorTable(&ct);

    const std::string path = std::format("{}/palette_clamped.png", kVsiDir);
    const char* options[] = {"NBITS=1", nullptr};  // max 2 entries
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    GDALColorTable* ct_out = ds->GetRasterBand(1)->GetColorTable();
    ASSERT_THAT(ct_out, NotNull());
    EXPECT_EQ(2, ct_out->GetColorEntryCount());
  }

  // 4. Palette dataset with nodata value exercising transparency write (pabyAlpha[iColor] = 0)
  {
    auto mem_pal = MakeMemDataset(2, 2, 1, GDT_Byte);
    ASSERT_THAT(mem_pal, NotNull());
    GDALColorTable ct;
    GDALColorEntry e0 = {255, 0, 0, 255};
    GDALColorEntry e1 = {0, 255, 0, 255};
    ct.SetColorEntry(0, &e0);
    ct.SetColorEntry(1, &e1);
    mem_pal->GetRasterBand(1)->SetColorTable(&ct);
    mem_pal->GetRasterBand(1)->SetNoDataValue(1.0);

    const std::string p_pal = std::format("{}/pal_nodata.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        p_pal.c_str(), mem_pal.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo oi(p_pal.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());
    int has_nodata = FALSE;
    EXPECT_DOUBLE_EQ(1.0, ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
    EXPECT_EQ(TRUE, has_nodata);
  }
}

// ============================================================================
// Metadata: Text Chunks, XMP, ICC Profiles, World Files
// ============================================================================

TEST(PngDatasetTest, TextMetadataAndCharacterSanitization) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
  mem_ds->SetMetadataItem("Title", "Sample Image");
  mem_ds->SetMetadataItem("Author", "Test Author");
  mem_ds->SetMetadataItem("Description", "A test description");
  mem_ds->SetMetadataItem("Copyright", "Public Domain");
  mem_ds->SetMetadataItem("Comment", "A friendly comment");
  // Custom key with space and UTF-8 value
  mem_ds->SetMetadataItem("Custom Key", "Caf\xc3\xa9");

  const std::string path = std::format("{}/metadata.png", kVsiDir);
  const char* options[] = {"WRITE_METADATA_AS_TEXT=YES", nullptr};
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
      GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());
  EXPECT_STREQ("Sample Image", ds->GetMetadataItem("Title"));
  EXPECT_STREQ("Test Author", ds->GetMetadataItem("Author"));
  EXPECT_STREQ("A test description", ds->GetMetadataItem("Description"));
  EXPECT_STREQ("Public Domain", ds->GetMetadataItem("Copyright"));
  EXPECT_STREQ("A friendly comment", ds->GetMetadataItem("Comment"));

  // Spaces sanitized to '_'
  EXPECT_STREQ("Caf\xc3\xa9", ds->GetMetadataItem("Custom_Key"));

  // Also verify '=' and ':' sanitization in CollectMetadata from custom chunk
  const std::string raw_path = std::format("{}/raw_text.png", kVsiDir);
  const std::string tEXt_data = std::string("Key=With:Space \0Val", 19);
  const std::pair<std::string_view, std::string_view> chunks[] = {
      {"tEXt", tEXt_data},
  };
  const std::vector<uint8_t> scanline = {0, 100};
  VsiMemTempWrapper raw_file(
      raw_path, MakeCustomPng(1, 1, 8, 0, 0, scanline, chunks));
  GDALOpenInfo open_info_raw(raw_path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds_raw(PNGDataset::Open(&open_info_raw));
  ASSERT_THAT(ds_raw, NotNull());
  EXPECT_STREQ("Val", ds_raw->GetMetadataItem("Key_With_Space_"));
}

TEST(PngDatasetTest, XmpMetadataExtraction) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  const std::string path = std::format("{}/xmp.png", kVsiDir);

  // 1. Non-XMP iTXt followed by valid XMP iTXt
  const std::string non_xmp_itxt =
      std::string("OtherHeader\0\0\0\0\0\0\0\0\0\0\0something"sv);
  const std::string xmp_content =
      "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF/></x:xmpmeta>";
  std::string valid_xmp_itxt("XML:com.adobe.xmp\0\0\0\0\0", 22);
  valid_xmp_itxt.append(xmp_content);

  const std::pair<std::string_view, std::string_view> chunks[] = {
      {"iTXt", non_xmp_itxt},
      {"iTXt", valid_xmp_itxt},
  };
  const std::vector<uint8_t> scanline = {0, 100};
  VsiMemTempWrapper file(
      path, MakeCustomPng(1, 1, 8, 0, 0, scanline, chunks));

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());

  StringListPtr domain_list(ds->GetMetadataDomainList(), CSLDestroy);
  ASSERT_THAT(domain_list, NotNull());
  auto domain_vec = CslToVector(domain_list.get());
  EXPECT_THAT(domain_vec, Contains("xml:XMP"));

  char** xmp_md = ds->GetMetadata("xml:XMP");
  ASSERT_THAT(xmp_md, NotNull());
  EXPECT_STREQ(xmp_content.c_str(), xmp_md[0]);
}

TEST(PngDatasetTest, ColorProfileAndGammaChromasRGB) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  // 1. sRGB intent
  {
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    const std::string path = std::format("{}/srgb.png", kVsiDir);
    const char* options[] = {"SOURCE_ICC_PROFILE_NAME=sRGB", nullptr};
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("sRGB",
                 ds->GetMetadataItem("SOURCE_ICC_PROFILE_NAME",
                                     "COLOR_PROFILE"));
  }

  // 2. PNG_GAMMA and chromaticities
  {
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    const std::string path = std::format("{}/gamma_chrm.png", kVsiDir);
    const char* options[] = {
        "PNG_GAMMA=0.45455",
        "SOURCE_PRIMARIES_RED=0.64,0.33,1.0",
        "SOURCE_PRIMARIES_GREEN=0.30,0.60,1.0",
        "SOURCE_PRIMARIES_BLUE=0.15,0.06,1.0",
        "SOURCE_WHITEPOINT=0.3127,0.3290,1.0",
        nullptr,
    };
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_THAT(ds->GetMetadataItem("PNG_GAMMA", "COLOR_PROFILE"),
                HasSubstr("0.4545"));
    EXPECT_THAT(ds->GetMetadataItem("SOURCE_PRIMARIES_RED", "COLOR_PROFILE"),
                HasSubstr("0.64"));
    EXPECT_THAT(ds->GetMetadataItem("SOURCE_WHITEPOINT", "COLOR_PROFILE"),
                HasSubstr("0.3127"));
  }

  // 3. Invalid chromaticity where 3rd component != 1.0 (skipped gracefully)
  {
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    const std::string path = std::format("{}/invalid_chrm.png", kVsiDir);
    const char* options[] = {
        "SOURCE_PRIMARIES_RED=0.64,0.33,2.0",  // 2.0 != 1.0
        "SOURCE_PRIMARIES_GREEN=0.30,0.60,1.0",
        "SOURCE_PRIMARIES_BLUE=0.15,0.06,1.0",
        "SOURCE_WHITEPOINT=0.3127,0.3290,1.0",
        nullptr,
    };
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
  }

  // 4. Base64 SOURCE_ICC_PROFILE with valid 132-byte header
  {
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    const auto valid_icc = MakeValidGrayIccProfile();
    std::unique_ptr<char, CplFreeDeleter> b64(CPLBase64Encode(
        static_cast<int>(valid_icc.size()), valid_icc.data()));
    ASSERT_THAT(b64, NotNull());

    const std::string path = std::format("{}/icc.png", kVsiDir);
    const std::string opt_icc = std::format("SOURCE_ICC_PROFILE={}", b64.get());
    const char* options[] = {
        opt_icc.c_str(),
        "SOURCE_ICC_PROFILE_NAME=CustomProfile",
        nullptr,
    };
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
        GDALDummyProgress, nullptr));
    ASSERT_THAT(created, NotNull());
    created.reset();

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("CustomProfile",
                 ds->GetMetadataItem("SOURCE_ICC_PROFILE_NAME",
                                     "COLOR_PROFILE"));
    EXPECT_THAT(ds->GetMetadataItem("SOURCE_ICC_PROFILE", "COLOR_PROFILE"),
                NotNull());
  }

  // 5. Invalid ICC profile (< 132 bytes) is rejected
  {
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    std::vector<uint8_t> short_icc(64, 0);
    std::unique_ptr<char, CplFreeDeleter> b64(CPLBase64Encode(
        static_cast<int>(short_icc.size()), short_icc.data()));
    ASSERT_THAT(b64, NotNull());

    const std::string path = std::format("{}/bad_icc.png", kVsiDir);
    const std::string opt_icc = std::format("SOURCE_ICC_PROFILE={}", b64.get());
    const char* options[] = {opt_icc.c_str(), nullptr};
    WithQuietHandler quiet;
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path.c_str(), mem_ds.get(), FALSE,
                    const_cast<char**>(options), GDALDummyProgress, nullptr)),
                IsNull());
  }
}

TEST(PngDatasetTest, WorldFileAndFileListAndOpenOptions) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 2, 1, GDT_Byte);
  double gt_in[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  mem_ds->SetGeoTransform(gt_in);

  const std::string path = std::format("{}/world.png", kVsiDir);
  const char* options[] = {"WORLDFILE=YES", nullptr};
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, const_cast<char**>(options),
      GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  const std::string wld_path = std::format("{}/world.wld", kVsiDir);
  VSIStatBufL s_stat;
  EXPECT_EQ(0, VSIStatL(wld_path.c_str(), &s_stat));

  // 1. Open without open options to read world file
  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());

  GDALGeoTransform gt_out;
  ASSERT_EQ(CE_None, ds->GetGeoTransform(gt_out));
  EXPECT_DOUBLE_EQ(100.0, gt_out[0]);
  EXPECT_DOUBLE_EQ(2.0, gt_out[1]);
  EXPECT_DOUBLE_EQ(200.0, gt_out[3]);
  EXPECT_DOUBLE_EQ(-2.0, gt_out[5]);

  StringListPtr file_list(ds->GetFileList(), CSLDestroy);
  ASSERT_THAT(file_list, NotNull());
  auto file_vec = CslToVector(file_list.get());
  EXPECT_THAT(file_vec, Contains(wld_path));

  // 2. Open with PHYSICAL_FILENAME and SUBDATASET_NAME options
  const std::string phys_opt = std::format("PHYSICAL_FILENAME={}", path);
  const char* open_opts[] = {
      phys_opt.c_str(),
      "SUBDATASET_NAME=sub_test",
      nullptr,
  };
  GDALOpenInfo open_info_opts(path.c_str(), GDAL_OF_READONLY);
  open_info_opts.papszOpenOptions = const_cast<char**>(open_opts);
  std::unique_ptr<GDALDataset> ds_opts(PNGDataset::Open(&open_info_opts));
  ASSERT_THAT(ds_opts, NotNull());
}

// ============================================================================
// CreateCopy Validation & Failure Paths
// ============================================================================

TEST(PngDatasetTest, CreateCopyValidationFailures) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  // 1. Invalid band count (0 and 5 bands)
  {
    WithQuietHandler quiet;
    auto mem_0 = MakeMemDataset(1, 1, 0, GDT_Byte);
    const std::string path = std::format("{}/fail_0band.png", kVsiDir);
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path.c_str(), mem_0.get(), FALSE, nullptr,
                    GDALDummyProgress, nullptr)),
                IsNull());

    auto mem_5 = MakeMemDataset(1, 1, 5, GDT_Byte);
    const std::string path5 = std::format("{}/fail_5band.png", kVsiDir);
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path5.c_str(), mem_5.get(), FALSE, nullptr,
                    GDALDummyProgress, nullptr)),
                IsNull());
  }

  // 2. Unsupported data type Float32: strict fails, non-strict warns and succeeds
  {
    WithQuietHandler quiet;
    auto mem_f32 = MakeMemDataset(1, 1, 1, GDT_Float32);
    const std::string path_strict = std::format("{}/fail_strict.png", kVsiDir);
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path_strict.c_str(), mem_f32.get(),
                    /*bStrict=*/TRUE, nullptr, GDALDummyProgress, nullptr)),
                IsNull());

    const std::string path_nonstrict =
        std::format("{}/warn_nonstrict.png", kVsiDir);
    GDALDatasetUniquePtr created(driver->CreateCopy(
        path_nonstrict.c_str(), mem_f32.get(), /*bStrict=*/FALSE, nullptr,
        GDALDummyProgress, nullptr));
    EXPECT_THAT(created, NotNull());
  }

  // 3. ZLEVEL control (invalid -1 and 10 fail, valid 6 succeeds)
  {
    WithQuietHandler quiet;
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    const std::string path = std::format("{}/fail_zlevel.png", kVsiDir);
    const char* opts_neg[] = {"ZLEVEL=-1", nullptr};
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path.c_str(), mem_ds.get(), FALSE,
                    const_cast<char**>(opts_neg), GDALDummyProgress, nullptr)),
                IsNull());

    const char* opts_10[] = {"ZLEVEL=10", nullptr};
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path.c_str(), mem_ds.get(), FALSE,
                    const_cast<char**>(opts_10), GDALDummyProgress, nullptr)),
                IsNull());

    const std::string path_z6 = std::format("{}/zlevel6.png", kVsiDir);
    const char* opts_6[] = {"ZLEVEL=6", nullptr};
    GDALDatasetUniquePtr created_z6(driver->CreateCopy(
        path_z6.c_str(), mem_ds.get(), FALSE, const_cast<char**>(opts_6),
        GDALDummyProgress, nullptr));
    EXPECT_THAT(created_z6, NotNull());
  }

  // 4. Progress callback interruption
  {
    WithQuietHandler quiet;
    auto mem_ds = MakeMemDataset(2, 4, 1, GDT_Byte);
    const std::string path = std::format("{}/user_abort.png", kVsiDir);
    EXPECT_THAT(GDALDatasetUniquePtr(driver->CreateCopy(
                    path.c_str(), mem_ds.get(), FALSE, nullptr,
                    AbortProgress, nullptr)),
                IsNull());
  }

  // 5. GDAL_OPEN_AFTER_COPY=NO returns fake dataset with fpImage == nullptr
  {
    auto mem_ds = MakeMemDataset(2, 2, 1, GDT_Byte);
    const std::string path = std::format("{}/no_open_after.png", kVsiDir);
    CPLConfigOptionSetter set_open_after("GDAL_OPEN_AFTER_COPY", "NO", false);

    GDALDatasetUniquePtr fake_ds(driver->CreateCopy(
        path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
    ASSERT_THAT(fake_ds, NotNull());
    EXPECT_EQ(2, fake_ds->GetRasterXSize());
    EXPECT_EQ(2, fake_ds->GetRasterYSize());
    EXPECT_THAT(fake_ds->GetMetadata(), IsNull());

    // IReadBlock on fake dataset fills buffer with 0
    int bx = 0, by = 0;
    fake_ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
    std::vector<uint8_t> block(bx * by, 0xff);
    ASSERT_EQ(CE_None, fake_ds->GetRasterBand(1)->ReadBlock(0, 0, block.data()));
    for (int i = 0; i < bx * by; ++i) {
      EXPECT_EQ(0, block[i]);
    }
  }
}

// ============================================================================
// Scanline Reading, Out-of-Order Reads, Restart(), & Interlacing
// ============================================================================

TEST(PngDatasetTest, ScanlineOutOfOrderReadingAndRestart) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 3, 1, GDT_Byte);
  uint8_t data[6] = {10, 20, 30, 40, 50, 60};
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 3, data, 2, 3,
                                      GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));

  const std::string path = std::format("{}/out_of_order.png", kVsiDir);
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  // Disable whole image optimization to exercise PNGRasterBand::IReadBlock scanline loop
  CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "NO");
  CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", "NO");
  absl::Cleanup clean_opt = [] {
    CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", nullptr);
    CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", nullptr);
  };

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_THAT(band, NotNull());

  uint8_t row[2] = {};
  // Read row 2 first
  ASSERT_EQ(CE_None, band->ReadBlock(0, 2, row));
  EXPECT_THAT(row, ElementsAre(50, 60));

  // Read row 0 (requires Restart(); triggers !m_bHasRewind debug branch)
  ASSERT_EQ(CE_None, band->ReadBlock(0, 0, row));
  EXPECT_THAT(row, ElementsAre(10, 20));

  // Read row 0 again (triggers m_bHasRewind already true)
  ASSERT_EQ(CE_None, band->ReadBlock(0, 0, row));
  EXPECT_THAT(row, ElementsAre(10, 20));

  // Read row 1
  ASSERT_EQ(CE_None, band->ReadBlock(0, 1, row));
  EXPECT_THAT(row, ElementsAre(30, 40));
}

TEST(PngDatasetTest, MultiBandBlockLockingAndCachedBlocks) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 2, 2, GDT_Byte);
  uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 2, data, 2, 2,
                                      GDT_Byte, 2, nullptr, 2, 4, 1, nullptr));

  const std::string path = std::format("{}/multiband_blocks.png", kVsiDir);
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "NO");
  CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", "NO");
  absl::Cleanup clean_opt = [] {
    CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", nullptr);
    CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", nullptr);
  };

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());

  // Reading band 1 block (0, 0) forcibly instantiates band 2 block (0, 0)
  uint8_t b1[2] = {};
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, b1));
  EXPECT_THAT(b1, ElementsAre(1, 3));

  // Flush band 1 block (0, 0) while band 2 block (0, 0) remains cached
  ds->GetRasterBand(1)->FlushBlock(0, 0);

  // Read band 1 block (0, 0) again: band 2 block is already cached (TryGetLockedBlockRef != null)
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, b1));
  EXPECT_THAT(b1, ElementsAre(1, 3));

  // FlushCache frees buffer
  EXPECT_EQ(CE_None, ds->FlushCache(false));
}

TEST(PngDatasetTest, InterlacedReadingAndFlushCache) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  const std::string path = std::format("{}/interlaced.png", kVsiDir);

  // 2x2 8-bit Gray interlaced PNG (Adam7)
  // For 2x2, only passes 1 (0,0), 6 (1,0), and 7 ((0,1) and (1,1)) are non-empty.
  const std::vector<uint8_t> scanlines = {
      0, 10,      // pass 1: (0,0)
      0, 20,      // pass 6: (1,0)
      0, 30, 40,  // pass 7: row 1: (0,1) and (1,1)
  };
  VsiMemTempWrapper file(path,
                         MakeCustomPng(2, 2, 8, 0, /*interlace=*/1, scanlines));

  CPLConfigOptionSetter set_opt("GDAL_PNG_WHOLE_IMAGE_OPTIM", "NO", false);

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_THAT(band, NotNull());

  uint8_t row[2] = {};
  ASSERT_EQ(CE_None, band->ReadBlock(0, 0, row));
  EXPECT_THAT(row, ElementsAre(10, 20));
  // Call FlushCache and read row 1 to cover nLastLineRead != -1 in LoadInterlacedChunk
  ASSERT_EQ(CE_None, ds->FlushCache(false));
  ASSERT_EQ(CE_None, band->ReadBlock(0, 1, row));
  EXPECT_THAT(row, ElementsAre(30, 40));
}

TEST(PngDatasetTest, CorruptScanlineAndInterlacedErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  WithQuietHandler quiet;

  CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "NO");
  CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", "NO");
  absl::Cleanup clean_opt = [] {
    CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", nullptr);
    CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", nullptr);
  };

  // 1. Corrupt non-interlaced IDAT
  {
    const std::string path = std::format("{}/corrupt_scanline.png", kVsiDir);
    const std::string_view bad_idat = "CORRUPT_ZLIB_DATA_123456";
    VsiMemTempWrapper file(
        path, MakeCustomPng(2, 2, 8, 0, 0, {}, {}, {{bad_idat}}));

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    if (ds != nullptr) {
      uint8_t row[2] = {};
      EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 0, row));
    }
  }

  // 2. Corrupt interlaced IDAT
  {
    const std::string path = std::format("{}/corrupt_interlaced.png", kVsiDir);
    const std::string_view bad_idat = "CORRUPT_INTERLACED_ZLIB_DATA";
    VsiMemTempWrapper file(
        path, MakeCustomPng(2, 2, 8, 0, /*interlace=*/1, {}, {}, {{bad_idat}}));

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    if (ds != nullptr) {
      uint8_t row[2] = {};
      EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->ReadBlock(0, 0, row));
    }
  }
}

// ============================================================================
// IRasterIO Layouts & Edge Paths
// ============================================================================

TEST(PngDatasetTest, DatasetIRasterIOLayouts) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 2, 3, GDT_Byte);
  uint8_t data[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 2, data, 2, 2,
                                      GDT_Byte, 3, nullptr, 3, 6, 1, nullptr));

  const std::string path = std::format("{}/rasterio_layouts.png", kVsiDir);
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "NO");
  absl::Cleanup clean_opt = [] {
    CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", nullptr);
  };

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());

  // 1. Contiguous pixel-interleaved read
  uint8_t buf_contig[12] = {};
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 2, 2, buf_contig, 2, 2,
                                  GDT_Byte, 3, nullptr, 3, 6, 1, nullptr));
  EXPECT_EQ(1, buf_contig[0]);
  EXPECT_EQ(2, buf_contig[1]);
  EXPECT_EQ(3, buf_contig[2]);

  // 2. Pixel interleaved strided
  uint8_t buf_strided[24] = {};
  int band_map[3] = {1, 2, 3};
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 2, 2, buf_strided, 2, 2,
                                  GDT_Byte, 3, band_map, /*nPixelSpace=*/5,
                                  /*nLineSpace=*/12, /*nBandSpace=*/1,
                                  nullptr));
  EXPECT_EQ(1, buf_strided[0]);
  EXPECT_EQ(2, buf_strided[1]);
  EXPECT_EQ(3, buf_strided[2]);

  // 3. Band interleaved deinterleaved
  uint8_t buf_deint[12] = {};
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 2, 2, buf_deint, 2, 2,
                                  GDT_Byte, 3, band_map, /*nPixelSpace=*/1,
                                  /*nLineSpace=*/2, /*nBandSpace=*/4, nullptr));
  EXPECT_EQ(1, buf_deint[0]);   // B1(0,0)
  EXPECT_EQ(4, buf_deint[1]);   // B1(1,0)
  EXPECT_EQ(2, buf_deint[4]);   // B2(0,0)
  EXPECT_EQ(3, buf_deint[8]);   // B3(0,0)

  // 4. Band interleaved generic strided
  uint8_t buf_gen[48] = {};
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 2, 2, buf_gen, 2, 2, GDT_Byte,
                                  3, band_map, /*nPixelSpace=*/4,
                                  /*nLineSpace=*/10, /*nBandSpace=*/16,
                                  nullptr));
  EXPECT_EQ(1, buf_gen[0]);
  EXPECT_EQ(2, buf_gen[16]);
  EXPECT_EQ(3, buf_gen[32]);
}

TEST(PngDatasetTest, BandIRasterIOWhenSingleBlock) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  auto mem_ds = MakeMemDataset(2, 2, 3, GDT_Byte);
  uint8_t data[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  ASSERT_EQ(CE_None, mem_ds->RasterIO(GF_Write, 0, 0, 2, 2, data, 2, 2,
                                      GDT_Byte, 3, nullptr, 3, 6, 1, nullptr));

  const std::string path = std::format("{}/band_rasterio.png", kVsiDir);
  GDALDatasetUniquePtr created(driver->CreateCopy(
      path.c_str(), mem_ds.get(), FALSE, nullptr, GDALDummyProgress, nullptr));
  ASSERT_THAT(created, NotNull());
  created.reset();

  CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "YES");
  CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", "YES");
  absl::Cleanup clean_opt = [] {
    CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", nullptr);
    CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", nullptr);
  };

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
  std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
  ASSERT_THAT(ds, NotNull());

  // Band RasterIO reading single band whole image
  uint8_t b1[4] = {};
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, b1, 2,
                                                    2, GDT_Byte, 1, 2, nullptr));
  EXPECT_THAT(b1, ElementsAre(1, 4, 7, 10));

  // Pre-load block and call RasterIO with different destination buffer (bBlockAlreadyLoaded path)
  uint8_t b2[4] = {};
  ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 2, 2, b2, 2,
                                                    2, GDT_Byte, 1, 2, nullptr));
  EXPECT_THAT(b2, ElementsAre(2, 5, 8, 11));
}

// ============================================================================
// Whole-Image Optimization & SSE2/Scalar Filters
// ============================================================================

TEST(PngDatasetTest, WholeImageOptimizationFiltersAndEdgeCases) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "YES");
  CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", "YES");
  absl::Cleanup clean_opt = [] {
    CPLSetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", nullptr);
    CPLSetConfigOption("GDAL_PNG_SINGLE_BLOCK", nullptr);
  };

  // 1. 3-band RGB with width = 3 (rowbytes = 9, exercising both vector loop & rb > 0 tail)
  // Rows:
  // Row 0: filter None (0)
  // Row 1: filter Sub (1) -> exercises sub3_sse2 loop and tail
  // Row 2: filter Up (2) -> exercises avg/vector add
  // Row 3: filter Average (3) -> exercises avg3_sse2 loop and tail
  // Row 4: filter Paeth (4) -> exercises paeth3_sse2 loop, tail, and tie breakers
  {
    const int w = 3;
    const int h = 5;
    std::vector<uint8_t> scanlines;
    // Row 0 (None): filter byte 0, 9 pixel bytes
    scanlines.push_back(0);
    for (int i = 0; i < 9; ++i) scanlines.push_back(static_cast<uint8_t>(i + 10));

    // Row 1 (Sub): filter byte 1, 9 pixel bytes
    scanlines.push_back(1);
    for (int i = 0; i < 9; ++i) scanlines.push_back(static_cast<uint8_t>(i + 2));

    // Row 2 (Up): filter byte 2, 9 pixel bytes
    scanlines.push_back(2);
    for (int i = 0; i < 9; ++i) scanlines.push_back(static_cast<uint8_t>(i + 5));

    // Row 3 (Average): filter byte 3, 9 pixel bytes
    scanlines.push_back(3);
    for (int i = 0; i < 9; ++i) scanlines.push_back(static_cast<uint8_t>(i + 3));

    // Row 4 (Paeth): filter byte 4, 9 pixel bytes
    scanlines.push_back(4);
    for (int i = 0; i < 9; ++i) scanlines.push_back(static_cast<uint8_t>(i + 4));

    const std::string path = std::format("{}/filters_rgb.png", kVsiDir);
    VsiMemTempWrapper file(path, MakeCustomPng(w, h, 8, 2, 0, scanlines));

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> out(w * h * 3);
    int band_map[3] = {1, 2, 3};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, w, h, out.data(), w, h,
                                    GDT_Byte, 3, band_map, 3, w * 3, 1,
                                    nullptr));
  }

  // 2. 4-band RGBA with width = 2 (rowbytes = 8, exercising sub4, avg4, paeth4)
  {
    const int w = 2;
    const int h = 4;
    std::vector<uint8_t> scanlines;
    // Row 0: filter None
    scanlines.push_back(0);
    for (int i = 0; i < 8; ++i) scanlines.push_back(10);
    // Row 1: filter Sub (4-band)
    scanlines.push_back(1);
    for (int i = 0; i < 8; ++i) scanlines.push_back(2);
    // Row 2: filter Average (4-band)
    scanlines.push_back(3);
    for (int i = 0; i < 8; ++i) scanlines.push_back(4);
    // Row 3: filter Paeth (4-band)
    scanlines.push_back(4);
    for (int i = 0; i < 8; ++i) scanlines.push_back(3);

    const std::string path = std::format("{}/filters_rgba.png", kVsiDir);
    VsiMemTempWrapper file(path, MakeCustomPng(w, h, 8, 6, 0, scanlines));

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> out(w * h * 4);
    int band_map[4] = {1, 2, 3, 4};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, w, h, out.data(), w, h,
                                    GDT_Byte, 4, band_map, 4, w * 4, 1,
                                    nullptr));
  }

  // 3. 1-band Gray scalar filter loops and vector add
  {
    const int w = 36;  // > 32 bytes to test vector add in Up filter
    const int h = 3;
    std::vector<uint8_t> scanlines;
    // Row 0: None
    scanlines.push_back(0);
    for (int i = 0; i < w; ++i) scanlines.push_back(static_cast<uint8_t>(i));
    // Row 1: Up
    scanlines.push_back(2);
    for (int i = 0; i < w; ++i) scanlines.push_back(1);
    // Row 2: Paeth (scalar Paeth path)
    scanlines.push_back(4);
    for (int i = 0; i < w; ++i) scanlines.push_back(2);

    const std::string path = std::format("{}/filters_gray_scalar.png", kVsiDir);
    VsiMemTempWrapper file(path, MakeCustomPng(w, h, 8, 0, 0, scanlines));

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> out(h * (w + 4) * 2);
    int band_map[1] = {1};
    // Non-tight buffer (!abyTemp.empty())
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, w, h, out.data(), w, h,
                                    GDT_Byte, 1, band_map, 2, (w + 4) * 2, 1,
                                    nullptr));
  }

  // 4. Invalid filter type 5 returns CE_Failure
  {
    const int w = 2;
    const int h = 1;
    std::vector<uint8_t> scanlines = {5, 10, 20};  // Filter type 5 invalid!
    const std::string path = std::format("{}/invalid_filter.png", kVsiDir);
    VsiMemTempWrapper file(path, MakeCustomPng(w, h, 8, 0, 0, scanlines));

    WithQuietHandler quiet;
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    uint8_t out[2] = {};
    int band_map[1] = {1};
    EXPECT_EQ(CE_Failure, ds->RasterIO(GF_Read, 0, 0, w, h, out, w, h,
                                       GDT_Byte, 1, band_map, 1, w, 1,
                                       nullptr));
  }

  // 5. Corrupt zlib inflate in LoadWholeImage
  {
    const std::string path = std::format("{}/corrupt_inflate.png", kVsiDir);
    const std::string_view bad_zlib = "\x78\x9c\xff\xff_BAD_ZLIB";
    VsiMemTempWrapper file(
        path, MakeCustomPng(2, 2, 8, 0, 0, {}, {}, {{bad_zlib}}));

    WithQuietHandler quiet;
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    uint8_t out[4] = {};
    int band_map[1] = {1};
    EXPECT_EQ(CE_Failure, ds->RasterIO(GF_Read, 0, 0, 2, 2, out, 2, 2,
                                       GDT_Byte, 1, band_map, 1, 2, 1,
                                       nullptr));
  }

  // 6. Split IDAT chunks exercising abyCompressedData.resize and memory limit
  {
    const int w = 4;
    const int h = 2;
    std::vector<uint8_t> scanlines = {0, 1, 2, 3, 4, 0, 5, 6, 7, 8};
    std::vector<uint8_t> comp(128);
    size_t comp_size = 0;
    CPLZLibDeflate(scanlines.data(), scanlines.size(), 6, comp.data(),
                   comp.size(), &comp_size);
    ASSERT_GT(comp_size, 4);

    const std::string_view part1(reinterpret_cast<char*>(comp.data()), 4);
    const std::string_view part2(reinterpret_cast<char*>(comp.data() + 4),
                                 comp_size - 4);

    const std::string path = std::format("{}/split_idat.png", kVsiDir);
    VsiMemTempWrapper file(
        path, MakeCustomPng(w, h, 8, 0, 0, {}, {}, {{part1, part2}}));

    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY, nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());
    uint8_t out[8] = {};
    int band_map[1] = {1};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, w, h, out, w, h, GDT_Byte,
                                    1, band_map, 1, w, 1, nullptr));
    EXPECT_EQ(1, out[0]);
    EXPECT_EQ(8, out[7]);
  }
}

// ============================================================================
// Low-Level VSI Errors via ControllableVsiHandler
// ============================================================================

TEST(PngDatasetTest, LowLevelVsiErrors) {
  ControllableVsiHandler handler;
  VSIFileManager::InstallHandler("/vsictrl_png/", &handler);
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler("/vsictrl_png/");
  };

  GDALRegister_PNG();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("PNG");
  ASSERT_THAT(driver, NotNull());

  WithQuietHandler quiet;

  // 1. File open failure during CreateCopy
  {
    handler.Reset();
    handler.fail_open_ = true;
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    EXPECT_THAT(driver->CreateCopy("/vsictrl_png/fail_open.png", mem_ds.get(),
                                  FALSE, nullptr, GDALDummyProgress, nullptr),
                IsNull());
  }

  // 2. Write error during header creation
  {
    handler.Reset();
    handler.fail_write_on_call_ = 1;
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    EXPECT_THAT(driver->CreateCopy("/vsictrl_png/fail_write.png", mem_ds.get(),
                                  FALSE, nullptr, GDALDummyProgress, nullptr),
                IsNull());
  }

  // 3. File close error during CreateCopy
  {
    handler.Reset();
    handler.fail_close_ = true;
    auto mem_ds = MakeMemDataset(1, 1, 1, GDT_Byte);
    EXPECT_THAT(driver->CreateCopy("/vsictrl_png/fail_close.png", mem_ds.get(),
                                  FALSE, nullptr, GDALDummyProgress, nullptr),
                IsNull());
  }

  // 4. Read error during image loading
  {
    handler.Reset();
    const std::vector<uint8_t> scanline = {0, 42};
    std::string valid_png = MakeCustomPng(1, 1, 8, 0, 0, scanline);
    handler.Reset(valid_png);
    handler.fail_read_on_call_ = 3;

    GDALOpenInfo open_info("/vsictrl_png/fail_read.png", GDAL_OF_READONLY,
                           nullptr);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    if (ds != nullptr) {
      uint8_t out = 0;
      int band_map[1] = {1};
      EXPECT_EQ(CE_Failure, ds->RasterIO(GF_Read, 0, 0, 1, 1, &out, 1, 1,
                                         GDT_Byte, 1, band_map, 1, 1, 1,
                                         nullptr));
    }
  }
}

// ============================================================================
// Targeted High-Coverage Optimization & Edge-Case Tests
// ============================================================================

TEST(PngDatasetTest, WholeImageOptimizationWideAndStridedFilters) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  // 1. 1-band (width=36, height=6) covering Filter 2 Up (SSE2 vector loop + tail),
  // Filter 3 Average (1-band abyTemp.empty()), and Filter 4 Paeth (all 3 decision branches).
  constexpr int width = 36;
  constexpr int height = 6;
  std::vector<uint8_t> raw_lines;

  // Line 0: Filter 2 (Up) on iY == 0 (hits memcpy at lines 706-707)
  raw_lines.push_back(2);
  for (int x = 0; x < width; ++x) {
    raw_lines.push_back(static_cast<uint8_t>(10 + x));
  }

  // Line 1: Filter 2 (Up) on iY > 0 with width 36 >= 32 samples (SSE2 loop + tail)
  raw_lines.push_back(2);
  for (int x = 0; x < width; ++x) {
    raw_lines.push_back(5);
  }

  // Line 2: Filter 3 (Average) on iY > 0 with nBands == 1 (lines 842-859)
  raw_lines.push_back(3);
  for (int x = 0; x < width; ++x) {
    raw_lines.push_back(static_cast<uint8_t>((x % 4) * 2));
  }

  // Line 3: Filter 4 (Paeth) with values exercising pa <= pb && pa <= pc, pb <= pc, and else
  raw_lines.push_back(4);
  // First sample (x = 0): no left neighbor, uses b (above)
  raw_lines.push_back(3);
  // Sample 1: designed for pb <= pc (b=100, c=0, a=10 -> pb=10, pa=100, pc=110)
  raw_lines.push_back(10);
  // Sample 2: designed for else (pc < pa && pc < pb)
  raw_lines.push_back(2);
  for (int x = 3; x < width; ++x) {
    raw_lines.push_back(static_cast<uint8_t>(x % 7));
  }

  // Line 4: Filter 0 (None)
  raw_lines.push_back(0);
  for (int x = 0; x < width; ++x) {
    raw_lines.push_back(static_cast<uint8_t>(x));
  }

  // Line 5: Filter 1 (Sub)
  raw_lines.push_back(1);
  for (int x = 0; x < width; ++x) {
    raw_lines.push_back(1);
  }

  const std::string path = std::format("{}/wide_filters.png", kVsiDir);
  VsiMemTempWrapper file(path, MakeCustomPng(width, height, 8, 0, 0, raw_lines));

  // Contiguous read (abyTemp.empty() is true)
  {
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> buf(width * height);
    int band_map[1] = {1};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, width, height, buf.data(),
                                    width, height, GDT_Byte, 1, band_map, 1,
                                    width, 1, nullptr));
    EXPECT_EQ(10, buf[0]);
    EXPECT_EQ(15, buf[width]);  // 10 + 5
  }

  // Strided read (abyTemp is NOT empty, exercising strided filter branches)
  {
    GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&open_info));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> buf(width * height * 2, 0);
    int band_map[1] = {1};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, width, height, buf.data(),
                                    width, height, GDT_Byte, 1, band_map,
                                    /*nPixelSpace=*/2,
                                    /*nLineSpace=*/width * 2,
                                    /*nBandSpace=*/1, nullptr));
    EXPECT_EQ(10, buf[0]);
    EXPECT_EQ(15, buf[width * 2]);
  }

  // 2. Line 0 Filter 3 (Average) and Filter 4 (Paeth) on iY == 0
  {
    const std::string p_avg0 = std::format("{}/avg0.png", kVsiDir);
    const std::vector<uint8_t> raw_avg = {3, 10, 20, 30, 40};
    VsiMemTempWrapper f_avg(p_avg0, MakeCustomPng(4, 1, 8, 0, 0, raw_avg));
    GDALOpenInfo oi(p_avg0.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());
    uint8_t out[4] = {};
    int bm[1] = {1};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 4, 1, out, 4, 1, GDT_Byte,
                                    1, bm, 1, 4, 1, nullptr));
    EXPECT_EQ(10, out[0]);
  }

  {
    const std::string p_paeth0 = std::format("{}/paeth0.png", kVsiDir);
    const std::vector<uint8_t> raw_paeth = {4, 15, 25, 35, 45};
    VsiMemTempWrapper f_paeth(p_paeth0, MakeCustomPng(4, 1, 8, 0, 0, raw_paeth));
    GDALOpenInfo oi(p_paeth0.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());
    uint8_t out[4] = {};
    int bm[1] = {1};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 4, 1, out, 4, 1, GDT_Byte,
                                    1, bm, 1, 4, 1, nullptr));
    EXPECT_EQ(15, out[0]);
  }

  // 3. 3-band and 4-band Filter 3 (Average) with strided layout (abyTemp not empty)
  {
    const std::string p_rgb_avg = std::format("{}/rgb_avg_strided.png", kVsiDir);
    // 2 rows of 8 pixels, 3 bands = 24 samples per line
    std::vector<uint8_t> rgb_scanlines;
    rgb_scanlines.push_back(0);  // row 0: none
    rgb_scanlines.resize(25, 10);
    rgb_scanlines.push_back(3);  // row 1: average
    rgb_scanlines.resize(50, 4);
    VsiMemTempWrapper f_rgb(p_rgb_avg, MakeCustomPng(8, 2, 8, 2, 0, rgb_scanlines));

    GDALOpenInfo oi(p_rgb_avg.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> buf(8 * 2 * 6, 0);
    int bm[3] = {1, 2, 3};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 8, 2, buf.data(), 8, 2,
                                    GDT_Byte, 3, bm, /*nPixelSpace=*/6,
                                    /*nLineSpace=*/48, /*nBandSpace=*/1,
                                    nullptr));
  }

  {
    const std::string p_rgba_avg = std::format("{}/rgba_avg_strided.png", kVsiDir);
    // 2 rows of 8 pixels, 4 bands = 32 samples per line
    std::vector<uint8_t> rgba_scanlines;
    rgba_scanlines.push_back(0);  // row 0: none
    rgba_scanlines.resize(33, 20);
    rgba_scanlines.push_back(3);  // row 1: average
    rgba_scanlines.resize(66, 8);
    VsiMemTempWrapper f_rgba(p_rgba_avg, MakeCustomPng(8, 2, 8, 6, 0, rgba_scanlines));

    GDALOpenInfo oi(p_rgba_avg.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> buf(8 * 2 * 8, 0);
    int bm[4] = {1, 2, 3, 4};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 8, 2, buf.data(), 8, 2,
                                    GDT_Byte, 4, bm, /*nPixelSpace=*/8,
                                    /*nLineSpace=*/64, /*nBandSpace=*/1,
                                    nullptr));
  }
}

TEST(PngDatasetTest, WholeImageOptimizationDeinterleaveCopyWordsAndChunkErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  // 1. 3-band 4x2 image testing Deinterleave and CopyWords in LoadWholeImage
  const std::string path_rgb = std::format("{}/deint_test.png", kVsiDir);
  std::vector<uint8_t> scanlines;
  // Row 0: Filter 0, 12 bytes
  scanlines.push_back(0);
  for (uint8_t i = 1; i <= 12; ++i) scanlines.push_back(i);
  // Row 1: Filter 0, 12 bytes
  scanlines.push_back(0);
  for (uint8_t i = 13; i <= 24; ++i) scanlines.push_back(i);

  VsiMemTempWrapper file_rgb(path_rgb, MakeCustomPng(4, 2, 8, 2, 0, scanlines));

  // Case A: bCanUseDeinterleave in LoadWholeImage (nPixelSpace=1, nBandSpace=8)
  {
    GDALOpenInfo oi(path_rgb.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());

    uint8_t buf[24] = {};
    int bm[3] = {1, 2, 3};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 4, 2, buf, 4, 2, GDT_Byte,
                                    3, bm, /*nPixelSpace=*/1,
                                    /*nLineSpace=*/4, /*nBandSpace=*/8,
                                    nullptr));
    EXPECT_EQ(1, buf[0]);
    EXPECT_EQ(2, buf[8]);
    EXPECT_EQ(3, buf[16]);
  }

  // Case B: GDALCopyWords in LoadWholeImage (nPixelSpace <= nBands && nBandSpace > nBands)
  {
    GDALOpenInfo oi(path_rgb.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());

    uint8_t buf[64] = {};
    int bm[3] = {1, 2, 3};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 4, 2, buf, 4, 2, GDT_Byte,
                                    3, bm, /*nPixelSpace=*/2,
                                    /*nLineSpace=*/8, /*nBandSpace=*/16,
                                    nullptr));
    EXPECT_EQ(1, buf[0]);
    EXPECT_EQ(2, buf[16]);
  }

  // 2. 2-band Gray+Alpha image: PNGRasterBand::IRasterIO calling LoadWholeImage with band buffers
  {
    const std::string path_ga = std::format("{}/gray_alpha_bands.png", kVsiDir);
    std::vector<uint8_t> ga_scanlines = {
        0, 10, 255, 20, 255, 30, 255, 40, 255,  // Row 0
        0, 50, 255, 60, 255, 70, 255, 80, 255,  // Row 1
    };
    VsiMemTempWrapper file_ga(path_ga, MakeCustomPng(4, 2, 8, 4, 0, ga_scanlines));

    GDALOpenInfo oi(path_ga.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    ASSERT_THAT(ds, NotNull());

    uint8_t band1_buf[8] = {};
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 2, band1_buf, 4, 2, GDT_Byte, 1, 4,
                           nullptr));
    EXPECT_EQ(10, band1_buf[0]);
    EXPECT_EQ(20, band1_buf[1]);
  }

  // 3. Chunk walking errors in LoadWholeImage
  const std::vector<uint8_t> valid_scanlines = {0, 1, 2, 0, 3, 4};
  const std::string valid_png = MakeCustomPng(2, 2, 8, 0, 0, valid_scanlines);
  // Strip IEND (last 12 bytes)
  ASSERT_GE(valid_png.size(), 12u);
  const std::string no_iend = valid_png.substr(0, valid_png.size() - 12);

  // Error A: Unexpected EOF reading next chunk size
  {
    const std::string p_err1 = std::format("{}/err_eof.png", kVsiDir);
    VsiMemTempWrapper f_err1(p_err1, no_iend);
    WithQuietHandler quiet;
    GDALOpenInfo oi(p_err1.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    if (ds != nullptr) {
      uint8_t out[4] = {};
      int bm[1] = {1};
      CPLErr err = ds->RasterIO(GF_Read, 0, 0, 2, 2, out, 2, 2, GDT_Byte, 1,
                                bm, 1, 2, 1, nullptr);
      int bx = 0, by = 0;
      ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
      if (by > 1) {
        EXPECT_EQ(CE_Failure, err);
      }
    }
  }

  // Error B: Incomplete chunk header (4 bytes of size, no type)
  {
    const std::string p_err2 = std::format("{}/err_trunc_hdr.png", kVsiDir);
    std::string truncated = no_iend;
    truncated.append("\x00\x00\x00\x04", 4);
    VsiMemTempWrapper f_err2(p_err2, truncated);
    WithQuietHandler quiet;
    GDALOpenInfo oi(p_err2.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    if (ds != nullptr) {
      uint8_t out[4] = {};
      int bm[1] = {1};
      CPLErr err = ds->RasterIO(GF_Read, 0, 0, 2, 2, out, 2, 2, GDT_Byte, 1,
                                bm, 1, 2, 1, nullptr);
      int bx = 0, by = 0;
      ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
      if (by > 1) {
        EXPECT_EQ(CE_Failure, err);
      }
    }
  }

  // Error C: Huge IDAT claiming > 100MB
  {
    const std::string p_err3 = std::format("{}/err_huge_idat.png", kVsiDir);
    std::string huge = no_iend;
    huge.append("\x0C\x80\x00\x00IDAT", 8);  // 209715200 bytes
    VsiMemTempWrapper f_err3(p_err3, huge);
    WithQuietHandler quiet;
    GDALOpenInfo oi(p_err3.c_str(), GDAL_OF_READONLY);
    std::unique_ptr<GDALDataset> ds(PNGDataset::Open(&oi));
    if (ds != nullptr) {
      uint8_t out[4] = {};
      int bm[1] = {1};
      CPLErr err = ds->RasterIO(GF_Read, 0, 0, 2, 2, out, 2, 2, GDT_Byte, 1,
                                bm, 1, 2, 1, nullptr);
      int bx = 0, by = 0;
      ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
      if (by > 1) {
        EXPECT_EQ(CE_Failure, err);
      }
    }
  }
}

// TODO: b/335317901 - PNGDataset::Create() and supporting methods (IWriteBlock,
// SetGeoTransform) are guarded by #ifdef SUPPORT_CREATE which is commented out
// in pngdataset.cpp. Add Create tests if writable creation is ever enabled.

}  // namespace
}  // namespace autotest2
