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
// Unit tests for locale-independent ASCII floating-point parsing
// (proj/src/strtod.cpp). Note: on Linux/glibc builds, the comma-decimal locale
// test synthesizes a minimal 54-byte glibc LC_NUMERIC archive under LOCPATH so
// setlocale(LC_NUMERIC, "comma.UTF-8") succeeds hermetically inside the test
// sandbox without host locale packages installed.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/strtod.cpp

#include <array>
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(StrtodTest, PjAtofAndPjStrtodParseCLocaleValuesAndEndPtr) {
  struct ValidCase {
    const char* text;
    double expected;
    const char* expected_tail;
  };
  constexpr ValidCase kCases[] = {
      {.text = "0", .expected = 0.0, .expected_tail = ""},
      {.text = "-0.0", .expected = -0.0, .expected_tail = ""},
      {.text = "123.456", .expected = 123.456, .expected_tail = ""},
      {.text = "  +1.25e3", .expected = 1250.0, .expected_tail = ""},
      {.text = "-2.5E-2", .expected = -0.025, .expected_tail = ""},
      {.text = ".5", .expected = 0.5, .expected_tail = ""},
      {.text = "-.25", .expected = -0.25, .expected_tail = ""},
      {.text = "0x1.8p1", .expected = 3.0, .expected_tail = ""},
      {.text = "  42.5abc", .expected = 42.5, .expected_tail = "abc"},
  };

  for (const auto& tc : kCases) {
    char* end = nullptr;
    EXPECT_DOUBLE_EQ(tc.expected, pj_strtod(tc.text, &end)) << tc.text;
    ASSERT_NE(nullptr, end) << tc.text;
    EXPECT_STREQ(tc.expected_tail, end) << tc.text;
    // NOLINTNEXTLINE(runtime/deprecated_fn)
    EXPECT_DOUBLE_EQ(tc.expected, pj_atof(tc.text)) << tc.text;
  }

  // Empty and non-numeric strings leave endptr at start of input.
  for (const char* bad : {"", "not_a_number"}) {
    char* end = nullptr;
    EXPECT_DOUBLE_EQ(0.0, pj_strtod(bad, &end)) << bad;
    EXPECT_EQ(bad, end) << bad;
  }

  // Special IEEE-754 values and ERANGE overflow/underflow propagation.
  EXPECT_TRUE(std::isinf(pj_atof("inf")));   // NOLINT(runtime/deprecated_fn)
  EXPECT_GT(pj_atof("inf"), 0.0);            // NOLINT(runtime/deprecated_fn)
  EXPECT_TRUE(std::isinf(pj_atof("-INF")));  // NOLINT(runtime/deprecated_fn)
  EXPECT_LT(pj_atof("-INF"), 0.0);           // NOLINT(runtime/deprecated_fn)
  EXPECT_TRUE(std::isnan(pj_atof("nan")));   // NOLINT(runtime/deprecated_fn)

  char* end = nullptr;
  errno = 0;
  const double overflow = pj_strtod("1e1000tail", &end);
  EXPECT_EQ(ERANGE, errno);
  EXPECT_TRUE(std::isinf(overflow));
  EXPECT_STREQ("tail", end);
  errno = 0;

  const double underflow = pj_strtod("1e-1000", &end);
  EXPECT_EQ(ERANGE, errno);
  EXPECT_DOUBLE_EQ(0.0, underflow);
  errno = 0;
}

TEST(StrtodTest, HonorsAsciiDecimalPointEvenUnderCommaDecimalLocale) {
  const char* saved_cstr = std::setlocale(LC_NUMERIC, nullptr);
  ASSERT_NE(nullptr, saved_cstr);
  const std::string saved_locale(saved_cstr);
  const char* saved_locpath_ptr = std::getenv("LOCPATH");
  const bool had_locpath = (saved_locpath_ptr != nullptr);
  const std::string saved_locpath = had_locpath ? saved_locpath_ptr : "";
  absl::Cleanup restore_locale = [&saved_locale, had_locpath, &saved_locpath] {
    if (had_locpath) {
      setenv("LOCPATH", saved_locpath.c_str(), 1);
    } else {
      unsetenv("LOCPATH");
    }
    std::setlocale(LC_NUMERIC, saved_locale.c_str());
    errno = 0;
  };

  // Synthesize a minimal 54-byte glibc LC_NUMERIC locale binary in a TempDir
  // with decimal_point = "," and thousands_sep = "." so setlocale(LC_NUMERIC,
  // "comma.UTF-8") succeeds hermetically inside the test sandbox:
  //   0x00..0x03: magic = 0x20031114 (glibc _NL_ITEM_INDEX(LC_NUMERIC) magic)
  //   0x04..0x07: nstrings = 6
  //   0x08..0x1f: 6 uint32_t string offsets (0x20, 0x22, 0x24, 0x28, 0x2c,
  //   0x30) 0x20..0x35: payload ("," NUL, "." NUL, "\x03\x00\x00\x00", wdecimal
  //   ",",
  //               wthousands ".", codeset "UTF-8\0").
  static constexpr auto kGlibcCommaLcNumeric = std::to_array<uint8_t>({
      0x14, 0x11, 0x03, 0x20, 0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
      0x00, 0x22, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x28, 0x00,
      0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x2c,
      0x00, 0x2e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
      0x2e, 0x00, 0x00, 0x00, 0x55, 0x54, 0x46, 0x2d, 0x38, 0x00,
  });
  TempDir loc_root("proj_strtod_locpath");
  ASSERT_TRUE(WriteBinaryFile(loc_root.child("comma.UTF-8/LC_NUMERIC"),
                              kGlibcCommaLcNumeric));
  setenv("LOCPATH", loc_root.c_str(), 1);

  constexpr const char* kCommaLocales[] = {
      "comma.UTF-8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8",
      "es_ES.UTF-8", "es_ES.utf8",  "de_DE",      "fr_FR",       "es_ES",
  };

  bool has_comma_locale = false;
  for (const char* loc : kCommaLocales) {
    if (std::setlocale(LC_NUMERIC, loc) != nullptr) {
      const struct lconv* lc = std::localeconv();
      if (lc != nullptr && lc->decimal_point != nullptr &&
          lc->decimal_point[0] == ',') {
        has_comma_locale = true;
        break;
      }
    }
  }
  EXPECT_TRUE(has_comma_locale);

  if (has_comma_locale) {
    // Check whether the linked C library's strtod() honors LC_NUMERIC
    // (glibc) or is strictly C-locale (llvm-libc).
    const bool libc_strtod_uses_locale = (std::strtod("12,5", nullptr) == 12.5);

    // 1. Input with ASCII '.' (exercises pszPoint != nullptr with both
    // endptr == nullptr via pj_atof and endptr != nullptr via pj_strtod).
    if (libc_strtod_uses_locale) {
      EXPECT_DOUBLE_EQ(12.5, pj_atof("12.5"));  // NOLINT(runtime/deprecated_fn)
    } else {
      EXPECT_DOUBLE_EQ(12.0, pj_atof("12.5"));  // NOLINT(runtime/deprecated_fn)
    }

    char* end = nullptr;
    if (libc_strtod_uses_locale) {
      EXPECT_DOUBLE_EQ(-1250.0, pj_strtod("-12.5e2xyz", &end));
      ASSERT_NE(nullptr, end);
      EXPECT_STREQ("xyz", end);
    } else {
      EXPECT_DOUBLE_EQ(-12.0, pj_strtod("-12.5e2xyz", &end));
      ASSERT_NE(nullptr, end);
      EXPECT_STREQ(".5e2xyz", end);
    }

    // 2. Locale radix ',' is replaced with ' ', stopping numeric conversion
    // before the comma rather than treating ',' as a decimal separator.
    EXPECT_DOUBLE_EQ(12.0, pj_strtod("12,5", &end));
    EXPECT_STREQ(",5", end);

    // 3. String containing both ASCII '.' and locale ',' exercises both
    // pszLocalePoint != nullptr and pszPoint != nullptr in
    // replace_point_by_locale_point.
    if (libc_strtod_uses_locale) {
      EXPECT_DOUBLE_EQ(12.5, pj_strtod("12.5,9", &end));
      EXPECT_STREQ(",9", end);
    } else {
      EXPECT_DOUBLE_EQ(12.0, pj_strtod("12.5,9", &end));
      EXPECT_STREQ(".5,9", end);
    }

    // 4. String containing neither '.' nor ',' takes the fast strtod path.
    EXPECT_DOUBLE_EQ(42.0, pj_strtod("42abc", &end));
    EXPECT_STREQ("abc", end);

    // 5. Overflow inside replaced buffer preserves errno = ERANGE across
    // free(pszNumber).
    errno = 0;
    const double huge = pj_strtod("1e1000.0", &end);
    EXPECT_EQ(ERANGE, errno);
    EXPECT_TRUE(std::isinf(huge));
    errno = 0;
  }

  // Verify C locale behavior explicitly after testing locale switching.
  ASSERT_NE(nullptr, std::setlocale(LC_NUMERIC, "C"));
  // NOLINTNEXTLINE(runtime/deprecated_fn)
  EXPECT_DOUBLE_EQ(3.14159, pj_atof("3.14159"));
}

}  // namespace
}  // namespace proj
