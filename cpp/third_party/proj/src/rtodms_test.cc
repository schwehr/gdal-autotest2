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
// Unit tests for radian-to-DMS formatting and precision configuration
// (proj/src/rtodms.cpp and proj_rtodms2 in proj/src/coordinates.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/rtodms.cpp

#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kArcMinToRad = kDegToRad / 60.0;
constexpr double kArcSecToRad = kDegToRad / 3600.0;

constexpr double DmsToRad(int deg, int min, double sec) {
  return deg * kDegToRad + min * kArcMinToRad + sec * kArcSecToRad;
}

TEST(RtodmsTest, DefaultFormattingZeroNegativeZeroAndCarryRollover) {
  // Process defaults are fract=3, con_w=0; restore defensively on exit.
  absl::Cleanup restore_rtodms = [] { set_rtodms(3, 0); };

  struct Case {
    double rad;
    int pos;
    int neg;
    std::string_view expected;
  };
  constexpr Case kCases[] = {
      {.rad = 0.0, .pos = 'N', .neg = 'S', .expected = "0dN"},
      // Negative zero is not < 0.0, so it uses the positive hemisphere suffix.
      {.rad = -0.0, .pos = 'N', .neg = 'S', .expected = "0dN"},
      {.rad = -0.0, .pos = 0, .neg = 0, .expected = "0d"},
      {.rad = 45.0 * kDegToRad, .pos = 'E', .neg = 'W', .expected = "45dE"},
      {.rad = -45.0 * kDegToRad, .pos = 'E', .neg = 'W', .expected = "45dW"},
      {.rad = -45.0 * kDegToRad, .pos = 0, .neg = 0, .expected = "-45d"},
      {.rad = DmsToRad(1, 30, 0.0),
       .pos = 'E',
       .neg = 'W',
       .expected = "1d30'E"},
      {.rad = -DmsToRad(1, 30, 0.0), .pos = 0, .neg = 0, .expected = "-1d30'"},
      // Rounding 59.9999" at 1e-3 s resolution carries into minutes/degrees.
      {.rad = DmsToRad(0, 0, 59.9999),
       .pos = 'N',
       .neg = 'S',
       .expected = "0d1'N"},
      {.rad = DmsToRad(0, 59, 59.9999),
       .pos = 'N',
       .neg = 'S',
       .expected = "1dN"},
  };

  for (const auto& tc : kCases) {
    char buf[64]{};
    const char* ret = rtodms(buf, sizeof(buf), tc.rad, tc.pos, tc.neg);
    EXPECT_EQ(buf, ret);
    EXPECT_EQ(tc.expected, std::string_view(buf));
  }
}

TEST(RtodmsTest, FractionalSecondsTrimmingBranches) {
  absl::Cleanup restore_rtodms = [] { set_rtodms(3, 0); };
  set_rtodms(3, 0);

  struct Case {
    double rad;
    int pos;
    int neg;
    std::string_view expected;
  };
  constexpr Case kCases[] = {
      // Non-zero last decimal digit: no trailing zeros stripped.
      {.rad = DmsToRad(2, 3, 4.567),
       .pos = 'N',
       .neg = 'S',
       .expected = "2d3'4.567\"N"},
      // Trailing fractional zeros stripped while keeping non-zero tenths digit.
      {.rad = DmsToRad(2, 3, 4.5),
       .pos = 'N',
       .neg = 'S',
       .expected = "2d3'4.5\"N"},
      // All fractional digits are zero: decimal point itself is stripped.
      {.rad = DmsToRad(2, 3, 4.0),
       .pos = 'N',
       .neg = 'S',
       .expected = "2d3'4\"N"},
      // Signed output (pos=0, neg=0) exercises suffix_len=2 trimming path.
      {.rad = -DmsToRad(2, 3, 4.5),
       .pos = 0,
       .neg = 0,
       .expected = "-2d3'4.5\""},
      {.rad = -DmsToRad(2, 3, 4.0), .pos = 0, .neg = 0, .expected = "-2d3'4\""},
  };

  for (const auto& tc : kCases) {
    char buf[64]{};
    rtodms(buf, sizeof(buf), tc.rad, tc.pos, tc.neg);
    EXPECT_EQ(tc.expected, std::string_view(buf));
  }
}

TEST(RtodmsTest, SetRtodmsPrecisionAndFixedWidthModes) {
  absl::Cleanup restore_rtodms = [] { set_rtodms(3, 0); };

  char buf[64]{};

  // 1. Fixed-width (con_w=1) with default 3 fractional digits.
  set_rtodms(3, 1);
  rtodms(buf, sizeof(buf), 0.0, 'N', 'S');
  EXPECT_STREQ("0d00'00.000\"N", buf);
  rtodms(buf, sizeof(buf), DmsToRad(2, 3, 4.5), 'N', 'S');
  EXPECT_STREQ("2d03'04.500\"N", buf);
  rtodms(buf, sizeof(buf), -DmsToRad(2, 3, 4.5), 0, 0);
  EXPECT_STREQ("-2d03'04.500\"", buf);

  // 2. Zero fractional digits with fixed-width (fract=0, con_w=1).
  set_rtodms(0, 1);
  rtodms(buf, sizeof(buf), DmsToRad(1, 5, 9.0), 'E', 'W');
  EXPECT_STREQ("1d05'09\"E", buf);
  rtodms(buf, sizeof(buf), DmsToRad(0, 0, 10.0), 'N', 'S');
  EXPECT_STREQ("0d00'10\"N", buf);

  // 3. Zero fractional digits without fixed-width (fract=0, con_w=0).
  set_rtodms(0, 0);
  rtodms(buf, sizeof(buf), DmsToRad(0, 0, 15.0), 'N', 'S');
  EXPECT_STREQ("0d0'15\"N", buf);

  // 4. Higher precisions within [0, 8].
  set_rtodms(6, 0);
  rtodms(buf, sizeof(buf), DmsToRad(10, 20, 30.123456), 'N', 'S');
  EXPECT_STREQ("10d20'30.123456\"N", buf);

  set_rtodms(8, 1);
  rtodms(buf, sizeof(buf), DmsToRad(0, 0, 1.12345678), 'N', 'S');
  EXPECT_STREQ("0d00'01.12345678\"N", buf);
}

TEST(RtodmsTest, SetRtodmsOutOfRangeLeavesStateUnchanged) {
  absl::Cleanup restore_rtodms = [] { set_rtodms(3, 0); };

  set_rtodms(1, 1);
  set_rtodms(-1, 0);
  set_rtodms(9, 0);

  char buf[64]{};
  rtodms(buf, sizeof(buf), DmsToRad(1, 2, 3.5), 'E', 'W');
  EXPECT_STREQ("1d02'03.5\"E", buf);
}

TEST(RtodmsTest, SmallBufferGuardsPreventOverflowAndUnderflow) {
  absl::Cleanup restore_rtodms = [] { set_rtodms(3, 0); };
  set_rtodms(3, 0);

  // 1. sizeof_s == 1 with negative value and pos == 0 returns empty NUL string.
  char one[1] = {'X'};
  const char* ret_one = rtodms(one, sizeof(one), -1.0, 0, 0);
  EXPECT_EQ(one, ret_one);
  EXPECT_STREQ("", one);

  // 2. sizeof_s == 2 with negative value and pos == 0 writes "-" and exits via
  // suffix_len > strlen(ss) without stepping backwards past ss.
  char two[2] = {'X', 'X'};
  rtodms(two, sizeof(two), -DmsToRad(0, 0, 12.345), 0, 0);
  EXPECT_STREQ("-", two);

  // 3. sizeof_s == 3 with pos != 0 writes 2 chars so suffix_len (3) >
  // strlen(ss) triggers early return without trimming.
  char three[3] = {'X', 'X', 'X'};
  rtodms(three, sizeof(three), DmsToRad(0, 0, 12.345), 'N', 'S');
  EXPECT_STREQ("0d", three);
}

TEST(RtodmsTest, PublicProjRtodms2RoundTripsWithProjDmstor) {
  absl::Cleanup restore_rtodms = [] { set_rtodms(3, 0); };
  set_rtodms(3, 0);

  constexpr double kAnglesRad[] = {
      DmsToRad(55, 40, 34.125),
      -DmsToRad(122, 25, 9.875),
      0.0,
  };
  for (const double rad : kAnglesRad) {
    char buf[64]{};
    const char* formatted = proj_rtodms2(buf, sizeof(buf), rad, 'E', 'W');
    EXPECT_EQ(buf, formatted);
    char* endptr = nullptr;
    const double parsed = proj_dmstor(buf, &endptr);
    ASSERT_NE(nullptr, endptr);
    EXPECT_EQ('\0', *endptr);
    EXPECT_NEAR(rad, parsed, 1e-8);
  }
}

// TODO(schwehr): Cover non-C LC_NUMERIC comma decimal separator normalization
// in rtodms.cpp lines 76-77 if a hermetic comma-decimal locale is available.

}  // namespace
}  // namespace proj
