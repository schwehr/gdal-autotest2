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

// Tests for the multi-file general raster (GR) interface (hdf/src/mfgr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/mfgr.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/mfgr.h
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/mgr.c

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
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
#include "third_party/hdf4/hdf/src/hlimits.h"
#include "third_party/hdf4/hdf/src/hproto.h"
#include "third_party/hdf4/hdf/src/htags.h"
#include "third_party/hdf4/hdf/src/mfgr.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::FloatEq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

std::string GetTempFilePath(std::string_view filename) {
  const std::string path =
      (std::filesystem::path(::testing::TempDir()) / filename).string();
  std::filesystem::remove(path);
  return path;
}

TEST(MfgrTest, StartAndEndArgumentValidation) {
  // Test invalid file IDs
  EXPECT_THAT(GRstart(FAIL), Eq(FAIL));
  EXPECT_THAT(GRstart(9999), Eq(FAIL));
  EXPECT_THAT(GRend(FAIL), Eq(FAIL));
  EXPECT_THAT(GRend(9999), Eq(FAIL));

  const std::string path = GetTempFilePath("mfgr_start_end.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t gr_id = GRstart(fid);
  ASSERT_THAT(gr_id, Ne(FAIL));

  int32 n_images = -1, n_file_attrs = -1;
  EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_file_attrs), Eq(SUCCEED));
  EXPECT_THAT(n_images, Eq(0));
  EXPECT_THAT(n_file_attrs, Eq(0));

  // Arguments for GRfileinfo
  EXPECT_THAT(GRfileinfo(FAIL, &n_images, &n_file_attrs), Eq(FAIL));
  EXPECT_THAT(GRfileinfo(gr_id, nullptr, &n_file_attrs), Eq(SUCCEED));
  EXPECT_THAT(GRfileinfo(gr_id, &n_images, nullptr), Eq(SUCCEED));

  // Invalid arguments for GRselect and GRnametoindex
  EXPECT_THAT(GRselect(gr_id, -1), Eq(FAIL));
  EXPECT_THAT(GRselect(gr_id, 0), Eq(FAIL));
  EXPECT_THAT(GRselect(FAIL, 0), Eq(FAIL));
  EXPECT_THAT(GRnametoindex(gr_id, "nonexistent"), Eq(FAIL));
  EXPECT_THAT(GRnametoindex(FAIL, "nonexistent"), Eq(FAIL));
  EXPECT_THAT(GRnametoindex(gr_id, nullptr), Eq(FAIL));

  // Invalid arguments for GRreftoindex
  EXPECT_THAT(GRreftoindex(gr_id, 0), Eq(FAIL));
  EXPECT_THAT(GRreftoindex(FAIL, 1), Eq(FAIL));

  EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
}

TEST(MfgrTest, BasicRasterCreateWriteAndRead) {
  const std::string path = GetTempFilePath("mfgr_basic.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kWidth = 8;
  constexpr int32_t kHeight = 6;
  constexpr int32_t kNumPixels = kWidth * kHeight;
  std::vector<uint8_t> write_data(kNumPixels);
  std::iota(write_data.begin(), write_data.end(), 10);

  // 1. Create file and write a 1-component 8-bit image
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kWidth, kHeight};
    const int32_t ri_id = GRcreate(gr_id, "BasicImage", 1, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Verify index and ref lookups
    EXPECT_THAT(GRnametoindex(gr_id, "BasicImage"), Eq(0));
    const uint16 ref = GRidtoref(ri_id);
    EXPECT_THAT(ref, Gt(0));
    EXPECT_THAT(GRreftoindex(gr_id, ref), Eq(0));

    // Write image data
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kWidth, kHeight};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, write_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Re-open file and read back the image
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 n_images = 0, n_attrs = 0;
    EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_attrs), Eq(SUCCEED));
    EXPECT_THAT(n_images, Eq(1));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    char name[H4_MAX_GR_NAME + 1] = {0};
    int32 ncomp = 0, data_type = 0, interlace = -1;
    int32 dims[2] = {0, 0};
    int32 nattrs = -1;
    EXPECT_THAT(
        GRgetiminfo(ri_id, name, &ncomp, &data_type, &interlace, dims, &nattrs),
        Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name), Eq("BasicImage"));
    EXPECT_THAT(ncomp, Eq(1));
    EXPECT_THAT(data_type, Eq(DFNT_UINT8));
    EXPECT_THAT(interlace, Eq(MFGR_INTERLACE_PIXEL));
    EXPECT_THAT(dims[0], Eq(kWidth));
    EXPECT_THAT(dims[1], Eq(kHeight));
    EXPECT_THAT(nattrs, Eq(0));

    // Full read
    std::vector<uint8_t> read_data(kNumPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kWidth, kHeight};
    EXPECT_THAT(GRreadimage(ri_id, start, stride, count, read_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_data, ElementsAreArray(write_data));

    // Sub-region read: 2x2 block at (1, 1) with stride 2
    int32 sub_start[2] = {1, 1};
    int32 sub_stride[2] = {2, 2};
    int32 sub_count[2] = {2, 2};
    std::vector<uint8_t> sub_data(4, 0);
    EXPECT_THAT(
        GRreadimage(ri_id, sub_start, sub_stride, sub_count, sub_data.data()),
        Eq(SUCCEED));
    // Expected at (1,1)=10+1*8+1=19, (3,1)=10+1*8+3=21, (1,3)=10+3*8+1=35,
    // (3,3)=10+3*8+3=37
    EXPECT_THAT(sub_data[0], Eq(write_data[1 * kWidth + 1]));
    EXPECT_THAT(sub_data[1], Eq(write_data[1 * kWidth + 3]));
    EXPECT_THAT(sub_data[2], Eq(write_data[3 * kWidth + 1]));
    EXPECT_THAT(sub_data[3], Eq(write_data[3 * kWidth + 3]));

    // Query nonexistent image name when images exist (loops over tree)
    EXPECT_THAT(GRnametoindex(gr_id, "NonExistentImage"), Eq(FAIL));

    // Calling GRsetchunkcache on non-chunked image fails
    EXPECT_THAT(GRsetchunkcache(ri_id, 10, 0), Eq(FAIL));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, MultiComponentAndInterlaceSchemes) {
  const std::string path = GetTempFilePath("mfgr_interlace.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kWidth = 4;
  constexpr int32_t kHeight = 4;
  constexpr int32_t kNComp = 3;
  constexpr int32_t kTotalBytes = kWidth * kHeight * kNComp;

  // Create test data
  std::vector<uint8_t> pixel_data(kTotalBytes);
  std::iota(pixel_data.begin(), pixel_data.end(), 1);

  // 1. Create pixel-interlaced RGB image
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kWidth, kHeight};
    const int32_t ri_id = GRcreate(gr_id, "RGB_Pixel", kNComp, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kWidth, kHeight};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, pixel_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));

    // 2. Create component-interlaced image
    const int32_t ri_comp = GRcreate(gr_id, "RGB_Comp", kNComp, DFNT_UINT8,
                                     MFGR_INTERLACE_COMPONENT, dims);
    ASSERT_THAT(ri_comp, Ne(FAIL));
    EXPECT_THAT(GRwriteimage(ri_comp, start, stride, count, pixel_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_comp), Eq(SUCCEED));

    // 3. Create line-interlaced image
    const int32_t ri_line = GRcreate(gr_id, "RGB_Line", kNComp, DFNT_UINT8,
                                     MFGR_INTERLACE_LINE, dims);
    ASSERT_THAT(ri_line, Ne(FAIL));
    EXPECT_THAT(GRwriteimage(ri_line, start, stride, count, pixel_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_line), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Read back and test GRreqimageil (interlace conversion on read)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Request component interlace on reading pixel-interlaced image
    EXPECT_THAT(GRreqimageil(ri_id, MFGR_INTERLACE_COMPONENT), Eq(SUCCEED));
    std::vector<uint8_t> comp_read(kTotalBytes, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kWidth, kHeight};
    EXPECT_THAT(GRreadimage(ri_id, start, stride, count, comp_read.data()),
                Eq(SUCCEED));

    // Verify first component begins with pixel_data[0]
    EXPECT_THAT(comp_read[0], Eq(pixel_data[0]));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, PaletteAndLutOperations) {
  const std::string path = GetTempFilePath("mfgr_lut.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kLutEntries = 256;
  constexpr int32_t kLutComps = 3;
  constexpr int32_t kLutSize = kLutEntries * kLutComps;
  std::vector<uint8_t> lut_data(kLutSize);
  for (int i = 0; i < kLutEntries; ++i) {
    lut_data[i * 3 + 0] = static_cast<uint8_t>(i);
    lut_data[i * 3 + 1] = static_cast<uint8_t>(255 - i);
    lut_data[i * 3 + 2] = static_cast<uint8_t>(i / 2);
  }

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {16, 16};
    const int32_t ri_id = GRcreate(gr_id, "IndexedImage", 1, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Initially 0 LUTs
    EXPECT_THAT(GRgetnluts(ri_id), Eq(0));

    // Write LUT
    EXPECT_THAT(GRwritelut(ri_id, kLutComps, DFNT_UINT8, MFGR_INTERLACE_PIXEL,
                           kLutEntries, lut_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRgetnluts(ri_id), Eq(1));

    // Access LUT ID (owned by ri_id, do NOT call GRendaccess on lut_id!)
    const int32_t lut_id = GRgetlutid(ri_id, 0);
    ASSERT_THAT(lut_id, Ne(FAIL));

    int32 ncomp = 0, data_type = 0, interlace = -1, n_entries = 0;
    EXPECT_THAT(
        GRgetlutinfo(lut_id, &ncomp, &data_type, &interlace, &n_entries),
        Eq(SUCCEED));
    EXPECT_THAT(ncomp, Eq(kLutComps));
    EXPECT_THAT(data_type, Eq(DFNT_UINT8));
    EXPECT_THAT(interlace, Eq(MFGR_INTERLACE_PIXEL));
    EXPECT_THAT(n_entries, Eq(kLutEntries));

    const uint16 lut_ref = GRluttoref(lut_id);
    EXPECT_THAT(lut_ref, Gt(0));

    // Read LUT back
    std::vector<uint8_t> read_lut(kLutSize, 0);
    EXPECT_THAT(GRreadlut(lut_id, read_lut.data()), Eq(SUCCEED));
    EXPECT_THAT(read_lut, ElementsAreArray(lut_data));

    // Test interlace conversion on LUT read
    EXPECT_THAT(GRreqlutil(lut_id, MFGR_INTERLACE_COMPONENT), Eq(SUCCEED));
    std::vector<uint8_t> comp_lut(kLutSize, 0);
    EXPECT_THAT(GRreadlut(lut_id, comp_lut.data()), Eq(SUCCEED));
    EXPECT_THAT(comp_lut[0], Eq(lut_data[0]));

    // Write dummy image data so file has valid raster
    std::vector<uint8_t> img(256, 1);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {16, 16};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, img.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open and verify LUT persisted
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    EXPECT_THAT(GRgetnluts(ri_id), Eq(1));
    const int32_t lut_id = GRgetlutid(ri_id, 0);
    ASSERT_THAT(lut_id, Ne(FAIL));

    std::vector<uint8_t> read_lut(kLutSize, 0);
    EXPECT_THAT(GRreadlut(lut_id, read_lut.data()), Eq(SUCCEED));
    EXPECT_THAT(read_lut, ElementsAreArray(lut_data));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, AttributeOperations) {
  const std::string path = GetTempFilePath("mfgr_attr.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // File-level attribute
    constexpr std::string_view kFileComment = "Global GR Test File";
    EXPECT_THAT(
        GRsetattr(gr_id, "FileComment", DFNT_CHAR,
                  static_cast<int32>(kFileComment.size()), kFileComment.data()),
        Eq(SUCCEED));

    int32 file_val = 12345;
    EXPECT_THAT(GRsetattr(gr_id, "FileVersion", DFNT_INT32, 1, &file_val),
                Eq(SUCCEED));
    // Overwriting existing attribute with different number type fails
    int16_t wrong_nt = 42;
    EXPECT_THAT(GRsetattr(gr_id, "FileVersion", DFNT_INT16, 1, &wrong_nt),
                Eq(FAIL));

    // Create an image and set image-level attributes
    int32 dims[2] = {4, 4};
    const int32_t ri_id =
        GRcreate(gr_id, "AttrImage", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    float cal_vals[2] = {0.5f, 10.0f};
    EXPECT_THAT(GRsetattr(ri_id, "Calibration", DFNT_FLOAT32, 2, cal_vals),
                Eq(SUCCEED));

    // Write dummy image data
    std::vector<uint8_t> img(16, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {4, 4};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, img.data()),
                Eq(SUCCEED));

    // Query attributes while open
    EXPECT_THAT(GRfindattr(gr_id, "FileComment"), Eq(0));
    EXPECT_THAT(GRfindattr(gr_id, "FileVersion"), Eq(1));
    EXPECT_THAT(GRfindattr(gr_id, "NonExistent"), Eq(FAIL));

    char attr_name[H4_MAX_NC_NAME + 1] = {0};
    int32 attr_type = 0, attr_count = 0;
    EXPECT_THAT(GRattrinfo(gr_id, 0, attr_name, &attr_type, &attr_count),
                Eq(SUCCEED));
    EXPECT_THAT(std::string_view(attr_name), Eq("FileComment"));
    EXPECT_THAT(attr_type, Eq(DFNT_CHAR));
    EXPECT_THAT(attr_count, Eq(static_cast<int32>(kFileComment.size())));

    std::vector<char> str_buf(attr_count + 1, '\0');
    EXPECT_THAT(GRgetattr(gr_id, 0, str_buf.data()), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(str_buf.data()), Eq(kFileComment));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open and verify attributes persisted
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 n_images = 0, n_attrs = 0;
    EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_attrs), Eq(SUCCEED));
    EXPECT_THAT(n_attrs, Eq(2));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    EXPECT_THAT(GRfindattr(ri_id, "Calibration"), Eq(0));
    float read_cal[2] = {0};
    EXPECT_THAT(GRgetattr(ri_id, 0, read_cal), Eq(SUCCEED));
    EXPECT_THAT(read_cal[0], Eq(0.5f));
    EXPECT_THAT(read_cal[1], Eq(10.0f));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, CompressionAndChunking) {
  const std::string path = GetTempFilePath("mfgr_comp_chunk.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 16;
  constexpr int32_t kNumPixels = kDim * kDim;
  std::vector<uint8_t> write_data(kNumPixels);
  for (int i = 0; i < kNumPixels; ++i) {
    write_data[i] = static_cast<uint8_t>(i % 32);
  }

  // 1. Create Deflate-compressed image
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kDim, kDim};
    const int32_t ri_id = GRcreate(gr_id, "DeflateImage", 1, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    comp_info c_info;
    std::memset(&c_info, 0, sizeof(c_info));
    c_info.deflate_setting.level = 6;
    EXPECT_THAT(GRsetcompress(ri_id, COMP_CODE_DEFLATE, &c_info), Eq(SUCCEED));

    comp_coder_t coder_type = COMP_CODE_NONE;
    comp_info q_info;
    std::memset(&q_info, 0, sizeof(q_info));
    EXPECT_THAT(GRgetcompress(ri_id, &coder_type, &q_info), Eq(SUCCEED));
    EXPECT_THAT(coder_type, Eq(COMP_CODE_DEFLATE));

    comp_coder_t ctype = COMP_CODE_NONE;
    EXPECT_THAT(GRgetcomptype(ri_id, &ctype), Eq(SUCCEED));
    EXPECT_THAT(ctype, Eq(COMP_CODE_DEFLATE));

    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kDim, kDim};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, write_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));

    // 2. Create Chunked image
    const int32_t ri_chunk = GRcreate(gr_id, "ChunkedImage", 1, DFNT_UINT8,
                                      MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_chunk, Ne(FAIL));

    HDF_CHUNK_DEF chunk_def;
    std::memset(&chunk_def, 0, sizeof(chunk_def));
    chunk_def.chunk_lengths[0] = 8;
    chunk_def.chunk_lengths[1] = 8;
    EXPECT_THAT(GRsetchunk(ri_chunk, chunk_def, HDF_CHUNK), Eq(SUCCEED));

    // Configure chunk cache
    EXPECT_THAT(GRsetchunkcache(ri_chunk, 10, 0), Gt(0));

    // Query chunk info
    HDF_CHUNK_DEF q_chunk;
    std::memset(&q_chunk, 0, sizeof(q_chunk));
    int32 flag = 0;
    EXPECT_THAT(GRgetchunkinfo(ri_chunk, &q_chunk, &flag), Eq(SUCCEED));
    EXPECT_THAT(flag, Eq(HDF_CHUNK));
    EXPECT_THAT(q_chunk.chunk_lengths[0], Eq(8));
    EXPECT_THAT(q_chunk.chunk_lengths[1], Eq(8));

    // Write chunked image using normal writeimage
    EXPECT_THAT(GRwriteimage(ri_chunk, start, stride, count, write_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_chunk), Eq(SUCCEED));

    // 3. Create another Chunked image and write chunk directly when img_aid ==
    // 0
    const int32_t ri_direct = GRcreate(gr_id, "DirectChunk", 1, DFNT_UINT8,
                                       MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_direct, Ne(FAIL));
    EXPECT_THAT(GRsetchunk(ri_direct, chunk_def, HDF_CHUNK), Eq(SUCCEED));
    int32 direct_origin[2] = {0, 0};
    std::vector<uint8_t> chk_block(64, 42);
    EXPECT_THAT(GRwritechunk(ri_direct, direct_origin, chk_block.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_direct), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open and verify reading compressed and chunked images
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // Read Deflate image
    const int32_t ri_def = GRselect(gr_id, 0);
    ASSERT_THAT(ri_def, Ne(FAIL));
    std::vector<uint8_t> read_def(kNumPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kDim, kDim};
    EXPECT_THAT(GRreadimage(ri_def, start, stride, count, read_def.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_def, ElementsAreArray(write_data));
    EXPECT_THAT(GRendaccess(ri_def), Eq(SUCCEED));

    // Read Chunked image
    const int32_t ri_chk = GRselect(gr_id, 1);
    ASSERT_THAT(ri_chk, Ne(FAIL));
    std::vector<uint8_t> read_chk(kNumPixels, 0);
    EXPECT_THAT(GRreadimage(ri_chk, start, stride, count, read_chk.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_chk, ElementsAreArray(write_data));

    // Read a specific chunk directly: origin (0, 0)
    int32 origin[2] = {0, 0};
    std::vector<uint8_t> chunk_buf(64, 0);
    EXPECT_THAT(GRreadchunk(ri_chk, origin, chunk_buf.data()), Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_chk), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, ExternalFileRaster) {
  const std::string path = GetTempFilePath("mfgr_ext.hdf");
  const std::string ext_path = GetTempFilePath("mfgr_ext_data.bin");
  absl::Cleanup remove_temp = [&path, &ext_path] {
    std::filesystem::remove(path);
    std::filesystem::remove(ext_path);
  };

  constexpr int32_t kDim = 8;
  constexpr int32_t kNumPixels = kDim * kDim;
  std::vector<uint8_t> write_data(kNumPixels, 42);

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kDim, kDim};
    const int32_t ri_id =
        GRcreate(gr_id, "ExtImage", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    EXPECT_THAT(GRsetexternalfile(ri_id, ext_path.c_str(), 0), Eq(SUCCEED));

    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kDim, kDim};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, write_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // External sidecar file should exist and have size >= kNumPixels
  EXPECT_TRUE(std::filesystem::exists(ext_path));
  EXPECT_THAT(std::filesystem::file_size(ext_path), Ge(kNumPixels));

  // Re-open and read back from external file
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    std::vector<uint8_t> read_data(kNumPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kDim, kDim};
    EXPECT_THAT(GRreadimage(ri_id, start, stride, count, read_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_data, ElementsAreArray(write_data));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, ChunkOperationsWriteChunkReadChunkAndCompression) {
  const std::string path = GetTempFilePath("mfgr_chunk_ops.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 16;
  constexpr int32_t kChunkLen = 8;
  constexpr int32_t kChunkPixels = kChunkLen * kChunkLen;

  // 1. Create file and chunked images
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kDim, kDim};

    // Uncompressed chunked image
    const int32_t ri_raw =
        GRcreate(gr_id, "RawChunk", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_raw, Ne(FAIL));

    HDF_CHUNK_DEF chunk_def;
    std::memset(&chunk_def, 0, sizeof(chunk_def));
    chunk_def.chunk_lengths[0] = kChunkLen;
    chunk_def.chunk_lengths[1] = kChunkLen;
    EXPECT_THAT(GRsetchunk(ri_raw, chunk_def, HDF_CHUNK), Eq(SUCCEED));

    // Test GRsetchunkcache setting and invalid argument
    EXPECT_THAT(GRsetchunkcache(ri_raw, 5, 0), Gt(0));
    EXPECT_THAT(GRsetchunkcache(ri_raw, 0, 0), Eq(FAIL));

    // Write chunks individually via GRwritechunk using chunk coordinates
    std::vector<uint8_t> chunk00(kChunkPixels, 10);
    std::vector<uint8_t> chunk10(kChunkPixels, 20);
    std::vector<uint8_t> chunk01(kChunkPixels, 30);
    std::vector<uint8_t> chunk11(kChunkPixels, 40);

    int32 org00[2] = {0, 0};
    int32 org10[2] = {1, 0};
    int32 org01[2] = {0, 1};
    int32 org11[2] = {1, 1};

    EXPECT_THAT(GRwritechunk(ri_raw, org00, chunk00.data()), Eq(SUCCEED));
    EXPECT_THAT(GRwritechunk(ri_raw, org10, chunk10.data()), Eq(SUCCEED));
    EXPECT_THAT(GRwritechunk(ri_raw, org01, chunk01.data()), Eq(SUCCEED));
    EXPECT_THAT(GRwritechunk(ri_raw, org11, chunk11.data()), Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_raw), Eq(SUCCEED));

    // Compressed chunked image (HDF_CHUNK | HDF_COMP)
    const int32_t ri_comp =
        GRcreate(gr_id, "CompChunk", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_comp, Ne(FAIL));

    HDF_CHUNK_DEF comp_chunk_def;
    std::memset(&comp_chunk_def, 0, sizeof(comp_chunk_def));
    comp_chunk_def.comp.chunk_lengths[0] = kChunkLen;
    comp_chunk_def.comp.chunk_lengths[1] = kChunkLen;
    comp_chunk_def.comp.comp_type = COMP_CODE_DEFLATE;
    comp_chunk_def.comp.cinfo.deflate_setting.level = 6;
    EXPECT_THAT(GRsetchunk(ri_comp, comp_chunk_def, HDF_CHUNK | HDF_COMP),
                Eq(SUCCEED));

    EXPECT_THAT(GRwritechunk(ri_comp, org00, chunk00.data()), Eq(SUCCEED));
    EXPECT_THAT(GRwritechunk(ri_comp, org10, chunk10.data()), Eq(SUCCEED));
    EXPECT_THAT(GRwritechunk(ri_comp, org01, chunk01.data()), Eq(SUCCEED));
    EXPECT_THAT(GRwritechunk(ri_comp, org11, chunk11.data()), Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_comp), Eq(SUCCEED));

    // Verify error branches for GRsetchunk
    const int32_t ri_err =
        GRcreate(gr_id, "ErrChunk", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_err, Ne(FAIL));
    // NBIT unsupported
    EXPECT_THAT(GRsetchunk(ri_err, chunk_def, HDF_CHUNK | HDF_NBIT), Eq(FAIL));
    // Invalid chunk flag
    EXPECT_THAT(GRsetchunk(ri_err, chunk_def, 0x1234), Eq(FAIL));
    // SZIP unsupported in Google3
    HDF_CHUNK_DEF szip_chunk_def;
    std::memset(&szip_chunk_def, 0, sizeof(szip_chunk_def));
    szip_chunk_def.comp.chunk_lengths[0] = kChunkLen;
    szip_chunk_def.comp.chunk_lengths[1] = kChunkLen;
    szip_chunk_def.comp.comp_type = COMP_CODE_SZIP;
    EXPECT_THAT(GRsetchunk(ri_err, szip_chunk_def, HDF_CHUNK | HDF_COMP),
                Eq(FAIL));

    // Chunk length < 1 fails
    HDF_CHUNK_DEF bad_len_chunk;
    std::memset(&bad_len_chunk, 0, sizeof(bad_len_chunk));
    bad_len_chunk.chunk_lengths[0] = 0;
    bad_len_chunk.chunk_lengths[1] = 4;
    EXPECT_THAT(GRsetchunk(ri_err, bad_len_chunk, HDF_CHUNK), Eq(FAIL));

    // Full image dimension chunks (distrib_type NONE)
    HDF_CHUNK_DEF full_chunk;
    std::memset(&full_chunk, 0, sizeof(full_chunk));
    full_chunk.chunk_lengths[0] = kDim;
    full_chunk.chunk_lengths[1] = kDim;
    EXPECT_THAT(GRsetchunk(ri_err, full_chunk, HDF_CHUNK), Eq(SUCCEED));

    // Error branches for GRwritechunk: null pointers
    EXPECT_THAT(GRwritechunk(ri_err, nullptr, chunk00.data()), Eq(FAIL));
    EXPECT_THAT(GRwritechunk(ri_err, org00, nullptr), Eq(FAIL));
    EXPECT_THAT(GRwritechunk(FAIL, org00, chunk00.data()), Eq(FAIL));

    // Error branches for GRsetchunkcache: bad cache or flags
    EXPECT_THAT(GRsetchunkcache(ri_err, 0, 0), Eq(FAIL));
    EXPECT_THAT(GRsetchunkcache(ri_err, 10, 999), Eq(FAIL));
    EXPECT_THAT(GRsetchunkcache(ri_err, 10, HDF_CACHEALL), Gt(0));

    EXPECT_THAT(GRendaccess(ri_err), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Re-open and verify chunks via GRreadchunk and GRreadimage
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kDim, kDim};
    int32 org00[2] = {0, 0};
    int32 org11[2] = {1, 1};

    // Raw chunk read
    const int32_t ri_raw = GRselect(gr_id, 0);
    ASSERT_THAT(ri_raw, Ne(FAIL));

    HDF_CHUNK_DEF q_chunk;
    int32 flags = 0;
    EXPECT_THAT(GRgetchunkinfo(ri_raw, &q_chunk, &flags), Eq(SUCCEED));
    EXPECT_THAT(flags, Eq(HDF_CHUNK));
    EXPECT_THAT(q_chunk.chunk_lengths[0], Eq(kChunkLen));
    EXPECT_THAT(q_chunk.chunk_lengths[1], Eq(kChunkLen));

    std::vector<uint8_t> read_chunk(kChunkPixels, 0);
    EXPECT_THAT(GRreadchunk(ri_raw, org00, read_chunk.data()), Eq(SUCCEED));
    EXPECT_THAT(read_chunk[0], Eq(10));
    EXPECT_THAT(read_chunk[kChunkPixels - 1], Eq(10));

    EXPECT_THAT(GRreadchunk(ri_raw, org11, read_chunk.data()), Eq(SUCCEED));
    EXPECT_THAT(read_chunk[0], Eq(40));
    EXPECT_THAT(read_chunk[kChunkPixels - 1], Eq(40));

    // Error branches for GRreadchunk: null pointers
    EXPECT_THAT(GRreadchunk(ri_raw, nullptr, read_chunk.data()), Eq(FAIL));
    EXPECT_THAT(GRreadchunk(ri_raw, org00, nullptr), Eq(FAIL));
    EXPECT_THAT(GRreadchunk(FAIL, org00, read_chunk.data()), Eq(FAIL));

    // Error branches for GRgetchunkinfo: null pointers
    EXPECT_THAT(GRgetchunkinfo(FAIL, &q_chunk, &flags), Eq(FAIL));
    // When chunk_def is null, flags is still populated
    flags = 0;
    EXPECT_THAT(GRgetchunkinfo(ri_raw, nullptr, &flags), Eq(SUCCEED));
    EXPECT_THAT(flags, Eq(HDF_CHUNK));

    EXPECT_THAT(GRendaccess(ri_raw), Eq(SUCCEED));

    // Comp chunk read
    const int32_t ri_comp = GRselect(gr_id, 1);
    ASSERT_THAT(ri_comp, Ne(FAIL));

    EXPECT_THAT(GRgetchunkinfo(ri_comp, &q_chunk, &flags), Eq(SUCCEED));
    EXPECT_THAT(flags, Eq(HDF_CHUNK | HDF_COMP));
    EXPECT_THAT(q_chunk.chunk_lengths[0], Eq(kChunkLen));
    EXPECT_THAT(q_chunk.chunk_lengths[1], Eq(kChunkLen));

    EXPECT_THAT(GRreadchunk(ri_comp, org00, read_chunk.data()), Eq(SUCCEED));
    EXPECT_THAT(read_chunk[0], Eq(10));

    // Read whole compressed chunked image via GRreadimage
    std::vector<uint8_t> read_all(kDim * kDim, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    EXPECT_THAT(GRreadimage(ri_comp, start, stride, dims, read_all.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_all[0], Eq(10));                             // (y=0, x=0)
    EXPECT_THAT(read_all[kChunkLen], Eq(30));                     // (y=0, x=8)
    EXPECT_THAT(read_all[kChunkLen * kDim], Eq(20));              // (y=8, x=0)
    EXPECT_THAT(read_all[kChunkLen * kDim + kChunkLen], Eq(40));  // (y=8, x=8)

    EXPECT_THAT(GRendaccess(ri_comp), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, PartialWritesAndFillValues) {
  const std::string path = GetTempFilePath("mfgr_fill.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  constexpr int32_t kTotalPixels = kDim * kDim;

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kDim, kDim};

    // 1. Partial write with custom fill value
    const int32_t ri_custom = GRcreate(gr_id, "CustomFill", 1, DFNT_UINT8,
                                       MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_custom, Ne(FAIL));

    constexpr uint8_t kFillVal = 0xAA;
    EXPECT_THAT(GRsetattr(ri_custom, FILL_ATTR, DFNT_UINT8, 1, &kFillVal),
                Eq(SUCCEED));

    // Read unwritten image to load fill_value into ri_ptr
    uint8_t read_fill = 0;
    int32 r_origin[2] = {0, 0};
    int32 r_count[2] = {1, 1};
    EXPECT_THAT(GRreadimage(ri_custom, r_origin, nullptr, r_count, &read_fill),
                Eq(SUCCEED));
    EXPECT_THAT(read_fill, Eq(kFillVal));

    // Write a 4x4 block at (2, 2)
    int32 start[2] = {2, 2};
    int32 stride[2] = {1, 1};
    int32 count[2] = {4, 4};
    std::vector<uint8_t> block_data(16, 0x55);
    EXPECT_THAT(
        GRwriteimage(ri_custom, start, stride, count, block_data.data()),
        Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_custom), Eq(SUCCEED));

    // 2. Partial write with stride > 1 and default fill value
    const int32_t ri_stride = GRcreate(gr_id, "StrideFill", 1, DFNT_UINT8,
                                       MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_stride, Ne(FAIL));

    int32 s_start[2] = {1, 1};
    int32 s_stride[2] = {2, 2};
    int32 s_count[2] = {3, 3};
    std::vector<uint8_t> s_data(9, 0x77);
    EXPECT_THAT(
        GRwriteimage(ri_stride, s_start, s_stride, s_count, s_data.data()),
        Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_stride), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Verify read back
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_custom = GRselect(gr_id, 0);
    ASSERT_THAT(ri_custom, Ne(FAIL));

    std::vector<uint8_t> read_custom(kTotalPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kDim, kDim};
    EXPECT_THAT(
        GRreadimage(ri_custom, start, stride, count, read_custom.data()),
        Eq(SUCCEED));

    // Check pixels outside block are 0xAA, inside block are 0x55
    EXPECT_THAT(read_custom[0], Eq(0xAA));             // (0, 0)
    EXPECT_THAT(read_custom[2 * kDim + 2], Eq(0x55));  // (2, 2)
    EXPECT_THAT(read_custom[5 * kDim + 5], Eq(0x55));  // (5, 5)
    EXPECT_THAT(read_custom[6 * kDim + 6], Eq(0xAA));  // (6, 6)

    EXPECT_THAT(GRendaccess(ri_custom), Eq(SUCCEED));

    const int32_t ri_stride = GRselect(gr_id, 1);
    ASSERT_THAT(ri_stride, Ne(FAIL));

    std::vector<uint8_t> read_stride(kTotalPixels, 0);
    EXPECT_THAT(
        GRreadimage(ri_stride, start, stride, count, read_stride.data()),
        Eq(SUCCEED));
    EXPECT_THAT(read_stride[0], Eq(0));                // default fill is 0
    EXPECT_THAT(read_stride[1 * kDim + 1], Eq(0x77));  // (1, 1)
    EXPECT_THAT(read_stride[1 * kDim + 2], Eq(0));     // (2, 1) is fill
    EXPECT_THAT(read_stride[1 * kDim + 3], Eq(0x77));  // (3, 1)

    EXPECT_THAT(GRendaccess(ri_stride), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, InterlaceConversionsAndRequests) {
  const std::string path = GetTempFilePath("mfgr_interlace.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kWidth = 4;
  constexpr int32_t kHeight = 4;
  constexpr int32_t kNComps = 3;
  constexpr int32_t kTotal = kWidth * kHeight * kNComps;

  std::vector<uint8_t> raw_data(kTotal);
  std::iota(raw_data.begin(), raw_data.end(), 1);

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kWidth, kHeight};

    // 1. Line interlace image
    const int32_t ri_line = GRcreate(gr_id, "LineImg", kNComps, DFNT_UINT8,
                                     MFGR_INTERLACE_LINE, dims);
    ASSERT_THAT(ri_line, Ne(FAIL));

    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kWidth, kHeight};
    EXPECT_THAT(GRwriteimage(ri_line, start, stride, count, raw_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_line), Eq(SUCCEED));

    // 2. Component interlace image
    const int32_t ri_comp = GRcreate(gr_id, "CompImg", kNComps, DFNT_UINT8,
                                     MFGR_INTERLACE_COMPONENT, dims);
    ASSERT_THAT(ri_comp, Ne(FAIL));
    EXPECT_THAT(GRwriteimage(ri_comp, start, stride, count, raw_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_comp), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Reopen and test interlace requests (GRreqimageil)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_line = GRselect(gr_id, 0);
    ASSERT_THAT(ri_line, Ne(FAIL));

    // Request pixel interlace for reading
    EXPECT_THAT(GRreqimageil(ri_line, MFGR_INTERLACE_PIXEL), Eq(SUCCEED));
    std::vector<uint8_t> read_pix(kTotal, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kWidth, kHeight};
    EXPECT_THAT(GRreadimage(ri_line, start, stride, count, read_pix.data()),
                Eq(SUCCEED));

    // Request component interlace for reading
    EXPECT_THAT(GRreqimageil(ri_line, MFGR_INTERLACE_COMPONENT), Eq(SUCCEED));
    std::vector<uint8_t> read_comp(kTotal, 0);
    EXPECT_THAT(GRreadimage(ri_line, start, stride, count, read_comp.data()),
                Eq(SUCCEED));

    // Request line interlace
    EXPECT_THAT(GRreqimageil(ri_line, MFGR_INTERLACE_LINE), Eq(SUCCEED));
    std::vector<uint8_t> read_line(kTotal, 0);
    EXPECT_THAT(GRreadimage(ri_line, start, stride, count, read_line.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_line, ElementsAreArray(raw_data));

    EXPECT_THAT(GRendaccess(ri_line), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, AttributesOverwriteAndUpdate) {
  const std::string path = GetTempFilePath("mfgr_attrs_update.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // 1. Create file and set initial global and image attributes
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // File-level global attribute
    int32 g_val1[2] = {10, 20};
    EXPECT_THAT(GRsetattr(gr_id, "GlobalAttr", DFNT_INT32, 2, g_val1),
                Eq(SUCCEED));

    // Overwrite global attribute within the same session
    int32 g_val2[2] = {100, 200};
    EXPECT_THAT(GRsetattr(gr_id, "GlobalAttr", DFNT_INT32, 2, g_val2),
                Eq(SUCCEED));

    int32 dims[2] = {4, 4};
    const int32_t ri_id = GRcreate(gr_id, "ImgWithAttr", 1, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Image-level attribute
    char str1[] = "Hello";
    EXPECT_THAT(GRsetattr(ri_id, "Greeting", DFNT_CHAR8, 5, str1), Eq(SUCCEED));

    // Overwrite image-level attribute within the same session
    char str2[] = "World";
    EXPECT_THAT(GRsetattr(ri_id, "Greeting", DFNT_CHAR8, 5, str2), Eq(SUCCEED));

    // Write image data
    std::vector<uint8_t> dummy(16, 1);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {4, 4};
    EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, dummy.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Re-open in DFACC_WRITE mode and update attributes in existing file
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_WRITE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // Overwrite existing global attribute from existing file
    int32 g_val3[2] = {1000, 2000};
    EXPECT_THAT(GRsetattr(gr_id, "GlobalAttr", DFNT_INT32, 2, g_val3),
                Eq(SUCCEED));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Overwrite existing image attribute from existing file
    char str3[] = "Earth";
    EXPECT_THAT(GRsetattr(ri_id, "Greeting", DFNT_CHAR8, 5, str3), Eq(SUCCEED));

    // Add a new image attribute
    float fval = 3.14f;
    EXPECT_THAT(GRsetattr(ri_id, "Pi", DFNT_FLOAT32, 1, &fval), Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 3. Re-open in DFACC_READ mode and verify all updated attributes
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // Verify global attribute
    int32 g_idx = GRfindattr(gr_id, "GlobalAttr");
    EXPECT_THAT(g_idx, Ge(0));
    int32 g_read[2] = {0, 0};
    EXPECT_THAT(GRgetattr(gr_id, g_idx, g_read), Eq(SUCCEED));
    EXPECT_THAT(g_read[0], Eq(1000));
    EXPECT_THAT(g_read[1], Eq(2000));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Verify Greeting attribute
    int32 a_idx = GRfindattr(ri_id, "Greeting");
    EXPECT_THAT(a_idx, Ge(0));
    char a_read[6] = {0};
    EXPECT_THAT(GRgetattr(ri_id, a_idx, a_read), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(a_read), Eq("Earth"));

    // Verify Pi attribute
    int32 p_idx = GRfindattr(ri_id, "Pi");
    EXPECT_THAT(p_idx, Ge(0));
    float p_read = 0.0f;
    EXPECT_THAT(GRgetattr(ri_id, p_idx, &p_read), Eq(SUCCEED));
    EXPECT_THAT(p_read, FloatEq(3.14f));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, ReadingOldStyleR8ImagesAndPalettes) {
  const std::string path = GetTempFilePath("mfgr_old_r8.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kWidth = 8;
  constexpr int32_t kHeight = 8;
  constexpr int32_t kNumPixels = kWidth * kHeight;

  std::vector<uint8_t> pixels(kNumPixels);
  std::iota(pixels.begin(), pixels.end(), 1);

  std::vector<uint8_t> palette(768);
  std::iota(palette.begin(), palette.end(), 0);

  // 1. Write old style images using DFR8 API
  EXPECT_THAT(DFR8setpalette(palette.data()), Eq(SUCCEED));
  EXPECT_THAT(
      DFR8addimage(path.c_str(), pixels.data(), kWidth, kHeight, COMP_NONE),
      Eq(SUCCEED));
  EXPECT_THAT(
      DFR8addimage(path.c_str(), pixels.data(), kWidth, kHeight, DFTAG_RLE),
      Eq(SUCCEED));
  DFR8restart();

  // 2. Open via GRstart to exercise old-style image scanning in
  // GRIget_image_list
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 n_images = 0, n_attrs = 0;
    EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_attrs), Eq(SUCCEED));
    EXPECT_THAT(n_images, Eq(2));

    // First image (uncompressed with palette)
    const int32_t ri_0 = GRselect(gr_id, 0);
    ASSERT_THAT(ri_0, Ne(FAIL));

    char name[H4_MAX_GR_NAME + 1] = {0};
    int32 ncomp = 0, nt = 0, il = -1;
    int32 dims[2] = {0, 0};
    int32 nattrs = -1;
    EXPECT_THAT(GRgetiminfo(ri_0, name, &ncomp, &nt, &il, dims, &nattrs),
                Eq(SUCCEED));
    EXPECT_THAT(dims[0], Eq(kWidth));
    EXPECT_THAT(dims[1], Eq(kHeight));
    EXPECT_THAT(ncomp, Eq(1));
    EXPECT_THAT(nt, Eq(DFNT_UCHAR8));

    // Check palette on old image
    EXPECT_THAT(GRgetnluts(ri_0), Eq(1));
    const int32_t lut_id = GRgetlutid(ri_0, 0);
    EXPECT_THAT(lut_id, Ne(FAIL));
    EXPECT_THAT(GRluttoref(lut_id), Gt(0));

    int32 lut_ncomp = 0, lut_nt = 0, lut_il = -1, lut_nentries = 0;
    EXPECT_THAT(GRgetlutinfo(ri_0, &lut_ncomp, &lut_nt, &lut_il, &lut_nentries),
                Eq(SUCCEED));
    EXPECT_THAT(lut_ncomp, Eq(3));
    EXPECT_THAT(lut_nt, Eq(DFNT_UINT8));
    EXPECT_THAT(lut_nentries, Eq(256));

    std::vector<uint8_t> read_pal(768, 0);
    EXPECT_THAT(GRreadlut(ri_0, read_pal.data()), Eq(SUCCEED));
    EXPECT_THAT(read_pal, ElementsAreArray(palette));

    // Read image pixels
    std::vector<uint8_t> read_img(kNumPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    EXPECT_THAT(GRreadimage(ri_0, start, stride, dims, read_img.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_img, ElementsAreArray(pixels));

    // Check GR2bmapped
    int tobe_mapped = 0, name_gen = 0;
    EXPECT_THAT(GR2bmapped(ri_0, &tobe_mapped, &name_gen), Eq(SUCCEED));
    EXPECT_THAT(tobe_mapped, Eq(1));

    EXPECT_THAT(GRendaccess(ri_0), Eq(SUCCEED));

    // Second image (RLE compressed)
    const int32_t ri_1 = GRselect(gr_id, 1);
    ASSERT_THAT(ri_1, Ne(FAIL));

    EXPECT_THAT(GRreadimage(ri_1, start, stride, dims, read_img.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_img, ElementsAreArray(pixels));

    EXPECT_THAT(GR2bmapped(ri_1, &tobe_mapped, &name_gen), Eq(SUCCEED));
    EXPECT_THAT(tobe_mapped, Eq(1));

    EXPECT_THAT(GRendaccess(ri_1), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, AdditionalCompressionSchemesAndQueries) {
  const std::string path = GetTempFilePath("mfgr_more_comp.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  constexpr int32_t kNumPixels = kDim * kDim;
  std::vector<uint8_t> pixels(kNumPixels);
  std::iota(pixels.begin(), pixels.end(), 1);

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 dims[2] = {kDim, kDim};

    // 1. RLE Compression
    const int32_t ri_rle =
        GRcreate(gr_id, "RleImg", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_rle, Ne(FAIL));

    comp_info c_info;
    std::memset(&c_info, 0, sizeof(c_info));
    EXPECT_THAT(GRsetcompress(ri_rle, COMP_CODE_RLE, &c_info), Eq(SUCCEED));

    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    int32 count[2] = {kDim, kDim};
    EXPECT_THAT(GRwriteimage(ri_rle, start, stride, count, pixels.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_rle), Eq(SUCCEED));

    // 2. Skipping Huffman Compression
    const int32_t ri_skp =
        GRcreate(gr_id, "SkpImg", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_skp, Ne(FAIL));

    c_info.skphuff.skp_size = 1;
    EXPECT_THAT(GRsetcompress(ri_skp, COMP_CODE_SKPHUFF, &c_info), Eq(SUCCEED));
    EXPECT_THAT(GRwriteimage(ri_skp, start, stride, count, pixels.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_skp), Eq(SUCCEED));

    // 3. Grayscale JPEG (ncomps == 1)
    const int32_t ri_jpeg1 =
        GRcreate(gr_id, "Jpeg1Img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_jpeg1, Ne(FAIL));

    std::memset(&c_info, 0, sizeof(c_info));
    c_info.jpeg.quality = 75;
    c_info.jpeg.force_baseline = 1;
    EXPECT_THAT(GRsetcompress(ri_jpeg1, COMP_CODE_JPEG, &c_info), Eq(SUCCEED));
    EXPECT_THAT(GRwriteimage(ri_jpeg1, start, stride, count, pixels.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_jpeg1), Eq(SUCCEED));

    // 4. Color JPEG (ncomps == 3)
    const int32_t ri_jpeg3 =
        GRcreate(gr_id, "Jpeg3Img", 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_jpeg3, Ne(FAIL));

    EXPECT_THAT(GRsetcompress(ri_jpeg3, COMP_CODE_JPEG, &c_info), Eq(SUCCEED));
    std::vector<uint8_t> rgb_pixels(kNumPixels * 3, 128);
    EXPECT_THAT(GRwriteimage(ri_jpeg3, start, stride, count, rgb_pixels.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_jpeg3), Eq(SUCCEED));

    // 5. Serial access type
    const int32_t ri_buf =
        GRcreate(gr_id, "BufImg", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_buf, Ne(FAIL));
    EXPECT_THAT(GRsetaccesstype(ri_buf, DFACC_SERIAL), Eq(SUCCEED));
    EXPECT_THAT(GRsetaccesstype(ri_buf, DFACC_DEFAULT), Eq(SUCCEED));
    EXPECT_THAT(GRsetaccesstype(ri_buf, 999), Eq(FAIL));
    EXPECT_THAT(GRwriteimage(ri_buf, start, stride, count, pixels.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_buf), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open and verify compression query functions
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // Verify RLE
    const int32_t ri_rle = GRselect(gr_id, 0);
    ASSERT_THAT(ri_rle, Ne(FAIL));
    comp_coder_t c_type = COMP_CODE_NONE;
    comp_info c_info;
    EXPECT_THAT(GRgetcomptype(ri_rle, &c_type), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_RLE));
    EXPECT_THAT(GRgetcompinfo(ri_rle, &c_type, &c_info), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_RLE));
    EXPECT_THAT(GRendaccess(ri_rle), Eq(SUCCEED));

    // Verify SKPHUFF
    const int32_t ri_skp = GRselect(gr_id, 1);
    ASSERT_THAT(ri_skp, Ne(FAIL));
    EXPECT_THAT(GRgetcomptype(ri_skp, &c_type), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_SKPHUFF));
    EXPECT_THAT(GRendaccess(ri_skp), Eq(SUCCEED));

    // Verify JPEG1
    const int32_t ri_jpeg1 = GRselect(gr_id, 2);
    ASSERT_THAT(ri_jpeg1, Ne(FAIL));
    EXPECT_THAT(GRgetcomptype(ri_jpeg1, &c_type), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_JPEG));
    EXPECT_THAT(GRgetcompinfo(ri_jpeg1, &c_type, &c_info), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_JPEG));

    // Invalid arguments for GRgetcompinfo
    EXPECT_THAT(GRgetcompinfo(FAIL, &c_type, &c_info), Eq(FAIL));
    EXPECT_THAT(GRgetcompinfo(ri_jpeg1, nullptr, &c_info), Eq(FAIL));
    EXPECT_THAT(GRgetcompinfo(ri_jpeg1, &c_type, nullptr), Eq(FAIL));

    // Read JPEG1 image data
    int32 r_start[2] = {0, 0};
    int32 r_count[2] = {kDim, kDim};
    std::vector<uint8_t> jpeg1_data(kNumPixels, 0);
    EXPECT_THAT(
        GRreadimage(ri_jpeg1, r_start, nullptr, r_count, jpeg1_data.data()),
        Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_jpeg1), Eq(SUCCEED));

    // Verify JPEG3
    const int32_t ri_jpeg3 = GRselect(gr_id, 3);
    ASSERT_THAT(ri_jpeg3, Ne(FAIL));
    EXPECT_THAT(GRgetcomptype(ri_jpeg3, &c_type), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_JPEG));
    EXPECT_THAT(GRgetcompinfo(ri_jpeg3, &c_type, &c_info), Eq(SUCCEED));
    EXPECT_THAT(c_type, Eq(COMP_CODE_JPEG));

    // Read JPEG3 image data
    std::vector<uint8_t> jpeg3_data(kNumPixels * 3, 0);
    EXPECT_THAT(
        GRreadimage(ri_jpeg3, r_start, nullptr, r_count, jpeg3_data.data()),
        Eq(SUCCEED));
    EXPECT_THAT(GRendaccess(ri_jpeg3), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, EldestStyleRasterAndRigReading) {
  const std::string path1 = GetTempFilePath("mfgr_eldest.hdf");
  const std::string path2 = GetTempFilePath("mfgr_rig_direct.hdf");
  absl::Cleanup remove_temp = [&path1, &path2] {
    std::filesystem::remove(path1);
    std::filesystem::remove(path2);
  };

  constexpr int32_t kDim = 8;
  constexpr int32_t kNumPixels = kDim * kDim;

  // 1. Synthesize Eldest-style raster file (no RIG group, just DFTAG_ID8,
  // DFTAG_IP8, DFTAG_RI8)
  {
    const int32_t fid = Hopen(path1.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    // DFTAG_ID8: dimensions 8x8 (two uint16s, big-endian)
    uint8_t id8_data[4] = {0, kDim, 0, kDim};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID8, 1, id8_data, 4), Gt(0));

    // DFTAG_IP8: palette (768 bytes)
    std::vector<uint8_t> pal(768, 55);
    EXPECT_THAT(Hputelement(fid, DFTAG_IP8, 1, pal.data(), 768), Gt(0));

    // DFTAG_RI8: image pixels (64 bytes)
    std::vector<uint8_t> pix(kNumPixels, 42);
    EXPECT_THAT(Hputelement(fid, DFTAG_RI8, 1, pix.data(), kNumPixels), Gt(0));

    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open with GRstart to trigger case DFTAG_NULL in GRIget_image_list
  {
    const int32_t fid = Hopen(path1.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    char name[H4_MAX_GR_NAME + 1] = {0};
    int32 ncomp = 0, nt = 0, il = -1;
    int32 dims[2] = {0, 0};
    int32 nattrs = -1;
    EXPECT_THAT(GRgetiminfo(ri_id, name, &ncomp, &nt, &il, dims, &nattrs),
                Eq(SUCCEED));
    EXPECT_THAT(dims[0], Eq(kDim));
    EXPECT_THAT(dims[1], Eq(kDim));

    // GRidtoref on eldest style returns img_ref, but GRreftoindex only checks
    // ri_ref or rig_ref, so GRreftoindex returns FAIL.
    const uint16 ref = GRidtoref(ri_id);
    EXPECT_THAT(ref, Gt(0));
    EXPECT_THAT(GRreftoindex(gr_id, ref), Eq(FAIL));

    // Read palette and image
    std::vector<uint8_t> read_pal(768, 0);
    EXPECT_THAT(GRreadlut(ri_id, read_pal.data()), Eq(SUCCEED));
    EXPECT_THAT(read_pal[0], Eq(55));

    std::vector<uint8_t> read_pix(kNumPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    EXPECT_THAT(GRreadimage(ri_id, start, stride, dims, read_pix.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_pix[0], Eq(42));

    int tobe_mapped = 0, name_gen = 0;
    EXPECT_THAT(GR2bmapped(ri_id, &tobe_mapped, &name_gen), Eq(SUCCEED));
    EXPECT_THAT(tobe_mapped, Eq(1));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Synthesize DFTAG_RIG file using DFdiwrite
  {
    const int32_t fid = Hopen(path2.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    // DFTAG_ID: 20 bytes
    // xdim=8 (4B), ydim=8 (4B), nt_tag=DFTAG_NT (2B), nt_ref=1 (2B),
    // ncomps=1 (2B), il=0 (2B), comp_tag=0 (2B), comp_ref=0 (2B)
    uint8_t id_buf[20] = {0, 0, 0, kDim, 0, 0, 0, kDim, 0, 106,
                          0, 1, 0, 1,    0, 0, 0, 0,    0, 0};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID, 1, id_buf, 20), Gt(0));

    // DFTAG_NT: 4 bytes {version=1, type=DFNT_UINT8, width=8,
    // class=DFNTF_HDFDEFAULT}
    uint8_t nt_buf[4] = {1, DFNT_UINT8, 8, DFNTF_HDFDEFAULT};
    EXPECT_THAT(Hputelement(fid, DFTAG_NT, 1, nt_buf, 4), Gt(0));

    // DFTAG_RI: 64 bytes
    std::vector<uint8_t> pix(kNumPixels, 99);
    EXPECT_THAT(Hputelement(fid, DFTAG_RI, 1, pix.data(), kNumPixels), Gt(0));

    // Write DFTAG_RIG group
    const int32_t gid = DFdisetup(3);
    ASSERT_THAT(gid, Ne(FAIL));
    EXPECT_THAT(DFdiput(gid, DFTAG_ID, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_NT, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_RI, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiwrite(fid, gid, DFTAG_RIG, 1), Gt(0));

    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open with GRstart to trigger case DFTAG_RIG in GRIget_image_list
  // Open in DFACC_WRITE and modify meta so GRIupdateRI covers lines 1800-1807!
  {
    const int32_t fid = Hopen(path2.c_str(), DFACC_WRITE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    char name[H4_MAX_GR_NAME + 1] = {0};
    int32 ncomp = 0, nt = 0, il = -1;
    int32 dims[2] = {0, 0};
    int32 nattrs = -1;
    EXPECT_THAT(GRgetiminfo(ri_id, name, &ncomp, &nt, &il, dims, &nattrs),
                Eq(SUCCEED));
    EXPECT_THAT(dims[0], Eq(kDim));
    EXPECT_THAT(dims[1], Eq(kDim));
    EXPECT_THAT(ncomp, Eq(1));
    EXPECT_THAT(nt, Eq(DFNT_UINT8));

    // GRidtoref on RIG style returns rig_ref
    EXPECT_THAT(GRidtoref(ri_id), Eq(1));

    std::vector<uint8_t> read_pix(kNumPixels, 0);
    int32 start[2] = {0, 0};
    int32 stride[2] = {1, 1};
    EXPECT_THAT(GRreadimage(ri_id, start, stride, dims, read_pix.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_pix[0], Eq(99));

    // Modify meta by writing an attribute to trigger GRIupdateRI and
    // GRIupdateRIG
    int32_t attr_val = 77;
    EXPECT_THAT(GRsetattr(ri_id, "rig_attr", DFNT_INT32, 1, &attr_val),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, UnwrittenImageMetaAndRigCreation) {
  const std::string path = GetTempFilePath("mfgr_unwritten.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  int32 dims[2] = {4, 4};

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // Image 1: UINT8 (will generate RIG on GRend)
    const int32_t ri_u8 =
        GRcreate(gr_id, "EmptyU8", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_u8, Ne(FAIL));
    EXPECT_THAT(GRendaccess(ri_u8), Eq(SUCCEED));

    // Image 2: INT32 (will skip RIG on GRend)
    const int32_t ri_i32 =
        GRcreate(gr_id, "EmptyI32", 1, DFNT_INT32, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_i32, Ne(FAIL));
    EXPECT_THAT(GRendaccess(ri_i32), Eq(SUCCEED));

    // GRend will execute GRIupdateRIG and GRIupdateRI with unwritten data
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open and verify images exist
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 n_images = 0, n_attrs = 0;
    EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_attrs), Eq(SUCCEED));
    EXPECT_THAT(n_images, Eq(2));

    const int32_t ri_0 = GRselect(gr_id, 0);
    ASSERT_THAT(ri_0, Ne(FAIL));
    EXPECT_THAT(GRendaccess(ri_0), Eq(SUCCEED));

    const int32_t ri_1 = GRselect(gr_id, 1);
    ASSERT_THAT(ri_1, Ne(FAIL));
    EXPECT_THAT(GRendaccess(ri_1), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, AdditionalLutAndAttrQueries) {
  const std::string path = GetTempFilePath("mfgr_queries.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  const int32_t gr_id = GRstart(fid);
  ASSERT_THAT(gr_id, Ne(FAIL));

  int32 dims[2] = {4, 4};
  const int32_t ri_id =
      GRcreate(gr_id, "QueryImg", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
  ASSERT_THAT(ri_id, Ne(FAIL));

  // 1. GRgetlutinfo on an image with no palette
  int32 ncomp = -1, nt = -1, il = -1, nentries = -1;
  EXPECT_THAT(GRgetlutinfo(ri_id, &ncomp, &nt, &il, &nentries), Eq(SUCCEED));
  EXPECT_THAT(ncomp, Eq(0));
  EXPECT_THAT(nt, Eq(DFNT_NONE));
  EXPECT_THAT(il, Eq(-1));
  EXPECT_THAT(nentries, Eq(0));

  // Null arguments to GRgetlutinfo
  EXPECT_THAT(GRgetlutinfo(ri_id, nullptr, nullptr, nullptr, nullptr),
              Eq(SUCCEED));

  // 2. Set an attribute and query with null arguments
  int32 attr_val = 123;
  EXPECT_THAT(GRsetattr(ri_id, "TestAttr", DFNT_INT32, 1, &attr_val),
              Eq(SUCCEED));
  EXPECT_THAT(GRsetattr(gr_id, "FileAttr", DFNT_INT32, 1, &attr_val),
              Eq(SUCCEED));

  EXPECT_THAT(GRattrinfo(ri_id, 0, nullptr, nullptr, nullptr), Eq(SUCCEED));
  EXPECT_THAT(GRattrinfo(gr_id, 0, nullptr, nullptr, nullptr), Eq(SUCCEED));

  // 3. GRgetiminfo with null arguments
  EXPECT_THAT(
      GRgetiminfo(ri_id, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
      Eq(SUCCEED));

  // Write dummy data so close succeeds cleanly
  uint8_t dummy[16] = {0};
  int32 start[2] = {0, 0};
  int32 stride[2] = {1, 1};
  EXPECT_THAT(GRwriteimage(ri_id, start, stride, dims, dummy), Eq(SUCCEED));

  EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
  EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(MfgrTest, ErrorAndInvalidArgumentsBranches) {
  const std::string path = GetTempFilePath("mfgr_errors.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t gr_id = GRstart(fid);
  ASSERT_THAT(gr_id, Ne(FAIL));
  absl::Cleanup close_gr = [gr_id] { EXPECT_THAT(GRend(gr_id), Eq(SUCCEED)); };

  // Invalid parameters for GRcreate
  int32 dims[2] = {8, 8};
  EXPECT_THAT(GRcreate(FAIL, "Img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims),
              Eq(FAIL));
  EXPECT_THAT(
      GRcreate(gr_id, nullptr, 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims),
      Eq(FAIL));
  EXPECT_THAT(GRcreate(gr_id, "Img", 0, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims),
              Eq(FAIL));
  EXPECT_THAT(GRcreate(gr_id, "Img", 1, DFNT_UINT8, -1, dims), Eq(FAIL));
  int32 bad_dims[2] = {0, 8};
  EXPECT_THAT(
      GRcreate(gr_id, "Img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, bad_dims),
      Eq(FAIL));

  // Valid image for testing error handling on image handles
  const int32_t ri_id =
      GRcreate(gr_id, "ValidImg", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
  ASSERT_THAT(ri_id, Ne(FAIL));
  absl::Cleanup close_ri = [ri_id] {
    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
  };

  // Write valid data once so the image is fully initialized on disk
  int32 start[2] = {0, 0};
  int32 stride[2] = {1, 1};
  int32 count[2] = {8, 8};
  uint8_t dummy[64] = {0};
  EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, dummy), Eq(SUCCEED));

  // Invalid parameters for GRwriteimage / GRreadimage
  EXPECT_THAT(GRwriteimage(FAIL, start, stride, count, dummy), Eq(FAIL));
  EXPECT_THAT(GRwriteimage(ri_id, start, stride, count, nullptr), Eq(FAIL));
  EXPECT_THAT(GRreadimage(FAIL, start, stride, count, dummy), Eq(FAIL));
  EXPECT_THAT(GRreadimage(ri_id, start, stride, count, nullptr), Eq(FAIL));

  // Negative start / zero count
  int32 bad_start[2] = {-1, 0};
  EXPECT_THAT(GRwriteimage(ri_id, bad_start, stride, count, dummy), Eq(FAIL));
  int32 bad_count[2] = {0, 0};
  EXPECT_THAT(GRwriteimage(ri_id, start, stride, bad_count, dummy), Eq(FAIL));

  // Invalid parameters for GRsetcompress
  comp_info c_info;
  std::memset(&c_info, 0, sizeof(c_info));
  EXPECT_THAT(GRsetcompress(FAIL, COMP_CODE_DEFLATE, &c_info), Eq(FAIL));
  EXPECT_THAT(GRsetcompress(ri_id, COMP_CODE_INVALID, &c_info), Eq(FAIL));

  // Invalid parameters for GRsetattr / GRgetattr
  EXPECT_THAT(GRsetattr(FAIL, "Attr", DFNT_INT32, 1, dummy), Eq(FAIL));
  EXPECT_THAT(GRsetattr(ri_id, nullptr, DFNT_INT32, 1, dummy), Eq(FAIL));
  EXPECT_THAT(GRsetattr(ri_id, "Attr", -1, 1, dummy), Eq(FAIL));
  EXPECT_THAT(GRsetattr(ri_id, "Attr", DFNT_INT32, 0, dummy), Eq(FAIL));
  EXPECT_THAT(GRsetattr(ri_id, "Attr", DFNT_INT32, 1, nullptr), Eq(FAIL));
  EXPECT_THAT(GRgetattr(FAIL, 0, dummy), Eq(FAIL));
  EXPECT_THAT(GRgetattr(ri_id, -1, dummy), Eq(FAIL));
  EXPECT_THAT(GRgetattr(ri_id, 9999, dummy), Eq(FAIL));
  EXPECT_THAT(GRgetattr(ri_id, 0, nullptr), Eq(FAIL));

  // Invalid parameters for GRwritelut / GRreadlut
  EXPECT_THAT(GRwritelut(FAIL, 3, DFNT_UINT8, 0, 256, dummy), Eq(FAIL));
  EXPECT_THAT(GRwritelut(ri_id, 0, DFNT_UINT8, 0, 256, dummy), Eq(FAIL));
  EXPECT_THAT(GRwritelut(ri_id, 3, DFNT_UINT8, 0, 256, nullptr), Eq(FAIL));
  EXPECT_THAT(GRreadlut(FAIL, dummy), Eq(FAIL));
  EXPECT_THAT(GRreadlut(ri_id, nullptr), Eq(FAIL));

  // Invalid parameters for GRsetchunk / GRgetchunkinfo
  HDF_CHUNK_DEF chunk_def;
  std::memset(&chunk_def, 0, sizeof(chunk_def));
  EXPECT_THAT(GRsetchunk(FAIL, chunk_def, HDF_CHUNK), Eq(FAIL));
  int32 flags = 0;
  EXPECT_THAT(GRgetchunkinfo(FAIL, &chunk_def, &flags), Eq(FAIL));
  EXPECT_THAT(GRgetchunkinfo(ri_id, &chunk_def, &flags), Eq(SUCCEED));
  EXPECT_THAT(flags, Eq(HDF_NONE));

  // Invalid parameters for GRsetexternalfile
  EXPECT_THAT(GRsetexternalfile(FAIL, nullptr, 0), Eq(FAIL));

  // Invalid parameters for GRsetaccesstype
  EXPECT_THAT(GRsetaccesstype(FAIL, DFACC_READ), Eq(FAIL));

  // GR2bmapped
  int tobe_mapped = 0;
  int name_generated = 0;
  EXPECT_THAT(GR2bmapped(FAIL, &tobe_mapped, &name_generated), Eq(FAIL));
  EXPECT_THAT(GR2bmapped(ri_id, &tobe_mapped, &name_generated), Eq(SUCCEED));
  EXPECT_THAT(tobe_mapped, Eq(1));
}

TEST(MfgrTest, PartialWritesWithoutFillAndSubsampling) {
  const std::string path = GetTempFilePath("mfgr_partial_nofill.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  int32_t dims[2] = {kDim, kDim};

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRcreate(gr_id, "nofill_img", 1, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // First write the whole image so Hlength > 0 and subsequent partial writes
    // have new_image == FALSE (meaning fill_image == FALSE).
    std::vector<uint8_t> init_buf(kDim * kDim, 10);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRwriteimage(ri_id, origin, nullptr, dims, init_buf.data()),
                Eq(SUCCEED));

    // 1. Write partial image as solid block (stride == nullptr)
    // start={2, 2}, count={3, 3}
    // Exercises lines 2798-2807 (solid_block == TRUE, fill_image == FALSE)
    std::vector<uint8_t> block(9, 77);
    int32_t start[2] = {2, 2};
    int32_t count[2] = {3, 3};
    EXPECT_THAT(GRwriteimage(ri_id, start, nullptr, count, block.data()),
                Eq(SUCCEED));

    // 2. Write partial image with subsampling (stride={2, 2})
    // start={0, 0}, count={2, 2}, stride={2, 2}
    // Exercises lines 2874-2895 (solid_block == FALSE, fill_image == FALSE)
    std::vector<uint8_t> subsamp(4, 99);
    int32_t start2[2] = {0, 0};
    int32_t count2[2] = {2, 2};
    int32_t stride2[2] = {2, 2};
    EXPECT_THAT(GRwriteimage(ri_id, start2, stride2, count2, subsamp.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Read back and verify reading partial / subsampled data
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Read full image with stride == nullptr
    // Exercises line 2981 (in_stride == NULL in GRreadimage)
    std::vector<uint8_t> full(kDim * kDim, 0);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRreadimage(ri_id, origin, nullptr, dims, full.data()),
                Eq(SUCCEED));

    // Read partial solid block (start={2, 2}, count={3, 3}, stride={1, 1})
    // Exercises lines 3118-3127 (solid_block == TRUE in GRreadimage)
    std::vector<uint8_t> read_block(9, 0);
    int32_t start[2] = {2, 2};
    int32_t count[2] = {3, 3};
    int32_t stride[2] = {1, 1};
    EXPECT_THAT(GRreadimage(ri_id, start, stride, count, read_block.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_block[0], Eq(99));  // (2, 2) was overwritten by subsamp
    EXPECT_THAT(read_block[1], Eq(77));  // (2, 3) retains solid block value

    // Read partial subsampled (start={0, 0}, count={2, 2}, stride={2, 2})
    // Exercises lines 3134-3150 (solid_block == FALSE in GRreadimage)
    std::vector<uint8_t> read_subsamp(4, 0);
    int32_t start2[2] = {0, 0};
    int32_t count2[2] = {2, 2};
    int32_t stride2[2] = {2, 2};
    EXPECT_THAT(
        GRreadimage(ri_id, start2, stride2, count2, read_subsamp.data()),
        Eq(SUCCEED));
    EXPECT_THAT(read_subsamp[0], Eq(99));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, UnwrittenImageReadFillAndChunkingWithAttributeFill) {
  const std::string path = GetTempFilePath("mfgr_unwritten_read.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  int32_t dims[2] = {kDim, kDim};

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  const int32_t gr_id = GRstart(fid);
  ASSERT_THAT(gr_id, Ne(FAIL));

  const int32_t ri_id = GRcreate(gr_id, "unwritten_img", 1, DFNT_UINT8,
                                 MFGR_INTERLACE_PIXEL, dims);
  ASSERT_THAT(ri_id, Ne(FAIL));

  // 1. Read unwritten image before fill value attribute is set -> fills with 0
  // (lines 3073-3078)
  std::vector<uint8_t> buf(kDim * kDim, 0xFF);
  int32_t origin[2] = {0, 0};
  EXPECT_THAT(GRreadimage(ri_id, origin, nullptr, dims, buf.data()),
              Eq(SUCCEED));
  EXPECT_THAT(buf[0], Eq(0));

  // 2. Set fill value using GRsetattr with FILL_ATTR name (lines 3069-3072)
  uint8_t fill_val = 123;
  EXPECT_THAT(GRsetattr(ri_id, FILL_ATTR, DFNT_UINT8, 1, &fill_val),
              Eq(SUCCEED));

  // Read unwritten image now -> finds FILL_ATTR and fills with 123
  EXPECT_THAT(GRreadimage(ri_id, origin, nullptr, dims, buf.data()),
              Eq(SUCCEED));
  EXPECT_THAT(buf[0], Eq(123));

  // 3. Call GRsetchunk on an image that has FILL_ATTR attribute (exercises
  // lines 5221-5229)
  HDF_CHUNK_DEF cdef;
  std::memset(&cdef, 0, sizeof(cdef));
  cdef.chunk_lengths[0] = 4;
  cdef.chunk_lengths[1] = 4;
  EXPECT_THAT(GRsetchunk(ri_id, cdef, HDF_CHUNK), Eq(SUCCEED));

  // 4. Test closing AID when img_aid != 0 in GRsetexternalfile (lines
  // 3850-3852) Calling GRwritechunk initializes img_aid
  std::vector<uint8_t> chunk_buf(16, 42);
  int32_t corigin[2] = {0, 0};
  EXPECT_THAT(GRwritechunk(ri_id, corigin, chunk_buf.data()), Eq(SUCCEED));

  EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
  EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(MfgrTest, LargeAttributesAndAttributeReopenUpdate) {
  const std::string path = GetTempFilePath("mfgr_large_attrs.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 4;
  int32_t dims[2] = {kDim, kDim};
  constexpr int32_t kLargeAttrSize = 3000;

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id =
        GRcreate(gr_id, "attr_img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Global and local large attributes (> 2048 threshold, lines 4477-4481)
    std::vector<uint8_t> big_data(kLargeAttrSize, 0xAB);
    EXPECT_THAT(GRsetattr(gr_id, "big_global", DFNT_UINT8, kLargeAttrSize,
                          big_data.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRsetattr(ri_id, "big_local", DFNT_UINT8, kLargeAttrSize,
                          big_data.data()),
                Eq(SUCCEED));

    // Small local attribute to update later
    int32_t small_val = 100;
    EXPECT_THAT(GRsetattr(ri_id, "updatable_attr", DFNT_INT32, 1, &small_val),
                Eq(SUCCEED));

    // Read back large attribute while file is still open (lines 4686-4700)
    std::vector<uint8_t> read_big(kLargeAttrSize, 0);
    const int32_t g_idx = GRfindattr(gr_id, "big_global");
    ASSERT_THAT(g_idx, Ne(FAIL));
    EXPECT_THAT(GRgetattr(gr_id, g_idx, read_big.data()), Eq(SUCCEED));
    EXPECT_THAT(read_big[0], Eq(0xAB));

    // Overwrite large attribute with a larger size (lines 4412-4435)
    std::vector<uint8_t> bigger_data(kLargeAttrSize + 500, 0xCD);
    EXPECT_THAT(GRsetattr(gr_id, "big_global", DFNT_UINT8, kLargeAttrSize + 500,
                          bigger_data.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open in DFACC_WRITE and update an existing cached attribute
  // Exercises lines 1900-1916 (GRIup_attr_data for existing attribute)
  // and lines 2040-2044 / 2092-2096.
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_WRITE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Update existing small attribute
    int32_t new_small_val = 200;
    EXPECT_THAT(
        GRsetattr(ri_id, "updatable_attr", DFNT_INT32, 1, &new_small_val),
        Eq(SUCCEED));

    // Update existing large global attribute
    std::vector<uint8_t> new_big(kLargeAttrSize + 600, 0xEF);
    EXPECT_THAT(GRsetattr(gr_id, "big_global", DFNT_UINT8, kLargeAttrSize + 600,
                          new_big.data()),
                Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, ChunkInterlaceConversionAndPaletteWriting) {
  const std::string path = GetTempFilePath("mfgr_chunk_il_pal.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  int32_t dims[2] = {kDim, kDim};

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // 3 components, line interlaced
    const int32_t ri_id = GRcreate(gr_id, "chunk_line_img", 3, DFNT_UINT8,
                                   MFGR_INTERLACE_LINE, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Write palette to this image so GRIupdateRI covers lines 1844-1853!
    std::vector<uint8_t> pal(768, 88);
    EXPECT_THAT(
        GRwritelut(ri_id, 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256, pal.data()),
        Eq(SUCCEED));

    // Set chunking
    HDF_CHUNK_DEF cdef;
    std::memset(&cdef, 0, sizeof(cdef));
    cdef.chunk_lengths[0] = 4;
    cdef.chunk_lengths[1] = 4;
    EXPECT_THAT(GRsetchunk(ri_id, cdef, HDF_CHUNK), Eq(SUCCEED));

    // Add a local attribute so DFTAG_VH is in the RI Vgroup
    int32_t val = 42;
    EXPECT_THAT(GRsetattr(ri_id, "chunk_attr", DFNT_INT32, 1, &val),
                Eq(SUCCEED));

    // Write chunk with line interlace
    // Exercises lines 5507-5539 (interlace conversion during GRwritechunk)
    std::vector<uint8_t> chunk_data(4 * 4 * 3, 11);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRwritechunk(ri_id, origin, chunk_data.data()), Eq(SUCCEED));

    // Request non-pixel interlace before readchunk to test lines 5740-5755!
    EXPECT_THAT(GRreqimageil(ri_id, MFGR_INTERLACE_LINE), Eq(SUCCEED));

    // Read chunk back
    // Exercises lines 5740-5755 (interlace conversion during GRreadchunk)
    std::vector<uint8_t> read_chunk(4 * 4 * 3, 0);
    EXPECT_THAT(GRreadchunk(ri_id, origin, read_chunk.data()), Eq(SUCCEED));
    EXPECT_THAT(read_chunk[0], Eq(11));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Re-open to trigger GRIget_image_list parsing DFTAG_LD, DFTAG_LUT, and
  // DFTAG_VH in the RI Vgroup (lines 886-930 and 970-1025).
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    std::vector<uint8_t> pal(768, 0);
    EXPECT_THAT(GRreadlut(ri_id, pal.data()), Eq(SUCCEED));
    EXPECT_THAT(pal[0], Eq(88));

    int32_t read_val = 0;
    const int32_t attr_idx = GRfindattr(ri_id, "chunk_attr");
    ASSERT_THAT(attr_idx, Ne(FAIL));
    EXPECT_THAT(GRgetattr(ri_id, attr_idx, &read_val), Eq(SUCCEED));
    EXPECT_THAT(read_val, Eq(42));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, FullRigReadingWithPaletteDimensionsAndShutdown) {
  const std::string path = GetTempFilePath("mfgr_rig_full.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  constexpr int32_t kNumPixels = kDim * kDim;

  // Synthesize RIG file with DFTAG_ID, DFTAG_NT, DFTAG_RI, DFTAG_LD, DFTAG_LUT
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    // DFTAG_ID: 20 bytes (xdim=8, ydim=8, nt_tag=DFTAG_NT, nt_ref=1, ncomps=1,
    // il=0)
    uint8_t id_buf[20] = {0, 0, 0, kDim, 0, 0, 0, kDim, 0, 106,
                          0, 1, 0, 1,    0, 0, 0, 0,    0, 0};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID, 1, id_buf, 20), Gt(0));

    // DFTAG_LD: 20 bytes (xdim=256, ydim=1, nt_tag=DFTAG_NT, nt_ref=1,
    // ncomps=3, il=0)
    uint8_t ld_buf[20] = {0, 0,   1, 0,  // xdim = 256
                          0, 0,   0, 1,  // ydim = 1
                          0, 106, 0, 1,  // nt_tag = DFTAG_NT (106), nt_ref = 1
                          0, 3,   0, 0,  // ncomps = 3, il = 0
                          0, 0,   0, 0};
    EXPECT_THAT(Hputelement(fid, DFTAG_LD, 1, ld_buf, 20), Gt(0));

    // DFTAG_NT: 4 bytes with subclass DFNTF_PC to trigger DFNT_LITEND (lines
    // 1137-1140 & 1178-1180)
    uint8_t nt_buf[4] = {1, DFNT_UINT8, 8, DFNTF_PC};
    EXPECT_THAT(Hputelement(fid, DFTAG_NT, 1, nt_buf, 4), Gt(0));

    // DFTAG_RI: pixels
    std::vector<uint8_t> pix(kNumPixels, 77);
    EXPECT_THAT(Hputelement(fid, DFTAG_RI, 1, pix.data(), kNumPixels), Gt(0));

    // DFTAG_LUT: palette data (768 bytes)
    std::vector<uint8_t> pal(768, 33);
    EXPECT_THAT(Hputelement(fid, DFTAG_LUT, 1, pal.data(), 768), Gt(0));

    // Write DFTAG_RIG group with DFTAG_ID, DFTAG_LD, DFTAG_LUT, DFTAG_RI,
    // DFTAG_NT
    const int32_t gid = DFdisetup(5);
    ASSERT_THAT(gid, Ne(FAIL));
    EXPECT_THAT(DFdiput(gid, DFTAG_ID, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_LD, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_NT, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_LUT, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_RI, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiwrite(fid, gid, DFTAG_RIG, 1), Gt(0));

    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open with GRstart to trigger case DFTAG_RIG with DFTAG_LD and DFTAG_LUT
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Verify palette read
    std::vector<uint8_t> read_pal(768, 0);
    EXPECT_THAT(GRreadlut(ri_id, read_pal.data()), Eq(SUCCEED));
    EXPECT_THAT(read_pal[0], Eq(33));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, ReopenedChunkedImageAndLutOverwrites) {
  const std::string path = GetTempFilePath("mfgr_reopen_chunk.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  int32_t dims[2] = {kDim, kDim};

  // 1. Create file with chunked image and write chunk
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRcreate(gr_id, "chunk_reopen", 1, DFNT_UINT8,
                                   MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Overwrite LUT test: write LUT twice on ri_id (lines 3682-3686)
    std::vector<uint8_t> pal1(768, 10);
    std::vector<uint8_t> pal2(768, 20);
    EXPECT_THAT(GRwritelut(ri_id, 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256,
                           pal1.data()),
                Eq(SUCCEED));
    EXPECT_THAT(GRwritelut(ri_id, 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256,
                           pal2.data()),
                Eq(SUCCEED));
    // Unsupported LUT parameters (line 3710)
    EXPECT_THAT(GRwritelut(ri_id, 4, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256,
                           pal2.data()),
                Eq(FAIL));

    HDF_CHUNK_DEF cdef;
    std::memset(&cdef, 0, sizeof(cdef));
    cdef.chunk_lengths[0] = 4;
    cdef.chunk_lengths[1] = 4;
    EXPECT_THAT(GRsetchunk(ri_id, cdef, HDF_CHUNK), Eq(SUCCEED));

    std::vector<uint8_t> cdata(16, 55);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRwritechunk(ri_id, origin, cdata.data()), Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Re-open chunked image in DFACC_WRITE mode
  // ri_ptr->img_aid is initially 0!
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_WRITE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // Calls GRIgetaid at line 5322 in GRgetchunkinfo
    HDF_CHUNK_DEF cdef_read;
    int32_t flags = 0;
    EXPECT_THAT(GRgetchunkinfo(ri_id, &cdef_read, &flags), Eq(SUCCEED));
    EXPECT_THAT(flags, Eq(HDF_CHUNK));

    // Calls HMCsetMaxcache at line 5866 in GRsetchunkcache
    EXPECT_THAT(GRsetchunkcache(ri_id, 5, 0), Gt(0));

    // Read chunk (img_aid is already opened)
    std::vector<uint8_t> read_c(16, 0);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRreadchunk(ri_id, origin, read_c.data()), Eq(SUCCEED));
    EXPECT_THAT(read_c[0], Eq(55));

    // Write chunk
    std::vector<uint8_t> new_c(16, 66);
    int32_t origin1[2] = {1, 0};
    EXPECT_THAT(GRwritechunk(ri_id, origin1, new_c.data()), Eq(SUCCEED));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 3. Re-open in DFACC_READ to test GRIgetaid in GRreadchunk when img_aid == 0
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    const int32_t ri_id = GRselect(gr_id, 0);
    ASSERT_THAT(ri_id, Ne(FAIL));

    // In DFACC_READ, img_aid is 0 so GRreadchunk calls GRIgetaid(...,
    // DFACC_WRITE) which fails because the file is read-only.
    std::vector<uint8_t> read_c(16, 0);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRreadchunk(ri_id, origin, read_c.data()), Eq(FAIL));

    EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, NumberTypeConversionInt16AndInt32) {
  const std::string path = GetTempFilePath("mfgr_nt_convert.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 4;
  int32_t dims[2] = {kDim, kDim};

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    // 1. Non-chunked INT16 with pixel interlace (exercises convert == TRUE
    // without interlace switch)
    const int32_t ri_i16 =
        GRcreate(gr_id, "int16_img", 1, DFNT_INT16, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_i16, Ne(FAIL));

    std::vector<int16_t> data16(kDim * kDim, 12345);
    int32_t start[2] = {0, 0};
    int32_t count[2] = {kDim, kDim};
    EXPECT_THAT(GRwriteimage(ri_i16, start, nullptr, count, data16.data()),
                Eq(SUCCEED));

    // Read back whole image with conversion
    std::vector<int16_t> read16(kDim * kDim, 0);
    EXPECT_THAT(GRreadimage(ri_i16, start, nullptr, count, read16.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read16[0], Eq(12345));

    // Read back partial image with conversion
    std::vector<int16_t> read16_part(4, 0);
    int32_t part_count[2] = {2, 2};
    EXPECT_THAT(
        GRreadimage(ri_i16, start, nullptr, part_count, read16_part.data()),
        Eq(SUCCEED));
    EXPECT_THAT(read16_part[0], Eq(12345));

    EXPECT_THAT(GRendaccess(ri_i16), Eq(SUCCEED));

    // 2. Chunked INT32 with pixel interlace
    const int32_t ri_i32 = GRcreate(gr_id, "int32_chunk", 1, DFNT_INT32,
                                    MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_i32, Ne(FAIL));

    HDF_CHUNK_DEF cdef;
    std::memset(&cdef, 0, sizeof(cdef));
    cdef.chunk_lengths[0] = 2;
    cdef.chunk_lengths[1] = 2;
    EXPECT_THAT(GRsetchunk(ri_i32, cdef, HDF_CHUNK), Eq(SUCCEED));

    std::vector<int32_t> cdata32(4, 987654);
    int32_t origin[2] = {0, 0};
    EXPECT_THAT(GRwritechunk(ri_i32, origin, cdata32.data()), Eq(SUCCEED));

    std::vector<int32_t> cread32(4, 0);
    EXPECT_THAT(GRreadchunk(ri_i32, origin, cread32.data()), Eq(SUCCEED));
    EXPECT_THAT(cread32[0], Eq(987654));

    EXPECT_THAT(GRendaccess(ri_i32), Eq(SUCCEED));

    // 3. Non-chunked CHAR8 with pixel interlace (exercises convert == FALSE in
    // GRwriteimage/GRreadimage)
    const int32_t ri_c8 =
        GRcreate(gr_id, "char8_img", 1, DFNT_CHAR8, MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_c8, Ne(FAIL));

    std::vector<char> datac8(kDim * kDim, 'Z');
    EXPECT_THAT(GRwriteimage(ri_c8, start, nullptr, count, datac8.data()),
                Eq(SUCCEED));

    std::vector<char> readc8(kDim * kDim, 0);
    EXPECT_THAT(GRreadimage(ri_c8, start, nullptr, count, readc8.data()),
                Eq(SUCCEED));
    EXPECT_THAT(readc8[0], Eq('Z'));

    EXPECT_THAT(GRendaccess(ri_c8), Eq(SUCCEED));

    // 4. Chunked CHAR8 with pixel interlace (exercises convert == FALSE in
    // GRwritechunk/GRreadchunk)
    const int32_t ri_chunk_c8 = GRcreate(gr_id, "char8_chunk", 1, DFNT_CHAR8,
                                         MFGR_INTERLACE_PIXEL, dims);
    ASSERT_THAT(ri_chunk_c8, Ne(FAIL));
    EXPECT_THAT(GRsetchunk(ri_chunk_c8, cdef, HDF_CHUNK), Eq(SUCCEED));

    std::vector<char> chunk_c8(4, 'W');
    EXPECT_THAT(GRwritechunk(ri_chunk_c8, origin, chunk_c8.data()),
                Eq(SUCCEED));

    std::vector<char> read_chunk_c8(4, 0);
    EXPECT_THAT(GRreadchunk(ri_chunk_c8, origin, read_chunk_c8.data()),
                Eq(SUCCEED));
    EXPECT_THAT(read_chunk_c8[0], Eq('W'));

    EXPECT_THAT(GRendaccess(ri_chunk_c8), Eq(SUCCEED));

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, NotFoundAtomObjects) {
  const std::string path = GetTempFilePath("mfgr_not_found.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  const int32_t real_gr = GRstart(fid);
  ASSERT_THAT(real_gr, Ne(FAIL));

  // Construct valid group IDs with non-existent atom index
  const int32_t fake_grid = (5 << 28) | 0x1234;
  const int32_t fake_riid = (6 << 28) | 0x1234;

  int32 n_images = 0, n_attrs = 0;
  EXPECT_THAT(GRfileinfo(fake_grid, &n_images, &n_attrs), Eq(FAIL));
  EXPECT_THAT(GRend(fake_grid), Eq(FAIL));
  EXPECT_THAT(GRselect(fake_grid, 0), Eq(FAIL));
  int32 dims[2] = {4, 4};
  EXPECT_THAT(
      GRcreate(fake_grid, "img", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims),
      Eq(FAIL));
  EXPECT_THAT(GRnametoindex(fake_grid, "img"), Eq(FAIL));
  EXPECT_THAT(GRreftoindex(fake_grid, 1), Eq(FAIL));
  int32 attr_val = 1;
  EXPECT_THAT(GRsetattr(fake_grid, "attr", DFNT_INT32, 1, &attr_val), Eq(FAIL));
  EXPECT_THAT(GRgetattr(fake_grid, 0, &attr_val), Eq(FAIL));
  EXPECT_THAT(GRfindattr(fake_grid, "attr"), Eq(FAIL));
  EXPECT_THAT(GRattrinfo(fake_grid, 0, nullptr, nullptr, nullptr), Eq(FAIL));

  // fake_riid calls
  EXPECT_THAT(GRendaccess(fake_riid), Eq(FAIL));
  char name[32] = {0};
  int32 ncomp = 0, nt = 0, il = 0, nattrs = 0;
  EXPECT_THAT(GRgetiminfo(fake_riid, name, &ncomp, &nt, &il, dims, &nattrs),
              Eq(FAIL));
  int32 start[2] = {0, 0};
  int32 count[2] = {4, 4};
  uint8_t buf[16] = {0};
  EXPECT_THAT(GRwriteimage(fake_riid, start, nullptr, count, buf), Eq(FAIL));
  EXPECT_THAT(GRreadimage(fake_riid, start, nullptr, count, buf), Eq(FAIL));
  comp_info cinfo;
  EXPECT_THAT(GRsetcompress(fake_riid, COMP_CODE_RLE, &cinfo), Eq(FAIL));
  comp_coder_t ctype;
  EXPECT_THAT(GRgetcompress(fake_riid, &ctype, &cinfo), Eq(FAIL));
  EXPECT_THAT(GRgetcompinfo(fake_riid, &ctype, &cinfo), Eq(FAIL));
  EXPECT_THAT(GRgetcomptype(fake_riid, &ctype), Eq(FAIL));
  HDF_CHUNK_DEF cdef;
  std::memset(&cdef, 0, sizeof(cdef));
  EXPECT_THAT(GRsetchunk(fake_riid, cdef, HDF_CHUNK), Eq(FAIL));
  int32 flags = 0;
  EXPECT_THAT(GRgetchunkinfo(fake_riid, &cdef, &flags), Eq(FAIL));
  int32 origin[2] = {0, 0};
  EXPECT_THAT(GRwritechunk(fake_riid, origin, buf), Eq(FAIL));
  EXPECT_THAT(GRreadchunk(fake_riid, origin, buf), Eq(FAIL));
  EXPECT_THAT(GRsetchunkcache(fake_riid, 5, 0), Eq(FAIL));
  EXPECT_THAT(GRsetexternalfile(fake_riid, "sidecar.bin", 0), Eq(FAIL));
  EXPECT_THAT(GRsetaccesstype(fake_riid, DFACC_SERIAL), Eq(FAIL));
  EXPECT_THAT(GRreqimageil(fake_riid, MFGR_INTERLACE_LINE), Eq(FAIL));
  EXPECT_THAT(GRreqlutil(fake_riid, MFGR_INTERLACE_LINE), Eq(FAIL));
  EXPECT_THAT(GRgetlutid(fake_riid, 1), Eq(FAIL));
  EXPECT_THAT(GRluttoref(fake_riid), Eq(0));
  int32 nentries = 0;
  EXPECT_THAT(GRgetlutinfo(fake_riid, &ncomp, &nt, &il, &nentries), Eq(FAIL));
  EXPECT_THAT(
      GRwritelut(fake_riid, 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256, buf),
      Eq(FAIL));
  EXPECT_THAT(GRreadlut(fake_riid, buf), Eq(FAIL));
  EXPECT_THAT(GRidtoref(fake_riid), Eq(0));
  int tobe_mapped = 0, name_gen = 0;
  EXPECT_THAT(GR2bmapped(fake_riid, &tobe_mapped, &name_gen), Eq(FAIL));
  EXPECT_THAT(GRsetattr(fake_riid, "attr", DFNT_INT32, 1, &attr_val), Eq(FAIL));
  EXPECT_THAT(GRgetattr(fake_riid, 0, &attr_val), Eq(FAIL));
  EXPECT_THAT(GRfindattr(fake_riid, "attr"), Eq(FAIL));
  EXPECT_THAT(GRattrinfo(fake_riid, 0, nullptr, nullptr, nullptr), Eq(FAIL));
  EXPECT_THAT(GRgetnluts(fake_riid), Eq(FAIL));

  EXPECT_THAT(GRend(real_gr), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(MfgrTest, VdataAndDuplicateTagChecks) {
  const std::string path = GetTempFilePath("mfgr_vdata_dups.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 8;
  constexpr int32_t kNumPixels = kDim * kDim;

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    // Initialize Vset interface so we can create Vdatas and Vgroups
    ASSERT_THAT(Vstart(fid), Ne(FAIL));

    // 1. Create a 2-field Vdata at file root (lines 668-670 in
    // GRIget_image_list)
    const int32_t vs1 = VSattach(fid, -1, "w");
    ASSERT_THAT(vs1, Ne(FAIL));
    EXPECT_THAT(VSfdefine(vs1, "f1", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vs1, "f2", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs1, "f1,f2"), Eq(SUCCEED));
    int32_t vdata_vals[2] = {10, 20};
    EXPECT_THAT(VSwrite(vs1, reinterpret_cast<const uint8_t*>(vdata_vals), 1,
                        FULL_INTERLACE),
                Gt(0));
    const int32_t vs1_ref = VSQueryref(vs1);
    EXPECT_THAT(VSdetach(vs1), Eq(SUCCEED));

    // 2. Create an Eldest-style image (DFTAG_ID8 + DFTAG_RI8, ref 2) with NO
    // palette (line 1256)
    uint8_t id8_buf[4] = {0, kDim, 0, kDim};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID8, 2, id8_buf, 4), Gt(0));
    std::vector<uint8_t> pix(kNumPixels, 11);
    EXPECT_THAT(Hputelement(fid, DFTAG_RI8, 2, pix.data(), kNumPixels), Gt(0));

    // Also write DFTAG_CI8 with ref 2 so duplicate check triggers lines 806-811
    EXPECT_THAT(Hputelement(fid, DFTAG_CI8, 2, pix.data(), kNumPixels), Gt(0));

    // 3. Create a RIG with DFTAG_CI (compressed image tag) to trigger lines
    // 1087-1093
    uint8_t id_buf[20] = {0, 0, 0, kDim, 0, 0, 0, kDim, 0, 106,
                          0, 1, 0, 1,    0, 0, 0, 0,    0, 0};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID, 3, id_buf, 20), Gt(0));
    uint8_t nt_buf[4] = {1, DFNT_UINT8, 8, DFNTF_HDFDEFAULT};
    EXPECT_THAT(Hputelement(fid, DFTAG_NT, 1, nt_buf, 4), Gt(0));
    EXPECT_THAT(Hputelement(fid, DFTAG_CI, 3, pix.data(), kNumPixels), Gt(0));

    const int32_t gid = DFdisetup(3);
    ASSERT_THAT(gid, Ne(FAIL));
    EXPECT_THAT(DFdiput(gid, DFTAG_ID, 3), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_NT, 1), Eq(SUCCEED));
    EXPECT_THAT(DFdiput(gid, DFTAG_CI, 3), Eq(SUCCEED));
    EXPECT_THAT(DFdiwrite(fid, gid, DFTAG_RIG, 3), Gt(0));

    // 4. Create an RI Vgroup with 2-field Vdata to trigger lines 986-988
    const int32_t vg = Vattach(fid, -1, "w");
    ASSERT_THAT(vg, Ne(FAIL));
    EXPECT_THAT(Vsetclass(vg, RI_NAME), Eq(SUCCEED));
    EXPECT_THAT(Vaddtagref(vg, DFTAG_ID, 3), Gt(0));
    EXPECT_THAT(Vaddtagref(vg, DFTAG_RI, 3), Gt(0));
    EXPECT_THAT(Vaddtagref(vg, DFTAG_VH, vs1_ref), Gt(0));
    EXPECT_THAT(Vdetach(vg), Eq(SUCCEED));

    Vend(fid);
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open with GRstart and verify reading images
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 n_images = 0, n_attrs = 0;
    EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_attrs), Eq(SUCCEED));
    EXPECT_THAT(n_images, Gt(0));

    for (int32_t i = 0; i < n_images; ++i) {
      const int32_t ri_id = GRselect(gr_id, i);
      if (ri_id != FAIL) {
        EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
      }
    }

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(MfgrTest, ImageDeduplicationAndUnnamedVgroups) {
  const std::string path = GetTempFilePath("mfgr_dedup_unnamed.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  constexpr int32_t kDim = 4;
  constexpr int32_t kNumPixels = kDim * kDim;

  // 1. Create file with synthesized RIG, RI8 duplicate, unnamed Vgroup, and PC
  // little-endian NT
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    // DFTAG_ID: 20 bytes
    uint8_t id_buf[20] = {0, 0,   0, kDim, 0, 0, 0, kDim,
                          0, 106, 0, 1,  // DFTAG_NT ref 1
                          0, 1,   0, 0,    0, 0, 0, 0};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID, 1, id_buf, 20), Gt(0));

    // DFTAG_NT with DFNTF_PC (little-endian subclass) to trigger lines 959-965
    uint8_t nt_buf[4] = {1, DFNT_UINT8, 8, DFNTF_PC};
    EXPECT_THAT(Hputelement(fid, DFTAG_NT, 1, nt_buf, 4), Gt(0));

    // DFTAG_RI: 16 bytes
    std::vector<uint8_t> pix(kNumPixels, 77);
    EXPECT_THAT(Hputelement(fid, DFTAG_RI, 1, pix.data(), kNumPixels), Gt(0));

    // Standalone DFTAG_RI8 ref 10 with DFTAG_ID8 ref 10
    uint8_t id8_buf[4] = {0, kDim, 0, kDim};
    EXPECT_THAT(Hputelement(fid, DFTAG_ID8, 10, id8_buf, 4), Gt(0));
    std::vector<uint8_t> ri8_pix(kNumPixels, 88);
    EXPECT_THAT(Hputelement(fid, DFTAG_RI8, 10, ri8_pix.data(), kNumPixels),
                Gt(0));

    // RIG containing DFTAG_RI8 ref 10 to cause duplicate scan (lines 806-811)
    const int32_t gid = DFdisetup(1);
    ASSERT_THAT(gid, Ne(FAIL));
    EXPECT_THAT(DFdiput(gid, DFTAG_RI8, 10), Eq(SUCCEED));
    EXPECT_THAT(DFdiwrite(fid, gid, DFTAG_RIG, 1), Gt(0));

    // Unnamed RI Vgroup (class RI_NAME without Vsetname, lines 848-852)
    EXPECT_THAT(Vstart(fid), Eq(SUCCEED));
    const int32_t vgid = Vattach(fid, -1, "w");
    ASSERT_THAT(vgid, Ne(FAIL));
    EXPECT_THAT(Vsetclass(vgid, RI_NAME), Eq(SUCCEED));
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_ID, 1), Gt(0));
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_RI, 1), Gt(0));
    EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
    EXPECT_THAT(Vend(fid), Eq(SUCCEED));

    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // 2. Open with GRstart: executes deduplication, little-endian NT, and unnamed
  // image
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t gr_id = GRstart(fid);
    ASSERT_THAT(gr_id, Ne(FAIL));

    int32 n_images = -1, n_attrs = -1;
    EXPECT_THAT(GRfileinfo(gr_id, &n_images, &n_attrs), Eq(SUCCEED));
    EXPECT_THAT(n_images, Gt(0));

    // Select the unnamed image: verify name generated as "Raster Image #..."
    for (int32 i = 0; i < n_images; ++i) {
      const int32_t ri_id = GRselect(gr_id, i);
      ASSERT_THAT(ri_id, Ne(FAIL));
      char name[H4_MAX_GR_NAME + 1] = {0};
      int32 ncomp = 0, nt = 0, il = -1;
      int32 dims[2] = {0, 0};
      int32 nattr = -1;
      EXPECT_THAT(GRgetiminfo(ri_id, name, &ncomp, &nt, &il, dims, &nattr),
                  Eq(SUCCEED));
      EXPECT_THAT(GRendaccess(ri_id), Eq(SUCCEED));
    }

    EXPECT_THAT(GRend(gr_id), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

}  // namespace
}  // namespace hdf4
