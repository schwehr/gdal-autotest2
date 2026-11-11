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
// Tests Delaunay triangulation data structures, barycentric coordinate math,
// and brute-force/directed facet lookup (alg/delaunay.c).
//
// Note: In Google3 builds (//third_party/gdal/BUILD), QHull is not compiled in
// (neither INTERNAL_QHULL nor EXTERNAL_QHULL is defined), so
// GDALHasTriangulation() returns FALSE and GDALTriangulationCreateDelaunay()
// reports CPLE_NotSupported. All other functions in alg/delaunay.c operate on
// caller-supplied GDALTriangulation structs and are compiled in
// unconditionally.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/delaunay.c
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_alg.h
//   https://gdal.org/api/gdal_alg.html

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>  // NOLINT

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

using TriangulationUniquePtr =
    std::unique_ptr<GDALTriangulation, decltype([](GDALTriangulation* dt) {
                      GDALTriangulationFree(dt);
                    })>;

// Allocates a GDALTriangulation and its pasFacets array using CPLCalloc so
// GDALTriangulationFree() releases them with matching VSIFree() calls.
TriangulationUniquePtr CreateTriangulation(
    std::span<const GDALTriFacet> facets) {
  auto* dt =
      static_cast<GDALTriangulation*>(CPLCalloc(1, sizeof(GDALTriangulation)));
  dt->nFacets = static_cast<int>(facets.size());
  if (!facets.empty()) {
    dt->pasFacets = static_cast<GDALTriFacet*>(
        CPLCalloc(facets.size(), sizeof(GDALTriFacet)));
    std::copy(facets.begin(), facets.end(), dt->pasFacets);
  }
  return TriangulationUniquePtr(dt);
}

TEST(DelaunayTest, HasTriangulationAndCreateDelaunayWithoutQHull) {
  // 1. In Google3 builds without QHull, GDALHasTriangulation() returns FALSE.
  EXPECT_EQ(FALSE, GDALHasTriangulation());

  // 2. GDALTriangulationCreateDelaunay() reports CPLE_NotSupported and returns
  // nullptr.
  constexpr auto kX = std::to_array<double>({0.0, 1.0, 0.0});
  constexpr auto kY = std::to_array<double>({0.0, 0.0, 1.0});
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    TriangulationUniquePtr dt(GDALTriangulationCreateDelaunay(
        static_cast<int>(kX.size()), kX.data(), kY.data()));
    EXPECT_EQ(nullptr, dt);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALTriangulationCreateDelaunay() unavailable since "
                          "GDAL built without QHull support"));
    CPLErrorReset();
  }

  // 3. GDALTriangulationFree(nullptr) is a safe no-op.
  GDALTriangulationFree(nullptr);
}

TEST(DelaunayTest, ComputeBarycentricCoefficientsAndIdempotence) {
  // Vertices:
  //   0: (11, 7), 1: (5, 24), 2: (1, 2) -> non-axis-aligned triangle with
  //      dfDenom = (24 - 2)*(11 - 1) + (1 - 5)*(7 - 2) = 200.0 so all 6
  //      coefficients are distinct and non-zero.
  //   3: (5, 5), 4: (9, 8) -> exactly collinear with vertex 2 (1, 2), so
  //      dfDenom == 0.0 (degenerate triangle branch).
  //   5: (2, 2), 6: (1, 2 + 5e-6) -> nearly collinear with vertex 2 (1, 2), so
  //      0 < fabs(dfDenom) = 5e-6 < 1e-5 (near-degenerate threshold branch).
  constexpr auto kX =
      std::to_array<double>({11.0, 5.0, 1.0, 5.0, 9.0, 2.0, 1.0});
  constexpr auto kY =
      std::to_array<double>({7.0, 24.0, 2.0, 5.0, 8.0, 2.0, 2.0 + 5e-6});
  constexpr auto kFacets = std::to_array<GDALTriFacet>({
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {-1, -1, -1}},
      {.anVertexIdx = {2, 3, 4}, .anNeighborIdx = {-1, -1, -1}},
      {.anVertexIdx = {2, 5, 6}, .anNeighborIdx = {-1, -1, -1}},
  });

  TriangulationUniquePtr dt = CreateTriangulation(kFacets);
  ASSERT_NE(nullptr, dt);
  ASSERT_EQ(nullptr, dt->pasFacetCoefficients);

  // 1. First call allocates pasFacetCoefficients and populates all facets.
  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      dt.get(), kX.data(), kY.data()));
  ASSERT_NE(nullptr, dt->pasFacetCoefficients);

  // Non-degenerate facet 0:
  //   dfMul1X = (24 - 2) / 200 = 0.11,   dfMul1Y = (1 - 5) / 200 = -0.02
  //   dfMul2X = (2 - 7) / 200 = -0.025,  dfMul2Y = (11 - 1) / 200 = 0.05
  //   dfCstX = 1.0,                      dfCstY = 2.0
  const GDALTriBarycentricCoefficients& c0 = dt->pasFacetCoefficients[0];
  EXPECT_DOUBLE_EQ(0.11, c0.dfMul1X);
  EXPECT_DOUBLE_EQ(-0.02, c0.dfMul1Y);
  EXPECT_DOUBLE_EQ(-0.025, c0.dfMul2X);
  EXPECT_DOUBLE_EQ(0.05, c0.dfMul2Y);
  EXPECT_DOUBLE_EQ(1.0, c0.dfCstX);
  EXPECT_DOUBLE_EQ(2.0, c0.dfCstY);

  // Degenerate facets 1 (dfDenom == 0) and 2 (0 < |dfDenom| < 1e-5): all
  // coefficients are zero-initialized.
  for (int facet_idx : {1, 2}) {
    const GDALTriBarycentricCoefficients& c =
        dt->pasFacetCoefficients[facet_idx];
    EXPECT_DOUBLE_EQ(0.0, c.dfMul1X);
    EXPECT_DOUBLE_EQ(0.0, c.dfMul1Y);
    EXPECT_DOUBLE_EQ(0.0, c.dfMul2X);
    EXPECT_DOUBLE_EQ(0.0, c.dfMul2Y);
    EXPECT_DOUBLE_EQ(0.0, c.dfCstX);
    EXPECT_DOUBLE_EQ(0.0, c.dfCstY);
  }

  // 2. Second call when pasFacetCoefficients != nullptr returns TRUE
  // immediately without reallocating or overwriting existing coefficients.
  const GDALTriBarycentricCoefficients* const orig_ptr =
      dt->pasFacetCoefficients;
  constexpr auto kOtherX =
      std::to_array<double>({999.0, 999.0, 999.0, 999.0, 999.0, 999.0, 999.0});
  constexpr auto kOtherY =
      std::to_array<double>({999.0, 999.0, 999.0, 999.0, 999.0, 999.0, 999.0});
  EXPECT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      dt.get(), kOtherX.data(), kOtherY.data()));
  EXPECT_EQ(orig_ptr, dt->pasFacetCoefficients);
  EXPECT_DOUBLE_EQ(0.11, dt->pasFacetCoefficients[0].dfMul1X);
  EXPECT_DOUBLE_EQ(0.05, dt->pasFacetCoefficients[0].dfMul2Y);

  // 3. When nFacets == 0, VSI_MALLOC2_VERBOSE(..., 0) returns nullptr silently
  // (without emitting a CPLError) and
  // GDALTriangulationComputeBarycentricCoefficients returns FALSE.
  TriangulationUniquePtr empty_dt = CreateTriangulation({});
  ASSERT_NE(nullptr, empty_dt);
  CPLErrorReset();
  EXPECT_EQ(FALSE, GDALTriangulationComputeBarycentricCoefficients(
                       empty_dt.get(), kX.data(), kY.data()));
  EXPECT_EQ(nullptr, empty_dt->pasFacetCoefficients);
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  EXPECT_STREQ("", CPLGetLastErrorMsg());
}

TEST(DelaunayTest, ComputeBarycentricCoordinatesValidationAndAccuracy) {
  // Asymmetric triangle: v0 = (2, 1), v1 = (8, 3), v2 = (4, 9).
  constexpr auto kX = std::to_array<double>({2.0, 8.0, 4.0});
  constexpr auto kY = std::to_array<double>({1.0, 3.0, 9.0});
  constexpr auto kFacets = std::to_array<GDALTriFacet>({
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {-1, -1, -1}},
  });

  TriangulationUniquePtr dt = CreateTriangulation(kFacets);
  ASSERT_NE(nullptr, dt);

  // 1. Calling GDALTriangulationComputeBarycentricCoordinates before
  // GDALTriangulationComputeBarycentricCoefficients fails with CPLE_AppDefined
  // and leaves output parameters unmodified.
  double l1 = -1.0;
  double l2 = -1.0;
  double l3 = -1.0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALTriangulationComputeBarycentricCoordinates(
                         dt.get(), 0, 4.0, 4.0, &l1, &l2, &l3));
    EXPECT_DOUBLE_EQ(-1.0, l1);
    EXPECT_DOUBLE_EQ(-1.0, l2);
    EXPECT_DOUBLE_EQ(-1.0, l3);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALTriangulationComputeBarycentricCoefficients() "
                          "should be called before"));
    CPLErrorReset();
  }

  // 2. Compute coefficients and verify barycentric coordinates across vertices,
  // edge midpoints, centroid, and interior/exterior points.
  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      dt.get(), kX.data(), kY.data()));

  struct BarycentricCase {
    double expected_l1;
    double expected_l2;
    double expected_l3;
  };
  constexpr auto kCases = std::to_array<BarycentricCase>({
      {.expected_l1 = 1.0, .expected_l2 = 0.0, .expected_l3 = 0.0},
      {.expected_l1 = 0.0, .expected_l2 = 1.0, .expected_l3 = 0.0},
      {.expected_l1 = 0.0, .expected_l2 = 0.0, .expected_l3 = 1.0},
      {.expected_l1 = 0.5, .expected_l2 = 0.5, .expected_l3 = 0.0},
      {.expected_l1 = 0.0, .expected_l2 = 0.5, .expected_l3 = 0.5},
      {.expected_l1 = 0.5, .expected_l2 = 0.0, .expected_l3 = 0.5},
      {.expected_l1 = 1.0 / 3.0,
       .expected_l2 = 1.0 / 3.0,
       .expected_l3 = 1.0 / 3.0},
      {.expected_l1 = 0.2, .expected_l2 = 0.5, .expected_l3 = 0.3},
      {.expected_l1 = -0.25, .expected_l2 = 0.75, .expected_l3 = 0.5},
  });

  for (const auto& tc : kCases) {
    const double x = tc.expected_l1 * kX[0] + tc.expected_l2 * kX[1] +
                     tc.expected_l3 * kX[2];
    const double y = tc.expected_l1 * kY[0] + tc.expected_l2 * kY[1] +
                     tc.expected_l3 * kY[2];
    l1 = -99.0;
    l2 = -99.0;
    l3 = -99.0;
    EXPECT_EQ(TRUE, GDALTriangulationComputeBarycentricCoordinates(
                        dt.get(), 0, x, y, &l1, &l2, &l3));
    EXPECT_NEAR(tc.expected_l1, l1, 1e-12);
    EXPECT_NEAR(tc.expected_l2, l2, 1e-12);
    EXPECT_NEAR(tc.expected_l3, l3, 1e-12);
    // Pins BARYC_COORD_L3(l1, l2) == 1.0 - l1 - l2.
    EXPECT_DOUBLE_EQ(1.0, l1 + l2 + l3);
  }
}

TEST(DelaunayTest, FindFacetBruteForceAllBranches) {
  // Vertices:
  //   0..2: (1, 0), (0, 1), (0, 0) -> unit right triangle where l1 = x, l2 = y,
  //         l3 = 1 - x - y exactly.
  //   3..5: (0, 0), (1, 1), (2, 2) -> degenerate collinear points.
  //   6..8: (11, 0), (10, 1), (10, 0) -> shifted unit right triangle at x + 10.
  constexpr auto kX =
      std::to_array<double>({1.0, 0.0, 0.0, 0.0, 1.0, 2.0, 11.0, 10.0, 10.0});
  constexpr auto kY =
      std::to_array<double>({0.0, 1.0, 0.0, 0.0, 1.0, 2.0, 0.0, 1.0, 0.0});

  // Facet 0: degenerate triangle (skipped by brute force).
  // Facet 1: unit right triangle with interior neighbors {2, 2, 2} so any
  //          failed coordinate check on facet 1 continues the loop to facet 2.
  // Facet 2: shifted unit right triangle with interior neighbors {1, 1, 1}.
  constexpr auto kFacets = std::to_array<GDALTriFacet>({
      {.anVertexIdx = {3, 4, 5}, .anNeighborIdx = {-1, -1, -1}},
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {2, 2, 2}},
      {.anVertexIdx = {6, 7, 8}, .anNeighborIdx = {1, 1, 1}},
  });

  TriangulationUniquePtr dt = CreateTriangulation(kFacets);
  ASSERT_NE(nullptr, dt);

  // 1. Calling before GDALTriangulationComputeBarycentricCoefficients fails.
  int out_facet = 99;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALTriangulationFindFacetBruteForce(dt.get(), 0.25, 0.25,
                                                          &out_facet));
    EXPECT_EQ(-1, out_facet);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALTriangulationComputeBarycentricCoefficients() "
                          "should be called before"));
    CPLErrorReset();
  }

  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      dt.get(), kX.data(), kY.data()));

  // 2. Point inside facet 1 (skips degenerate facet 0, matches facet 1), plus
  // a point slightly outside edge l1 = 0 but within EPS = 1e-10 tolerance.
  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetBruteForce(dt.get(), 0.25, 0.25,
                                                       &out_facet));
  EXPECT_EQ(1, out_facet);

  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetBruteForce(dt.get(), -0.5e-10, 0.5,
                                                       &out_facet));
  EXPECT_EQ(1, out_facet);

  // 3. Point inside facet 2 (x = 10.25, y = 0.25):
  //    On facet 1: l1 = 10.25 > 1 + EPS -> continues to facet 2 and matches.
  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetBruteForce(dt.get(), 10.25, 0.25,
                                                       &out_facet));
  EXPECT_EQ(2, out_facet);

  // 4. Exercise all remaining continue branches on facet 1 followed by loop
  // exhaustion (returning FALSE with out_facet == -1 because facet 2 also has
  // non-negative neighbors):
  //   (a) l1 < -EPS on facet 1 (x = -0.5, y = 0.2).
  //   (b) l2 < -EPS on facet 1 (x = 0.2, y = -0.5).
  //   (c) l2 > 1 + EPS on facet 1 (x = 0.2, y = 1.5).
  //   (d) l3 < -EPS on facet 1 (x = 0.8, y = 0.8 -> l3 = -0.6).
  //   (e) l3 > 1 + EPS while l1 >= -EPS and l2 >= -EPS on facet 1
  //       (x = -0.8e-10, y = -0.8e-10 -> l3 = 1 + 1.6e-10 > 1 + 1e-10).
  struct Point {
    double x;
    double y;
  };
  constexpr auto kMissPoints = std::to_array<Point>({
      {.x = -0.5, .y = 0.2},
      {.x = 0.2, .y = -0.5},
      {.x = 0.2, .y = 1.5},
      {.x = 0.8, .y = 0.8},
      {.x = -0.8e-10, .y = -0.8e-10},
  });
  for (const auto& pt : kMissPoints) {
    out_facet = 99;
    EXPECT_EQ(FALSE, GDALTriangulationFindFacetBruteForce(dt.get(), pt.x, pt.y,
                                                          &out_facet));
    EXPECT_EQ(-1, out_facet);
  }

  // 5. Exterior hull boundary exits (anNeighborIdx[k] == -1) on a single
  // triangle return FALSE with *panOutputFacetIdx set to the boundary facet
  // (0).
  constexpr auto kHullFacet = std::to_array<GDALTriFacet>({
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {-1, -1, -1}},
  });
  TriangulationUniquePtr hull_dt = CreateTriangulation(kHullFacet);
  ASSERT_NE(nullptr, hull_dt);
  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      hull_dt.get(), kX.data(), kY.data()));

  // l1 < -EPS with anNeighborIdx[0] == -1:
  out_facet = -1;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetBruteForce(hull_dt.get(), -0.5,
                                                        0.2, &out_facet));
  EXPECT_EQ(0, out_facet);

  // l2 < -EPS with anNeighborIdx[1] == -1:
  out_facet = -1;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetBruteForce(hull_dt.get(), 0.2,
                                                        -0.5, &out_facet));
  EXPECT_EQ(0, out_facet);

  // l3 < -EPS with anNeighborIdx[2] == -1:
  out_facet = -1;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetBruteForce(hull_dt.get(), 0.8, 0.8,
                                                        &out_facet));
  EXPECT_EQ(0, out_facet);
}

TEST(DelaunayTest, FindFacetDirectedWalkAndFallbackBranches) {
  // 4-triangle mesh around the unit right triangle (facet 0):
  //   Vertices:
  //     0: (1, 0), 1: (0, 1), 2: (0, 0)  -> Facet 0: central triangle
  //     3: (-1, 0)                       -> Facet 1: across edge v1-v2 (l1 < 0)
  //     4: (0, -1)                       -> Facet 2: across edge v0-v2 (l2 < 0)
  //     5: (1, 1)                        -> Facet 3: across edge v0-v1 (l3 < 0)
  //     6: (2, 2)                        -> Facet 4: degenerate
  //     (0,0)-(1,1)-(2,2)
  constexpr auto kX =
      std::to_array<double>({1.0, 0.0, 0.0, -1.0, 0.0, 1.0, 2.0});
  constexpr auto kY =
      std::to_array<double>({0.0, 1.0, 0.0, 0.0, -1.0, 1.0, 2.0});

  constexpr auto kFacets = std::to_array<GDALTriFacet>({
      // Facet 0: neighbors are facet 1 (opposite v0), facet 2 (opposite v1),
      // and facet 3 (opposite v2).
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {1, 2, 3}},
      // Facet 1: (-1, 0), (0, 1), (0, 0). Opposite v0=(-1,0) is facet 0.
      {.anVertexIdx = {3, 1, 2}, .anNeighborIdx = {0, -1, -1}},
      // Facet 2: (1, 0), (0, -1), (0, 0). Opposite v1=(0,-1) is facet 0.
      {.anVertexIdx = {0, 4, 2}, .anNeighborIdx = {-1, 0, -1}},
      // Facet 3: (1, 0), (0, 1), (1, 1). Opposite v2=(1,1) is facet 0.
      {.anVertexIdx = {0, 1, 5}, .anNeighborIdx = {-1, -1, 0}},
      // Facet 4: degenerate collinear triangle (0,0)-(1,1)-(2,2).
      {.anVertexIdx = {2, 5, 6}, .anNeighborIdx = {-1, -1, -1}},
  });

  TriangulationUniquePtr dt = CreateTriangulation(kFacets);
  ASSERT_NE(nullptr, dt);

  // 1. Calling before GDALTriangulationComputeBarycentricCoefficients fails.
  int out_facet = 99;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(dt.get(), 0, 0.25, 0.25,
                                                        &out_facet));
    EXPECT_EQ(-1, out_facet);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALTriangulationComputeBarycentricCoefficients() "
                          "should be called before"));
    CPLErrorReset();
  }

  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      dt.get(), kX.data(), kY.data()));

  // 2. Directed walk from facet 0 into each neighbor and inside facet 0 itself,
  // plus a 2-hop directed walk from facet 1 -> facet 0 -> facet 3:
  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetDirected(dt.get(), 0, 0.25, 0.25,
                                                     &out_facet));
  EXPECT_EQ(0, out_facet);

  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetDirected(dt.get(), 0, -0.25, 0.25,
                                                     &out_facet));
  EXPECT_EQ(1, out_facet);

  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetDirected(dt.get(), 0, 0.25, -0.25,
                                                     &out_facet));
  EXPECT_EQ(2, out_facet);

  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetDirected(dt.get(), 0, 0.6, 0.6,
                                                     &out_facet));
  EXPECT_EQ(3, out_facet);

  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetDirected(dt.get(), 1, 0.6, 0.6,
                                                     &out_facet));
  EXPECT_EQ(3, out_facet);

  // 3. Starting on degenerate facet 4 breaks out of the directed walk and
  // succeeds via the GDALTriangulationFindFacetBruteForce fallback.
  out_facet = -1;
  EXPECT_EQ(TRUE, GDALTriangulationFindFacetDirected(dt.get(), 4, 0.25, 0.25,
                                                     &out_facet));
  EXPECT_EQ(0, out_facet);

  // 4. Exterior hull boundary exits when neighbor < 0 on each edge of a
  // single-facet hull triangle:
  constexpr auto kHullFacet = std::to_array<GDALTriFacet>({
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {-1, -1, -1}},
  });
  TriangulationUniquePtr hull_dt = CreateTriangulation(kHullFacet);
  ASSERT_NE(nullptr, hull_dt);
  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      hull_dt.get(), kX.data(), kY.data()));

  // Edge 0 (l1 < -EPS, anNeighborIdx[0] == -1):
  out_facet = -1;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(hull_dt.get(), 0, -0.5,
                                                      0.2, &out_facet));
  EXPECT_EQ(0, out_facet);

  // Edge 1 (l2 < -EPS, anNeighborIdx[1] == -1):
  out_facet = -1;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(hull_dt.get(), 0, 0.2,
                                                      -0.5, &out_facet));
  EXPECT_EQ(0, out_facet);

  // Edge 2 (l3 < -EPS, anNeighborIdx[2] == -1):
  out_facet = -1;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(hull_dt.get(), 0, 0.8,
                                                      0.8, &out_facet));
  EXPECT_EQ(0, out_facet);

  // 5. Exercise the three `else if (l_i > 1 + EPS) bMatch = FALSE;` branches
  // and `if (bMatch) ... else { break; }` in
  // GDALTriangulationFindFacetDirected. On the unit right triangle (facet 0, l1
  // = x, l2 = y, l3 = 1 - x - y), choosing two coordinates at -0.8 * EPS (>=
  // -EPS) makes the third coordinate equal to 1 + 1.6 * EPS (> 1 + EPS),
  // setting bMatch = FALSE without taking any `l_i < -EPS` neighbor jump, then
  // breaking to the brute-force fallback.
  out_facet = 99;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(
                       hull_dt.get(), 0, 1.0 + 1.6e-10, -0.8e-10, &out_facet));
  EXPECT_EQ(-1, out_facet);

  out_facet = 99;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(
                       hull_dt.get(), 0, -0.8e-10, 1.0 + 1.6e-10, &out_facet));
  EXPECT_EQ(-1, out_facet);

  out_facet = 99;
  EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(
                       hull_dt.get(), 0, -0.8e-10, -0.8e-10, &out_facet));
  EXPECT_EQ(-1, out_facet);

  // 6. Exercise normal loop termination (`k == nIterMax`, where
  // `nIterMax = 2 + nFacets / 4`) when a directed walk cycles between two
  // facets before falling back to GDALTriangulationFindFacetBruteForce.
  // Repeat > 21 times so the process-wide static nDebugMsgCount cap
  // (nDebugMsgCount <= 20, including == 20 and > 20) is covered regardless of
  // test execution order under Testify.
  constexpr auto kCycleFacets = std::to_array<GDALTriFacet>({
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {1, -1, -1}},
      {.anVertexIdx = {0, 1, 2}, .anNeighborIdx = {0, -1, -1}},
      {.anVertexIdx = {1, 0, 2}, .anNeighborIdx = {-1, -1, -1}},
  });
  TriangulationUniquePtr cycle_dt = CreateTriangulation(kCycleFacets);
  ASSERT_NE(nullptr, cycle_dt);
  ASSERT_EQ(TRUE, GDALTriangulationComputeBarycentricCoefficients(
                      cycle_dt.get(), kX.data(), kY.data()));
  for (std::size_t i = 0; i < 25; ++i) {
    out_facet = -1;
    // At (-0.5, 0.2), l1 = -0.5 < -EPS on both facet 0 and facet 1, so the
    // directed walk alternates 0 -> 1 -> 0 for nIterMax = 2 iterations, exits
    // the loop, and falls back to brute force (which finds facet 2 where
    // v0=(0,1), v1=(1,0), v2=(0,0) has l2 = -0.5 < -EPS and neighbor[1] == -1).
    EXPECT_EQ(FALSE, GDALTriangulationFindFacetDirected(cycle_dt.get(), 0, -0.5,
                                                        0.2, &out_facet));
    EXPECT_EQ(2, out_facet);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - QHull-dependent body of GDALTriangulationCreateDelaunay()
// (INTERNAL_QHULL / EXTERNAL_QHULL), DEBUG_VERBOSE diagnostic blocks, and
// CPLAssert failure branches are compiled out or unreachable in google3.
