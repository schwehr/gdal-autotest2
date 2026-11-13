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
// Tests CODA time conversions, calendar arithmetic (Julian/Gregorian), string
// formatting/parsing, leap second table loading, and deprecated backward
// compatibility wrappers (libcoda/coda-time.c).
//
// google3 build caveats:
//   - The built-in leap second table in coda-time.c ends at 2017-01-01
//     (2016-12-31 23:59:60 UTC). In coda_time_double_to_parts_utc(), querying a
//     timestamp strictly after the last entry advances leap_sec to
//     num_leap_seconds and reads leap_second_table[num_leap_seconds] (an
//     uninitialized/out-of-bounds element), so UTC double-to-parts tests stay
//     at or before the last loaded leap second entry.
//   - coda_time_parts_to_string() formats "MMM" using month_name[month]
//     (0-based array indexed by 1-based month), which is an upstream
//     off-by-one bug and would read out-of-bounds for month == 12; tests pin
//     month < 12.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-time.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

constexpr double kSecondsPerDay = 86400.0;

std::string MakeTaiUtcLine(int year, std::string_view month, int day, double jd,
                           double tai_utc, double mjd_offset) {
  // read_leap_second_table() requires every line to be exactly 80 characters
  // excluding '\n' and match the USNO tai-utc.dat field layout.
  return absl::StrFormat(
      " %04d %-3s %2d =JD %9.1f  TAI-UTC=%7.1f      S + (MJD - %6.0f.) X 0.0"
      "     S",
      year, month, day, jd, tai_utc, mjd_offset);
}

// Isolates CODA_DEFINITION, CODA_LEAP_SECOND_TABLE, and runtime options before
// coda_init() so ambient leap-second env vars cannot affect expected TAI
// values.
class ScopedIsolatedCodaSession {
 public:
  ScopedIsolatedCodaSession()
      : env_def_("CODA_DEFINITION"), env_leap_("CODA_LEAP_SECOND_TABLE") {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_definition_path(nullptr);
  }

  ~ScopedIsolatedCodaSession() { coda_set_definition_path(nullptr); }

  int Init() { return session_.Init(); }

 private:
  ScopedCodaOptions options_;
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  ScopedCodaSession session_;
};

TEST(CodaTimeTest, MonthToIntegerHandlesAllMonthsCaseInsensitively) {
  struct MonthCase {
    const char input[4];
    int expected;
  };
  constexpr MonthCase kMonthCases[] = {
      {.input = "jan", .expected = 1},  {.input = "FEB", .expected = 2},
      {.input = "Mar", .expected = 3},  {.input = "aPr", .expected = 4},
      {.input = "MAY", .expected = 5},  {.input = "jun", .expected = 6},
      {.input = "Jul", .expected = 7},  {.input = "AUG", .expected = 8},
      {.input = "sEp", .expected = 9},  {.input = "oct", .expected = 10},
      {.input = "NOV", .expected = 11}, {.input = "Dec", .expected = 12},
  };

  for (const MonthCase& tc : kMonthCases) {
    EXPECT_EQ(tc.expected, coda_month_to_integer(tc.input))
        << "input=" << tc.input;
  }

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };
  EXPECT_EQ(-1, coda_month_to_integer("xyz"));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST(CodaTimeTest, DayOfYearToMonthDayHandlesLeapNonLeapAndErrors) {
  int month = -1;
  int day = -1;

  // Leap year (2004): day 60 is Feb 29, day 61 is Mar 1, day 366 is Dec 31.
  ASSERT_EQ(CODA_SUCCESS, coda_dayofyear_to_month_day(2004, 60, &month, &day));
  EXPECT_EQ(2, month);
  EXPECT_EQ(29, day);
  ASSERT_EQ(CODA_SUCCESS, coda_dayofyear_to_month_day(2004, 61, &month, &day));
  EXPECT_EQ(3, month);
  EXPECT_EQ(1, day);
  ASSERT_EQ(CODA_SUCCESS, coda_dayofyear_to_month_day(2004, 366, &month, &day));
  EXPECT_EQ(12, month);
  EXPECT_EQ(31, day);

  // Non-leap century year (1900): day 60 is Mar 1.
  ASSERT_EQ(CODA_SUCCESS, coda_dayofyear_to_month_day(1900, 60, &month, &day));
  EXPECT_EQ(3, month);
  EXPECT_EQ(1, day);

  // Null pointer and out-of-range checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(-1, coda_dayofyear_to_month_day(2004, 1, nullptr, &day));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_dayofyear_to_month_day(2004, 1, &month, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_dayofyear_to_month_day(2004, -1, &month, &day));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_dayofyear_to_month_day(2004, 367, &month, &day));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);

  // Year 0 does not exist in the Julian/Gregorian astronomical calendar.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_dayofyear_to_month_day(0, 1, &month, &day));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
}

TEST(CodaTimeTest, PartsAndDoubleRoundTripAcrossJulianAndGregorianRegimes) {
  struct CalendarCase {
    int year;
    int month;
    int day;
    int mjd2000;
  };
  // Reference milestones from coda-time.c covering BC Julian, AD Julian
  // (including year 100 Julian leap year), the 1752 British calendar
  // transition, and multiple 400-year Gregorian sub-cycles (century non-leap,
  // 4-year leap).
  constexpr CalendarCase kCalendarCases[] = {
      {.year = -4713, .month = 1, .day = 1, .mjd2000 = -2451545},
      {.year = -1, .month = 12, .day = 31, .mjd2000 = -730122},
      {.year = 1, .month = 1, .day = 1, .mjd2000 = -730121},
      {.year = 100, .month = 1, .day = 1, .mjd2000 = -693962},
      {.year = 1586, .month = 10, .day = 4, .mjd2000 = -150924},
      {.year = 1751, .month = 6, .day = 15, .mjd2000 = -90769},
      {.year = 1752, .month = 9, .day = 2, .mjd2000 = -90324},
      {.year = 1752, .month = 9, .day = 14, .mjd2000 = -90323},
      {.year = 1858, .month = 11, .day = 17, .mjd2000 = -51544},
      {.year = 1900, .month = 3, .day = 1, .mjd2000 = -36465},
      {.year = 1950, .month = 1, .day = 1, .mjd2000 = -18262},
      {.year = 1970, .month = 1, .day = 1, .mjd2000 = -10957},
      {.year = 1996, .month = 2, .day = 29, .mjd2000 = -1402},
      {.year = 1999, .month = 12, .day = 31, .mjd2000 = -1},
      {.year = 2000, .month = 1, .day = 1, .mjd2000 = 0},
      {.year = 2001, .month = 6, .day = 15, .mjd2000 = 531},
      {.year = 2100, .month = 3, .day = 1, .mjd2000 = 36584},
      {.year = 2104, .month = 2, .day = 29, .mjd2000 = 38044},
      {.year = 2501, .month = 12, .day = 31, .mjd2000 = 183351},
  };

  for (const CalendarCase& tc : kCalendarCases) {
    double dt = -1.0;
    ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(tc.year, tc.month, tc.day,
                                                      0, 0, 0, 0, &dt))
        << tc.year << "-" << tc.month << "-" << tc.day;
    EXPECT_DOUBLE_EQ(tc.mjd2000 * kSecondsPerDay, dt);

    int y = 0;
    int m = 0;
    int d = 0;
    int h = -1;
    int min = -1;
    int s = -1;
    int us = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_time_double_to_parts(dt, &y, &m, &d, &h, &min, &s, &us));
    EXPECT_EQ(tc.year, y);
    EXPECT_EQ(tc.month, m);
    EXPECT_EQ(tc.day, d);
    EXPECT_EQ(0, h);
    EXPECT_EQ(0, min);
    EXPECT_EQ(0, s);
    EXPECT_EQ(0, us);
  }
}

TEST(CodaTimeTest, DoubleToPartsRoundsAtHalfMicrosecondAndHandlesNegatives) {
  int y = 0;
  int m = 0;
  int d = 0;
  int h = 0;
  int min = 0;
  int s = 0;
  int us = 0;

  // +0.4 us truncates to 0 us; +0.6 us rounds to 1 us.
  ASSERT_EQ(CODA_SUCCESS, coda_time_double_to_parts(0.0000004, &y, &m, &d, &h,
                                                    &min, &s, &us));
  EXPECT_EQ(0, us);

  ASSERT_EQ(CODA_SUCCESS, coda_time_double_to_parts(0.0000006, &y, &m, &d, &h,
                                                    &min, &s, &us));
  EXPECT_EQ(1, us);

  // Half-microsecond roll-over across midnight.
  ASSERT_EQ(CODA_SUCCESS, coda_time_double_to_parts(86399.9999996, &y, &m, &d,
                                                    &h, &min, &s, &us));
  EXPECT_EQ(2000, y);
  EXPECT_EQ(1, m);
  EXPECT_EQ(2, d);
  EXPECT_EQ(0, h);
  EXPECT_EQ(0, min);
  EXPECT_EQ(0, s);
  EXPECT_EQ(0, us);

  // Negative fractional offset before epoch.
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_double_to_parts(-0.5, &y, &m, &d, &h, &min, &s, &us));
  EXPECT_EQ(1999, y);
  EXPECT_EQ(12, m);
  EXPECT_EQ(31, d);
  EXPECT_EQ(23, h);
  EXPECT_EQ(59, min);
  EXPECT_EQ(59, s);
  EXPECT_EQ(500000, us);

  // Forward parts -> double with full HMS + musec.
  double dt = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_parts_to_double(2000, 1, 1, 1, 2, 3, 456789, &dt));
  EXPECT_DOUBLE_EQ(3600.0 + 120.0 + 3.456789, dt);
}

TEST(CodaTimeTest, DoubleToPartsRejectsNullPointersAndNonFiniteInputs) {
  int y = 0;
  int m = 0;
  int d = 0;
  int h = 0;
  int min = 0;
  int s = 0;
  int us = 0;

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, nullptr, &m, &d, &h, &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, &y, nullptr, &d, &h, &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, &y, &m, nullptr, &h, &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, &y, &m, &d, nullptr, &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, &y, &m, &d, &h, nullptr, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, &y, &m, &d, &h, &min, nullptr, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_double_to_parts(0.0, &y, &m, &d, &h, &min, &s, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_time_double_to_parts(coda_NaN(), &y, &m, &d, &h, &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts(coda_PlusInf(), &y, &m, &d, &h, &min,
                                          &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts(coda_MinInf(), &y, &m, &d, &h, &min,
                                          &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
}

TEST(CodaTimeTest, PartsToDoubleRejectsInvalidDatesAndTimes) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(-1, coda_time_parts_to_double(2000, 1, 1, 0, 0, 0, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  struct InvalidPartsCase {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int musec;
  };
  constexpr InvalidPartsCase kInvalidCases[] = {
      {.year = 0,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 0,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 13,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 0,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 2,
       .day = 30,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      // 1900 is a Julian leap year, so dmy_to_mjd2000_julian() passes and
      // dmy_to_mjd2000_gregorian() rejects Feb 29.
      {.year = 1900,
       .month = 2,
       .day = 29,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      // Sep 3..13, 1752 were skipped in the British Julian->Gregorian switch.
      {.year = 1752,
       .month = 9,
       .day = 3,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 1752,
       .month = 9,
       .day = 13,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0},
      // Invalid HMS / musec fields.
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = -1,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 24,
       .minute = 0,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = -1,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 60,
       .second = 0,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = -1,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 61,
       .musec = 0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = -1},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 1000000},
  };

  for (const InvalidPartsCase& tc : kInvalidCases) {
    double dt = 0.0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_time_parts_to_double(tc.year, tc.month, tc.day, tc.hour,
                                        tc.minute, tc.second, tc.musec, &dt))
        << tc.year << "-" << tc.month << "-" << tc.day << " " << tc.hour << ":"
        << tc.minute << ":" << tc.second << "." << tc.musec;
    EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  }
}

TEST(CodaTimeTest, UtcPartsAndDoubleConversionsHandleLeapSecondsAndErrors) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct UtcCase {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int musec;
    double expected_tai;
  };
  constexpr UtcCase kUtcCases[] = {
      // Pre-1972 uses fixed TAI-UTC offset of 10 s.
      {.year = 1970,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0,
       .expected_tai = -10957.0 * kSecondsPerDay + 10.0},
      // 1972-01-01 00:00:00 UTC -> -883612790 TAI.
      {.year = 1972,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0,
       .expected_tai = -883612790.0},
      // First leap second in the built-in table (1972-06-30 23:59:60 UTC).
      {.year = 1972,
       .month = 6,
       .day = 30,
       .hour = 23,
       .minute = 59,
       .second = 60,
       .musec = 0,
       .expected_tai = -867887990.0},
      // Epoch transition: 1999-12-31 23:59:28 UTC == 0.0 TAI; 2000-01-01 == 32.
      {.year = 1999,
       .month = 12,
       .day = 31,
       .hour = 23,
       .minute = 59,
       .second = 28,
       .musec = 0,
       .expected_tai = 0.0},
      {.year = 2000,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0,
       .expected_tai = 32.0},
      // 2008-12-31 leap second boundary from coda-time.c docs.
      {.year = 2008,
       .month = 12,
       .day = 31,
       .hour = 23,
       .minute = 59,
       .second = 59,
       .musec = 0,
       .expected_tai = 284083232.0},
      {.year = 2008,
       .month = 12,
       .day = 31,
       .hour = 23,
       .minute = 59,
       .second = 60,
       .musec = 250000,
       .expected_tai = 284083233.25},
      {.year = 2009,
       .month = 1,
       .day = 1,
       .hour = 0,
       .minute = 0,
       .second = 0,
       .musec = 0,
       .expected_tai = 284083234.0},
      // Last leap second in the built-in table (2016-12-31 23:59:60 UTC).
      {.year = 2016,
       .month = 12,
       .day = 31,
       .hour = 23,
       .minute = 59,
       .second = 60,
       .musec = 0,
       .expected_tai = 536544036.0},
  };

  for (const UtcCase& tc : kUtcCases) {
    double tai = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double_utc(
                                tc.year, tc.month, tc.day, tc.hour, tc.minute,
                                tc.second, tc.musec, &tai));
    EXPECT_DOUBLE_EQ(tc.expected_tai, tai);

    int y = 0;
    int m = 0;
    int d = 0;
    int h = 0;
    int min = 0;
    int s = 0;
    int us = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_time_double_to_parts_utc(tai, &y, &m, &d, &h,
                                                          &min, &s, &us));
    EXPECT_EQ(tc.year, y);
    EXPECT_EQ(tc.month, m);
    EXPECT_EQ(tc.day, d);
    EXPECT_EQ(tc.hour, h);
    EXPECT_EQ(tc.minute, min);
    EXPECT_EQ(tc.second, s);
    EXPECT_EQ(tc.musec, us);
  }

  // parts_to_double_utc works past the end of the table (TAI-UTC = 37 after
  // 2017-01-01).
  double post_2017_tai = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double_utc(2017, 1, 1, 0, 0, 0, 0,
                                                        &post_2017_tai));
  EXPECT_DOUBLE_EQ(536544037.0, post_2017_tai);

  // Error paths on UTC functions.
  int y = 0;
  int m = 0;
  int d = 0;
  int h = 0;
  int min = 0;
  int s = 0;
  int us = 0;
  double dt = 0.0;

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_parts_to_double_utc(2000, 1, 1, 0, 0, 0, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_parts_to_double_utc(0, 1, 1, 0, 0, 0, 0, &dt));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_parts_to_double_utc(2000, 1, 1, 24, 0, 0, 0, &dt));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(0.0, nullptr, &m, &d, &h, &min,
                                              &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(0.0, &y, nullptr, &d, &h, &min,
                                              &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(0.0, &y, &m, nullptr, &h, &min,
                                              &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(0.0, &y, &m, &d, nullptr, &min,
                                              &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_time_double_to_parts_utc(0.0, &y, &m, &d, &h, nullptr, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(0.0, &y, &m, &d, &h, &min,
                                              nullptr, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(0.0, &y, &m, &d, &h, &min, &s,
                                              nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(coda_NaN(), &y, &m, &d, &h, &min,
                                              &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_parts_utc(coda_PlusInf(), &y, &m, &d, &h,
                                              &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
}

TEST(CodaTimeTest, LeapSecondTableInitLoadsCustomEntriesAndHandlesErrors) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();
  const absl::Cleanup restore_state = [] {
    coda_leap_second_table_done();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Missing file fails with CODA_ERROR_FILE_OPEN.
  TempFile missing_table("coda_time_missing_tai_utc.dat");
  restore_leap.Set(missing_table.c_str());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_leap_second_table_init());
  EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);

  // 2. Non-80-character line fails with CODA_ERROR_FILE_READ (also via
  //    coda_init()).
  TempFile bad_len_table("coda_time_bad_len_tai_utc.dat");
  ASSERT_TRUE(bad_len_table.WriteText("too short\n"));
  restore_leap.Set(bad_len_table.c_str());
  coda_set_error(CODA_SUCCESS, nullptr);
  const int bad_init_rc = coda_init();
  if (bad_init_rc == CODA_SUCCESS) {
    coda_done();
  }
  EXPECT_EQ(-1, bad_init_rc);
  EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
  EXPECT_THAT(coda_errno_to_string(CODA_ERROR_FILE_READ),
              HasSubstr(bad_len_table.string()));

  // 3. Valid custom table (> BLOCK_SIZE=16 entries to exercise realloc,
  //    pre-1972 skipped entries, and an out-of-order entry for sorted insert).
  std::string table_content;
  const std::string pre_1972 =
      MakeTaiUtcLine(1972, "JAN", 1, 2441317.5, 10.0, 41317.0);
  ASSERT_EQ(80U, pre_1972.size());
  absl::StrAppend(&table_content, pre_1972, "\n");

  // Insert 2025 out-of-order before the 1972-1989 loop entries: on initial
  // registration (table empty) it takes offset 10, then subsequent sorted
  // insertions increment every entry later than the newly inserted one.
  const std::string future_2025 =
      MakeTaiUtcLine(2025, "JAN", 1, 2460676.5, 29.0, 60676.0);
  ASSERT_EQ(80U, future_2025.size());
  absl::StrAppend(&table_content, future_2025, "\n");

  constexpr double kStartJd = 2441499.5;  // 1972-07-01
  for (int i = 0; i < 18; ++i) {
    const std::string line =
        MakeTaiUtcLine(1972 + i, "JUL", 1, kStartJd + i * 365.0, 11.0 + i,
                       41499.0 + i * 365.0);
    ASSERT_EQ(80U, line.size());
    absl::StrAppend(&table_content, line, "\n");
  }

  TempFile custom_table("coda_time_custom_tai_utc.dat");
  ASSERT_TRUE(custom_table.WriteText(table_content));
  restore_leap.Set(custom_table.c_str());
  ASSERT_EQ(CODA_SUCCESS, coda_leap_second_table_init());

  // 18 chronological entries + 1 future entry -> 19 leap seconds accumulated by
  // 2025-01-01 (TAI-UTC = 10 + 19 = 29).
  double tai_2025 = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_parts_to_double_utc(2025, 1, 1, 0, 0, 0, 0, &tai_2025));
  double unadjusted_2025 = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(2025, 1, 1, 0, 0, 0, 0,
                                                    &unadjusted_2025));
  EXPECT_DOUBLE_EQ(unadjusted_2025 + 29.0, tai_2025);
}

TEST(CodaTimeTest, PartsToStringFormatsSpecifiersLiteralsAndErrors) {
  // Zero-initialize output buffer because coda_time_parts_to_string() does not
  // append a trailing '\0' if the format ends with a literal character.
  std::array<char, 64> out{};

  // Standard ISO-like format with fractional seconds.
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_string(
                              2005, 4, 3, 2, 1, 60, 543210,
                              "yyyy-MM-dd'T'HH:mm:ss.SSSSSS", out.data()));
  EXPECT_STREQ("2005-04-03T02:01:60.543210", out.data());

  // Fractional widths < 6 (truncates) and > 6 (zero-pads), plus DDD and '|'.
  out.fill('\0');
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_string(
                              2004, 3, 1, 9, 8, 7, 543210,
                              "yyyy-DDD S SSS SSSSSSSS|ignored", out.data()));
  EXPECT_STREQ("2004-061 5 543 00543210", out.data());

  // Quoted literals, escaped quotes (''), and quoted '|' inside a literal.
  out.fill('\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_parts_to_string(2005, 4, 3, 2, 1, 0, 0,
                                      "'a''b|c'yyyy''MM", out.data()));
  EXPECT_STREQ("a'b|c2005'04", out.data());

  // Pin MMM output for month < 12 (see file header caveat on month_name[month]
  // off-by-one in coda-time.c).
  out.fill('\0');
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_string(2005, 3, 15, 0, 0, 0, 0,
                                                    "dd-MMM-yyyy", out.data()));
  EXPECT_STREQ("15-APR-2005", out.data());

  // Error cases.
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(-1, coda_time_parts_to_string(2005, 1, 1, 0, 0, 0, 0, nullptr,
                                          out.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_parts_to_string(2005, 1, 1, 0, 0, 0, 0, "yyyy", nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  struct BadFormatOrRangeCase {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int musec;
    const char* format;
    int expected_errno;
  };
  constexpr BadFormatOrRangeCase kErrorCases[] = {
      {-1, 1, 1, 0, 0, 0, 0, "yyyy", CODA_ERROR_INVALID_DATETIME},
      {10000, 1, 1, 0, 0, 0, 0, "yyyy", CODA_ERROR_INVALID_DATETIME},
      {2005, 0, 1, 0, 0, 0, 0, "MM", CODA_ERROR_INVALID_DATETIME},
      {2005, 13, 1, 0, 0, 0, 0, "MM", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 0, 0, 0, 0, 0, "dd", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 32, 0, 0, 0, 0, "dd", CODA_ERROR_INVALID_DATETIME},
      {0, 1, 1, 0, 0, 0, 0, "DDD", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, -1, 0, 0, 0, "HH", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 24, 0, 0, 0, "HH", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, -1, 0, 0, "mm", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, 60, 0, 0, "mm", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, 0, -1, 0, "ss", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, 0, 61, 0, "ss", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, 0, 0, -1, "S", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, 0, 0, 1000000, "S", CODA_ERROR_INVALID_DATETIME},
      {2005, 1, 1, 0, 0, 0, 0, "X", CODA_ERROR_INVALID_FORMAT},
      {2005, 1, 1, 0, 0, 0, 0, "q", CODA_ERROR_INVALID_FORMAT},
      {2005, 1, 1, 0, 0, 0, 0, "'unclosed", CODA_ERROR_INVALID_FORMAT},
  };

  for (const BadFormatOrRangeCase& tc : kErrorCases) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_time_parts_to_string(tc.year, tc.month, tc.day, tc.hour,
                                            tc.minute, tc.second, tc.musec,
                                            tc.format, out.data()))
        << "format=" << tc.format;
    EXPECT_EQ(tc.expected_errno, coda_errno) << "format=" << tc.format;
  }
}

TEST(CodaTimeTest, StringToPartsParsesAllSpecifiersFallbacksAndLeadingSpaces) {
  int y = 0;
  int m = 0;
  int d = 0;
  int h = 0;
  int min = 0;
  int s = 0;
  int us = 0;

  // 1. Default epoch initialization for omitted fields ("yyyy" leaves Jan 1
  //    00:00:00.000000).
  ASSERT_EQ(CODA_SUCCESS, coda_time_string_to_parts("yyyy", "2024", &y, &m, &d,
                                                    &h, &min, &s, &us));
  EXPECT_EQ(2024, y);
  EXPECT_EQ(1, m);
  EXPECT_EQ(1, d);
  EXPECT_EQ(0, h);
  EXPECT_EQ(0, min);
  EXPECT_EQ(0, s);
  EXPECT_EQ(0, us);

  // 2. Leading-space modifiers (MM*, dd*, HH*, mm*, ss*), quoted literals,
  //    escaped quotes (''), and sub-microsecond S (n < 6).
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_string_to_parts("yyyy-MM*-dd* 'a''b'HH*:mm*:ss*.SSS",
                                      "2005- 4- 3 a'b 2: 1: 9.123", &y, &m, &d,
                                      &h, &min, &s, &us));
  EXPECT_EQ(2005, y);
  EXPECT_EQ(4, m);
  EXPECT_EQ(3, d);
  EXPECT_EQ(2, h);
  EXPECT_EQ(1, min);
  EXPECT_EQ(9, s);
  EXPECT_EQ(123000, us);

  // 3. DDD* (with leading spaces) and SSSSSSSS (n > 6 digits).
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_string_to_parts("yyyy-DDD* HH:mm:ss.SSSSSSSS",
                                      "2004-  5 12:34:56.65432199", &y, &m, &d,
                                      &h, &min, &s, &us));
  EXPECT_EQ(2004, y);
  EXPECT_EQ(1, m);
  EXPECT_EQ(5, d);
  EXPECT_EQ(12, h);
  EXPECT_EQ(34, min);
  EXPECT_EQ(56, s);
  EXPECT_EQ(654321, us);

  // 4. MMM specifier and '|' multi-format fallback (including quoted '|' in
  //    both the first and a subsequent alternative).
  ASSERT_EQ(CODA_SUCCESS, coda_time_string_to_parts(
                              "yyyy-MM-dd|dd-MMM-yyyy'|'HH", "15-dEc-2005|07",
                              &y, &m, &d, &h, &min, &s, &us));
  EXPECT_EQ(2005, y);
  EXPECT_EQ(12, m);
  EXPECT_EQ(15, d);
  EXPECT_EQ(7, h);

  ASSERT_EQ(CODA_SUCCESS,
            coda_time_string_to_parts("yyyy'|'MM|yyyy/MM", "2005|04", &y, &m,
                                      &d, &h, &min, &s, &us));
  EXPECT_EQ(2005, y);
  EXPECT_EQ(4, m);
}

TEST(CodaTimeTest, StringToPartsRejectsNullsInvalidFormatsAndBadStrings) {
  int y = 0;
  int m = 0;
  int d = 0;
  int h = 0;
  int min = 0;
  int s = 0;
  int us = 0;

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Null pointer checks.
  EXPECT_EQ(-1, coda_time_string_to_parts(nullptr, "2000", &y, &m, &d, &h, &min,
                                          &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", nullptr, &y, &m, &d, &h, &min,
                                          &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", nullptr, &m, &d, &h,
                                          &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", &y, nullptr, &d, &h,
                                          &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", &y, &m, nullptr, &h,
                                          &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", &y, &m, &d, nullptr,
                                          &min, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", &y, &m, &d, &h,
                                          nullptr, &s, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", &y, &m, &d, &h, &min,
                                          nullptr, &us));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_parts("yyyy", "2000", &y, &m, &d, &h, &min,
                                          &s, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  struct BadParseCase {
    const char* format;
    const char* str;
    int expected_errno;
  };
  constexpr BadParseCase kBadParseCases[] = {
      {"'abc'yyyy", "abX2000", CODA_ERROR_INVALID_DATETIME},
      {"yyyy-MM", "2000/01", CODA_ERROR_INVALID_DATETIME},
      {"yyyy", "200/", CODA_ERROR_INVALID_DATETIME},
      {"yyyy", "200:", CODA_ERROR_INVALID_DATETIME},
      // string_to_parts() checks format[format_index] == '*' instead of +4,
      // leaving '*' unconsumed to fail as CODA_ERROR_INVALID_FORMAT.
      {"yyyy*", " 200", CODA_ERROR_INVALID_FORMAT},
      {"yyyy*", "200a", CODA_ERROR_INVALID_DATETIME},
      {"MMM", "XYZ", CODA_ERROR_INVALID_DATETIME},
      {"MM", "1a", CODA_ERROR_INVALID_DATETIME},
      {"dd", "1a", CODA_ERROR_INVALID_DATETIME},
      {"DDD", "12a", CODA_ERROR_INVALID_DATETIME},
      {"DDD", "400", CODA_ERROR_INVALID_DATETIME},
      {"yyyy-DDD", "0000-001", CODA_ERROR_INVALID_DATETIME},
      {"HH", "1a", CODA_ERROR_INVALID_DATETIME},
      {"mm", "1a", CODA_ERROR_INVALID_DATETIME},
      {"ss", "1a", CODA_ERROR_INVALID_DATETIME},
      {"SSS", "12a", CODA_ERROR_INVALID_DATETIME},
      {"X", "1", CODA_ERROR_INVALID_FORMAT},
      {"q", "1", CODA_ERROR_INVALID_FORMAT},
      {"'unclosed", "unclosed", CODA_ERROR_INVALID_FORMAT},
      {"yyyy", "2000extra", CODA_ERROR_INVALID_DATETIME},
      {"yyyy-MM|dd/MM", "not-a-date", CODA_ERROR_INVALID_DATETIME},
  };

  for (const BadParseCase& tc : kBadParseCases) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_time_string_to_parts(tc.format, tc.str, &y, &m, &d, &h,
                                            &min, &s, &us))
        << "format=" << tc.format << " str=" << tc.str;
    EXPECT_EQ(tc.expected_errno, coda_errno)
        << "format=" << tc.format << " str=" << tc.str;
  }
}

TEST(CodaTimeTest, DoubleStringRoundTripsNonUtcAndUtc) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::array<char, 64> out{};
  double dt = 0.0;

  // Non-UTC string <-> double.
  ASSERT_EQ(CODA_SUCCESS, coda_time_double_to_string(
                              86400.5, "yyyy-MM-dd HH:mm:ss.SSS", out.data()));
  EXPECT_STREQ("2000-01-02 00:00:00.500", out.data());

  ASSERT_EQ(CODA_SUCCESS, coda_time_string_to_double("yyyy-MM-dd HH:mm:ss.SSS",
                                                     out.data(), &dt));
  EXPECT_DOUBLE_EQ(86400.5, dt);

  // UTC string <-> TAI double across 2008-12-31 leap second.
  out.fill('\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_time_double_to_string_utc(
                284083233.25, "yyyy-MM-dd HH:mm:ss.SS", out.data()));
  EXPECT_STREQ("2008-12-31 23:59:60.25", out.data());

  ASSERT_EQ(CODA_SUCCESS, coda_time_string_to_double_utc(
                              "yyyy-MM-dd HH:mm:ss.SS", out.data(), &dt));
  EXPECT_DOUBLE_EQ(284083233.25, dt);

  // Error propagation in both first and second stages.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_string(coda_NaN(), "yyyy", out.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_string(0.0, "X", out.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_string_utc(coda_NaN(), "yyyy", out.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_double_to_string_utc(0.0, "X", out.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_double("yyyy", "bad", &dt));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_double("yyyy-MM-dd", "2001-02-29", &dt));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_time_string_to_double_utc("yyyy", "bad", &dt));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_time_string_to_double_utc("yyyy-MM-dd", "2001-02-29", &dt));
  EXPECT_EQ(CODA_ERROR_INVALID_DATETIME, coda_errno);
}

TEST(CodaTimeTest, DeprecatedBackwardCompatibilityWrappersWork) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Non-UTC deprecated wrappers.
  double dt = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_datetime_to_double(2005, 4, 3, 2, 1, 0, 123456, &dt));

  int y = 0;
  int m = 0;
  int d = 0;
  int h = 0;
  int min = 0;
  int s = 0;
  int us = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_double_to_datetime(dt, &y, &m, &d, &h, &min, &s, &us));
  EXPECT_EQ(2005, y);
  EXPECT_EQ(4, m);
  EXPECT_EQ(3, d);
  EXPECT_EQ(2, h);
  EXPECT_EQ(1, min);
  EXPECT_EQ(0, s);
  EXPECT_EQ(123456, us);

  std::array<char, 32> str{};
  ASSERT_EQ(CODA_SUCCESS, coda_time_to_string(dt, str.data()));
  EXPECT_STREQ("2005-04-03 02:01:00.123456", str.data());

  double parsed_dt = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_string_to_time(str.data(), &parsed_dt));
  EXPECT_DOUBLE_EQ(dt, parsed_dt);
  ASSERT_EQ(CODA_SUCCESS, coda_string_to_time("03-Apr-2005", &parsed_dt));
  EXPECT_DOUBLE_EQ(1919.0 * kSecondsPerDay, parsed_dt);

  // 2. UTC deprecated wrappers.
  double tai = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_utcdatetime_to_double(2008, 12, 31, 23, 59, 60, 500000, &tai));
  EXPECT_DOUBLE_EQ(284083233.5, tai);

  ASSERT_EQ(CODA_SUCCESS,
            coda_double_to_utcdatetime(tai, &y, &m, &d, &h, &min, &s, &us));
  EXPECT_EQ(2008, y);
  EXPECT_EQ(12, m);
  EXPECT_EQ(31, d);
  EXPECT_EQ(23, h);
  EXPECT_EQ(59, min);
  EXPECT_EQ(60, s);
  EXPECT_EQ(500000, us);

  str.fill('\0');
  ASSERT_EQ(CODA_SUCCESS, coda_time_to_utcstring(tai, str.data()));
  EXPECT_STREQ("2008-12-31 23:59:60.500000", str.data());

  double parsed_tai = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_utcstring_to_time(str.data(), &parsed_tai));
  EXPECT_DOUBLE_EQ(tai, parsed_tai);
  ASSERT_EQ(CODA_SUCCESS, coda_utcstring_to_time("01-Jan-2009", &parsed_tai));
  EXPECT_DOUBLE_EQ(284083234.0, parsed_tai);
}

// TODO(schwehr): 97.4% line coverage in libcoda/coda-time.c (755/775);
// remaining 20 missed lines are malloc/realloc OOM checks and defensive
// leap-second table allocation guards.

}  // namespace
}  // namespace stcorp_coda
