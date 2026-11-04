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
// Tests the GDAL WEBP dataset and raster band implementation (webpdataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/webp/webpdataset.cpp
//   https://gdal.org/drivers/raster/webp.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/webp.py

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
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
using ::testing::IsEmpty;
using ::testing::Not;

constexpr char kTestDir[] = "/vsimem/webpdataset_test";

constexpr uint8_t ExpectedPixel(int x, int y, int b) {
  return static_cast<uint8_t>(x * 30 + y * 50 + b * 40);
}

constexpr uint8_t ExpectedAlpha(int x, int y) {
  return static_cast<uint8_t>((x * 40 + y * 60 + 100) % 250);
}

void PatchLe32(void *ptr, uint32_t val) {
  uint32_t val_le = val;
  CPL_LSBPTR32(&val_le);
  memcpy(ptr, &val_le, sizeof(val_le));
}

GDALDatasetUniquePtr CreateMemSource(int width, int height, int bands,
                                     GDALDataType dt = GDT_Byte) {
  GDALDriver *mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_driver == nullptr) {
    return nullptr;
  }
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", width, height, bands, dt, nullptr));
  if (ds == nullptr) {
    return nullptr;
  }

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand *band = ds->GetRasterBand(b);
    std::vector<uint8_t> data(width * height);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        if (b == 4) {
          // Alpha band: use non-255 values so libwebp preserves alpha channel.
          data[y * width + x] = ExpectedAlpha(x, y);
        } else {
          data[y * width + x] = ExpectedPixel(x, y, b);
        }
      }
    }
    if (band->RasterIO(GF_Write, 0, 0, width, height, data.data(), width,
                       height, GDT_Byte, 0, 0) != CE_None) {
      return nullptr;
    }
  }
  return ds;
}

// In-memory controllable VSI filesystem handler for simulating boundary file
// sizes and file-close failures without huge allocations or disk I/O.
// TODO: b/335317901 - Hoist ControllableVsiHandler into util/ if shared.
class ControllableVsiHandler final : public VSIFilesystemHandler {
 public:
  class Handle final : public VSIVirtualHandle {
   public:
    Handle(ControllableVsiHandler *handler, bool read_mode)
        : handler_(handler), read_mode_(read_mode) {}

    int Seek(vsi_l_offset offset, int whence) override {
      if (whence == SEEK_SET) {
        offset_ = offset;
      } else if (whence == SEEK_CUR) {
        offset_ += offset;
      } else if (whence == SEEK_END) {
        const vsi_l_offset end_size = handler_->virtual_size_ > 0
                                          ? handler_->virtual_size_
                                          : handler_->buffer_.size();
        offset_ = end_size + offset;
      }
      return 0;
    }

    vsi_l_offset Tell() override { return offset_; }

    size_t Read(void *buffer, size_t size, size_t count) override {
      if (!read_mode_ || offset_ >= handler_->buffer_.size()) {
        eof_ = true;
        return 0;
      }
      const size_t total = size * count;
      const size_t avail =
          static_cast<size_t>(handler_->buffer_.size() - offset_);
      const size_t to_copy = std::min(total, avail);
      if (to_copy < total) {
        eof_ = true;
      }
      if (to_copy > 0) {
        memcpy(buffer, handler_->buffer_.data() + offset_, to_copy);
        offset_ += to_copy;
      }
      return size > 0 ? to_copy / size : 0;
    }

    size_t Write(const void *buffer, size_t size, size_t count) override {
      const size_t total = size * count;
      const char *chars = static_cast<const char *>(buffer);
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
    ControllableVsiHandler *handler_;
    bool read_mode_;
    vsi_l_offset offset_ = 0;
    bool eof_ = false;
  };

  VSIVirtualHandleUniquePtr Open(const char * /*filename*/, const char *access,
                                 bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    const bool is_read = strchr(access, 'r') != nullptr;
    if (!is_read) {
      buffer_.clear();
    }
    return VSIVirtualHandleUniquePtr(new Handle(this, is_read));
  }

  int Stat(const char * /*pszFilename*/, VSIStatBufL *stat_buf,
           int /*nFlags*/) override {
    memset(stat_buf, 0, sizeof(*stat_buf));
    stat_buf->st_mode = S_IFREG;
    stat_buf->st_size = virtual_size_ > 0 ? virtual_size_ : buffer_.size();
    return 0;
  }

  int Unlink(const char * /*pszFilename*/) override {
    buffer_.clear();
    return 0;
  }

  void Reset() {
    buffer_.clear();
    virtual_size_ = 0;
    fail_close_ = false;
  }

  std::string buffer_;
  vsi_l_offset virtual_size_ = 0;
  bool fail_close_ = false;
};

class WebpDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_WEBP();
    GDALRegister_MEM();
    driver_ = GetGDALDriverManager()->GetDriverByName("WEBP");
    ASSERT_NE(nullptr, driver_);
    control_handler_.Reset();
    VSIFileManager::InstallHandler(kControlPrefix, &control_handler_);
  }

  void TearDown() override {
    VSIFileManager::RemoveHandler(kControlPrefix);
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }

  static constexpr char kControlPrefix[] = "/vsicontrol_webp/";
  ControllableVsiHandler control_handler_;
  GDALDriver *driver_ = nullptr;
};

TEST_F(WebpDatasetTest, RegisterAgain) {
  // Calling GDALRegister_WEBP when already registered is a no-op.
  GDALRegister_WEBP();
  EXPECT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("WEBP"));
}

TEST_F(WebpDatasetTest, OpenErrors) {
  // 1. Non-existent file.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx("/does_not_exist.webp", GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    EXPECT_EQ(nullptr, ds);
  }

  // 2. 20-byte file that passes Identify but fails WebPGetInfo.
  {
    const std::string filename = std::string(kTestDir) + "/invalid_webp.webp";
    constexpr std::string_view kInvalidHeader(
        "RIFF\x0c\x00\x00\x00WEBPVP8 \x00\x00\x00\x00", 20);
    VsiMemTempWrapper wrapper(filename, std::string(kInvalidHeader));

    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(filename.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, ds);
  }

  // 3. Open with GA_Update fails (WEBP driver is read-only for Open).
  {
    auto src_ds = CreateMemSource(4, 3, 3);
    ASSERT_NE(nullptr, src_ds);
    const std::string filename = std::string(kTestDir) + "/update_test.webp";
    GDALDatasetUniquePtr webp_ds(driver_->CreateCopy(
        filename.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, webp_ds);
    webp_ds.reset();

    WithQuietHandler quiet;
    CPLErrorReset();
    auto update_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(filename.c_str(), GDAL_OF_UPDATE | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, update_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not support update access"));
  }

  // 4. Calling driver->pfnOpen with fpL == nullptr returns nullptr after Identify succeeds.
  {
    auto src_ds = CreateMemSource(4, 3, 3);
    ASSERT_NE(nullptr, src_ds);
    const std::string filename = std::string(kTestDir) + "/null_fpl_test.webp";
    GDALDatasetUniquePtr webp_ds(driver_->CreateCopy(
        filename.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, webp_ds);
    webp_ds.reset();

    GDALOpenInfo info(filename.c_str(), GDAL_OF_READONLY);
    ASSERT_NE(nullptr, info.fpL);
    EXPECT_EQ(0, VSIFCloseL(info.fpL));
    info.fpL = nullptr;
    EXPECT_EQ(nullptr, driver_->pfnOpen(&info));
  }
}

TEST_F(WebpDatasetTest, RgbLossyAndLosslessRoundTrip) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 3;
  constexpr int kBands = 3;
  auto src_ds = CreateMemSource(kWidth, kHeight, kBands);
  ASSERT_NE(nullptr, src_ds);

  const std::string lossless_file = std::string(kTestDir) + "/rgb_lossless.webp";
  const std::string lossy_file = std::string(kTestDir) + "/rgb_lossy.webp";

  // Create lossless WebP
  {
    CPLStringList options;
    options.AddNameValue("LOSSLESS", "YES");
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        lossless_file.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // Create lossy WebP
  {
    CPLStringList options;
    options.AddNameValue("QUALITY", "75");
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        lossy_file.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // Verify lossy dataset reversibility metadata
  {
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(lossy_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("LOSSY",
                 ds->GetMetadataItem("COMPRESSION_REVERSIBILITY", "IMAGE_STRUCTURE"));
  }

  // Verify lossless dataset basics, block read, and raster IO
  {
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(lossless_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("LOSSLESS",
                 ds->GetMetadataItem("COMPRESSION_REVERSIBILITY", "IMAGE_STRUCTURE"));
    EXPECT_EQ(kWidth, ds->GetRasterXSize());
    EXPECT_EQ(kHeight, ds->GetRasterYSize());
    EXPECT_EQ(kBands, ds->GetRasterCount());
    EXPECT_EQ(0, ds->GetGCPCount());

    GDALGeoTransform gt;
    EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));

    // Color interpretations
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());

    // Band block size & type
    int block_x = 0;
    int block_y = 0;
    ds->GetRasterBand(1)->GetBlockSize(&block_x, &block_y);
    EXPECT_EQ(kWidth, block_x);
    EXPECT_EQ(1, block_y);
    EXPECT_EQ(GDT_Byte, ds->GetRasterBand(1)->GetRasterDataType());
    EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
    EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetColorTable());

    // 1. IReadBlock
    for (int b = 1; b <= kBands; ++b) {
      GDALRasterBand *band = ds->GetRasterBand(b);
      GDALRasterBand *src_band = src_ds->GetRasterBand(b);
      std::vector<uint8_t> block_buf(kWidth);
      std::vector<uint8_t> expected_buf(kWidth);
      for (int y = 0; y < kHeight; ++y) {
        ASSERT_EQ(CE_None, band->ReadBlock(0, y, block_buf.data()));
        ASSERT_EQ(CE_None, src_band->RasterIO(GF_Read, 0, y, kWidth, 1,
                                              expected_buf.data(), kWidth, 1,
                                              GDT_Byte, 0, 0));
        EXPECT_EQ(expected_buf, block_buf)
            << "Mismatch at band " << b << " row " << y;
      }
    }

    // 2. IRasterIO pixel-interleaved fast path (memcpy)
    {
      std::vector<uint8_t> pixel_interleaved(kWidth * kHeight * kBands, 0);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight,
                             pixel_interleaved.data(), kWidth, kHeight,
                             GDT_Byte, kBands, nullptr, kBands,
                             kBands * kWidth, 1, nullptr));
      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          for (int b = 0; b < kBands; ++b) {
            uint8_t expected =
                static_cast<uint8_t>((x * 30 + y * 50 + (b + 1) * 40) % 256);
            EXPECT_EQ(expected, pixel_interleaved[(y * kWidth + x) * kBands + b]);
          }
        }
      }
    }

    // 3. IRasterIO strided path
    {
      std::vector<uint8_t> band_sequential(kWidth * kHeight * kBands, 0);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight,
                             band_sequential.data(), kWidth, kHeight, GDT_Byte,
                             kBands, nullptr, 1, kWidth, kWidth * kHeight,
                             nullptr));
      for (int b = 0; b < kBands; ++b) {
        for (int y = 0; y < kHeight; ++y) {
          for (int x = 0; x < kWidth; ++x) {
            uint8_t expected =
                static_cast<uint8_t>((x * 30 + y * 50 + (b + 1) * 40) % 256);
            EXPECT_EQ(expected,
                      band_sequential[b * kWidth * kHeight + y * kWidth + x]);
          }
        }
      }
    }

    // 4. IRasterIO subwindow fallback path
    {
      std::vector<uint8_t> subwindow(2 * 2 * kBands, 0xaa);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Read, 1, 1, 2, 2, subwindow.data(), 2, 2,
                             GDT_Byte, kBands, nullptr, kBands, 2 * kBands, 1,
                             nullptr));
      for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
          for (int b = 0; b < kBands; ++b) {
            EXPECT_EQ(ExpectedPixel(x + 1, y + 1, b + 1),
                      subwindow[(y * 2 + x) * kBands + b]);
          }
        }
      }
    }

    // Explicit Close
    EXPECT_EQ(CE_None, ds->Close());
  }

  // Open with open option FORCE_4BANDS=YES
  {
    const char *open_options[] = {"FORCE_4BANDS=YES", nullptr};
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(lossless_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, open_options, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(4)->GetColorInterpretation());

    std::vector<uint8_t> alpha_data(kWidth * kHeight);
    ASSERT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, alpha_data.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    for (uint8_t val : alpha_data) {
      EXPECT_EQ(255, val);
    }
  }
}

TEST_F(WebpDatasetTest, RgbaFourBands) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 3;
  constexpr int kBands = 4;
  auto src_ds = CreateMemSource(kWidth, kHeight, kBands);
  ASSERT_NE(nullptr, src_ds);

  const std::string filename = std::string(kTestDir) + "/rgba_lossless.webp";
  CPLStringList options;
  options.AddNameValue("LOSSLESS", "YES");
  options.AddNameValue("EXACT", "1");
  GDALDatasetUniquePtr ds(driver_->CreateCopy(
      filename.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  auto reloaded_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, reloaded_ds);
  EXPECT_EQ(4, reloaded_ds->GetRasterCount());
  ASSERT_NE(nullptr, reloaded_ds->GetRasterBand(4));
  EXPECT_EQ(GCI_AlphaBand,
            reloaded_ds->GetRasterBand(4)->GetColorInterpretation());

  // Reading uncompresses via WebPDecodeRGBAInto
  std::vector<uint8_t> data(kWidth * kHeight * kBands);
  ASSERT_EQ(CE_None,
            reloaded_ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, data.data(),
                                  kWidth, kHeight, GDT_Byte, kBands, nullptr,
                                  kBands, kBands * kWidth, 1, nullptr));
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      uint8_t expected_alpha =
          static_cast<uint8_t>((x * 40 + y * 60 + 100) % 250);
      EXPECT_EQ(expected_alpha, data[(y * kWidth + x) * kBands + 3]);
    }
  }
}

TEST_F(WebpDatasetTest, UncompressFailure) {
  auto src_ds = CreateMemSource(4, 3, 3);
  ASSERT_NE(nullptr, src_ds);
  const std::string filename = std::string(kTestDir) + "/valid_source.webp";
  CPLStringList options;
  options.AddNameValue("LOSSLESS", "YES");
  GDALDatasetUniquePtr ds(driver_->CreateCopy(
      filename.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  // Read valid WebP data into memory
  vsi_l_offset file_size = 0;
  GByte *file_bytes = VSIGetMemFileBuffer(filename.c_str(), &file_size, FALSE);
  ASSERT_NE(nullptr, file_bytes);
  ASSERT_GT(file_size, 25);

  // Preserve the container and VP8L header (first 25 bytes), but corrupt
  // the image bitstream from byte 25 onward. WebPGetFeatures() succeeds,
  // but WebPDecodeRGBInto() fails.
  std::string corrupted_data(reinterpret_cast<char *>(file_bytes), file_size);
  for (size_t i = 25; i < corrupted_data.size(); ++i) {
    corrupted_data[i] = static_cast<char>(0xff);
  }

  const std::string corrupt_filename = std::string(kTestDir) + "/corrupt.webp";
  VsiMemTempWrapper wrapper(corrupt_filename, corrupted_data);

  auto corrupt_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(corrupt_filename.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, corrupt_ds);

  std::vector<uint8_t> buf(4);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              corrupt_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("WebPDecodeRGBInto() failed"));

    // Second call returns cached failure immediately
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              corrupt_ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
  }

  // Opening another handle and calling RasterIO directly triggers uncompress failure
  {
    auto corrupt_ds2 = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(corrupt_filename.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, corrupt_ds2);
    std::vector<uint8_t> all_buf(4 * 3 * 3);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              corrupt_ds2->RasterIO(GF_Read, 0, 0, 4, 3, all_buf.data(), 4, 3,
                                    GDT_Byte, 3, nullptr, 3, 12, 1, nullptr));
  }
}

TEST_F(WebpDatasetTest, WorldFilesAndGetFileList) {
  auto src_ds = CreateMemSource(4, 3, 3);
  ASSERT_NE(nullptr, src_ds);
  const GDALGeoTransform gt_orig = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, src_ds->SetGeoTransform(gt_orig));

  const std::string filename = std::string(kTestDir) + "/with_wld.webp";
  CPLStringList options;
  options.AddNameValue("WORLDFILE", "YES");
  GDALDatasetUniquePtr ds(driver_->CreateCopy(
      filename.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  // Reopen and check GetGeoTransform and GetFileList
  auto reloaded_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, reloaded_ds);

  GDALGeoTransform gt_read;
  EXPECT_EQ(CE_None, reloaded_ds->GetGeoTransform(gt_read));
  EXPECT_EQ(gt_orig, gt_read);

  char **raw_file_list = reloaded_ds->GetFileList();
  ASSERT_NE(nullptr, raw_file_list);
  const CPLStringList files(raw_file_list, /*bTakeOwnership=*/TRUE);
  EXPECT_THAT(CslToVector(files.List()), Contains(filename));
  EXPECT_THAT(CslToVector(files.List()),
              Contains(std::string(kTestDir) + "/with_wld.wld"));

  // Test .wpw and .webpw worldfile extensions
  constexpr std::string_view kWorldExts[] = {"wpw", "webpw"};
  for (std::string_view ext : kWorldExts) {
    const std::string ext_webp =
        std::string(kTestDir) + "/test_" + std::string(ext) + ".webp";
    const std::string ext_file = std::string(kTestDir) + "/test_" +
                                 std::string(ext) + "." + std::string(ext);
    ASSERT_EQ(0, CPLCopyFile(ext_webp.c_str(), filename.c_str()));
    ASSERT_EQ(0, CPLCopyFile(ext_file.c_str(),
                             (std::string(kTestDir) + "/with_wld.wld").c_str()));
    auto ext_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(ext_webp.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ext_ds);
    GDALGeoTransform gt_ext;
    EXPECT_EQ(CE_None, ext_ds->GetGeoTransform(gt_ext));
    EXPECT_EQ(gt_orig, gt_ext);
  }
}

TEST_F(WebpDatasetTest, XmpMetadata) {
  // 1. Synthesize a WebP file containing VP8X + META + VP8L chunks.
  // First create a small valid lossless WebP to obtain a valid VP8L chunk.
  auto src_ds = CreateMemSource(4, 3, 3);
  ASSERT_NE(nullptr, src_ds);
  const std::string webp_src_file = std::string(kTestDir) + "/src_for_xmp.webp";
  {
    CPLStringList options;
    options.AddNameValue("LOSSLESS", "YES");
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        webp_src_file.c_str(), src_ds.get(), FALSE, options.List(), nullptr,
        nullptr));
    ASSERT_NE(nullptr, ds);
  }

  vsi_l_offset src_size = 0;
  GByte *src_bytes = VSIGetMemFileBuffer(webp_src_file.c_str(), &src_size, FALSE);
  ASSERT_NE(nullptr, src_bytes);
  ASSERT_GT(src_size, 12);
  std::string vp8l_chunk(reinterpret_cast<char *>(src_bytes) + 12,
                         src_size - 12);

  // VP8X chunk: 4-byte ID + 4-byte size (10) + 4-byte flags (0x08 for XMP) +
  // 3-byte width-1 (3) + 3-byte height-1 (2) = 18 bytes
  constexpr auto kVp8x = std::to_array<uint8_t>({
      'V', 'P', '8', 'X',
      0x0a, 0x00, 0x00, 0x00,  // chunk size = 10
      // flags: bit 3 (0x08). Note: the WebP spec defines bit 2 (0x04) for XMP
      // and bit 3 (0x08) for EXIF, and FourCC "XMP " instead of legacy "META",
      // but GDAL's ParseXMP tests (nFlags & 8) and matches "META".
      0x08, 0x00, 0x00, 0x00,
      0x03, 0x00, 0x00,        // width - 1 = 3 (4 pixels)
      0x02, 0x00, 0x00,        // height - 1 = 2 (3 pixels)
  });
  static_assert(kVp8x.size() == 18);
  std::string vp8x_chunk(reinterpret_cast<const char *>(kVp8x.data()),
                         kVp8x.size());

  // META chunk: 4-byte ID + 4-byte size + XMP content + optional pad byte
  constexpr char kXmpText[] = "<x:xmpmeta>test</x:xmpmeta>";
  uint32_t xmp_len = sizeof(kXmpText) - 1;
  uint32_t xmp_len_le = xmp_len;
  CPL_LSBPTR32(&xmp_len_le);
  std::string meta_chunk = "META";
  meta_chunk.append(reinterpret_cast<const char *>(&xmp_len_le), 4);
  meta_chunk.append(kXmpText, xmp_len);
  if (xmp_len % 2 != 0) {
    meta_chunk.push_back('\0');
  }

  std::string payload = "WEBP";
  payload.append(vp8x_chunk);
  payload.append(meta_chunk);
  payload.append(vp8l_chunk);

  uint32_t riff_payload_len = payload.size();
  CPL_LSBPTR32(&riff_payload_len);
  std::string total_file = "RIFF";
  total_file.append(reinterpret_cast<const char *>(&riff_payload_len), 4);
  total_file.append(payload);

  const std::string xmp_file = std::string(kTestDir) + "/with_xmp.webp";
  VsiMemTempWrapper wrapper(xmp_file, total_file);

  auto xmp_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(xmp_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, xmp_ds);

  // GetMetadataDomainList includes xml:XMP
  const CPLStringList domains(xmp_ds->GetMetadataDomainList(),
                              /*bTakeOwnership=*/TRUE);
  ASSERT_FALSE(domains.empty());
  EXPECT_THAT(CslToVector(domains.List()), Contains("xml:XMP"));

  // GetMetadata("xml:XMP") returns the XMP chunk
  char **xmp_md = xmp_ds->GetMetadata("xml:XMP");
  ASSERT_NE(nullptr, xmp_md);
  EXPECT_THAT(CslToVector(xmp_md), ElementsAre(kXmpText));

  // Second call returns cached metadata
  char **xmp_md2 = xmp_ds->GetMetadata("xml:XMP");
  ASSERT_NE(nullptr, xmp_md2);
  EXPECT_THAT(CslToVector(xmp_md2), ElementsAre(kXmpText));

  // 2. File without XMP returns nullptr
  auto no_xmp_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(webp_src_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, no_xmp_ds);
  EXPECT_EQ(nullptr, no_xmp_ds->GetMetadata("xml:XMP"));

  // 3. VP8X with nChunkSize < 10 breaks cleanly (mutated in-place after
  // GDALOpenEx because WebPGetInfo rejects a 4-byte VP8X chunk at open time).
  {
    const std::string bad_vp8x_file = std::string(kTestDir) + "/bad_vp8x.webp";
    VsiMemTempWrapper wrapper_bad(bad_vp8x_file, total_file);
    auto bad_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(bad_vp8x_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, bad_ds);
    vsi_l_offset sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(bad_vp8x_file.c_str(), &sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    ASSERT_GE(sz, 20);
    PatchLe32(bytes + 16, 4);
    EXPECT_EQ(nullptr, bad_ds->GetMetadata("xml:XMP"));
  }

  // 4. In-place mutations of underlying file buffer after opening:
  // a) Non-META chunk skipped and EOF reached at chunk header.
  {
    const std::string fn = std::string(kTestDir) + "/xmp_skip_chunk.webp";
    VsiMemTempWrapper wrap(fn, total_file);
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(fn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    vsi_l_offset sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(fn.c_str(), &sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    size_t mpos = total_file.find("META");
    ASSERT_NE(std::string::npos, mpos);
    memcpy(bytes + mpos, "ICCP", 4);
    EXPECT_EQ(nullptr, ds->GetMetadata("xml:XMP"));
  }

  // b) Truncated file when reading VP8X flags.
  {
    const std::string fn = std::string(kTestDir) + "/xmp_trunc_flags.webp";
    VsiMemTempWrapper wrap(fn, total_file);
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(fn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    VSIVirtualHandle *fp = VSIFOpenL(fn.c_str(), "rb+");
    ASSERT_NE(nullptr, fp);
    {
      absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
      EXPECT_EQ(0, VSIFTruncateL(fp, 20));
    }
    EXPECT_EQ(nullptr, ds->GetMetadata("xml:XMP"));
  }

  // c) VP8X without XMP flag (bit 3 == 0).
  {
    const std::string fn = std::string(kTestDir) + "/xmp_no_flag.webp";
    VsiMemTempWrapper wrap(fn, total_file);
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(fn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    vsi_l_offset sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(fn.c_str(), &sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    ASSERT_GT(sz, 20);
    bytes[20] = 0;
    EXPECT_EQ(nullptr, ds->GetMetadata("xml:XMP"));
  }

  // d) VP8X with META chunk size > 1MB.
  {
    const std::string fn = std::string(kTestDir) + "/xmp_huge_chunk.webp";
    VsiMemTempWrapper wrap(fn, total_file);
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(fn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    vsi_l_offset sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(fn.c_str(), &sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    size_t mpos = total_file.find("META");
    ASSERT_NE(std::string::npos, mpos);
    PatchLe32(bytes + mpos + 4, 1024 * 1024 + 1);
    EXPECT_EQ(nullptr, ds->GetMetadata("xml:XMP"));
  }

  // e) Truncated META chunk payload.
  {
    const std::string fn = std::string(kTestDir) + "/xmp_trunc_meta.webp";
    VsiMemTempWrapper wrap(fn, total_file);
    auto ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(fn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    vsi_l_offset sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(fn.c_str(), &sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    size_t mpos = total_file.find("META");
    ASSERT_NE(std::string::npos, mpos);
    PatchLe32(bytes + mpos + 4, 500);
    EXPECT_EQ(nullptr, ds->GetMetadata("xml:XMP"));
  }

  // 5. ReadCompressedData on dataset where META chunk precedes VP8L.
  {
    void *pbuf = nullptr;
    size_t size = 0;
    ASSERT_EQ(CE_None,
              xmp_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                         &pbuf, &size, nullptr));
    ASSERT_NE(nullptr, pbuf);
    absl::Cleanup free_pbuf = [pbuf] { VSIFree(pbuf); };
    std::string_view sv(reinterpret_cast<const char *>(pbuf), size);
    EXPECT_THAT(sv, Not(HasSubstr("META")));
  }
}

TEST_F(WebpDatasetTest, CompressionFormatsAndReadCompressedData) {
  auto src_ds = CreateMemSource(4, 3, 3);
  ASSERT_NE(nullptr, src_ds);
  const std::string filename = std::string(kTestDir) + "/comp_formats.webp";
  GDALDatasetUniquePtr ds(driver_->CreateCopy(
      filename.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  auto reloaded_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, reloaded_ds);

  // GetCompressionFormats
  EXPECT_THAT(CslToVector(reloaded_ds->GetCompressionFormats(0, 0, 4, 3, 3, nullptr).List()),
              ElementsAre("WEBP"));
  EXPECT_THAT(CslToVector(reloaded_ds->GetCompressionFormats(1, 0, 3, 3, 3, nullptr).List()),
              IsEmpty());
  int single_band = 1;
  EXPECT_THAT(CslToVector(reloaded_ds->GetCompressionFormats(0, 0, 4, 3, 1, &single_band).List()),
              IsEmpty());

  // ReadCompressedData error paths
  {
    size_t buf_size = 0;
    // Subwindow fails
    EXPECT_EQ(CE_Failure, reloaded_ds->ReadCompressedData(
                              "WEBP", 1, 0, 3, 3, 3, nullptr, nullptr, &buf_size, nullptr));
    // Invalid format fails
    EXPECT_EQ(CE_Failure, reloaded_ds->ReadCompressedData(
                              "JPEG", 0, 0, 4, 3, 3, nullptr, nullptr, &buf_size, nullptr));
    // Multi-token format fails
    EXPECT_EQ(CE_Failure, reloaded_ds->ReadCompressedData(
                              "WEBP;FOO", 0, 0, 4, 3, 3, nullptr, nullptr, &buf_size, nullptr));
  }

  // Query size and format
  size_t query_size = 0;
  char *detailed_format = nullptr;
  ASSERT_EQ(CE_None, reloaded_ds->ReadCompressedData(
                         "WEBP", 0, 0, 4, 3, 3, nullptr, nullptr, &query_size,
                         &detailed_format));
  EXPECT_GT(query_size, 0);
  ASSERT_NE(nullptr, detailed_format);
  absl::Cleanup free_format = [detailed_format] { VSIFree(detailed_format); };
  EXPECT_STREQ("WEBP", detailed_format);

  // Read into caller-allocated buffer: error when buffer too small
  {
    void *small_buf = CPLMalloc(10);
    absl::Cleanup free_buf = [small_buf] { CPLFree(small_buf); };
    size_t small_size = 10;
    EXPECT_EQ(CE_Failure,
              reloaded_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                              &small_buf, &small_size, nullptr));
  }

  // Error when ppBuffer is non-null but pnBufferSize is null
  {
    void *buf = nullptr;
    EXPECT_EQ(CE_Failure,
              reloaded_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                              &buf, nullptr, nullptr));
  }

  // Read into caller-allocated buffer: success
  {
    std::vector<uint8_t> buffer(query_size);
    void *pbuf = buffer.data();
    size_t size = buffer.size();
    ASSERT_EQ(CE_None,
              reloaded_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                              &pbuf, &size, nullptr));
    EXPECT_EQ(query_size, size);
    EXPECT_EQ(0, memcmp(buffer.data(), "RIFF", 4));
  }

  // Read into driver-allocated buffer (*ppBuffer == nullptr)
  {
    void *pbuf = nullptr;
    size_t size = 0;
    ASSERT_EQ(CE_None,
              reloaded_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                              &pbuf, &size, nullptr));
    ASSERT_NE(nullptr, pbuf);
    absl::Cleanup free_pbuf = [pbuf] { VSIFree(pbuf); };
    EXPECT_EQ(query_size, size);
  }

  // ReadCompressedData and Uncompress with size > uint32_t max
  {
    vsi_l_offset file_sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(filename.c_str(), &file_sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    control_handler_.Reset();
    control_handler_.buffer_.assign(reinterpret_cast<char *>(bytes), file_sz);
    control_handler_.virtual_size_ = 5000000000ULL;

    const std::string ctrl_file = std::string(kControlPrefix) + "large.webp";
    auto sub_large_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(ctrl_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, sub_large_ds);
    size_t q_size = 0;
    EXPECT_EQ(CE_Failure,
              sub_large_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                               nullptr, &q_size, nullptr));
    std::vector<uint8_t> block_buf(4);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              sub_large_ds->GetRasterBand(1)->ReadBlock(0, 0, block_buf.data()));
  }

  // ReadCompressedData and Uncompress with truncated read
  {
    vsi_l_offset file_sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(filename.c_str(), &file_sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    control_handler_.Reset();
    control_handler_.buffer_.assign(reinterpret_cast<char *>(bytes), file_sz);
    control_handler_.virtual_size_ = 500;

    const std::string ctrl_file = std::string(kControlPrefix) + "short.webp";
    auto sub_short_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(ctrl_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, sub_short_ds);
    void *pbuf = nullptr;
    size_t size = 0;
    EXPECT_EQ(CE_Failure,
              sub_short_ds->ReadCompressedData("WEBP", 0, 0, 4, 3, 3, nullptr,
                                               &pbuf, &size, nullptr));
    EXPECT_EQ(nullptr, pbuf);
  }

  // Close failure exercises I/O error path on dataset close
  {
    vsi_l_offset file_sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(filename.c_str(), &file_sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    control_handler_.Reset();
    control_handler_.buffer_.assign(reinterpret_cast<char *>(bytes), file_sz);
    control_handler_.fail_close_ = true;

    const std::string ctrl_file =
        std::string(kControlPrefix) + "fail_close.webp";
    auto ds_close = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(ctrl_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds_close);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds_close->Close());
  }
}

TEST_F(WebpDatasetTest, ReadCompressedDataStripsMetaChunk) {
  // Test stripping of META box in ReadCompressedData
  auto src_ds = CreateMemSource(4, 3, 4);
  ASSERT_NE(nullptr, src_ds);
  const std::string base_file = std::string(kTestDir) + "/base_for_strip.webp";
  {
    CPLStringList options;
    options.AddNameValue("LOSSLESS", "YES");
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        base_file.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  auto webp_src = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(base_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, webp_src);
  // Odd length XMP text to test both META removal and padding
  CPLStringList md;
  md.AddString("XMP_123");
  ASSERT_EQ(CE_None, webp_src->SetMetadata(md.List(), "xml:XMP"));

  const std::string file_with_meta = std::string(kTestDir) + "/file_with_meta.webp";
  {
    CPLStringList copy_options;
    copy_options.AddNameValue("LOSSLESS_COPY", "YES");
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        file_with_meta.c_str(), webp_src.get(), FALSE, copy_options.List(), nullptr,
        nullptr));
    ASSERT_NE(nullptr, ds);
  }

  auto ds_meta = GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(file_with_meta.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, ds_meta);

  void *pbuf = nullptr;
  size_t size = 0;
  ASSERT_EQ(CE_None, ds_meta->ReadCompressedData("WEBP", 0, 0, 4, 3, 4, nullptr,
                                                 &pbuf, &size, nullptr));
  ASSERT_NE(nullptr, pbuf);
  absl::Cleanup free_pbuf = [pbuf] { VSIFree(pbuf); };

  // Buffer should not contain "META" box after stripping
  std::string_view data_sv(reinterpret_cast<const char *>(pbuf), size);
  EXPECT_THAT(data_sv, Not(HasSubstr("META")));

  // Corrupt META chunk size to exceed remaining buffer
  {
    const std::string bad_meta_file =
        std::string(kTestDir) + "/bad_meta_chunk_size.webp";
    ASSERT_EQ(0, CPLCopyFile(bad_meta_file.c_str(), file_with_meta.c_str()));
    vsi_l_offset sz = 0;
    GByte *bytes = VSIGetMemFileBuffer(bad_meta_file.c_str(), &sz, FALSE);
    ASSERT_NE(nullptr, bytes);
    size_t mpos = std::string_view(reinterpret_cast<const char *>(bytes), sz)
                      .find("META");
    ASSERT_NE(std::string_view::npos, mpos);
    PatchLe32(bytes + mpos + 4, 50000);

    auto bad_meta_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(bad_meta_file.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, bad_meta_ds);
    void *pbuf2 = nullptr;
    size_t size2 = 0;
    ASSERT_EQ(CE_None, bad_meta_ds->ReadCompressedData(
                           "WEBP", 0, 0, 4, 3, 4, nullptr, &pbuf2, &size2, nullptr));
    ASSERT_NE(nullptr, pbuf2);
    absl::Cleanup free_pbuf2 = [pbuf2] { VSIFree(pbuf2); };
  }
}

TEST_F(WebpDatasetTest, CreateCopyOptionsAndErrors) {
  auto src_ds = CreateMemSource(4, 3, 3);
  ASSERT_NE(nullptr, src_ds);

  // 1. Unwritable destination path in CreateCopy
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        "/does_not_exist_dir/out.webp", src_ds.get(), FALSE, nullptr, nullptr,
        nullptr));
    EXPECT_EQ(nullptr, ds);
  }

  // 2. LOSSLESS_COPY=YES requested on non-WEBP source dataset without LOSSLESS=YES
  {
    CPLStringList options;
    options.AddNameValue("LOSSLESS_COPY", "YES");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/lossless_copy_fail.webp").c_str(),
        src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("LOSSLESS_COPY=YES requested but not possible"));
  }

  // 3. Dimension limits > 16383
  {
    auto large_x = CreateMemSource(16384, 1, 3);
    ASSERT_NE(nullptr, large_x);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/large_x.webp").c_str(), large_x.get(), FALSE,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("WEBP maximum image dimensions are 16383 x 16383."));
  }
  {
    auto large_y = CreateMemSource(1, 16384, 3);
    ASSERT_NE(nullptr, large_y);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/large_y.webp").c_str(), large_y.get(), FALSE,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("WEBP maximum image dimensions are 16383 x 16383."));
  }

  // 4. Invalid band count
  {
    auto one_band = CreateMemSource(4, 3, 1);
    ASSERT_NE(nullptr, one_band);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/one_band.webp").c_str(), one_band.get(), FALSE,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("WEBP driver doesn't support 1 bands"));
  }
  {
    auto two_bands = CreateMemSource(4, 3, 2);
    ASSERT_NE(nullptr, two_bands);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/two_bands.webp").c_str(), two_bands.get(),
        FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("WEBP driver doesn't support 2 bands"));
  }

  // 5. Non-Byte data type
  {
    auto u16_ds = CreateMemSource(4, 3, 3, GDT_UInt16);
    ASSERT_NE(nullptr, u16_ds);

    // Strict mode -> failure
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          (std::string(kTestDir) + "/u16_strict.webp").c_str(), u16_ds.get(),
          TRUE, nullptr, nullptr, nullptr));
      EXPECT_EQ(nullptr, ds);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("WEBP driver doesn't support data type UInt16"));
    }

    // Non-strict mode -> warning and succeeds
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          (std::string(kTestDir) + "/u16_nonstrict.webp").c_str(), u16_ds.get(),
          FALSE, nullptr, nullptr, nullptr));
      EXPECT_NE(nullptr, ds);
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    }
  }

  // 6. PRESET values
  constexpr std::string_view kPresets[] = {
      "DEFAULT", "PICTURE", "PHOTO", "DRAWING", "ICON", "TEXT",
  };
  for (std::string_view preset : kPresets) {
    CPLStringList options;
    options.AddNameValue("PRESET", std::string(preset).c_str());
    const std::string filename =
        std::string(kTestDir) + "/preset_" + std::string(preset) + ".webp";
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        filename.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    EXPECT_NE(nullptr, ds) << "Failed for preset: " << preset;
  }

  // Invalid PRESET
  {
    CPLStringList options;
    options.AddNameValue("PRESET", "INVALID_PRESET");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/bad_preset.webp").c_str(), src_ds.get(), FALSE,
        options.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("PRESET=INVALID_PRESET is not a legal value"));
  }

  // 7. Valid full encoding options
  {
    CPLStringList options;
    options.AddNameValue("QUALITY", "80");
    options.AddNameValue("TARGETSIZE", "2000");
    options.AddNameValue("PSNR", "42.0");
    options.AddNameValue("METHOD", "3");
    options.AddNameValue("SEGMENTS", "2");
    options.AddNameValue("SNS_STRENGTH", "50");
    options.AddNameValue("FILTER_STRENGTH", "25");
    options.AddNameValue("FILTER_SHARPNESS", "1");
    options.AddNameValue("FILTER_TYPE", "1");
    options.AddNameValue("AUTOFILTER", "1");
    options.AddNameValue("PASS", "2");
    options.AddNameValue("PREPROCESSING", "1");
    options.AddNameValue("PARTITIONS", "1");
    options.AddNameValue("PARTITION_LIMIT", "10");
    options.AddNameValue("EXACT", "1");
    options.AddNameValue("@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME", "YES");

    const std::string filename = std::string(kTestDir) + "/full_options.webp";
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        filename.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    EXPECT_NE(nullptr, ds);
  }

  // 8. Table-driven invalid option values
  struct InvalidOptionCase {
    std::string_view key;
    std::string_view val;
  };
  constexpr InvalidOptionCase kInvalidCases[] = {
      {.key = "QUALITY", .val = "-1"},
      {.key = "QUALITY", .val = "101"},
      {.key = "TARGETSIZE", .val = "-1"},
      {.key = "PSNR", .val = "-1"},
      {.key = "METHOD", .val = "-1"},
      {.key = "METHOD", .val = "7"},
      {.key = "SEGMENTS", .val = "0"},
      {.key = "SEGMENTS", .val = "5"},
      {.key = "SNS_STRENGTH", .val = "-1"},
      {.key = "SNS_STRENGTH", .val = "101"},
      {.key = "FILTER_STRENGTH", .val = "-1"},
      {.key = "FILTER_STRENGTH", .val = "101"},
      {.key = "FILTER_SHARPNESS", .val = "-1"},
      {.key = "FILTER_SHARPNESS", .val = "8"},
      {.key = "FILTER_TYPE", .val = "-1"},
      {.key = "FILTER_TYPE", .val = "2"},
      {.key = "AUTOFILTER", .val = "-1"},
      {.key = "AUTOFILTER", .val = "2"},
      {.key = "PASS", .val = "0"},
      {.key = "PASS", .val = "11"},
      {.key = "PREPROCESSING", .val = "-1"},
      {.key = "PREPROCESSING", .val = "2"},
      {.key = "PARTITIONS", .val = "-1"},
      {.key = "PARTITIONS", .val = "4"},
      {.key = "PARTITION_LIMIT", .val = "-1"},
      {.key = "PARTITION_LIMIT", .val = "101"},
      {.key = "EXACT", .val = "-1"},
      {.key = "EXACT", .val = "2"},
  };

  for (const auto &tc : kInvalidCases) {
    CPLStringList options;
    options.AddNameValue(std::string(tc.key).c_str(), std::string(tc.val).c_str());
    const std::string filename = std::string(kTestDir) + "/invalid_" +
                                 std::string(tc.key) + "_" +
                                 std::string(tc.val) + ".webp";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        filename.c_str(), src_ds.get(), FALSE, options.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, ds) << "Expected failure for " << tc.key << "=" << tc.val;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is not a legal value"));
  }

  // 9. Cancel progress callback during encode
  {
    auto cancel_progress = [](double /*dfComplete*/, const char * /*pszMsg*/,
                              void * /*pArg*/) -> int { return FALSE; };
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        (std::string(kTestDir) + "/cancelled.webp").c_str(), src_ds.get(), FALSE,
        nullptr, cancel_progress, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User interrupted"));
  }

  // 10. Lossless CreateCopy paths from a WEBP source dataset
  {
    const std::string valid_webp =
        std::string(kTestDir) + "/valid_webp_for_lossless.webp";
    {
      CPLStringList options;
      options.AddNameValue("LOSSLESS", "YES");
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          valid_webp.c_str(), src_ds.get(), FALSE, options.List(), nullptr,
          nullptr));
      ASSERT_NE(nullptr, ds);
    }
    auto webp_src = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(valid_webp.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, webp_src);

    // a) @CREATE_ONLY_VISIBLE_AT_CLOSE_TIME=YES
    {
      CPLStringList options;
      options.AddNameValue("LOSSLESS_COPY", "YES");
      options.AddNameValue("@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME", "YES");
      const std::string out_fn =
          std::string(kTestDir) + "/lossless_close_time.webp";
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          out_fn.c_str(), webp_src.get(), FALSE, options.List(), nullptr,
          nullptr));
      EXPECT_NE(nullptr, ds);
    }

    // b) Open failure in lossless copy
    {
      CPLStringList options;
      options.AddNameValue("LOSSLESS_COPY", "YES");
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          "/does_not_exist_dir/lossless_fail.webp", webp_src.get(), FALSE,
          options.List(), nullptr, nullptr));
      EXPECT_EQ(nullptr, ds);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to create jpeg file"));
    }

    // c) Write failure in lossless copy via /vsimem/ ||maxlength=4
    {
      CPLStringList options;
      options.AddNameValue("LOSSLESS_COPY", "YES");
      const std::string max_fn =
          std::string(kTestDir) + "/max_lossless.webp||maxlength=4";
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          max_fn.c_str(), webp_src.get(), FALSE, options.List(), nullptr,
          nullptr));
      EXPECT_EQ(nullptr, ds);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failure writing data"));
    }

    // d) Lossless copy to write-only /vsistdout_redirect/... fails reopening
    {
      CPLStringList options;
      options.AddNameValue("LOSSLESS_COPY", "YES");
      const std::string stdout_fn =
          "/vsistdout_redirect/" + std::string(kTestDir) +
          "/stdout_lossless.webp";
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          stdout_fn.c_str(), webp_src.get(), FALSE, options.List(), nullptr,
          nullptr));
      EXPECT_EQ(nullptr, ds);
    }

    // e) Close failure in lossless copy
    {
      control_handler_.Reset();
      control_handler_.fail_close_ = true;
      CPLStringList options;
      options.AddNameValue("LOSSLESS_COPY", "YES");
      const std::string out_fn =
          std::string(kControlPrefix) + "lossless_fail_close.webp";
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          out_fn.c_str(), webp_src.get(), FALSE, options.List(), nullptr,
          nullptr));
      EXPECT_EQ(nullptr, ds);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Error at file closing"));
    }
  }

  // 11. 4-band lossy encode via WebPPictureImportRGBA
  {
    auto four_bands_src = CreateMemSource(4, 3, 4);
    ASSERT_NE(nullptr, four_bands_src);
    const std::string out_fn = std::string(kTestDir) + "/four_bands_lossy.webp";
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        out_fn.c_str(), four_bands_src.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_NE(nullptr, ds);
  }

  // 12. Source dataset RasterIO failure during encode
  {
    const std::string valid_fn = std::string(kTestDir) + "/src_to_corrupt.webp";
    {
      CPLStringList options;
      options.AddNameValue("LOSSLESS", "YES");
      GDALDatasetUniquePtr ds(driver_->CreateCopy(
          valid_fn.c_str(), src_ds.get(), FALSE, options.List(), nullptr,
          nullptr));
      ASSERT_NE(nullptr, ds);
    }
    vsi_l_offset file_size = 0;
    GByte *file_bytes =
        VSIGetMemFileBuffer(valid_fn.c_str(), &file_size, FALSE);
    ASSERT_NE(nullptr, file_bytes);
    std::string corrupt_bytes(reinterpret_cast<char *>(file_bytes), file_size);
    for (size_t i = 25; i < corrupt_bytes.size(); ++i) {
      corrupt_bytes[i] = static_cast<char>(0xff);
    }
    const std::string corrupt_fn = std::string(kTestDir) + "/corrupt_src.webp";
    VsiMemTempWrapper wrap(corrupt_fn, corrupt_bytes);
    auto corrupt_ds = GDALDatasetUniquePtr(GDALDataset::FromHandle(
        GDALOpenEx(corrupt_fn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, corrupt_ds);

    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList options;
    options.AddNameValue("LOSSLESS_COPY", "NO");
    const std::string out_fn = std::string(kTestDir) + "/from_corrupt.webp";
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        out_fn.c_str(), corrupt_ds.get(), FALSE, options.List(), nullptr,
        nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("WebPDecodeRGBInto() failed"));
  }

  // 13. Encode write failure via /vsimem/ ||maxlength=4 -> VP8_ENC_ERROR_BAD_WRITE
  {
    const std::string max_fn =
        std::string(kTestDir) + "/max_encode.webp||maxlength=4";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        max_fn.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Error while flushing bytes"));
  }

  // 14. Lossy CreateCopy to write-only /vsistdout_redirect/... fails reopening
  {
    const std::string stdout_fn =
        "/vsistdout_redirect/" + std::string(kTestDir) + "/stdout_lossy.webp";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        stdout_fn.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
  }

  // 15. Encode copy close failure
  {
    control_handler_.Reset();
    control_handler_.fail_close_ = true;
    const std::string out_fn =
        std::string(kControlPrefix) + "encode_fail_close.webp";
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(driver_->CreateCopy(
        out_fn.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Error at file closing"));
  }
}

// TODO: b/335317901 - Remaining ~8% uncovered lines in webpdataset.cpp are:
// - OOM / allocator failure branches (VSIMalloc, CPLMalloc, WebPPictureAlloc).
// - Pre-0x0100 WebP ABI version fallback gates.
// - Unreachable WebPEncodingError enum arms in WebPEncode error switch.
// - Defensive WebPConfigInit / WebPPictureInit / WebPValidateConfig failures.

}  // namespace
}  // namespace autotest2
