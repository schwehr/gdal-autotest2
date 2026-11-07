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
// Tests for 8-bit raster image interface (third_party/hdf4/hdf/src/dfr8.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfr8.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/tdfr8.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hdf_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hproto.h"

namespace hdf4 {
namespace {

class Dfr8Test : public ::testing::Test {
 protected:
  void SetUp() override {
    DFR8restart();
    HEclear();
  }

  void TearDown() override {
    DFR8restart();
    HEclear();
  }
};

TEST_F(Dfr8Test, InvalidArguments) {
  int32_t xdim = 0;
  int32_t ydim = 0;
  int ispal = 0;
  uint8_t img[16] = {};
  uint8_t pal[768] = {};

  // DFR8getdims invalid arguments
  EXPECT_EQ(DFR8getdims(nullptr, &xdim, &ydim, &ispal), FAIL);
  EXPECT_EQ(DFR8getdims("", &xdim, &ydim, &ispal), FAIL);
  EXPECT_EQ(DFR8getdims("some_file.hdf", nullptr, &ydim, &ispal), FAIL);
  EXPECT_EQ(DFR8getdims("some_file.hdf", &xdim, nullptr, &ispal), FAIL);

  // DFR8getimage invalid arguments
  EXPECT_EQ(DFR8getimage(nullptr, img, 4, 4, pal), FAIL);
  EXPECT_EQ(DFR8getimage("", img, 4, 4, pal), FAIL);
  EXPECT_EQ(DFR8getimage("some_file.hdf", nullptr, 4, 4, pal), FAIL);
  EXPECT_EQ(DFR8getimage("some_file.hdf", img, 0, 4, pal), FAIL);
  EXPECT_EQ(DFR8getimage("some_file.hdf", img, -1, 4, pal), FAIL);
  EXPECT_EQ(DFR8getimage("some_file.hdf", img, 4, 0, pal), FAIL);
  EXPECT_EQ(DFR8getimage("some_file.hdf", img, 4, -1, pal), FAIL);

  // DFR8putimage invalid arguments
  EXPECT_EQ(DFR8putimage(nullptr, img, 4, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8putimage("", img, 4, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8putimage("some_file.hdf", nullptr, 4, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8putimage("some_file.hdf", img, 0, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8putimage("some_file.hdf", img, -1, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8putimage("some_file.hdf", img, 4, 0, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8putimage("some_file.hdf", img, 4, -1, COMP_NONE), FAIL);

  // DFR8addimage invalid arguments
  EXPECT_EQ(DFR8addimage(nullptr, img, 4, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8addimage("", img, 4, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8addimage("some_file.hdf", nullptr, 4, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8addimage("some_file.hdf", img, 0, 4, COMP_NONE), FAIL);
  EXPECT_EQ(DFR8addimage("some_file.hdf", img, 4, 0, COMP_NONE), FAIL);

  // DFR8nimages invalid arguments
  EXPECT_EQ(DFR8nimages(nullptr), FAIL);
  EXPECT_EQ(DFR8nimages(""), FAIL);

  // DFR8readref invalid arguments
  EXPECT_EQ(DFR8readref(nullptr, 1), FAIL);
  EXPECT_EQ(DFR8readref("", 1), FAIL);
  EXPECT_EQ(DFR8readref("some_file.hdf", 0), FAIL);

  // DFR8writeref invalid arguments
  EXPECT_EQ(DFR8writeref(nullptr, 1), FAIL);
  EXPECT_EQ(DFR8writeref("", 1), FAIL);
  EXPECT_EQ(DFR8writeref("some_file.hdf", 0), SUCCEED);

  // DFR8getpalref invalid argument
  EXPECT_EQ(DFR8getpalref(nullptr), FAIL);

  // Non-existent file operations fail
  const std::string non_existent = "/non_existent_dir/missing.hdf";
  EXPECT_EQ(DFR8nimages(non_existent.c_str()), FAIL);
  EXPECT_EQ(DFR8getdims(non_existent.c_str(), &xdim, &ydim, &ispal), FAIL);
  EXPECT_EQ(DFR8getimage(non_existent.c_str(), img, 4, 4, pal), FAIL);
  EXPECT_EQ(DFR8readref(non_existent.c_str(), 1), FAIL);
  EXPECT_EQ(DFR8writeref(non_existent.c_str(), 1), SUCCEED);
}

TEST_F(Dfr8Test, BasicPutAndGetImage) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_basic.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int32_t width = 10;
  constexpr int32_t height = 8;
  std::vector<uint8_t> image(width * height);
  for (size_t i = 0; i < image.size(); ++i) {
    image[i] = static_cast<uint8_t>(i % 256);
  }

  // Write image
  EXPECT_EQ(DFR8putimage(path.c_str(), image.data(), width, height, COMP_NONE),
            SUCCEED);
  const uint16_t last_ref = DFR8lastref();
  EXPECT_NE(last_ref, 0);

  // Check image count
  EXPECT_EQ(DFR8nimages(path.c_str()), 1);

  // Check dimensions with ispal
  int32_t xdim = 0;
  int32_t ydim = 0;
  int ispal = -1;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, &ispal), SUCCEED);
  EXPECT_EQ(xdim, width);
  EXPECT_EQ(ydim, height);
  EXPECT_EQ(ispal, 0);

  // Check dimensions with nullptr for ispal (restart to re-read first image)
  DFR8restart();
  xdim = 0;
  ydim = 0;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(xdim, width);
  EXPECT_EQ(ydim, height);

  // Read image back
  std::vector<uint8_t> read_buf(width * height, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, image);

  // Read image into a buffer that is larger than the original image dimensions
  // This exercises lines 316-328 (expanding lines to larger xdim)
  constexpr int32_t large_width = 16;
  constexpr int32_t large_height = 12;
  std::vector<uint8_t> large_buf(large_width * large_height, 0);
  // Reset read position
  DFR8restart();
  EXPECT_EQ(DFR8getimage(path.c_str(), large_buf.data(), large_width,
                         large_height, nullptr),
            SUCCEED);
  // Verify first row matches original image
  for (int32_t x = 0; x < width; ++x) {
    EXPECT_EQ(large_buf[x], image[x]);
  }

  // Read image with smaller dimensions -> FAIL (lines 302-303)
  DFR8restart();
  std::vector<uint8_t> small_buf(4 * 4, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), small_buf.data(), 4, 4, nullptr), FAIL);
}

TEST_F(Dfr8Test, PaletteHandling) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_palette.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int32_t width = 6;
  constexpr int32_t height = 5;
  std::vector<uint8_t> image(width * height, 42);

  std::vector<uint8_t> palette(768);
  for (size_t i = 0; i < palette.size(); ++i) {
    palette[i] = static_cast<uint8_t>((i * 7) % 256);
  }

  // Set palette before writing image
  EXPECT_EQ(DFR8setpalette(palette.data()), SUCCEED);
  EXPECT_EQ(DFR8putimage(path.c_str(), image.data(), width, height, COMP_NONE),
            SUCCEED);

  // Check dimensions and palette presence
  int32_t xdim = 0;
  int32_t ydim = 0;
  int ispal = 0;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, &ispal), SUCCEED);
  EXPECT_EQ(xdim, width);
  EXPECT_EQ(ydim, height);
  EXPECT_EQ(ispal, 1);

  // Read image and palette back
  std::vector<uint8_t> read_img(width * height, 0);
  std::vector<uint8_t> read_pal(768, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_img.data(), width, height,
                         read_pal.data()),
            SUCCEED);
  EXPECT_EQ(read_img, image);
  EXPECT_EQ(read_pal, palette);

  // Check palette reference
  uint16_t pal_ref = 0;
  EXPECT_EQ(DFR8getpalref(&pal_ref), SUCCEED);
  EXPECT_NE(pal_ref, 0);

  // Reset palette with nullptr
  EXPECT_EQ(DFR8setpalette(nullptr), SUCCEED);

  // Write another image without palette to overwrite file
  EXPECT_EQ(DFR8putimage(path.c_str(), image.data(), width, height, COMP_NONE),
            SUCCEED);
  ispal = -1;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, &ispal), SUCCEED);
  EXPECT_EQ(ispal, 0);
}

TEST_F(Dfr8Test, MultipleImagesAndNavigation) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_multi.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int32_t width = 8;
  constexpr int32_t height = 8;
  std::vector<uint8_t> img1(width * height, 11);
  std::vector<uint8_t> img2(width * height, 22);
  std::vector<uint8_t> img3(width * height, 33);

  // Write first image
  EXPECT_EQ(DFR8putimage(path.c_str(), img1.data(), width, height, COMP_NONE),
            SUCCEED);
  const uint16_t ref1 = DFR8lastref();

  // Append second and third images
  EXPECT_EQ(DFR8addimage(path.c_str(), img2.data(), width, height, COMP_NONE),
            SUCCEED);
  const uint16_t ref2 = DFR8lastref();

  EXPECT_EQ(DFR8addimage(path.c_str(), img3.data(), width, height, COMP_NONE),
            SUCCEED);
  const uint16_t ref3 = DFR8lastref();

  EXPECT_NE(ref1, ref2);
  EXPECT_NE(ref2, ref3);

  // Verify image count
  EXPECT_EQ(DFR8nimages(path.c_str()), 3);

  // Sequential read of all 3 images
  std::vector<uint8_t> read_buf(width * height, 0);

  // Image 1
  int32_t xdim = 0, ydim = 0;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img1);
  EXPECT_EQ(DFR8lastref(), ref1);

  // Image 2
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img2);
  EXPECT_EQ(DFR8lastref(), ref2);

  // Image 3
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img3);
  EXPECT_EQ(DFR8lastref(), ref3);

  // 4th read reaches end of images -> FAIL
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), FAIL);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            FAIL);

  // Restart from beginning
  EXPECT_EQ(DFR8restart(), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img1);

  // Seek using DFR8readref to read image 2 directly
  EXPECT_EQ(DFR8readref(path.c_str(), ref2), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img2);

  // Seek to non-existent ref fails
  EXPECT_EQ(DFR8readref(path.c_str(), 9999), FAIL);
}

TEST_F(Dfr8Test, WriterefExplicitAssignment) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_writeref.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int32_t width = 4;
  constexpr int32_t height = 4;
  std::vector<uint8_t> img(width * height, 77);

  constexpr uint16_t custom_ref = 1234;
  EXPECT_EQ(DFR8writeref(path.c_str(), custom_ref), SUCCEED);
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, COMP_NONE),
            SUCCEED);
  EXPECT_EQ(DFR8lastref(), custom_ref);

  // Verify image can be read back using that reference
  EXPECT_EQ(DFR8readref(path.c_str(), custom_ref), SUCCEED);
  std::vector<uint8_t> read_buf(width * height, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img);
}

TEST_F(Dfr8Test, CompressionTypes) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_comp.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int32_t width = 16;
  constexpr int32_t height = 16;
  std::vector<uint8_t> img(width * height);
  for (size_t i = 0; i < img.size(); ++i) {
    img[i] = static_cast<uint8_t>((i / width) * 16);  // Runs of same values
  }

  // 1. RLE Compression via parameter
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, COMP_RLE),
            SUCCEED);
  std::vector<uint8_t> read_buf(width * height, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img);

  // 2. RLE Compression via DFR8setcompress
  DFR8restart();
  EXPECT_EQ(DFR8setcompress(COMP_RLE, nullptr), SUCCEED);
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, COMP_NONE),
            SUCCEED);
  std::fill(read_buf.begin(), read_buf.end(), 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);
  EXPECT_EQ(read_buf, img);

  // 3. IMCOMP (Adaptive Huffman) Compression
  DFR8restart();
  std::vector<uint8_t> palette(768);
  for (size_t i = 0; i < palette.size(); ++i) {
    palette[i] = static_cast<uint8_t>(i % 256);
  }
  EXPECT_EQ(DFR8setpalette(palette.data()), SUCCEED);
  EXPECT_EQ(DFR8setcompress(COMP_IMCOMP, nullptr), SUCCEED);
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, COMP_NONE),
            SUCCEED);
  std::fill(read_buf.begin(), read_buf.end(), 0);
  std::vector<uint8_t> read_pal(768, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height,
                         read_pal.data()),
            SUCCEED);
  // IMCOMP is lossy so read_buf won't match exactly, but getimage succeeds

  // 4. Deflate is not in compress_map for DFR8, so DFR8setcompress fails
  DFR8restart();
  comp_info deflate_info = {};
  deflate_info.deflate_setting.level = 6;
  EXPECT_EQ(DFR8setcompress(COMP_CODE_DEFLATE, &deflate_info), FAIL);

  // 5. JPEG Compression (via DFR8setcompress)
  DFR8restart();
  comp_info jpeg_info = {};
  jpeg_info.jpeg.quality = 75;
  jpeg_info.jpeg.force_baseline = 1;
  EXPECT_EQ(DFR8setcompress(COMP_JPEG, &jpeg_info), SUCCEED);
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, COMP_NONE),
            SUCCEED);
  std::fill(read_buf.begin(), read_buf.end(), 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf.data(), width, height, nullptr),
            SUCCEED);

  // 6. Bad compression type in DFR8setcompress -> FAIL
  EXPECT_EQ(DFR8setcompress(9999, nullptr), FAIL);
}

TEST_F(Dfr8Test, LegacyRI8AndCI8Compatibility) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_legacy.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr uint16_t ri_ref = 50;
  constexpr int32_t width = 4;
  constexpr int32_t height = 4;
  const std::vector<uint8_t> pixels = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  };

  // Synthesize legacy HDF file with DFTAG_RI8, DFTAG_ID8, and DFTAG_IP8 (no
  // DFTAG_RIG)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    // Write raster data
    ASSERT_NE(Hputelement(fid, DFTAG_RI8, ri_ref, pixels.data(), pixels.size()),
              FAIL);

    // Write dimensions (ID8: 2 uint16s: xdim, ydim)
    uint8_t id8_buf[4] = {};
    uint8_t* p = id8_buf;
    UINT16ENCODE(p, width);
    UINT16ENCODE(p, height);
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, ri_ref, id8_buf, sizeof(id8_buf)),
              FAIL);

    // Write palette (IP8: 768 bytes)
    std::vector<uint8_t> pal(768, 99);
    ASSERT_NE(Hputelement(fid, DFTAG_IP8, ri_ref, pal.data(), pal.size()),
              FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  // Test DFR8nimages detects the legacy RI8
  DFR8restart();
  EXPECT_EQ(DFR8nimages(path.c_str()), 1);

  // Test DFR8getdims parses the legacy RI8
  int32_t xdim = 0, ydim = 0;
  int ispal = 0;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, &ispal), SUCCEED);
  EXPECT_EQ(xdim, width);
  EXPECT_EQ(ydim, height);
  EXPECT_EQ(ispal, 1);

  // Test DFR8getimage reads the legacy RI8 and palette
  std::vector<uint8_t> read_pixels(width * height, 0);
  std::vector<uint8_t> read_pal(768, 0);
  EXPECT_EQ(DFR8getimage(path.c_str(), read_pixels.data(), width, height,
                         read_pal.data()),
            SUCCEED);
  EXPECT_EQ(read_pixels, pixels);
  EXPECT_EQ(read_pal[0], 99);

  // Now create a legacy CI8 (compressed 8-bit image with RLE)
  const std::string ci_path = dir + "/dfr8_legacy_ci8.hdf";
  (void)remove(ci_path.c_str());
  const absl::Cleanup ci_cleanup = [&ci_path] {
    (void)remove(ci_path.c_str());
  };

  {
    const int32_t fid = Hopen(ci_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ci_ref = 60;
    // Compress pixels with RLE using DFputcomp
    ASSERT_NE(DFputcomp(fid, DFTAG_CI8, ci_ref, pixels.data(), width, height,
                        nullptr, nullptr, DFTAG_RLE, nullptr),
              FAIL);

    uint8_t id8_buf[4] = {};
    uint8_t* p = id8_buf;
    UINT16ENCODE(p, width);
    UINT16ENCODE(p, height);
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, ci_ref, id8_buf, sizeof(id8_buf)),
              FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  EXPECT_EQ(DFR8nimages(ci_path.c_str()), 1);
  xdim = 0;
  ydim = 0;
  ispal = -1;
  EXPECT_EQ(DFR8getdims(ci_path.c_str(), &xdim, &ydim, &ispal), SUCCEED);
  EXPECT_EQ(xdim, width);
  EXPECT_EQ(ydim, height);
  EXPECT_EQ(ispal, 0);

  std::fill(read_pixels.begin(), read_pixels.end(), 0);
  EXPECT_EQ(
      DFR8getimage(ci_path.c_str(), read_pixels.data(), width, height, nullptr),
      SUCCEED);
  EXPECT_EQ(read_pixels, pixels);
}

TEST_F(Dfr8Test, CorruptedAndEdgeScenarios) {
  const std::string dir = ::testing::TempDir();
  const std::string empty_path = dir + "/dfr8_empty.hdf";
  (void)remove(empty_path.c_str());
  const absl::Cleanup empty_cleanup = [&empty_path] {
    (void)remove(empty_path.c_str());
  };

  // 1. Empty file has 0 images
  {
    const int32_t fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }
  DFR8restart();
  EXPECT_EQ(DFR8nimages(empty_path.c_str()), 0);
  int32_t xdim = 0, ydim = 0;
  EXPECT_EQ(DFR8getdims(empty_path.c_str(), &xdim, &ydim, nullptr), FAIL);
  uint8_t buf[16] = {};
  EXPECT_EQ(DFR8getimage(empty_path.c_str(), buf, 4, 4, nullptr), FAIL);

  // 2. Corrupt RIG (RIG element with missing ID element)
  const std::string corrupt_path = dir + "/dfr8_corrupt_rig.hdf";
  (void)remove(corrupt_path.c_str());
  const absl::Cleanup corrupt_cleanup = [&corrupt_path] {
    (void)remove(corrupt_path.c_str());
  };

  {
    const int32_t fid = Hopen(corrupt_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t bad_ref = 88;
    int32_t gid = DFdisetup(4);
    ASSERT_NE(gid, FAIL);
    ASSERT_NE(DFdiput(gid, DFTAG_ID, 9999), FAIL);  // Non-existent ID element
    ASSERT_NE(DFdiwrite(fid, gid, DFTAG_RIG, bad_ref), FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  EXPECT_EQ(DFR8getdims(corrupt_path.c_str(), &xdim, &ydim, nullptr), FAIL);

  // 3. Corrupt ID8 element in legacy file (ID8 is less than 4 bytes)
  const std::string bad_id8_path = dir + "/dfr8_bad_id8.hdf";
  (void)remove(bad_id8_path.c_str());
  const absl::Cleanup bad_cleanup = [&bad_id8_path] {
    (void)remove(bad_id8_path.c_str());
  };

  {
    const int32_t fid = Hopen(bad_id8_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    uint8_t dummy = 1;
    ASSERT_NE(Hputelement(fid, DFTAG_RI8, 10, &dummy, 1), FAIL);
    // Write 2 bytes instead of 4 for ID8
    uint8_t short_id8[2] = {0, 4};
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, 10, short_id8, 2), FAIL);
    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  EXPECT_EQ(DFR8getdims(bad_id8_path.c_str(), &xdim, &ydim, nullptr), FAIL);

  // 4. Multiple identical RIGs with same image offset (tests duplicate
  // deduplication in DFR8nimages)
  const std::string dup_path = dir + "/dfr8_dup.hdf";
  (void)remove(dup_path.c_str());
  const absl::Cleanup dup_cleanup = [&dup_path] {
    (void)remove(dup_path.c_str());
  };

  {
    const int32_t fid = Hopen(dup_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr int32_t w = 2, h = 2;
    uint8_t pixels[4] = {1, 2, 3, 4};
    ASSERT_NE(Hputelement(fid, DFTAG_RI, 1, pixels, 4), FAIL);

    uint8_t id_buf[64] = {};
    uint8_t* p = id_buf;
    INT32ENCODE(p, w);
    INT32ENCODE(p, h);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    INT16ENCODE(p, 1);
    INT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_ID, 1, id_buf, p - id_buf), FAIL);

    // RIG 1 pointing to RI 1
    int32_t gid1 = DFdisetup(4);
    ASSERT_NE(DFdiput(gid1, DFTAG_ID, 1), FAIL);
    ASSERT_NE(DFdiput(gid1, DFTAG_RI, 1), FAIL);
    ASSERT_NE(DFdiwrite(fid, gid1, DFTAG_RIG, 1), FAIL);

    // RIG 2 pointing to the SAME RI 1
    int32_t gid2 = DFdisetup(4);
    ASSERT_NE(DFdiput(gid2, DFTAG_ID, 1), FAIL);
    ASSERT_NE(DFdiput(gid2, DFTAG_RI, 1), FAIL);
    ASSERT_NE(DFdiwrite(fid, gid2, DFTAG_RIG, 2), FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  // Both RIGs point to the same image offset, deduplication reduces count to 1
  EXPECT_EQ(DFR8nimages(dup_path.c_str()), 1);

  // 5. Test DFR8Pshutdown explicitly
  EXPECT_EQ(DFR8Pshutdown(), SUCCEED);
}

TEST_F(Dfr8Test, SetCompressAndPutImageSchemes) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_schemes.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // DFR8setcompress with COMP_NONE
  EXPECT_EQ(DFR8setcompress(COMP_NONE, nullptr), SUCCEED);
  // Invalid compression schemes
  EXPECT_EQ(DFR8setcompress(9999, nullptr), FAIL);
  EXPECT_EQ(DFR8setcompress(-1, nullptr), FAIL);

  // Shutdown to clear paletteBuf
  EXPECT_EQ(DFR8Pshutdown(), SUCCEED);

  constexpr int32_t width = 8;
  constexpr int32_t height = 8;
  std::vector<uint8_t> img(width * height, 55);

  // Calling DFR8putimage when paletteBuf is null allocates paletteBuf
  // internally
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, COMP_NONE),
            SUCCEED);

  // Put image with direct COMP_JPEG parameter
  const std::string jpeg_path = dir + "/dfr8_jpeg_param.hdf";
  (void)remove(jpeg_path.c_str());
  const absl::Cleanup jpeg_cleanup = [&jpeg_path] {
    (void)remove(jpeg_path.c_str());
  };
  EXPECT_EQ(
      DFR8putimage(jpeg_path.c_str(), img.data(), width, height, COMP_JPEG),
      SUCCEED);

  // Put image with invalid compression scheme
  EXPECT_EQ(DFR8putimage(path.c_str(), img.data(), width, height, 9999), FAIL);
}

TEST_F(Dfr8Test, LowLevelGetRigAndPutRig) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_lowlevel.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  constexpr int32_t w = 4, h = 4;
  std::vector<uint8_t> pixels(w * h, 33);
  ASSERT_NE(Hputelement(fid, DFTAG_RI, 10, pixels.data(), pixels.size()), FAIL);

  // 1. RIG with ncomponents == 3 (e.g. 24-bit RIG) -> DFR8getrig fails with
  // DFE_BADCALL
  uint8_t id_buf[64] = {};
  uint8_t* p = id_buf;
  INT32ENCODE(p, w);
  INT32ENCODE(p, h);
  UINT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  INT16ENCODE(p, 3);  // ncomponents = 3
  INT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  ASSERT_NE(Hputelement(fid, DFTAG_ID, 20, id_buf, p - id_buf), FAIL);

  int32_t gid = DFdisetup(4);
  ASSERT_NE(DFdiput(gid, DFTAG_ID, 20), FAIL);
  ASSERT_NE(DFdiput(gid, DFTAG_RI, 10), FAIL);
  ASSERT_NE(DFdiwrite(fid, gid, DFTAG_RIG, 20), FAIL);

  // 2. RIG with nt.tag == 0 (old RIG without NT)
  p = id_buf;
  INT32ENCODE(p, w);
  INT32ENCODE(p, h);
  UINT16ENCODE(p, 0);  // nt.tag = 0
  UINT16ENCODE(p, 0);
  INT16ENCODE(p, 1);
  INT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  ASSERT_NE(Hputelement(fid, DFTAG_ID, 30, id_buf, p - id_buf), FAIL);

  gid = DFdisetup(4);
  ASSERT_NE(DFdiput(gid, DFTAG_ID, 30), FAIL);
  ASSERT_NE(DFdiput(gid, DFTAG_RI, 10), FAIL);
  ASSERT_NE(DFdiwrite(fid, gid, DFTAG_RIG, 30), FAIL);

  // 3. RIG with nt.tag != 0 but non-existent NT element
  p = id_buf;
  INT32ENCODE(p, w);
  INT32ENCODE(p, h);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, 9999);  // Non-existent NT ref
  INT16ENCODE(p, 1);
  INT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  ASSERT_NE(Hputelement(fid, DFTAG_ID, 40, id_buf, p - id_buf), FAIL);

  gid = DFdisetup(4);
  ASSERT_NE(DFdiput(gid, DFTAG_ID, 40), FAIL);
  ASSERT_NE(DFdiput(gid, DFTAG_RI, 10), FAIL);
  ASSERT_NE(DFdiwrite(fid, gid, DFTAG_RIG, 40), FAIL);

  // 4. RIG with NT having wrong type/width (e.g. 16-bit instead of 8-bit)
  uint8_t bad_nt[4] = {DFNT_VERSION, DFNT_INT16, 16, DFNTC_BYTE};
  ASSERT_NE(Hputelement(fid, DFTAG_NT, 50, bad_nt, 4), FAIL);
  p = id_buf;
  INT32ENCODE(p, w);
  INT32ENCODE(p, h);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, 50);
  INT16ENCODE(p, 1);
  INT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  UINT16ENCODE(p, 0);
  ASSERT_NE(Hputelement(fid, DFTAG_ID, 50, id_buf, p - id_buf), FAIL);

  gid = DFdisetup(4);
  ASSERT_NE(DFdiput(gid, DFTAG_ID, 50), FAIL);
  ASSERT_NE(DFdiput(gid, DFTAG_RI, 10), FAIL);
  ASSERT_NE(DFdiwrite(fid, gid, DFTAG_RIG, 50), FAIL);

  // 5. RIG with an unexpected tag (e.g. DFTAG_MT) in the group
  gid = DFdisetup(6);
  ASSERT_NE(DFdiput(gid, DFTAG_ID, 30), FAIL);
  ASSERT_NE(DFdiput(gid, DFTAG_RI, 10), FAIL);
  ASSERT_NE(DFdiput(gid, DFTAG_MT, 10), FAIL);
  ASSERT_NE(DFdiwrite(fid, gid, DFTAG_RIG, 60), FAIL);

  ASSERT_NE(Hclose(fid), FAIL);

  int32_t xdim = 0, ydim = 0;

  // Test reading old RIG without NT (ref 30) -> succeeds
  DFR8restart();
  EXPECT_EQ(DFR8readref(path.c_str(), 30), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(xdim, w);

  // Test reading RIG with ncomponents == 3 (ref 20) -> fails with DFE_BADCALL
  DFR8restart();
  EXPECT_EQ(DFR8readref(path.c_str(), 20), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), FAIL);

  // Test reading RIG with non-existent NT (ref 40) -> fails
  DFR8restart();
  EXPECT_EQ(DFR8readref(path.c_str(), 40), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), FAIL);

  // Test reading RIG with invalid NT width/type (ref 50) -> fails
  DFR8restart();
  EXPECT_EQ(DFR8readref(path.c_str(), 50), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), FAIL);

  // Test reading RIG with extra tag (ref 60) -> succeeds
  DFR8restart();
  EXPECT_EQ(DFR8readref(path.c_str(), 60), SUCCEED);
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);

  // Test DFR8addimage with identical dimensions to reuse prevdimref (wdim == 0)
  const std::string add_path = dir + "/dfr8_add_same_dim.hdf";
  (void)remove(add_path.c_str());
  const absl::Cleanup add_cleanup = [&add_path] {
    (void)remove(add_path.c_str());
  };
  EXPECT_EQ(DFR8putimage(add_path.c_str(), pixels.data(), w, h, COMP_NONE),
            SUCCEED);
  EXPECT_EQ(DFR8addimage(add_path.c_str(), pixels.data(), w, h, COMP_NONE),
            SUCCEED);
}

TEST_F(Dfr8Test, EmptyFileNimages) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_empty.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  DFR8restart();
  EXPECT_EQ(DFR8nimages(path.c_str()), 0);
}

TEST_F(Dfr8Test, LegacyMultipleRi8AndCi8Images) {
  const std::string dir = ::testing::TempDir();
  const std::string ri8_path = dir + "/dfr8_multi_ri8.hdf";
  (void)remove(ri8_path.c_str());
  const absl::Cleanup cleanup1 = [&ri8_path] {
    (void)remove(ri8_path.c_str());
  };

  constexpr int32_t w = 2, h = 2;
  uint8_t img1[4] = {10, 11, 12, 13};
  uint8_t img2[4] = {20, 21, 22, 23};
  uint8_t id8_buf[4] = {};
  uint8_t* p = id8_buf;
  UINT16ENCODE(p, w);
  UINT16ENCODE(p, h);

  // Write file with two RI8 images and NO RIGs
  {
    const int32_t fid = Hopen(ri8_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    ASSERT_NE(Hputelement(fid, DFTAG_RI8, 1, img1, 4), FAIL);
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, 1, id8_buf, 4), FAIL);

    ASSERT_NE(Hputelement(fid, DFTAG_RI8, 2, img2, 4), FAIL);
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, 2, id8_buf, 4), FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  // Read first RI8 image
  int32_t xdim = 0, ydim = 0;
  EXPECT_EQ(DFR8getdims(ri8_path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(xdim, w);
  EXPECT_EQ(ydim, h);
  uint8_t read_buf[4] = {};
  EXPECT_EQ(DFR8getimage(ri8_path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img1, img1 + 4));

  // Read second RI8 image sequentially
  EXPECT_EQ(DFR8getdims(ri8_path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(ri8_path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img2, img2 + 4));

  // Test DFR8readref on RI8
  DFR8restart();
  EXPECT_EQ(DFR8readref(ri8_path.c_str(), 2), SUCCEED);
  EXPECT_EQ(DFR8getdims(ri8_path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(ri8_path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img2, img2 + 4));

  // Now create a file with two CI8 images and NO RIGs
  const std::string ci8_path = dir + "/dfr8_multi_ci8.hdf";
  (void)remove(ci8_path.c_str());
  const absl::Cleanup cleanup2 = [&ci8_path] {
    (void)remove(ci8_path.c_str());
  };

  {
    const int32_t fid = Hopen(ci8_path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    // Compress using RLE
    uint8_t rle_buf1[16] = {};
    const int32_t rle_len1 = DFCIrle(img1, rle_buf1, 4);
    ASSERT_GT(rle_len1, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_CI8, 1, rle_buf1, rle_len1), FAIL);
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, 1, id8_buf, 4), FAIL);

    uint8_t rle_buf2[16] = {};
    const int32_t rle_len2 = DFCIrle(img2, rle_buf2, 4);
    ASSERT_GT(rle_len2, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_CI8, 2, rle_buf2, rle_len2), FAIL);
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, 2, id8_buf, 4), FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  // Read first CI8 image
  EXPECT_EQ(DFR8getdims(ci8_path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(ci8_path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img1, img1 + 4));

  // Read second CI8 image sequentially
  EXPECT_EQ(DFR8getdims(ci8_path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(ci8_path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img2, img2 + 4));

  // Test DFR8readref on CI8
  DFR8restart();
  EXPECT_EQ(DFR8readref(ci8_path.c_str(), 1), SUCCEED);
  EXPECT_EQ(DFR8getdims(ci8_path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(DFR8getimage(ci8_path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img1, img1 + 4));
}

TEST_F(Dfr8Test, MixedRigSkippingNon8BitRig) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfr8_mixed_rigs.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int32_t w = 2, h = 2;
  uint8_t img[4] = {1, 2, 3, 4};

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    ASSERT_NE(Hputelement(fid, DFTAG_RI, 1, img, 4), FAIL);

    // RIG 1: 24-bit RIG (ncomponents = 3)
    uint8_t id24[64] = {};
    uint8_t* p = id24;
    INT32ENCODE(p, w);
    INT32ENCODE(p, h);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    INT16ENCODE(p, 3);  // ncomponents = 3
    INT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_ID, 1, id24, p - id24), FAIL);

    int32_t gid1 = DFdisetup(4);
    ASSERT_NE(DFdiput(gid1, DFTAG_ID, 1), FAIL);
    ASSERT_NE(DFdiput(gid1, DFTAG_RI, 1), FAIL);
    ASSERT_NE(DFdiwrite(fid, gid1, DFTAG_RIG, 1), FAIL);

    // RIG 2: 8-bit RIG (ncomponents = 1)
    uint8_t id8[64] = {};
    p = id8;
    INT32ENCODE(p, w);
    INT32ENCODE(p, h);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    INT16ENCODE(p, 1);  // ncomponents = 1
    INT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_ID, 2, id8, p - id8), FAIL);

    int32_t gid2 = DFdisetup(4);
    ASSERT_NE(DFdiput(gid2, DFTAG_ID, 2), FAIL);
    ASSERT_NE(DFdiput(gid2, DFTAG_RI, 1), FAIL);
    ASSERT_NE(DFdiwrite(fid, gid2, DFTAG_RIG, 2), FAIL);

    ASSERT_NE(Hclose(fid), FAIL);
  }

  DFR8restart();
  // DFR8getdims should skip RIG 1 (returns DFE_BADCALL) and find RIG 2
  int32_t xdim = 0, ydim = 0;
  EXPECT_EQ(DFR8getdims(path.c_str(), &xdim, &ydim, nullptr), SUCCEED);
  EXPECT_EQ(xdim, w);
  EXPECT_EQ(ydim, h);
  uint8_t read_buf[4] = {};
  EXPECT_EQ(DFR8getimage(path.c_str(), read_buf, w, h, nullptr), SUCCEED);
  EXPECT_EQ(std::vector<uint8_t>(read_buf, read_buf + 4),
            std::vector<uint8_t>(img, img + 4));
}

}  // namespace
}  // namespace hdf4
