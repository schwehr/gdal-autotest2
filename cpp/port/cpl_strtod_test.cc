// Tests for GDAL's C String List (CSL) API.
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_strtod.cpp
//
// Copyright 2015 Google Inc. All Rights Reserved.
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


#include <cmath>

#include "gunit.h"
#include "port/cpl_conv.h"

namespace autotest2 {
namespace {


TEST(CplStrtodTest, CPLAtofDelim) {
  EXPECT_DOUBLE_EQ(-1.2, CPLAtofDelim("-1=2", '='));
}

TEST(CplStrtodTest, CPLAtof) {
  EXPECT_DOUBLE_EQ(-1.2, CPLAtof("-1.2"));
}

TEST(CplStrtodTest, AtofM) {
  // Test conversion to double using multi-lingual / locale processing.
  double value = CPLAtofM("0");
  EXPECT_EQ(0.0, value);

  value = CPLAtofM("6");
  EXPECT_EQ(6, value);

  value = CPLAtofM("1.");
  EXPECT_EQ(1.0, value);

  value = CPLAtofM("1.2");
  EXPECT_EQ(1.2, value);

  value = CPLAtofM("+1.3");
  EXPECT_EQ(1.3, value);

  value = CPLAtofM("-2");
  EXPECT_EQ(-2.0, value);

  value = CPLAtofM("-3,");
  EXPECT_EQ(-3.0, value);

  value = CPLAtofM("-4,5");
  EXPECT_EQ(-4.5, value);

  value = CPLAtofM("1,");
  EXPECT_EQ(1.0, value);

  value = CPLAtofM("1,2");
  EXPECT_EQ(1.2, value);

  value = CPLAtofM("+1,3");
  EXPECT_EQ(1.3, value);

  value = CPLAtofM("-3,");
  EXPECT_EQ(-3.0, value);

  value = CPLAtofM("-4,5");
  EXPECT_EQ(-4.5, value);

  value = CPLAtofM("1.23456789123456789");
  EXPECT_EQ(1.23456789123456789, value);
}

TEST(CplStrtodTest, StrtodDelim) {
  double value = CPLStrtodDelim("0", nullptr, '.');
  EXPECT_EQ(0.0, value);

  value = CPLStrtodDelim("1", nullptr, '.');
  EXPECT_EQ(1, value);

  value = CPLStrtodDelim("2z3", nullptr, 'z');
  EXPECT_EQ(2.3, value);

  // Insane, but works.
  value = CPLStrtodDelim("405", nullptr, '0');
  EXPECT_EQ(4.5, value);

  char str[] = "12.34abc";
  char *end = nullptr;
  value = CPLStrtodDelim(str, &end, '.');
  EXPECT_EQ(12.34, value);
  EXPECT_EQ('a', *end);

  value = CPLStrtodDelim("1.#QNAN", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("-1.#QNAN", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("-1.#QNAN0000000", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("1.#QNAN", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("1.#QNAN0000000", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("-1.#IND", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("-1.#IND0000000", nullptr, '.');
  EXPECT_TRUE(isnan(value));

  value = CPLStrtodDelim("inf", nullptr, '.');
  EXPECT_TRUE(isinf(value));
  EXPECT_LE(0, value);

  value = CPLStrtodDelim("-inf", nullptr, '.');
  EXPECT_TRUE(isinf(value));
  EXPECT_GE(0, value);

  value = CPLStrtodDelim("1.#INF", nullptr, '.');
  EXPECT_TRUE(isinf(value));
  EXPECT_LE(0, value);

  value = CPLStrtodDelim("1.23456789123456789", nullptr, '.');
  EXPECT_EQ(1.23456789123456789, value);
}

TEST(CplStrtodTest, Strtod) {
  // CPLStrtod is StrtoDelim with '.' as the delimiter.
  double value = CPLStrtod("0", nullptr);
  EXPECT_EQ(0.0, value);

  value = CPLStrtod("-1.0", nullptr);
  EXPECT_EQ(-1.0, value);

  value = CPLStrtod("2.3", nullptr);
  EXPECT_EQ(2.3, value);

  value = CPLStrtod("-123456789.123456789123456789", nullptr);
  EXPECT_EQ(-123456789.123456789123456789, value);

  // Bogus strings do what?

  value = CPLStrtod("-9:8", nullptr);
  EXPECT_EQ(-9.0, value);

  value = CPLStrtod("-8,7", nullptr);
  EXPECT_EQ(-8.0, value);

  value = CPLStrtod("bad wolf", nullptr);
  EXPECT_EQ(0.0, value);
}

TEST(CplStrtodTest, CPLStrtofDelim) {
  float value = CPLStrtofDelim("0", nullptr, '.');
  EXPECT_EQ(0.0, value);

  value = CPLStrtofDelim("1.2", nullptr, '.');
  EXPECT_EQ(1.2f, value);

  value = CPLStrtofDelim("-3a4", nullptr, 'a');
  EXPECT_EQ(-3.4f, value);

  value = CPLStrtofDelim("-3", nullptr, '-');
  EXPECT_EQ(0.3f, value);

  value = CPLStrtofDelim("5+6", nullptr, '+');
  EXPECT_EQ(5.6f, value);

  value = CPLStrtofDelim("789", nullptr, '8');
  EXPECT_EQ(7.9f, value);

  value = CPLStrtofDelim("2 9", nullptr, ' ');
  EXPECT_EQ(2.9f, value);

  value = CPLStrtod("-123456789.123456789123456789", nullptr);
  EXPECT_EQ(-123456789.123456789123456789f, value);
}

}  // namespace
}  // namespace autotest2
