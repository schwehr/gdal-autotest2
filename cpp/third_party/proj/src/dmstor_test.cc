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
// Unit tests for DMS/radian string parsing in dmstor_ctx(), dmstor(), and
// proj_dmstor() (proj/src/dmstor.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/dmstor.cpp

#include <cerrno>
#include <cmath>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(DmstorTest, ParsesValidDmsUtf8IsoDegreeAndRadianStrings) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ValidCase {
    const char* input;
    double expected_rad;
    const char* expected_tail;
  };
  constexpr ValidCase kCases[] = {
      {.input = "45d30'15\"",
       .expected_rad = DegToRad(45.0 + 30.0 / 60.0 + 15.0 / 3600.0),
       .expected_tail = ""},
      {.input = "45D30'15.5\"",
       .expected_rad = DegToRad(45.0 + 30.0 / 60.0 + 15.5 / 3600.0),
       .expected_tail = ""},
      {.input = "-45d30'15\"",
       .expected_rad = -DegToRad(45.0 + 30.0 / 60.0 + 15.0 / 3600.0),
       .expected_tail = ""},
      {.input = "+12d15'",
       .expected_rad = DegToRad(12.25),
       .expected_tail = ""},
      {.input = "30'15\"",
       .expected_rad = DegToRad(30.0 / 60.0 + 15.0 / 3600.0),
       .expected_tail = ""},
      {.input = "45\"",
       .expected_rad = DegToRad(45.0 / 3600.0),
       .expected_tail = ""},
      {.input = "45d15\"",
       .expected_rad = DegToRad(45.0 + 15.0 / 3600.0),
       .expected_tail = ""},
      {.input = ".5d", .expected_rad = DegToRad(0.5), .expected_tail = ""},
      {.input = "-.25d", .expected_rad = DegToRad(-0.25), .expected_tail = ""},
      // Trailing unit omitted falls back to vm[nl].
      {.input = "45", .expected_rad = DegToRad(45.0), .expected_tail = ""},
      {.input = "45d30", .expected_rad = DegToRad(45.5), .expected_tail = ""},
      {.input = "45d30'30",
       .expected_rad = DegToRad(45.0 + 30.0 / 60.0 + 30.0 / 3600.0),
       .expected_tail = ""},
      // UTF-8 degree sign (U+00B0 = 0xC2 0xB0) and ISO-8859-1 byte 0xB0.
      {.input = "45\xc2\xb0"
                "30'15\"N",
       .expected_rad = DegToRad(45.0 + 30.0 / 60.0 + 15.0 / 3600.0),
       .expected_tail = ""},
      {.input = "45\xb0"
                "30'W",
       .expected_rad = -DegToRad(45.5),
       .expected_tail = ""},
      // Radians suffix 'r' and 'R'.
      {.input = "1.25r", .expected_rad = 1.25, .expected_tail = ""},
      {.input = "-0.5R", .expected_rad = -0.5, .expected_tail = ""},
      // All hemisphere postfix letters.
      {.input = "10dN", .expected_rad = DegToRad(10.0), .expected_tail = ""},
      {.input = "10dn", .expected_rad = DegToRad(10.0), .expected_tail = ""},
      {.input = "10dE", .expected_rad = DegToRad(10.0), .expected_tail = ""},
      {.input = "10de", .expected_rad = DegToRad(10.0), .expected_tail = ""},
      {.input = "10dS", .expected_rad = -DegToRad(10.0), .expected_tail = ""},
      {.input = "10ds", .expected_rad = -DegToRad(10.0), .expected_tail = ""},
      {.input = "10dW", .expected_rad = -DegToRad(10.0), .expected_tail = ""},
      {.input = "10dw", .expected_rad = -DegToRad(10.0), .expected_tail = ""},
      // Postfix sign overrides prefix sign.
      {.input = "-10dS", .expected_rad = -DegToRad(10.0), .expected_tail = ""},
      {.input = "-10dN", .expected_rad = DegToRad(10.0), .expected_tail = ""},
      // Leading whitespace and trailing unparsed characters.
      {.input = "  \t45d30'E extra",
       .expected_rad = DegToRad(45.5),
       .expected_tail = " extra"},
      {.input = "45d 30'",
       .expected_rad = DegToRad(45.0),
       .expected_tail = " 30'"},
      {.input = "45dX", .expected_rad = DegToRad(45.0), .expected_tail = "X"},
  };

  for (const auto& tc : kCases) {
    char* end = nullptr;
    EXPECT_NEAR(tc.expected_rad, dmstor_ctx(ctx.get(), tc.input, &end), 1e-13)
        << tc.input;
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << tc.input;
    ASSERT_NE(nullptr, end) << tc.input;
    EXPECT_STREQ(tc.expected_tail, end) << tc.input;

    // Also exercise rs == nullptr path.
    EXPECT_NEAR(tc.expected_rad, dmstor_ctx(ctx.get(), tc.input, nullptr),
                1e-13)
        << tc.input;
  }
}

TEST(DmstorTest, HandlesEmptyNonNumericAndTruncatedInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  char* end = nullptr;
  EXPECT_DOUBLE_EQ(0.0, dmstor_ctx(ctx.get(), "", &end));
  EXPECT_STREQ("", end);

  EXPECT_DOUBLE_EQ(0.0, dmstor_ctx(ctx.get(), "   \t", &end));
  EXPECT_STREQ("", end);

  const char* alpha = "abc";
  EXPECT_DOUBLE_EQ(0.0, dmstor_ctx(ctx.get(), alpha, &end));
  EXPECT_EQ(alpha, end);

  // Inputs exceeding MAX_WORK - 1 (63) printable bytes stop copying cleanly.
  const std::string overlong = std::string(70, '0') + "45d";
  EXPECT_DOUBLE_EQ(0.0, dmstor_ctx(ctx.get(), overlong.c_str(), &end));
  EXPECT_EQ(overlong.c_str() + 63, end);
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

TEST(DmstorTest, RejectsOutOfOrderUnitsInvalidRadiansAndOverflow) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kInvalidCases[] = {
      "45d10d",  // duplicate degree unit (n < nl)
      "30'10d",  // degrees after minutes (n < nl)
      "30'10'",  // duplicate minute unit (n < nl)
      "45d1r",   // radians after degrees (nl != 0)
      "30'1R",   // radians after minutes (nl != 0)
  };

  for (const char* bad : kInvalidCases) {
    proj_context_errno_set(ctx.get(), 0);
    char* end = nullptr;
    EXPECT_EQ(HUGE_VAL, dmstor_ctx(ctx.get(), bad, &end)) << bad;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << bad;
    EXPECT_EQ(bad, end) << bad;
  }

  // Overflow via both 'd'-terminated strtod() and pj_strtod() paths.
  for (const char* overflow_str : {"1e1000d", "1e1000"}) {
    errno = 0;
    char* end = nullptr;
    EXPECT_EQ(HUGE_VAL, dmstor_ctx(ctx.get(), overflow_str, &end))
        << overflow_str;
    EXPECT_EQ(overflow_str, end) << overflow_str;
    errno = 0;
  }
}

TEST(DmstorTest, PublicProjDmstorAndDmstorUseDefaultContextSafely) {
  absl::Cleanup reset_default = [] { proj_errno_reset(nullptr); };

  char* end = nullptr;
  EXPECT_NEAR(DegToRad(45.5), dmstor("45d30'N", &end), 1e-13);
  EXPECT_STREQ("", end);

  EXPECT_NEAR(-DegToRad(12.25), proj_dmstor("12d15'W", &end), 1e-13);
  EXPECT_STREQ("", end);
}

}  // namespace
}  // namespace proj
