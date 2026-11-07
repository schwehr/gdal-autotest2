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
// Tests for JPEG compression interface (hdf/src/dfjpeg.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfjpeg.c

#include <cstdint>
#include <cstdio>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hcomp.h"
// clang-format on
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/libjpeg_turbo/src/jpeglib.h"

extern "C" {
extern void (*jpeg_message_handler)(j_common_ptr cinfo);
extern int jpeg_HDF_dest(struct jpeg_compress_struct* cinfo_ptr, int32 file_id,
                         uint16 tag, uint16 ref, const void* image, int32 xdim,
                         int32 ydim, int16 scheme);
extern int jpeg_HDF_dest_term(struct jpeg_compress_struct* cinfo_ptr);
}

namespace hdf4 {
namespace {

static bool g_custom_message_handler_called = false;
static void CustomMessageHandler(j_common_ptr cinfo) {
  g_custom_message_handler_called = true;
}

class DfjpegTest : public ::testing::Test {
 protected:
  void SetUp() override {
    HEclear();
    jpeg_message_handler = nullptr;
    g_custom_message_handler_called = false;
  }

  void TearDown() override {
    jpeg_message_handler = nullptr;
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

// 1. 24-bit RGB JPEG compression (DFTAG_JPEG5) - small image
TEST_F(DfjpegTest, CompressRgbSmallImage) {
  std::string path = GetTempFilePath("dfjpeg_rgb_small.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t num_pixels = xdim * ydim;
  std::vector<uint8_t> rgb_image(num_pixels * 3);
  for (int32_t i = 0; i < num_pixels * 3; ++i) {
    rgb_image[i] = static_cast<uint8_t>((i * 13) % 256);
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 75;
  cinfo.jpeg.force_baseline = 1;

  EXPECT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, rgb_image.data(),
                              DFTAG_JPEG5, &cinfo));

  // Verify that an element with DFTAG_CI was created and has non-zero length
  int32 aid = Hstartread(fid, DFTAG_CI, ref);
  ASSERT_NE(FAIL, aid);
  int32 length = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, &length, nullptr,
                              nullptr, nullptr, nullptr));
  EXPECT_GT(length, 0);
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
}

// 2. Large 24-bit image to trigger buffer emptying (hdf_empty_output_buffer)
TEST_F(DfjpegTest, CompressRgbLargeImageTriggersBufferEmpty) {
  std::string path = GetTempFilePath("dfjpeg_rgb_large.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  // 120x120 noise image with quality 95 compresses to > 10KB (exceeds 4096-byte
  // buffer)
  const int32_t xdim = 120;
  const int32_t ydim = 120;
  const int32_t num_pixels = xdim * ydim;
  std::vector<uint8_t> rgb_image(num_pixels * 3);
  for (int32_t i = 0; i < num_pixels * 3; ++i) {
    rgb_image[i] = static_cast<uint8_t>((i * 7919 + 31) % 256);
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 95;
  cinfo.jpeg.force_baseline = 1;

  EXPECT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, rgb_image.data(),
                              DFTAG_JPEG5, &cinfo));

  int32 aid = Hstartread(fid, DFTAG_CI, ref);
  ASSERT_NE(FAIL, aid);
  int32 length = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, &length, nullptr,
                              nullptr, nullptr, nullptr));
  // Compressed size should exceed the 4096-byte buffer
  EXPECT_GT(length, 4096);
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
}

// 3. 8-bit Greyscale JPEG compression (DFTAG_GREYJPEG5)
TEST_F(DfjpegTest, CompressGreyscaleImage) {
  std::string path = GetTempFilePath("dfjpeg_grey.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const int32_t xdim = 32;
  const int32_t ydim = 32;
  const int32_t num_pixels = xdim * ydim;
  std::vector<uint8_t> grey_image(num_pixels);
  for (int32_t i = 0; i < num_pixels; ++i) {
    grey_image[i] = static_cast<uint8_t>((i % xdim) * (255 / xdim));
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 80;
  cinfo.jpeg.force_baseline = 1;

  EXPECT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, grey_image.data(),
                              DFTAG_GREYJPEG5, &cinfo));

  int32 aid = Hstartread(fid, DFTAG_CI, ref);
  ASSERT_NE(FAIL, aid);
  int32 length = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, &length, nullptr,
                              nullptr, nullptr, nullptr));
  EXPECT_GT(length, 0);
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
}

// 4. Invalid scheme argument handling
TEST_F(DfjpegTest, InvalidSchemeReturnsError) {
  std::string path = GetTempFilePath("dfjpeg_invalid_scheme.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const int32_t xdim = 16;
  const int32_t ydim = 16;
  std::vector<uint8_t> dummy(xdim * ydim * 3, 0);

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 75;

  HEclear();
  EXPECT_EQ(FAIL, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, dummy.data(),
                           9999 /* invalid */, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

// 5. Custom jpeg_message_handler integration
TEST_F(DfjpegTest, CustomMessageHandlerSetCorrectly) {
  std::string path = GetTempFilePath("dfjpeg_custom_handler.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  jpeg_message_handler = CustomMessageHandler;

  const int32_t xdim = 16;
  const int32_t ydim = 16;
  std::vector<uint8_t> rgb_image(xdim * ydim * 3, 128);

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 75;
  cinfo.jpeg.force_baseline = 1;

  EXPECT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, rgb_image.data(),
                              DFTAG_JPEG5, &cinfo));
}

// 6. Direct testing of destination manager lifecycle functions
TEST_F(DfjpegTest, DirectDestinationManagerLifecycle) {
  std::string path = GetTempFilePath("dfjpeg_dest_mgr.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  struct jpeg_compress_struct cinfo = {};
  uint8_t dummy_img[16] = {};

  EXPECT_EQ(SUCCEED, jpeg_HDF_dest(&cinfo, fid, DFTAG_CI, ref, dummy_img, 4, 4,
                                   DFTAG_JPEG5));
  EXPECT_NE(nullptr, cinfo.dest);

  EXPECT_EQ(SUCCEED, jpeg_HDF_dest_term(&cinfo));
}

}  // namespace
}  // namespace hdf4
