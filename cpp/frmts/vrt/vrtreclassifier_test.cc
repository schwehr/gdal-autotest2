// Copyright 2026 Google LLC
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
// Tests for vrtreclassifier.cpp.

#include "third_party/gdal/frmts/vrt/vrtreclassifier.h"

#include <cmath>
#include <limits>
#include <optional>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::gdal::Reclassifier;

TEST(VrtReclassifierTest, IntervalConstant) {
  Reclassifier::Interval interval;
  interval.SetToConstant(42.0);
  EXPECT_TRUE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(interval.dfMin, 42.0);
  EXPECT_DOUBLE_EQ(interval.dfMax, 42.0);
  EXPECT_TRUE(interval.Contains(42.0));
  EXPECT_FALSE(interval.Contains(41.9));
  EXPECT_FALSE(interval.Contains(42.1));
}

TEST(VrtReclassifierTest, IntervalOverlaps) {
  Reclassifier::Interval a;
  a.dfMin = 1.0;
  a.dfMax = 5.0;

  Reclassifier::Interval b;
  b.dfMin = 4.0;
  b.dfMax = 8.0;
  EXPECT_TRUE(a.Overlaps(b));
  EXPECT_TRUE(b.Overlaps(a));

  Reclassifier::Interval c;
  c.dfMin = 6.0;
  c.dfMax = 10.0;
  EXPECT_FALSE(a.Overlaps(c));
  EXPECT_FALSE(c.Overlaps(a));

  Reclassifier::Interval d;
  d.dfMin = 2.0;
  d.dfMax = 3.0;
  EXPECT_TRUE(a.Overlaps(d));
  EXPECT_TRUE(d.Overlaps(a));
}

TEST(VrtReclassifierTest, IntervalParseConstant) {
  Reclassifier::Interval interval;
  char* rest = nullptr;
  EXPECT_EQ(interval.Parse("  123.5  rest", &rest), CE_None);
  EXPECT_TRUE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(interval.dfMin, 123.5);
  EXPECT_DOUBLE_EQ(interval.dfMax, 123.5);
  ASSERT_NE(rest, nullptr);
  EXPECT_STREQ(rest, "  rest");

  // Parse constant with null rest
  EXPECT_EQ(interval.Parse("99.0", nullptr), CE_None);
  EXPECT_TRUE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(interval.dfMin, 99.0);
}

TEST(VrtReclassifierTest, IntervalParseClosedRange) {
  Reclassifier::Interval interval;
  char* rest = nullptr;
  EXPECT_EQ(interval.Parse(" [ 10.0, 20.0] ;", &rest), CE_None);
  EXPECT_FALSE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(interval.dfMin, 10.0);
  EXPECT_DOUBLE_EQ(interval.dfMax, 20.0);
  ASSERT_NE(rest, nullptr);
  EXPECT_STREQ(rest, " ;");
  EXPECT_TRUE(interval.Contains(10.0));
  EXPECT_TRUE(interval.Contains(20.0));
  EXPECT_TRUE(interval.Contains(15.0));
  EXPECT_FALSE(interval.Contains(9.99));
  EXPECT_FALSE(interval.Contains(20.01));
}

TEST(VrtReclassifierTest, IntervalParseOpenRange) {
  Reclassifier::Interval interval;
  EXPECT_EQ(interval.Parse("(10.0, 20.0)", nullptr), CE_None);
  EXPECT_FALSE(interval.IsConstant());
  EXPECT_GT(interval.dfMin, 10.0);
  EXPECT_LT(interval.dfMax, 20.0);
  EXPECT_FALSE(interval.Contains(10.0));
  EXPECT_FALSE(interval.Contains(20.0));
  EXPECT_TRUE(interval.Contains(15.0));
}

TEST(VrtReclassifierTest, IntervalParseInfinities) {
  Reclassifier::Interval interval;
  EXPECT_EQ(interval.Parse("[-inf, 0]", nullptr), CE_None);
  EXPECT_DOUBLE_EQ(interval.dfMin, -std::numeric_limits<double>::infinity());
  EXPECT_DOUBLE_EQ(interval.dfMax, 0.0);

  EXPECT_EQ(interval.Parse("(0, inf)", nullptr), CE_None);
  EXPECT_GT(interval.dfMin, 0.0);
  EXPECT_DOUBLE_EQ(interval.dfMax, std::numeric_limits<double>::infinity());
}

TEST(VrtReclassifierTest, IntervalParseErrors) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  Reclassifier::Interval interval;
  char* rest = nullptr;

  // Invalid start
  EXPECT_EQ(interval.Parse("abc", &rest), CE_Failure);
  EXPECT_EQ(interval.Parse("", &rest), CE_Failure);

  // Missing or invalid min number
  EXPECT_EQ(interval.Parse("[, 5]", &rest), CE_Failure);
  EXPECT_EQ(interval.Parse("[abc, 5]", &rest), CE_Failure);

  // Missing comma
  EXPECT_EQ(interval.Parse("[10 20]", &rest), CE_Failure);

  // Missing or invalid max number
  EXPECT_EQ(interval.Parse("[10, ]", &rest), CE_Failure);
  EXPECT_EQ(interval.Parse("[10, abc]", &rest), CE_Failure);

  // Invalid end bracket
  EXPECT_EQ(interval.Parse("[10, 20}", &rest), CE_Failure);
  EXPECT_EQ(interval.Parse("[10, 20", &rest), CE_Failure);

  // NaN bounds
  EXPECT_EQ(interval.Parse("[nan, 20]", &rest), CE_Failure);
  EXPECT_EQ(interval.Parse("[10, nan]", &rest), CE_Failure);

  // Min > Max
  EXPECT_EQ(interval.Parse("[20, 10]", &rest), CE_Failure);

  CPLPopErrorHandler();
}

TEST(VrtReclassifierTest, OverlappingIntervalsFinalize) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  Reclassifier reclass;
  Reclassifier::Interval a;
  a.dfMin = 1.0;
  a.dfMax = 5.0;
  reclass.AddMapping(a, 10.0);

  Reclassifier::Interval b;
  b.dfMin = 4.0;
  b.dfMax = 8.0;
  reclass.AddMapping(b, std::nullopt);

  EXPECT_EQ(reclass.Finalize(), CE_Failure);
  CPLPopErrorHandler();
}

TEST(VrtReclassifierTest, InitBasicMappings) {
  Reclassifier reclass;
  EXPECT_EQ(reclass.Init("1  =  10; 2=20; [3, 5]=30; (5, 10)=40",
                         /*noDataValue=*/std::nullopt, GDT_Float64),
            CE_None);

  bool found = false;
  EXPECT_DOUBLE_EQ(reclass.Reclassify(1.0, found), 10.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(2.0, found), 20.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(4.0, found), 30.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(7.0, found), 40.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(0.0, found), 0.0);
  EXPECT_FALSE(found);
}

TEST(VrtReclassifierTest, InitNoDataMappings) {
  Reclassifier reclass;
  EXPECT_EQ(reclass.Init("NO_DATA=99; 5=NO_DATA", /*noDataValue=*/-9999.0,
                         GDT_Float64),
            CE_None);

  bool found = false;
  EXPECT_DOUBLE_EQ(reclass.Reclassify(-9999.0, found), 99.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(5.0, found), -9999.0);
  EXPECT_TRUE(found);
}

TEST(VrtReclassifierTest, InitNaNDefaultPassThrough) {
  Reclassifier reclass;
  EXPECT_EQ(reclass.Init("NAN=100; DEFAULT=PASS_THROUGH; [10, 20]=PASS_THROUGH",
                         /*noDataValue=*/std::nullopt, GDT_Float64),
            CE_None);

  bool found = false;
  EXPECT_DOUBLE_EQ(
      reclass.Reclassify(std::numeric_limits<double>::quiet_NaN(), found),
      100.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(15.0, found), 15.0);
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(reclass.Reclassify(50.0, found), 50.0);
  EXPECT_TRUE(found);
}

TEST(VrtReclassifierTest, InitNaNPassThroughAndDefaultValue) {
  Reclassifier reclass;
  EXPECT_EQ(reclass.Init("NAN=PASS_THROUGH; DEFAULT=42",
                         /*noDataValue=*/std::nullopt, GDT_Float64),
            CE_None);

  bool found = false;
  double res =
      reclass.Reclassify(std::numeric_limits<double>::quiet_NaN(), found);
  EXPECT_TRUE(found);
  EXPECT_TRUE(std::isnan(res));

  EXPECT_DOUBLE_EQ(reclass.Reclassify(999.0, found), 42.0);
  EXPECT_TRUE(found);
}

TEST(VrtReclassifierTest, InitErrors) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  Reclassifier reclass;

  // NO_DATA without nodata value set
  EXPECT_EQ(reclass.Init("NO_DATA=10", std::nullopt, GDT_Float64), CE_Failure);
  EXPECT_EQ(reclass.Init("10=NO_DATA", std::nullopt, GDT_Float64), CE_Failure);

  // Missing '=' separator
  EXPECT_EQ(reclass.Init("10:20", std::nullopt, GDT_Float64), CE_Failure);

  // Bad destination value
  EXPECT_EQ(reclass.Init("10=abc", std::nullopt, GDT_Float64), CE_Failure);

  // Bad delimiter
  EXPECT_EQ(reclass.Init("10=20 30=40", std::nullopt, GDT_Float64), CE_Failure);

  // Value cannot be represented in destination data type
  EXPECT_EQ(reclass.Init("1=300", std::nullopt, GDT_Byte), CE_Failure);
  EXPECT_EQ(reclass.Init("1=-1", std::nullopt, GDT_UInt16), CE_Failure);
  EXPECT_EQ(reclass.Init("1=1.5", std::nullopt, GDT_Int32), CE_Failure);

  // Interval parse error during init
  EXPECT_EQ(reclass.Init("[5, 2]=10", std::nullopt, GDT_Float64), CE_Failure);

  CPLPopErrorHandler();
}

TEST(VrtReclassifierTest, FindIntervalBranches) {
  Reclassifier reclass;
  EXPECT_EQ(reclass.Init("10=100; 20=200; 30=300", std::nullopt, GDT_Float64),
            CE_None);

  bool found = false;
  // Lower than lowest (mid == 0 path)
  EXPECT_DOUBLE_EQ(reclass.Reclassify(5.0, found), 0.0);
  EXPECT_FALSE(found);

  // Higher than highest (low = mid + 1 path)
  EXPECT_DOUBLE_EQ(reclass.Reclassify(35.0, found), 0.0);
  EXPECT_FALSE(found);

  // Between intervals
  EXPECT_DOUBLE_EQ(reclass.Reclassify(15.0, found), 0.0);
  EXPECT_FALSE(found);
}

TEST(VrtReclassifierTest, ReclassifyNaNWithoutNaNValue) {
  Reclassifier reclass;
  EXPECT_EQ(reclass.Init("DEFAULT=99", std::nullopt, GDT_Float64), CE_None);

  bool found = false;
  EXPECT_DOUBLE_EQ(
      reclass.Reclassify(std::numeric_limits<double>::quiet_NaN(), found),
      99.0);
  EXPECT_TRUE(found);

  Reclassifier reclass2;
  EXPECT_EQ(reclass2.Init("DEFAULT=PASS_THROUGH", std::nullopt, GDT_Float64),
            CE_None);
  double res =
      reclass2.Reclassify(std::numeric_limits<double>::quiet_NaN(), found);
  EXPECT_TRUE(found);
  EXPECT_TRUE(std::isnan(res));

  Reclassifier reclass3;
  EXPECT_EQ(reclass3.Init("1=1", std::nullopt, GDT_Float64), CE_None);
  EXPECT_DOUBLE_EQ(
      reclass3.Reclassify(std::numeric_limits<double>::quiet_NaN(), found),
      0.0);
  EXPECT_FALSE(found);
}

}  // namespace
}  // namespace autotest2
