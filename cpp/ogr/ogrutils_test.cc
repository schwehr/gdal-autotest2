// Copyright 2017 Google Inc. All Rights Reserved.
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
// Test ogrutils.cpp.

#define GDAL_COMPILATION

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

constexpr int kOGR_G_3D = 0x2;
constexpr int kOGR_G_MEASURED = 0x4;

TEST(OGRFormatDoubleTest, Edges) {
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
  EXPECT_STREQ("1.23456", buf);
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

TEST(OGRFormatDoubleTest, SpecialValues) {
  char buf[64] = {};
  OGRFormatDouble(buf, sizeof(buf), std::numeric_limits<double>::infinity(),
                  '.', 2, 'f');
  EXPECT_STREQ("inf", buf);
  OGRFormatDouble(buf, sizeof(buf), -std::numeric_limits<double>::infinity(),
                  '.', 2, 'f');
  EXPECT_STREQ("-inf", buf);
  OGRFormatDouble(buf, sizeof(buf), std::numeric_limits<double>::quiet_NaN(),
                  '.', 2, 'f');
  EXPECT_STREQ("nan", buf);

  EXPECT_EQ(OGRFormatDouble(std::numeric_limits<double>::infinity(),
                            OGRWktOptions(), 1),
            "inf");
  EXPECT_EQ(OGRFormatDouble(-std::numeric_limits<double>::infinity(),
                            OGRWktOptions(), 1),
            "-inf");
  EXPECT_EQ(OGRFormatDouble(std::numeric_limits<double>::quiet_NaN(),
                            OGRWktOptions(), 1),
            "nan");
}

TEST(OGRFormatDoubleTest, CustomDecimalSep) {
  char buf[64] = {};
  OGRFormatDouble(buf, sizeof(buf), 1.5, ',', 2, 'f');
  EXPECT_STREQ("1,5", buf);
}

TEST(OGRFormatDoubleTest, WktOptions) {
  OGRWktOptions opts;
  opts.format = OGRWktFormat::F;
  opts.xyPrecision = 3;
  opts.zPrecision = 2;
  opts.mPrecision = 1;
  EXPECT_EQ(OGRFormatDouble(1.23456, opts, 1), "1.235");
  EXPECT_EQ(OGRFormatDouble(1.23456, opts, 2), "1.235");
  EXPECT_EQ(OGRFormatDouble(1.23456, opts, 3), "1.23");
  EXPECT_EQ(OGRFormatDouble(1.23456, opts, 4), "1.2");

  OGRWktOptions defaultOpts;
  defaultOpts.format = OGRWktFormat::Default;
  defaultOpts.xyPrecision = 4;
  EXPECT_EQ(OGRFormatDouble(0.123456, defaultOpts, 1), "0.1235");
  EXPECT_EQ(OGRFormatDouble(12.3456, defaultOpts, 1), "12.35");
}

TEST(OGRFormatDoubleTest, IntelliRound) {
  OGRWktOptions opts;
  opts.format = OGRWktFormat::F;
  opts.xyPrecision = 10;
  opts.round = true;

  // Trailing 00000x -> pop_back
  EXPECT_EQ(OGRFormatDouble(1.2345000001, opts, 1), "1.2345");
  // Large number with trailing zeros -> lines 161-168
  EXPECT_EQ(OGRFormatDouble(12345.000000011, opts, 1), "12345.0");

  // Trailing 99999x -> roundup
  EXPECT_EQ(OGRFormatDouble(1.2345999998, opts, 1), "1.2346");
  EXPECT_EQ(OGRFormatDouble(9.9999999998, opts, 1), "10.0");
  EXPECT_EQ(OGRFormatDouble(-9.9999999998, opts, 1), "-10.0");
  EXPECT_EQ(OGRFormatDouble(12345.999999988, opts, 1), "12346.0");
}

TEST(OGRMakeWktCoordinateTest, Basic) {
  char buf[128] = {};
  OGRMakeWktCoordinate(buf, 0.0, 0.0, 0.0, 2);
  EXPECT_STREQ("0 0", buf);
  OGRMakeWktCoordinate(buf, 1.0, 2.0, 3.0, 3);
  EXPECT_STREQ("1 2 3", buf);
  OGRMakeWktCoordinate(buf, 1.25, 2.5, 3.75, 3);
  EXPECT_STREQ("1.25 2.5 3.75", buf);

  OGRWktOptions opts;
  opts.format = OGRWktFormat::F;
  opts.xyPrecision = 0;
  opts.zPrecision = 0;
  EXPECT_EQ(OGRMakeWktCoordinate(1.0, 2.0, 3.0, 3, opts), "1.0 2.0 3");
}

TEST(OGRMakeWktCoordinateMTest, Basic) {
  char buf[128] = {};
  OGRMakeWktCoordinateM(buf, 1.0, 2.0, 0.0, 0.0, FALSE, FALSE);
  EXPECT_STREQ("1 2", buf);
  OGRMakeWktCoordinateM(buf, 1.0, 2.0, 3.0, 0.0, TRUE, FALSE);
  EXPECT_STREQ("1 2 3", buf);
  OGRMakeWktCoordinateM(buf, 1.0, 2.0, 0.0, 4.0, FALSE, TRUE);
  EXPECT_STREQ("1 2 4", buf);
  OGRMakeWktCoordinateM(buf, 1.0, 2.0, 3.0, 4.0, TRUE, TRUE);
  EXPECT_STREQ("1 2 3 4", buf);

  OGRWktOptions opts;
  opts.format = OGRWktFormat::F;
  opts.xyPrecision = 0;
  opts.zPrecision = 0;
  opts.mPrecision = 0;
  EXPECT_EQ(OGRMakeWktCoordinateM(1.0, 2.0, 3.0, 4.0, TRUE, TRUE, opts),
            "1.0 2.0 3 4");

  OGRWktOptions optsFractional;
  optsFractional.format = OGRWktFormat::F;
  optsFractional.xyPrecision = 2;
  optsFractional.zPrecision = 2;
  optsFractional.mPrecision = 2;
  EXPECT_EQ(
      OGRMakeWktCoordinateM(1.25, 2.5, 3.75, 4.5, TRUE, TRUE, optsFractional),
      "1.25 2.5 3.75 4.5");
}

TEST(OGRWktReadTokenTest, Basic) {
  char token[OGR_WKT_TOKEN_MAX] = {};
  EXPECT_EQ(OGRWktReadToken(nullptr, token), nullptr);

  const char* psz = " \t\r\n(POINT, 123.45)   ";
  psz = OGRWktReadToken(psz, token);
  EXPECT_STREQ("(", token);
  psz = OGRWktReadToken(psz, token);
  EXPECT_STREQ("POINT", token);
  psz = OGRWktReadToken(psz, token);
  EXPECT_STREQ(",", token);
  psz = OGRWktReadToken(psz, token);
  EXPECT_STREQ("123.45", token);
  psz = OGRWktReadToken(psz, token);
  EXPECT_STREQ(")", token);
  EXPECT_STREQ("", psz);

  // Long token truncation at OGR_WKT_TOKEN_MAX - 1
  std::string longToken(100, 'A');
  psz = OGRWktReadToken(longToken.c_str(), token);
  EXPECT_EQ(strlen(token), OGR_WKT_TOKEN_MAX - 1);
}

TEST(OGRWktReadPointsTest, Basic) {
  OGRRawPoint* points = nullptr;
  double* padfZ = nullptr;
  int maxPoints = 0;
  int readPoints = 0;

  EXPECT_EQ(OGRWktReadPoints(nullptr, &points, &padfZ, &maxPoints, &readPoints),
            nullptr);

  // Not starting with '('
  EXPECT_STREQ(
      OGRWktReadPoints("POINT", &points, &padfZ, &maxPoints, &readPoints),
      "POINT");

  // Bad tokens
  EXPECT_EQ(
      OGRWktReadPoints("(abc 1)", &points, &padfZ, &maxPoints, &readPoints),
      nullptr);
  EXPECT_EQ(
      OGRWktReadPoints("(1 abc)", &points, &padfZ, &maxPoints, &readPoints),
      nullptr);

  // 2D points
  const char* ret =
      OGRWktReadPoints("(1 2, 3 4)", &points, &padfZ, &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 2);
  EXPECT_DOUBLE_EQ(points[0].x, 1.0);
  EXPECT_DOUBLE_EQ(points[0].y, 2.0);
  EXPECT_DOUBLE_EQ(points[1].x, 3.0);
  EXPECT_DOUBLE_EQ(points[1].y, 4.0);
  EXPECT_EQ(padfZ, nullptr);

  // 3D points and 3D then 2D
  ret = OGRWktReadPoints("(1 2 3, 4 5)", &points, &padfZ, &maxPoints,
                         &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 2);
  EXPECT_NE(padfZ, nullptr);
  EXPECT_DOUBLE_EQ(padfZ[0], 3.0);
  EXPECT_DOUBLE_EQ(padfZ[1], 0.0);

  // 4D points (M is skipped)
  ret = OGRWktReadPoints("(1 2 3 4, 5 6 7 8)", &points, &padfZ, &maxPoints,
                         &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 2);

  // Grow buffer beyond maxPoints with padfZ != nullptr
  maxPoints = 1;
  ret = OGRWktReadPoints("(1 2 3, 4 5 6, 7 8 9, 10 11 12)", &points, &padfZ,
                         &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 4);

  // Corrupt delimiter
  EXPECT_EQ(
      OGRWktReadPoints("(1 2 ; 3 4)", &points, &padfZ, &maxPoints, &readPoints),
      nullptr);

  CPLFree(points);
  CPLFree(padfZ);
}

TEST(OGRWktReadPointsMTest, Basic) {
  OGRRawPoint* points = nullptr;
  double* padfZ = nullptr;
  double* padfM = nullptr;
  int flags = 0;
  int maxPoints = 0;
  int readPoints = 0;

  EXPECT_EQ(OGRWktReadPointsM(nullptr, &points, &padfZ, &padfM, &flags,
                              &maxPoints, &readPoints),
            nullptr);
  EXPECT_STREQ(OGRWktReadPointsM("POINT", &points, &padfZ, &padfM, &flags,
                                 &maxPoints, &readPoints),
               "POINT");
  EXPECT_EQ(OGRWktReadPointsM("(abc 1)", &points, &padfZ, &padfM, &flags,
                              &maxPoints, &readPoints),
            nullptr);
  EXPECT_EQ(OGRWktReadPointsM("(1 abc)", &points, &padfZ, &padfM, &flags,
                              &maxPoints, &readPoints),
            nullptr);

  // bNoFlags: 2D
  flags = 0;
  const char* ret = OGRWktReadPointsM("(1 2, 3 4)", &points, &padfZ, &padfM,
                                      &flags, &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 2);
  EXPECT_EQ(flags, 0);

  // bNoFlags: 3D -> auto-sets OGR_G_3D
  flags = 0;
  ret = OGRWktReadPointsM("(1 2 3, 4 5 6)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(flags, kOGR_G_3D);
  EXPECT_DOUBLE_EQ(padfZ[0], 3.0);

  // bNoFlags: 4D -> auto-sets OGR_G_3D | OGR_G_MEASURED
  flags = 0;
  ret = OGRWktReadPointsM("(1 2 3 4, 5 6 7 8)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(flags, kOGR_G_3D | kOGR_G_MEASURED);
  EXPECT_DOUBLE_EQ(padfZ[0], 3.0);
  EXPECT_DOUBLE_EQ(padfM[0], 4.0);

  // nan coordinates
  flags = 0;
  ret = OGRWktReadPointsM("(nan nan nan nan)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_TRUE(std::isnan(points[0].x));
  EXPECT_TRUE(std::isnan(points[0].y));
  EXPECT_TRUE(std::isnan(padfZ[0]));
  EXPECT_TRUE(std::isnan(padfM[0]));

  // flags = OGR_G_3D: 3D then 2D
  flags = kOGR_G_3D;
  ret = OGRWktReadPointsM("(1 2 3, 4 5)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_DOUBLE_EQ(padfZ[0], 3.0);
  EXPECT_DOUBLE_EQ(padfZ[1], 0.0);

  // flags = OGR_G_3D: 4D input discards 4th coordinate when bNoFlags is false
  flags = kOGR_G_3D;
  ret = OGRWktReadPointsM("(1 2 3 4)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);

  // flags = OGR_G_MEASURED: 3 coords (XYM) then 2 coords (XY)
  flags = kOGR_G_MEASURED;
  ret = OGRWktReadPointsM("(1 2 3, 4 5)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_DOUBLE_EQ(padfM[0], 3.0);
  EXPECT_DOUBLE_EQ(padfM[1], 0.0);

  // flags = OGR_G_MEASURED: 4 coords (XYZM) upgrades flags to OGR_G_3D and
  // shifts M to Z
  flags = kOGR_G_MEASURED;
  ret = OGRWktReadPointsM("(1 2 3, 4 5 6 7)", &points, &padfZ, &padfM, &flags,
                          &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_TRUE(flags & kOGR_G_3D);
  EXPECT_DOUBLE_EQ(padfZ[1], 6.0);
  EXPECT_DOUBLE_EQ(padfM[1], 7.0);

  // flags = OGR_G_3D | OGR_G_MEASURED: 4D then 3D then 2D
  flags = kOGR_G_3D | kOGR_G_MEASURED;
  ret = OGRWktReadPointsM("(1 2 3 4, 5 6 7, 8 9)", &points, &padfZ, &padfM,
                          &flags, &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 3);
  EXPECT_DOUBLE_EQ(padfZ[2], 0.0);
  EXPECT_DOUBLE_EQ(padfM[2], 0.0);

  // Buffer growth across maxPoints with both padfZ and padfM
  maxPoints = 1;
  flags = kOGR_G_3D | kOGR_G_MEASURED;
  ret = OGRWktReadPointsM("(1 2 3 4, 5 6 7 8, 9 10 11 12)", &points, &padfZ,
                          &padfM, &flags, &maxPoints, &readPoints);
  EXPECT_NE(ret, nullptr);
  EXPECT_EQ(readPoints, 3);

  // Corrupt delimiter
  EXPECT_EQ(OGRWktReadPointsM("(1 2 ; 3 4)", &points, &padfZ, &padfM, &flags,
                              &maxPoints, &readPoints),
            nullptr);

  CPLFree(points);
  CPLFree(padfZ);
  CPLFree(padfM);
}

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

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 11);
  EXPECT_EQ(field.Date.Day, 31);
  EXPECT_EQ(field.Date.Hour, 12);
  EXPECT_EQ(field.Date.Minute, 34);
  EXPECT_EQ(field.Date.Second, 56.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56+00", &field, 0), TRUE);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56+12:00", &field, 0), TRUE);
  EXPECT_EQ(field.Date.TZFlag, 100 + 12 * 4);

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56+1200", &field, 0), TRUE);
  EXPECT_EQ(field.Date.TZFlag, 100 + 12 * 4);

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56+815", &field, 0), TRUE);
  EXPECT_EQ(field.Date.TZFlag, 100 + 8 * 4 + 1);

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56-12:00", &field, 0), TRUE);
  EXPECT_EQ(field.Date.TZFlag, 100 - 12 * 4);

  EXPECT_EQ(OGRParseDate(" 2017/11/31 12:34:56", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);

  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:56.789", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Second, 56.789f);

  // Leap second
  EXPECT_EQ(OGRParseDate("2017/11/31 12:34:60", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Second, 60.0f);

  EXPECT_EQ(OGRParseDate("2017-11-31T12:34:56", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 11);
  EXPECT_EQ(field.Date.Day, 31);
  EXPECT_EQ(field.Date.Hour, 12);
  EXPECT_EQ(field.Date.Minute, 34);
  EXPECT_EQ(field.Date.Second, 56.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("2017-11-31T12:34:56Z", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Second, 56.0f);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_EQ(OGRParseDate("2017-11-31T12:34:56.789Z", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Second, 56.789f);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_EQ(OGRParseDate("2017-11-31T23:59:59.999999Z", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Hour, 23);
  EXPECT_EQ(field.Date.Minute, 59);
  EXPECT_EQ(field.Date.Second, 59.999f);

  EXPECT_EQ(OGRParseDate("2017-11-31", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 11);
  EXPECT_EQ(field.Date.Day, 31);
  EXPECT_EQ(field.Date.Hour, 0);
  EXPECT_EQ(field.Date.Minute, 0);
  EXPECT_EQ(field.Date.Second, 0.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("2017-11-31Z", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 11);
  EXPECT_EQ(field.Date.Day, 31);
  EXPECT_EQ(field.Date.Hour, 0);
  EXPECT_EQ(field.Date.Minute, 0);
  EXPECT_EQ(field.Date.Second, 0.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("12:34", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 0);
  EXPECT_EQ(field.Date.Month, 0);
  EXPECT_EQ(field.Date.Day, 0);
  EXPECT_EQ(field.Date.Hour, 12);
  EXPECT_EQ(field.Date.Minute, 34);
  EXPECT_EQ(field.Date.Second, 0.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("12:34:56", &field, 0), TRUE);
  EXPECT_EQ(OGRParseDate("12:34:56.789", &field, 0), TRUE);

  EXPECT_EQ(OGRParseDate("T12:34:56", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 0);
  EXPECT_EQ(field.Date.Month, 0);
  EXPECT_EQ(field.Date.Day, 0);
  EXPECT_EQ(field.Date.Hour, 12);
  EXPECT_EQ(field.Date.Minute, 34);
  EXPECT_EQ(field.Date.Second, 56.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("T123456", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 0);
  EXPECT_EQ(field.Date.Month, 0);
  EXPECT_EQ(field.Date.Day, 0);
  EXPECT_EQ(field.Date.Hour, 12);
  EXPECT_EQ(field.Date.Minute, 34);
  EXPECT_EQ(field.Date.Second, 56.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("T123456.789", &field, 0), TRUE);
  EXPECT_EQ(field.Date.Year, 0);
  EXPECT_EQ(field.Date.Month, 0);
  EXPECT_EQ(field.Date.Day, 0);
  EXPECT_EQ(field.Date.Hour, 12);
  EXPECT_EQ(field.Date.Minute, 34);
  EXPECT_EQ(field.Date.Second, 56.789f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  {
    WithQuietHandler quiet;
    EXPECT_TRUE(!OGRParseDate("123456-01-01", &field, 0));
  }
  EXPECT_TRUE(!OGRParseDate("2017", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017x-01-01", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-1-01", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-1", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01x", &field, 0));
  EXPECT_TRUE(!OGRParseDate("12:", &field, 0));
  EXPECT_TRUE(!OGRParseDate("12:3", &field, 0));
  EXPECT_TRUE(!OGRParseDate("1:23", &field, 0));
  EXPECT_TRUE(!OGRParseDate("12:34:5", &field, 0));
  EXPECT_TRUE(!OGRParseDate("1a:34", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-a-31T12:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-00-31T12:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-13-31T12:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-00T12:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-aT12:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-32T12:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("a:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01Ta:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01T25:34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01T00:a:00", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01T00: 34:56", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01T00:61:00", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01T00:00:61", &field, 0));
  EXPECT_TRUE(!OGRParseDate("2017-01-01T00:00:a", &field, 0));

  // Test OGRPARSEDATE_OPTION_LAX
  EXPECT_EQ(OGRParseDate("2017-1-9", &field, OGRPARSEDATE_OPTION_LAX), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 1);
  EXPECT_EQ(field.Date.Day, 9);

  EXPECT_EQ(OGRParseDate("2017-1-31", &field, OGRPARSEDATE_OPTION_LAX), TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 1);
  EXPECT_EQ(field.Date.Day, 31);

  EXPECT_EQ(OGRParseDate("2017-1-31T1:2:3", &field, OGRPARSEDATE_OPTION_LAX),
            TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 1);
  EXPECT_EQ(field.Date.Day, 31);
  EXPECT_EQ(field.Date.Hour, 1);
  EXPECT_EQ(field.Date.Minute, 2);
  EXPECT_EQ(field.Date.Second, 3.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("2017-1-31T1:3", &field, OGRPARSEDATE_OPTION_LAX),
            TRUE);
  EXPECT_EQ(field.Date.Year, 2017);
  EXPECT_EQ(field.Date.Month, 1);
  EXPECT_EQ(field.Date.Day, 31);
  EXPECT_EQ(field.Date.Hour, 1);
  EXPECT_EQ(field.Date.Minute, 3);
  EXPECT_EQ(field.Date.Second, 0.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_EQ(OGRParseDate("1:3", &field, OGRPARSEDATE_OPTION_LAX), TRUE);
  EXPECT_EQ(field.Date.Year, 0);
  EXPECT_EQ(field.Date.Month, 0);
  EXPECT_EQ(field.Date.Day, 0);
  EXPECT_EQ(field.Date.Hour, 1);
  EXPECT_EQ(field.Date.Minute, 3);
  EXPECT_EQ(field.Date.Second, 0.0f);
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_TRUE(!OGRParseDate("2017-a-01", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-0-01", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1-", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1-a", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1-0", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1-32", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(
      !OGRParseDate("2017-1-1Ta:00:00", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1-1T1", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(
      !OGRParseDate("2017-1-1T00:a:00", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("2017-1-1T1:", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(
      !OGRParseDate("2017-1-1T00:00:a", &field, OGRPARSEDATE_OPTION_LAX));
  EXPECT_TRUE(!OGRParseDate("1a:3", &field, OGRPARSEDATE_OPTION_LAX));
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

TEST(OGRTimezoneTest, Conversions) {
  EXPECT_EQ(OGRTimezoneToTZFlag("UTC", true), 100);
  EXPECT_EQ(OGRTimezoneToTZFlag("Etc/UTC", true), 100);
  EXPECT_EQ(OGRTimezoneToTZFlag("+00:00", true), 100);
  EXPECT_EQ(OGRTimezoneToTZFlag("+02:15", true), 109);
  EXPECT_EQ(OGRTimezoneToTZFlag("-5:45", true), 77);
  EXPECT_EQ(OGRTimezoneToTZFlag("", true), 0);
  EXPECT_EQ(OGRTimezoneToTZFlag("+99:00", true), 0);
  EXPECT_EQ(OGRTimezoneToTZFlag("+01:99", true), 0);
  EXPECT_EQ(OGRTimezoneToTZFlag("+01:10", true), 0);

  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRTimezoneToTZFlag("America/New_York", true), 0);
  }
  EXPECT_EQ(OGRTimezoneToTZFlag("America/New_York", false), 0);

  EXPECT_EQ(OGRTZFlagToTimezone(100, "Z"), "Z");
  EXPECT_EQ(OGRTZFlagToTimezone(100, "UTC"), "UTC");
  EXPECT_EQ(OGRTZFlagToTimezone(109, "Z"), "+02:15");
  EXPECT_EQ(OGRTZFlagToTimezone(77, "Z"), "-05:45");
  EXPECT_EQ(OGRTZFlagToTimezone(0, "Z"), "");
  EXPECT_EQ(OGRTZFlagToTimezone(1, "Z"), "");
}

TEST(OGRParseDateTest, AdditionalBranches) {
  OGRField field;
  // 2-digit years
  EXPECT_TRUE(OGRParseDate("85-05-01", &field, 0));
  EXPECT_EQ(field.Date.Year, 1985);
  EXPECT_TRUE(OGRParseDate("15-05-01", &field, 0));
  EXPECT_EQ(field.Date.Year, 2015);

  // Negative timezone offsets
  EXPECT_TRUE(OGRParseDate("2017/11/31 12:34:56-0530", &field, 0));
  EXPECT_EQ(field.Date.TZFlag, 100 - 5 * 4 - 2);
  EXPECT_TRUE(OGRParseDate("2017/11/31 12:34:56-530", &field, 0));
  EXPECT_EQ(field.Date.TZFlag, 100 - 5 * 4 - 2);

  // Positive timezone offsets
  EXPECT_TRUE(OGRParseDate("2017/11/31 12:34:56+0530", &field, 0));
  EXPECT_EQ(field.Date.TZFlag, 100 + 5 * 4 + 2);
  EXPECT_TRUE(OGRParseDate("2017/11/31 12:34:56+530", &field, 0));
  EXPECT_EQ(field.Date.TZFlag, 100 + 5 * 4 + 2);
}

TEST(OGRParseDateTimeFastTest, YYYYMMDDTHHMMZ) {
  OGRField field;
  EXPECT_TRUE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15T14:30Z", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.Month, 5);
  EXPECT_EQ(field.Date.Day, 15);
  EXPECT_EQ(field.Date.Hour, 14);
  EXPECT_EQ(field.Date.Minute, 30);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_TRUE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15T14:30", &field));
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15T14:3", &field));
  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15T14:30:00", &field));
  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15X14:30Z", &field));
  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-13-15T14:30Z", &field));
  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-32T14:30Z", &field));
  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15T25:30Z", &field));
  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMZ("2023-05-15T14:60Z", &field));
}

TEST(OGRParseDateTimeFastTest, YYYYMMDDTHHMMSSZ) {
  OGRField field;
  EXPECT_TRUE(OGRParseDateTimeYYYYMMDDTHHMMSSZ("2023-05-15T14:30:45Z", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.Month, 5);
  EXPECT_EQ(field.Date.Day, 15);
  EXPECT_EQ(field.Date.Hour, 14);
  EXPECT_EQ(field.Date.Minute, 30);
  EXPECT_FLOAT_EQ(field.Date.Second, 45.0f);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_TRUE(OGRParseDateTimeYYYYMMDDTHHMMSSZ("2023-05-15T14:30:45", &field));
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_FALSE(OGRParseDateTimeYYYYMMDDTHHMMSSZ("2023-05-15T14:30:4", &field));
  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSZ("2023-05-15T14:30:450", &field));
  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSZ("2023-05-15X14:30:45Z", &field));
  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSZ("2023-05-15T14:30:62Z", &field));
}

TEST(OGRParseDateTimeFastTest, YYYYMMDDTHHMMSSsssZ) {
  OGRField field;
  EXPECT_TRUE(
      OGRParseDateTimeYYYYMMDDTHHMMSSsssZ("2023-05-15T14:30:45.123Z", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.Month, 5);
  EXPECT_EQ(field.Date.Day, 15);
  EXPECT_EQ(field.Date.Hour, 14);
  EXPECT_EQ(field.Date.Minute, 30);
  EXPECT_NEAR(field.Date.Second, 45.123f, 1e-3);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_TRUE(
      OGRParseDateTimeYYYYMMDDTHHMMSSsssZ("2023-05-15T14:30:45.123", &field));
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSsssZ("2023-05-15T14:30:45.12", &field));
  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSsssZ("2023-05-15T14:30:45.1234", &field));
  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSsssZ("2023-05-15T14:30:45X123Z", &field));
  EXPECT_FALSE(
      OGRParseDateTimeYYYYMMDDTHHMMSSsssZ("2023-05-15T14:30:45.abcZ", &field));
}

TEST(OGRDateTimeHelpersTest, ParseXMLDateTime) {
  OGRField field;
  EXPECT_TRUE(OGRParseXMLDateTime("2023-05-15T14:30:45Z", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_TRUE(OGRParseXMLDateTime("2023-05-15T14:30:45.123+02:30", &field));
  EXPECT_EQ(field.Date.TZFlag, 100 + 2 * 4 + 2);

  EXPECT_TRUE(OGRParseXMLDateTime("2023-05-15T14:30:45-05:00", &field));
  EXPECT_EQ(field.Date.TZFlag, 100 - 5 * 4);

  EXPECT_TRUE(OGRParseXMLDateTime("2023-05-15T14:30:45", &field));
  EXPECT_EQ(field.Date.TZFlag, 0);

  EXPECT_TRUE(OGRParseXMLDateTime("2023-05-15", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.Month, 5);
  EXPECT_EQ(field.Date.Day, 15);

  EXPECT_TRUE(OGRParseXMLDateTime("2023-05", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.Month, 5);

  EXPECT_FALSE(OGRParseXMLDateTime("invalid-date", &field));
}

TEST(OGRDateTimeHelpersTest, ParseRFC822DateTime) {
  OGRField field;
  EXPECT_TRUE(OGRParseRFC822DateTime("Mon, 15 May 2023 14:30:45 GMT", &field));
  EXPECT_EQ(field.Date.Year, 2023);
  EXPECT_EQ(field.Date.Month, 5);
  EXPECT_EQ(field.Date.Day, 15);
  EXPECT_EQ(field.Date.Hour, 14);
  EXPECT_EQ(field.Date.Minute, 30);
  EXPECT_FLOAT_EQ(field.Date.Second, 45.0f);
  EXPECT_EQ(field.Date.TZFlag, 100);

  EXPECT_TRUE(OGRParseRFC822DateTime("15 May 2023 14:30 GMT", &field));
  EXPECT_EQ(field.Date.Second, 0.0f);

  EXPECT_TRUE(OGRParseRFC822DateTime("15 May 85 14:30:45 +0200", &field));
  EXPECT_EQ(field.Date.Year, 1985);
  EXPECT_EQ(field.Date.TZFlag, 108);

  EXPECT_TRUE(OGRParseRFC822DateTime("15 May 15 14:30:45 -0500", &field));
  EXPECT_EQ(field.Date.Year, 2015);
  EXPECT_EQ(field.Date.TZFlag, 80);

  EXPECT_FALSE(
      OGRParseRFC822DateTime("15 InvalidMonth 2023 14:30:45 GMT", &field));
  EXPECT_FALSE(OGRParseRFC822DateTime("short", &field));
}

TEST(OGRDateTimeHelpersTest, GetDayOfWeek) {
  // Jan 15, 2023 is Sunday (6)
  EXPECT_EQ(OGRGetDayOfWeek(15, 1, 2023), 6);
  // May 15, 2023 is Monday (0)
  EXPECT_EQ(OGRGetDayOfWeek(15, 5, 2023), 0);
}

TEST(OGRDateTimeHelpersTest, GetRFC822DateTime) {
  OGRField field;
  field.Date.Year = 2023;
  field.Date.Month = 5;
  field.Date.Day = 15;
  field.Date.Hour = 14;
  field.Date.Minute = 30;
  field.Date.Second = 45.0f;
  field.Date.TZFlag = 100;

  char* psz = OGRGetRFC822DateTime(&field);
  ASSERT_NE(psz, nullptr);
  EXPECT_STREQ(psz, "Mon, 15 May 2023 14:30:45 GMT");
  CPLFree(psz);

  field.Date.TZFlag = 0;
  psz = OGRGetRFC822DateTime(&field);
  EXPECT_STREQ(psz, "Mon, 15 May 2023 14:30:45 GMT");
  CPLFree(psz);

  field.Date.TZFlag = 108;  // +0200
  psz = OGRGetRFC822DateTime(&field);
  EXPECT_STREQ(psz, "Mon, 15 May 2023 14:30:45 +0200");
  CPLFree(psz);

  field.Date.TZFlag = 92;  // -0200
  psz = OGRGetRFC822DateTime(&field);
  EXPECT_STREQ(psz, "Mon, 15 May 2023 14:30:45 -0200");
  CPLFree(psz);

  field.Date.Month = 0;
  psz = OGRGetRFC822DateTime(&field);
  EXPECT_STREQ(psz, "Thu, 15 Jan 2023 14:30:45 -0200");
  CPLFree(psz);
}

TEST(OGRDateTimeHelpersTest, GetXMLDateTime) {
  OGRField field;
  field.Date.Year = 2023;
  field.Date.Month = 5;
  field.Date.Day = 15;
  field.Date.Hour = 14;
  field.Date.Minute = 30;
  field.Date.Second = 45.0f;
  field.Date.TZFlag = 100;

  char* psz = OGRGetXMLDateTime(&field);
  ASSERT_NE(psz, nullptr);
  EXPECT_STREQ(psz, "2023-05-15T14:30:45Z");
  CPLFree(psz);

  psz = OGRGetXMLDateTime(&field, true);
  EXPECT_STREQ(psz, "2023-05-15T14:30:45.000Z");
  CPLFree(psz);

  field.Date.Second = 45.123f;
  psz = OGRGetXMLDateTime(&field, false);
  EXPECT_STREQ(psz, "2023-05-15T14:30:45.123Z");
  CPLFree(psz);

  field.Date.TZFlag = 108;  // +02:00
  psz = OGRGetXMLDateTime(&field);
  EXPECT_STREQ(psz, "2023-05-15T14:30:45.123+02:00");
  CPLFree(psz);

  field.Date.TZFlag = 92;  // -02:00
  psz = OGRGetXMLDateTime(&field);
  EXPECT_STREQ(psz, "2023-05-15T14:30:45.123-02:00");
  CPLFree(psz);

  field.Date.TZFlag = 0;
  psz = OGRGetXMLDateTime(&field);
  EXPECT_STREQ(psz, "2023-05-15T14:30:45.123");
  CPLFree(psz);
}

TEST(OGRDateTimeHelpersTest, GetISO8601DateTime) {
  WithQuietHandler quiet;
  char buf[OGR_SIZEOF_ISO8601_DATETIME_BUFFER] = {};
  OGRField field;
  field.Date.Year = 2023;
  field.Date.Month = 5;
  field.Date.Day = 15;
  field.Date.Hour = 14;
  field.Date.Minute = 30;
  field.Date.Second = 45.0f;
  field.Date.TZFlag = 100;

  EXPECT_GT(OGRGetISO8601DateTime(&field, false, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45Z");

  EXPECT_GT(OGRGetISO8601DateTime(&field, true, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45.000Z");

  // Out of range years
  field.Date.Year = -1;
  EXPECT_EQ(OGRGetISO8601DateTime(&field, false, buf), 0);
  field.Date.Year = 10000;
  EXPECT_EQ(OGRGetISO8601DateTime(&field, false, buf), 0);
  field.Date.Year = 2023;

  OGRISO8601Format sFormat;
  sFormat.ePrecision = OGRISO8601Precision::MINUTE;
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30Z");

  sFormat.ePrecision = OGRISO8601Precision::SECOND;
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45Z");

  sFormat.ePrecision = OGRISO8601Precision::MILLISECOND;
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45.000Z");

  sFormat.ePrecision = OGRISO8601Precision::AUTO;
  field.Date.Second = 45.123f;
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45.123Z");

  // Additional timezone flags
  field.Date.TZFlag = 108;  // +02:00
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45.123+02:00");

  field.Date.TZFlag = 92;  // -02:00
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45.123-02:00");

  field.Date.TZFlag = 0;  // Unknown/Local
  EXPECT_GT(OGRGetISO8601DateTime(&field, sFormat, buf), 0);
  EXPECT_STREQ(buf, "2023-05-15T14:30:45.123");
}

TEST(OGRDateTimeHelpersTest, OGR_GET_MS) {
  EXPECT_EQ(OGR_GET_MS(std::numeric_limits<float>::quiet_NaN()), 0);
  EXPECT_EQ(OGR_GET_MS(1000.0f), 999);
  EXPECT_EQ(OGR_GET_MS(-1.0f), 0);
  EXPECT_EQ(OGR_GET_MS(0.0f), 0);
  EXPECT_EQ(OGR_GET_MS(1.25f), 250);
}

TEST(OGRUtilsStringTest, GetXML_UTF8_EscapedString) {
  WithQuietHandler quiet;
  char* psz = OGRGetXML_UTF8_EscapedString("<a>&\"'</a>");
  ASSERT_NE(psz, nullptr);
  EXPECT_STREQ(psz, "&lt;a&gt;&amp;&quot;'&lt;/a&gt;");
  CPLFree(psz);

  // Invalid UTF-8 with FORCE_ASCII YES and NO
  CPLSetThreadLocalConfigOption("OGR_FORCE_ASCII", "YES");
  psz = OGRGetXML_UTF8_EscapedString("\xFF\xFE<test>");
  ASSERT_NE(psz, nullptr);
  CPLFree(psz);
  // Call again so bFirstTime is false
  psz = OGRGetXML_UTF8_EscapedString("\xFF\xFE<test2>");
  ASSERT_NE(psz, nullptr);
  CPLFree(psz);

  CPLSetThreadLocalConfigOption("OGR_FORCE_ASCII", "NO");
  psz = OGRGetXML_UTF8_EscapedString("\xFF\xFE<test3>");
  ASSERT_NE(psz, nullptr);
  CPLFree(psz);
  CPLSetThreadLocalConfigOption("OGR_FORCE_ASCII", nullptr);
}

TEST(OGRUtilsStringTest, DuplicateCharacter) {
  EXPECT_EQ(OGRDuplicateCharacter("", '\''), "");
  EXPECT_EQ(OGRDuplicateCharacter("a'b'c", '\''), "a''b''c");
  EXPECT_EQ(OGRDuplicateCharacter("abc", '\''), "abc");
}

TEST(OGRDateTimeHelpersTest, CompareDate) {
  OGRField f1, f2;
  memset(&f1, 0, sizeof(f1));
  memset(&f2, 0, sizeof(f2));

  f1.Date.Year = 2023;
  f2.Date.Year = 2022;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 1);
  EXPECT_EQ(OGRCompareDate(&f2, &f1), -1);

  f2.Date.Year = 2023;
  f1.Date.Month = 6;
  f2.Date.Month = 5;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 1);
  EXPECT_EQ(OGRCompareDate(&f2, &f1), -1);

  f2.Date.Month = 6;
  f1.Date.Day = 15;
  f2.Date.Day = 14;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 1);
  EXPECT_EQ(OGRCompareDate(&f2, &f1), -1);

  f2.Date.Day = 15;
  f1.Date.Hour = 12;
  f2.Date.Hour = 11;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 1);
  EXPECT_EQ(OGRCompareDate(&f2, &f1), -1);

  f2.Date.Hour = 12;
  f1.Date.Minute = 30;
  f2.Date.Minute = 29;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 1);
  EXPECT_EQ(OGRCompareDate(&f2, &f1), -1);

  f2.Date.Minute = 30;
  f1.Date.Second = 45.0f;
  f2.Date.Second = 44.0f;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 1);
  EXPECT_EQ(OGRCompareDate(&f2, &f1), -1);

  f2.Date.Second = 45.0f;
  EXPECT_EQ(OGRCompareDate(&f1, &f2), 0);
}

TEST(OGRFastAtofTest, Basic) {
  EXPECT_DOUBLE_EQ(OGRFastAtof("  +123.456"), 123.456);
  EXPECT_DOUBLE_EQ(OGRFastAtof("-123.456"), -123.456);
  EXPECT_DOUBLE_EQ(OGRFastAtof("1.5e2"), 150.0);
  EXPECT_DOUBLE_EQ(OGRFastAtof("150e-1"), 15.0);
  EXPECT_DOUBLE_EQ(OGRFastAtof("123"), 123.0);
  EXPECT_DOUBLE_EQ(OGRFastAtof("1.12345678901234567890123456789012345"),
                   1.1234567890123457);

  std::string longNum(150, '9');
  longNum[1] = '.';
  EXPECT_NE(OGRFastAtof(longNum.c_str()), 0.0);
}

TEST(OGRCheckPermutationTest, Basic) {
  const int permGood[] = {2, 0, 1};
  EXPECT_EQ(OGRCheckPermutation(permGood, 3), OGRERR_NONE);

  const int permBadNeg[] = {-1, 0, 1};
  EXPECT_EQ(OGRCheckPermutation(permBadNeg, 3), OGRERR_FAILURE);

  const int permBadLarge[] = {0, 3, 1};
  EXPECT_EQ(OGRCheckPermutation(permBadLarge, 3), OGRERR_FAILURE);

  const int permDup[] = {0, 1, 1};
  EXPECT_EQ(OGRCheckPermutation(permDup, 3), OGRERR_FAILURE);
}

TEST(OGRReadWKBGeometryTypeTest, Basic) {
  WithQuietHandler quiet;
  OGRwkbGeometryType eType;
  EXPECT_EQ(OGRReadWKBGeometryType(nullptr, wkbVariantOldOgc, nullptr),
            OGRERR_FAILURE);

  // Invalid byte order
  const unsigned char badOrder[] = {2, 0, 0, 0, 1};
  EXPECT_EQ(OGRReadWKBGeometryType(badOrder, wkbVariantOldOgc, &eType),
            OGRERR_CORRUPT_DATA);

  // Helper lambda to construct 5-byte WKB header
  auto testWKB = [](unsigned char byteOrder, uint32_t val,
                    OGRwkbVariant variant, OGRwkbGeometryType expected) {
    unsigned char buf[5] = {byteOrder, 0, 0, 0, 0};
    if (byteOrder == 1) {  // NDR (little endian)
      buf[1] = static_cast<unsigned char>(val & 0xFF);
      buf[2] = static_cast<unsigned char>((val >> 8) & 0xFF);
      buf[3] = static_cast<unsigned char>((val >> 16) & 0xFF);
      buf[4] = static_cast<unsigned char>((val >> 24) & 0xFF);
    } else {  // XDR (big endian)
      buf[1] = static_cast<unsigned char>((val >> 24) & 0xFF);
      buf[2] = static_cast<unsigned char>((val >> 16) & 0xFF);
      buf[3] = static_cast<unsigned char>((val >> 8) & 0xFF);
      buf[4] = static_cast<unsigned char>(val & 0xFF);
    }
    OGRwkbGeometryType outType;
    EXPECT_EQ(OGRReadWKBGeometryType(buf, variant, &outType), OGRERR_NONE);
    EXPECT_EQ(outType, expected);
  };

  // NDR 2D Point
  testWKB(1, 1, wkbVariantOldOgc, wkbPoint);
  // XDR 2D Point
  testWKB(0, 1, wkbVariantOldOgc, wkbPoint);

  // PostGIS Z flag (0x80000000)
  testWKB(1, 1 | 0x80000000, wkbVariantOldOgc, wkbPoint25D);
  // PostGIS M flag (0x40000000)
  testWKB(1, 1 | 0x40000000, wkbVariantOldOgc, wkbPointM);
  // PostGIS ZM flag (0xC0000000)
  testWKB(1, 1 | 0xC0000000, wkbVariantOldOgc, wkbPointZM);
  // Old 2.5D bit (0x8000)
  testWKB(1, 1 | 0x8000, wkbVariantOldOgc, wkbPoint25D);

  // Deprecated SQL/MM codes: 1000001..1000005
  testWKB(1, 1000001, wkbVariantOldOgc, wkbCircularString);
  testWKB(1, 1000002, wkbVariantOldOgc, wkbCompoundCurve);
  testWKB(1, 1000003, wkbVariantOldOgc, wkbCurvePolygon);
  testWKB(1, 1000004, wkbVariantOldOgc, wkbMultiCurve);
  testWKB(1, 1000005, wkbVariantOldOgc, wkbMultiSurface);

  // 2000001..2000012 (ZM)
  testWKB(1, 2000001, wkbVariantOldOgc, wkbPointZM);
  testWKB(1, 2000002, wkbVariantOldOgc, wkbLineStringZM);
  testWKB(1, 2000003, wkbVariantOldOgc, wkbCircularStringZM);
  testWKB(1, 2000004, wkbVariantOldOgc, wkbCompoundCurveZM);
  testWKB(1, 2000005, wkbVariantOldOgc, wkbPolygonZM);
  testWKB(1, 2000006, wkbVariantOldOgc, wkbCurvePolygonZM);
  testWKB(1, 2000007, wkbVariantOldOgc, wkbMultiPointZM);
  testWKB(1, 2000008, wkbVariantOldOgc, wkbMultiCurveZM);
  testWKB(1, 2000009, wkbVariantOldOgc, wkbMultiLineStringZM);
  testWKB(1, 2000010, wkbVariantOldOgc, wkbMultiSurfaceZM);
  testWKB(1, 2000011, wkbVariantOldOgc, wkbMultiPolygonZM);
  testWKB(1, 2000012, wkbVariantOldOgc, wkbGeometryCollectionZM);

  // 3000001..3000012 (Z / 25D)
  // Types that map to wkb*25D have 0x80000000 bit set, failing the 1..3017
  // range check
  auto testUnsupportedWKB = [](unsigned char byteOrder, uint32_t val) {
    unsigned char buf[5] = {byteOrder, 0, 0, 0, 0};
    buf[1] = static_cast<unsigned char>(val & 0xFF);
    buf[2] = static_cast<unsigned char>((val >> 8) & 0xFF);
    buf[3] = static_cast<unsigned char>((val >> 16) & 0xFF);
    buf[4] = static_cast<unsigned char>((val >> 24) & 0xFF);
    OGRwkbGeometryType outType;
    EXPECT_EQ(OGRReadWKBGeometryType(buf, wkbVariantOldOgc, &outType),
              OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  };
  testUnsupportedWKB(1, 3000001);
  testUnsupportedWKB(1, 3000002);
  testWKB(1, 3000003, wkbVariantOldOgc, wkbCircularStringZ);
  testWKB(1, 3000004, wkbVariantOldOgc, wkbCompoundCurveZ);
  testUnsupportedWKB(1, 3000005);
  testWKB(1, 3000006, wkbVariantOldOgc, wkbCurvePolygonZ);
  testUnsupportedWKB(1, 3000007);
  testWKB(1, 3000008, wkbVariantOldOgc, wkbMultiCurveZ);
  testUnsupportedWKB(1, 3000009);
  testWKB(1, 3000010, wkbVariantOldOgc, wkbMultiSurfaceZ);
  testUnsupportedWKB(1, 3000011);
  testUnsupportedWKB(1, 3000012);

  // 4000001..4000012 (M)
  testWKB(1, 4000001, wkbVariantOldOgc, wkbPointM);
  testWKB(1, 4000002, wkbVariantOldOgc, wkbLineStringM);
  testWKB(1, 4000003, wkbVariantOldOgc, wkbCircularStringM);
  testWKB(1, 4000004, wkbVariantOldOgc, wkbCompoundCurveM);
  testWKB(1, 4000005, wkbVariantOldOgc, wkbPolygonM);
  testWKB(1, 4000006, wkbVariantOldOgc, wkbCurvePolygonM);
  testWKB(1, 4000007, wkbVariantOldOgc, wkbMultiPointM);
  testWKB(1, 4000008, wkbVariantOldOgc, wkbMultiCurveM);
  testWKB(1, 4000009, wkbVariantOldOgc, wkbMultiLineStringM);
  testWKB(1, 4000010, wkbVariantOldOgc, wkbMultiSurfaceM);
  testWKB(1, 4000011, wkbVariantOldOgc, wkbMultiPolygonM);
  testWKB(1, 4000012, wkbVariantOldOgc, wkbGeometryCollectionM);

  // ISO types: 1001 (PointZ), 2001 (PointM), 3001 (PointZM)
  testWKB(1, 1001, wkbVariantIso, wkbPoint25D);
  testWKB(1, 2001, wkbVariantIso, wkbPointM);
  testWKB(1, 3001, wkbVariantIso, wkbPointZM);

  // wkbVariantPostGIS1
  testWKB(1, 13, wkbVariantPostGIS1, wkbCurvePolygon);
  testWKB(1, 14, wkbVariantPostGIS1, wkbMultiCurve);
  testWKB(1, 15, wkbVariantPostGIS1, wkbMultiSurface);

  // Unsupported geometry type
  unsigned char badType[5] = {1, 0xE7, 0x03, 0, 0};  // 999
  EXPECT_EQ(OGRReadWKBGeometryType(badType, wkbVariantOldOgc, &eType),
            OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
}

TEST(OGRReadWKTGeometryTypeTest, Basic) {
  OGRwkbGeometryType eType;
  EXPECT_EQ(OGRReadWKTGeometryType(nullptr, nullptr), OGRERR_FAILURE);
  EXPECT_EQ(OGRReadWKTGeometryType("POINT (0 0)", nullptr), OGRERR_FAILURE);

  EXPECT_EQ(OGRReadWKTGeometryType("POINT (0 0)", &eType), OGRERR_NONE);
  EXPECT_EQ(eType, wkbPoint);
  EXPECT_EQ(OGRReadWKTGeometryType("POINT Z (0 0 0)", &eType), OGRERR_NONE);
  EXPECT_EQ(eType, wkbPoint25D);
  EXPECT_EQ(OGRReadWKTGeometryType("POINT M (0 0 0)", &eType), OGRERR_NONE);
  EXPECT_EQ(eType, wkbPointM);
  EXPECT_EQ(OGRReadWKTGeometryType("POINT ZM (0 0 0 0)", &eType), OGRERR_NONE);
  EXPECT_EQ(eType, wkbPointZM);

  EXPECT_EQ(OGRReadWKTGeometryType("LINESTRING (0 0, 1 1)", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbLineString);
  EXPECT_EQ(OGRReadWKTGeometryType("LINESTRING Z (0 0 0, 1 1 1)", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbLineString25D);
  EXPECT_EQ(OGRReadWKTGeometryType("LINESTRING M (0 0 0, 1 1 1)", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbLineStringM);
  EXPECT_EQ(OGRReadWKTGeometryType("LINESTRING ZM (0 0 0 0, 1 1 1 1)", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbLineStringZM);

  EXPECT_EQ(OGRReadWKTGeometryType("POLYGON ((0 0, 1 1, 1 0, 0 0))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbPolygon);
  EXPECT_EQ(OGRReadWKTGeometryType("POLYGON Z ((0 0 0, 1 1 0, 1 0 0, 0 0 0))",
                                   &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbPolygon25D);
  EXPECT_EQ(OGRReadWKTGeometryType("POLYGON M ((0 0 0, 1 1 0, 1 0 0, 0 0 0))",
                                   &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbPolygonM);
  EXPECT_EQ(OGRReadWKTGeometryType(
                "POLYGON ZM ((0 0 0 0, 1 1 0 0, 1 0 0 0, 0 0 0 0))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbPolygonZM);

  EXPECT_EQ(OGRReadWKTGeometryType("MULTIPOINT ((0 0))", &eType), OGRERR_NONE);
  EXPECT_EQ(eType, wkbMultiPoint);
  EXPECT_EQ(OGRReadWKTGeometryType("MULTILINESTRING (((0 0, 1 1)))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbMultiLineString);
  EXPECT_EQ(
      OGRReadWKTGeometryType("MULTIPOLYGON ((((0 0, 1 1, 1 0, 0 0))))", &eType),
      OGRERR_NONE);
  EXPECT_EQ(eType, wkbMultiPolygon);
  EXPECT_EQ(OGRReadWKTGeometryType("GEOMETRYCOLLECTION (POINT (0 0))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbGeometryCollection);
  EXPECT_EQ(OGRReadWKTGeometryType("CIRCULARSTRING (0 0, 1 1, 2 0)", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbCircularString);
  EXPECT_EQ(OGRReadWKTGeometryType("COMPOUNDCURVE ((0 0, 1 1))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbCompoundCurve);
  EXPECT_EQ(
      OGRReadWKTGeometryType("CURVEPOLYGON (((0 0, 1 1, 1 0, 0 0)))", &eType),
      OGRERR_NONE);
  EXPECT_EQ(eType, wkbCurvePolygon);
  EXPECT_EQ(OGRReadWKTGeometryType("MULTICURVE ((0 0, 1 1))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbMultiCurve);
  EXPECT_EQ(
      OGRReadWKTGeometryType("MULTISURFACE ((((0 0, 1 1, 1 0, 0 0))))", &eType),
      OGRERR_NONE);
  EXPECT_EQ(eType, wkbMultiSurface);
  EXPECT_EQ(OGRReadWKTGeometryType(
                "POLYHEDRALSURFACE ((((0 0, 1 1, 1 0, 0 0))))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbPolyhedralSurface);
  EXPECT_EQ(OGRReadWKTGeometryType("TIN ((((0 0, 1 1, 1 0, 0 0))))", &eType),
            OGRERR_NONE);
  EXPECT_EQ(eType, wkbTIN);

  EXPECT_EQ(OGRReadWKTGeometryType("UNKNOWN_GEOMETRY (0 0)", &eType),
            OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
}

TEST(OGRFormatFloatTest, Basic) {
  char buf[64] = {};
  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf),
                           std::numeric_limits<float>::infinity(), 6, 'f'),
            0);
  EXPECT_STREQ(buf, "inf");
  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf),
                           -std::numeric_limits<float>::infinity(), 6, 'f'),
            0);
  EXPECT_STREQ(buf, "-inf");
  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf),
                           std::numeric_limits<float>::quiet_NaN(), 6, 'f'),
            0);
  EXPECT_STREQ(buf, "nan");

  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf), 1.0f, -1, 'f'), 0);
  EXPECT_STREQ(buf, "1.00000000");

  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf), 1.0f, -1, 'g'), 0);
  EXPECT_STREQ(buf, "1.0");

  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf), 1.0f, 0, 'f'), 0);
  EXPECT_STREQ(buf, "1.0");

  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf), 0.35f, 8, 'g'), 0);
  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf), 0.15f, 9, 'g'), 0);
  EXPECT_GT(OGRFormatFloat(buf, sizeof(buf), 1.2345678f, 8, 'f'), 0);
}

TEST(OGRUtilsMemoryTest, Basic) {
  void* ptr = OGRMalloc(16);
  EXPECT_NE(ptr, nullptr);
  ptr = OGRRealloc(ptr, 32);
  EXPECT_NE(ptr, nullptr);
  OGRFree(ptr);

  ptr = OGRCalloc(4, 4);
  EXPECT_NE(ptr, nullptr);
  OGRFree(ptr);

  char** papszArgs = nullptr;
  papszArgs = CSLAddString(papszArgs, "prog");
  papszArgs = CSLAddString(papszArgs, "arg1");
  int argc = CSLCount(papszArgs);
  char** papszArgsOrig = papszArgs;
  EXPECT_EQ(OGRGeneralCmdLineProcessor(argc, &papszArgs, 0), 2);
  CSLDestroy(papszArgsOrig);
  CSLDestroy(papszArgs);
}

}  // namespace
}  // namespace autotest2
