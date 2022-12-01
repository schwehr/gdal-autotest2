// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests degrib myerror.c.
//
// https://trac.osgeo.org/gdal/browser/trunk/gdal/frmts/grib/degrib/degrib/clock.cpp
//
// See also:
//   https://www.weather.gov/mdl/degrib_home
//   http://www.gdal.org/frmt_grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py

#include "frmts/grib/degrib/degrib/clock.h"

#include "gmock.h"
#include "gunit.h"

namespace {

TEST(DegribClockTest, Epoch2YearDay) {
  int day = -1;
  sInt4 year = -1;
  Clock_Epoch2YearDay(0, &day, &year);
  EXPECT_EQ(0, day);
  EXPECT_EQ(1970, year);

  Clock_Epoch2YearDay(150000, &day, &year);
  EXPECT_EQ(251, day);
  EXPECT_EQ(2380, year);

  Clock_Epoch2YearDay(-150000, &day, &year);
  EXPECT_EQ(115, day);
  EXPECT_EQ(1559, year);
}

TEST(DegribClockTest, MonthNum) {
  // 2016 was a leap year
  EXPECT_EQ(2, Clock_MonthNum(59, 2016));
  EXPECT_EQ(3, Clock_MonthNum(60, 2016));
  EXPECT_EQ(2, Clock_MonthNum(58, 2017));
  EXPECT_EQ(3, Clock_MonthNum(59, 2017));

  // Special case day.
  EXPECT_EQ(8, Clock_MonthNum (242, 1970));
}

TEST(DegribClockTest, NumDay) {
  // Days of the month
  EXPECT_EQ(29, Clock_NumDay (2, 28, 2016, 0));
  EXPECT_EQ(28, Clock_NumDay (2, 28, 2017, 0));

  // Day of the year.
  EXPECT_EQ(60, Clock_NumDay (3, 1, 2016, 1));
  EXPECT_EQ(59, Clock_NumDay (3, 1, 2017, 1));
}

TEST(DegribClockTest, GetTimeZone) {
  // Not a particularly testable function.
  EXPECT_LT(-24, Clock_GetTimeZone());
  EXPECT_GT(24, Clock_GetTimeZone());
  // TODO(schwehr): Check the local timezone.
}

TEST(DegribClockTest, IsDaylightSaving) {
  EXPECT_FALSE(Clock_IsDaylightSaving2(0, 0));
  // TODO(schwehr): When is true?  l_clock is seconds?
}

TEST(DegribClockTest, PrintDate) {
  sInt4 year = -1;
  int month = -1;
  int day = -1;
  int hour = -1;
  int min = -1;
  double sec = -1.0;
  Clock_PrintDate(0.0, &year, &month, &day, &hour, &min, &sec);
  EXPECT_EQ(1970, year);
  EXPECT_EQ(1, month);
  EXPECT_EQ(1, day);
  EXPECT_EQ(0, hour);
  EXPECT_EQ(0, min);
  EXPECT_EQ(0, sec);

  Clock_PrintDate(1.0e9, &year, &month, &day, &hour, &min, &sec);
  EXPECT_EQ(2001, year);
  EXPECT_EQ(9, month);
  EXPECT_EQ(9, day);
  EXPECT_EQ(1, hour);
  EXPECT_EQ(46, min);
  EXPECT_EQ(40, sec);
}

TEST(DegribClockTest, Print) {
  constexpr int kBufSize = 1000;
  char buffer[kBufSize] = {};
  char format[] = "%Y%m%d%H%M";
  Clock_Print(buffer, kBufSize, 1.0e9, format, 0);
  EXPECT_THAT(buffer, testing::StartsWith("200109"));
  Clock_Print(buffer, kBufSize, 1.0e9, format, 1);
  EXPECT_THAT(buffer, testing::StartsWith("200109"));
}

TEST(DegribClockTest, Print2) {
  constexpr int kBufSize = 1000;
  char buffer[kBufSize] = {};
  char format[] = "%Y%m%d%H%M";
  Clock_Print2(buffer, kBufSize, 1.0e9, format, 0, 0);
  EXPECT_STREQ("200109090146", buffer);
  Clock_Print2(buffer, kBufSize, 1.0e9, format, 0, 1);
  EXPECT_STREQ("200109090246", buffer);
}

TEST(DegribClockTest, Clicks) {
  double a = Clock_Clicks();
  EXPECT_LT(0.0, a);
  double b = Clock_Clicks();
  EXPECT_LE(a, b);
}

// TODO(schwehr): Test the rest of clock.c.

}  // namespace
