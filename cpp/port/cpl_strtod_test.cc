// Tests for GDAL's C strtod API.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_strtod.cpp
//
// Copyright 2015 Google Inc. All Rights Reserved.
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

#include <cmath>
#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(CplStrtodTest, CPLAtofDelim) {
  EXPECT_DOUBLE_EQ(-1.2, CPLAtofDelim("-1=2", '='));
}

TEST(CplStrtodTest, CPLAtof) { EXPECT_DOUBLE_EQ(-1.2, CPLAtof("-1.2")); }

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

  // String with >= 50 characters before any '.' or ',' or '\0'.
  value = CPLAtofM("                                                  1.5");
  EXPECT_DOUBLE_EQ(1.5, value);
}

TEST(CplStrtodTest, StrtodM) {
  char* end = nullptr;
  EXPECT_DOUBLE_EQ(1.25, CPLStrtodM("1.25abc", &end));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("abc", end);

  end = nullptr;
  EXPECT_DOUBLE_EQ(1.25, CPLStrtodM("1,25xyz", &end));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("xyz", end);

  end = nullptr;
  EXPECT_DOUBLE_EQ(42.0, CPLStrtodM("42", &end));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("", end);

  // String with >= 50 characters before any '.' or ',' or '\0'.
  end = nullptr;
  EXPECT_DOUBLE_EQ(
      2.5,
      CPLStrtodM("                                                  2.5tail",
                 &end));
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("tail", end);
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
  char* end = nullptr;
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

  // Leading spaces and locale decimal point in input when using custom point.
  end = nullptr;
  value = CPLStrtodDelim("   1,25.99", &end, ',');
  EXPECT_DOUBLE_EQ(1.25, value);
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ(".99", end);

  // Special values with non-null endptr.
  const char* s_neg_qnan = "-1.#QNAN";
  end = nullptr;
  EXPECT_TRUE(std::isnan(CPLStrtodDelim(s_neg_qnan, &end, '.')));
  EXPECT_EQ(s_neg_qnan + strlen(s_neg_qnan), end);

  const char* s_neg_inf = "-inf";
  end = nullptr;
  value = CPLStrtodDelim(s_neg_inf, &end, '.');
  EXPECT_TRUE(std::isinf(value));
  EXPECT_LT(value, 0.0);
  EXPECT_EQ(s_neg_inf + strlen(s_neg_inf), end);

  const char* s_pos_snan = "1.#SNAN";
  end = nullptr;
  EXPECT_TRUE(std::isnan(CPLStrtodDelim(s_pos_snan, &end, '.')));
  EXPECT_EQ(s_pos_snan + strlen(s_pos_snan), end);

  const char* s_pos_1inf = "1.#INF";
  end = nullptr;
  value = CPLStrtodDelim(s_pos_1inf, &end, '.');
  EXPECT_TRUE(std::isinf(value));
  EXPECT_GT(value, 0.0);
  EXPECT_EQ(s_pos_1inf + strlen(s_pos_1inf), end);

  const char* s_pos_inf = "inf";
  end = nullptr;
  value = CPLStrtodDelim(s_pos_inf, &end, '.');
  EXPECT_TRUE(std::isinf(value));
  EXPECT_GT(value, 0.0);
  EXPECT_EQ(s_pos_inf + strlen(s_pos_inf), end);

  const char* s_nan = "nan";
  end = nullptr;
  EXPECT_TRUE(std::isnan(CPLStrtodDelim(s_nan, &end, '.')));
  EXPECT_EQ(s_nan + strlen(s_nan), end);
  EXPECT_TRUE(std::isnan(CPLStrtodDelim("nan", nullptr, '.')));
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

  char* end = nullptr;
  value = CPLStrtofDelim("3,5rest", &end, ',');
  EXPECT_FLOAT_EQ(3.5f, value);
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("rest", end);

  end = nullptr;
  value = CPLStrtof("4.25end", &end);
  EXPECT_FLOAT_EQ(4.25f, value);
  ASSERT_NE(nullptr, end);
  EXPECT_STREQ("end", end);
}

}  // namespace
}  // namespace autotest2
