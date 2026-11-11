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

// Unit tests for //third_party/netcdf/libdispatch/nctime.c.
//
// Covers epoch/human time conversions (Cdh2e, Cde2h), relative unit parsing
// (cdParseRelunits), component time string parsing (cdChar2Comp), ISO-8601
// formatting (cdRel2Iso) across standard, Julian, no-leap, 360/366-day,
// mixed Julian/Gregorian, and climatological calendars, and error-option
// handling (cdSetErrOpts).
//
// Note on compiled-in coverage ceiling: upstream libdispatch/nctime.c retains
// static helper functions (CdDivDelTime, cdDiffMixed, cdFromHours,
// cdComp2RelMixed, and unused unit switch arms in cdComp2Rel/CdAddDelTime)
// from the LLNL cdtime extraction that are not reachable through any of the
// six exported functions declared in include/nctime.h.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/nctime.c

#include <unistd.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

// include/nctime.h lacks an extern "C" guard; pre-include prelude above first.
// clang-format off
extern "C" {
#include "third_party/netcdf/include/nctime.h"
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

class ScopedCdErrOpts {
 public:
  explicit ScopedCdErrOpts(int opts) : prev_(cdSetErrOpts(opts)) {}
  ~ScopedCdErrOpts() { cdSetErrOpts(prev_); }

  ScopedCdErrOpts(const ScopedCdErrOpts&) = delete;
  ScopedCdErrOpts& operator=(const ScopedCdErrOpts&) = delete;

  int previous() const { return prev_; }

 private:
  int prev_;
};

class ScopedDeathTestStyle {
 public:
  explicit ScopedDeathTestStyle(std::string_view style)
      : prev_(GTEST_FLAG_GET(death_test_style)) {
    GTEST_FLAG_SET(death_test_style, std::string(style));
  }
  ~ScopedDeathTestStyle() { GTEST_FLAG_SET(death_test_style, prev_); }

  ScopedDeathTestStyle(const ScopedDeathTestStyle&) = delete;
  ScopedDeathTestStyle& operator=(const ScopedDeathTestStyle&) = delete;

 private:
  std::string prev_;
};

std::string Rel2Iso(cdCalenType calendar, std::string relunits, int separator,
                    double reltime) {
  char buf[64]{};
  cdRel2Iso(calendar, relunits.data(), separator, reltime, buf, sizeof(buf));
  return std::string(buf);
}

TEST(NctimeTest, EpochAndHumanConversionsHandleStandardAndLeapCalendars) {
  // Base epoch 1970-01-01T00:00 in CdChron maps to etime == 0.0.
  CdTime htime = {1970L, 1, 1, 0.0, 1970L, CdChron};
  double etime = -1.0;
  Cdh2e(&htime, &etime);
  EXPECT_DOUBLE_EQ(0.0, etime);

  CdTime out{};
  Cde2h(0.0, CdChron, 1970L, &out);
  EXPECT_EQ(1970L, out.year);
  EXPECT_EQ(1, out.month);
  EXPECT_EQ(1, out.day);
  EXPECT_DOUBLE_EQ(0.0, out.hour);
  EXPECT_EQ(1970L, out.baseYear);
  EXPECT_EQ(CdChron, out.timeType);

  // Positive multi-year conversion across leap year 1972.
  htime = {1972L, 3, 1, 12.5, 1970L, CdChron};
  Cdh2e(&htime, &etime);
  // 1970 (365) + 1971 (365) + Jan 1972 (31) + Feb 1972 (29) = 790 days.
  EXPECT_DOUBLE_EQ(790.0 * 24.0 + 12.5, etime);
  Cde2h(etime, CdChron, 1970L, &out);
  EXPECT_EQ(1972L, out.year);
  EXPECT_EQ(3, out.month);
  EXPECT_EQ(1, out.day);
  EXPECT_DOUBLE_EQ(12.5, out.hour);

  // Gregorian century rule: 1900 is not a leap year, 2000 is a leap year.
  CdTime feb28_1900 = {1900L, 2, 28, 0.0, 1970L, CdChron};
  CdTime mar01_1900 = {1900L, 3, 1, 0.0, 1970L, CdChron};
  double e_feb28_1900 = 0.0;
  double e_mar01_1900 = 0.0;
  Cdh2e(&feb28_1900, &e_feb28_1900);
  Cdh2e(&mar01_1900, &e_mar01_1900);
  EXPECT_DOUBLE_EQ(24.0, e_mar01_1900 - e_feb28_1900);

  CdTime feb28_2000 = {2000L, 2, 28, 0.0, 1970L, CdChron};
  CdTime mar01_2000 = {2000L, 3, 1, 0.0, 1970L, CdChron};
  double e_feb28_2000 = 0.0;
  double e_mar01_2000 = 0.0;
  Cdh2e(&feb28_2000, &e_feb28_2000);
  Cdh2e(&mar01_2000, &e_mar01_2000);
  EXPECT_DOUBLE_EQ(48.0, e_mar01_2000 - e_feb28_2000);

  // Negative epochal time before 1970 (both Cde2h and Cdh2e year < baseYear).
  CdTime dec31_1969 = {1969L, 12, 31, 18.0, 1970L, CdChron};
  Cdh2e(&dec31_1969, &etime);
  EXPECT_DOUBLE_EQ(-6.0, etime);

  Cde2h(-6.0, CdChron, 1970L, &out);
  EXPECT_EQ(1969L, out.year);
  EXPECT_EQ(12, out.month);
  EXPECT_EQ(31, out.day);
  EXPECT_DOUBLE_EQ(18.0, out.hour);

  // Tiny negative etime triggers the hour >= 24.0 floor correction path.
  Cde2h(-1.0e-16, CdChron, 1970L, &out);
  EXPECT_EQ(1970L, out.year);
  EXPECT_EQ(1, out.month);
  EXPECT_EQ(1, out.day);
  EXPECT_DOUBLE_EQ(0.0, out.hour);
}

TEST(NctimeTest, EpochAndHumanConversionsCoverAllTimeTypes) {
  CdTime out{};
  double etime = 0.0;

  // CdJulianCal treats 1900 as a leap year.
  CdTime julian_1900 = {1900L, 2, 29, 0.0, 1970L, CdJulianCal};
  Cdh2e(&julian_1900, &etime);
  Cde2h(etime, CdJulianCal, 1970L, &out);
  EXPECT_EQ(1900L, out.year);
  EXPECT_EQ(2, out.month);
  EXPECT_EQ(29, out.day);

  // CdChronNoLeap: 2000 has 365 days; March 1 is 1 day after Feb 28.
  CdTime nl_feb28 = {2000L, 2, 28, 0.0, 1970L, CdChronNoLeap};
  CdTime nl_mar01 = {2000L, 3, 1, 0.0, 1970L, CdChronNoLeap};
  double e_nl1 = 0.0;
  double e_nl2 = 0.0;
  Cdh2e(&nl_feb28, &e_nl1);
  Cdh2e(&nl_mar01, &e_nl2);
  EXPECT_DOUBLE_EQ(24.0, e_nl2 - e_nl1);
  Cde2h(e_nl2, CdChronNoLeap, 1970L, &out);
  EXPECT_EQ(2000L, out.year);
  EXPECT_EQ(3, out.month);
  EXPECT_EQ(1, out.day);

  // CdChron360: 30 days per month, 360 days per year.
  CdTime c360 = {1971L, 2, 30, 6.0, 1970L, CdChron360};
  Cdh2e(&c360, &etime);
  EXPECT_DOUBLE_EQ((360.0 + 59.0) * 24.0 + 6.0, etime);
  Cde2h(etime, CdChron360, 1970L, &out);
  EXPECT_EQ(1971L, out.year);
  EXPECT_EQ(2, out.month);
  EXPECT_EQ(30, out.day);
  EXPECT_DOUBLE_EQ(6.0, out.hour);

  // CdChron366: every year has 366 days, including 1971.
  CdTime c366 = {1971L, 2, 29, 0.0, 1970L, CdChron366};
  Cdh2e(&c366, &etime);
  EXPECT_DOUBLE_EQ((366.0 + 31.0 + 28.0) * 24.0, etime);
  Cde2h(etime, CdChron366, 1970L, &out);
  EXPECT_EQ(1971L, out.year);
  EXPECT_EQ(2, out.month);
  EXPECT_EQ(29, out.day);

  // Relative time types (!CdBase1970): year is relative to baseYear.
  CdTime rel = {4L, 3, 1, 0.0, 2000L, CdRel};
  Cdh2e(&rel, &etime);
  // 2000 (366) + 2001..2003 (3*365) + Jan (31) + Feb 2004 (29) = 1521 days.
  EXPECT_DOUBLE_EQ(1521.0 * 24.0, etime);
  Cde2h(etime, CdRel, 2000L, &out);
  EXPECT_EQ(4L, out.year);
  EXPECT_EQ(2000L, out.baseYear);
  EXPECT_EQ(3, out.month);
  EXPECT_EQ(1, out.day);

  // Negative relative time before baseYear (both Cdh2e and Cde2h).
  CdTime neg_rel = {-1L, 12, 31, 0.0, 2000L, CdRelNoLeap};
  Cdh2e(&neg_rel, &etime);
  EXPECT_DOUBLE_EQ(-24.0, etime);

  Cde2h(-24.0, CdRelNoLeap, 2000L, &out);
  EXPECT_EQ(-1L, out.year);
  EXPECT_EQ(12, out.month);
  EXPECT_EQ(31, out.day);

  // Climatological time types (!CdChronCal): year and baseYear become 0.
  CdTime clim = {1999L, 3, 1, 0.0, 1999L, CdClim};
  Cdh2e(&clim, &etime);
  EXPECT_DOUBLE_EQ(59.0 * 24.0, etime);
  Cde2h(-24.0, CdClim, 1999L, &out);
  EXPECT_EQ(0L, out.year);
  EXPECT_EQ(0L, out.baseYear);
  EXPECT_EQ(12, out.month);
  EXPECT_EQ(31, out.day);

  // CdClimLeap treats year 0 as leap (year 0 % 400 == 0).
  CdTime clim_leap = {0L, 3, 1, 0.0, 0L, CdClimLeap};
  Cdh2e(&clim_leap, &etime);
  EXPECT_DOUBLE_EQ(60.0 * 24.0, etime);
  Cde2h(59.0 * 24.0, CdClimLeap, 0L, &out);
  EXPECT_EQ(2, out.month);
  EXPECT_EQ(29, out.day);

  // CdClim360 round-trips cleanly.
  CdTime clim_360 = {0L, 4, 10, 3.0, 0L, CdClim360};
  Cdh2e(&clim_360, &etime);
  Cde2h(etime, CdClim360, 0L, &out);
  EXPECT_EQ(4, out.month);
  EXPECT_EQ(10, out.day);
  EXPECT_DOUBLE_EQ(3.0, out.hour);
}

TEST(NctimeTest, InvalidMonthInDayOfYearTriggersErrorAndFallsBackToJanuary) {
  ScopedCdErrOpts silent(0);
  CdTime bad_low = {1970L, 0, 5, 0.0, 1970L, CdChron};
  double etime = -1.0;
  Cdh2e(&bad_low, &etime);
  EXPECT_DOUBLE_EQ(4.0 * 24.0, etime);

  CdTime bad_high = {1970L, 13, 2, 0.0, 1970L, CdChron};
  Cdh2e(&bad_high, &etime);
  EXPECT_DOUBLE_EQ(1.0 * 24.0, etime);
}

TEST(NctimeTest, ParseRelunitsSupportsAllUnitsSeparatorsAndErrors) {
  ScopedCdErrOpts silent(0);

  struct UnitCase {
    const char* input;
    cdCalenType calendar;
    cdUnitTime expected_unit;
    long expected_year;
    short expected_month;
    short expected_day;
    double expected_hour;
  };
  constexpr UnitCase kValidCases[] = {
      {"seconds since 2000-01-02 03:04:05", cdStandard, cdSecond, 2000L, 1, 2,
       3.0 + 4.0 / 60.0 + 5.0 / 3600.0},
      {"sec since 2000-01-02T06:30:00", cdStandard, cdSecond, 2000L, 1, 2, 6.5},
      {"s since 2001-05-06", cdStandard, cdSecond, 2001L, 5, 6, 0.0},
      {"minutes since 1995-07-08", cdStandard, cdMinute, 1995L, 7, 8, 0.0},
      {"mn since 1995-07-08", cdStandard, cdMinute, 1995L, 7, 8, 0.0},
      {"HOURS since 1980-02-03", cdStandard, cdHour, 1980L, 2, 3, 0.0},
      {"hr", cdStandard, cdHour, 1979L, 1, 1, 0.0},
      {"days since 2010-11-12", cdStandard, cdDay, 2010L, 11, 12, 0.0},
      {"dy since 2010-11-12", cdStandard, cdDay, 2010L, 11, 12, 0.0},
      {"weeks since 2012-01-01", cdStandard, cdWeek, 2012L, 1, 1, 0.0},
      {"wk since 2012-01-01", cdStandard, cdWeek, 2012L, 1, 1, 0.0},
      {"months since 2015-04-01", cdStandard, cdMonth, 2015L, 4, 1, 0.0},
      {"mo since 2015-04-01", cdStandard, cdMonth, 2015L, 4, 1, 0.0},
      {"seasons since 2015-04-01", cdStandard, cdSeason, 2015L, 4, 1, 0.0},
      {"years since 1950-01-01", cdStandard, cdYear, 1950L, 1, 1, 0.0},
      {"yr since 1950-01-01", cdStandard, cdYear, 1950L, 1, 1, 0.0},
      {"hours", cdClim, cdHour, 0L, 1, 1, 0.0},
      {"months since 06-15", cdClim, cdMonth, 0L, 6, 15, 0.0},
  };

  for (const UnitCase& tc : kValidCases) {
    SCOPED_TRACE(tc.input);
    std::string buf(tc.input);
    cdUnitTime unit = cdBadUnit;
    cdCompTime base{};
    ASSERT_EQ(0, cdParseRelunits(tc.calendar, buf.data(), &unit, &base));
    EXPECT_EQ(tc.expected_unit, unit);
    EXPECT_EQ(tc.expected_year, base.year);
    EXPECT_EQ(tc.expected_month, base.month);
    EXPECT_EQ(tc.expected_day, base.day);
    EXPECT_NEAR(tc.expected_hour, base.hour, 1e-12);
  }

  // Error cases return 1 (including > CD_MAX_RELUNITS token for cdTrim cap).
  const char* kInvalidCases[] = {
      "",
      "   ",
      "fortnights since 2000-01-01",
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx "
      "since 2000-01-01",
  };
  for (const char* invalid : kInvalidCases) {
    SCOPED_TRACE(invalid);
    std::string buf(invalid);
    cdUnitTime unit = cdBadUnit;
    cdCompTime base{};
    EXPECT_EQ(1, cdParseRelunits(cdStandard, buf.data(), &unit, &base));
  }

  // Climatological calendars reject "years".
  {
    std::string buf = "years since 01-01";
    cdUnitTime unit = cdBadUnit;
    cdCompTime base{};
    EXPECT_EQ(1, cdParseRelunits(cdClim, buf.data(), &unit, &base));
  }
}

TEST(NctimeTest, Char2CompParsesStandardAndClimatologicalStringsAndErrors) {
  ScopedCdErrOpts silent(0);

  cdCompTime ct{};
  std::string s = "2024";
  cdChar2Comp(cdStandard, s.data(), &ct);
  EXPECT_EQ(2024L, ct.year);
  EXPECT_EQ(1, ct.month);
  EXPECT_EQ(1, ct.day);
  EXPECT_DOUBLE_EQ(0.0, ct.hour);

  s = "2024-02";
  cdChar2Comp(cdStandard, s.data(), &ct);
  EXPECT_EQ(2024L, ct.year);
  EXPECT_EQ(2, ct.month);
  EXPECT_EQ(1, ct.day);

  s = "2024-02-29 14";
  cdChar2Comp(cdStandard, s.data(), &ct);
  EXPECT_EQ(29, ct.day);
  EXPECT_DOUBLE_EQ(14.0, ct.hour);

  s = "2024-02-29 14:30";
  cdChar2Comp(cdStandard, s.data(), &ct);
  EXPECT_DOUBLE_EQ(14.5, ct.hour);

  s = "2024-02-29 14:30:15.5";
  cdChar2Comp(cdStandard, s.data(), &ct);
  EXPECT_EQ(2024L, ct.year);
  EXPECT_EQ(2, ct.month);
  EXPECT_EQ(29, ct.day);
  EXPECT_NEAR(14.5 + 15.5 / 3600.0, ct.hour, 1e-12);

  // Climatological parsing (month-day HH:MM:SS).
  s = "07";
  cdChar2Comp(cdClim, s.data(), &ct);
  EXPECT_EQ(0L, ct.year);
  EXPECT_EQ(7, ct.month);
  EXPECT_EQ(1, ct.day);
  EXPECT_DOUBLE_EQ(0.0, ct.hour);

  s = "07-15";
  cdChar2Comp(cdClim, s.data(), &ct);
  EXPECT_EQ(7, ct.month);
  EXPECT_EQ(15, ct.day);
  EXPECT_DOUBLE_EQ(0.0, ct.hour);

  s = "07-15 06";
  cdChar2Comp(cdClim, s.data(), &ct);
  EXPECT_DOUBLE_EQ(6.0, ct.hour);

  s = "07-15 06:15";
  cdChar2Comp(cdClim, s.data(), &ct);
  EXPECT_DOUBLE_EQ(6.25, ct.hour);

  s = "07-15 06:15:30";
  cdChar2Comp(cdClim, s.data(), &ct);
  EXPECT_EQ(7, ct.month);
  EXPECT_EQ(15, ct.day);
  EXPECT_NEAR(6.25 + 30.0 / 3600.0, ct.hour, 1e-12);

  // Negative/invalid parse strings hit all validation branches cleanly.
  const char* kBadStandard[] = {
      "",
      "not-a-date",
      "2000-00-01",
      "2000-13-01",
      "2000-01-00",
      "2000-01-32",
      "2000-01-01 -1:00:00",
      "2000-01-01 24:00:00",
      "2000-01-01 12:-1:00",
      "2000-01-01 12:60:00",
      "2000-01-01 12:00:-0.5",
      "2000-01-01 12:00:60.5",
  };
  for (const char* bad : kBadStandard) {
    SCOPED_TRACE(bad);
    std::string buf(bad);
    cdChar2Comp(cdStandard, buf.data(), &ct);
  }

  const char* kBadClim[] = {
      "",
      "invalid",
      "00-01",
      "13-01",
      "01-00",
      "01-32",
      "01-01 -1:00:00",
      "01-01 24:00:00",
      "01-01 12:-1:00",
      "01-01 12:60:00",
      "01-01 12:00:-0.5",
      "01-01 12:00:60.5",
  };
  for (const char* bad : kBadClim) {
    SCOPED_TRACE(bad);
    std::string buf(bad);
    cdChar2Comp(cdClim, buf.data(), &ct);
  }
}

TEST(NctimeTest, Rel2IsoFormatsAllPrecisionsAndUnits) {
  // Exercise all 5 nskip formatting branches (0..4) for cdStandard.
  EXPECT_EQ("2000-01-01",
            Rel2Iso(cdStandard, "hours since 2000-01-01", 'T', 0.0));
  EXPECT_EQ("2000-01-01T05",
            Rel2Iso(cdStandard, "hours since 2000-01-01", 'T', 5.0));
  EXPECT_EQ("2000-01-01T05:30",
            Rel2Iso(cdStandard, "hours since 2000-01-01", 'T', 5.5));
  EXPECT_EQ("2000-01-01 05:30:15",
            Rel2Iso(cdStandard, "seconds since 2000-01-01 00:00:00", ' ',
                    5.0 * 3600.0 + 30.0 * 60.0 + 15.0));
  // Upstream cdComp2Iso formats fractional seconds (nskip == 0) with bare %lf
  // without zero-padding, so 0.25 s renders as "0.250000".
  EXPECT_EQ(
      "2000-01-01T00:00:0.250000",
      Rel2Iso(cdStandard, "seconds since 2000-01-01T00:00:00", 'T', 0.25));

  // Exercise all 5 nskip formatting branches (0..4) for cdClim.
  EXPECT_EQ("01-01", Rel2Iso(cdClim, "hours since 01-01", 'T', 0.0));
  EXPECT_EQ("01-01T05", Rel2Iso(cdClim, "hours since 01-01", 'T', 5.0));
  EXPECT_EQ("01-01T05:30", Rel2Iso(cdClim, "hours since 01-01", 'T', 5.5));
  EXPECT_EQ("01-01 05:30:15", Rel2Iso(cdClim, "seconds since 01-01 00:00:00",
                                      ' ', 5.0 * 3600.0 + 30.0 * 60.0 + 15.0));
  EXPECT_EQ("01-01T00:00:0.250000",
            Rel2Iso(cdClim, "seconds since 01-01T00:00:00", 'T', 0.25));

  // Exercise minutes, weeks, months, seasons, and years (positive, zero,
  // negative).
  EXPECT_EQ("2000-01-01T01:30",
            Rel2Iso(cdStandard, "minutes since 2000-01-01", 'T', 90.0));
  EXPECT_EQ("2000-01-15",
            Rel2Iso(cdStandard, "weeks since 2000-01-01", 'T', 2.0));
  EXPECT_EQ("2000-01-15",
            Rel2Iso(cdStandard, "months since 2000-01-15", 'T', 0.0));
  EXPECT_EQ("2000-04-01",
            Rel2Iso(cdStandard, "months since 2000-01-01", 'T', 3.0));
  EXPECT_EQ("1999-10-01",
            Rel2Iso(cdStandard, "months since 2000-03-01", 'T', -5.0));
  EXPECT_EQ("1999-12-01",
            Rel2Iso(cdStandard, "months since 2000-01-01", 'T', -1.0));
  EXPECT_EQ("2000-07-01",
            Rel2Iso(cdStandard, "seasons since 2000-01-01", 'T', 2.0));
  EXPECT_EQ("1999-10-01",
            Rel2Iso(cdStandard, "seasons since 2000-01-01", 'T', -1.0));
  EXPECT_EQ("2003-01-01",
            Rel2Iso(cdStandard, "years since 2000-01-01", 'T', 3.0));
  EXPECT_EQ("1998-01-01",
            Rel2Iso(cdStandard, "years since 2000-01-01", 'T', -2.0));

  // Climatological month/season arithmetic and implicit base units.
  EXPECT_EQ("02-01", Rel2Iso(cdClim, "months since 03-01", 'T', -1.0));
  EXPECT_EQ("04-01", Rel2Iso(cdClim, "seasons since 01-01", 'T', 1.0));
  EXPECT_EQ("1979-01-02", Rel2Iso(cdStandard, "hours", 'T', 24.0));
  EXPECT_EQ("01-02", Rel2Iso(cdClim, "hours", 'T', 24.0));

  // Invalid basetime date in cdComp2Iso exits early cleanly without crash.
  {
    ScopedCdErrOpts silent(0);
    char buf[32] = "unchanged";
    std::string units = "months since 2000-13-01";
    cdRel2Iso(cdStandard, units.data(), 'T', 0.0, buf, sizeof(buf));
    EXPECT_STREQ("unchanged", buf);
  }
}

TEST(NctimeTest, Rel2IsoHandlesAllCalendarVariantsAndMixedReformGap) {
  EXPECT_EQ("1900-02-29", Rel2Iso(cdJulian, "days since 1900-02-28", 'T', 1.0));
  EXPECT_EQ("2000-03-01", Rel2Iso(cdNoLeap, "days since 2000-02-28", 'T', 1.0));
  EXPECT_EQ("2000-02-30", Rel2Iso(cd360, "days since 2000-02-28", 'T', 2.0));
  EXPECT_EQ("2000-03-01", Rel2Iso(cd360, "days since 2000-02-28", 'T', 3.0));
  EXPECT_EQ("2001-02-29", Rel2Iso(cd366, "days since 2001-02-28", 'T', 1.0));
  EXPECT_EQ("02-29", Rel2Iso(cdClimLeap, "days since 02-28", 'T', 1.0));
  EXPECT_EQ("03-01", Rel2Iso(cdClim360, "days since 02-28", 'T', 1.0));

  // cdMixed: Julian before 1582-10-15, Gregorian from 1582-10-15 onward.
  EXPECT_EQ("1582-10-04T12",
            Rel2Iso(cdMixed, "hours since 1582-10-04", 'T', 12.0));
  EXPECT_EQ("1582-10-16", Rel2Iso(cdMixed, "days since 1582-10-04", 'T', 2.0));
  EXPECT_EQ("1582-10-17", Rel2Iso(cdMixed, "days since 1582-10-16", 'T', 1.0));
  EXPECT_EQ("1582-10-04", Rel2Iso(cdMixed, "days since 1582-10-16", 'T', -2.0));

  // Exercise cdCompCompare field branches (year, month, day, hour vs
  // 1582-10-15).
  EXPECT_EQ("1581-01-02", Rel2Iso(cdMixed, "days since 1581-01-01", 'T', 1.0));
  EXPECT_EQ("1583-01-02", Rel2Iso(cdMixed, "days since 1583-01-01", 'T', 1.0));
  EXPECT_EQ("1582-09-16", Rel2Iso(cdMixed, "days since 1582-09-15", 'T', 1.0));
  EXPECT_EQ("1582-11-16", Rel2Iso(cdMixed, "days since 1582-11-15", 'T', 1.0));
  EXPECT_EQ("1582-10-15T18",
            Rel2Iso(cdMixed, "hours since 1582-10-15 12:00:00", 'T', 6.0));
  EXPECT_EQ("1582-10-15T00:01",
            Rel2Iso(cdMixed, "minutes since 1582-10-15", 'T', 1.0));
  EXPECT_EQ("1582-10-15T00:00:30",
            Rel2Iso(cdMixed, "seconds since 1582-10-15", 'T', 30.0));
  EXPECT_EQ("1582-10-22", Rel2Iso(cdMixed, "weeks since 1582-10-15", 'T', 1.0));
  EXPECT_EQ("1582-11-01",
            Rel2Iso(cdMixed, "months since 1582-10-01", 'T', 1.0));
  EXPECT_EQ("1583-01-01",
            Rel2Iso(cdMixed, "seasons since 1582-10-01", 'T', 1.0));
  EXPECT_EQ("1583-10-01", Rel2Iso(cdMixed, "years since 1582-10-01", 'T', 1.0));
}

TEST(NctimeTest, SetErrOptsSupportsVerboseAndFatalErrorModes) {
  // Verify cdSetErrOpts round-trips previous option value cleanly.
  {
    ScopedCdErrOpts guard(CU_VERBOSE);
    EXPECT_EQ(CU_VERBOSE, cdSetErrOpts(0));
  }

  ScopedDeathTestStyle style_guard("threadsafe");
  EXPECT_EXIT(
      {
        cdSetErrOpts(CU_VERBOSE);
        cdUnitTime unit = cdBadUnit;
        cdCompTime base{};
        char bad[] = "bogus_unit since 2000-01-01";
        const int rc = cdParseRelunits(cdStandard, bad, &unit, &base);
        std::fflush(nullptr);
        if (&__gcov_dump != nullptr) {
          __gcov_dump();
        }
        _exit(rc == 1 ? 0 : 1);
      },
      ::testing::ExitedWithCode(0), "CDMS error:");

  EXPECT_EXIT(
      {
        cdSetErrOpts(CU_VERBOSE | CU_FATAL);
        cdUnitTime unit = cdBadUnit;
        cdCompTime base{};
        char bad[] = "bogus_unit since 2000-01-01";
        cdParseRelunits(cdStandard, bad, &unit, &base);
        _exit(0);
      },
      ::testing::ExitedWithCode(1), "CDMS error:");
}

}  // namespace
}  // namespace netcdf
