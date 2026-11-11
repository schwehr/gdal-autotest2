// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/grid/GridIntersection.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/grid/Grid.h"
#include "third_party/geos/include/geos/operation/grid/Matrix.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::GeometryTypeId;
using ::geos::io::WKTReader;
using ::geos::operation::grid::bounded_extent;
using ::geos::operation::grid::Grid;
using ::geos::operation::grid::GridIntersection;
using ::geos::operation::grid::Matrix;

class GridIntersectionTest : public ::testing::Test {
 protected:
  GridIntersectionTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWKT(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  static Grid<bounded_extent> MakeGrid(double xmin, double xmax, double ymin,
                                       double ymax, double dx, double dy) {
    return Grid<bounded_extent>(Envelope(xmin, xmax, ymin, ymax), dx, dy);
  }

  static float SumMatrix(const Matrix<float>& m) {
    float total = 0.0f;
    for (std::size_t r = 0; r < m.getNumRows(); ++r) {
      for (std::size_t c = 0; c < m.getNumCols(); ++c) {
        total += m(r, c);
      }
    }
    return total;
  }

  static void ExpectMatrixNear(const Matrix<float>& actual,
                               const std::vector<std::vector<float>>& expected,
                               float tol = 1e-5f) {
    ASSERT_EQ(actual.getNumRows(), expected.size());
    for (std::size_t r = 0; r < expected.size(); ++r) {
      ASSERT_EQ(actual.getNumCols(), expected[r].size());
      for (std::size_t c = 0; c < expected[r].size(); ++c) {
        EXPECT_NEAR(actual(r, c), expected[r][c], tol)
            << "mismatch at (" << r << ", " << c << ")";
      }
    }
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(GridIntersectionTest, ProcessingRegionCoversAllComponentCases) {
  const Envelope raster_extent(0.0, 10.0, 0.0, 10.0);

  // Empty collection (0 components) returns a null Envelope.
  auto empty_gc = ReadWKT("GEOMETRYCOLLECTION EMPTY");
  EXPECT_TRUE(
      GridIntersection::processingRegion(raster_extent, *empty_gc).isNull());

  // Empty atomic polygon returns a null Envelope.
  auto empty_poly = ReadWKT("POLYGON EMPTY");
  EXPECT_TRUE(
      GridIntersection::processingRegion(raster_extent, *empty_poly).isNull());

  // Disjoint polygon returns a null Envelope.
  auto disjoint = ReadWKT("POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))");
  EXPECT_TRUE(
      GridIntersection::processingRegion(raster_extent, *disjoint).isNull());

  // Single partially overlapping polygon returns its clipped intersection.
  auto partial = ReadWKT("POLYGON ((2 3, 15 3, 15 7, 2 7, 2 3))");
  const Envelope partial_region =
      GridIntersection::processingRegion(raster_extent, *partial);
  ASSERT_FALSE(partial_region.isNull());
  EXPECT_EQ(partial_region, Envelope(2.0, 10.0, 3.0, 7.0));

  // MultiPolygon where first component covers the entire raster_extent hits
  // the early-exit branch before inspecting subsequent components.
  auto covering = ReadWKT(
      "MULTIPOLYGON (((-1 -1, 11 -1, 11 11, -1 11, -1 -1)), "
      "((2 2, 4 2, 4 4, 2 4, 2 2)))");
  const Envelope full_region =
      GridIntersection::processingRegion(raster_extent, *covering);
  EXPECT_EQ(full_region, raster_extent);

  // MultiPolygon exercising: disjoint first component (ret remains null),
  // intersecting second component (sets ret), contained third component (does
  // not expand ret), expanding fourth component (expands ret), and disjoint
  // fifth component (does not alter ret).
  auto multi = ReadWKT(
      "MULTIPOLYGON ("
      "((20 20, 22 20, 22 22, 20 22, 20 20)),"
      "((2 3, 5 3, 5 6, 2 6, 2 3)),"
      "((3 4, 4 4, 4 5, 3 5, 3 4)),"
      "((4 1, 8 1, 8 4, 4 4, 4 1)),"
      "((-5 -5, -2 -5, -2 -2, -5 -2, -5 -5)))");
  const Envelope expanded_region =
      GridIntersection::processingRegion(raster_extent, *multi);
  ASSERT_FALSE(expanded_region.isNull());
  EXPECT_EQ(expanded_region, Envelope(2.0, 8.0, 1.0, 6.0));
}

TEST_F(GridIntersectionTest, EnvelopeFractionsAndAccumulation) {
  const auto grid = MakeGrid(0.0, 5.0, 0.0, 5.0, 1.0, 1.0);

  // Interior box spanning >= 3x3 cells hits all corners, edges, and interior.
  auto res = GridIntersection::getIntersectionFractions(
      grid, Envelope(0.5, 3.5, 0.5, 3.5));
  ExpectMatrixNear(*res, {
                             {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                             {0.25f, 0.50f, 0.50f, 0.25f, 0.00f},
                             {0.50f, 1.00f, 1.00f, 0.50f, 0.00f},
                             {0.50f, 1.00f, 1.00f, 0.50f, 0.00f},
                             {0.25f, 0.50f, 0.50f, 0.25f, 0.00f},
                         });

  // Box strictly larger than the entire grid hits all exterior-boundary false
  // branches in processRectangularRing and fills all cells with 1.0.
  auto full = GridIntersection::getIntersectionFractions(
      grid, Envelope(-1.0, 6.0, -1.0, 6.0));
  ExpectMatrixNear(
      *full, std::vector<std::vector<float>>(5, std::vector<float>(5, 1.0f)));

  // Box overlapping only top-left portion exercises partial boundary clipping.
  auto top_left = GridIntersection::getIntersectionFractions(
      grid, Envelope(-1.0, 2.5, 2.5, 6.0));
  ExpectMatrixNear(*top_left, {
                                  {1.00f, 1.00f, 0.50f, 0.00f, 0.00f},
                                  {1.00f, 1.00f, 0.50f, 0.00f, 0.00f},
                                  {0.50f, 0.50f, 0.25f, 0.00f, 0.00f},
                                  {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                                  {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                              });

  // Box contained within a single cell.
  auto single_cell = GridIntersection::getIntersectionFractions(
      grid, Envelope(1.2, 1.8, 1.2, 1.8));
  ExpectMatrixNear(*single_cell, {
                                     {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                                     {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                                     {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                                     {0.00f, 0.36f, 0.00f, 0.00f, 0.00f},
                                     {0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
                                 });

  // Disjoint box and null box produce all-zero results.
  auto disjoint = GridIntersection::getIntersectionFractions(
      grid, Envelope(10.0, 12.0, 10.0, 12.0));
  EXPECT_FLOAT_EQ(SumMatrix(*disjoint), 0.0f);

  auto null_box_res =
      GridIntersection::getIntersectionFractions(grid, Envelope());
  EXPECT_FLOAT_EQ(SumMatrix(*null_box_res), 0.0f);

  // Pre-existing coverage matrix accumulates values.
  auto cov = std::make_shared<Matrix<float>>(5, 5, 0.5f);
  GridIntersection accum(grid, Envelope(1.2, 1.8, 1.2, 1.8), cov);
  EXPECT_EQ(accum.getResults(), cov);
  EXPECT_NEAR((*cov)(3, 1), 0.86f, 1e-5f);
  EXPECT_NEAR((*cov)(0, 0), 0.50f, 1e-5f);

  // Empty raster grid produces an empty matrix without processing.
  const auto empty_grid = Grid<bounded_extent>::makeEmpty();
  GridIntersection empty_grid_isect(empty_grid, Envelope(0.0, 1.0, 0.0, 1.0));
  EXPECT_EQ(empty_grid_isect.getResults()->getNumRows(), 0u);
  EXPECT_EQ(empty_grid_isect.getResults()->getNumCols(), 0u);
}

TEST_F(GridIntersectionTest, NonUnitNonOriginGridWithEnvelopeAndPolygon) {
  // Grid with dx = 2.0, dy = 0.5, origin at (10, -4): 3 cols x 4 rows.
  const auto grid = MakeGrid(10.0, 16.0, -4.0, -2.0, 2.0, 0.5);
  const Envelope box(11.0, 15.0, -3.75, -2.25);

  auto env_res = GridIntersection::getIntersectionFractions(grid, box);
  auto poly =
      ReadWKT("POLYGON ((11 -3.75, 15 -3.75, 15 -2.25, 11 -2.25, 11 -3.75))");
  auto poly_res = GridIntersection::getIntersectionFractions(grid, *poly);

  const std::vector<std::vector<float>> expected = {
      {0.25f, 0.50f, 0.25f},
      {0.50f, 1.00f, 0.50f},
      {0.50f, 1.00f, 0.50f},
      {0.25f, 0.50f, 0.25f},
  };
  ExpectMatrixNear(*env_res, expected);
  ExpectMatrixNear(*poly_res, expected);
}

TEST_F(GridIntersectionTest, PolygonFractionsRectangularAndGeneral) {
  const auto grid = MakeGrid(0.0, 4.0, 0.0, 4.0, 1.0, 1.0);

  // 5-point axis-aligned rectangle hits processRectangularRing shortcut.
  auto rect_poly =
      ReadWKT("POLYGON ((0.5 0.5, 3.5 0.5, 3.5 3.5, 0.5 3.5, 0.5 0.5))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid, *rect_poly),
      {
          {0.25f, 0.50f, 0.50f, 0.25f},
          {0.50f, 1.00f, 1.00f, 0.50f},
          {0.50f, 1.00f, 1.00f, 0.50f},
          {0.25f, 0.50f, 0.50f, 0.25f},
      });

  // 5-point non-rectangular trapezoid skips processRectangularRing.
  auto trap =
      ReadWKT("POLYGON ((0.5 0.5, 3.5 0.5, 2.5 3.5, 0.5 3.5, 0.5 0.5))");
  auto trap_res = GridIntersection::getIntersectionFractions(grid, *trap);
  EXPECT_NEAR(SumMatrix(*trap_res), 7.5f, 1e-4f);
  EXPECT_FLOAT_EQ((*trap_res)(1, 1), 1.0f);
  EXPECT_FLOAT_EQ((*trap_res)(0, 3), 0.0f);

  // Triangle starting strictly inside cell (3, 0), tested in both CCW and CW
  // orientations to cover ring reversal and incomplete initial-cell wrapping.
  auto tri_ccw = ReadWKT("POLYGON ((0.5 0.5, 3.5 0.5, 2.0 3.5, 0.5 0.5))");
  auto tri_cw = ReadWKT("POLYGON ((0.5 0.5, 2.0 3.5, 3.5 0.5, 0.5 0.5))");
  auto ccw_res = GridIntersection::getIntersectionFractions(grid, *tri_ccw);
  auto cw_res = GridIntersection::getIntersectionFractions(grid, *tri_cw);
  EXPECT_NEAR(SumMatrix(*ccw_res), 4.5f, 1e-4f);
  EXPECT_NEAR((*ccw_res)(3, 0), 0.1875f, 1e-5f);
  EXPECT_NEAR((*ccw_res)(2, 1), 0.9375f, 1e-5f);
  EXPECT_NEAR((*ccw_res)(1, 1), 0.5000f, 1e-5f);
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      EXPECT_NEAR((*ccw_res)(r, c), (*cw_res)(r, c), 1e-5f);
    }
  }

  // Non-rectangular polygon strictly enclosing the grid fills every cell to 1.
  auto big_diamond = ReadWKT("POLYGON ((-3 2, 2 -3, 7 2, 2 7, -3 2))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid, *big_diamond),
      std::vector<std::vector<float>>(4, std::vector<float>(4, 1.0f)));

  // Diamond clipping all 4 corners symmetrically.
  auto clipped_diamond = ReadWKT("POLYGON ((-1 2, 2 -1, 5 2, 2 5, -1 2))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid, *clipped_diamond),
      {
          {0.5f, 1.0f, 1.0f, 0.5f},
          {1.0f, 1.0f, 1.0f, 1.0f},
          {1.0f, 1.0f, 1.0f, 1.0f},
          {0.5f, 1.0f, 1.0f, 0.5f},
      });

  // Grid-aligned polygon matching a subset of cells cleanly.
  auto aligned = ReadWKT("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid, *aligned),
                   {
                       {0.0f, 0.0f, 0.0f, 0.0f},
                       {0.0f, 0.0f, 0.0f, 0.0f},
                       {1.0f, 1.0f, 0.0f, 0.0f},
                       {1.0f, 1.0f, 0.0f, 0.0f},
                   });
}

TEST_F(GridIntersectionTest, ConcavePolygonReentersSameCellMultipleTimes) {
  const auto grid = MakeGrid(0.0, 3.0, 0.0, 3.0, 1.0, 1.0);
  // Both arms of the C-shape traverse cell (1, 1) inside y in [1, 2].
  auto c_shape = ReadWKT(
      "POLYGON ((0.2 1.1, 2.8 1.1, 2.8 1.3, 0.8 1.3, "
      "0.8 1.7, 2.8 1.7, 2.8 1.9, 0.2 1.9, 0.2 1.1))");
  auto res = GridIntersection::getIntersectionFractions(grid, *c_shape);
  ExpectMatrixNear(*res, {
                             {0.00f, 0.00f, 0.00f},
                             {0.56f, 0.40f, 0.32f},
                             {0.00f, 0.00f, 0.00f},
                         });
}

TEST_F(GridIntersectionTest, PolygonWithHolesAndSingleCellShortCircuit) {
  const auto grid5 = MakeGrid(0.0, 5.0, 0.0, 5.0, 1.0, 1.0);
  // Rectangular shell with a rectangular hole spanning >= 3x3 cells.
  auto poly_rect_hole = ReadWKT(
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0), "
      "(0.5 0.5, 0.5 3.5, 3.5 3.5, 3.5 0.5, 0.5 0.5))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid5, *poly_rect_hole),
      {
          {1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
          {0.75f, 0.50f, 0.50f, 0.75f, 1.00f},
          {0.50f, 0.00f, 0.00f, 0.50f, 1.00f},
          {0.50f, 0.00f, 0.00f, 0.50f, 1.00f},
          {0.75f, 0.50f, 0.50f, 0.75f, 1.00f},
      });

  // Outer shell with a non-rectangular triangular hole spanning multiple cells,
  // tested with both CW and CCW hole orientations.
  const auto grid4 = MakeGrid(0.0, 4.0, 0.0, 4.0, 1.0, 1.0);
  auto poly_tri_hole_cw = ReadWKT(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(0.5 0.5, 2.0 3.5, 3.5 0.5, 0.5 0.5))");
  auto poly_tri_hole_ccw = ReadWKT(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(0.5 0.5, 3.5 0.5, 2.0 3.5, 0.5 0.5))");
  auto hole_cw_res =
      GridIntersection::getIntersectionFractions(grid4, *poly_tri_hole_cw);
  auto hole_ccw_res =
      GridIntersection::getIntersectionFractions(grid4, *poly_tri_hole_ccw);
  EXPECT_NEAR(SumMatrix(*hole_cw_res), 11.5f, 1e-4f);
  EXPECT_NEAR(SumMatrix(*hole_ccw_res), 11.5f, 1e-4f);

  // Single-cell short circuit for exterior triangle.
  const auto grid3 = MakeGrid(0.0, 3.0, 0.0, 3.0, 1.0, 1.0);
  auto tiny_tri = ReadWKT("POLYGON ((1.2 1.2, 1.8 1.2, 1.5 1.8, 1.2 1.2))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid3, *tiny_tri),
      {
          {0.00f, 0.00f, 0.00f},
          {0.00f, 0.18f, 0.00f},
          {0.00f, 0.00f, 0.00f},
      });

  // Single-cell short circuit for a 6-coordinate rectangle (non-5 coords).
  auto tiny_6pt_rect = ReadWKT(
      "POLYGON ((1.2 1.2, 1.5 1.2, 1.8 1.2, 1.8 1.8, 1.2 1.8, 1.2 1.2))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid3, *tiny_6pt_rect),
      {
          {0.00f, 0.00f, 0.00f},
          {0.00f, 0.36f, 0.00f},
          {0.00f, 0.00f, 0.00f},
      });

  // Single-cell short circuit for interior hole subtracts from outer shell.
  auto poly_small_hole = ReadWKT(
      "POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0), "
      "(1.2 1.2, 1.5 1.8, 1.8 1.2, 1.2 1.2))");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid3, *poly_small_hole),
      {
          {1.00f, 1.00f, 1.00f},
          {1.00f, 0.82f, 1.00f},
          {1.00f, 1.00f, 1.00f},
      });

  // Polygon with multiple holes exercises the interior ring loop for > 1 hole.
  auto poly_two_holes = ReadWKT(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(1.2 1.2, 1.8 1.2, 1.5 1.8, 1.2 1.2), "
      "(2.2 2.2, 2.5 2.8, 2.8 2.2, 2.2 2.2))");
  auto two_holes_res =
      GridIntersection::getIntersectionFractions(grid4, *poly_two_holes);
  EXPECT_NEAR((*two_holes_res)(2, 1), 0.82f, 1e-5f);
  EXPECT_NEAR((*two_holes_res)(1, 2), 0.82f, 1e-5f);
  EXPECT_NEAR(SumMatrix(*two_holes_res), 15.64f, 1e-4f);
}

TEST_F(GridIntersectionTest, CornerGrazingUndeterminedCellsFloodFallback) {
  const auto grid = MakeGrid(0.0, 4.0, 0.0, 4.0, 1.0, 1.0);

  // Diagonal (0.5, 0.5) -> (2.5, 2.5) passes cleanly through grid corners
  // (1, 1) and (2, 2), leaving adjacent cells (2, 0) and (1, 1) with an
  // undetermined single-point traversal inside the polygon (INTERIOR fallback).
  auto poly_inside_grazed = ReadWKT(
      "POLYGON ((-0.5 0.5, 0.5 0.5, 2.5 2.5, 2.5 3.5, -0.5 3.5, -0.5 0.5))");
  auto res_in =
      GridIntersection::getIntersectionFractions(grid, *poly_inside_grazed);
  EXPECT_FLOAT_EQ((*res_in)(2, 0), 1.0f);
  EXPECT_FLOAT_EQ((*res_in)(1, 1), 1.0f);
  EXPECT_NEAR((*res_in)(2, 1), 0.5f, 1e-5f);
  EXPECT_NEAR((*res_in)(3, 0), 0.375f, 1e-5f);
  EXPECT_NEAR((*res_in)(1, 2), 0.375f, 1e-5f);
  EXPECT_NEAR(SumMatrix(*res_in), 5.5f, 1e-4f);

  // Same diagonal with polygon on the opposite side leaves cells (2, 0) and
  // (1, 1) with an undetermined traversal outside the polygon (EXTERIOR).
  auto poly_outside_grazed =
      ReadWKT("POLYGON ((0.5 0.5, 3.5 0.5, 3.5 2.5, 2.5 2.5, 0.5 0.5))");
  auto res_out =
      GridIntersection::getIntersectionFractions(grid, *poly_outside_grazed);
  EXPECT_FLOAT_EQ((*res_out)(2, 0), 0.0f);
  EXPECT_FLOAT_EQ((*res_out)(1, 1), 0.0f);
  EXPECT_NEAR((*res_out)(2, 1), 0.5f, 1e-5f);
  EXPECT_NEAR(SumMatrix(*res_out), 4.0f, 1e-4f);
}

TEST_F(GridIntersectionTest, LinearGeometriesAndCollections) {
  const auto grid3 = MakeGrid(0.0, 3.0, 0.0, 3.0, 1.0, 1.0);

  // Single-cell LineString short circuit.
  auto single_ls = ReadWKT("LINESTRING (1.1 1.2, 1.5 1.5)");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid3, *single_ls),
      {
          {0.0f, 0.0f, 0.0f},
          {0.0f, 0.5f, 0.0f},
          {0.0f, 0.0f, 0.0f},
      });

  // Multi-cell LineString traversing RIGHT, TOP, LEFT, BOTTOM and ending on a
  // cell boundary (triggering forceExit on boundary).
  auto multi_ls =
      ReadWKT("LINESTRING (0.5 0.5, 2.5 0.5, 2.5 2.5, 0.5 2.5, 0.5 1.0)");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid3, *multi_ls),
      {
          {1.0f, 1.0f, 1.0f},
          {1.0f, 0.0f, 1.0f},
          {0.5f, 1.0f, 1.0f},
      });

  // LineString that re-enters the same cell multiple times.
  const auto grid2 = MakeGrid(0.0, 2.0, 0.0, 2.0, 1.0, 1.0);
  auto reentry_ls = ReadWKT("LINESTRING (0.2 1.2, 1.8 1.2, 1.8 1.8, 0.2 1.8)");
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid2, *reentry_ls),
      {
          {1.6f, 2.2f},
          {0.0f, 0.0f},
      });

  // LinearRing input exercises GEOS_LINEARRING branch in process().
  auto ring =
      ReadWKT("LINEARRING (1.5 0.5, 2.5 1.5, 1.5 2.5, 0.5 1.5, 1.5 0.5)");
  const float s2 = std::sqrt(2.0f);
  ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid3, *ring),
                   {
                       {0.0f, s2, 0.0f},
                       {s2, 0.0f, s2},
                       {0.0f, s2, 0.0f},
                   });

  // MultiLineString sums traversal lengths from all components.
  auto mls =
      ReadWKT("MULTILINESTRING ((1.1 1.2, 1.5 1.5), (0.2 0.2, 0.5 0.6))");
  ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid3, *mls),
                   {
                       {0.0f, 0.0f, 0.0f},
                       {0.0f, 0.5f, 0.0f},
                       {0.5f, 0.0f, 0.0f},
                   });

  // MultiPolygon sums coverage fractions from all components.
  auto mpoly = ReadWKT(
      "MULTIPOLYGON ("
      "((1.2 1.2, 1.8 1.2, 1.5 1.8, 1.2 1.2)), "
      "((0.0 2.0, 1.0 2.0, 1.0 3.0, 0.0 3.0, 0.0 2.0)))");
  ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid3, *mpoly),
                   {
                       {1.00f, 0.00f, 0.00f},
                       {0.00f, 0.18f, 0.00f},
                       {0.00f, 0.00f, 0.00f},
                   });

  // Nested GeometryCollection of Polygons.
  auto gc_poly = ReadWKT(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "GEOMETRYCOLLECTION (POLYGON ((2 2, 3 2, 3 3, 2 3, 2 2))))");
  ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid3, *gc_poly),
                   {
                       {0.0f, 0.0f, 1.0f},
                       {0.0f, 0.0f, 0.0f},
                       {1.0f, 0.0f, 0.0f},
                   });

  // Homogeneous GeometryCollection of LineStrings.
  auto gc_line = ReadWKT(
      "GEOMETRYCOLLECTION ("
      "LINESTRING (1.1 1.2, 1.5 1.5), "
      "LINESTRING (0.2 0.2, 0.5 0.6))");
  ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid3, *gc_line),
                   {
                       {0.0f, 0.0f, 0.0f},
                       {0.0f, 0.5f, 0.0f},
                       {0.5f, 0.0f, 0.0f},
                   });

  // Accumulation constructor with Geometry and pre-existing coverage matrix.
  auto cov = std::make_shared<Matrix<float>>(3, 3, 1.0f);
  GridIntersection accum(grid3, *single_ls, cov);
  EXPECT_EQ(accum.getResults(), cov);
  EXPECT_NEAR((*cov)(1, 1), 1.5f, 1e-5f);
  EXPECT_NEAR((*cov)(0, 0), 1.0f, 1e-5f);
}

TEST_F(GridIntersectionTest, EmptyDisjointAndDegenerateGeometries) {
  const auto grid = MakeGrid(0.0, 3.0, 0.0, 3.0, 1.0, 1.0);
  const std::vector<std::vector<float>> zeros(3, std::vector<float>(3, 0.0f));

  // Empty geometries of supported types.
  for (const char* wkt : {
           "POLYGON EMPTY",
           "LINESTRING EMPTY",
           "MULTIPOLYGON EMPTY",
           "MULTILINESTRING EMPTY",
           "GEOMETRYCOLLECTION EMPTY",
       }) {
    auto g = ReadWKT(wkt);
    ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid, *g),
                     zeros);
  }

  // Disjoint geometries and boundary-touching geometries (0 intersection area).
  for (const char* wkt : {
           "POLYGON ((5 5, 6 5, 6 6, 5 6, 5 5))",
           "POLYGON ((3 0, 4 0, 4 3, 3 3, 3 0))",
           "LINESTRING (5 5, 6 6)",
           "LINESTRING (0 1.5, 3 1.5)",
       }) {
    auto g = ReadWKT(wkt);
    ExpectMatrixNear(*GridIntersection::getIntersectionFractions(grid, *g),
                     zeros);
  }

  // Degenerate 3-coordinate LinearRing in Polygon (< 4 coordinates) is skipped.
  auto seq3 = std::make_unique<CoordinateSequence>(3, false, false);
  seq3->setAt(geos::geom::CoordinateXY(0.0, 0.0), 0);
  seq3->setAt(geos::geom::CoordinateXY(2.0, 2.0), 1);
  seq3->setAt(geos::geom::CoordinateXY(0.0, 0.0), 2);
  auto degen_poly =
      factory_->createPolygon(factory_->createLinearRing(std::move(seq3)));
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid, *degen_poly), zeros);

  // Degenerate 3-coordinate hole inside a valid polygon is skipped cleanly.
  auto shell_seq = std::make_unique<CoordinateSequence>(5, false, false);
  shell_seq->setAt(geos::geom::CoordinateXY(0.0, 0.0), 0);
  shell_seq->setAt(geos::geom::CoordinateXY(3.0, 0.0), 1);
  shell_seq->setAt(geos::geom::CoordinateXY(3.0, 3.0), 2);
  shell_seq->setAt(geos::geom::CoordinateXY(0.0, 3.0), 3);
  shell_seq->setAt(geos::geom::CoordinateXY(0.0, 0.0), 4);
  auto hole_seq3 = std::make_unique<CoordinateSequence>(3, false, false);
  hole_seq3->setAt(geos::geom::CoordinateXY(1.0, 1.0), 0);
  hole_seq3->setAt(geos::geom::CoordinateXY(2.0, 2.0), 1);
  hole_seq3->setAt(geos::geom::CoordinateXY(1.0, 1.0), 2);
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(factory_->createLinearRing(std::move(hole_seq3)));
  auto poly_degen_hole = factory_->createPolygon(
      factory_->createLinearRing(std::move(shell_seq)), std::move(holes));
  ExpectMatrixNear(
      *GridIntersection::getIntersectionFractions(grid, *poly_degen_hole),
      std::vector<std::vector<float>>(3, std::vector<float>(3, 1.0f)));

  // Empty raster grid with Geometry produces empty matrix.
  const auto empty_grid = Grid<bounded_extent>::makeEmpty();
  auto poly = ReadWKT("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  GridIntersection empty_grid_geom(empty_grid, *poly);
  EXPECT_EQ(empty_grid_geom.getResults()->getNumRows(), 0u);
  EXPECT_EQ(empty_grid_geom.getResults()->getNumCols(), 0u);
}

TEST_F(GridIntersectionTest, UnsupportedAndMixedGeometryTypesThrow) {
  const auto grid = MakeGrid(0.0, 3.0, 0.0, 3.0, 1.0, 1.0);

  // Dimension 0 geometries throw std::invalid_argument in constructor.
  auto pt = ReadWKT("POINT (1 1)");
  EXPECT_THROW(GridIntersection(grid, *pt), std::invalid_argument);

  auto mpt = ReadWKT("MULTIPOINT ((1 1), (2 2))");
  EXPECT_THROW(GridIntersection(grid, *mpt), std::invalid_argument);

  // Collection with dim == 2 containing a Point throws inside process().
  auto gc_with_pt = ReadWKT(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), POINT (1 1))");
  EXPECT_THROW(GridIntersection(grid, *gc_with_pt), std::invalid_argument);

  // Curved geometry throws inside process().
  auto circ = ReadWKT("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(GridIntersection(grid, *circ), std::invalid_argument);

  // Mixed areal and linear types throw std::runtime_error in setAreal().
  auto mixed_poly_first = ReadWKT(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "LINESTRING (1.1 1.1, 1.9 1.9))");
  EXPECT_THROW(GridIntersection(grid, *mixed_poly_first), std::runtime_error);

  auto mixed_line_first = ReadWKT(
      "GEOMETRYCOLLECTION ("
      "LINESTRING (1.1 1.1, 1.9 1.9), "
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))");
  EXPECT_THROW(GridIntersection(grid, *mixed_line_first), std::runtime_error);
}

TEST_F(GridIntersectionTest, SubdividePolygonInteriorExteriorAndHoles) {
  const auto grid4 = MakeGrid(0.0, 4.0, 0.0, 4.0, 1.0, 1.0);

  // Polygon completely inside the grid with untouched interior full cells.
  auto interior_poly =
      ReadWKT("POLYGON ((0.5 0.5, 3.5 0.5, 3.5 3.5, 0.5 3.5, 0.5 0.5))");
  for (bool include_ext : {false, true}) {
    auto sub =
        GridIntersection::subdividePolygon(grid4, *interior_poly, include_ext);
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->getGeometryTypeId(),
              GeometryTypeId::GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(sub->getNumGeometries(), 16u);
    EXPECT_NEAR(sub->getArea(), 9.0, 1e-6);
    for (std::size_t i = 0; i < sub->getNumGeometries(); ++i) {
      const Geometry* part = sub->getGeometryN(i);
      EXPECT_TRUE(part->isValid());
      EXPECT_GT(part->getArea(), 0.0);
      EXPECT_TRUE(grid4.getExtent().contains(*part->getEnvelopeInternal()));
    }
  }

  // Polygon extending outside grid tests includeExterior = false vs true.
  const auto grid3 = MakeGrid(0.0, 3.0, 0.0, 3.0, 1.0, 1.0);
  auto overflow_rect =
      ReadWKT("POLYGON ((-0.5 -0.5, 3.5 -0.5, 3.5 3.5, -0.5 3.5, -0.5 -0.5))");
  auto sub_no_ext =
      GridIntersection::subdividePolygon(grid3, *overflow_rect, false);
  EXPECT_EQ(sub_no_ext->getNumGeometries(), 9u);
  EXPECT_NEAR(sub_no_ext->getArea(), 9.0, 1e-6);

  auto sub_with_ext =
      GridIntersection::subdividePolygon(grid3, *overflow_rect, true);
  EXPECT_EQ(sub_with_ext->getNumGeometries(), 10u);
  EXPECT_NEAR(sub_with_ext->getArea(), 16.0, 1e-6);
  for (std::size_t i = 0; i < sub_with_ext->getNumGeometries(); ++i) {
    EXPECT_TRUE(sub_with_ext->getGeometryN(i)->isValid());
  }

  // Polygon crossing only one exterior edge with includeExterior = true/false.
  auto left_overflow =
      ReadWKT("POLYGON ((-0.5 0.5, 2.5 0.5, 2.5 2.5, -0.5 2.5, -0.5 0.5))");
  auto left_no_ext =
      GridIntersection::subdividePolygon(grid3, *left_overflow, false);
  EXPECT_NEAR(left_no_ext->getArea(), 5.0, 1e-6);
  auto left_with_ext =
      GridIntersection::subdividePolygon(grid3, *left_overflow, true);
  EXPECT_NEAR(left_with_ext->getArea(), 6.0, 1e-6);

  // Corner-grazing polygons hit visited-cell INTERIOR and EXTERIOR branches.
  auto grazed_in = ReadWKT(
      "POLYGON ((-0.5 0.5, 0.5 0.5, 2.5 2.5, 2.5 3.5, -0.5 3.5, -0.5 0.5))");
  auto sub_grazed_in =
      GridIntersection::subdividePolygon(grid4, *grazed_in, false);
  EXPECT_NEAR(sub_grazed_in->getArea(), 5.5, 1e-6);

  auto grazed_out =
      ReadWKT("POLYGON ((0.5 0.5, 3.5 0.5, 3.5 2.5, 2.5 2.5, 0.5 0.5))");
  auto sub_grazed_out =
      GridIntersection::subdividePolygon(grid4, *grazed_out, false);
  EXPECT_NEAR(sub_grazed_out->getArea(), 4.0, 1e-6);

  // Grid-aligned boundary polygon hits traversed full-cell and zero-area cells.
  auto aligned = ReadWKT("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto sub_aligned = GridIntersection::subdividePolygon(grid3, *aligned, false);
  EXPECT_EQ(sub_aligned->getNumGeometries(), 4u);
  EXPECT_NEAR(sub_aligned->getArea(), 4.0, 1e-6);

  // Polygon with CW shell and CCW single-cell hole tests traverse_ring
  // orientation reversal for both shell and hole.
  auto cw_shell_ccw_hole = ReadWKT(
      "POLYGON ((0 0, 0 3, 3 3, 3 0, 0 0), "
      "(1.2 1.2, 1.8 1.2, 1.5 1.8, 1.2 1.2))");
  auto sub_hole =
      GridIntersection::subdividePolygon(grid3, *cw_shell_ccw_hole, false);
  EXPECT_EQ(sub_hole->getNumGeometries(), 9u);
  EXPECT_NEAR(sub_hole->getArea(), 8.82, 1e-6);

  // Polygon with CCW shell and multi-cell CW hole plus multiple holes.
  auto multi_hole_poly = ReadWKT(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(0.5 0.5, 2.0 3.5, 3.5 0.5, 0.5 0.5))");
  auto sub_multi_hole =
      GridIntersection::subdividePolygon(grid4, *multi_hole_poly, false);
  EXPECT_NEAR(sub_multi_hole->getArea(), 11.5, 1e-6);
  for (std::size_t i = 0; i < sub_multi_hole->getNumGeometries(); ++i) {
    EXPECT_TRUE(sub_multi_hole->getGeometryN(i)->isValid());
  }

  auto two_hole_poly = ReadWKT(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(1.2 1.2, 1.8 1.2, 1.5 1.8, 1.2 1.2), "
      "(2.2 2.2, 2.5 2.8, 2.8 2.2, 2.2 2.2))");
  auto sub_two_hole =
      GridIntersection::subdividePolygon(grid4, *two_hole_poly, false);
  EXPECT_NEAR(sub_two_hole->getArea(), 15.64, 1e-6);
}

TEST_F(GridIntersectionTest, SubdividePolygonCollectionsEmptyAndErrors) {
  const auto grid = MakeGrid(0.0, 4.0, 0.0, 4.0, 1.0, 1.0);

  // MultiPolygon and nested GeometryCollection of polygons.
  auto mpoly = ReadWKT(
      "MULTIPOLYGON ("
      "((0.5 0.5, 1.5 0.5, 1.5 1.5, 0.5 1.5, 0.5 0.5)), "
      "((2.5 2.5, 3.5 2.5, 3.5 3.5, 2.5 3.5, 2.5 2.5)))");
  auto sub_mp = GridIntersection::subdividePolygon(grid, *mpoly, false);
  EXPECT_NEAR(sub_mp->getArea(), 2.0, 1e-6);

  auto gc = ReadWKT(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0.5 0.5, 1.5 0.5, 1.5 1.5, 0.5 1.5, 0.5 0.5)), "
      "GEOMETRYCOLLECTION ("
      "POLYGON ((2.5 2.5, 3.5 2.5, 3.5 3.5, 2.5 3.5, 2.5 2.5))))");
  auto sub_gc = GridIntersection::subdividePolygon(grid, *gc, false);
  EXPECT_NEAR(sub_gc->getArea(), 2.0, 1e-6);

  // Empty MultiPolygon returns an empty GeometryCollection.
  auto empty_mp = ReadWKT("MULTIPOLYGON EMPTY");
  auto sub_empty = GridIntersection::subdividePolygon(grid, *empty_mp, false);
  EXPECT_EQ(sub_empty->getNumGeometries(), 0u);
  EXPECT_FLOAT_EQ(static_cast<float>(sub_empty->getArea()), 0.0f);

  // Curved geometry throws UnsupportedOperationException.
  auto circ = ReadWKT("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(GridIntersection::subdividePolygon(grid, *circ, false),
               geos::util::UnsupportedOperationException);

  // Non-polygonal linear/point geometry throws std::runtime_error.
  auto ls = ReadWKT("LINESTRING (0 0, 2 2)");
  EXPECT_THROW(GridIntersection::subdividePolygon(grid, *ls, false),
               std::runtime_error);

  auto pt = ReadWKT("POINT (1 1)");
  EXPECT_THROW(GridIntersection::subdividePolygon(grid, *pt, false),
               std::runtime_error);

  auto gc_line = ReadWKT(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "LINESTRING (1 1, 2 2))");
  EXPECT_THROW(GridIntersection::subdividePolygon(grid, *gc_line, false),
               std::runtime_error);
}

}  // namespace
}  // namespace geos_google
