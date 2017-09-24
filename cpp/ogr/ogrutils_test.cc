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
// Test ogrutils.cpp.

#include "port/cpl_port.h"
#include "ogr/ogr_p.h"

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"

namespace autotest2 {
namespace {

TEST(OGRFormatDoubleTest, Edges) {
  // Nonsense, but should not crash or leak.
  OGRFormatDouble(nullptr, 0, 0.0, 'a', 0, 'b');

  // No room to write anything.
  char buf[1] = {};
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 0.0, 'a', 0, 'b');
  EXPECT_STREQ("", buf);
}

TEST(OGRFormatDoubleTest, BasicF) {
  // Test the printf double "f" conversion specifier.
  char buf[100] = {};
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 0.0, '.', 0, 'f');
  EXPECT_STREQ("0", buf);
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.2, '.', 0, 'f');
  EXPECT_STREQ("1", buf);
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.2, '.', 1, 'f');
  EXPECT_STREQ("1.2", buf);
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.2, '.', 2, 'f');
  EXPECT_STREQ("1.2", buf);
}

TEST(OGRFormatDoubleTest, TooBig) {
  char buf[8] = {};
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.23456789, '.', 8, 'f');
  EXPECT_STREQ("too_big", buf);
}

TEST(OGRFormatDoubleTest, BasicG) {
  // Test the printf double "g" conversion specifier.
  char buf[100] = {};
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 0.0, '.', 0, 'g');
  EXPECT_STREQ("0", buf);
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.2, '.', 0, 'g');
  EXPECT_STREQ("1", buf);
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.2, '.', 1, 'g');
  EXPECT_STREQ("1", buf);
  OGRFormatDouble(buf, CPL_ARRAYSIZE(buf), 1.2, '.', 2, 'g');
  EXPECT_STREQ("1.2", buf);
  // TODO(schwehr): More cases.
}

// TODO(schwehr): OGRFormatDoubleTest: Test 'a' and 'e' formats.
// TODO(schwehr): OGRFormatDoubleTest: Test non-sense char format.
// TODO(schwehr): OGRFormatDoubleTest: Test trimming of trailing 9999.

TEST(OGRMakeWktCoordinateTest, Basic) {
  // NOTE: Interal assumption that the buffer is no more than 75 long.
  constexpr int kMaxSize = 75 + 1;
  char buf[kMaxSize] = {};
  OGRMakeWktCoordinate(buf, 0.0, 0.0, 0.0, 2);
  EXPECT_STREQ("0 0", buf);
  OGRMakeWktCoordinate(buf, 0.0, 0.0, 0.0, 3);
  EXPECT_STREQ("0 0 0", buf);
  OGRMakeWktCoordinate(buf, -1.0, -2.0, -3.0, 3);
  EXPECT_STREQ("-1 -2 -3", buf);
  OGRMakeWktCoordinate(buf, -1.2, -3.4, -5.6, 3);
  EXPECT_STREQ("-1.2 -3.4 -5.6", buf);

  // TODO(schwehr): inf and nan.
  // TODO(schwehr): Set OGR_WKT_PRECISION.
  // TODO(schwehr): Trigger too many characters.
}

// TODO(schwehr): Test OGRMakeWktCoordinateM.
// TODO(schwehr): Test OGRWktReadToken.
// TODO(schwehr): Test OGRWktReadPoints.
// TODO(schwehr): Test OGRWktReadPointsM.

TEST(OGRParseDateTest, Fail) {
  // nullptr for the string not allowed.

  WithQuietHandler error_handler;

  OGRField field;
  EXPECT_FALSE(OGRParseDate("", &field, 0));

  // Invalid separators.
  EXPECT_FALSE(OGRParseDate("2017#08-10 19:59:01.123", &field, 0));
  EXPECT_FALSE(OGRParseDate("2017-08#10 19:59:01.123", &field, 0));
  EXPECT_FALSE(OGRParseDate("2017-08-10#19:59:01.123", &field, 0));
  EXPECT_FALSE(OGRParseDate("2017-08-10 19#59:01.123", &field, 0));

  // Partial date.
  EXPECT_FALSE(OGRParseDate("2000", &field, 0));
  EXPECT_FALSE(OGRParseDate("2000-08", &field, 0));

  // Out of bounds fields.
  EXPECT_FALSE(OGRParseDate("-32769-08-19", &field, 0));
  EXPECT_FALSE(OGRParseDate("32768-08-19", &field, 0));
  EXPECT_FALSE(OGRParseDate("32768-08-18", &field, 0));
  EXPECT_FALSE(OGRParseDate("2000-0-18", &field, 0));
  EXPECT_FALSE(OGRParseDate("2000-13-18", &field, 0));
  EXPECT_FALSE(OGRParseDate("2000-08-0", &field, 0));
  EXPECT_FALSE(OGRParseDate("2000-08-33", &field, 0));

  EXPECT_FALSE(OGRParseDate("2017-08-10 24:32:01.123", &field, 0));
  EXPECT_FALSE(OGRParseDate("2017-08-10 23:60:01.123", &field, 0));
  EXPECT_FALSE(OGRParseDate("2017-08-10 23:32:62.123", &field, 0));
}

TEST(OGRParseDateTest, Basic) {
  {
    OGRField field;
    EXPECT_TRUE(OGRParseDate("2017-08-10", &field, 0));
    EXPECT_EQ(2017, field.Date.Year);
    EXPECT_EQ(0, field.Date.TZFlag);  // Unknown
  }

  {
    OGRField field;
    EXPECT_TRUE(OGRParseDate("2017-08-10 19:59:01.123", &field, 0));
    EXPECT_EQ(2017, field.Date.Year);
  }

  // Offset time zone.
  {
    OGRField field;
    EXPECT_TRUE(OGRParseDate("2017-08-10 19:59:01.123+0:15", &field, 0));
    EXPECT_EQ(101, field.Date.TZFlag);
  }
  {
    OGRField field;
    EXPECT_TRUE(OGRParseDate("2017-08-10 19:59:01.123+1", &field, 0));
    EXPECT_EQ(104, field.Date.TZFlag);
  }
  {
    OGRField field;
    EXPECT_TRUE(OGRParseDate("2017-08-10 19:59:01.123+12", &field, 0));
    EXPECT_EQ(148, field.Date.TZFlag);
  }
  {
    OGRField field;
    EXPECT_TRUE(OGRParseDate("2017-08-10 19:59:01.123-1", &field, 0));
    EXPECT_EQ(96, field.Date.TZFlag);
  }

  // Zulu.
  OGRField field;
  EXPECT_TRUE(OGRParseDate("2017-08-07T17:29:49.123Z", &field, 0));
  EXPECT_EQ(2017, field.Date.Year);
  EXPECT_EQ(8, field.Date.Month);
  EXPECT_EQ(7, field.Date.Day);
  EXPECT_EQ(17, field.Date.Hour);
  EXPECT_EQ(29, field.Date.Minute);
  EXPECT_FLOAT_EQ(49.123, field.Date.Second);
  EXPECT_EQ(100, field.Date.TZFlag);  // GMT
  EXPECT_EQ(0, field.Date.Reserved);
}

// TODO(schwehr): Test OGRParseXMLDateTime.
// TODO(schwehr): Test OGRParseRFC822DateTime.
// TODO(schwehr): Test OGRGetDayOfWeek.
// TODO(schwehr): Test OGRGetRFC822DateTime.
// TODO(schwehr): Test OGRGetXMLDateTime.
// TODO(schwehr): Test OGRGetXML_UTF8_EscapedString.
// TODO(schwehr): Test OGRCompareDate.
// TODO(schwehr): Test OGRFastAtof.
// TODO(schwehr): Test OGRCheckPermutation.
// TODO(schwehr): Test OGRReadWKBGeometryType.

}  // namespace
}  // namespace autotest2
