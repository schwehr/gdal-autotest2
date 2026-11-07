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

#include <cstdint>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

class Df24Test : public ::testing::Test {
 protected:
  void SetUp() override {
    DF24restart();
    DF24setil(DFIL_PIXEL);
    DF24reqil(DFIL_PIXEL);
    DF24setcompress(COMP_NONE, nullptr);
    HEclear();
  }

  void TearDown() override {
    DF24restart();
    DF24setil(DFIL_PIXEL);
    DF24reqil(DFIL_PIXEL);
    DF24setcompress(COMP_NONE, nullptr);
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  std::vector<uint8_t> MakePatternImage(int32_t width, int32_t height) {
    std::vector<uint8_t> data(width * height * 3);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>(i % 256);
    }
    return data;
  }

  void Write8BitRig(const std::string& path, uint16_t rig_ref) {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    // 14 bytes: int32 xdim (4), int32 ydim (4), uint16 nt_tag (0),
    // uint16 nt_ref (0), int16 ncomponents (1).
    const uint8_t id_buf[14] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 0, 0, 1};
    ASSERT_NE(Hputelement(fid, DFTAG_ID, rig_ref, id_buf, sizeof(id_buf)),
              FAIL);

    // Write dummy image data.
    const uint8_t dummy_pix[4 * 4] = {};
    ASSERT_NE(Hputelement(fid, DFTAG_RI, rig_ref, dummy_pix, sizeof(dummy_pix)),
              FAIL);

    // Create RIG pointing to ID element and RI element.
    const int32_t gid = DFdisetup(2);
    ASSERT_NE(gid, FAIL);
    ASSERT_EQ(DFdiput(gid, DFTAG_ID, rig_ref), SUCCEED);
    ASSERT_EQ(DFdiput(gid, DFTAG_RI, rig_ref), SUCCEED);
    ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, rig_ref), 0);
    ASSERT_NE(Hclose(fid), FAIL);
  }
};

TEST_F(Df24Test, ReqilAndSetil) {
  EXPECT_EQ(DF24reqil(DFIL_PIXEL), SUCCEED);
  EXPECT_EQ(DF24reqil(DFIL_LINE), SUCCEED);
  EXPECT_EQ(DF24reqil(DFIL_PLANE), SUCCEED);

  EXPECT_EQ(DF24setil(DFIL_PIXEL), SUCCEED);
  EXPECT_EQ(DF24setil(DFIL_LINE), SUCCEED);
  EXPECT_EQ(DF24setil(DFIL_PLANE), SUCCEED);
  EXPECT_EQ(DF24setil(-1), FAIL);
}

TEST_F(Df24Test, Setdims) {
  EXPECT_EQ(DF24setdims(10, 20), SUCCEED);
  EXPECT_EQ(DF24setdims(0, 10), FAIL);
  EXPECT_EQ(DF24setdims(10, -1), FAIL);
}

TEST_F(Df24Test, Setcompress) {
  EXPECT_EQ(DF24setcompress(COMP_NONE, nullptr), SUCCEED);

  comp_info cinfo{};
  EXPECT_EQ(DF24setcompress(COMP_RLE, &cinfo), SUCCEED);

  cinfo.jpeg.quality = 75;
  cinfo.jpeg.force_baseline = 1;
  EXPECT_EQ(DF24setcompress(COMP_JPEG, &cinfo), SUCCEED);

  EXPECT_EQ(DF24setcompress(-999, nullptr), FAIL);
}

TEST_F(Df24Test, DimErrorsOnPutAndAdd) {
  const std::vector<uint8_t> dummy(16);

  HEclear();
  EXPECT_EQ(DF24putimage("invalid_put.hdf", dummy.data(), -1, 10), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADDIM);

  HEclear();
  EXPECT_EQ(DF24addimage("invalid_add.hdf", dummy.data(), 10, 0), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADDIM);
}

TEST_F(Df24Test, GetimageArgErrors) {
  std::vector<uint8_t> buf(3 * 4 * 4);

  HEclear();
  EXPECT_EQ(DF24getimage(nullptr, buf.data(), 4, 4), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  HEclear();
  EXPECT_EQ(DF24getimage("", buf.data(), 4, 4), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  HEclear();
  EXPECT_EQ(DF24getimage("file.hdf", nullptr, 4, 4), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  HEclear();
  EXPECT_EQ(DF24getimage("file.hdf", buf.data(), 0, 4), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  HEclear();
  EXPECT_EQ(DF24getimage("file.hdf", buf.data(), 4, -1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);
}

TEST_F(Df24Test, GetdimsErrors) {
  int32_t xdim = 0;
  int32_t ydim = 0;
  int il = 0;

  HEclear();
  EXPECT_EQ(DF24getdims("non_existent_file.hdf", &xdim, &ydim, &il), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_NODIM);

  // File containing only an 8-bit image: DF24getdims loops past 8-bit images
  // (ncomps == 1 != 3) and returns FAIL with DFE_NODIM when no 24-bit images
  // exist.
  const std::string path = GetTempFilePath("df24_8bit_only.hdf");
  Write8BitRig(path, 10);
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  HEclear();
  EXPECT_EQ(DF24getdims(path.c_str(), &xdim, &ydim, &il), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_NODIM);

  // DF24getimage directly on a file with no 24-bit images triggers DFE_NODIM.
  std::vector<uint8_t> buf(3 * 4 * 4);
  HEclear();
  EXPECT_EQ(DF24getimage(path.c_str(), buf.data(), 4, 4), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_NODIM);
}

TEST_F(Df24Test, RoundTripPutAndGet) {
  const std::string path = GetTempFilePath("df24_roundtrip.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const int32_t width = 4;
  const int32_t height = 3;
  const auto src_img = MakePatternImage(width, height);

  ASSERT_EQ(DF24putimage(path.c_str(), src_img.data(), width, height), SUCCEED);
  EXPECT_GT(DF24lastref(), 0);

  int32_t xdim = 0;
  int32_t ydim = 0;
  int il = -1;
  ASSERT_EQ(DF24getdims(path.c_str(), &xdim, &ydim, &il), SUCCEED);
  EXPECT_EQ(xdim, width);
  EXPECT_EQ(ydim, height);
  EXPECT_EQ(il, DFIL_PIXEL);

  std::vector<uint8_t> dst_img(width * height * 3);

  // Buffer dimensions smaller than image triggers DFE_BADDIM.
  HEclear();
  EXPECT_EQ(DF24getimage(path.c_str(), dst_img.data(), width - 1, height),
            FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADDIM);

  HEclear();
  EXPECT_EQ(DF24getimage(path.c_str(), dst_img.data(), width, height - 1),
            FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADDIM);

  // Successful read.
  ASSERT_EQ(DF24getimage(path.c_str(), dst_img.data(), width, height), SUCCEED);
  EXPECT_EQ(dst_img, src_img);
}

TEST_F(Df24Test, DirectGetimageWithoutGetdims) {
  const std::string path = GetTempFilePath("df24_direct_get.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const int32_t width = 3;
  const int32_t height = 3;
  const auto src_img = MakePatternImage(width, height);

  ASSERT_EQ(DF24putimage(path.c_str(), src_img.data(), width, height), SUCCEED);
  ASSERT_EQ(DF24restart(), SUCCEED);

  // Read directly without prior DF24getdims (Newdata is 0).
  std::vector<uint8_t> dst_img(width * height * 3);
  ASSERT_EQ(DF24getimage(path.c_str(), dst_img.data(), width, height), SUCCEED);
  EXPECT_EQ(dst_img, src_img);
}

TEST_F(Df24Test, MultipleImagesWithAddAndRefTraversal) {
  const std::string path = GetTempFilePath("df24_multiple.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  // First image: 4x2
  const auto img1 = MakePatternImage(4, 2);
  ASSERT_EQ(DF24putimage(path.c_str(), img1.data(), 4, 2), SUCCEED);
  const uint16_t ref1 = DF24lastref();
  EXPECT_GT(ref1, 0);

  // Second image: 2x3 with explicit DF24setdims
  const auto img2 = MakePatternImage(2, 3);
  ASSERT_EQ(DF24setdims(2, 3), SUCCEED);
  ASSERT_EQ(DF24addimage(path.c_str(), img2.data(), 2, 3), SUCCEED);
  const uint16_t ref2 = DF24lastref();
  EXPECT_GT(ref2, 0);
  EXPECT_NE(ref1, ref2);

  // Verify image count.
  EXPECT_EQ(DF24nimages(path.c_str()), 2);

  // Read images sequentially.
  int32_t xdim = 0;
  int32_t ydim = 0;
  int il = 0;
  ASSERT_EQ(DF24getdims(path.c_str(), &xdim, &ydim, &il), SUCCEED);
  EXPECT_EQ(xdim, 4);
  EXPECT_EQ(ydim, 2);
  std::vector<uint8_t> read_buf(4 * 2 * 3);
  ASSERT_EQ(DF24getimage(path.c_str(), read_buf.data(), 4, 2), SUCCEED);
  EXPECT_EQ(read_buf, img1);

  ASSERT_EQ(DF24getdims(path.c_str(), &xdim, &ydim, &il), SUCCEED);
  EXPECT_EQ(xdim, 2);
  EXPECT_EQ(ydim, 3);
  read_buf.resize(2 * 3 * 3);
  ASSERT_EQ(DF24getimage(path.c_str(), read_buf.data(), 2, 3), SUCCEED);
  EXPECT_EQ(read_buf, img2);

  // Restart and read ref2 directly.
  ASSERT_EQ(DF24restart(), SUCCEED);
  ASSERT_EQ(DF24readref(path.c_str(), ref2), SUCCEED);
  ASSERT_EQ(DF24getdims(path.c_str(), &xdim, &ydim, &il), SUCCEED);
  EXPECT_EQ(xdim, 2);
  EXPECT_EQ(ydim, 3);
  ASSERT_EQ(DF24getimage(path.c_str(), read_buf.data(), 2, 3), SUCCEED);
  EXPECT_EQ(read_buf, img2);
}

TEST_F(Df24Test, NimagesErrorsAndMixedImages) {
  HEclear();
  EXPECT_EQ(DF24nimages("non_existent_file.hdf"), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADOPEN);

  // Empty file has 0 images.
  const std::string empty_path = GetTempFilePath("df24_empty.hdf");
  const int32_t empty_fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(empty_fid, FAIL);
  ASSERT_NE(Hclose(empty_fid), FAIL);
  absl::Cleanup empty_cleanup = [&]() { std::filesystem::remove(empty_path); };
  EXPECT_EQ(DF24nimages(empty_path.c_str()), 0);

  // File with an 8-bit image followed by a 24-bit image.
  const std::string mixed_path = GetTempFilePath("df24_mixed.hdf");
  absl::Cleanup mixed_cleanup = [&]() { std::filesystem::remove(mixed_path); };
  Write8BitRig(mixed_path, 11);

  const auto img24 = MakePatternImage(3, 3);
  ASSERT_EQ(DF24addimage(mixed_path.c_str(), img24.data(), 3, 3), SUCCEED);

  // nimages correctly filters for 24-bit images (ncomponents == 3).
  EXPECT_EQ(DF24nimages(mixed_path.c_str()), 1);

  // DF24getdims correctly skips the 8-bit RIG and finds the 24-bit RIG.
  int32_t xdim = 0;
  int32_t ydim = 0;
  int il = 0;
  ASSERT_EQ(DF24restart(), SUCCEED);
  ASSERT_EQ(DF24getdims(mixed_path.c_str(), &xdim, &ydim, &il), SUCCEED);
  EXPECT_EQ(xdim, 3);
  EXPECT_EQ(ydim, 3);
}

TEST_F(Df24Test, NimagesMalformedRigGetelemError) {
  const std::string path = GetTempFilePath("df24_malformed_rig.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  // Write a RIG that references DFTAG_ID with a nonexistent ref (9999).
  const int32_t gid = DFdisetup(1);
  ASSERT_NE(gid, FAIL);
  ASSERT_EQ(DFdiput(gid, DFTAG_ID, 9999), SUCCEED);
  ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, 100), 0);
  ASSERT_NE(Hclose(fid), FAIL);

  EXPECT_EQ(DF24nimages(path.c_str()), FAIL);
}

TEST_F(Df24Test, NimagesCorruptedRigInternalError) {
  const std::string path = GetTempFilePath("df24_corrupt_rig.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  // Write a RIG element with 8 dummy bytes.
  const uint8_t dummy[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_NE(Hputelement(fid, DFTAG_RIG, 101, dummy, sizeof(dummy)), FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  // Truncate file so that Hgetelement in DFdiread will fail.
  const auto original_size = std::filesystem::file_size(path);
  ASSERT_GT(original_size, sizeof(dummy));
  std::filesystem::resize_file(path, original_size - sizeof(dummy));

  EXPECT_EQ(DF24nimages(path.c_str()), FAIL);
}

TEST_F(Df24Test, InterlaceConversion) {
  const std::string path = GetTempFilePath("df24_interlace.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const int32_t width = 3;
  const int32_t height = 2;
  const auto src_img = MakePatternImage(width, height);

  ASSERT_EQ(DF24setil(DFIL_PLANE), SUCCEED);
  ASSERT_EQ(DF24putimage(path.c_str(), src_img.data(), width, height), SUCCEED);

  // Read back requested as DFIL_PIXEL.
  ASSERT_EQ(DF24reqil(DFIL_PIXEL), SUCCEED);
  int32_t xdim = 0;
  int32_t ydim = 0;
  int il = 0;
  ASSERT_EQ(DF24getdims(path.c_str(), &xdim, &ydim, &il), SUCCEED);
  std::vector<uint8_t> dst_img(width * height * 3);
  ASSERT_EQ(DF24getimage(path.c_str(), dst_img.data(), width, height), SUCCEED);
}

TEST_F(Df24Test, CompressionSchemes) {
  const int32_t width = 16;
  const int32_t height = 16;
  const auto src_img = MakePatternImage(width, height);

  // RLE compression scheme setting.
  {
    comp_info cinfo{};
    EXPECT_EQ(DF24setcompress(COMP_RLE, &cinfo), SUCCEED);
  }

  // JPEG compression round-trip.
  {
    const std::string path = GetTempFilePath("df24_jpeg.hdf");
    absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

    comp_info cinfo{};
    cinfo.jpeg.quality = 90;
    cinfo.jpeg.force_baseline = 1;
    ASSERT_EQ(DF24setcompress(COMP_JPEG, &cinfo), SUCCEED);
    ASSERT_EQ(DF24putimage(path.c_str(), src_img.data(), width, height),
              SUCCEED);

    std::vector<uint8_t> dst(width * height * 3);
    ASSERT_EQ(DF24getimage(path.c_str(), dst.data(), width, height), SUCCEED);
  }
}

}  // namespace
}  // namespace hdf4
