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
// TODO(schwehr): Test OGRParseDate.
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
