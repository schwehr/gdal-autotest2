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

// Unit tests for third_party/geos/src/operation/intersection/Rectangle.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/intersection/Rectangle.cpp

#include "third_party/geos/include/geos/operation/intersection/Rectangle.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::operation::intersection::Rectangle;
using geos::util::IllegalArgumentException;

static_assert(Rectangle::Inside == 1);
static_assert(Rectangle::Outside == 2);
static_assert(Rectangle::Left == 4);
static_assert(Rectangle::Top == 8);
static_assert(Rectangle::Right == 16);
static_assert(Rectangle::Bottom == 32);
static_assert(Rectangle::TopLeft == (Rectangle::Top | Rectangle::Left));
static_assert(Rectangle::TopRight == (Rectangle::Top | Rectangle::Right));
static_assert(Rectangle::BottomLeft == (Rectangle::Bottom | Rectangle::Left));
static_assert(Rectangle::BottomRight == (Rectangle::Bottom | Rectangle::Right));

struct BoundsCase {
  std::string_view name;
  double x1;
  double y1;
  double x2;
  double y2;
};

struct Point2D {
  double x;
  double y;
};

struct PositionCase {
  std::string_view name;
  double x;
  double y;
  Rectangle::Position expected;
};

struct OnEdgeCase {
  std::string_view name;
  Rectangle::Position pos;
  bool expected;
};

struct OnSameEdgeCase {
  std::string_view name;
  Rectangle::Position pos1;
  Rectangle::Position pos2;
  bool expected;
};

struct NextEdgeCase {
  std::string_view name;
  Rectangle::Position input;
  Rectangle::Position expected;
};

constexpr double kInf = std::numeric_limits<double>::infinity();

constexpr auto kValidBounds = std::to_array<BoundsCase>({
    {
        .name = "unit_square",
        .x1 = 0.0,
        .y1 = 0.0,
        .x2 = 1.0,
        .y2 = 1.0,
    },
    {
        .name = "negative_quadrant",
        .x1 = -10.5,
        .y1 = -20.25,
        .x2 = -2.5,
        .y2 = -5.0,
    },
    {
        .name = "spanning_origin",
        .x1 = -3.5,
        .y1 = -4.5,
        .x2 = 7.25,
        .y2 = 9.75,
    },
    {
        .name = "tiny_extent",
        .x1 = 1.0,
        .y1 = 2.0,
        .x2 = 1.0 + 0x1p-30,
        .y2 = 2.0 + 0x1p-30,
    },
    {
        .name = "large_extent",
        .x1 = -0x1p30,
        .y1 = -0x1p30,
        .x2 = 0x1p30,
        .y2 = 0x1p30,
    },
});

constexpr auto kInvalidBounds = std::to_array<BoundsCase>({
    {
        .name = "zero_width",
        .x1 = 5.0,
        .y1 = 1.0,
        .x2 = 5.0,
        .y2 = 4.0,
    },
    {
        .name = "inverted_x",
        .x1 = 6.0,
        .y1 = 1.0,
        .x2 = 5.0,
        .y2 = 4.0,
    },
    {
        .name = "zero_height",
        .x1 = 1.0,
        .y1 = 3.0,
        .x2 = 4.0,
        .y2 = 3.0,
    },
    {
        .name = "inverted_y",
        .x1 = 1.0,
        .y1 = 5.0,
        .x2 = 4.0,
        .y2 = 3.0,
    },
    {
        .name = "degenerate_point",
        .x1 = 2.0,
        .y1 = 2.0,
        .x2 = 2.0,
        .y2 = 2.0,
    },
    {
        .name = "inverted_both",
        .x1 = 10.0,
        .y1 = 10.0,
        .x2 = -10.0,
        .y2 = -10.0,
    },
    {
        .name = "signed_zero_x",
        .x1 = -0.0,
        .y1 = 0.0,
        .x2 = 0.0,
        .y2 = 1.0,
    },
    {
        .name = "signed_zero_y",
        .x1 = 0.0,
        .y1 = -0.0,
        .x2 = 1.0,
        .y2 = 0.0,
    },
    {
        .name = "equal_pos_inf_x",
        .x1 = kInf,
        .y1 = 0.0,
        .x2 = kInf,
        .y2 = 1.0,
    },
    {
        .name = "equal_neg_inf_y",
        .x1 = 0.0,
        .y1 = -kInf,
        .x2 = 1.0,
        .y2 = -kInf,
    },
});

TEST(RectangleTest, ConstructsValidBoundsAndExposesAccessors) {
  for (const BoundsCase& c : kValidBounds) {
    SCOPED_TRACE(c.name);
    const Rectangle rect(c.x1, c.y1, c.x2, c.y2);
    EXPECT_DOUBLE_EQ(rect.xmin(), c.x1);
    EXPECT_DOUBLE_EQ(rect.ymin(), c.y1);
    EXPECT_DOUBLE_EQ(rect.xmax(), c.x2);
    EXPECT_DOUBLE_EQ(rect.ymax(), c.y2);
  }
}

TEST(RectangleTest, AcceptsInfiniteBounds) {
  const Rectangle full_plane(-kInf, -kInf, kInf, kInf);
  EXPECT_EQ(full_plane.xmin(), -kInf);
  EXPECT_EQ(full_plane.ymin(), -kInf);
  EXPECT_EQ(full_plane.xmax(), kInf);
  EXPECT_EQ(full_plane.ymax(), kInf);
  EXPECT_EQ(full_plane.position(0.0, 0.0), Rectangle::Inside);
  EXPECT_EQ(full_plane.position(kInf, 0.0), Rectangle::Right);
  EXPECT_EQ(full_plane.position(-kInf, kInf), Rectangle::TopLeft);

  const Rectangle half_open(-kInf, 0.0, 10.0, kInf);
  EXPECT_EQ(half_open.position(5.0, 100.0), Rectangle::Inside);
  EXPECT_EQ(half_open.position(10.0, 0.0), Rectangle::BottomRight);
  EXPECT_EQ(half_open.position(11.0, 1.0), Rectangle::Outside);
}

TEST(RectangleTest, ThrowsOnEmptyOrInvertedBounds) {
  for (const BoundsCase& c : kInvalidBounds) {
    SCOPED_TRACE(c.name);
    EXPECT_THROW(Rectangle(c.x1, c.y1, c.x2, c.y2), IllegalArgumentException);
  }

  try {
    Rectangle(2.0, 1.0, 2.0, 3.0);
    FAIL() << "Expected IllegalArgumentException";
  } catch (const IllegalArgumentException& e) {
    EXPECT_NE(
        std::string_view(e.what()).find("Clipping rectangle must be non-empty"),
        std::string_view::npos);
  }
}

TEST(RectangleTest, ToLinearRingProducesValidClosedClockwiseTwoDimRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  for (const BoundsCase& c : kValidBounds) {
    SCOPED_TRACE(c.name);
    const Rectangle rect(c.x1, c.y1, c.x2, c.y2);
    std::unique_ptr<LinearRing> ring = rect.toLinearRing(*factory);
    ASSERT_NE(ring, nullptr);

    EXPECT_EQ(ring->getGeometryTypeId(), GeometryTypeId::GEOS_LINEARRING);
    EXPECT_EQ(ring->getGeometryType(), "LinearRing");
    EXPECT_EQ(ring->getFactory(), factory.get());
    EXPECT_FALSE(ring->isEmpty());
    EXPECT_TRUE(ring->isClosed());
    EXPECT_TRUE(ring->isSimple());
    EXPECT_TRUE(ring->isValid());
    EXPECT_FALSE(ring->hasZ());
    EXPECT_FALSE(ring->hasM());
    EXPECT_EQ(ring->getCoordinateDimension(), 2u);
    EXPECT_EQ(ring->getNumPoints(), 5u);

    const double expected_perimeter = 2.0 * ((c.x2 - c.x1) + (c.y2 - c.y1));
    EXPECT_DOUBLE_EQ(ring->getLength(), expected_perimeter);

    const CoordinateSequence* coords = ring->getCoordinatesRO();
    ASSERT_NE(coords, nullptr);
    ASSERT_EQ(coords->size(), 5u);
    EXPECT_EQ(coords->getDimension(), 2u);

    const std::array<Point2D, 5> expected_vertices = {{
        {c.x1, c.y1},
        {c.x1, c.y2},
        {c.x2, c.y2},
        {c.x2, c.y1},
        {c.x1, c.y1},
    }};
    for (std::size_t i = 0; i < expected_vertices.size(); ++i) {
      EXPECT_DOUBLE_EQ(coords->getX(i), expected_vertices[i].x);
      EXPECT_DOUBLE_EQ(coords->getY(i), expected_vertices[i].y);
      EXPECT_TRUE(std::isnan(coords->getOrdinate(i, CoordinateSequence::Z)));
      EXPECT_TRUE(std::isnan(coords->getOrdinate(i, CoordinateSequence::M)));
    }
    EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D(
        coords->getAt<CoordinateXY>(4)));
  }
}

TEST(RectangleTest, ToPolygonProducesValidRectangleWithFactoryMetadata) {
  const PrecisionModel pm(PrecisionModel::FLOATING);
  constexpr int kSrid = 4326;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, kSrid);

  const Rectangle rect(1.5, -2.5, 5.5, 3.5);
  std::unique_ptr<Polygon> poly = rect.toPolygon(*factory);
  ASSERT_NE(poly, nullptr);

  EXPECT_EQ(poly->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_EQ(poly->getGeometryType(), "Polygon");
  EXPECT_EQ(poly->getFactory(), factory.get());
  EXPECT_EQ(poly->getSRID(), kSrid);
  EXPECT_FALSE(poly->isEmpty());
  EXPECT_TRUE(poly->isValid());
  EXPECT_TRUE(poly->isRectangle());
  EXPECT_FALSE(poly->hasZ());
  EXPECT_FALSE(poly->hasM());
  EXPECT_EQ(poly->getCoordinateDimension(), 2u);
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 24.0);
  EXPECT_DOUBLE_EQ(poly->getLength(), 20.0);

  const Envelope* env = poly->getEnvelopeInternal();
  ASSERT_NE(env, nullptr);
  ASSERT_FALSE(env->isNull());
  EXPECT_DOUBLE_EQ(env->getMinX(), 1.5);
  EXPECT_DOUBLE_EQ(env->getMinY(), -2.5);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 5.5);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 3.5);

  std::unique_ptr<LinearRing> expected_ring = rect.toLinearRing(*factory);
  ASSERT_NE(expected_ring, nullptr);
  EXPECT_TRUE(poly->getExteriorRing()->equalsExact(expected_ring.get()));

  const PrecisionModel fixed_pm(100.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm, 3857);
  std::unique_ptr<Polygon> fixed_poly = rect.toPolygon(*fixed_factory);
  ASSERT_NE(fixed_poly, nullptr);
  EXPECT_EQ(fixed_poly->getFactory(), fixed_factory.get());
  EXPECT_EQ(fixed_poly->getSRID(), 3857);
  EXPECT_TRUE(fixed_poly->isRectangle());
  EXPECT_DOUBLE_EQ(fixed_poly->getArea(), 24.0);
}

TEST(RectangleTest, ClassifiesPointPositionsInsideEdgesCornersAndOutside) {
  const Rectangle rect(-2.0, -1.0, 4.0, 5.0);

  constexpr auto kCases = std::to_array<PositionCase>({
      {.name = "center", .x = 1.0, .y = 2.0, .expected = Rectangle::Inside},
      {.name = "near_top_left",
       .x = -1.999,
       .y = 4.999,
       .expected = Rectangle::Inside},
      {.name = "left_edge", .x = -2.0, .y = 2.0, .expected = Rectangle::Left},
      {.name = "right_edge", .x = 4.0, .y = 2.0, .expected = Rectangle::Right},
      {.name = "bottom_edge",
       .x = 1.0,
       .y = -1.0,
       .expected = Rectangle::Bottom},
      {.name = "top_edge", .x = 1.0, .y = 5.0, .expected = Rectangle::Top},
      {.name = "bottom_left",
       .x = -2.0,
       .y = -1.0,
       .expected = Rectangle::BottomLeft},
      {.name = "top_left", .x = -2.0, .y = 5.0, .expected = Rectangle::TopLeft},
      {.name = "top_right",
       .x = 4.0,
       .y = 5.0,
       .expected = Rectangle::TopRight},
      {.name = "bottom_right",
       .x = 4.0,
       .y = -1.0,
       .expected = Rectangle::BottomRight},
      {.name = "outside_left",
       .x = -2.1,
       .y = 2.0,
       .expected = Rectangle::Outside},
      {.name = "outside_right",
       .x = 4.1,
       .y = 2.0,
       .expected = Rectangle::Outside},
      {.name = "outside_below",
       .x = 1.0,
       .y = -1.1,
       .expected = Rectangle::Outside},
      {.name = "outside_above",
       .x = 1.0,
       .y = 5.1,
       .expected = Rectangle::Outside},
      {.name = "outside_left_collinear_above",
       .x = -2.0,
       .y = 5.1,
       .expected = Rectangle::Outside},
      {.name = "outside_top_collinear_right",
       .x = 4.1,
       .y = 5.0,
       .expected = Rectangle::Outside},
      {.name = "outside_diagonal_bottom_left",
       .x = -3.0,
       .y = -2.0,
       .expected = Rectangle::Outside},
      {.name = "outside_diagonal_top_right",
       .x = 5.0,
       .y = 6.0,
       .expected = Rectangle::Outside},
  });

  for (const PositionCase& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(rect.position(c.x, c.y), c.expected);
  }

  const Rectangle signed_zero_rect(-0.0, -0.0, 1.0, 1.0);
  EXPECT_EQ(signed_zero_rect.position(0.0, 0.0), Rectangle::BottomLeft);
  EXPECT_EQ(signed_zero_rect.position(-0.0, 0.5), Rectangle::Left);
}

TEST(RectangleTest, OnEdgeIdentifiesEdgesAndCornersOnly) {
  constexpr auto kCases = std::to_array<OnEdgeCase>({
      {.name = "inside", .pos = Rectangle::Inside, .expected = false},
      {.name = "outside", .pos = Rectangle::Outside, .expected = false},
      {.name = "left", .pos = Rectangle::Left, .expected = true},
      {.name = "top", .pos = Rectangle::Top, .expected = true},
      {.name = "right", .pos = Rectangle::Right, .expected = true},
      {.name = "bottom", .pos = Rectangle::Bottom, .expected = true},
      {.name = "top_left", .pos = Rectangle::TopLeft, .expected = true},
      {.name = "top_right", .pos = Rectangle::TopRight, .expected = true},
      {.name = "bottom_left", .pos = Rectangle::BottomLeft, .expected = true},
      {.name = "bottom_right", .pos = Rectangle::BottomRight, .expected = true},
  });

  for (const OnEdgeCase& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(Rectangle::onEdge(c.pos), c.expected);
  }
}

TEST(RectangleTest, OnSameEdgeDetectsSharedBoundaryEdges) {
  constexpr auto kCases = std::to_array<OnSameEdgeCase>({
      {.name = "left_left",
       .pos1 = Rectangle::Left,
       .pos2 = Rectangle::Left,
       .expected = true},
      {.name = "top_top",
       .pos1 = Rectangle::Top,
       .pos2 = Rectangle::Top,
       .expected = true},
      {.name = "right_right",
       .pos1 = Rectangle::Right,
       .pos2 = Rectangle::Right,
       .expected = true},
      {.name = "bottom_bottom",
       .pos1 = Rectangle::Bottom,
       .pos2 = Rectangle::Bottom,
       .expected = true},
      {.name = "left_top_left",
       .pos1 = Rectangle::Left,
       .pos2 = Rectangle::TopLeft,
       .expected = true},
      {.name = "left_bottom_left",
       .pos1 = Rectangle::Left,
       .pos2 = Rectangle::BottomLeft,
       .expected = true},
      {.name = "top_top_left",
       .pos1 = Rectangle::Top,
       .pos2 = Rectangle::TopLeft,
       .expected = true},
      {.name = "top_top_right",
       .pos1 = Rectangle::Top,
       .pos2 = Rectangle::TopRight,
       .expected = true},
      {.name = "right_top_right",
       .pos1 = Rectangle::Right,
       .pos2 = Rectangle::TopRight,
       .expected = true},
      {.name = "right_bottom_right",
       .pos1 = Rectangle::Right,
       .pos2 = Rectangle::BottomRight,
       .expected = true},
      {.name = "bottom_bottom_left",
       .pos1 = Rectangle::Bottom,
       .pos2 = Rectangle::BottomLeft,
       .expected = true},
      {.name = "bottom_bottom_right",
       .pos1 = Rectangle::Bottom,
       .pos2 = Rectangle::BottomRight,
       .expected = true},
      {.name = "corners_sharing_left",
       .pos1 = Rectangle::TopLeft,
       .pos2 = Rectangle::BottomLeft,
       .expected = true},
      {.name = "corners_sharing_top",
       .pos1 = Rectangle::TopLeft,
       .pos2 = Rectangle::TopRight,
       .expected = true},
      {.name = "corners_sharing_right",
       .pos1 = Rectangle::TopRight,
       .pos2 = Rectangle::BottomRight,
       .expected = true},
      {.name = "corners_sharing_bottom",
       .pos1 = Rectangle::BottomLeft,
       .pos2 = Rectangle::BottomRight,
       .expected = true},
      {.name = "same_corner",
       .pos1 = Rectangle::TopLeft,
       .pos2 = Rectangle::TopLeft,
       .expected = true},
      {.name = "opposite_edges_lr",
       .pos1 = Rectangle::Left,
       .pos2 = Rectangle::Right,
       .expected = false},
      {.name = "opposite_edges_tb",
       .pos1 = Rectangle::Top,
       .pos2 = Rectangle::Bottom,
       .expected = false},
      {.name = "perpendicular_edges",
       .pos1 = Rectangle::Left,
       .pos2 = Rectangle::Top,
       .expected = false},
      {.name = "opposite_corners_tl_br",
       .pos1 = Rectangle::TopLeft,
       .pos2 = Rectangle::BottomRight,
       .expected = false},
      {.name = "opposite_corners_tr_bl",
       .pos1 = Rectangle::TopRight,
       .pos2 = Rectangle::BottomLeft,
       .expected = false},
      {.name = "edge_and_opposite_corner",
       .pos1 = Rectangle::Left,
       .pos2 = Rectangle::TopRight,
       .expected = false},
      {.name = "inside_inside",
       .pos1 = Rectangle::Inside,
       .pos2 = Rectangle::Inside,
       .expected = false},
      {.name = "outside_outside",
       .pos1 = Rectangle::Outside,
       .pos2 = Rectangle::Outside,
       .expected = false},
      {.name = "inside_outside",
       .pos1 = Rectangle::Inside,
       .pos2 = Rectangle::Outside,
       .expected = false},
      {.name = "inside_left",
       .pos1 = Rectangle::Inside,
       .pos2 = Rectangle::Left,
       .expected = false},
      {.name = "outside_top_right",
       .pos1 = Rectangle::Outside,
       .pos2 = Rectangle::TopRight,
       .expected = false},
  });

  for (const OnSameEdgeCase& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(Rectangle::onSameEdge(c.pos1, c.pos2), c.expected);
    EXPECT_EQ(Rectangle::onSameEdge(c.pos2, c.pos1), c.expected);
  }
}

TEST(RectangleTest, NextEdgeAdvancesClockwiseAndLeavesNonEdgesUnchanged) {
  constexpr auto kCases = std::to_array<NextEdgeCase>({
      {.name = "bottom_left",
       .input = Rectangle::BottomLeft,
       .expected = Rectangle::Top},
      {.name = "left", .input = Rectangle::Left, .expected = Rectangle::Top},
      {.name = "top_left",
       .input = Rectangle::TopLeft,
       .expected = Rectangle::Right},
      {.name = "top", .input = Rectangle::Top, .expected = Rectangle::Right},
      {.name = "top_right",
       .input = Rectangle::TopRight,
       .expected = Rectangle::Bottom},
      {.name = "right",
       .input = Rectangle::Right,
       .expected = Rectangle::Bottom},
      {.name = "bottom_right",
       .input = Rectangle::BottomRight,
       .expected = Rectangle::Left},
      {.name = "bottom",
       .input = Rectangle::Bottom,
       .expected = Rectangle::Left},
      {.name = "inside",
       .input = Rectangle::Inside,
       .expected = Rectangle::Inside},
      {.name = "outside",
       .input = Rectangle::Outside,
       .expected = Rectangle::Outside},
  });

  for (const NextEdgeCase& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(Rectangle::nextEdge(c.input), c.expected);
  }

  Rectangle::Position pos = Rectangle::Left;
  pos = Rectangle::nextEdge(pos);
  EXPECT_EQ(pos, Rectangle::Top);
  pos = Rectangle::nextEdge(pos);
  EXPECT_EQ(pos, Rectangle::Right);
  pos = Rectangle::nextEdge(pos);
  EXPECT_EQ(pos, Rectangle::Bottom);
  pos = Rectangle::nextEdge(pos);
  EXPECT_EQ(pos, Rectangle::Left);
}

}  // namespace
}  // namespace geos_google
