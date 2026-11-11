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

// Unit tests for third_party/geos/src/operation/grid/Grid.cpp and
// third_party/geos/include/geos/operation/grid/Grid.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for std::out_of_range /
//   std::range_error / std::runtime_error assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/grid/Grid.cpp
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/grid/Grid.h

#include "third_party/geos/include/geos/operation/grid/Grid.h"

#include <array>
#include <cstddef>
#include <stdexcept>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::operation::grid::bounded_extent;
using geos::operation::grid::Grid;
using geos::operation::grid::infinite_extent;
using geos::operation::grid::make_finite;
using geos::operation::grid::make_infinite;

static_assert(bounded_extent::padding == 0);
static_assert(infinite_extent::padding == 1);
static_assert(::DEFAULT_GRID_COMPAT_TOL == 1e-6);

void ExpectEnvelopeDoubleEq(const Envelope& actual, double minx, double maxx,
                            double miny, double maxy) {
  ASSERT_FALSE(actual.isNull());
  EXPECT_DOUBLE_EQ(actual.getMinX(), minx);
  EXPECT_DOUBLE_EQ(actual.getMaxX(), maxx);
  EXPECT_DOUBLE_EQ(actual.getMinY(), miny);
  EXPECT_DOUBLE_EQ(actual.getMaxY(), maxy);
}

TEST(GridTest, BoundedConstructionAndAccessors) {
  const Envelope extent(10.0, 22.0, 20.0, 35.0);
  const Grid<bounded_extent> grid(extent, 3.0, 5.0);

  EXPECT_FALSE(grid.isEmpty());
  EXPECT_EQ(grid.getNumCols(), 4);
  EXPECT_EQ(grid.getNumRows(), 3);
  EXPECT_EQ(grid.getSize(), 12);
  EXPECT_DOUBLE_EQ(grid.xmin(), 10.0);
  EXPECT_DOUBLE_EQ(grid.xmax(), 22.0);
  EXPECT_DOUBLE_EQ(grid.ymin(), 20.0);
  EXPECT_DOUBLE_EQ(grid.ymax(), 35.0);
  EXPECT_DOUBLE_EQ(grid.dx(), 3.0);
  EXPECT_DOUBLE_EQ(grid.dy(), 5.0);
  EXPECT_EQ(grid.getExtent(), extent);

  // Dimensions round to the nearest integer cell count.
  const Grid<bounded_extent> rounded(Envelope(0.0, 10.2, 0.0, 9.6), 5.0, 5.0);
  EXPECT_EQ(rounded.getNumCols(), 2);
  EXPECT_EQ(rounded.getNumRows(), 2);
}

TEST(GridTest, InfiniteConstructionAndAccessors) {
  const Envelope extent(10.0, 22.0, 20.0, 35.0);
  const Envelope domain(0.0, 30.0, 10.0, 40.0);
  const Grid<infinite_extent> grid(extent, 3.0, 5.0, domain);

  EXPECT_FALSE(grid.isEmpty());
  EXPECT_EQ(grid.getNumCols(), 6);
  EXPECT_EQ(grid.getNumRows(), 5);
  EXPECT_EQ(grid.getSize(), 30);
  EXPECT_DOUBLE_EQ(grid.xmin(), 10.0);
  EXPECT_DOUBLE_EQ(grid.xmax(), 22.0);
  EXPECT_DOUBLE_EQ(grid.ymin(), 20.0);
  EXPECT_DOUBLE_EQ(grid.ymax(), 35.0);
  EXPECT_DOUBLE_EQ(grid.dx(), 3.0);
  EXPECT_DOUBLE_EQ(grid.dy(), 5.0);
  EXPECT_EQ(grid.getExtent(), extent);
}

TEST(GridTest, EmptyAndDegenerateGrids) {
  const Grid<bounded_extent> empty_bounded = Grid<bounded_extent>::makeEmpty();
  EXPECT_TRUE(empty_bounded.isEmpty());
  EXPECT_EQ(empty_bounded.getNumRows(), 0);
  EXPECT_EQ(empty_bounded.getNumCols(), 0);
  EXPECT_EQ(empty_bounded.getSize(), 0);
  EXPECT_DOUBLE_EQ(empty_bounded.xmin(), 0.0);
  EXPECT_DOUBLE_EQ(empty_bounded.xmax(), 0.0);
  EXPECT_DOUBLE_EQ(empty_bounded.ymin(), 0.0);
  EXPECT_DOUBLE_EQ(empty_bounded.ymax(), 0.0);
  EXPECT_DOUBLE_EQ(empty_bounded.dx(), 0.0);
  EXPECT_DOUBLE_EQ(empty_bounded.dy(), 0.0);

  const Grid<infinite_extent> empty_infinite =
      Grid<infinite_extent>::makeEmpty();
  EXPECT_TRUE(empty_infinite.isEmpty());
  EXPECT_EQ(empty_infinite.getNumRows(), 2);
  EXPECT_EQ(empty_infinite.getNumCols(), 2);
  EXPECT_EQ(empty_infinite.getSize(), 4);
  EXPECT_DOUBLE_EQ(empty_infinite.xmin(), 0.0);
  EXPECT_DOUBLE_EQ(empty_infinite.xmax(), 0.0);
  EXPECT_DOUBLE_EQ(empty_infinite.ymin(), 0.0);
  EXPECT_DOUBLE_EQ(empty_infinite.ymax(), 0.0);
  EXPECT_DOUBLE_EQ(empty_infinite.dx(), 0.0);
  EXPECT_DOUBLE_EQ(empty_infinite.dy(), 0.0);

  // Point extents yield 0 interior cells and report isEmpty() == true.
  const Grid<bounded_extent> bounded_point(Envelope(5.0, 5.0, 5.0, 5.0), 1.0,
                                           1.0);
  EXPECT_TRUE(bounded_point.isEmpty());
  EXPECT_EQ(bounded_point.getNumRows(), 0);
  EXPECT_EQ(bounded_point.getNumCols(), 0);

  const Grid<infinite_extent> infinite_point(
      Envelope(5.0, 5.0, 5.0, 5.0), 1.0, 1.0, Envelope(0.0, 10.0, 0.0, 10.0));
  EXPECT_TRUE(infinite_point.isEmpty());
  EXPECT_EQ(infinite_point.getNumRows(), 2);
  EXPECT_EQ(infinite_point.getNumCols(), 2);

  // Zero-width or zero-height lines have one zero dimension; isEmpty() requires
  // both row and col counts <= 2 * padding.
  const Grid<bounded_extent> zero_width(Envelope(5.0, 5.0, 0.0, 10.0), 5.0,
                                        5.0);
  EXPECT_EQ(zero_width.getNumCols(), 0);
  EXPECT_EQ(zero_width.getNumRows(), 2);
  EXPECT_EQ(zero_width.getSize(), 0);
  EXPECT_FALSE(zero_width.isEmpty());

  const Grid<bounded_extent> zero_height(Envelope(0.0, 10.0, 5.0, 5.0), 5.0,
                                         5.0);
  EXPECT_EQ(zero_height.getNumCols(), 2);
  EXPECT_EQ(zero_height.getNumRows(), 0);
  EXPECT_EQ(zero_height.getSize(), 0);
  EXPECT_FALSE(zero_height.isEmpty());
}

TEST(GridTest, BoundedGetColumnRowAndCell) {
  const Grid<bounded_extent> grid(Envelope(0.0, 12.0, 10.0, 25.0), 4.0, 5.0);

  struct ColCase {
    double x;
    std::size_t col;
  };
  constexpr auto kColCases = std::to_array<ColCase>({
      {.x = 0.0, .col = 0},
      {.x = 3.9, .col = 0},
      {.x = 4.0, .col = 1},
      {.x = 7.9, .col = 1},
      {.x = 8.0, .col = 2},
      {.x = 12.0, .col = 2},
  });
  for (const auto& c : kColCases) {
    SCOPED_TRACE(c.x);
    EXPECT_EQ(grid.getColumn(c.x), c.col);
  }

  struct RowCase {
    double y;
    std::size_t row;
  };
  constexpr auto kRowCases = std::to_array<RowCase>({
      {.y = 25.0, .row = 0},
      {.y = 20.1, .row = 0},
      {.y = 20.0, .row = 1},
      {.y = 15.1, .row = 1},
      {.y = 15.0, .row = 2},
      {.y = 10.0, .row = 2},
  });
  for (const auto& r : kRowCases) {
    SCOPED_TRACE(r.y);
    EXPECT_EQ(grid.getRow(r.y), r.row);
  }

  EXPECT_EQ(grid.getCell(0.0, 25.0), 0);
  EXPECT_EQ(grid.getCell(5.0, 17.5), 4);
  EXPECT_EQ(grid.getCell(12.0, 10.0), 8);
}

TEST(GridTest, BoundedGetColumnRowAndCellOutOfRangeThrows) {
  const Grid<bounded_extent> grid(Envelope(0.0, 12.0, 10.0, 25.0), 4.0, 5.0);

  EXPECT_THROW(grid.getColumn(-0.1), std::out_of_range);
  EXPECT_THROW(grid.getColumn(12.1), std::out_of_range);
  EXPECT_THROW(grid.getRow(9.9), std::out_of_range);
  EXPECT_THROW(grid.getRow(25.1), std::out_of_range);
  EXPECT_THROW(grid.getCell(-0.1, 17.5), std::out_of_range);
  EXPECT_THROW(grid.getCell(6.0, 25.1), std::out_of_range);
}

TEST(GridTest, InfiniteGetColumnRowAndCell) {
  const Grid<infinite_extent> grid(Envelope(0.0, 12.0, 10.0, 25.0), 4.0, 5.0,
                                   Envelope(-10.0, 30.0, 0.0, 40.0));

  struct ColCase {
    double x;
    std::size_t col;
  };
  constexpr auto kColCases = std::to_array<ColCase>({
      {.x = -5.0, .col = 0},
      {.x = 0.0, .col = 1},
      {.x = 3.9, .col = 1},
      {.x = 4.0, .col = 2},
      {.x = 8.0, .col = 3},
      {.x = 12.0, .col = 3},
      {.x = 12.1, .col = 4},
  });
  for (const auto& c : kColCases) {
    SCOPED_TRACE(c.x);
    EXPECT_EQ(grid.getColumn(c.x), c.col);
  }

  struct RowCase {
    double y;
    std::size_t row;
  };
  constexpr auto kRowCases = std::to_array<RowCase>({
      {.y = 26.0, .row = 0},
      {.y = 25.0, .row = 1},
      {.y = 20.1, .row = 1},
      {.y = 20.0, .row = 2},
      {.y = 15.0, .row = 3},
      {.y = 10.0, .row = 3},
      {.y = 9.9, .row = 4},
  });
  for (const auto& r : kRowCases) {
    SCOPED_TRACE(r.y);
    EXPECT_EQ(grid.getRow(r.y), r.row);
  }

  EXPECT_EQ(grid.getCell(-1.0, 26.0), 0);
  EXPECT_EQ(grid.getCell(5.0, 17.5), 12);
  EXPECT_EQ(grid.getCell(13.0, 9.0), 24);
}

TEST(GridTest, GetColumnAndRowClampNearBoundaryWhenExtentExceedsMultiple) {
  const Envelope extent(0.0, 10.1, -0.1, 10.0);
  const Grid<bounded_extent> bounded(extent, 5.0, 5.0);
  ASSERT_EQ(bounded.getNumCols(), 2);
  ASSERT_EQ(bounded.getNumRows(), 2);
  EXPECT_EQ(bounded.getColumn(10.05), 1);
  EXPECT_EQ(bounded.getRow(-0.05), 1);

  const Grid<infinite_extent> infinite =
      make_infinite(bounded, Envelope(-1.0, 12.0, -1.0, 12.0));
  ASSERT_EQ(infinite.getNumCols(), 4);
  ASSERT_EQ(infinite.getNumRows(), 4);
  EXPECT_EQ(infinite.getColumn(10.05), 2);
  EXPECT_EQ(infinite.getRow(-0.05), 2);
}

TEST(GridTest, CellCentersAndOffsets) {
  const Grid<bounded_extent> bounded(Envelope(10.0, 22.0, 20.0, 35.0), 4.0,
                                     5.0);
  EXPECT_DOUBLE_EQ(bounded.getColX(0), 12.0);
  EXPECT_DOUBLE_EQ(bounded.getColX(1), 16.0);
  EXPECT_DOUBLE_EQ(bounded.getColX(2), 20.0);
  EXPECT_DOUBLE_EQ(bounded.getRowY(0), 32.5);
  EXPECT_DOUBLE_EQ(bounded.getRowY(1), 27.5);
  EXPECT_DOUBLE_EQ(bounded.getRowY(2), 22.5);

  const Grid<bounded_extent> bounded_shifted(Envelope(18.0, 22.0, 20.0, 25.0),
                                             4.0, 5.0);
  EXPECT_EQ(bounded.getColOffset(bounded), 0);
  EXPECT_EQ(bounded.getRowOffset(bounded), 0);
  EXPECT_EQ(bounded.getColOffset(bounded_shifted), 2);
  EXPECT_EQ(bounded.getRowOffset(bounded_shifted), 2);
  EXPECT_EQ(bounded_shifted.getColOffset(bounded), 2);
  EXPECT_EQ(bounded_shifted.getRowOffset(bounded), 2);

  const Grid<infinite_extent> infinite =
      make_infinite(bounded, Envelope(0.0, 30.0, 10.0, 40.0));
  EXPECT_DOUBLE_EQ(infinite.getColX(1), 12.0);
  EXPECT_DOUBLE_EQ(infinite.getColX(2), 16.0);
  EXPECT_DOUBLE_EQ(infinite.getColX(3), 20.0);
  EXPECT_DOUBLE_EQ(infinite.getRowY(1), 32.5);
  EXPECT_DOUBLE_EQ(infinite.getRowY(2), 27.5);
  EXPECT_DOUBLE_EQ(infinite.getRowY(3), 22.5);

  const Grid<infinite_extent> infinite_shifted =
      make_infinite(bounded_shifted, Envelope(0.0, 30.0, 10.0, 40.0));
  EXPECT_EQ(infinite.getColOffset(infinite_shifted), 2);
  EXPECT_EQ(infinite.getRowOffset(infinite_shifted), 2);
}

TEST(GridTest, EqualityAndInequality) {
  const Envelope extent(0.0, 10.0, 0.0, 10.0);
  const Grid<bounded_extent> a(extent, 2.0, 5.0);
  const Grid<bounded_extent> b(extent, 2.0, 5.0);
  const Grid<bounded_extent> diff_extent(Envelope(0.0, 12.0, 0.0, 10.0), 2.0,
                                         5.0);
  const Grid<bounded_extent> diff_dx(extent, 2.5, 5.0);
  const Grid<bounded_extent> diff_dy(extent, 2.0, 2.5);

  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
  EXPECT_FALSE(a == diff_extent);
  EXPECT_TRUE(a != diff_extent);
  EXPECT_FALSE(a == diff_dx);
  EXPECT_TRUE(a != diff_dx);
  EXPECT_FALSE(a == diff_dy);
  EXPECT_TRUE(a != diff_dy);

  EXPECT_EQ(Grid<bounded_extent>::makeEmpty(),
            Grid<bounded_extent>::makeEmpty());

  // operator== on Grid<infinite_extent> compares extent, dx, and dy only.
  const Grid<infinite_extent> inf_a(extent, 2.0, 5.0,
                                    Envelope(-1.0, 11.0, -1.0, 11.0));
  const Grid<infinite_extent> inf_b(extent, 2.0, 5.0,
                                    Envelope(-50.0, 50.0, -50.0, 50.0));
  const Grid<infinite_extent> inf_diff(extent, 5.0, 5.0,
                                       Envelope(-1.0, 11.0, -1.0, 11.0));
  EXPECT_TRUE(inf_a == inf_b);
  EXPECT_FALSE(inf_a != inf_b);
  EXPECT_FALSE(inf_a == inf_diff);
  EXPECT_TRUE(inf_a != inf_diff);
}

TEST(GridTest, MakeInfiniteAndMakeFiniteRoundTrip) {
  const Envelope extent(0.0, 12.0, 4.0, 16.0);
  const Envelope domain(-15.0, 25.0, -10.0, 30.0);
  const Grid<bounded_extent> bounded(extent, 4.0, 6.0);

  const Grid<infinite_extent> infinite = make_infinite(bounded, domain);
  EXPECT_FALSE(infinite.isEmpty());
  EXPECT_EQ(infinite.getExtent(), extent);
  EXPECT_DOUBLE_EQ(infinite.dx(), 4.0);
  EXPECT_DOUBLE_EQ(infinite.dy(), 6.0);
  EXPECT_EQ(infinite.getNumCols(), bounded.getNumCols() + 2);
  EXPECT_EQ(infinite.getNumRows(), bounded.getNumRows() + 2);

  // Verify the domain envelope passed to make_infinite governs outer cells.
  ExpectEnvelopeDoubleEq(infinite.getCellEnvelope(0, 0), -15.0, 0.0, 16.0,
                         30.0);
  ExpectEnvelopeDoubleEq(infinite.getCellEnvelope(infinite.getNumRows() - 1,
                                                  infinite.getNumCols() - 1),
                         12.0, 25.0, -10.0, 4.0);

  const Grid<bounded_extent> back_to_bounded = make_finite(infinite);
  EXPECT_EQ(back_to_bounded, bounded);
  EXPECT_EQ(back_to_bounded.getNumCols(), 3);
  EXPECT_EQ(back_to_bounded.getNumRows(), 2);

  const Grid<infinite_extent> empty_inf =
      make_infinite(Grid<bounded_extent>::makeEmpty(), Envelope());
  EXPECT_TRUE(empty_inf.isEmpty());
  EXPECT_EQ(empty_inf, Grid<infinite_extent>::makeEmpty());
  EXPECT_EQ(make_finite(empty_inf), Grid<bounded_extent>::makeEmpty());
}

TEST(GridTest, BoundedGetCellEnvelope) {
  // Note: Grid<bounded_extent>::getCellEnvelope forwards
  // {cell_xmin, cell_ymin, cell_xmax, cell_ymax} to Envelope(x1, x2, y1, y2),
  // which sorts (x1, x2) into [minx, maxx] and (y1, y2) into [miny, maxy].
  const Grid<bounded_extent> grid(Envelope(0.0, 15.2, -0.3, 15.0), 5.0, 5.0);
  ASSERT_EQ(grid.getNumRows(), 3);
  ASSERT_EQ(grid.getNumCols(), 3);

  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(0, 0), 0.0, 10.0, 5.0, 15.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(0, 2), 10.0, 10.0, 15.0, 15.2);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(2, 0), -0.3, 0.0, 5.0, 5.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(2, 2), -0.3, 10.0, 5.0, 15.2);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(1, 1), 5.0, 5.0, 10.0, 10.0);

  const Grid<bounded_extent> single(Envelope(0.0, 4.0, 0.0, 4.0), 4.0, 4.0);
  ASSERT_EQ(single.getNumRows(), 1);
  ASSERT_EQ(single.getNumCols(), 1);
  ExpectEnvelopeDoubleEq(single.getCellEnvelope(0, 0), 0.0, 0.0, 4.0, 4.0);
}

TEST(GridTest, InfiniteGetCellEnvelopeDomainWiderThanPadding) {
  const Envelope extent(0.0, 15.2, -0.3, 15.0);
  const Envelope domain(-10.0, 25.0, -12.0, 30.0);
  const Grid<infinite_extent> grid(extent, 5.0, 5.0, domain);
  ASSERT_EQ(grid.getNumRows(), 5);
  ASSERT_EQ(grid.getNumCols(), 5);

  constexpr std::array<double, 6> kColBounds = {-10.0, 0.0,  5.0,
                                                10.0,  15.2, 25.0};
  constexpr std::array<double, 6> kRowBounds = {30.0, 15.0, 10.0,
                                                5.0,  -0.3, -12.0};

  for (std::size_t row = 0; row < grid.getNumRows(); ++row) {
    for (std::size_t col = 0; col < grid.getNumCols(); ++col) {
      SCOPED_TRACE(row);
      SCOPED_TRACE(col);
      ExpectEnvelopeDoubleEq(grid.getCellEnvelope(row, col), kColBounds[col],
                             kColBounds[col + 1], kRowBounds[row + 1],
                             kRowBounds[row]);
    }
  }
}

TEST(GridTest, InfiniteGetCellEnvelopePaddingWiderThanDomainAndSingleCell) {
  // Domain lies strictly inside extent, so PADDED_CELL_SIZE (1.0) controls
  // the outer padding boundaries on all four sides.
  const Grid<infinite_extent> grid(Envelope(0.0, 10.0, 0.0, 10.0), 5.0, 5.0,
                                   Envelope(2.0, 8.0, 2.0, 8.0));
  ASSERT_EQ(grid.getNumRows(), 4);
  ASSERT_EQ(grid.getNumCols(), 4);

  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(0, 0), -1.0, 0.0, 10.0, 11.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(0, 3), 10.0, 11.0, 10.0, 11.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(3, 0), -1.0, 0.0, -1.0, 0.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(3, 3), 10.0, 11.0, -1.0, 0.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(1, 2), 5.0, 10.0, 5.0, 10.0);
  ExpectEnvelopeDoubleEq(grid.getCellEnvelope(2, 1), 0.0, 5.0, 0.0, 5.0);

  // Single interior cell (3x3 total grid).
  const Grid<infinite_extent> single(Envelope(0.0, 5.0, 0.0, 5.0), 5.0, 5.0,
                                     Envelope(-2.0, 7.0, -2.0, 7.0));
  ASSERT_EQ(single.getNumRows(), 3);
  ASSERT_EQ(single.getNumCols(), 3);
  ExpectEnvelopeDoubleEq(single.getCellEnvelope(1, 1), 0.0, 5.0, 0.0, 5.0);
  ExpectEnvelopeDoubleEq(single.getCellEnvelope(0, 0), -2.0, 0.0, 5.0, 7.0);
  ExpectEnvelopeDoubleEq(single.getCellEnvelope(2, 2), 5.0, 7.0, -2.0, 0.0);
}

TEST(GridTest, ShrinkToFitBoundedAndInfinite) {
  const Grid<bounded_extent> bounded(Envelope(0.0, 10.0, 0.0, 10.0), 2.0, 2.0);
  const Grid<bounded_extent> shrunk_bounded =
      bounded.shrinkToFit(Envelope(2.5, 7.5, 2.5, 7.5));
  EXPECT_EQ(shrunk_bounded.getExtent(), Envelope(2.0, 8.0, 2.0, 8.0));
  EXPECT_EQ(shrunk_bounded.getNumCols(), 3);
  EXPECT_EQ(shrunk_bounded.getNumRows(), 3);
  EXPECT_EQ(bounded.getColOffset(shrunk_bounded), 1);
  EXPECT_EQ(bounded.getRowOffset(shrunk_bounded), 1);

  const Envelope domain(-20.0, 20.0, -20.0, 20.0);
  const Grid<infinite_extent> infinite = make_infinite(bounded, domain);
  const Grid<infinite_extent> shrunk_infinite =
      infinite.shrinkToFit(Envelope(2.5, 7.5, 2.5, 7.5));
  EXPECT_EQ(shrunk_infinite.getExtent(), Envelope(2.0, 8.0, 2.0, 8.0));
  EXPECT_EQ(shrunk_infinite.getNumCols(), 5);
  EXPECT_EQ(shrunk_infinite.getNumRows(), 5);
  EXPECT_EQ(infinite.getColOffset(shrunk_infinite), 1);
  EXPECT_EQ(infinite.getRowOffset(shrunk_infinite), 1);
  // Preserves m_domain on shrunk infinite grid.
  ExpectEnvelopeDoubleEq(shrunk_infinite.getCellEnvelope(0, 0), -20.0, 2.0, 8.0,
                         20.0);
}

TEST(GridTest, ShrinkToFitBoundaryAlignedCases) {
  const Grid<bounded_extent> grid(Envelope(0.0, 10.0, 0.0, 10.0), 1.0, 1.0);

  // When num_cols > 2 / num_rows > 2 and xmax / ymin lie on a cell boundary,
  // shrinkToFit omits the extra boundary cell.
  const Grid<bounded_extent> aligned_3x3 =
      grid.shrinkToFit(Envelope(2.0, 5.0, 3.0, 6.0));
  EXPECT_EQ(aligned_3x3.getExtent(), Envelope(2.0, 5.0, 3.0, 6.0));
  EXPECT_EQ(aligned_3x3.getNumCols(), 3);
  EXPECT_EQ(aligned_3x3.getNumRows(), 3);

  // Single cell fit.
  const Grid<bounded_extent> single =
      grid.shrinkToFit(Envelope(2.0, 2.5, 3.5, 4.0));
  EXPECT_EQ(single.getExtent(), Envelope(2.0, 3.0, 3.0, 4.0));
  EXPECT_EQ(single.getNumCols(), 1);
  EXPECT_EQ(single.getNumRows(), 1);

  // Exact 2x2 boundary (num_cols == 2 and num_rows == 2, not > 2) keeps 2x2.
  const Grid<bounded_extent> aligned_2x2 =
      grid.shrinkToFit(Envelope(2.0, 3.0, 3.0, 4.0));
  EXPECT_EQ(aligned_2x2.getNumCols(), 2);
  EXPECT_EQ(aligned_2x2.getNumRows(), 2);

  const Grid<infinite_extent> inf_grid =
      make_infinite(grid, Envelope(-5.0, 15.0, -5.0, 15.0));
  const Grid<infinite_extent> inf_aligned =
      inf_grid.shrinkToFit(Envelope(2.0, 5.0, 3.0, 6.0));
  EXPECT_EQ(inf_aligned.getExtent(), Envelope(2.0, 5.0, 3.0, 6.0));
  EXPECT_EQ(inf_aligned.getNumCols(), 5);
  EXPECT_EQ(inf_aligned.getNumRows(), 5);
}

TEST(GridTest, ShrinkToFitEmptyAndDegenerateReturnsEmpty) {
  const Grid<bounded_extent> bounded(Envelope(0.0, 10.0, 0.0, 10.0), 2.0, 2.0);
  const Grid<infinite_extent> infinite =
      make_infinite(bounded, Envelope(-5.0, 15.0, -5.0, 15.0));

  const auto zero_area_boxes = std::to_array<Envelope>({
      Envelope(),
      Envelope(2.0, 2.0, 2.0, 2.0),
      Envelope(1.0, 5.0, 2.0, 2.0),
      Envelope(2.0, 2.0, 1.0, 5.0),
  });
  for (const auto& box : zero_area_boxes) {
    EXPECT_TRUE(bounded.shrinkToFit(box).isEmpty());
    EXPECT_TRUE(infinite.shrinkToFit(box).isEmpty());
  }
}

TEST(GridTest, ShrinkToFitOutOfBoundsThrows) {
  const Grid<bounded_extent> bounded(Envelope(0.0, 10.0, 0.0, 10.0), 2.0, 2.0);
  const Grid<infinite_extent> infinite =
      make_infinite(bounded, Envelope(-5.0, 15.0, -5.0, 15.0));

  const auto out_of_bounds_boxes = std::to_array<Envelope>({
      Envelope(-0.1, 5.0, 1.0, 5.0),
      Envelope(1.0, 10.1, 1.0, 5.0),
      Envelope(1.0, 5.0, -0.1, 5.0),
      Envelope(1.0, 5.0, 1.0, 10.1),
  });
  for (const auto& box : out_of_bounds_boxes) {
    EXPECT_THROW(bounded.shrinkToFit(box), std::range_error);
    EXPECT_THROW(infinite.shrinkToFit(box), std::range_error);
  }
}

TEST(GridTest, ShrinkToFitHandlesFloatRoundoffSnapping) {
  const Grid<bounded_extent> grid(Envelope(0.0, 1e-4, -1e-4, 0.0), 1e-5, 1e-5);
  // 3 * 1e-5 > 3e-5 and -3 * 1e-5 < -3e-5 in IEEE-754 double, triggering the
  // snapped_xmin / snapped_ymax rollback guards.
  const Envelope target(3e-5, 8e-5, -8e-5, -3e-5);
  const Grid<bounded_extent> shrunk = grid.shrinkToFit(target);

  EXPECT_TRUE(shrunk.getExtent().covers(target));
  EXPECT_GE(shrunk.xmin(), grid.xmin());
  EXPECT_LE(shrunk.xmax(), grid.xmax());
  EXPECT_GE(shrunk.ymin(), grid.ymin());
  EXPECT_LE(shrunk.ymax(), grid.ymax());
  EXPECT_GT(shrunk.getNumCols(), 0);
  EXPECT_GT(shrunk.getNumRows(), 0);
}

TEST(GridTest, ShrinkToFitClampsOrThrowsOnFudgeFailure) {
  // Width and height are 9.9 -> 10 cells of size 1.0, so reduced_box slightly
  // overshoots xmax / ymin while keeping the rounded cell count equal.
  const Grid<bounded_extent> clamp_grid(Envelope(0.0, 9.9, 0.1, 10.0), 1.0,
                                        1.0);
  const Envelope clamp_target(1.5, 9.8, 0.2, 8.5);
  const Grid<bounded_extent> clamped = clamp_grid.shrinkToFit(clamp_target);
  EXPECT_DOUBLE_EQ(clamped.xmax(), 9.9);
  EXPECT_DOUBLE_EQ(clamped.ymin(), 0.1);
  EXPECT_TRUE(clamped.getExtent().covers(clamp_target));

  // When rounded width/height of the clamped extent diverges from the computed
  // reduced box cell count, shrinkToFit throws std::runtime_error.
  const Grid<bounded_extent> fail_x(Envelope(0.0, 0.25, 0.0, 1.0), 0.1, 0.1);
  EXPECT_THROW(fail_x.shrinkToFit(Envelope(0.125, 0.249, 0.1, 0.9)),
               std::runtime_error);

  const Grid<bounded_extent> fail_y(Envelope(0.0, 1.0, 0.0, 0.25), 0.1, 0.1);
  EXPECT_THROW(fail_y.shrinkToFit(Envelope(0.1, 0.9, 0.001, 0.125)),
               std::runtime_error);
}

}  // namespace
}  // namespace geos_google
