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
// Tests WKTReader.cpp
//
// See also:
//   https://trac.osgeo.org/geos/browser/trunk/src/io/StringTokenizer.cpp
//   https://trac.osgeo.org/geos/browser/trunk/include/geos/io/StringTokenizer.h
//   https://trac.osgeo.org/geos/browser/trunk/tests/unit/io/
//
// WARNINGS:
//   If you pass anything other than a std::string (e.g. custom string
//   or char *), the compiler will make a std::string copy that is
//   destroyed after the constructor is finished.  If that happens,
//   the first access of iter inside of StringTokenizer will give an
//   ASAN error.
//
//   The tokenizer uses a const ref: you must keep the std::string around for
//   as long as the StringTokenizer.

#include "include/geos/io/StringTokenizer.h"

#include <cmath>
#include <string>

#include "gunit.h"

namespace geos {
namespace io {
namespace {

TEST(StringTokenizerTest, Empty) {
  const std::string s;
  StringTokenizer t(s);
  EXPECT_EQ(0.0, t.getNVal());
  EXPECT_EQ("", t.getSVal());
  EXPECT_EQ(StringTokenizer::TT_EOF, t.peekNextToken());
  EXPECT_EQ(StringTokenizer::TT_EOF, t.nextToken());
}

TEST(StringTokenizerTest, Basic) {
  const std::string s = "a";
  StringTokenizer t(s);
  EXPECT_EQ(0.0, t.getNVal());
  EXPECT_EQ("", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.peekNextToken());
  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ(0.0, t.getNVal());
  EXPECT_EQ("a", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_EOF, t.peekNextToken());
  EXPECT_EQ(StringTokenizer::TT_EOF, t.nextToken());
  EXPECT_EQ(StringTokenizer::TT_EOF, t.peekNextToken());
  EXPECT_EQ(StringTokenizer::TT_EOF, t.nextToken());
}

TEST(StringTokenizerTest, Numbers) {
  const std::string s = "0 -1 0.2 +3.2 -4.";
  StringTokenizer t(s);

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(0.0, t.getNVal());

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(-1.0, t.getNVal());

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(0.2, t.getNVal());

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(3.2, t.getNVal());

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(-4.0, t.getNVal());

  EXPECT_EQ(StringTokenizer::TT_EOF, t.peekNextToken());
}

TEST(StringTokenizerTest, InfNan) {
  const std::string s = "inf -inf nan";
  StringTokenizer t(s);

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  const double inf = t.getNVal();
  EXPECT_TRUE(isinf(inf));
  EXPECT_FALSE(signbit(inf));

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  const double neg_inf = t.getNVal();
  EXPECT_TRUE(isinf(neg_inf));
  EXPECT_TRUE(signbit(neg_inf));

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_TRUE(isnan(t.getNVal()));

  EXPECT_EQ(StringTokenizer::TT_EOF, t.peekNextToken());
}

TEST(StringTokenizerTest, Point) {
  const std::string s = "POINT(1 2)";
  StringTokenizer t(s);
  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_DOUBLE_EQ(0.0, t.getNVal());
  EXPECT_EQ("POINT", t.getSVal());

  // "(" - Returns the ASCII character number.
  EXPECT_EQ(40, t.nextToken());
  // Old values remain.
  EXPECT_DOUBLE_EQ(0.0, t.getNVal());
  EXPECT_EQ("POINT", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(1.0, t.getNVal());
  EXPECT_EQ("", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_NUMBER, t.nextToken());
  EXPECT_DOUBLE_EQ(2.0, t.getNVal());
  EXPECT_EQ("", t.getSVal());

  // ")"
  EXPECT_EQ(41, t.nextToken());
  // Old values remain.
  EXPECT_DOUBLE_EQ(2.0, t.getNVal());
  EXPECT_EQ("", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_EOF, t.peekNextToken());
}

TEST(StringTokenizerTest, Comma) {
  const std::string s = "a,b";
  StringTokenizer t(s);

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("a", t.getSVal());

  // ","
  EXPECT_EQ(44, t.nextToken());
  EXPECT_DOUBLE_EQ(0.0, t.getNVal());
  EXPECT_EQ("a", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("b", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_EOF, t.nextToken());
}

TEST(StringTokenizerTest, NewlineTabSpaces) {
  const std::string s = " a\nb\rc\td      e\n\r\t f";
  StringTokenizer t(s);

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("a", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("b", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("c", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("d", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("e", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_WORD, t.nextToken());
  EXPECT_EQ("f", t.getSVal());

  EXPECT_EQ(StringTokenizer::TT_EOF, t.nextToken());
}

}  // namespace
}  // namespace io
}  // namespace geos
