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

// Unit tests for third_party/geos/src/operation/grid/FloodFill.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/grid/FloodFill.cpp

#include "third_party/geos/include/geos/operation/grid/FloodFill.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/grid/Grid.h"
#include "third_party/geos/include/geos/operation/grid/Matrix.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::grid::bounded_extent;
using geos::operation::grid::fill_values;
using geos::operation::grid::FloodFill;
using geos::operation::grid::floodFromPixel;
using geos::operation::grid::Grid;
using geos::operation::grid::Matrix;

constexpr float kExterior = fill_values<float>::EXTERIOR;
constexpr float kInterior = fill_values<float>::INTERIOR;
constexpr float kFillable = fill_values<float>::FILLABLE;
constexpr float kUnknown = fill_values<float>::UNKNOWN;

class FloodFillTest : public ::testing::Test {
 protected:
  FloodFillTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(FloodFillTest, CellIsInsidePolygonWithHoleAndBoundary) {
  // 5x5 grid covering [0, 5] x [0, 5]: row 0 has y=4.5, col 0 has x=0.5.
  Grid<bounded_extent> grid(Envelope(0.0, 5.0, 0.0, 5.0), 1.0, 1.0);
  auto poly = ReadWkt(
      "POLYGON ((0.5 0.5, 4.5 0.5, 4.5 4.5, 0.5 4.5, 0.5 0.5), "
      "(2 2, 3 2, 3 3, 2 3, 2 2))");
  FloodFill ff(*poly, grid);

  // Boundary vertex (0.5, 4.5) and boundary edge (2.5, 4.5) are not INTERIOR.
  EXPECT_FALSE(ff.cellIsInside(0, 0));
  EXPECT_FALSE(ff.cellIsInside(0, 2));

  // Strictly inside exterior ring and outside hole.
  EXPECT_TRUE(ff.cellIsInside(1, 1));
  EXPECT_TRUE(ff.cellIsInside(3, 3));

  // Center (2.5, 2.5) lies inside the hole.
  EXPECT_FALSE(ff.cellIsInside(2, 2));
}

TEST_F(FloodFillTest, CellIsInsideMultiPolygonAndEmptyGeometry) {
  Grid<bounded_extent> grid(Envelope(0.0, 5.0, 0.0, 4.0), 1.0, 1.0);
  auto multi = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((3 2, 5 2, 5 4, 3 4, 3 2)))");
  {
    FloodFill ff(*multi, grid);
    // Top-right component covers rows 0..1 (y=3.5, 2.5), cols 3..4
    // (x=3.5, 4.5).
    EXPECT_TRUE(ff.cellIsInside(0, 3));
    EXPECT_TRUE(ff.cellIsInside(1, 4));
    // Bottom-left component covers rows 2..3 (y=1.5, 0.5), cols 0..1
    // (x=0.5, 1.5).
    EXPECT_TRUE(ff.cellIsInside(2, 0));
    EXPECT_TRUE(ff.cellIsInside(3, 1));
    // Disjoint exterior cells.
    EXPECT_FALSE(ff.cellIsInside(0, 0));
    EXPECT_FALSE(ff.cellIsInside(2, 3));
  }

  auto empty_poly = ReadWkt("POLYGON EMPTY");
  FloodFill empty_ff(*empty_poly, grid);
  EXPECT_FALSE(empty_ff.cellIsInside(0, 0));
  EXPECT_FALSE(empty_ff.cellIsInside(2, 2));
}

TEST_F(FloodFillTest, FloodFillsDisconnectedInteriorAndExteriorRegions) {
  Grid<bounded_extent> grid(Envelope(0.0, 5.0, 0.0, 5.0), 1.0, 1.0);
  auto poly =
      ReadWkt("POLYGON ((1.2 1.2, 3.8 1.2, 3.8 3.8, 1.2 3.8, 1.2 1.2))");
  FloodFill ff(*poly, grid);

  constexpr float kB = 0.5f;  // Partial boundary fraction barrier.
  Matrix<float> arr({
      {kFillable, kFillable, kFillable, kFillable, kFillable},
      {kFillable, kB, kB, kB, kFillable},
      {kFillable, kB, kFillable, kB, kFillable},
      {kFillable, kB, kB, kB, kFillable},
      {kFillable, kFillable, kFillable, kFillable, kFillable},
  });

  ff.flood(arr);

  const Matrix<float> expected({
      {kExterior, kExterior, kExterior, kExterior, kExterior},
      {kExterior, kB, kB, kB, kExterior},
      {kExterior, kB, kInterior, kB, kExterior},
      {kExterior, kB, kB, kB, kExterior},
      {kExterior, kExterior, kExterior, kExterior, kExterior},
  });
  EXPECT_EQ(arr, expected);
}

TEST_F(FloodFillTest, FloodPreservesPreFilledAndHandlesEdgesAndErrors) {
  Grid<bounded_extent> grid(Envelope(0.0, 2.0, 0.0, 2.0), 1.0, 1.0);
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  FloodFill ff(*poly, grid);

  // Pre-filled values (EXTERIOR, INTERIOR, partial) are untouched.
  {
    Matrix<float> arr({
        {kExterior, 0.25f},
        {kInterior, kFillable},
    });
    ff.flood(arr);
    const Matrix<float> expected({
        {kExterior, 0.25f},
        {kInterior, kInterior},
    });
    EXPECT_EQ(arr, expected);
  }

  // UNKNOWN cell causes std::runtime_error.
  {
    Matrix<float> arr({
        {kExterior, kUnknown},
        {kFillable, kFillable},
    });
    EXPECT_THROW(ff.flood(arr), std::runtime_error);
  }

  // 0x0 empty matrix and 1x1 matrix succeed without index underflow.
  {
    Matrix<float> empty_mat(0, 0);
    ff.flood(empty_mat);
    EXPECT_EQ(empty_mat.getNumRows(), 0u);
    EXPECT_EQ(empty_mat.getNumCols(), 0u);

    Grid<bounded_extent> unit_grid(Envelope(0.0, 1.0, 0.0, 1.0), 1.0, 1.0);
    FloodFill unit_ff(*poly, unit_grid);
    Matrix<float> single(1, 1, kFillable);
    unit_ff.flood(single);
    EXPECT_FLOAT_EQ(single(0, 0), kInterior);
  }
}

TEST_F(FloodFillTest, FloodFromPixelCoversAllDirectionsAndDuplicates) {
  constexpr float kB = 0.5f;
  // Concave U-path exercises left, right, up, and down scanline expansion,
  // while the 2x2 block exercises skipping already-filled queued cells.
  Matrix<float> arr({
      {kFillable, kB, kFillable, kFillable},
      {kFillable, kB, kB, kFillable},
      {kFillable, kFillable, kFillable, kFillable},
      {kFillable, kFillable, kB, kB},
  });

  floodFromPixel(arr, 0, 3, kInterior);

  const Matrix<float> expected({
      {kInterior, kB, kInterior, kInterior},
      {kInterior, kB, kB, kInterior},
      {kInterior, kInterior, kInterior, kInterior},
      {kInterior, kInterior, kB, kB},
  });
  EXPECT_EQ(arr, expected);
}

}  // namespace
}  // namespace geos_google
