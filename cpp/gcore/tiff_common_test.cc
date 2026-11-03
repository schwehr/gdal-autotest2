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
// Tests common TIFF helper utilities (gcore/tiff_common.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/tiff_common.cpp

#include "third_party/gdal/gcore/tiff_common.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_mdreader.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

struct ErrorFormatCase {
  const char* module;
  const char* fmt;
  const char* expected;
};

constexpr auto kErrorFormatCases = std::to_array<ErrorFormatCase>({
    {.module = "GTiff", .fmt = "msg", .expected = "GTiff:msg"},
    {.module = "mod%name%", .fmt = "err %s", .expected = "mod%%name%%:err %s"},
    {.module = "%%", .fmt = "%d %s", .expected = "%%%%:%d %s"},
    {.module = "", .fmt = "only_fmt", .expected = ":only_fmt"},
});

TEST(TiffCommonTest, PrepareTIFFErrorFormat) {
  for (const auto& tc : kErrorFormatCases) {
    std::unique_ptr<char, CplFreeDeleter> fmt(
        gdal::tiff_common::PrepareTIFFErrorFormat(tc.module, tc.fmt));
    ASSERT_NE(nullptr, fmt);
    EXPECT_STREQ(tc.expected, fmt.get());
  }
}

struct ColorMapCase {
  std::span<const unsigned short> r;
  std::span<const unsigned short> g;
  std::span<const unsigned short> b;
  int initial_mult;
  int default_mult;
  bool nodata_set;
  double nodata_val;
  int expected_mult;
  int check_idx;
  GDALColorEntry expected_entry;
  bool check_nodata_alpha;
};

constexpr std::array<unsigned short, 2> kSmallR = {0, 100};
constexpr std::array<unsigned short, 2> kSmallG = {0, 150};
constexpr std::array<unsigned short, 2> kSmallB = {0, 200};

constexpr std::array<unsigned short, 2> kMult256R = {0, 2560};
constexpr std::array<unsigned short, 2> kMult256G = {0, 5120};
constexpr std::array<unsigned short, 2> kMult256B = {0, 7680};

constexpr std::array<unsigned short, 2> kNonMultR = {0, 1000};
constexpr std::array<unsigned short, 2> kNonMultG = {0, 2000};
constexpr std::array<unsigned short, 2> kNonMultB = {0, 3000};

constexpr std::array<unsigned short, 1> kExplicitR = {65535};
constexpr std::array<unsigned short, 1> kExplicitG = {65535};
constexpr std::array<unsigned short, 1> kExplicitB = {65535};

constexpr auto kColorMapCases = std::to_array<ColorMapCase>({
    // Values in [0, 255] range -> multiplier fixed up to 1.
    {.r = kSmallR,
     .g = kSmallG,
     .b = kSmallB,
     .initial_mult = 0,
     .default_mult = 257,
     .nodata_set = true,
     .nodata_val = 0.0,
     .expected_mult = 1,
     .check_idx = 1,
     .expected_entry = {100, 150, 200, 255},
     .check_nodata_alpha = true},
    // Multiples of 256 -> multiplier = 256.
    {.r = kMult256R,
     .g = kMult256G,
     .b = kMult256B,
     .initial_mult = 0,
     .default_mult = 257,
     .nodata_set = false,
     .nodata_val = 0.0,
     .expected_mult = 256,
     .check_idx = 1,
     .expected_entry = {10, 20, 30, 255},
     .check_nodata_alpha = false},
    // Not multiples of 256 or 257 -> defaults to nDefaultColorTableMultiplier.
    {.r = kNonMultR,
     .g = kNonMultG,
     .b = kNonMultB,
     .initial_mult = 0,
     .default_mult = 257,
     .nodata_set = false,
     .nodata_val = 0.0,
     .expected_mult = 257,
     .check_idx = 1,
     .expected_entry = {1000 / 257, 2000 / 257, 3000 / 257, 255},
     .check_nodata_alpha = false},
    // Explicit non-zero multiplier passed in.
    {.r = kExplicitR,
     .g = kExplicitG,
     .b = kExplicitB,
     .initial_mult = 257,
     .default_mult = 257,
     .nodata_set = false,
     .nodata_val = 0.0,
     .expected_mult = 257,
     .check_idx = 0,
     .expected_entry = {255, 255, 255, 255},
     .check_nodata_alpha = false},
});

TEST(TiffCommonTest, TIFFColorMapTagToColorTable) {
  for (const auto& tc : kColorMapCases) {
    ASSERT_EQ(tc.r.size(), tc.g.size());
    ASSERT_EQ(tc.r.size(), tc.b.size());
    int mult = tc.initial_mult;
    std::unique_ptr<GDALColorTable> ct =
        gdal::tiff_common::TIFFColorMapTagToColorTable(
            tc.r.data(), tc.g.data(), tc.b.data(),
            static_cast<int>(tc.r.size()), mult, tc.default_mult, tc.nodata_set,
            tc.nodata_val);
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(tc.expected_mult, mult);
    if (tc.check_nodata_alpha) {
      const GDALColorEntry* nodata_entry =
          ct->GetColorEntry(static_cast<int>(tc.nodata_val));
      ASSERT_NE(nullptr, nodata_entry);
      EXPECT_EQ(0, nodata_entry->c4);
    }
    const GDALColorEntry* entry = ct->GetColorEntry(tc.check_idx);
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(tc.expected_entry.c1, entry->c1);
    EXPECT_EQ(tc.expected_entry.c2, entry->c2);
    EXPECT_EQ(tc.expected_entry.c3, entry->c3);
    EXPECT_EQ(tc.expected_entry.c4, entry->c4);
  }
}

consteval std::array<double, 92> MakeRpcValues() {
  std::array<double, 92> values{};
  for (size_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<double>(i + 1);
  }
  return values;
}

constexpr auto kRpcValues = MakeRpcValues();

struct RpcMetadataCase {
  const char* key;
  const char* expected;
};

constexpr auto kRpcScalarCases = std::to_array<RpcMetadataCase>({
    {.key = RPC_ERR_BIAS, .expected = "1"},
    {.key = RPC_ERR_RAND, .expected = "2"},
    {.key = RPC_LINE_OFF, .expected = "3"},
    {.key = RPC_SAMP_OFF, .expected = "4"},
    {.key = RPC_LAT_OFF, .expected = "5"},
    {.key = RPC_LONG_OFF, .expected = "6"},
    {.key = RPC_HEIGHT_OFF, .expected = "7"},
    {.key = RPC_LINE_SCALE, .expected = "8"},
    {.key = RPC_SAMP_SCALE, .expected = "9"},
    {.key = RPC_LAT_SCALE, .expected = "10"},
    {.key = RPC_LONG_SCALE, .expected = "11"},
    {.key = RPC_HEIGHT_SCALE, .expected = "12"},
    {.key = RPC_LINE_NUM_COEFF,
     .expected = "13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32"},
    {.key = RPC_LINE_DEN_COEFF,
     .expected = "33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52"},
    {.key = RPC_SAMP_NUM_COEFF,
     .expected = "53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72"},
    {.key = RPC_SAMP_DEN_COEFF,
     .expected = "73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92"},
});

TEST(TiffCommonTest, TIFFRPCTagToRPCMetadata) {
  CPLStringList md =
      gdal::tiff_common::TIFFRPCTagToRPCMetadata(kRpcValues.data());
  for (const auto& tc : kRpcScalarCases) {
    EXPECT_STREQ(tc.expected, md.FetchNameValue(tc.key));
  }
}

}  // namespace
}  // namespace autotest2
