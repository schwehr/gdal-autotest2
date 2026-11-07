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
// Tests for JPEG decompression interface (hdf/src/dfunjpeg.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfunjpeg.c

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
extern void hdf_skip_input_data(struct jpeg_decompress_struct* cinfo_ptr,
                                long num_bytes);
extern int jpeg_HDF_src(struct jpeg_decompress_struct* cinfo_ptr, int32 file_id,
                        uint16 tag, uint16 ref, void* image, int32 xdim,
                        int32 ydim, int16 scheme);
extern int jpeg_HDF_src_term(struct jpeg_decompress_struct* cinfo_ptr);
}

namespace hdf4 {
namespace {

class DfunjpegTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

// 1. Decompress new-style 24-bit RGB JPEG (DFTAG_JPEG5)
TEST_F(DfunjpegTest, DecompressRgbNewStyleJpeg) {
  std::string path = GetTempFilePath("dfunjpeg_rgb.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 24;
  const int32_t ydim = 24;
  const int32_t num_pixels = xdim * ydim;
  std::vector<uint8_t> orig_image(num_pixels * 3);
  for (int32_t i = 0; i < num_pixels * 3; ++i) {
    orig_image[i] = static_cast<uint8_t>((i * 17) % 256);
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 85;
  cinfo.jpeg.force_baseline = 1;

  ASSERT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, orig_image.data(),
                              DFTAG_JPEG5, &cinfo));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for read and decompress
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  std::vector<uint8_t> decomp_image(num_pixels * 3, 0);
  EXPECT_EQ(SUCCEED, DFCIunjpeg(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                                ydim, DFTAG_JPEG5));

  // JPEG is lossy, but verify pixels are reasonably close
  int64_t diff = 0;
  for (int32_t i = 0; i < num_pixels * 3; ++i) {
    diff += std::abs(static_cast<int>(orig_image[i]) -
                     static_cast<int>(decomp_image[i]));
  }
  double avg_diff = static_cast<double>(diff) / (num_pixels * 3);
  EXPECT_LT(avg_diff, 30.0);
}

// 2. Decompress new-style 8-bit Greyscale JPEG (DFTAG_GREYJPEG5)
TEST_F(DfunjpegTest, DecompressGreyscaleNewStyleJpeg) {
  std::string path = GetTempFilePath("dfunjpeg_grey.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 32;
  const int32_t ydim = 32;
  const int32_t num_pixels = xdim * ydim;
  std::vector<uint8_t> orig_image(num_pixels);
  for (int32_t i = 0; i < num_pixels; ++i) {
    orig_image[i] = static_cast<uint8_t>((i % xdim) * 8);
  }

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  comp_info cinfo = {};
  cinfo.jpeg.quality = 80;
  cinfo.jpeg.force_baseline = 1;

  ASSERT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref, xdim, ydim, orig_image.data(),
                              DFTAG_GREYJPEG5, &cinfo));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  std::vector<uint8_t> decomp_image(num_pixels, 0);
  EXPECT_EQ(SUCCEED, DFCIunjpeg(fid, DFTAG_CI, ref, decomp_image.data(), xdim,
                                ydim, DFTAG_GREYJPEG5));

  int64_t diff = 0;
  for (int32_t i = 0; i < num_pixels; ++i) {
    diff += std::abs(static_cast<int>(orig_image[i]) -
                     static_cast<int>(decomp_image[i]));
  }
  double avg_diff = static_cast<double>(diff) / num_pixels;
  EXPECT_LT(avg_diff, 15.0);
}

// 3. Decompress old-style HDF JPEG (header in DFTAG_JPEG, body in DFTAG_CI)
TEST_F(DfunjpegTest, DecompressOldStyleJpegImage) {
  std::string path = GetTempFilePath("dfunjpeg_oldstyle.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const int32_t xdim = 120;
  const int32_t ydim = 120;
  const int32_t num_pixels = xdim * ydim;
  std::vector<uint8_t> orig_image(num_pixels * 3);
  for (int32_t i = 0; i < num_pixels * 3; ++i) {
    orig_image[i] = static_cast<uint8_t>((i * 7919 + 31) % 256);
  }

  uint16 ref_temp = Hnewref(fid);
  comp_info cinfo = {};
  cinfo.jpeg.quality = 95;
  cinfo.jpeg.force_baseline = 1;

  ASSERT_EQ(SUCCEED, DFCIjpeg(fid, DFTAG_CI, ref_temp, xdim, ydim,
                              orig_image.data(), DFTAG_JPEG5, &cinfo));

  // Read back the compressed JPEG bytes
  int32 aid = Hstartread(fid, DFTAG_CI, ref_temp);
  ASSERT_NE(FAIL, aid);
  int32 jpeg_bytes = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, &jpeg_bytes,
                              nullptr, nullptr, nullptr, nullptr));
  ASSERT_GT(jpeg_bytes, 8192);
  std::vector<uint8_t> raw_jpeg(jpeg_bytes);
  EXPECT_EQ(jpeg_bytes, Hread(aid, jpeg_bytes, raw_jpeg.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // Split into old-style layout: first 80 bytes in DFTAG_JPEG, remainder in
  // DFTAG_CI
  uint16 ref_old = Hnewref(fid);
  const int32_t header_len = 80;
  int32 aid_hdr = Hstartwrite(fid, DFTAG_JPEG, ref_old, header_len);
  ASSERT_NE(FAIL, aid_hdr);
  EXPECT_EQ(header_len, Hwrite(aid_hdr, header_len, raw_jpeg.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid_hdr));

  int32 body_len = jpeg_bytes - header_len;
  int32 aid_body = Hstartwrite(fid, DFTAG_CI, ref_old, body_len);
  ASSERT_NE(FAIL, aid_body);
  EXPECT_EQ(body_len, Hwrite(aid_body, body_len, raw_jpeg.data() + header_len));
  EXPECT_EQ(SUCCEED, Hendaccess(aid_body));

  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and decompress using old-style scheme DFTAG_JPEG
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  std::vector<uint8_t> decomp_image(num_pixels * 3, 0);
  EXPECT_EQ(SUCCEED, DFCIunjpeg(fid, DFTAG_CI, ref_old, decomp_image.data(),
                                xdim, ydim, DFTAG_JPEG));
}

// 4. Corrupted JPEG stream triggering setjmp error recovery
TEST_F(DfunjpegTest, CorruptJpegTriggersSetjmpRecovery) {
  std::string path = GetTempFilePath("dfunjpeg_corrupt.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  // Write invalid garbage bytes to DFTAG_CI
  const uint8_t garbage[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  int32 aid = Hstartwrite(fid, DFTAG_CI, ref, sizeof(garbage));
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(sizeof(garbage), Hwrite(aid, sizeof(garbage), garbage));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for read
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  std::vector<uint8_t> decomp(64, 0);
  // Should trigger setjmp recovery and return -1 without crashing or leaking
  EXPECT_EQ(-1,
            DFCIunjpeg(fid, DFTAG_CI, ref, decomp.data(), 8, 8, DFTAG_JPEG5));
}

// 5. Direct testing of hdf_skip_input_data
TEST_F(DfunjpegTest, DirectSkipInputData) {
  std::string path = GetTempFilePath("dfunjpeg_skip.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 ref = Hnewref(fid);
  std::vector<uint8_t> data(8192, 42);
  int32 aid = Hstartwrite(fid, DFTAG_CI, ref, data.size());
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(data.size(), Hwrite(aid, data.size(), data.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  struct jpeg_decompress_struct cinfo = {};
  struct jpeg_error_mgr jerr = {};
  cinfo.err = jpeg_std_error(&jerr);

  EXPECT_EQ(SUCCEED, jpeg_HDF_src(&cinfo, fid, DFTAG_CI, ref, nullptr, 0, 0,
                                  DFTAG_JPEG5));
  cinfo.src->init_source(&cinfo);

  // Test skipping fewer bytes than buffer size
  hdf_skip_input_data(&cinfo, 100);

  // Test skipping more bytes than current buffer to exercise while loop
  hdf_skip_input_data(&cinfo, 5000);

  // Skip remaining bytes to EOF and trigger EOI byte insertion
  hdf_skip_input_data(&cinfo, 4000);
  EXPECT_TRUE(cinfo.src->fill_input_buffer(&cinfo));
  EXPECT_EQ(2, cinfo.src->bytes_in_buffer);
  EXPECT_EQ(static_cast<JOCTET>(0xFF), cinfo.src->next_input_byte[0]);
  EXPECT_EQ(static_cast<JOCTET>(JPEG_EOI), cinfo.src->next_input_byte[1]);

  cinfo.src->term_source(&cinfo);
  EXPECT_EQ(SUCCEED, jpeg_HDF_src_term(&cinfo));
}

}  // namespace
}  // namespace hdf4
