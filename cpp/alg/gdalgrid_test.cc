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
// Tests GDAL gridding algorithm and context APIs (alg/gdalgrid.cpp).
// Note: In Google3 GDAL builds, QHull (INTERNAL_QHULL / EXTERNAL_QHULL) is
// not compiled in, and AVX/SSE intrinsics are not enabled at compile time
// (HAVE_SSE_AT_COMPILE_TIME / HAVE_AVX_AT_COMPILE_TIME undefined).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgrid.cpp
//   https://gdal.org/api/gdal_alg.html

#include "third_party/gdal/alg/gdalgrid.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdalgrid_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// 16 points in [0, 9] x [0, 9] arranged on a 4x4 regular grid with distinct Z.
// Around center point (4.5, 4.5), points are symmetrically split into 4
// quadrants with exactly 4 points in each quadrant.
constexpr GUInt32 k16PointsCount = 16;
constexpr std::array<double, 16> k16PointsX = {
    0.0, 3.0, 6.0, 9.0, 0.0, 3.0, 6.0, 9.0,
    0.0, 3.0, 6.0, 9.0, 0.0, 3.0, 6.0, 9.0,
};
constexpr std::array<double, 16> k16PointsY = {
    0.0, 0.0, 0.0, 0.0, 3.0, 3.0, 3.0, 3.0,
    6.0, 6.0, 6.0, 6.0, 9.0, 9.0, 9.0, 9.0,
};
constexpr std::array<double, 16> k16PointsZ = {
    10.0, 13.0, 16.0, 19.0, 40.0,  43.0,  46.0,  49.0,
    70.0, 73.0, 76.0, 79.0, 100.0, 103.0, 106.0, 109.0,
};

// 4 points forming a symmetric square in [0, 10] x [0, 10].
constexpr GUInt32 k4PointsCount = 4;
constexpr std::array<double, 4> k4PointsX = {0.0, 10.0, 10.0, 0.0};
constexpr std::array<double, 4> k4PointsY = {0.0, 0.0, 10.0, 10.0};
constexpr std::array<double, 4> k4PointsZ = {10.0, 20.0, 30.0, 40.0};

// 3 collinear points for Delaunay triangulation failure.
constexpr GUInt32 k3CollinearCount = 3;
constexpr std::array<double, 3> k3CollinearX = {0.0, 1.0, 2.0};
constexpr std::array<double, 3> k3CollinearY = {0.0, 1.0, 2.0};
constexpr std::array<double, 3> k3CollinearZ = {10.0, 20.0, 30.0};

int DummyProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                  void* pProgressArg) {
  if (pProgressArg != nullptr) {
    *static_cast<int*>(pProgressArg) += 1;
  }
  return TRUE;
}

int CancellingProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                       void* /*pProgressArg*/) {
  return FALSE;
}

// -----------------------------------------------------------------------------
// GDALGridParseAlgorithmAndOptions Tests
// -----------------------------------------------------------------------------

TEST(GdalgridTest, ParseEmptyAndInvalidAlgorithm) {
  GDALGridAlgorithm alg = GGA_InverseDistanceToAPower;
  void* options = nullptr;
  EXPECT_EQ(CE_Failure, GDALGridParseAlgorithmAndOptions("", &alg, &options));
  EXPECT_EQ(nullptr, options);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALGridParseAlgorithmAndOptions(
                              "unsupported_alg_name", &alg, &options));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Unsupported gridding method \"unsupported_alg_name\""));
  }
  EXPECT_EQ(nullptr, options);
}

TEST(GdalgridTest, ParseInvDistDefaultsAndOptions) {
  // 1. Defaults.
  GDALGridAlgorithm alg = GGA_MovingAverage;
  void* options1 = nullptr;
  EXPECT_EQ(CE_None,
            GDALGridParseAlgorithmAndOptions("invdist", &alg, &options1));
  absl::Cleanup cleaner1 = [options1] { CPLFree(options1); };
  EXPECT_EQ(GGA_InverseDistanceToAPower, alg);
  ASSERT_NE(nullptr, options1);
  const auto* p1 =
      static_cast<const GDALGridInverseDistanceToAPowerOptions*>(options1);
  EXPECT_EQ(sizeof(*p1), p1->nSizeOfStructure);
  EXPECT_DOUBLE_EQ(2.0, p1->dfPower);
  EXPECT_DOUBLE_EQ(0.0, p1->dfSmoothing);
  EXPECT_DOUBLE_EQ(0.0, p1->dfRadius1);
  EXPECT_DOUBLE_EQ(0.0, p1->dfRadius2);
  EXPECT_DOUBLE_EQ(0.0, p1->dfAngle);
  EXPECT_EQ(0u, p1->nMaxPoints);
  EXPECT_EQ(0u, p1->nMinPoints);
  EXPECT_DOUBLE_EQ(0.0, p1->dfNoDataValue);

  // 2. Custom parameters with single radius.
  void* options2 = nullptr;
  EXPECT_EQ(CE_None, GDALGridParseAlgorithmAndOptions(
                         "invdist:power=3.5:smoothing=1.2:radius=5.0:angle=45."
                         "0:max_points=8:min_points=2:nodata=-9999.0",
                         &alg, &options2));
  absl::Cleanup cleaner2 = [options2] { CPLFree(options2); };
  const auto* p2 =
      static_cast<const GDALGridInverseDistanceToAPowerOptions*>(options2);
  EXPECT_DOUBLE_EQ(3.5, p2->dfPower);
  EXPECT_DOUBLE_EQ(1.2, p2->dfSmoothing);
  EXPECT_DOUBLE_EQ(5.0, p2->dfRadius1);
  EXPECT_DOUBLE_EQ(5.0, p2->dfRadius2);
  EXPECT_DOUBLE_EQ(45.0, p2->dfAngle);
  EXPECT_EQ(8u, p2->nMaxPoints);
  EXPECT_EQ(2u, p2->nMinPoints);
  EXPECT_DOUBLE_EQ(-9999.0, p2->dfNoDataValue);

  // 3. Separate radius1 and radius2.
  void* options3 = nullptr;
  EXPECT_EQ(CE_None, GDALGridParseAlgorithmAndOptions(
                         "invdist:radius1=4.0:radius2=6.0", &alg, &options3));
  absl::Cleanup cleaner3 = [options3] { CPLFree(options3); };
  const auto* p3 =
      static_cast<const GDALGridInverseDistanceToAPowerOptions*>(options3);
  EXPECT_DOUBLE_EQ(4.0, p3->dfRadius1);
  EXPECT_DOUBLE_EQ(6.0, p3->dfRadius2);
}

TEST(GdalgridTest, ParseInvDistQuadrantRemapping) {
  // 1. Error: radius1 != radius2 when quadrant parameter is specified.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure,
              GDALGridParseAlgorithmAndOptions(
                  "invdist:radius1=2:radius2=4:min_points_per_quadrant=1", &alg,
                  &options));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("radius1 != radius2 not supported"));
  }

  // 2. Error: angle != 0 when quadrant parameter is specified.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure,
              GDALGridParseAlgorithmAndOptions(
                  "invdist:radius=5:angle=15:max_points_per_quadrant=2", &alg,
                  &options));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("angle != 0 not supported"));
  }

  // 3. Success: remapped to invdistnn with radius.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "invdist:radius=5:power=2.5:smoothing=0.5:max_points=10:min_"
                  "points=1:nodata=-1:min_points_per_quadrant=2:max_points_"
                  "per_quadrant=4",
                  &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(GGA_InverseDistanceToAPowerNearestNeighbor, alg);
    ASSERT_NE(nullptr, options);
    const auto* p = static_cast<
        const GDALGridInverseDistanceToAPowerNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius);
    EXPECT_DOUBLE_EQ(2.5, p->dfPower);
    EXPECT_DOUBLE_EQ(0.5, p->dfSmoothing);
    EXPECT_EQ(10u, p->nMaxPoints);
    EXPECT_EQ(1u, p->nMinPoints);
    EXPECT_DOUBLE_EQ(-1.0, p->dfNoDataValue);
    EXPECT_EQ(2u, p->nMinPointsPerQuadrant);
    EXPECT_EQ(4u, p->nMaxPointsPerQuadrant);
  }

  // 4. Success: remapped to invdistnn without radius (defaults radius1 to "1").
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "invdist:min_points_per_quadrant=1", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(GGA_InverseDistanceToAPowerNearestNeighbor, alg);
    const auto* p = static_cast<
        const GDALGridInverseDistanceToAPowerNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(1.0, p->dfRadius);
  }
}

TEST(GdalgridTest, ParseInvDistNNOptions) {
  // 1. Defaults.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions("invdistnn", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(GGA_InverseDistanceToAPowerNearestNeighbor, alg);
    const auto* p = static_cast<
        const GDALGridInverseDistanceToAPowerNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(2.0, p->dfPower);
    EXPECT_DOUBLE_EQ(1.0, p->dfRadius);
    EXPECT_DOUBLE_EQ(0.0, p->dfSmoothing);
    EXPECT_EQ(12u, p->nMaxPoints);
    EXPECT_EQ(0u, p->nMinPoints);
    EXPECT_DOUBLE_EQ(0.0, p->dfNoDataValue);
    EXPECT_EQ(0u, p->nMinPointsPerQuadrant);
    EXPECT_EQ(0u, p->nMaxPointsPerQuadrant);
  }

  // 2. Failure: radius <= 0.
  // Note: GDALGridParseAlgorithmAndOptions allocates *ppOptions before
  // validating arguments, so cleanup is required even on CE_Failure.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure, GDALGridParseAlgorithmAndOptions("invdistnn:radius=0",
                                                           &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Radius value should be strictly positive"));
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure, GDALGridParseAlgorithmAndOptions(
                              "invdistnn:radius=-3.5", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Custom options.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "invdistnn:power=1.5:smoothing=0.25:radius=8.0:max_points=5:"
                  "min_points=2:nodata=-99.0:min_points_per_quadrant=1:max_"
                  "points_per_quadrant=3",
                  &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<
        const GDALGridInverseDistanceToAPowerNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(1.5, p->dfPower);
    EXPECT_DOUBLE_EQ(0.25, p->dfSmoothing);
    EXPECT_DOUBLE_EQ(8.0, p->dfRadius);
    EXPECT_EQ(5u, p->nMaxPoints);
    EXPECT_EQ(2u, p->nMinPoints);
    EXPECT_DOUBLE_EQ(-99.0, p->dfNoDataValue);
    EXPECT_EQ(1u, p->nMinPointsPerQuadrant);
    EXPECT_EQ(3u, p->nMaxPointsPerQuadrant);
  }
}

TEST(GdalgridTest, ParseAverageOptions) {
  // 1. Defaults.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions("average", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(GGA_MovingAverage, alg);
    const auto* p = static_cast<const GDALGridMovingAverageOptions*>(options);
    EXPECT_DOUBLE_EQ(0.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(0.0, p->dfRadius2);
    EXPECT_DOUBLE_EQ(0.0, p->dfAngle);
    EXPECT_EQ(0u, p->nMinPoints);
    EXPECT_EQ(0u, p->nMaxPoints);
    EXPECT_DOUBLE_EQ(0.0, p->dfNoDataValue);
    EXPECT_EQ(0u, p->nMinPointsPerQuadrant);
    EXPECT_EQ(0u, p->nMaxPointsPerQuadrant);
  }

  // 2. Custom radius, radius1, radius2, angle, min_points, nodata.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "average:radius1=6.0:radius2=12.0:angle=30.0:min_points=2:"
                  "nodata=-1.0",
                  &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<const GDALGridMovingAverageOptions*>(options);
    EXPECT_DOUBLE_EQ(6.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(12.0, p->dfRadius2);
    EXPECT_DOUBLE_EQ(30.0, p->dfAngle);
    EXPECT_EQ(2u, p->nMinPoints);
    EXPECT_DOUBLE_EQ(-1.0, p->dfNoDataValue);
  }

  // 3. Warning when max_points > 0 without quadrant parameters.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None, GDALGridParseAlgorithmAndOptions(
                           "average:radius=5:max_points=5", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("max_points is ignored"));
  }

  // 4. Quadrant error: radius <= 0.
  // Note: GDALGridParseAlgorithmAndOptions allocates *ppOptions before
  // validating arguments, so cleanup is required even on CE_Failure.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure, GDALGridParseAlgorithmAndOptions(
                              "average:min_points_per_quadrant=1:radius=0",
                              &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // 5. Quadrant error: angle != 0.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure,
              GDALGridParseAlgorithmAndOptions(
                  "average:radius=5:min_points_per_quadrant=1:angle=45", &alg,
                  &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  }

  // 6. Quadrant success.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "average:radius=5:min_points_per_quadrant=1:max_points_per_"
                  "quadrant=4",
                  &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<const GDALGridMovingAverageOptions*>(options);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius2);
    EXPECT_EQ(1u, p->nMinPointsPerQuadrant);
    EXPECT_EQ(4u, p->nMaxPointsPerQuadrant);
  }
}

TEST(GdalgridTest, ParseNearestOptions) {
  // 1. Defaults.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions("nearest", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(GGA_NearestNeighbor, alg);
    const auto* p = static_cast<const GDALGridNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(0.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(0.0, p->dfRadius2);
    EXPECT_DOUBLE_EQ(0.0, p->dfAngle);
    EXPECT_DOUBLE_EQ(0.0, p->dfNoDataValue);
  }

  // 2. Custom radius and nodata.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "nearest:radius=5.0:nodata=-9999.0", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<const GDALGridNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius2);
    EXPECT_DOUBLE_EQ(-9999.0, p->dfNoDataValue);
  }

  // 3. Separate radius1 and radius2.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None, GDALGridParseAlgorithmAndOptions(
                           "nearest:radius1=3.0:radius2=7.0:angle=15.0", &alg,
                           &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<const GDALGridNearestNeighborOptions*>(options);
    EXPECT_DOUBLE_EQ(3.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(7.0, p->dfRadius2);
    EXPECT_DOUBLE_EQ(15.0, p->dfAngle);
  }
}

TEST(GdalgridTest, ParseMetricsOptions) {
  struct MetricCase {
    const char* pszName;
    GDALGridAlgorithm eExpectedAlg;
  };
  constexpr MetricCase kCases[] = {
      {"minimum", GGA_MetricMinimum},
      {"maximum", GGA_MetricMaximum},
      {"range", GGA_MetricRange},
      {"count", GGA_MetricCount},
      {"average_distance", GGA_MetricAverageDistance},
      {"average_distance_pts", GGA_MetricAverageDistancePts},
  };

  for (const auto& item : kCases) {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(item.pszName, &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(item.eExpectedAlg, alg);
    ASSERT_NE(nullptr, options);
    const auto* p = static_cast<const GDALGridDataMetricsOptions*>(options);
    EXPECT_EQ(sizeof(*p), p->nSizeOfStructure);
  }

  // Quadrant error: average_distance_pts does not support quadrant search.
  // Note: GDALGridParseAlgorithmAndOptions allocates *ppOptions before
  // validating arguments, so cleanup is required even on CE_Failure.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure,
              GDALGridParseAlgorithmAndOptions(
                  "average_distance_pts:radius=5:min_points_per_quadrant=1",
                  &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not supported when per quadrant"));
  }

  // Quadrant error: metric with radius <= 0.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure, GDALGridParseAlgorithmAndOptions(
                              "minimum:min_points_per_quadrant=1:radius=0",
                              &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // Quadrant error: metric with angle != 0.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_Failure,
              GDALGridParseAlgorithmAndOptions(
                  "maximum:radius=5:min_points_per_quadrant=1:angle=10", &alg,
                  &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  }

  // Quadrant success for metric.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "range:radius=5:min_points_per_quadrant=1:max_points_per_"
                  "quadrant=4",
                  &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<const GDALGridDataMetricsOptions*>(options);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius1);
    EXPECT_DOUBLE_EQ(5.0, p->dfRadius2);
    EXPECT_EQ(1u, p->nMinPointsPerQuadrant);
    EXPECT_EQ(4u, p->nMaxPointsPerQuadrant);
  }
}

TEST(GdalgridTest, ParseLinearOptions) {
  // 1. Defaults.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions("linear", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    EXPECT_EQ(GGA_Linear, alg);
    const auto* p = static_cast<const GDALGridLinearOptions*>(options);
    EXPECT_EQ(sizeof(*p), p->nSizeOfStructure);
    EXPECT_DOUBLE_EQ(-1.0, p->dfRadius);
    EXPECT_DOUBLE_EQ(0.0, p->dfNoDataValue);
  }

  // 2. Custom radius and nodata.
  {
    void* options = nullptr;
    GDALGridAlgorithm alg;
    EXPECT_EQ(CE_None,
              GDALGridParseAlgorithmAndOptions(
                  "linear:radius=25.0:nodata=-9999.0", &alg, &options));
    absl::Cleanup cleaner = [options] { CPLFree(options); };
    const auto* p = static_cast<const GDALGridLinearOptions*>(options);
    EXPECT_DOUBLE_EQ(25.0, p->dfRadius);
    EXPECT_DOUBLE_EQ(-9999.0, p->dfNoDataValue);
  }
}

TEST(GdalgridTest, ParseUnknownOptionWarning) {
  WithQuietHandler quiet;
  CPLErrorReset();
  void* options = nullptr;
  GDALGridAlgorithm alg;
  EXPECT_EQ(CE_None, GDALGridParseAlgorithmAndOptions(
                         "invdist:unknown_option_name=42", &alg, &options));
  absl::Cleanup cleaner = [options] { CPLFree(options); };
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Option unknown_option_name ignored"));
}

// -----------------------------------------------------------------------------
// Context Lifecycle and Error Handling Tests
// -----------------------------------------------------------------------------

TEST(GdalgridTest, ContextCreateInvalidOptionsSize) {
  // Test every algorithm enum with invalid nSizeOfStructure (0).
  struct AlgCase {
    GDALGridAlgorithm eAlg;
    size_t nValidSize;
  };
  const AlgCase kCases[] = {
      {GGA_InverseDistanceToAPower,
       sizeof(GDALGridInverseDistanceToAPowerOptions)},
      {GGA_MovingAverage, sizeof(GDALGridMovingAverageOptions)},
      {GGA_NearestNeighbor, sizeof(GDALGridNearestNeighborOptions)},
      {GGA_MetricMinimum, sizeof(GDALGridDataMetricsOptions)},
      {GGA_MetricMaximum, sizeof(GDALGridDataMetricsOptions)},
      {GGA_MetricRange, sizeof(GDALGridDataMetricsOptions)},
      {GGA_MetricCount, sizeof(GDALGridDataMetricsOptions)},
      {GGA_MetricAverageDistance, sizeof(GDALGridDataMetricsOptions)},
      {GGA_MetricAverageDistancePts, sizeof(GDALGridDataMetricsOptions)},
      {GGA_Linear, sizeof(GDALGridLinearOptions)},
      {GGA_InverseDistanceToAPowerNearestNeighbor,
       sizeof(GDALGridInverseDistanceToAPowerNearestNeighborOptions)},
  };

  for (const auto& item : kCases) {
    std::vector<char> buffer(item.nValidSize, 0);
    // nSizeOfStructure is the first member of each struct.
    *reinterpret_cast<size_t*>(buffer.data()) = 0;  // Invalid size!

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALGridContext* ctx = GDALGridContextCreate(
        item.eAlg, buffer.data(), k4PointsCount, k4PointsX.data(),
        k4PointsY.data(), k4PointsZ.data(), TRUE);
    EXPECT_EQ(nullptr, ctx);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong value of nSizeOfStructure member"));
  }
}

TEST(GdalgridTest, ContextCreateInvalidAlgorithm) {
  GDALGridMovingAverageOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALGridContext* ctx = GDALGridContextCreate(
      static_cast<GDALGridAlgorithm>(0), &opts, k4PointsCount, k4PointsX.data(),
      k4PointsY.data(), k4PointsZ.data(), TRUE);
  EXPECT_EQ(nullptr, ctx);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("GDAL does not support gridding method 0"));
}

TEST(GdalgridTest, ContextCreateLinearFailureWithoutQHull) {
  GDALGridLinearOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius = -1.0;

  WithQuietHandler quiet;
  CPLErrorReset();
  // Linear Delaunay gridding fails because QHull is not compiled in google3.
  GDALGridContext* ctx = GDALGridContextCreate(
      GGA_Linear, &opts, k3CollinearCount, k3CollinearX.data(),
      k3CollinearY.data(), k3CollinearZ.data(), TRUE);
  EXPECT_EQ(nullptr, ctx);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without QHull support"));
}

TEST(GdalgridTest, ContextFreeNullAndDuplicateArrays) {
  // 1. Freeing nullptr must not crash.
  GDALGridContextFree(nullptr);

  // 2. Caller does not keep arrays alive (bCallerWillKeepPointArraysAlive =
  // FALSE).
  GDALGridMovingAverageOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  GDALGridContext* ctx = GDALGridContextCreate(
      GGA_MovingAverage, &opts, k4PointsCount, k4PointsX.data(),
      k4PointsY.data(), k4PointsZ.data(),
      /*bCallerWillKeepPointArraysAlive=*/FALSE);
  ASSERT_NE(nullptr, ctx);
  GDALGridContextFree(ctx);
}

TEST(GdalgridTest, ContextProcessValidation) {
  GDALGridMovingAverageOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  GDALGridContext* ctx = GDALGridContextCreate(
      GGA_MovingAverage, &opts, k4PointsCount, k4PointsX.data(),
      k4PointsY.data(), k4PointsZ.data(), TRUE);
  ASSERT_NE(nullptr, ctx);
  absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

  double output_pixel = 0.0;
  // nXSize == 0
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALGridContextProcess(ctx, 0.0, 10.0, 0.0, 10.0, 0,
                                                 1, GDT_Float64, &output_pixel,
                                                 nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Output raster dimensions should have non-zero size"));
  }

  // nYSize == 0
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALGridContextProcess(ctx, 0.0, 10.0, 0.0, 10.0, 1,
                                                 0, GDT_Float64, &output_pixel,
                                                 nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }
}

TEST(GdalgridTest, ContextProcessProgressAndCancellation) {
  GDALGridMovingAverageOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius1 = 15.0;
  opts.dfRadius2 = 15.0;

  // 1. Mono-threaded progress callback (GDAL_NUM_THREADS = "1").
  CPLSetConfigOption("GDAL_NUM_THREADS", "1");
  absl::Cleanup threads_cleanup = [] {
    CPLSetConfigOption("GDAL_NUM_THREADS", nullptr);
  };

  GDALGridContext* ctx = GDALGridContextCreate(
      GGA_MovingAverage, &opts, k4PointsCount, k4PointsX.data(),
      k4PointsY.data(), k4PointsZ.data(), TRUE);
  ASSERT_NE(nullptr, ctx);
  absl::Cleanup ctx_cleaner = [ctx] { GDALGridContextFree(ctx); };

  std::vector<double> out(4);
  int progress_calls = 0;
  EXPECT_EQ(CE_None,
            GDALGridContextProcess(ctx, 0.0, 10.0, 0.0, 10.0, 2, 2, GDT_Float64,
                                   out.data(), DummyProgress, &progress_calls));
  EXPECT_GT(progress_calls, 0);

  // Mono-threaded cancellation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALGridContextProcess(ctx, 0.0, 10.0, 0.0, 10.0, 2,
                                                 2, GDT_Float64, out.data(),
                                                 CancellingProgress, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  }

  // GDALDummyProgress test.
  EXPECT_EQ(CE_None,
            GDALGridContextProcess(ctx, 0.0, 10.0, 0.0, 10.0, 2, 2, GDT_Float64,
                                   out.data(), GDALDummyProgress, nullptr));

  // 2. Multi-threaded processing (GDAL_NUM_THREADS = "2").
  CPLSetConfigOption("GDAL_NUM_THREADS", "2");
  GDALGridContext* ctx_mt = GDALGridContextCreate(
      GGA_MovingAverage, &opts, k4PointsCount, k4PointsX.data(),
      k4PointsY.data(), k4PointsZ.data(), TRUE);
  ASSERT_NE(nullptr, ctx_mt);
  absl::Cleanup ctx_mt_cleaner = [ctx_mt] { GDALGridContextFree(ctx_mt); };

  std::vector<double> out_mt(16);
  int mt_progress_calls = 0;
  EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_mt, 0.0, 10.0, 0.0, 10.0, 4, 4,
                                            GDT_Float64, out_mt.data(),
                                            DummyProgress, &mt_progress_calls));
  EXPECT_GT(mt_progress_calls, 0);

#if !defined(THREAD_SANITIZER) && !defined(__SANITIZE_THREAD__)
  // Multi-threaded cancellation.
  // Note: Upstream GDALGridJobProcess has an unsynchronized read/write on
  // *psJob->pbStop during cancellation that trips ThreadSanitizer.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALGridContextProcess(
                              ctx_mt, 0.0, 10.0, 0.0, 10.0, 4, 4, GDT_Float64,
                              out_mt.data(), CancellingProgress, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  }
#endif

  // 3. Thread count capped at 128.
  CPLSetConfigOption("GDAL_NUM_THREADS", "200");
  GDALGridContext* ctx_capped = GDALGridContextCreate(
      GGA_MovingAverage, &opts, k4PointsCount, k4PointsX.data(),
      k4PointsY.data(), k4PointsZ.data(), TRUE);
  ASSERT_NE(nullptr, ctx_capped);
  GDALGridContextFree(ctx_capped);
}

// -----------------------------------------------------------------------------
// Algorithm 1: GGA_InverseDistanceToAPower
// -----------------------------------------------------------------------------

TEST(GdalgridTest, InverseDistanceToAPowerNoSearchSimdAndScalar) {
  GDALGridInverseDistanceToAPowerOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfPower = 2.0;
  opts.dfSmoothing = 0.0;
  opts.dfRadius1 = 0.0;
  opts.dfRadius2 = 0.0;
  opts.dfNoDataValue = -9999.0;

  // 1. Scalar fallback: GDAL_USE_AVX="NO", GDAL_USE_SSE="NO".
  {
    CPLSetConfigOption("GDAL_USE_AVX", "NO");
    CPLSetConfigOption("GDAL_USE_SSE", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_USE_AVX", nullptr);
      CPLSetConfigOption("GDAL_USE_SSE", nullptr);
    };

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_InverseDistanceToAPower, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup ctx_cleaner = [ctx] { GDALGridContextFree(ctx); };

    // 4x4 output grid. Center pixels and boundary pixels.
    std::vector<double> out(16);
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 0.0, 9.0, 0.0, 9.0, 4, 4, GDT_Float64,
                                     out.data(), nullptr, nullptr));
    // Pixel centers: dx = 2.25, dy = 2.25. (1.125, 3.375, 5.625, 7.875).
    // All interpolated values should be in [10, 109].
    for (double val : out) {
      EXPECT_GE(val, 10.0);
      EXPECT_LE(val, 109.0);
    }

    // Test coincident point: node exactly at (3.0, 3.0) where Z=43.0.
    double coincident_val = 0.0;
    // 1x1 raster centered at (3.0, 3.0): dfXMin=2.0, dfXMax=4.0, dfYMin=2.0,
    // dfYMax=4.0
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 2.0, 4.0, 2.0, 4.0, 1, 1, GDT_Float64,
                                     &coincident_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(43.0, coincident_val);
  }

  // 2. SSE path configuration: In google3, AVX/SSE intrinsics are not
  // enabled at compile time, so this falls back to scalar gridding while
  // verifying the config options are safely accepted.
  {
    CPLSetConfigOption("GDAL_USE_AVX", "NO");
    CPLSetConfigOption("GDAL_USE_SSE", "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_USE_AVX", nullptr);
      CPLSetConfigOption("GDAL_USE_SSE", nullptr);
    };

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_InverseDistanceToAPower, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup ctx_cleaner = [ctx] { GDALGridContextFree(ctx); };

    double val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &val, nullptr, nullptr));
    // Center point (4.5, 4.5) is symmetric for the 16 regular points
    // (exact: 59.5).
    EXPECT_NEAR(59.5, val, 1e-4);
  }

  // 3. AVX path configuration: Verifies GDAL_USE_AVX config option is accepted.
  {
    CPLSetConfigOption("GDAL_USE_AVX", "YES");
    absl::Cleanup cleanup = [] { CPLSetConfigOption("GDAL_USE_AVX", nullptr); };

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_InverseDistanceToAPower, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup ctx_cleaner = [ctx] { GDALGridContextFree(ctx); };

    double val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &val, nullptr, nullptr));
    EXPECT_NEAR(59.5, val, 1e-4);
  }

  // 4. Power=2.0 with smoothing > 0 (dfSmoothing2 > 0 branch).
  {
    opts.dfSmoothing = 1.0;
    double val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridInverseDistanceToAPowerNoSearch(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &val, nullptr));
    EXPECT_NEAR(59.5, val, 1e-4);
  }

  // 5. Power != 2.0 (!bPower2 branch) with coincident and non-coincident.
  {
    opts.dfPower = 1.0;
    opts.dfSmoothing = 0.0;
    double val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridInverseDistanceToAPowerNoSearch(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &val, nullptr));
    EXPECT_NEAR(59.5, val, 1e-4);

    // Coincident point at (3.0, 3.0)
    double coincident = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridInverseDistanceToAPowerNoSearch(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 3.0, 3.0, &coincident, nullptr));
    EXPECT_DOUBLE_EQ(43.0, coincident);
  }

  // 6. Direct call with 0 points: dfDenominator == 0.0 -> dfNoDataValue.
  {
    double nodata_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridInverseDistanceToAPowerNoSearch(
                           &opts, 0, nullptr, nullptr, nullptr, 0.0, 0.0,
                           &nodata_val, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, nodata_val);
  }
}

TEST(GdalgridTest, InverseDistanceToAPowerSearchEllipse) {
  GDALGridInverseDistanceToAPowerOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfPower = 2.0;
  opts.dfSmoothing = 0.0;
  opts.dfRadius1 = 3.5;
  opts.dfRadius2 = 3.5;
  opts.dfAngle = 0.0;
  opts.nMaxPoints = 0;
  opts.nMinPoints = 0;
  opts.dfNoDataValue = -9999.0;

  // 1. Non-rotated search circle.
  double val = 0.0;
  EXPECT_EQ(CE_None,
            GDALGridInverseDistanceToAPower(
                &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                k16PointsZ.data(), 4.5, 4.5, &val, nullptr));
  // At center (4.5, 4.5), points (3,3), (6,3), (3,6), (6,6) with Z 43, 46, 73,
  // 76 are within 3.5. By symmetry: average = (43+46+73+76)/4 = 59.5.
  EXPECT_NEAR(59.5, val, 1e-4);

  // 2. nMinPoints constraint not met -> nodata.
  opts.nMinPoints = 10;
  double nodata_val = 0.0;
  EXPECT_EQ(CE_None,
            GDALGridInverseDistanceToAPower(
                &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                k16PointsZ.data(), 4.5, 4.5, &nodata_val, nullptr));
  EXPECT_DOUBLE_EQ(-9999.0, nodata_val);
  opts.nMinPoints = 0;

  // 3. nMaxPoints limit.
  opts.nMaxPoints = 2;
  double max_pts_val = 0.0;
  EXPECT_EQ(CE_None,
            GDALGridInverseDistanceToAPower(
                &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                k16PointsZ.data(), 4.5, 4.5, &max_pts_val, nullptr));
  EXPECT_GE(max_pts_val, 10.0);
  opts.nMaxPoints = 0;

  // 4. Coincident point inside search ellipse.
  double coincident = 0.0;
  EXPECT_EQ(CE_None,
            GDALGridInverseDistanceToAPower(
                &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                k16PointsZ.data(), 3.0, 3.0, &coincident, nullptr));
  EXPECT_DOUBLE_EQ(43.0, coincident);

  // 5. No points in search ellipse -> nodata.
  double far_val = 0.0;
  EXPECT_EQ(CE_None,
            GDALGridInverseDistanceToAPower(
                &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                k16PointsZ.data(), 100.0, 100.0, &far_val, nullptr));
  EXPECT_DOUBLE_EQ(-9999.0, far_val);

  // 6. Rotated ellipse (dfAngle = 45.0, dfRadius1 = 8.0, dfRadius2 = 2.0).
  opts.dfAngle = 45.0;
  opts.dfRadius1 = 8.0;
  opts.dfRadius2 = 2.0;
  double rotated_val = 0.0;
  EXPECT_EQ(CE_None,
            GDALGridInverseDistanceToAPower(
                &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                k16PointsZ.data(), 4.5, 4.5, &rotated_val, nullptr));
  EXPECT_GE(rotated_val, 10.0);
  EXPECT_LE(rotated_val, 109.0);
}

// -----------------------------------------------------------------------------
// Algorithm 2: GGA_InverseDistanceToAPowerNearestNeighbor
// -----------------------------------------------------------------------------

TEST(GdalgridTest, InverseDistanceToAPowerNearestNeighbor) {
  GDALGridInverseDistanceToAPowerNearestNeighborOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfPower = 2.0;
  opts.dfRadius = 5.0;
  opts.dfSmoothing = 0.0;
  opts.nMaxPoints = 8;
  opts.nMinPoints = 0;
  opts.dfNoDataValue = -9999.0;
  opts.nMinPointsPerQuadrant = 0;
  opts.nMaxPointsPerQuadrant = 0;

  // 1. Non-quadrant mode through GDALGridContext.
  {
    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_InverseDistanceToAPowerNearestNeighbor, &opts, k16PointsCount,
        k16PointsX.data(), k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double center_val = 0.0;
    // At center (4.5, 4.5): 1x1 raster from 4.0 to 5.0
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &center_val, nullptr, nullptr));
    // Symmetrical 4 inner points (43, 46, 73, 76) at equal distance, plus 8
    // outer points. Result should be near 54.5.
    EXPECT_NEAR(54.5, center_val, 1e-5);

    // Coincident point at (3.0, 3.0)
    double coincident_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 2.0, 4.0, 2.0, 4.0, 1, 1, GDT_Float64,
                                     &coincident_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(43.0, coincident_val);

    // Far point outside radius: should get nodata.
    double far_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 50.0, 52.0, 50.0, 52.0, 1, 1,
                                     GDT_Float64, &far_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, far_val);
  }

  // 2. nMinPoints constraint in non-quadrant mode.
  {
    opts.nMinPoints = 20;  // > 16 points
    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_InverseDistanceToAPowerNearestNeighbor, &opts, k16PointsCount,
        k16PointsX.data(), k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double nodata_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &nodata_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, nodata_val);
    opts.nMinPoints = 0;
  }

  // 3. Quadrant mode (nMinPointsPerQuadrant > 0 and nMaxPointsPerQuadrant > 0).
  {
    opts.dfRadius = 7.0;
    opts.nMinPointsPerQuadrant = 1;
    opts.nMaxPointsPerQuadrant = 2;

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_InverseDistanceToAPowerNearestNeighbor, &opts, k16PointsCount,
        k16PointsX.data(), k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double quad_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &quad_val, nullptr, nullptr));
    EXPECT_NEAR(57.0, quad_val, 1e-5);

    // Coincident point in quadrant mode: query at (3.0, 3.0)
    double quad_coincident = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 2.0, 4.0, 2.0, 4.0, 1, 1, GDT_Float64,
                                     &quad_coincident, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(43.0, quad_coincident);

    // Quadrant condition not met: require 5 points per quadrant (only 4 exist).
    opts.nMinPointsPerQuadrant = 5;
    GDALGridContext* ctx_unmet = GDALGridContextCreate(
        GGA_InverseDistanceToAPowerNearestNeighbor, &opts, k16PointsCount,
        k16PointsX.data(), k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_unmet);
    absl::Cleanup unmet_cleaner = [ctx_unmet] {
      GDALGridContextFree(ctx_unmet);
    };

    double unmet_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_unmet, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &unmet_val,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, unmet_val);
  }
}

// -----------------------------------------------------------------------------
// Algorithm 3: GGA_MovingAverage
// -----------------------------------------------------------------------------

TEST(GdalgridTest, MovingAverage) {
  GDALGridMovingAverageOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius1 = 3.5;
  opts.dfRadius2 = 3.5;
  opts.dfAngle = 0.0;
  opts.nMinPoints = 0;
  opts.nMaxPoints = 0;
  opts.dfNoDataValue = -9999.0;
  opts.nMinPointsPerQuadrant = 0;
  opts.nMaxPointsPerQuadrant = 0;

  // 1. Linear scan mode (dfAngle != 0.0 exercises phQuadTree == nullptr).
  {
    opts.dfAngle = 30.0;
    GDALGridExtraParameters extra{};
    double val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridMovingAverage(&opts, k16PointsCount, k16PointsX.data(),
                                    k16PointsY.data(), k16PointsZ.data(), 4.5,
                                    4.5, &val, &extra));
    EXPECT_NEAR(59.5, val, 1e-4);

    // nMinPoints not met
    opts.nMinPoints = 20;
    double nodata_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridMovingAverage(&opts, k16PointsCount, k16PointsX.data(),
                                    k16PointsY.data(), k16PointsZ.data(), 4.5,
                                    4.5, &nodata_val, &extra));
    EXPECT_DOUBLE_EQ(-9999.0, nodata_val);
    opts.nMinPoints = 0;
    opts.dfAngle = 0.0;
  }

  // 2. Quadtree mode (dfAngle == 0.0 and GDAL_GRID_POINT_COUNT_THRESHOLD="4").
  {
    CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", "4");
    absl::Cleanup threshold_cleanup = [] {
      CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", nullptr);
    };

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_MovingAverage, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double val = 0.0;
    // Exactly 4 points (43, 46, 73, 76) inside radius 3.5. Average = 59.5.
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(59.5, val);

    // Far query: no points found in quadtree.
    double far_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 50.0, 52.0, 50.0, 52.0, 1, 1,
                                     GDT_Float64, &far_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, far_val);
  }

  // 3. Quadrant mode.
  {
    opts.dfRadius1 = 7.0;
    opts.dfRadius2 = 7.0;
    opts.nMinPointsPerQuadrant = 1;
    opts.nMaxPointsPerQuadrant = 2;

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_MovingAverage, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double quad_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 4.0, 5.0, 4.0, 5.0, 1, 1, GDT_Float64,
                                     &quad_val, nullptr, nullptr));
    EXPECT_NEAR(52.0, quad_val, 1e-5);

    // Unmet quadrant requirement: 5 points per quadrant.
    opts.nMinPointsPerQuadrant = 5;
    GDALGridContext* ctx_unmet = GDALGridContextCreate(
        GGA_MovingAverage, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_unmet);
    absl::Cleanup unmet_cleaner = [ctx_unmet] {
      GDALGridContextFree(ctx_unmet);
    };

    double unmet_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_unmet, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &unmet_val,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, unmet_val);
  }
}

// -----------------------------------------------------------------------------
// Algorithm 4: GGA_NearestNeighbor
// -----------------------------------------------------------------------------

TEST(GdalgridTest, NearestNeighbor) {
  GDALGridNearestNeighborOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius1 = 3.5;
  opts.dfRadius2 = 3.5;
  opts.dfAngle = 0.0;
  opts.dfNoDataValue = -9999.0;

  // 1. Linear scan mode (dfAngle != 0.0).
  {
    opts.dfAngle = 45.0;
    GDALGridExtraParameters extra{};
    double val = 0.0;
    // Nearest to (3.1, 3.1) is point (3.0, 3.0) with Z=43.0.
    EXPECT_EQ(CE_None,
              GDALGridNearestNeighbor(&opts, k16PointsCount, k16PointsX.data(),
                                      k16PointsY.data(), k16PointsZ.data(), 3.1,
                                      3.1, &val, &extra));
    EXPECT_DOUBLE_EQ(43.0, val);

    // Far query: no points found in ellipse.
    double far_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridNearestNeighbor(&opts, k16PointsCount, k16PointsX.data(),
                                      k16PointsY.data(), k16PointsZ.data(),
                                      100.0, 100.0, &far_val, &extra));
    EXPECT_DOUBLE_EQ(-9999.0, far_val);
    opts.dfAngle = 0.0;
  }

  // 2. Quadtree mode with search radius (lower point count threshold).
  {
    CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", "4");
    absl::Cleanup cleanup_threshold = [] {
      CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", nullptr);
    };

    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_NearestNeighbor, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double val = 0.0;
    // Center at (3.1, 3.1) -> 1x1 raster from 2.6 to 3.6
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 2.6, 3.6, 2.6, 3.6, 1, 1, GDT_Float64,
                                     &val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(43.0, val);

    // Far query: outside radius 3.5.
    double far_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx, 50.0, 52.0, 50.0, 52.0, 1, 1,
                                     GDT_Float64, &far_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, far_val);
  }

  // 3. Linear scan without search radius (dfRadius1=0, dfRadius2=0: unbounded
  // search ellipse).
  {
    opts.dfRadius1 = 0.0;
    opts.dfRadius2 = 0.0;
    GDALGridContext* ctx = GDALGridContextCreate(
        GGA_NearestNeighbor, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx);
    absl::Cleanup cleaner = [ctx] { GDALGridContextFree(ctx); };

    double far_unbounded = 0.0;
    // Query at (15.0, 15.0). Nearest point is (9.0, 9.0) with Z=109.0.
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx, 14.5, 15.5, 14.5, 15.5, 1, 1,
                                              GDT_Float64, &far_unbounded,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(109.0, far_unbounded);
  }
}

// -----------------------------------------------------------------------------
// Algorithms 5 & 6: MetricMinimum and MetricMaximum
// -----------------------------------------------------------------------------

TEST(GdalgridTest, MetricMinimumAndMaximum) {
  GDALGridDataMetricsOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius1 = 3.5;
  opts.dfRadius2 = 3.5;
  opts.dfAngle = 0.0;
  opts.nMinPoints = 0;
  opts.dfNoDataValue = -9999.0;
  opts.nMinPointsPerQuadrant = 0;
  opts.nMaxPointsPerQuadrant = 0;

  // 1. Linear scan mode (dfAngle != 0.0).
  {
    opts.dfAngle = 30.0;
    GDALGridExtraParameters extra{};
    double min_val = 0.0;
    double max_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridDataMetricMinimum(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &min_val, &extra));
    EXPECT_EQ(CE_None,
              GDALGridDataMetricMaximum(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &max_val, &extra));
    EXPECT_LE(min_val, max_val);

    // Min points constraint.
    opts.nMinPoints = 20;
    double nodata_min = 0.0;
    double nodata_max = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridDataMetricMinimum(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &nodata_min, &extra));
    EXPECT_EQ(CE_None,
              GDALGridDataMetricMaximum(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &nodata_max, &extra));
    EXPECT_DOUBLE_EQ(-9999.0, nodata_min);
    EXPECT_DOUBLE_EQ(-9999.0, nodata_max);
    opts.nMinPoints = 0;
    opts.dfAngle = 0.0;
  }

  // 2. Quadtree mode (dfAngle == 0.0, threshold = "4").
  {
    CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", "4");
    absl::Cleanup threshold_cleanup = [] {
      CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", nullptr);
    };

    GDALGridContext* ctx_min = GDALGridContextCreate(
        GGA_MetricMinimum, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_min);
    absl::Cleanup cleaner_min = [ctx_min] { GDALGridContextFree(ctx_min); };

    GDALGridContext* ctx_max = GDALGridContextCreate(
        GGA_MetricMaximum, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_max);
    absl::Cleanup cleaner_max = [ctx_max] { GDALGridContextFree(ctx_max); };

    double min_val = 0.0;
    double max_val = 0.0;
    // Inside radius 3.5 from (4.5, 4.5), the points are Z = 43, 46, 73, 76.
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_min, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &min_val, nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_max, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &max_val, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(43.0, min_val);
    EXPECT_DOUBLE_EQ(76.0, max_val);

    // Far query: nodata.
    double far_min = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_min, 50.0, 52.0, 50.0, 52.0, 1, 1,
                                     GDT_Float64, &far_min, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, far_min);
  }

  // 3. Quadrant mode (minimum and maximum per quadrant).
  {
    opts.dfRadius1 = 7.0;
    opts.dfRadius2 = 7.0;
    opts.nMinPointsPerQuadrant = 1;
    opts.nMaxPointsPerQuadrant = 2;

    GDALGridContext* ctx_min_q = GDALGridContextCreate(
        GGA_MetricMinimum, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_min_q);
    absl::Cleanup cleaner_min_q = [ctx_min_q] {
      GDALGridContextFree(ctx_min_q);
    };

    GDALGridContext* ctx_max_q = GDALGridContextCreate(
        GGA_MetricMaximum, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_max_q);
    absl::Cleanup cleaner_max_q = [ctx_max_q] {
      GDALGridContextFree(ctx_max_q);
    };

    double min_val = 0.0;
    double max_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_min_q, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &min_val, nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_max_q, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &max_val, nullptr, nullptr));
    EXPECT_GE(min_val, 10.0);
    EXPECT_LE(max_val, 109.0);
    EXPECT_LE(min_val, max_val);

    // Unmet quadrant requirement.
    opts.nMinPointsPerQuadrant = 5;
    GDALGridContext* ctx_unmet = GDALGridContextCreate(
        GGA_MetricMinimum, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_unmet);
    absl::Cleanup unmet_cleaner = [ctx_unmet] {
      GDALGridContextFree(ctx_unmet);
    };

    double unmet_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_unmet, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &unmet_val,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, unmet_val);
  }
}

// -----------------------------------------------------------------------------
// Algorithms 7 & 8: MetricRange and MetricCount
// -----------------------------------------------------------------------------

TEST(GdalgridTest, MetricRangeAndCount) {
  GDALGridDataMetricsOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius1 = 3.5;
  opts.dfRadius2 = 3.5;
  opts.dfAngle = 0.0;
  opts.nMinPoints = 0;
  opts.dfNoDataValue = -9999.0;
  opts.nMinPointsPerQuadrant = 0;
  opts.nMaxPointsPerQuadrant = 0;

  // 1. Linear scan mode (dfAngle != 0.0).
  {
    opts.dfAngle = 30.0;
    GDALGridExtraParameters extra{};
    double range_val = 0.0;
    double count_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridDataMetricRange(&opts, k16PointsCount, k16PointsX.data(),
                                      k16PointsY.data(), k16PointsZ.data(), 4.5,
                                      4.5, &range_val, &extra));
    EXPECT_EQ(CE_None,
              GDALGridDataMetricCount(&opts, k16PointsCount, k16PointsX.data(),
                                      k16PointsY.data(), k16PointsZ.data(), 4.5,
                                      4.5, &count_val, &extra));
    EXPECT_GE(range_val, 0.0);
    EXPECT_GE(count_val, 1.0);

    // Min points constraint.
    opts.nMinPoints = 20;
    double nodata_range = 0.0;
    double nodata_count = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridDataMetricRange(&opts, k16PointsCount, k16PointsX.data(),
                                      k16PointsY.data(), k16PointsZ.data(), 4.5,
                                      4.5, &nodata_range, &extra));
    EXPECT_EQ(CE_None,
              GDALGridDataMetricCount(&opts, k16PointsCount, k16PointsX.data(),
                                      k16PointsY.data(), k16PointsZ.data(), 4.5,
                                      4.5, &nodata_count, &extra));
    EXPECT_DOUBLE_EQ(-9999.0, nodata_range);
    EXPECT_DOUBLE_EQ(-9999.0, nodata_count);
    opts.nMinPoints = 0;
    opts.dfAngle = 0.0;
  }

  // 2. Quadtree mode (dfAngle == 0.0, threshold = "4").
  {
    CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", "4");
    absl::Cleanup threshold_cleanup = [] {
      CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", nullptr);
    };

    GDALGridContext* ctx_range = GDALGridContextCreate(
        GGA_MetricRange, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_range);
    absl::Cleanup cleaner_range = [ctx_range] {
      GDALGridContextFree(ctx_range);
    };

    GDALGridContext* ctx_count = GDALGridContextCreate(
        GGA_MetricCount, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_count);
    absl::Cleanup cleaner_count = [ctx_count] {
      GDALGridContextFree(ctx_count);
    };

    double range_val = 0.0;
    double count_val = 0.0;
    // Inside radius 3.5: Z = 43, 46, 73, 76.
    // Range = 76 - 43 = 33. Count = 4.
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_range, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &range_val,
                                              nullptr, nullptr));
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_count, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &count_val,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(33.0, range_val);
    EXPECT_DOUBLE_EQ(4.0, count_val);

    // Far query: nodata.
    double far_range = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_range, 50.0, 52.0, 50.0, 52.0,
                                              1, 1, GDT_Float64, &far_range,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, far_range);
  }

  // 3. Quadrant mode.
  {
    opts.dfRadius1 = 7.0;
    opts.dfRadius2 = 7.0;
    opts.nMinPointsPerQuadrant = 1;
    opts.nMaxPointsPerQuadrant = 2;

    GDALGridContext* ctx_range_q = GDALGridContextCreate(
        GGA_MetricRange, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_range_q);
    absl::Cleanup cleaner_range_q = [ctx_range_q] {
      GDALGridContextFree(ctx_range_q);
    };

    GDALGridContext* ctx_count_q = GDALGridContextCreate(
        GGA_MetricCount, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_count_q);
    absl::Cleanup cleaner_count_q = [ctx_count_q] {
      GDALGridContextFree(ctx_count_q);
    };

    double range_val = 0.0;
    double count_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_range_q, 4.0, 5.0, 4.0, 5.0,
                                              1, 1, GDT_Float64, &range_val,
                                              nullptr, nullptr));
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_count_q, 4.0, 5.0, 4.0, 5.0,
                                              1, 1, GDT_Float64, &count_val,
                                              nullptr, nullptr));
    EXPECT_GE(range_val, 0.0);
    EXPECT_DOUBLE_EQ(8.0, count_val);  // 4 quadrants * 2 max points = 8 points.

    // Unmet quadrant requirement.
    opts.nMinPointsPerQuadrant = 5;
    GDALGridContext* ctx_unmet = GDALGridContextCreate(
        GGA_MetricRange, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_unmet);
    absl::Cleanup unmet_cleaner = [ctx_unmet] {
      GDALGridContextFree(ctx_unmet);
    };

    double unmet_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_unmet, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &unmet_val,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, unmet_val);
  }
}

// -----------------------------------------------------------------------------
// Algorithms 9 & 10: MetricAverageDistance and MetricAverageDistancePts
// -----------------------------------------------------------------------------

TEST(GdalgridTest, MetricAverageDistanceAndAverageDistancePts) {
  GDALGridDataMetricsOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius1 = 3.5;
  opts.dfRadius2 = 3.5;
  opts.dfAngle = 0.0;
  opts.nMinPoints = 0;
  opts.dfNoDataValue = -9999.0;
  opts.nMinPointsPerQuadrant = 0;
  opts.nMaxPointsPerQuadrant = 0;

  // 1. Linear scan mode (dfAngle != 0.0).
  {
    opts.dfAngle = 30.0;
    GDALGridExtraParameters extra{};
    double avg_dist = 0.0;
    double avg_dist_pts = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridDataMetricAverageDistance(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &avg_dist, &extra));
    EXPECT_EQ(CE_None,
              GDALGridDataMetricAverageDistancePts(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &avg_dist_pts, &extra));
    EXPECT_GT(avg_dist, 0.0);
    EXPECT_GT(avg_dist_pts, 0.0);

    // Min points constraint.
    opts.nMinPoints = 20;
    double nodata_dist = 0.0;
    double nodata_pts = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridDataMetricAverageDistance(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &nodata_dist, &extra));
    EXPECT_EQ(CE_None,
              GDALGridDataMetricAverageDistancePts(
                  &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
                  k16PointsZ.data(), 4.5, 4.5, &nodata_pts, &extra));
    EXPECT_DOUBLE_EQ(-9999.0, nodata_dist);
    EXPECT_DOUBLE_EQ(-9999.0, nodata_pts);
    opts.nMinPoints = 0;
    opts.dfAngle = 0.0;
  }

  // 2. Quadtree mode (dfAngle == 0.0, threshold = "4").
  {
    CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", "4");
    absl::Cleanup threshold_cleanup = [] {
      CPLSetConfigOption("GDAL_GRID_POINT_COUNT_THRESHOLD", nullptr);
    };

    GDALGridContext* ctx_dist = GDALGridContextCreate(
        GGA_MetricAverageDistance, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_dist);
    absl::Cleanup cleaner_dist = [ctx_dist] { GDALGridContextFree(ctx_dist); };

    GDALGridContext* ctx_pts = GDALGridContextCreate(
        GGA_MetricAverageDistancePts, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_pts);
    absl::Cleanup cleaner_pts = [ctx_pts] { GDALGridContextFree(ctx_pts); };

    double dist_val = 0.0;
    double pts_val = 0.0;
    // Inside radius 3.5: 4 points at distance sqrt(1.5^2 + 1.5^2) = sqrt(4.5)
    // ~ 2.1213.
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_dist, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &dist_val, nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_pts, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &pts_val, nullptr, nullptr));
    EXPECT_NEAR(std::sqrt(4.5), dist_val, 1e-4);
    EXPECT_GT(pts_val, 0.0);

    // Far query: nodata.
    double far_dist = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_dist, 50.0, 52.0, 50.0, 52.0, 1, 1,
                                     GDT_Float64, &far_dist, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, far_dist);
  }

  // 3. Quadrant mode for AverageDistance.
  {
    opts.dfRadius1 = 7.0;
    opts.dfRadius2 = 7.0;
    opts.nMinPointsPerQuadrant = 1;
    opts.nMaxPointsPerQuadrant = 2;

    GDALGridContext* ctx_dist_q = GDALGridContextCreate(
        GGA_MetricAverageDistance, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_dist_q);
    absl::Cleanup cleaner_dist_q = [ctx_dist_q] {
      GDALGridContextFree(ctx_dist_q);
    };

    double dist_val = 0.0;
    EXPECT_EQ(CE_None,
              GDALGridContextProcess(ctx_dist_q, 4.0, 5.0, 4.0, 5.0, 1, 1,
                                     GDT_Float64, &dist_val, nullptr, nullptr));
    EXPECT_GT(dist_val, 0.0);

    // Unmet quadrant requirement.
    opts.nMinPointsPerQuadrant = 5;
    GDALGridContext* ctx_unmet = GDALGridContextCreate(
        GGA_MetricAverageDistance, &opts, k16PointsCount, k16PointsX.data(),
        k16PointsY.data(), k16PointsZ.data(), TRUE);
    ASSERT_NE(nullptr, ctx_unmet);
    absl::Cleanup unmet_cleaner = [ctx_unmet] {
      GDALGridContextFree(ctx_unmet);
    };

    double unmet_val = 0.0;
    EXPECT_EQ(CE_None, GDALGridContextProcess(ctx_unmet, 4.0, 5.0, 4.0, 5.0, 1,
                                              1, GDT_Float64, &unmet_val,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(-9999.0, unmet_val);
  }
}

// -----------------------------------------------------------------------------
// Algorithm 11: GGA_Linear (Delaunay Triangulation)
// -----------------------------------------------------------------------------

TEST(GdalgridTest, LinearUnavailableWithoutQHull) {
  GDALGridLinearOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfRadius = -1.0;
  opts.dfNoDataValue = -9999.0;

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALGridContext* ctx = GDALGridContextCreate(
      GGA_Linear, &opts, k16PointsCount, k16PointsX.data(), k16PointsY.data(),
      k16PointsZ.data(), TRUE);
  EXPECT_EQ(nullptr, ctx);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without QHull support"));
}

// -----------------------------------------------------------------------------
// Output Data Types Specializations via GDALGridCreate
// -----------------------------------------------------------------------------

TEST(GdalgridTest, OutputDataTypes) {
  GDALGridInverseDistanceToAPowerOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  opts.dfPower = 2.0;
  opts.dfSmoothing = 0.0;
  opts.dfRadius1 = 0.0;
  opts.dfRadius2 = 0.0;
  opts.dfNoDataValue = 0.0;

  constexpr GUInt32 nX = 2;
  constexpr GUInt32 nY = 2;
  constexpr GUInt32 nPixels = nX * nY;

  // 1. GDT_Byte
  std::vector<uint8_t> out_byte(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Byte,
                           out_byte.data(), nullptr, nullptr));
  for (uint8_t val : out_byte) {
    EXPECT_GE(val, 10);
    EXPECT_LE(val, 40);
  }

  // 2. GDT_Int8
  std::vector<int8_t> out_int8(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Int8,
                           out_int8.data(), nullptr, nullptr));
  for (int8_t val : out_int8) {
    EXPECT_GE(val, 10);
    EXPECT_LE(val, 40);
  }

  // 3. GDT_UInt16
  std::vector<uint16_t> out_uint16(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_UInt16,
                           out_uint16.data(), nullptr, nullptr));
  for (uint16_t val : out_uint16) {
    EXPECT_GE(val, 10);
    EXPECT_LE(val, 40);
  }

  // 4. GDT_Int16
  std::vector<int16_t> out_int16(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Int16,
                           out_int16.data(), nullptr, nullptr));
  for (int16_t val : out_int16) {
    EXPECT_GE(val, 10);
    EXPECT_LE(val, 40);
  }

  // 5. GDT_UInt32
  std::vector<uint32_t> out_uint32(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_UInt32,
                           out_uint32.data(), nullptr, nullptr));
  for (uint32_t val : out_uint32) {
    EXPECT_GE(val, 10u);
    EXPECT_LE(val, 40u);
  }

  // 6. GDT_Int32
  std::vector<int32_t> out_int32(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Int32,
                           out_int32.data(), nullptr, nullptr));
  for (int32_t val : out_int32) {
    EXPECT_GE(val, 10);
    EXPECT_LE(val, 40);
  }

  // 7. GDT_UInt64
  std::vector<uint64_t> out_uint64(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_UInt64,
                           out_uint64.data(), nullptr, nullptr));
  for (uint64_t val : out_uint64) {
    EXPECT_GE(val, 10u);
    EXPECT_LE(val, 40u);
  }

  // 8. GDT_Int64
  std::vector<int64_t> out_int64(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Int64,
                           out_int64.data(), nullptr, nullptr));
  for (int64_t val : out_int64) {
    EXPECT_GE(val, 10);
    EXPECT_LE(val, 40);
  }

  // 9. GDT_Float32
  std::vector<float> out_float32(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Float32,
                           out_float32.data(), nullptr, nullptr));
  for (float val : out_float32) {
    EXPECT_GE(val, 10.0f);
    EXPECT_LE(val, 40.0f);
  }

  // 10. GDT_Float64
  std::vector<double> out_float64(nPixels);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_Float64,
                           out_float64.data(), nullptr, nullptr));
  for (double val : out_float64) {
    EXPECT_GE(val, 10.0);
    EXPECT_LE(val, 40.0);
  }

  // 11. Complex data type: GDT_CFloat32 (2 floats per pixel).
  std::vector<float> out_cfloat32(nPixels * 2);
  EXPECT_EQ(CE_None,
            GDALGridCreate(GGA_InverseDistanceToAPower, &opts, k4PointsCount,
                           k4PointsX.data(), k4PointsY.data(), k4PointsZ.data(),
                           0.0, 10.0, 0.0, 10.0, nX, nY, GDT_CFloat32,
                           out_cfloat32.data(), nullptr, nullptr));
  for (size_t i = 0; i < nPixels; ++i) {
    EXPECT_GE(out_cfloat32[i * 2], 10.0f);
    EXPECT_LE(out_cfloat32[i * 2], 40.0f);
    EXPECT_FLOAT_EQ(0.0f,
                    out_cfloat32[i * 2 + 1]);  // Imaginary component is 0.
  }
}

TEST(GdalgridTest, GDALGridCreateErrorHandling) {
  // 1. Invalid algorithm in GDALGridCreate -> returns CE_Failure.
  GDALGridMovingAverageOptions opts{};
  opts.nSizeOfStructure = sizeof(opts);
  double out = 0.0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALGridCreate(static_cast<GDALGridAlgorithm>(0), &opts,
                             k4PointsCount, k4PointsX.data(), k4PointsY.data(),
                             k4PointsZ.data(), 0.0, 10.0, 0.0, 10.0, 1, 1,
                             GDT_Float64, &out, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Invalid structure size in GDALGridCreate -> returns CE_Failure.
  opts.nSizeOfStructure = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALGridCreate(GGA_MovingAverage, &opts, k4PointsCount,
                             k4PointsX.data(), k4PointsY.data(),
                             k4PointsZ.data(), 0.0, 10.0, 0.0, 10.0, 1, 1,
                             GDT_Float64, &out, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. GGA_Linear without QHull support -> returns CE_Failure.
  GDALGridLinearOptions linear_opts{};
  linear_opts.nSizeOfStructure = sizeof(linear_opts);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALGridCreate(GGA_Linear, &linear_opts, k3CollinearCount,
                             k3CollinearX.data(), k3CollinearY.data(),
                             k3CollinearZ.data(), 0.0, 10.0, 0.0, 10.0, 1, 1,
                             GDT_Float64, &out, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("without QHull support"));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - QHull Delaunay triangulation is compiled out in google3.
