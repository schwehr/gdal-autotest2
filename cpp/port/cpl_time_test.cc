// Tests for UNIX to-from struct tm functions.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_time.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// See also:
//
//   https://tools.ietf.org/html/rfc822
//   https://tools.ietf.org/html/rfc2445
//   https://tools.ietf.org/html/rfc3339


#include <time.h>
#include <cstdlib>

#include "gunit.h"
#include "port/cpl_port.h"
#include "port/cpl_time.h"

// Tests Unix to struct time and back.
TEST(TimeTest, Time) {
  struct tm time;
  struct tm *time_result;

  // Start of the unix epoch.
  GIntBig test_seconds = 0;
  time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
  ASSERT_EQ(&time, time_result);
  ASSERT_EQ(0, time.tm_sec);
  ASSERT_EQ(0, time.tm_min);
  ASSERT_EQ(0, time.tm_hour);
  ASSERT_EQ(1, time.tm_mday);
  ASSERT_EQ(0, time.tm_mon);
  ASSERT_EQ(70, time.tm_year);
  ASSERT_EQ(4, time.tm_wday);
  ASSERT_EQ(0, time.tm_yday);
  ASSERT_EQ(0, time.tm_isdst);  // Always 0.
  ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));

  // One second before the unix epoch.
  test_seconds = -1;
  time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
  ASSERT_EQ(&time, time_result);
  ASSERT_EQ(59, time.tm_sec);
  ASSERT_EQ(59, time.tm_min);
  ASSERT_EQ(23, time.tm_hour);
  ASSERT_EQ(31, time.tm_mday);
  ASSERT_EQ(11, time.tm_mon);
  ASSERT_EQ(69, time.tm_year);
  ASSERT_EQ(3, time.tm_wday);
  ASSERT_EQ(364, time.tm_yday);
  ASSERT_EQ(0, time.tm_isdst);  // Always 0.
  ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));

  // Middle of summer for daylight savings time.
  test_seconds = 1405018019;
  time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
  ASSERT_EQ(59, time.tm_sec);
  ASSERT_EQ(46, time.tm_min);
  ASSERT_EQ(18, time.tm_hour);
  ASSERT_EQ(10, time.tm_mday);
  ASSERT_EQ(6, time.tm_mon);
  ASSERT_EQ(114, time.tm_year);
  ASSERT_EQ(4, time.tm_wday);
  ASSERT_EQ(190, time.tm_yday);
  ASSERT_EQ(0, time.tm_isdst);  // Always 0.
  ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));

  // Leap day.
  test_seconds = 1204243247;
  time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
  ASSERT_EQ(47, time.tm_sec);
  ASSERT_EQ(0, time.tm_min);
  ASSERT_EQ(0, time.tm_hour);
  ASSERT_EQ(29, time.tm_mday);
  ASSERT_EQ(1, time.tm_mon);
  ASSERT_EQ(108, time.tm_year);
  ASSERT_EQ(5, time.tm_wday);
  ASSERT_EQ(59, time.tm_yday);
  ASSERT_EQ(0, time.tm_isdst);  // Always 0.
  ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));

  // Far future.
  test_seconds = 5000000000;
  time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
  ASSERT_EQ(20, time.tm_sec);
  ASSERT_EQ(53, time.tm_min);
  ASSERT_EQ(8, time.tm_hour);
  ASSERT_EQ(11, time.tm_mday);
  ASSERT_EQ(5, time.tm_mon);
  ASSERT_EQ(228, time.tm_year);
  ASSERT_EQ(5, time.tm_wday);
  ASSERT_EQ(162, time.tm_yday);
  ASSERT_EQ(0, time.tm_isdst);  // Always 0.
  ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));

  // 1900.
  test_seconds = -2208988800;
  time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
  ASSERT_EQ(0, time.tm_sec);
  ASSERT_EQ(0, time.tm_min);
  ASSERT_EQ(0, time.tm_hour);
  ASSERT_EQ(1, time.tm_mday);
  ASSERT_EQ(0, time.tm_mon);
  ASSERT_EQ(0, time.tm_year);
  ASSERT_EQ(1, time.tm_wday);
  ASSERT_EQ(0, time.tm_yday);
  ASSERT_EQ(0, time.tm_isdst);  // Always 0.
  ASSERT_EQ(test_seconds, CPLYMDHMSToUnixTime(&time));
}

// Tests round trip of random values.
TEST(TimeTest, FuzzRandomTime) {
  struct tm time;
  struct tm *time_result;

  // Start of the unix epoch.
  for (int count=0; count < 10000; ++count) {
    const GIntBig test_seconds
        = static_cast<GIntBig> (random() % 10000000000 - 5000000000);
    time_result = CPLUnixTimeToYMDHMS(test_seconds, &time);
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
    int result = CPLParseRFC822DateTime(rfc822datetime, &year_, &month_, &day_,
                                     &hour_, &minute_, &second_, &tzflag_,
                                     &weekday_);
    CHECK(result == FALSE || result == TRUE);
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

TEST_F(Rfc822Test, Basic) {
  EXPECT_TRUE(Check("28 Dec 2007 05:24:17"));
  EXPECT_EQ(2007, year_);
  EXPECT_EQ(12, month_);
  EXPECT_EQ(28, day_);
  EXPECT_EQ(5, hour_);
  EXPECT_EQ(24, minute_);
  EXPECT_EQ(17, second_);
  EXPECT_EQ(0, tzflag_);
  EXPECT_EQ(0, weekday_);
}

TEST_F(Rfc822Test, NoSeconds) {
  EXPECT_TRUE(Check("28 Dec 2007 05:24"));
  EXPECT_EQ(2007, year_);
  EXPECT_EQ(12, month_);
  EXPECT_EQ(28, day_);
  EXPECT_EQ(5, hour_);
  EXPECT_EQ(24, minute_);
  EXPECT_EQ(-1, second_);
  EXPECT_EQ(0, tzflag_);
  EXPECT_EQ(0, weekday_);
}

TEST_F(Rfc822Test, NoDayOrTimeZone) {
  EXPECT_TRUE(Check("28 Dec 2007 05:24:17"));
  EXPECT_EQ(2007, year_);
  EXPECT_EQ(12, month_);
  EXPECT_EQ(28, day_);
  EXPECT_EQ(5, hour_);
  EXPECT_EQ(24, minute_);
  EXPECT_EQ(17, second_);
  EXPECT_EQ(0, tzflag_);
  EXPECT_EQ(0, weekday_);
}

TEST_F(Rfc822Test, SmallYear) {
  EXPECT_TRUE(Check("28 Dec 1 05:24:17"));
  EXPECT_EQ(2001, year_);
  EXPECT_TRUE(Check("28 Dec 301 05:24:17"));
  EXPECT_EQ(301, year_);
}

TEST_F(Rfc822Test, DayAndTimeZone) {
  EXPECT_TRUE(Check("Fri 28 Dec 2007 05:24:17 GMT"));
  EXPECT_EQ(2007, year_);
  EXPECT_EQ(12, month_);
  EXPECT_EQ(28, day_);
  EXPECT_EQ(5, hour_);
  EXPECT_EQ(24, minute_);
  EXPECT_EQ(17, second_);
  EXPECT_EQ(100, tzflag_);
  EXPECT_EQ(5, weekday_);
}
