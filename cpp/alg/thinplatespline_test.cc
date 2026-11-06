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
// Tests VizGeorefSpline2D (alg/thinplatespline.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/thinplatespline.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/thinplatespline.h

#include "third_party/gdal/alg/thinplatespline.h"

#include <array>
#include <cmath>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(ThinPlateSplineTest, ZeroPoints) {
  VizGeorefSpline2D spline;
  EXPECT_EQ(0, spline.solve());

  std::array<double, 1> vars = {999.0};
  EXPECT_EQ(1, spline.get_point(10.0, 20.0, vars.data()));
  EXPECT_DOUBLE_EQ(0.0, vars[0]);
}

TEST(ThinPlateSplineTest, OnePoint) {
  VizGeorefSpline2D spline;
  const double val = 42.0;
  EXPECT_TRUE(spline.add_point(10.0, 20.0, &val));
  EXPECT_EQ(1, spline.solve());

  std::array<double, 1> vars = {0.0};
  EXPECT_EQ(1, spline.get_point(0.0, 0.0, vars.data()));
  EXPECT_DOUBLE_EQ(42.0, vars[0]);

  EXPECT_EQ(1, spline.get_point(100.0, 200.0, vars.data()));
  EXPECT_DOUBLE_EQ(42.0, vars[0]);
}

TEST(ThinPlateSplineTest, TwoPointsCoincidentFails) {
  VizGeorefSpline2D spline;
  const double val1 = 10.0;
  const double val2 = 20.0;
  EXPECT_TRUE(spline.add_point(5.0, 5.0, &val1));
  EXPECT_TRUE(spline.add_point(5.0, 5.0, &val2));
  EXPECT_EQ(0, spline.solve());
}

TEST(ThinPlateSplineTest, TwoPointsDistinctInterpolation) {
  VizGeorefSpline2D spline;
  const double val1 = 10.0;
  const double val2 = 30.0;
  EXPECT_TRUE(spline.add_point(0.0, 0.0, &val1));
  EXPECT_TRUE(spline.add_point(10.0, 0.0, &val2));
  EXPECT_EQ(2, spline.solve());

  std::array<double, 1> vars = {0.0};
  EXPECT_EQ(1, spline.get_point(0.0, 0.0, vars.data()));
  EXPECT_NEAR(10.0, vars[0], 1e-6);

  EXPECT_EQ(1, spline.get_point(10.0, 0.0, vars.data()));
  EXPECT_NEAR(30.0, vars[0], 1e-6);

  EXPECT_EQ(1, spline.get_point(5.0, 0.0, vars.data()));
  EXPECT_NEAR(20.0, vars[0], 1e-6);
}

TEST(ThinPlateSplineTest, DegenerateSystemAllSameX) {
  VizGeorefSpline2D spline;
  const double val = 1.0;
  EXPECT_TRUE(spline.add_point(5.0, 1.0, &val));
  EXPECT_TRUE(spline.add_point(5.0, 2.0, &val));
  EXPECT_TRUE(spline.add_point(5.0, 3.0, &val));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(0, spline.solve());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Degenerate system"));
  CPLErrorReset();
}

TEST(ThinPlateSplineTest, DegenerateSystemAllSameY) {
  VizGeorefSpline2D spline;
  const double val = 1.0;
  EXPECT_TRUE(spline.add_point(1.0, 5.0, &val));
  EXPECT_TRUE(spline.add_point(2.0, 5.0, &val));
  EXPECT_TRUE(spline.add_point(3.0, 5.0, &val));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(0, spline.solve());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Degenerate system"));
  CPLErrorReset();
}

TEST(ThinPlateSplineTest, OneDimensionalCollinear) {
  VizGeorefSpline2D spline;
  // Diagonal collinear points: (0,0), (2,2), (4,4), (6,6)
  const double val1 = 10.0;
  const double val2 = 20.0;
  const double val3 = 30.0;
  const double val4 = 40.0;
  EXPECT_TRUE(spline.add_point(0.0, 0.0, &val1));
  EXPECT_TRUE(spline.add_point(2.0, 2.0, &val2));
  EXPECT_TRUE(spline.add_point(4.0, 4.0, &val3));
  EXPECT_TRUE(spline.add_point(6.0, 6.0, &val4));
  EXPECT_EQ(3, spline.solve());

  std::array<double, 1> vars = {0.0};
  // 1. Before first point (Pu <= u[index[0]])
  EXPECT_EQ(1, spline.get_point(-2.0, -2.0, vars.data()));
  EXPECT_NEAR(0.0, vars[0], 1e-6);

  // 2. After last point (Pu >= u[index[_nof_points - 1]])
  EXPECT_EQ(1, spline.get_point(8.0, 8.0, vars.data()));
  EXPECT_NEAR(50.0, vars[0], 1e-6);

  // 3. Middle between points (2,2) and (4,4)
  EXPECT_EQ(1, spline.get_point(3.0, 3.0, vars.data()));
  EXPECT_NEAR(25.0, vars[0], 1e-6);

  // 4. Exact hit on a control point
  EXPECT_EQ(1, spline.get_point(4.0, 4.0, vars.data()));
  EXPECT_NEAR(30.0, vars[0], 1e-6);
}

TEST(ThinPlateSplineTest, OneDimensionalExtremeAspectRatio) {
  // delx < 0.001 * dely
  {
    VizGeorefSpline2D spline;
    const double val = 1.0;
    EXPECT_TRUE(spline.add_point(0.0, 0.0, &val));
    EXPECT_TRUE(spline.add_point(0.00001, 10.0, &val));
    EXPECT_TRUE(spline.add_point(0.00002, 20.0, &val));
    EXPECT_EQ(3, spline.solve());
  }
  // dely < 0.001 * delx
  {
    VizGeorefSpline2D spline;
    const double val = 1.0;
    EXPECT_TRUE(spline.add_point(0.0, 0.0, &val));
    EXPECT_TRUE(spline.add_point(10.0, 0.00001, &val));
    EXPECT_TRUE(spline.add_point(20.0, 0.00002, &val));
    EXPECT_EQ(3, spline.solve());
  }
}

TEST(ThinPlateSplineTest, Full2DSingularMatrix) {
  VizGeorefSpline2D spline;
  const double val1 = 10.0;
  const double val2 = 20.0;
  const double val3 = 30.0;
  const double val4 = 40.0;
  // Non-collinear bounding box, but duplicate points at p=0 and p=1 create
  // identical matrix rows checked during the LU decomposition pivot loop.
  EXPECT_TRUE(spline.add_point(0.0, 0.0, &val1));
  EXPECT_TRUE(spline.add_point(0.0, 0.0, &val2));
  EXPECT_TRUE(spline.add_point(10.0, 0.0, &val3));
  EXPECT_TRUE(spline.add_point(0.0, 10.0, &val4));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(0, spline.solve());
  CPLErrorReset();
}

TEST(ThinPlateSplineTest, Full2DExactInterpolation) {
  VizGeorefSpline2D spline;
  const double val1 = 100.0;
  const double val2 = 200.0;
  const double val3 = 300.0;
  const double val4 = 400.0;
  EXPECT_TRUE(spline.add_point(0.0, 0.0, &val1));
  EXPECT_TRUE(spline.add_point(10.0, 0.0, &val2));
  EXPECT_TRUE(spline.add_point(0.0, 10.0, &val3));
  EXPECT_TRUE(spline.add_point(10.0, 10.0, &val4));
  EXPECT_EQ(4, spline.solve());

  std::array<double, 1> vars = {0.0};
  // Exact points:
  EXPECT_EQ(1, spline.get_point(0.0, 0.0, vars.data()));
  EXPECT_NEAR(100.0, vars[0], 1e-4);
  EXPECT_EQ(1, spline.get_point(10.0, 0.0, vars.data()));
  EXPECT_NEAR(200.0, vars[0], 1e-4);
  EXPECT_EQ(1, spline.get_point(0.0, 10.0, vars.data()));
  EXPECT_NEAR(300.0, vars[0], 1e-4);
  EXPECT_EQ(1, spline.get_point(10.0, 10.0, vars.data()));
  EXPECT_NEAR(400.0, vars[0], 1e-4);

  // Intermediate point:
  EXPECT_EQ(1, spline.get_point(5.0, 5.0, vars.data()));
  EXPECT_NEAR(250.0, vars[0], 1.0);
}

TEST(ThinPlateSplineTest, Full2DLargeWithBufferGrowthAndVectorization) {
  VizGeorefSpline2D spline;
  // Adding 9 points causes grow_points() to resize multiple times (from initial
  // 2 to 6 to 14). In get_point(), 8 points are evaluated with
  // VizGeorefSpline2DBase_func4 in chunks of 4, and the 9th remainder point is
  // evaluated with VizGeorefSpline2DBase_func.
  struct Pt {
    double x;
    double y;
    double val;
  };
  constexpr std::array<Pt, 9> kPoints = {{
      {.x = 0.0, .y = 0.0, .val = 10.0},
      {.x = 5.0, .y = 0.0, .val = 15.0},
      {.x = 10.0, .y = 0.0, .val = 20.0},
      {.x = 0.0, .y = 5.0, .val = 25.0},
      {.x = 5.0, .y = 5.0, .val = 30.0},
      {.x = 10.0, .y = 5.0, .val = 35.0},
      {.x = 0.0, .y = 10.0, .val = 40.0},
      {.x = 5.0, .y = 10.0, .val = 45.0},
      {.x = 10.0, .y = 10.0, .val = 50.0},
  }};

  for (const auto& pt : kPoints) {
    EXPECT_TRUE(spline.add_point(pt.x, pt.y, &pt.val));
  }
  EXPECT_EQ(4, spline.solve());

  for (const auto& pt : kPoints) {
    std::array<double, 1> vars = {0.0};
    EXPECT_EQ(1, spline.get_point(pt.x, pt.y, vars.data()));
    EXPECT_NEAR(pt.val, vars[0], 1e-3);
  }
}

TEST(ThinPlateSplineTest, MultipleVariables) {
  VizGeorefSpline2D spline(2);  // 2 variables
  struct Pt {
    double x;
    double y;
    double val[2];
  };
  constexpr std::array<Pt, 4> kPoints = {{
      {.x = 0.0, .y = 0.0, .val = {10.0, 100.0}},
      {.x = 10.0, .y = 0.0, .val = {20.0, 200.0}},
      {.x = 0.0, .y = 10.0, .val = {30.0, 300.0}},
      {.x = 10.0, .y = 10.0, .val = {40.0, 400.0}},
  }};

  for (const auto& pt : kPoints) {
    EXPECT_TRUE(spline.add_point(pt.x, pt.y, pt.val));
  }
  EXPECT_EQ(4, spline.solve());

  std::array<double, 2> vars = {0.0, 0.0};
  EXPECT_EQ(1, spline.get_point(0.0, 0.0, vars.data()));
  EXPECT_NEAR(10.0, vars[0], 1e-4);
  EXPECT_NEAR(100.0, vars[1], 1e-4);

  EXPECT_EQ(1, spline.get_point(10.0, 10.0, vars.data()));
  EXPECT_NEAR(40.0, vars[0], 1e-4);
  EXPECT_NEAR(400.0, vars[1], 1e-4);
}

TEST(ThinPlateSplineTest, PointAddedAfterSolveReturnsError) {
  VizGeorefSpline2D spline;
  const double val1 = 10.0;
  EXPECT_TRUE(spline.add_point(0.0, 0.0, &val1));
  EXPECT_EQ(1, spline.solve());

  // Add a point after solve without calling solve() again.
  const double val2 = 20.0;
  EXPECT_TRUE(spline.add_point(10.0, 0.0, &val2));

  std::array<double, 1> vars = {999.0};
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(0, spline.get_point(5.0, 0.0, vars.data()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("A point was added after the last solve"));
  EXPECT_DOUBLE_EQ(0.0, vars[0]);
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are non-VIZ_GIS_OPTIM_1
// fallback and OOM guards.
