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
// Tests the VRT interval and value reclassifier (vrtreclassifier.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtreclassifier.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include "third_party/gdal/frmts/vrt/vrtreclassifier.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::gdal::Reclassifier;
using ::testing::HasSubstr;

TEST(VrtReclassifierTest, IntervalConstant) {
  Reclassifier::Interval interval;
  interval.SetToConstant(42.0);
  EXPECT_TRUE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(42.0, interval.dfMin);
  EXPECT_DOUBLE_EQ(42.0, interval.dfMax);
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
  EXPECT_EQ(CE_None, interval.Parse("  123.5  rest", &rest));
  EXPECT_TRUE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(123.5, interval.dfMin);
  EXPECT_DOUBLE_EQ(123.5, interval.dfMax);
  ASSERT_NE(nullptr, rest);
  EXPECT_STREQ("  rest", rest);

  // Parse constant with null rest pointer.
  EXPECT_EQ(CE_None, interval.Parse("99.0", nullptr));
  EXPECT_TRUE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(99.0, interval.dfMin);
}

TEST(VrtReclassifierTest, IntervalParseClosedRange) {
  Reclassifier::Interval interval;
  char* rest = nullptr;
  EXPECT_EQ(CE_None, interval.Parse(" [ 10.0, 20.0] ;", &rest));
  EXPECT_FALSE(interval.IsConstant());
  EXPECT_DOUBLE_EQ(10.0, interval.dfMin);
  EXPECT_DOUBLE_EQ(20.0, interval.dfMax);
  ASSERT_NE(nullptr, rest);
  EXPECT_STREQ(" ;", rest);
  EXPECT_TRUE(interval.Contains(10.0));
  EXPECT_TRUE(interval.Contains(20.0));
  EXPECT_TRUE(interval.Contains(15.0));
  EXPECT_FALSE(interval.Contains(9.99));
  EXPECT_FALSE(interval.Contains(20.01));
}

TEST(VrtReclassifierTest, IntervalParseOpenRange) {
  Reclassifier::Interval interval;
  EXPECT_EQ(CE_None, interval.Parse("(10.0, 20.0)", nullptr));
  EXPECT_FALSE(interval.IsConstant());
  EXPECT_GT(interval.dfMin, 10.0);
  EXPECT_LT(interval.dfMax, 20.0);
  EXPECT_FALSE(interval.Contains(10.0));
  EXPECT_FALSE(interval.Contains(20.0));
  EXPECT_TRUE(interval.Contains(15.0));
}

TEST(VrtReclassifierTest, IntervalParseInfinities) {
  Reclassifier::Interval interval;
  EXPECT_EQ(CE_None, interval.Parse("[-inf, 0]", nullptr));
  EXPECT_DOUBLE_EQ(-std::numeric_limits<double>::infinity(), interval.dfMin);
  EXPECT_DOUBLE_EQ(0.0, interval.dfMax);

  EXPECT_EQ(CE_None, interval.Parse("(0, inf)", nullptr));
  EXPECT_GT(interval.dfMin, 0.0);
  EXPECT_DOUBLE_EQ(std::numeric_limits<double>::infinity(), interval.dfMax);
}

TEST(VrtReclassifierTest, IntervalParseErrors) {
  constexpr const char* kInvalidInputs[] = {
      "abc",       "",         "[, 5]",   "[abc, 5]",  "[10 20]",   "[10, ]",
      "[10, abc]", "[10, 20}", "[10, 20", "[nan, 20]", "[10, nan]", "[20, 10]",
  };

  WithQuietHandler quiet;
  Reclassifier::Interval interval;
  for (const char* input : kInvalidInputs) {
    SCOPED_TRACE(input);
    CPLErrorReset();
    char* rest = nullptr;
    EXPECT_EQ(CE_Failure, interval.Parse(input, &rest));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST(VrtReclassifierTest, OverlappingIntervalsFinalize) {
  Reclassifier reclass;
  Reclassifier::Interval a;
  a.dfMin = 1.0;
  a.dfMax = 5.0;
  reclass.AddMapping(a, 10.0);

  Reclassifier::Interval b;
  b.dfMin = 4.0;
  b.dfMax = 8.0;
  reclass.AddMapping(b, std::nullopt);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, reclass.Finalize());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("overlaps"));
}

TEST(VrtReclassifierTest, InitBasicMappings) {
  Reclassifier reclass;
  EXPECT_EQ(CE_None, reclass.Init("1  =  10; 2=20; [3, 5]=30; (5, 10)=40",
                                  /*noDataValue=*/std::nullopt, GDT_Float64));

  bool found = false;
  EXPECT_DOUBLE_EQ(10.0, reclass.Reclassify(1.0, found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(20.0, reclass.Reclassify(2.0, found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(30.0, reclass.Reclassify(4.0, found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(40.0, reclass.Reclassify(7.0, found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(0.0, reclass.Reclassify(0.0, found));
  EXPECT_FALSE(found);
}

TEST(VrtReclassifierTest, InitNoDataMappings) {
  Reclassifier reclass;
  EXPECT_EQ(CE_None, reclass.Init("NO_DATA=99; 5=NO_DATA",
                                  /*noDataValue=*/-9999.0, GDT_Float64));

  bool found = false;
  EXPECT_DOUBLE_EQ(99.0, reclass.Reclassify(-9999.0, found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(-9999.0, reclass.Reclassify(5.0, found));
  EXPECT_TRUE(found);
}

TEST(VrtReclassifierTest, InitNaNDefaultPassThrough) {
  Reclassifier reclass;
  EXPECT_EQ(CE_None,
            reclass.Init("NAN=100; DEFAULT=PASS_THROUGH; [10, 20]=PASS_THROUGH",
                         /*noDataValue=*/std::nullopt, GDT_Float64));

  bool found = false;
  EXPECT_DOUBLE_EQ(100.0, reclass.Reclassify(
                              std::numeric_limits<double>::quiet_NaN(), found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(15.0, reclass.Reclassify(15.0, found));
  EXPECT_TRUE(found);

  EXPECT_DOUBLE_EQ(50.0, reclass.Reclassify(50.0, found));
  EXPECT_TRUE(found);
}

TEST(VrtReclassifierTest, InitNaNPassThroughAndDefaultValue) {
  Reclassifier reclass;
  EXPECT_EQ(CE_None, reclass.Init("NAN=PASS_THROUGH; DEFAULT=42",
                                  /*noDataValue=*/std::nullopt, GDT_Float64));

  bool found = false;
  double res =
      reclass.Reclassify(std::numeric_limits<double>::quiet_NaN(), found);
  EXPECT_TRUE(found);
  EXPECT_TRUE(std::isnan(res));

  EXPECT_DOUBLE_EQ(42.0, reclass.Reclassify(999.0, found));
  EXPECT_TRUE(found);
}

TEST(VrtReclassifierTest, InitErrors) {
  struct ErrorCase {
    const char* mapping;
    GDALDataType dst_type;
  };
  constexpr ErrorCase kErrorCases[] = {
      {.mapping = "NO_DATA=10", .dst_type = GDT_Float64},
      {.mapping = "10=NO_DATA", .dst_type = GDT_Float64},
      {.mapping = "10:20", .dst_type = GDT_Float64},
      {.mapping = "10=abc", .dst_type = GDT_Float64},
      {.mapping = "10=20 30=40", .dst_type = GDT_Float64},
      {.mapping = "1=300", .dst_type = GDT_Byte},
      {.mapping = "1=-1", .dst_type = GDT_UInt16},
      {.mapping = "1=1.5", .dst_type = GDT_Int32},
      {.mapping = "[5, 2]=10", .dst_type = GDT_Float64},
  };

  WithQuietHandler quiet;
  for (const auto& tc : kErrorCases) {
    SCOPED_TRACE(tc.mapping);
    Reclassifier reclass;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, reclass.Init(tc.mapping, /*noDataValue=*/std::nullopt,
                                       tc.dst_type));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST(VrtReclassifierTest, FindIntervalBranches) {
  Reclassifier reclass;
  EXPECT_EQ(CE_None,
            reclass.Init("10=100; 20=200; 30=300", std::nullopt, GDT_Float64));

  bool found = false;
  // Lower than lowest (mid == 0 path).
  EXPECT_DOUBLE_EQ(0.0, reclass.Reclassify(5.0, found));
  EXPECT_FALSE(found);

  // Higher than highest (low = mid + 1 path).
  EXPECT_DOUBLE_EQ(0.0, reclass.Reclassify(35.0, found));
  EXPECT_FALSE(found);

  // Between intervals.
  EXPECT_DOUBLE_EQ(0.0, reclass.Reclassify(15.0, found));
  EXPECT_FALSE(found);
}

TEST(VrtReclassifierTest, ReclassifyNaNWithoutNaNValue) {
  Reclassifier reclass;
  EXPECT_EQ(CE_None, reclass.Init("DEFAULT=99", std::nullopt, GDT_Float64));

  bool found = false;
  EXPECT_DOUBLE_EQ(99.0, reclass.Reclassify(
                             std::numeric_limits<double>::quiet_NaN(), found));
  EXPECT_TRUE(found);

  Reclassifier reclass2;
  EXPECT_EQ(CE_None,
            reclass2.Init("DEFAULT=PASS_THROUGH", std::nullopt, GDT_Float64));
  double res =
      reclass2.Reclassify(std::numeric_limits<double>::quiet_NaN(), found);
  EXPECT_TRUE(found);
  EXPECT_TRUE(std::isnan(res));

  Reclassifier reclass3;
  EXPECT_EQ(CE_None, reclass3.Init("1=1", std::nullopt, GDT_Float64));
  EXPECT_DOUBLE_EQ(0.0, reclass3.Reclassify(
                            std::numeric_limits<double>::quiet_NaN(), found));
  EXPECT_FALSE(found);
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover the final unreachable fallback branch in
// VRTReclassifier::InitFromMappingString in vrtreclassifier.cpp.
