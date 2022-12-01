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
// https://trac.osgeo.org/gdal/browser/trunk/gdal/frmts/grib/degrib/degrib/myerror.c
//
// See also:
//   https://www.weather.gov/mdl/degrib_home
//   http://www.gdal.org/frmt_grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py

#include "frmts/grib/degrib/degrib/myutil.h"

#include <memory>
#include <limits>

#include "gunit.h"

namespace {

TEST(MyUtilTest, MyRound) {
  EXPECT_DOUBLE_EQ(0.0, myRound(0.0, 0));

  EXPECT_DOUBLE_EQ(1.0, myRound(1.1, 0));
  EXPECT_DOUBLE_EQ(1.0e-1, myRound(1.1e-1, 1));
  EXPECT_DOUBLE_EQ(1.0e-2, myRound(1.1e-2, 2));
  // ...
  EXPECT_DOUBLE_EQ(1.0e-16, myRound(1.1e-16, 16));
  EXPECT_DOUBLE_EQ(1.0e-17, myRound(1.1e-17, 17));
  EXPECT_DOUBLE_EQ(2.0e-17, myRound(1.5e-17, 17));
  // Over the limit on number of digits.
  EXPECT_DOUBLE_EQ(1.0e-17, myRound(1.123e-17, 18));

  EXPECT_DOUBLE_EQ(-1.0e-1, myRound(-0.9e-1, 1));
  EXPECT_DOUBLE_EQ(-1.0e-1, myRound(-1.1e-1, 1));

  constexpr double double_max = std::numeric_limits<double>::max();
  EXPECT_DOUBLE_EQ(double_max, myRound(double_max, 1));
  EXPECT_DOUBLE_EQ(double_max, myRound(double_max, 17));
  constexpr double lowest = std::numeric_limits<double>::lowest();
  EXPECT_DOUBLE_EQ(lowest, myRound(lowest, 1));
  EXPECT_DOUBLE_EQ(lowest, myRound(lowest, 17));

  constexpr double inf = std::numeric_limits<double>::infinity();
  EXPECT_DOUBLE_EQ(inf, myRound(inf, 1));
  EXPECT_DOUBLE_EQ(inf, myRound(inf, 17));

  EXPECT_DOUBLE_EQ(-inf, myRound(-inf, 1));
  EXPECT_DOUBLE_EQ(-inf, myRound(-inf, 17));

  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(std::isnan(myRound(nan, 1)));
  EXPECT_TRUE(std::isnan(myRound(nan, 17)));
}

TEST(MyUtilTest, StrTrim) {
  // Calling with a nullptr should not crash.
  strTrim(nullptr);

  char empty[] = "";
  strTrim(empty);
  EXPECT_STREQ("", empty);

  char whitespace[] = "  ";
  strTrim(whitespace);
  EXPECT_STREQ("", whitespace);

  char a[] = "a ";
  strTrim(a);
  EXPECT_STREQ("a", a);

  char b[] = " b";
  strTrim(b);
  EXPECT_STREQ("b", b);

  char c[] = " c ";
  strTrim(c);
  EXPECT_STREQ("c", c);

  char d[] = "\n\t d \te\t \n\v\f";
  strTrim(d);
  EXPECT_STREQ("d \te", d);

  char e[] = " \xee ";
  strTrim(e);
  EXPECT_STREQ("\xee", e);
}

TEST(MyUtilTest, StrTrimRight) {
  // Calling with a nullptr should not crash.
  strTrimRight(nullptr, 'z');

  char empty[] = "";
  strTrimRight(empty, 'z');
  EXPECT_STREQ("", empty);

  char whitespace[] = "";
  strTrimRight(whitespace, 'z');
  EXPECT_STREQ("", whitespace);

  char a[] = "a ";
  strTrimRight(a, 'z');
  EXPECT_STREQ("a", a);

  char b[] = " b";
  strTrimRight(b, 'z');
  EXPECT_STREQ(" b", b);

  char c[] = " \xee ";
  strTrimRight(c, 'z');
  EXPECT_STREQ(" \xee", c);

  char d[] = "z";
  strTrimRight(d, 'z');
  EXPECT_STREQ("", d);

  char e[] = " z ";
  strTrimRight(e, 'z');
  EXPECT_STREQ("", e);
}

TEST(MyUtilTest, StrCompact) {
  strCompact(nullptr, 'c');

  char empty[] = "";
  strCompact(empty, 'c');
  EXPECT_STREQ("", empty);

  char a[] = "a";
  strCompact(a, 'a');
  EXPECT_STREQ("a", a);

  char b[] = "bb";
  strCompact(b, 'b');
  EXPECT_STREQ("b", b);

  char c[] = "c ccc";
  strCompact(c, 'c');
  EXPECT_STREQ("c c", c);
}

TEST(MyUtilTest, GetIndexFromStrShort) {
  int index = 0;
  const char *a[] = {nullptr};

  EXPECT_EQ(-1, GetIndexFromStr("", a, &index));
  EXPECT_EQ(-1, index);
}

TEST(MyUtilTest, GetIndexFromStr) {
  int index = 0;
  const char *a[] = {"a", "bc", "", "\xee", nullptr};
  EXPECT_EQ(0, GetIndexFromStr("a", a, &index));
  EXPECT_EQ(0, index);

  EXPECT_EQ(1, GetIndexFromStr("bc", a, &index));

  EXPECT_EQ(2, GetIndexFromStr("", a, &index));

  EXPECT_EQ(3, GetIndexFromStr("\xee", a, &index));
  EXPECT_EQ(3, index);

  EXPECT_EQ(-1, GetIndexFromStr("a ", a, &index));
  EXPECT_EQ(-1, index);
}

}  // namespace
