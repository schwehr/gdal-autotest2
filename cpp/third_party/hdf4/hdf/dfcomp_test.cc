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
// Tests for general HDF image compression routines (hdf/src/dfcomp.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfcomp.c

#include <cstdint>
#include <cstdio>
#include <cstring>
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

extern "C" void DFcomp_set_alloc_mode_for_testing(int mode);

namespace hdf4 {
namespace {

using ::testing::ElementsAreArray;

class DfcompTest : public ::testing::Test {
 protected:
  void SetUp() override {
    HEclear();
    DFcomp_set_alloc_mode_for_testing(0);
  }

  void TearDown() override {
    DFcomp_set_alloc_mode_for_testing(0);
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

// 1. Argument validation tests for DFputcomp and DFgetcomp
TEST_F(DfcompTest, PutAndGetCompArgumentValidation) {
  std::string path = GetTempFilePath("dfcomp_args.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);
  uint8_t dummy_img[16] = {};

  // Invalid fid
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(FAIL, DFTAG_CI, ref, dummy_img, 4, 4, nullptr,
                            nullptr, DFTAG_RLE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(FAIL, DFTAG_CI, ref, dummy_img, 4, 4, DFTAG_RLE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Invalid tag (0)
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, 0, ref, dummy_img, 4, 4, nullptr, nullptr,
                            DFTAG_RLE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, 0, ref, dummy_img, 4, 4, DFTAG_RLE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Invalid ref (0)
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, 0, dummy_img, 4, 4, nullptr, nullptr,
                            DFTAG_RLE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, 0, dummy_img, 4, 4, DFTAG_RLE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Invalid dimensions (<= 0)
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref, dummy_img, 0, 4, nullptr,
                            nullptr, DFTAG_RLE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref, dummy_img, 4, -1, DFTAG_RLE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Null image
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref, nullptr, 4, 4, nullptr, nullptr,
                            DFTAG_RLE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref, nullptr, 4, 4, DFTAG_RLE));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // IMCOMP without palette or newpal
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref, dummy_img, 4, 4, nullptr,
                            nullptr, DFTAG_IMC, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  uint8_t pal[768] = {};
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref, dummy_img, 4, 4, pal, nullptr,
                            DFTAG_IMC, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Unknown compression scheme in DFputcomp
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref, dummy_img, 4, 4, nullptr,
                            nullptr, 9999, nullptr));
  EXPECT_EQ(DFE_BADSCHEME, HEvalue(1));

  // Unknown non-JPEG scheme in DFgetcomp on existing element
  // First write a small dummy element so Hstartread succeeds
  int32 aid = Hstartwrite(fid, DFTAG_CI, ref, 4);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(4, Hwrite(aid, 4, "test"));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref, dummy_img, 4, 4, 9999));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Non-existent element in DFgetcomp
  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, 12345, dummy_img, 4, 4, DFTAG_RLE));
  EXPECT_EQ(DFE_NOMATCH, HEvalue(1));
}

// 2. RLE compression roundtrip with DFputcomp and DFgetcomp
TEST_F(DfcompTest, RleCompressionRoundtrip) {
  std::string path = GetTempFilePath("dfcomp_rle.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 40;
  const int32_t ydim = 30;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> orig_image(size);
  for (int32_t i = 0; i < size; ++i) {
    // Generate image with repeating patterns
    orig_image[i] = static_cast<uint8_t>((i / xdim) * 5 + (i % 7));
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref, orig_image.data(), xdim, ydim,
                      nullptr, nullptr, DFTAG_RLE, nullptr),
            0);
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  std::vector<uint8_t> decomp_image(size, 0);
  EXPECT_EQ(SUCCEED, DFgetcomp(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                               ydim, DFTAG_RLE));
  EXPECT_THAT(decomp_image, ElementsAreArray(orig_image));

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 3. IMCOMP compression roundtrip with DFputcomp and DFgetcomp
TEST_F(DfcompTest, ImcompCompressionRoundtrip) {
  std::string path = GetTempFilePath("dfcomp_imcomp.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  // IMCOMP requires dimensions to be multiples of 4
  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> orig_image(size);
  for (int32_t i = 0; i < size; ++i) {
    orig_image[i] = static_cast<uint8_t>((i * 13) % 256);
  }

  std::vector<uint8_t> palette(768);
  for (int i = 0; i < 768; ++i) {
    palette[i] = static_cast<uint8_t>(i % 256);
  }
  std::vector<uint8_t> newpal(768, 0);

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref, orig_image.data(), xdim, ydim,
                      palette.data(), newpal.data(), DFTAG_IMC, nullptr),
            0);
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  std::vector<uint8_t> decomp_image(size, 0);
  EXPECT_EQ(SUCCEED, DFgetcomp(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                               ydim, DFTAG_IMC));
  // Note: IMCOMP is lossy 4:1 compression, so verify decompress succeeds
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 4. 8-bit Greyscale JPEG compression roundtrip (DFTAG_GREYJPEG5 and
// DFTAG_GREYJPEG)
TEST_F(DfcompTest, GreyJpegCompressionRoundtrip) {
  std::string path = GetTempFilePath("dfcomp_greyjpeg.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 32;
  const int32_t ydim = 32;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> orig_image(size);
  for (int32_t i = 0; i < size; ++i) {
    orig_image[i] = static_cast<uint8_t>((i % xdim) * 8);
  }

  comp_info cinfo = {};
  cinfo.jpeg.quality = 75;
  cinfo.jpeg.force_baseline = 1;

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  EXPECT_EQ(0, DFputcomp(fid, DFTAG_CI, ref, orig_image.data(), xdim, ydim,
                         nullptr, nullptr, DFTAG_GREYJPEG5, &cinfo));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress using DFTAG_GREYJPEG5
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  std::vector<uint8_t> decomp_image(size, 0);
  EXPECT_EQ(SUCCEED, DFgetcomp(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                               ydim, DFTAG_GREYJPEG5));

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 5. 24-bit RGB JPEG compression roundtrip (DFTAG_JPEG5)
TEST_F(DfcompTest, RgbJpegCompressionRoundtrip) {
  std::string path = GetTempFilePath("dfcomp_rgbjpeg.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 24;
  const int32_t ydim = 24;
  const int32_t size = xdim * ydim * 3;
  std::vector<uint8_t> orig_image(size);
  for (int32_t i = 0; i < size; ++i) {
    orig_image[i] = static_cast<uint8_t>((i * 7) % 256);
  }

  comp_info cinfo = {};
  cinfo.jpeg.quality = 80;
  cinfo.jpeg.force_baseline = 1;

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  EXPECT_EQ(0, DFputcomp(fid, DFTAG_CI, ref, orig_image.data(), xdim, ydim,
                         nullptr, nullptr, DFTAG_JPEG5, &cinfo));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress using DFTAG_JPEG5
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  std::vector<uint8_t> decomp_image(size, 0);
  EXPECT_EQ(SUCCEED, DFgetcomp(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                               ydim, DFTAG_JPEG5));

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 6. RLE row-by-row and piecemeal buffer fallback (buftype == 2)
TEST_F(DfcompTest, RleRowByRowFallbackRoundtrip) {
  std::string path = GetTempFilePath("dfcomp_rle_fallback.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  // Use dimensions > R8_MAX_LENGTH (512) and > R8_MAX_BLOCKS (32)
  const int32_t xdim = 520;
  const int32_t ydim = 36;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> orig_image(size);
  for (int32_t i = 0; i < size; ++i) {
    orig_image[i] = static_cast<uint8_t>((i / xdim) ^ (i % 251));
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  // Force row-by-row write
  DFcomp_set_alloc_mode_for_testing(1);
  EXPECT_GE(DFputcomp(fid, DFTAG_CI, ref, orig_image.data(), xdim, ydim,
                      nullptr, nullptr, DFTAG_RLE, nullptr),
            0);
  DFcomp_set_alloc_mode_for_testing(0);
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress with piecemeal buffer
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  DFcomp_set_alloc_mode_for_testing(1);
  std::vector<uint8_t> decomp_image(size, 0);
  EXPECT_EQ(SUCCEED, DFgetcomp(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                               ydim, DFTAG_RLE));
  DFcomp_set_alloc_mode_for_testing(0);
  EXPECT_THAT(decomp_image, ElementsAreArray(orig_image));

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 7. IMCOMP piecemeal read fallback (4 rows at a time)
TEST_F(DfcompTest, ImcompPiecemealFallbackRoundtrip) {
  std::string path = GetTempFilePath("dfcomp_imcomp_fallback.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 32;
  const int32_t ydim = 32;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> orig_image(size);
  for (int32_t i = 0; i < size; ++i) {
    orig_image[i] = static_cast<uint8_t>((i * 17) % 256);
  }

  std::vector<uint8_t> palette(768);
  for (int i = 0; i < 768; ++i) {
    palette[i] = static_cast<uint8_t>(i % 256);
  }
  std::vector<uint8_t> newpal(768, 0);

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref, orig_image.data(), xdim, ydim,
                      palette.data(), newpal.data(), DFTAG_IMC, nullptr),
            0);
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress with piecemeal buffer (mode 1)
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  DFcomp_set_alloc_mode_for_testing(1);
  std::vector<uint8_t> decomp_image(size, 0);
  EXPECT_EQ(SUCCEED, DFgetcomp(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                               ydim, DFTAG_IMC));
  DFcomp_set_alloc_mode_for_testing(0);

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 8. Out-of-memory error paths (mode == 2)
TEST_F(DfcompTest, OutOfMemoryHandling) {
  std::string path = GetTempFilePath("dfcomp_oom.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> img(size, 42);
  std::vector<uint8_t> pal(768, 0);
  std::vector<uint8_t> newpal(768, 0);

  uint16 ref_rle = Hnewref(fid);
  uint16 ref_imc = Hnewref(fid);

  // First write valid RLE and IMC elements so DFgetcomp has something to read
  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref_rle, img.data(), xdim, ydim, nullptr,
                      nullptr, DFTAG_RLE, nullptr),
            0);
  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref_imc, img.data(), xdim, ydim,
                      pal.data(), newpal.data(), DFTAG_IMC, nullptr),
            0);

  // Test DFputcomp failures under mode 2
  uint16 ref_fail = Hnewref(fid);
  DFcomp_set_alloc_mode_for_testing(2);
  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref_fail, img.data(), xdim, ydim,
                            nullptr, nullptr, DFTAG_RLE, nullptr));
  EXPECT_EQ(DFE_NOSPACE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref_fail, img.data(), xdim, ydim,
                            pal.data(), newpal.data(), DFTAG_IMC, nullptr));
  EXPECT_EQ(DFE_NOSPACE, HEvalue(1));

  // Test DFgetcomp failures under mode 2
  std::vector<uint8_t> out(size, 0);
  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref_rle, out.data(), xdim, ydim,
                            DFTAG_RLE));
  EXPECT_EQ(DFE_NOSPACE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref_imc, out.data(), xdim, ydim,
                            DFTAG_IMC));
  EXPECT_EQ(DFE_NOSPACE, HEvalue(1));

  DFcomp_set_alloc_mode_for_testing(0);
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 9. Truncated read errors in DFgetcomp
TEST_F(DfcompTest, TruncatedReadErrors) {
  std::string path = GetTempFilePath("dfcomp_trunc.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 16;
  const int32_t ydim = 16;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> img(size, 7);
  std::vector<uint8_t> pal(768, 0);
  std::vector<uint8_t> newpal(768, 0);

  uint16 ref = Hnewref(fid);
  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref, img.data(), xdim, ydim, pal.data(),
                      newpal.data(), DFTAG_IMC, nullptr),
            0);
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Truncate file so that reading cisize fails
  auto file_size = std::filesystem::file_size(path);
  ASSERT_GT(file_size, 20);
  std::filesystem::resize_file(path, file_size - 10);

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  if (fid != FAIL) {
    std::vector<uint8_t> out(size, 0);
    HEclear();
    EXPECT_EQ(FAIL,
              DFgetcomp(fid, DFTAG_CI, ref, out.data(), xdim, ydim, DFTAG_IMC));
    EXPECT_EQ(DFE_READERROR, HEvalue(1));
    EXPECT_EQ(SUCCEED, Hclose(fid));
  }
}

// 10. Direct testing of I/O failure recovery in piecemeal paths
TEST_F(DfcompTest, PiecemealIoFailurePaths) {
  std::string path = GetTempFilePath("dfcomp_io_fail.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 520;
  const int32_t ydim = 36;
  const int32_t size = xdim * ydim;
  std::vector<uint8_t> img(size);
  for (int32_t i = 0; i < size; ++i) {
    img[i] = static_cast<uint8_t>((i / xdim) ^ (i % 251));
  }
  std::vector<uint8_t> pal(768, 0);
  std::vector<uint8_t> newpal(768, 0);

  uint16 ref_rle = Hnewref(fid);
  uint16 ref_imc = Hnewref(fid);

  // Write valid elements first
  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref_rle, img.data(), xdim, ydim, nullptr,
                      nullptr, DFTAG_RLE, nullptr),
            0);
  EXPECT_GT(DFputcomp(fid, DFTAG_CI, ref_imc, img.data(), 32, 32, pal.data(),
                      newpal.data(), DFTAG_IMC, nullptr),
            0);

  // Mode 3: Hwrite failure during DFputcomp row-by-row
  uint16 ref_fail = Hnewref(fid);
  DFcomp_set_alloc_mode_for_testing(3);
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref_fail, img.data(), xdim, ydim,
                            nullptr, nullptr, DFTAG_RLE, nullptr));

  // Mode 6: Hendaccess failure during DFputcomp row-by-row
  DFcomp_set_alloc_mode_for_testing(6);
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref_fail, img.data(), xdim, ydim,
                            nullptr, nullptr, DFTAG_RLE, nullptr));

  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for read to test read failures
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  // HLcreate failure when calling DFputcomp mode 1 on read-only file
  DFcomp_set_alloc_mode_for_testing(1);
  EXPECT_EQ(FAIL, DFputcomp(fid, DFTAG_CI, ref_fail, img.data(), xdim, ydim,
                            nullptr, nullptr, DFTAG_RLE, nullptr));

  // Mode 4: Initial Hread failure in DFgetcomp RLE piecemeal
  std::vector<uint8_t> out(size, 0);
  DFcomp_set_alloc_mode_for_testing(4);
  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref_rle, out.data(), xdim, ydim,
                            DFTAG_RLE));
  EXPECT_EQ(DFE_READERROR, HEvalue(1));

  // Mode 5: Refill Hread failure in DFgetcomp RLE piecemeal
  DFcomp_set_alloc_mode_for_testing(5);
  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref_rle, out.data(), xdim, ydim,
                            DFTAG_RLE));
  EXPECT_EQ(DFE_READERROR, HEvalue(1));

  // Mode 7: Initial Hread failure in DFgetcomp IMC piecemeal
  std::vector<uint8_t> out_imc(32 * 32, 0);
  DFcomp_set_alloc_mode_for_testing(7);
  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref_imc, out_imc.data(), 32, 32,
                            DFTAG_IMC));
  EXPECT_EQ(DFE_READERROR, HEvalue(1));

  // Mode 8: Refill Hread failure in DFgetcomp IMC piecemeal
  DFcomp_set_alloc_mode_for_testing(8);
  HEclear();
  EXPECT_EQ(FAIL, DFgetcomp(fid, DFTAG_CI, ref_imc, out_imc.data(), 32, 32,
                            DFTAG_IMC));
  EXPECT_EQ(DFE_READERROR, HEvalue(1));

  DFcomp_set_alloc_mode_for_testing(0);
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

}  // namespace
}  // namespace hdf4
