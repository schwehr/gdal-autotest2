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

// Unit tests for third_party/geos/src/operation/grid/Cell.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/grid/Cell.cpp

#include "third_party/geos/include/geos/operation/grid/Cell.h"

#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/grid/Side.h"
#include "third_party/geos/include/geos/operation/grid/Traversal.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::grid::Cell;
using geos::operation::grid::Side;

// geos::operation::grid::operator<<(ostream&, const Side&) is declared in
// Side.h without a library definition, so convert Side before GTest prints.
constexpr std::string_view SideName(Side s) {
  switch (s) {
    case Side::NONE:
      return "NONE";
    case Side::LEFT:
      return "LEFT";
    case Side::RIGHT:
      return "RIGHT";
    case Side::TOP:
      return "TOP";
    case Side::BOTTOM:
      return "BOTTOM";
  }
  return "UNKNOWN";
}

Envelope UnitBox10() { return Envelope(0.0, 10.0, 0.0, 10.0); }

TEST(CellTest, ConstructorsAndMetrics) {
  // Note: Cell's 4-double constructor forwards its arguments directly to
  // Envelope(x1, x2, y1, y2), so m_box has x-range [arg0, arg1] and
  // y-range [arg2, arg3].
  Cell cell_xy(1.0, 5.0, 2.0, 8.0);
  EXPECT_EQ(cell_xy.box(), Envelope(1.0, 5.0, 2.0, 8.0));
  EXPECT_DOUBLE_EQ(cell_xy.getWidth(), 4.0);
  EXPECT_DOUBLE_EQ(cell_xy.getHeight(), 6.0);
  EXPECT_DOUBLE_EQ(cell_xy.getArea(), 24.0);

  Cell cell_forwarded(1.0, 2.0, 5.0, 8.0);
  EXPECT_EQ(cell_forwarded.box(), Envelope(1.0, 2.0, 5.0, 8.0));
  EXPECT_DOUBLE_EQ(cell_forwarded.getWidth(), 1.0);
  EXPECT_DOUBLE_EQ(cell_forwarded.getHeight(), 3.0);
  EXPECT_DOUBLE_EQ(cell_forwarded.getArea(), 3.0);

  Envelope env(-4.0, 2.0, -3.0, 9.0);
  Cell cell_env(env);
  EXPECT_EQ(cell_env.box(), env);
  EXPECT_DOUBLE_EQ(cell_env.getWidth(), 6.0);
  EXPECT_DOUBLE_EQ(cell_env.getHeight(), 12.0);
  EXPECT_DOUBLE_EQ(cell_env.getArea(), 72.0);

  // Initial empty state.
  EXPECT_DOUBLE_EQ(cell_env.getTraversalLength(), 0.0);
  EXPECT_FALSE(cell_env.isDetermined());
  EXPECT_THROW(cell_env.getLastTraversal(), std::out_of_range);
  EXPECT_THROW(cell_env.forceExit(), std::out_of_range);
  EXPECT_THROW(cell_env.getCoveredFraction(), std::runtime_error);
  GeometryFactory::Ptr factory = GeometryFactory::create();
  EXPECT_THROW(cell_env.getCoveredPolygons(*factory), std::runtime_error);
}

TEST(CellTest, EntrySidesDetectedForBoundaryAndInterior) {
  struct EntryCase {
    std::string_view name;
    CoordinateXY point;
    Side expected_side;
  };
  const auto kCases = std::to_array<EntryCase>({
      {.name = "left", .point = {0.0, 5.0}, .expected_side = Side::LEFT},
      {.name = "right", .point = {10.0, 5.0}, .expected_side = Side::RIGHT},
      {.name = "bottom", .point = {5.0, 0.0}, .expected_side = Side::BOTTOM},
      {.name = "top", .point = {5.0, 10.0}, .expected_side = Side::TOP},
      {.name = "interior", .point = {5.0, 5.0}, .expected_side = Side::NONE},
      {.name = "bottom_left_corner",
       .point = {0.0, 0.0},
       .expected_side = Side::LEFT},
      {.name = "top_left_corner",
       .point = {0.0, 10.0},
       .expected_side = Side::LEFT},
      {.name = "bottom_right_corner",
       .point = {10.0, 0.0},
       .expected_side = Side::RIGHT},
      {.name = "top_right_corner",
       .point = {10.0, 10.0},
       .expected_side = Side::RIGHT},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(tc.point));
    EXPECT_EQ(SideName(cell.getLastTraversal().getEntrySide()),
              SideName(tc.expected_side));
    EXPECT_FALSE(cell.getLastTraversal().isExited());
  }
}

TEST(CellTest, CrossingAllSidesAndQuadrants) {
  struct CrossingCase {
    std::string_view name;
    CoordinateXY target;
    Side expected_side;
    CoordinateXY expected_exit;
  };
  // All exits start from the cell center (5, 5) of [0, 10] x [0, 10].
  const auto kCases = std::to_array<CrossingCase>({
      {.name = "vertical_top",
       .target = {5.0, 15.0},
       .expected_side = Side::TOP,
       .expected_exit = {5.0, 10.0}},
      {.name = "vertical_bottom",
       .target = {5.0, -5.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {5.0, 0.0}},
      {.name = "horizontal_right",
       .target = {15.0, 5.0},
       .expected_side = Side::RIGHT,
       .expected_exit = {10.0, 5.0}},
      {.name = "horizontal_left",
       .target = {-5.0, 5.0},
       .expected_side = Side::LEFT,
       .expected_exit = {0.0, 5.0}},
      {.name = "q1_right",
       .target = {13.0, 7.0},
       .expected_side = Side::RIGHT,
       .expected_exit = {10.0, 6.25}},
      {.name = "q1_top",
       .target = {7.0, 13.0},
       .expected_side = Side::TOP,
       .expected_exit = {6.25, 10.0}},
      {.name = "q1_exact_corner",
       .target = {15.0, 15.0},
       .expected_side = Side::TOP,
       .expected_exit = {10.0, 10.0}},
      {.name = "q2_left",
       .target = {-3.0, 7.0},
       .expected_side = Side::LEFT,
       .expected_exit = {0.0, 6.25}},
      {.name = "q2_top",
       .target = {3.0, 13.0},
       .expected_side = Side::TOP,
       .expected_exit = {3.75, 10.0}},
      {.name = "q2_exact_corner",
       .target = {-5.0, 15.0},
       .expected_side = Side::TOP,
       .expected_exit = {0.0, 10.0}},
      {.name = "q4_right",
       .target = {13.0, 3.0},
       .expected_side = Side::RIGHT,
       .expected_exit = {10.0, 3.75}},
      {.name = "q4_bottom",
       .target = {7.0, -3.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {6.25, 0.0}},
      {.name = "q4_exact_corner",
       .target = {15.0, -5.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {10.0, 0.0}},
      {.name = "q3_left",
       .target = {-3.0, 3.0},
       .expected_side = Side::LEFT,
       .expected_exit = {0.0, 3.75}},
      {.name = "q3_bottom",
       .target = {3.0, -3.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {3.75, 0.0}},
      {.name = "q3_exact_corner",
       .target = {-5.0, -5.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {0.0, 0.0}},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    EXPECT_FALSE(cell.take(tc.target));
    const auto& t = cell.getLastTraversal();
    EXPECT_TRUE(t.isExited());
    EXPECT_EQ(SideName(t.getExitSide()), SideName(tc.expected_side));
    EXPECT_DOUBLE_EQ(t.getExitCoordinate().x, tc.expected_exit.x);
    EXPECT_DOUBLE_EQ(t.getExitCoordinate().y, tc.expected_exit.y);
  }
}

TEST(CellTest, TakeUsesPrevOriginalWhenProvidedAndClampsOrRejectsOutside) {
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 2.0}));
    // Segment from (-4, -1) to (12, 7) has slope 0.5 and crosses x=10 at y=6,
    // distinguishing it from using the stored interpolated point (0, 2).
    const CoordinateXY prev_original{-4.0, -1.0};
    EXPECT_FALSE(cell.take(CoordinateXY{12.0, 7.0}, &prev_original));
    EXPECT_EQ(SideName(cell.getLastTraversal().getExitSide()), "RIGHT");
    EXPECT_DOUBLE_EQ(cell.getLastTraversal().getExitCoordinate().x, 10.0);
    EXPECT_DOUBLE_EQ(cell.getLastTraversal().getExitCoordinate().y, 6.0);
  }

  // Clamping in each quadrant when prev_original lies outside the cell.
  struct ClampCase {
    std::string_view name;
    CoordinateXY entry;
    CoordinateXY prev_original;
    CoordinateXY target;
    Side expected_side;
    CoordinateXY expected_exit;
  };
  const auto kClampCases = std::to_array<ClampCase>({
      {.name = "q1_clamp_ymin",
       .entry = {0.0, 0.0},
       .prev_original = {-1.0, -2.0},
       .target = {12.0, -0.5},
       .expected_side = Side::RIGHT,
       .expected_exit = {10.0, 0.0}},
      {.name = "q1_clamp_xmin",
       .entry = {0.0, 5.0},
       .prev_original = {-2.0, 4.0},
       .target = {-0.5, 12.0},
       .expected_side = Side::TOP,
       .expected_exit = {0.0, 10.0}},
      {.name = "q2_clamp_ymin",
       .entry = {10.0, 0.0},
       .prev_original = {11.0, -2.0},
       .target = {-2.0, -0.5},
       .expected_side = Side::LEFT,
       .expected_exit = {0.0, 0.0}},
      {.name = "q2_clamp_xmax",
       .entry = {10.0, 5.0},
       .prev_original = {12.0, 4.0},
       .target = {10.5, 12.0},
       .expected_side = Side::TOP,
       .expected_exit = {10.0, 10.0}},
      {.name = "q3_clamp_ymax",
       .entry = {10.0, 10.0},
       .prev_original = {11.0, 12.0},
       .target = {-2.0, 10.5},
       .expected_side = Side::LEFT,
       .expected_exit = {0.0, 10.0}},
      {.name = "q3_clamp_xmax",
       .entry = {10.0, 5.0},
       .prev_original = {12.0, 6.0},
       .target = {10.5, -2.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {10.0, 0.0}},
      {.name = "q4_clamp_ymax",
       .entry = {0.0, 10.0},
       .prev_original = {-1.0, 12.0},
       .target = {12.0, 10.5},
       .expected_side = Side::RIGHT,
       .expected_exit = {10.0, 10.0}},
      {.name = "q4_clamp_xmin",
       .entry = {0.0, 5.0},
       .prev_original = {-2.0, 6.0},
       .target = {-0.5, -2.0},
       .expected_side = Side::BOTTOM,
       .expected_exit = {0.0, 0.0}},
  });

  for (const auto& tc : kClampCases) {
    SCOPED_TRACE(tc.name);
    Cell cell(UnitBox10());
    ASSERT_TRUE(cell.take(tc.entry));
    EXPECT_FALSE(cell.take(tc.target, &tc.prev_original));
    const auto& t = cell.getLastTraversal();
    EXPECT_EQ(SideName(t.getExitSide()), SideName(tc.expected_side));
    EXPECT_DOUBLE_EQ(t.getExitCoordinate().x, tc.expected_exit.x);
    EXPECT_DOUBLE_EQ(t.getExitCoordinate().y, tc.expected_exit.y);
  }

  // Vertical/horizontal aligned coordinates strictly outside y/x bounds throw.
  {
    Cell cell(UnitBox10());
    ASSERT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    const CoordinateXY prev_outside_right{12.0, 2.0};
    EXPECT_THROW(cell.take(CoordinateXY{12.0, 8.0}, &prev_outside_right),
                 std::runtime_error);
  }
  {
    Cell cell(UnitBox10());
    ASSERT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    const CoordinateXY prev_outside_top{2.0, 12.0};
    EXPECT_THROW(cell.take(CoordinateXY{8.0, 12.0}, &prev_outside_top),
                 std::runtime_error);
  }
}

TEST(CellTest, ForceExitBehavior) {
  // Ending on boundary marks exit side and makes subsequent calls idempotent.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    EXPECT_FALSE(cell.getLastTraversal().isExited());

    cell.forceExit();
    EXPECT_TRUE(cell.getLastTraversal().isExited());
    EXPECT_EQ(SideName(cell.getLastTraversal().getExitSide()), "RIGHT");

    // Calling forceExit again after already exited is a no-op.
    cell.forceExit();
    EXPECT_TRUE(cell.getLastTraversal().isExited());
    EXPECT_EQ(SideName(cell.getLastTraversal().getExitSide()), "RIGHT");
  }

  // Ending strictly inside leaves traversal un-exited.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    cell.forceExit();
    EXPECT_FALSE(cell.getLastTraversal().isExited());
    EXPECT_EQ(SideName(cell.getLastTraversal().getExitSide()), "NONE");
  }

  // Single boundary point exited via forceExit is traversed but not determined.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 10.0}));
    cell.forceExit();
    EXPECT_TRUE(cell.getLastTraversal().isExited());
    EXPECT_EQ(SideName(cell.getLastTraversal().getExitSide()), "TOP");
    EXPECT_TRUE(cell.getLastTraversal().isTraversed());
    EXPECT_FALSE(cell.isDetermined());
  }
}

TEST(CellTest, ClosedRingsAndIsDeterminedEdgeCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Closed CCW ring inside cell is determined without exit side.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 2.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{8.0, 2.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{8.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 2.0}));

    EXPECT_TRUE(cell.getLastTraversal().isClosedRing());
    EXPECT_FALSE(cell.getLastTraversal().isExited());
    EXPECT_TRUE(cell.isDetermined());
    EXPECT_DOUBLE_EQ(cell.getTraversalLength(), 24.0);
    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.36);

    // Subsequent take starts a new traversal because previous isClosedRing().
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_EQ(SideName(cell.getLastTraversal().getEntrySide()), "LEFT");
    EXPECT_FALSE(cell.getLastTraversal().isClosedRing());
  }

  // Degenerate closed ring of identical coordinates is not determined.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    EXPECT_TRUE(cell.getLastTraversal().isClosedRing());
    EXPECT_FALSE(cell.isDetermined());
    EXPECT_THROW(cell.getCoveredFraction(), std::runtime_error);
    EXPECT_THROW(cell.getCoveredPolygons(*factory), std::runtime_error);
  }

  // Traversal started strictly inside and exited still lacks an entry side
  // (isTraversed() == false), so it is not determined until a boundary-entered
  // full traversal is added.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{6.0, 5.0}));
    EXPECT_FALSE(cell.isDetermined());

    EXPECT_FALSE(cell.take(CoordinateXY{12.0, 5.0}));
    EXPECT_FALSE(cell.getLastTraversal().isEntered());
    EXPECT_TRUE(cell.getLastTraversal().isExited());
    EXPECT_FALSE(cell.getLastTraversal().isTraversed());
    EXPECT_FALSE(cell.isDetermined());

    // Add a boundary-entered incomplete traversal, then complete it.
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.getLastTraversal().isEntered());
    EXPECT_FALSE(cell.isDetermined());
    EXPECT_FALSE(cell.take(CoordinateXY{12.0, 5.0}));
    EXPECT_TRUE(cell.isDetermined());
  }

  // Degenerate single-point and repeated-point boundary traversals followed by
  // a non-degenerate traversal.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 2.0}));
    cell.forceExit();
    EXPECT_FALSE(cell.isDetermined());

    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 3.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 3.0}));
    cell.forceExit();
    EXPECT_FALSE(cell.isDetermined());

    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    cell.forceExit();
    EXPECT_TRUE(cell.isDetermined());
    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.5);
  }
}

TEST(CellTest, TraversalLengthAcrossMultipleTraversals) {
  Cell cell(UnitBox10());
  // First traversal: (0,0) -> (3,4) [len 5] -> (3,10) [len 6].
  EXPECT_TRUE(cell.take(CoordinateXY{0.0, 0.0}));
  EXPECT_TRUE(cell.take(CoordinateXY{3.0, 4.0}));
  EXPECT_TRUE(cell.take(CoordinateXY{3.0, 10.0}));
  cell.forceExit();
  EXPECT_DOUBLE_EQ(cell.getTraversalLength(), 11.0);

  // Second traversal: (10,2) -> (6,2) [len 4] -> (6,0) [len 2].
  EXPECT_TRUE(cell.take(CoordinateXY{10.0, 2.0}));
  EXPECT_TRUE(cell.take(CoordinateXY{6.0, 2.0}));
  EXPECT_TRUE(cell.take(CoordinateXY{6.0, 0.0}));
  cell.forceExit();
  EXPECT_DOUBLE_EQ(cell.getTraversalLength(), 17.0);

  // Third incomplete traversal still contributes to traversal length.
  EXPECT_TRUE(cell.take(CoordinateXY{1.0, 1.0}));
  EXPECT_TRUE(cell.take(CoordinateXY{4.0, 5.0}));
  EXPECT_DOUBLE_EQ(cell.getTraversalLength(), 22.0);
}

TEST(CellTest, CoveredFractionAndPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // 1. Horizontal split: left-hand area of (0,5)->(10,5) is top half.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    cell.forceExit();

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.5);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 5, 10 5, 10 10, 0 10, 0 5))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));

    // Trailing incomplete traversal is ignored by getCoordLists().
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 2.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{6.0, 2.0}));
    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.5);
  }

  // 2. Diagonal corner clip (5,0)->(10,5): left-hand covers 87.5%.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 0.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    cell.forceExit();

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.875);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 0, 5 0, 10 5, 10 10, 0 10, 0 0))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }

  // 3. Reversed corner clip (10,5)->(5,0): left-hand is bottom-right triangle.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 0.0}));
    cell.forceExit();

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.125);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((5 0, 10 0, 10 5, 5 0))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }

  // 4. Two disjoint corner triangles (left-hand side of each clip) produce a
  // MultiPolygon.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 0.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 2.0}));
    cell.forceExit();

    EXPECT_TRUE(cell.take(CoordinateXY{8.0, 10.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 8.0}));
    cell.forceExit();

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.04);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    EXPECT_DOUBLE_EQ(cov->getArea(), 4.0);
    EXPECT_EQ(cov->getNumGeometries(), 2u);
    cov->normalize();
    std::unique_ptr<Geometry> expected = reader.read(
        "MULTIPOLYGON (((0 0, 2 0, 0 2, 0 0)), ((8 10, 10 8, 10 10, 8 10)))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }

  // 4b. Reversing both corner clips places the central hexagon on the left.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 2.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 0.0}));
    cell.forceExit();

    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{8.0, 10.0}));
    cell.forceExit();

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.96);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    EXPECT_DOUBLE_EQ(cov->getArea(), 96.0);
    EXPECT_EQ(cov->getNumGeometries(), 1u);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 2, 2 0, 10 0, 10 8, 8 10, 0 10, 0 2))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }

  // 5. Clockwise closed ring creates a hole subtracted from the full cell.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 2.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{8.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{8.0, 2.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 2.0}));

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.64);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    const auto* poly = dynamic_cast<const Polygon*>(cov.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 64.0);
  }

  // 6. Exterior traversal combined with an interior clockwise hole.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    cell.forceExit();

    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 6.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{4.0, 8.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{4.0, 6.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{2.0, 6.0}));

    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.46);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    const auto* poly = dynamic_cast<const Polygon*>(cov.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(poly->getArea(), 46.0);
  }

  // 7. Boundary-grazing traversal along the LEFT edge determines empty vs full.
  {
    Cell cell_out(UnitBox10());
    EXPECT_TRUE(cell_out.take(CoordinateXY{0.0, 2.0}));
    EXPECT_TRUE(cell_out.take(CoordinateXY{0.0, 8.0}));
    cell_out.forceExit();
    EXPECT_TRUE(cell_out.isDetermined());
    EXPECT_DOUBLE_EQ(cell_out.getCoveredFraction(), 0.0);
    std::unique_ptr<Geometry> cov_out = cell_out.getCoveredPolygons(*factory);
    EXPECT_TRUE(cov_out->isEmpty());

    Cell cell_in(UnitBox10());
    EXPECT_TRUE(cell_in.take(CoordinateXY{0.0, 8.0}));
    EXPECT_TRUE(cell_in.take(CoordinateXY{0.0, 2.0}));
    cell_in.forceExit();
    EXPECT_TRUE(cell_in.isDetermined());
    EXPECT_DOUBLE_EQ(cell_in.getCoveredFraction(), 1.0);
    std::unique_ptr<Geometry> cov_in = cell_in.getCoveredPolygons(*factory);
    EXPECT_DOUBLE_EQ(cov_in->getArea(), 100.0);
  }

  // 8. Closed CCW diamond touching all four boundaries.
  {
    Cell cell(UnitBox10());
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 0.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{10.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{5.0, 10.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{0.0, 5.0}));

    EXPECT_TRUE(cell.getLastTraversal().isClosedRing());
    EXPECT_FALSE(cell.getLastTraversal().isExited());
    EXPECT_TRUE(cell.isDetermined());
    EXPECT_DOUBLE_EQ(cell.getTraversalLength(), 4.0 * std::hypot(5.0, 5.0));
    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.5);
    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((0 5, 5 0, 10 5, 5 10, 0 5))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }
}

TEST(CellTest, NonUnitShiftedNonSquareCell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  const Envelope box(-10.0, -2.0, 5.0, 17.0);  // 8 x 12, area = 96.

  {
    Cell cell(box);
    EXPECT_DOUBLE_EQ(cell.getWidth(), 8.0);
    EXPECT_DOUBLE_EQ(cell.getHeight(), 12.0);
    EXPECT_DOUBLE_EQ(cell.getArea(), 96.0);

    // Split vertically down the middle: left half [-10, -6] x [5, 17].
    EXPECT_TRUE(cell.take(CoordinateXY{-6.0, 5.0}));
    EXPECT_TRUE(cell.take(CoordinateXY{-6.0, 17.0}));
    cell.forceExit();
    EXPECT_TRUE(cell.isDetermined());
    EXPECT_DOUBLE_EQ(cell.getTraversalLength(), 12.0);
    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 0.5);

    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((-10 5, -6 5, -6 17, -10 17, -10 5))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }

  {
    Cell cell(box);
    // Enter on LEFT at (-10, 7) along segment (-14, 5) -> (2, 13), exiting on
    // RIGHT at (-2, 11).
    EXPECT_TRUE(cell.take(CoordinateXY{-10.0, 7.0}));
    const CoordinateXY prev_original{-14.0, 5.0};
    EXPECT_FALSE(cell.take(CoordinateXY{2.0, 13.0}, &prev_original));
    EXPECT_EQ(SideName(cell.getLastTraversal().getExitSide()), "RIGHT");
    EXPECT_DOUBLE_EQ(cell.getLastTraversal().getExitCoordinate().x, -2.0);
    EXPECT_DOUBLE_EQ(cell.getLastTraversal().getExitCoordinate().y, 11.0);
    EXPECT_TRUE(cell.isDetermined());
    EXPECT_DOUBLE_EQ(cell.getTraversalLength(), std::hypot(8.0, 4.0));
    EXPECT_DOUBLE_EQ(cell.getCoveredFraction(), 64.0 / 96.0);

    std::unique_ptr<Geometry> cov = cell.getCoveredPolygons(*factory);
    cov->normalize();
    std::unique_ptr<Geometry> expected =
        reader.read("POLYGON ((-10 7, -2 11, -2 17, -10 17, -10 7))");
    expected->normalize();
    EXPECT_TRUE(cov->equalsExact(expected.get(), 1e-12));
  }
}

}  // namespace
}  // namespace geos_google
