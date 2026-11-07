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

// Tests for floating point to 8-bit raster conversion (hdf/src/dfufp2i.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dfufp2i.c

#include <cstdint>
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
#include "third_party/hdf4/hdf/src/herr_priv.h"

extern "C" {
#include "third_party/hdf4/hdf/src/dfufp2i_priv.h"
}

namespace hdf4 {
namespace {

constexpr int kExpand = 1;
constexpr int kInterp = 2;

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

class Dfufp2iTest : public ::testing::Test {
 protected:
  void SetUp() override {
    DFR8restart();
    DFR8setpalette(nullptr);
    HEclear();
  }

  void TearDown() override {
    DFR8restart();
    DFR8setpalette(nullptr);
    HEclear();
  }
};

TEST_F(Dfufp2iTest, GenerateScaleDirect) {
  std::vector<float32> scale(6, 0.0f);
  EXPECT_EQ(0, generate_scale(5, scale.data()));
  for (int i = 0; i <= 5; ++i) {
    EXPECT_FLOAT_EQ(static_cast<float32>(i), scale[i]);
  }
}

TEST_F(Dfufp2iTest, ComputeOffsetsDirect) {
  std::vector<float32> scale = {0.0f, 10.0f, 20.0f, 30.0f};
  std::vector<int32> offsets(8, 0);
  EXPECT_EQ(0, compute_offsets(scale.data(), 4, offsets.data(), 8));
  EXPECT_EQ(0, offsets[0]);
  EXPECT_LE(offsets.back(), 3);
}

TEST_F(Dfufp2iTest, ExpandSimpleWithoutScales) {
  const std::string path = GetTempFilePath("dfufp2i_simple.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] {
    DFR8restart();
    std::filesystem::remove(path);
  };

  constexpr int32 kHdim = 4;
  constexpr int32 kVdim = 3;
  std::vector<float32> data = {
      10.0f, 20.0f, 30.0f, 40.0f,  50.0f,  60.0f,
      70.0f, 80.0f, 90.0f, 100.0f, 110.0f, 120.0f,
  };

  // Convert with no scales, no palette, no compression, expanding 4x3 to 8x6.
  EXPECT_EQ(0, DFUfptoimage(kHdim, kVdim, 120.0f, 10.0f, nullptr, nullptr,
                            data.data(), nullptr,
                            const_cast<char*>(path.c_str()), kExpand, 8, 6, 0));

  // Verify raster image dimensions.
  int32 xdim = 0;
  int32 ydim = 0;
  int ispal = 0;
  EXPECT_EQ(SUCCEED, DFR8getdims(path.c_str(), &xdim, &ydim, &ispal));
  EXPECT_EQ(8, xdim);
  EXPECT_EQ(6, ydim);
  EXPECT_EQ(0, ispal);

  std::vector<uint8_t> image(xdim * ydim, 0);
  EXPECT_EQ(SUCCEED,
            DFR8getimage(path.c_str(), image.data(), xdim, ydim, nullptr));
  EXPECT_GT(image[0], 0);
}

TEST_F(Dfufp2iTest, ExpandSimpleClampsResolution) {
  const std::string path = GetTempFilePath("dfufp2i_clamp.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] {
    DFR8restart();
    std::filesystem::remove(path);
  };

  constexpr int32 kHdim = 4;
  constexpr int32 kVdim = 4;
  std::vector<float32> data(kHdim * kVdim, 50.0f);

  // Request resolution smaller than input dimensions (hres=2, vres=2).
  // process() clamps hres to hdim and vres to vdim.
  EXPECT_EQ(0, DFUfptoimage(kHdim, kVdim, 100.0f, 0.0f, nullptr, nullptr,
                            data.data(), nullptr,
                            const_cast<char*>(path.c_str()), kExpand, 2, 2, 1));

  int32 xdim = 0;
  int32 ydim = 0;
  int ispal = 0;
  EXPECT_EQ(SUCCEED, DFR8getdims(path.c_str(), &xdim, &ydim, &ispal));
  EXPECT_EQ(4, xdim);
  EXPECT_EQ(4, ydim);
}

TEST_F(Dfufp2iTest, ExpandScaledWithProvidedScalesAndPalette) {
  const std::string path = GetTempFilePath("dfufp2i_scaled.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] {
    DFR8restart();
    std::filesystem::remove(path);
  };

  constexpr int32 kHdim = 3;
  constexpr int32 kVdim = 3;
  std::vector<float32> data = {
      0.0f,   50.0f,  100.0f, 25.0f,  75.0f,
      125.0f, -50.0f, 200.0f, 500.0f,  // tests out-of-range clipping to 0
  };
  std::vector<float32> hscale = {0.0f, 10.0f, 30.0f};
  std::vector<float32> vscale = {0.0f, 5.0f, 20.0f};
  std::vector<uint8_t> palette(768, 128);

  // Expanded with non-linear scales and palette, with compression.
  EXPECT_EQ(0, DFUfptoimage(kHdim, kVdim, 100.0f, 0.0f, hscale.data(),
                            vscale.data(), data.data(), palette.data(),
                            const_cast<char*>(path.c_str()), kExpand, 9, 9, 1));

  // Also test inverted min/max in scaled expansion (tests line 515 range =
  // -range).
  EXPECT_EQ(0, DFUfptoimage(kHdim, kVdim, 0.0f, 100.0f, hscale.data(),
                            vscale.data(), data.data(), nullptr,
                            const_cast<char*>(path.c_str()), kExpand, 9, 9, 0));

  int32 xdim = 0;
  int32 ydim = 0;
  int ispal = 0;
  EXPECT_EQ(SUCCEED, DFR8getdims(path.c_str(), &xdim, &ydim, &ispal));
  EXPECT_EQ(9, xdim);
  EXPECT_EQ(9, ydim);
  EXPECT_EQ(1, ispal);

  std::vector<uint8_t> read_pal(768, 0);
  std::vector<uint8_t> image(xdim * ydim, 0);
  EXPECT_EQ(SUCCEED, DFR8getimage(path.c_str(), image.data(), xdim, ydim,
                                  read_pal.data()));
  EXPECT_EQ(128, read_pal[0]);
}

TEST_F(Dfufp2iTest, InterpolateIncreasingScales) {
  const std::string path = GetTempFilePath("dfufp2i_interp_inc.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] {
    DFR8restart();
    std::filesystem::remove(path);
  };

  constexpr int32 kHdim = 3;
  constexpr int32 kVdim = 3;
  std::vector<float32> data = {
      10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f,
  };
  std::vector<float32> hscale = {0.0f, 1.0f, 2.0f};
  std::vector<float32> vscale = {0.0f, 1.0f, 2.0f};

  // Bilinear interpolation with increasing scales.
  EXPECT_EQ(0, DFUfptoimage(kHdim, kVdim, 90.0f, 10.0f, hscale.data(),
                            vscale.data(), data.data(), nullptr,
                            const_cast<char*>(path.c_str()), kInterp, 6, 6, 0));

  int32 xdim = 0;
  int32 ydim = 0;
  int ispal = 0;
  EXPECT_EQ(SUCCEED, DFR8getdims(path.c_str(), &xdim, &ydim, &ispal));
  EXPECT_EQ(6, xdim);
  EXPECT_EQ(6, ydim);
}

TEST_F(Dfufp2iTest, InterpolateDecreasingScalesAndInvertedRange) {
  const std::string path = GetTempFilePath("dfufp2i_interp_dec.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] {
    DFR8restart();
    std::filesystem::remove(path);
  };

  constexpr int32 kHdim = 3;
  constexpr int32 kVdim = 3;
  std::vector<float32> data = {
      10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f,
  };
  // Decreasing scales: xrange < 0 and yrange < 0.
  std::vector<float32> hscale = {2.0f, 1.0f, 0.0f};
  std::vector<float32> vscale = {2.0f, 1.0f, 0.0f};

  // Inverted min/max (max < min) to test range = -range.
  EXPECT_EQ(0, DFUfptoimage(kHdim, kVdim, 10.0f, 90.0f, hscale.data(),
                            vscale.data(), data.data(), nullptr,
                            const_cast<char*>(path.c_str()), kInterp, 6, 6, 0));

  int32 xdim = 0;
  int32 ydim = 0;
  int ispal = 0;
  EXPECT_EQ(SUCCEED, DFR8getdims(path.c_str(), &xdim, &ydim, &ispal));
  EXPECT_EQ(6, xdim);
  EXPECT_EQ(6, ydim);
}

TEST_F(Dfufp2iTest, ProcessErrorHandling) {
  // Test invalid output path causing DFR8addimage failure in process().
  Input in{};
  in.hdim = 2;
  in.vdim = 2;
  in.max = 10.0f;
  in.min = 0.0f;
  in.is_hscale = FALSE;
  in.is_vscale = FALSE;
  std::vector<float32> data = {1.0f, 2.0f, 3.0f, 4.0f};
  in.data = data.data();
  in.is_pal = FALSE;
  in.ct_method = kExpand;

  Output out{};
  out.hres = 2;
  out.vres = 2;
  out.compress = 0;
  out.outfile = "/nonexistent_dir_12345/test.hdf";
  out.palette = nullptr;

  EXPECT_LT(process(&in, &out), 0);
}

}  // namespace
}  // namespace hdf4
