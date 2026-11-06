// Copyright 2014 Google LLC
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
// Tests for UNIX to-from struct tm functions:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_time.cpp
//
// See also:
//   https://tools.ietf.org/html/rfc822
//   https://tools.ietf.org/html/rfc2445
//   https://tools.ietf.org/html/rfc3339

#include "third_party/gdal/port/cpl_time.h"

#include <time.h>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

// Tests Unix to struct time and back.
TEST(TimeTest, Time) {
  struct TimeCase {
    GIntBig unix_seconds;
    int sec;
    int min;
    int hour;
    int mday;
    int mon;
    int year;
    int wday;
    int yday;
  };
  constexpr TimeCase kCases[] = {
      // Start of the unix epoch.
      {.unix_seconds = 0,
       .sec = 0,
       .min = 0,
       .hour = 0,
       .mday = 1,
       .mon = 0,
       .year = 70,
       .wday = 4,
       .yday = 0},
      // One second before the unix epoch.
      {.unix_seconds = -1,
       .sec = 59,
       .min = 59,
       .hour = 23,
       .mday = 31,
       .mon = 11,
       .year = 69,
       .wday = 3,
       .yday = 364},
      // Middle of summer for daylight savings time.
      {.unix_seconds = 1405018019,
       .sec = 59,
       .min = 46,
       .hour = 18,
       .mday = 10,
       .mon = 6,
       .year = 114,
       .wday = 4,
       .yday = 190},
      // Leap day.
      {.unix_seconds = 1204243247,
       .sec = 47,
       .min = 0,
       .hour = 0,
       .mday = 29,
       .mon = 1,
       .year = 108,
       .wday = 5,
       .yday = 59},
      // Far future.
      {.unix_seconds = 5000000000LL,
       .sec = 20,
       .min = 53,
       .hour = 8,
       .mday = 11,
       .mon = 5,
       .year = 228,
       .wday = 5,
       .yday = 162},
      // 1900.
      {.unix_seconds = -2208988800LL,
       .sec = 0,
       .min = 0,
       .hour = 0,
       .mday = 1,
       .mon = 0,
       .year = 0,
       .wday = 1,
       .yday = 0},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.unix_seconds);
    struct tm time = {};
    ASSERT_EQ(&time, CPLUnixTimeToYMDHMS(tc.unix_seconds, &time));
    EXPECT_EQ(tc.sec, time.tm_sec);
    EXPECT_EQ(tc.min, time.tm_min);
    EXPECT_EQ(tc.hour, time.tm_hour);
    EXPECT_EQ(tc.mday, time.tm_mday);
    EXPECT_EQ(tc.mon, time.tm_mon);
    EXPECT_EQ(tc.year, time.tm_year);
    EXPECT_EQ(tc.wday, time.tm_wday);
    EXPECT_EQ(tc.yday, time.tm_yday);
    EXPECT_EQ(0, time.tm_isdst);  // Always 0.
    EXPECT_EQ(tc.unix_seconds, CPLYMDHMSToUnixTime(&time));
  }
}

// Tests round trip of random values.
TEST(TimeTest, FuzzRandomTime) {
  struct tm time = {};
  for (int count = 0; count < 10000; ++count) {
    const GIntBig test_seconds =
        static_cast<GIntBig>(random() % 10000000000 - 5000000000);
    ASSERT_EQ(&time, CPLUnixTimeToYMDHMS(test_seconds, &time));
    ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));
  }
}

TEST(DirectRfc822Test, NullPointers) {
  EXPECT_FALSE(CPLParseRFC822DateTime(nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr,
                                      nullptr));
}

constexpr int kUndefined = -9999;

class Rfc822Test : public ::testing::Test {
 protected:
  bool Check(const char* rfc822datetime) {
    const int result =
        CPLParseRFC822DateTime(rfc822datetime, &year_, &month_, &day_, &hour_,
                               &minute_, &second_, &tzflag_, &weekday_);
    EXPECT_TRUE(result == FALSE || result == TRUE);
    return CPL_TO_BOOL(result);
  }

  int year_ = kUndefined;
  int month_ = kUndefined;
  int day_ = kUndefined;
  int hour_ = kUndefined;
  int minute_ = kUndefined;
  int second_ = kUndefined;
  int tzflag_ = kUndefined;
  int weekday_ = kUndefined;
};

TEST_F(Rfc822Test, BasicFails) {
  EXPECT_FALSE(Check(""));
  EXPECT_EQ(kUndefined, year_);
  EXPECT_EQ(kUndefined, month_);
  EXPECT_EQ(kUndefined, day_);
  EXPECT_EQ(kUndefined, hour_);
  EXPECT_EQ(kUndefined, minute_);
  EXPECT_EQ(kUndefined, second_);
  EXPECT_EQ(kUndefined, tzflag_);
  EXPECT_EQ(kUndefined, weekday_);

  day_ = kUndefined;
  EXPECT_FALSE(Check("0 Dec 1 05:24:17"));
  EXPECT_EQ(kUndefined, day_);
  EXPECT_FALSE(Check("32 Dec 301 05:24:17"));
  EXPECT_EQ(kUndefined, day_);
  EXPECT_FALSE(Check("100 Dec 301 05:24:17"));
  EXPECT_EQ(kUndefined, day_);

  // Out of range day
  EXPECT_FALSE(Check("-1 Dec 1 05:24:17"));
  EXPECT_EQ(kUndefined, day_);
}

TEST_F(Rfc822Test, ValidCases) {
  struct Rfc822Case {
    const char* input;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int tzflag;
    int weekday;
  };
  constexpr Rfc822Case kCases[] = {
      {.input = "28 Dec 2007 05:24:17",
       .year = 2007,
       .month = 12,
       .day = 28,
       .hour = 5,
       .minute = 24,
       .second = 17,
       .tzflag = 0,
       .weekday = 0},
      {.input = "28 Dec 2007 05:24",
       .year = 2007,
       .month = 12,
       .day = 28,
       .hour = 5,
       .minute = 24,
       .second = -1,
       .tzflag = 0,
       .weekday = 0},
      {.input = "28 Dec 1 05:24:17",
       .year = 2001,
       .month = 12,
       .day = 28,
       .hour = 5,
       .minute = 24,
       .second = 17,
       .tzflag = 0,
       .weekday = 0},
      {.input = "28 Dec 301 05:24:17",
       .year = 301,
       .month = 12,
       .day = 28,
       .hour = 5,
       .minute = 24,
       .second = 17,
       .tzflag = 0,
       .weekday = 0},
      {.input = "Fri 28 Dec 2007 05:24:17 GMT",
       .year = 2007,
       .month = 12,
       .day = 28,
       .hour = 5,
       .minute = 24,
       .second = 17,
       .tzflag = 100,
       .weekday = 5},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_TRUE(Check(tc.input));
    EXPECT_EQ(tc.year, year_);
    EXPECT_EQ(tc.month, month_);
    EXPECT_EQ(tc.day, day_);
    EXPECT_EQ(tc.hour, hour_);
    EXPECT_EQ(tc.minute, minute_);
    EXPECT_EQ(tc.second, second_);
    EXPECT_EQ(tc.tzflag, tzflag_);
    EXPECT_EQ(tc.weekday, weekday_);
  }
}

TEST(TimeTest, OutOfRangeAndInvalidMonth) {
  struct tm time = {};
  {
    WithQuietHandler quiet;
    constexpr GIntBig kWayPastLimit = 10000LL * 86400LL * 366LL + 100LL;
    EXPECT_EQ(&time, CPLUnixTimeToYMDHMS(kWayPastLimit, &time));
    EXPECT_EQ(0, time.tm_year);
    EXPECT_EQ(&time, CPLUnixTimeToYMDHMS(-kWayPastLimit, &time));
    EXPECT_EQ(0, time.tm_year);
  }

  time.tm_mon = -1;
  EXPECT_EQ(-1, CPLYMDHMSToUnixTime(&time));
  time.tm_mon = 12;
  EXPECT_EQ(-1, CPLYMDHMSToUnixTime(&time));
}

TEST_F(Rfc822Test, AdditionalBranches) {
  // Invalid month.
  EXPECT_FALSE(Check("28 Foo 2007 05:24:17"));

  // Year in [30, 99] -> +1900.
  EXPECT_TRUE(Check("28 Dec 85 05:24:17"));
  EXPECT_EQ(1985, year_);

  // Invalid hour.
  EXPECT_FALSE(Check("28 Dec 2007 24:24:17"));
  EXPECT_FALSE(Check("28 Dec 2007 -1:24:17"));

  // Missing minute (5 tokens where first token is weekday).
  EXPECT_FALSE(Check("Fri 28 Dec 2007 05"));

  // Invalid minute.
  EXPECT_FALSE(Check("28 Dec 2007 05:60:17"));
  EXPECT_FALSE(Check("28 Dec 2007 05:-1:17"));

  // Invalid second.
  EXPECT_FALSE(Check("28 Dec 2007 05:24:61"));
  EXPECT_FALSE(Check("28 Dec 2007 05:24:-1"));

  // Numeric timezones (+HHMM and -HHMM).
  EXPECT_TRUE(Check("28 Dec 2007 05:24:17 +0230"));
  EXPECT_EQ(110, tzflag_);

  EXPECT_TRUE(Check("28 Dec 2007 05:24:17 -0500"));
  EXPECT_EQ(80, tzflag_);

  // Out-of-range numeric timezone hour (>= 15 or < 0).
  EXPECT_FALSE(Check("28 Dec 2007 05:24:17 +1500"));

  // Named timezone later in list (e.g. PDT) and unknown timezone.
  EXPECT_TRUE(Check("28 Dec 2007 05:24:17 PDT"));
  EXPECT_EQ(72, tzflag_);

  EXPECT_FALSE(Check("28 Dec 2007 05:24:17 BOGUSTZ"));
}

}  // namespace
}  // namespace autotest2
