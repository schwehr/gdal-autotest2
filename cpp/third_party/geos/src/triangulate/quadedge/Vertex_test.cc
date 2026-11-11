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

// Unit tests for third_party/geos/src/triangulate/quadedge/Vertex.cpp.
//
// Notes:
// - `bisector`, `circumRadiusRatio`, `midPoint`, `circleCenter`,
//   `interpolateZValue`, and both `interpolateZ` overloads (along with the
//   inline helper `distance`) are declared `private:` in `Vertex.h` with no
//   friends or callers in GEOS; they are exercised here via standard C++20
//   explicit template instantiation member-pointer accessors.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/quadedge/Vertex.cpp

#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <set>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/HCoordinate.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"

namespace geos_google {
namespace {

using geos::algorithm::HCoordinate;
using geos::geom::Coordinate;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeQuartet;
using geos::triangulate::quadedge::Vertex;

template <typename Tag, typename Tag::type M>
struct AccessPrivate {
  friend typename Tag::type GetPrivate(Tag) { return M; }
};

struct BisectorTag {
  using type = std::unique_ptr<HCoordinate> (*)(const Vertex&, const Vertex&);
  friend type GetPrivate(BisectorTag);
};
template struct AccessPrivate<BisectorTag, &Vertex::bisector>;

struct CircumRadiusRatioTag {
  using type = double (Vertex::*)(const Vertex&, const Vertex&);
  friend type GetPrivate(CircumRadiusRatioTag);
};
template struct AccessPrivate<CircumRadiusRatioTag, &Vertex::circumRadiusRatio>;

struct MidPointTag {
  using type = std::unique_ptr<Vertex> (Vertex::*)(const Vertex&);
  friend type GetPrivate(MidPointTag);
};
template struct AccessPrivate<MidPointTag, &Vertex::midPoint>;

struct CircleCenterTag {
  using type = std::unique_ptr<Vertex> (Vertex::*)(const Vertex&,
                                                   const Vertex&) const;
  friend type GetPrivate(CircleCenterTag);
};
template struct AccessPrivate<CircleCenterTag, &Vertex::circleCenter>;

struct InterpolateZValueTag {
  using type = double (Vertex::*)(const Vertex&, const Vertex&,
                                  const Vertex&) const;
  friend type GetPrivate(InterpolateZValueTag);
};
template struct AccessPrivate<InterpolateZValueTag, &Vertex::interpolateZValue>;

struct InterpolateZTriangleTag {
  using type = double (*)(const Coordinate&, const Coordinate&,
                          const Coordinate&, const Coordinate&);
  friend type GetPrivate(InterpolateZTriangleTag);
};
template struct AccessPrivate<InterpolateZTriangleTag, &Vertex::interpolateZ>;

struct InterpolateZSegmentTag {
  using type = double (*)(const Coordinate&, const Coordinate&,
                          const Coordinate&);
  friend type GetPrivate(InterpolateZSegmentTag);
};
template struct AccessPrivate<InterpolateZSegmentTag, &Vertex::interpolateZ>;

struct ClassifyCase {
  std::string_view name;
  double p0x;
  double p0y;
  double p1x;
  double p1y;
  double p2x;
  double p2y;
  int expected;
};

constexpr auto kClassifyCases = std::to_array<ClassifyCase>({
    // Diagonal segment (1, 2) -> (5, 6), direction vector (4, 4).
    {
        .name = "diag_left",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = 1.0,
        .p2y = 5.0,
        .expected = +Vertex::LEFT,
    },
    {
        .name = "diag_right",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = 5.0,
        .p2y = 2.0,
        .expected = +Vertex::RIGHT,
    },
    {
        .name = "diag_behind",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = -1.0,
        .p2y = 0.0,
        .expected = +Vertex::BEHIND,
    },
    {
        .name = "diag_beyond",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = 7.0,
        .p2y = 8.0,
        .expected = +Vertex::BEYOND,
    },
    {
        .name = "diag_origin",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = 1.0,
        .p2y = 2.0,
        .expected = +Vertex::ORIGIN,
    },
    {
        .name = "diag_destination",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = 5.0,
        .p2y = 6.0,
        .expected = +Vertex::DESTINATION,
    },
    {
        .name = "diag_between",
        .p0x = 1.0,
        .p0y = 2.0,
        .p1x = 5.0,
        .p1y = 6.0,
        .p2x = 3.0,
        .p2y = 4.0,
        .expected = +Vertex::BETWEEN,
    },
    // Horizontal segment (0, 0) -> (4, 0) exercises X-axis BEHIND branch.
    {
        .name = "horiz_behind",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 4.0,
        .p1y = 0.0,
        .p2x = -2.0,
        .p2y = 0.0,
        .expected = +Vertex::BEHIND,
    },
    {
        .name = "horiz_between",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 4.0,
        .p1y = 0.0,
        .p2x = 1.0,
        .p2y = 0.0,
        .expected = +Vertex::BETWEEN,
    },
    // Vertical segment (0, 0) -> (0, 4) exercises Y-axis BEHIND branch.
    {
        .name = "vert_left",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = -1.0,
        .p2y = 2.0,
        .expected = +Vertex::LEFT,
    },
    {
        .name = "vert_right",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = 1.0,
        .p2y = 2.0,
        .expected = +Vertex::RIGHT,
    },
    {
        .name = "vert_behind",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = 0.0,
        .p2y = -2.0,
        .expected = +Vertex::BEHIND,
    },
    {
        .name = "vert_beyond",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = 0.0,
        .p2y = 5.0,
        .expected = +Vertex::BEYOND,
    },
    {
        .name = "vert_origin",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = 0.0,
        .p2y = 0.0,
        .expected = +Vertex::ORIGIN,
    },
    {
        .name = "vert_destination",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = 0.0,
        .p2y = 4.0,
        .expected = +Vertex::DESTINATION,
    },
    {
        .name = "vert_between",
        .p0x = 0.0,
        .p0y = 0.0,
        .p1x = 0.0,
        .p1y = 4.0,
        .p2x = 0.0,
        .p2y = 3.0,
        .expected = +Vertex::BETWEEN,
    },
    // Degenerate zero-length segment (2, 3) -> (2, 3).
    {
        .name = "zero_seg_same_point_origin",
        .p0x = 2.0,
        .p0y = 3.0,
        .p1x = 2.0,
        .p1y = 3.0,
        .p2x = 2.0,
        .p2y = 3.0,
        .expected = +Vertex::ORIGIN,
    },
    {
        .name = "zero_seg_other_point_beyond",
        .p0x = 2.0,
        .p0y = 3.0,
        .p1x = 2.0,
        .p1y = 3.0,
        .p2x = 4.0,
        .p2y = 5.0,
        .expected = +Vertex::BEYOND,
    },
});

TEST(VertexTest, ConstructorsAndCoordinateAccessors) {
  const Vertex default_v;
  EXPECT_DOUBLE_EQ(default_v.getX(), 0.0);
  EXPECT_DOUBLE_EQ(default_v.getY(), 0.0);
  EXPECT_TRUE(std::isnan(default_v.getZ()));
  EXPECT_DOUBLE_EQ(default_v.getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(default_v.getCoordinate().y, 0.0);

  const Vertex xy_v(-3.5, 7.25);
  EXPECT_DOUBLE_EQ(xy_v.getX(), -3.5);
  EXPECT_DOUBLE_EQ(xy_v.getY(), 7.25);
  EXPECT_TRUE(std::isnan(xy_v.getZ()));

  Vertex xyz_v(1.5, -2.5, 9.75);
  EXPECT_DOUBLE_EQ(xyz_v.getX(), 1.5);
  EXPECT_DOUBLE_EQ(xyz_v.getY(), -2.5);
  EXPECT_DOUBLE_EQ(xyz_v.getZ(), 9.75);
  EXPECT_TRUE(xyz_v.getCoordinate().equals3D(Coordinate(1.5, -2.5, 9.75)));

  xyz_v.setZ(-4.25);
  EXPECT_DOUBLE_EQ(xyz_v.getZ(), -4.25);
  xyz_v.setZ(std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::isnan(xyz_v.getZ()));
  EXPECT_DOUBLE_EQ(xyz_v.getX(), 1.5);
  EXPECT_DOUBLE_EQ(xyz_v.getY(), -2.5);

  const Vertex from_coord_3d(Coordinate(4.0, 5.0, 6.0));
  EXPECT_DOUBLE_EQ(from_coord_3d.getX(), 4.0);
  EXPECT_DOUBLE_EQ(from_coord_3d.getY(), 5.0);
  EXPECT_DOUBLE_EQ(from_coord_3d.getZ(), 6.0);

  const Vertex from_coord_2d(Coordinate(4.0, 5.0));
  EXPECT_DOUBLE_EQ(from_coord_2d.getX(), 4.0);
  EXPECT_DOUBLE_EQ(from_coord_2d.getY(), 5.0);
  EXPECT_TRUE(std::isnan(from_coord_2d.getZ()));
}

TEST(VertexTest, Equality2DAndWithTolerance) {
  const Vertex v1(1.0, 2.0, 10.0);
  const Vertex v2(1.0, 2.0, 99.0);
  const Vertex v3(1.0, 2.0);
  const Vertex diff_x(1.5, 2.0, 10.0);
  const Vertex diff_y(1.0, 2.5, 10.0);

  // 2D equals ignores Z.
  EXPECT_TRUE(v1.equals(v2));
  EXPECT_TRUE(v1.equals(v3));
  EXPECT_FALSE(v1.equals(diff_x));
  EXPECT_FALSE(v1.equals(diff_y));

  // Tolerance comparison uses strict `<`.
  const Vertex origin(0.0, 0.0, 0.0);
  const Vertex at_five(3.0, 4.0, 50.0);
  EXPECT_TRUE(origin.equals(at_five, 5.0001));
  EXPECT_FALSE(origin.equals(at_five, 5.0));
  EXPECT_FALSE(origin.equals(at_five, 4.9999));

  // Zero or negative tolerance rejects even identical XY vertices.
  EXPECT_FALSE(v1.equals(v2, 0.0));
  EXPECT_FALSE(v1.equals(v2, -1.0));
  EXPECT_TRUE(v1.equals(v2, 1e-12));
}

TEST(VertexTest, ClassifyCoversAllRelationsAndZInvariance) {
  for (const auto& tc : kClassifyCases) {
    SCOPED_TRACE(tc.name);
    const Vertex p0(tc.p0x, tc.p0y);
    const Vertex p1(tc.p1x, tc.p1y);
    Vertex p2(tc.p2x, tc.p2y);
    EXPECT_EQ(p2.classify(p0, p1), tc.expected);
  }

  // Z coordinates are ignored by classify.
  const Vertex p0(0.0, 0.0, 1.0);
  const Vertex p1(4.0, 0.0, 2.0);
  Vertex z_origin(0.0, 0.0, 99.0);
  Vertex z_dest(4.0, 0.0, -5.0);
  EXPECT_EQ(z_origin.classify(p0, p1), +Vertex::ORIGIN);
  EXPECT_EQ(z_dest.classify(p0, p1), +Vertex::DESTINATION);
}

TEST(VertexTest, VectorOperations) {
  const Vertex u(3.0, 4.0, 7.0);
  const Vertex v(-2.0, 5.0, 11.0);

  EXPECT_DOUBLE_EQ(u.crossProduct(v), 3.0 * 5.0 - 4.0 * (-2.0));
  EXPECT_DOUBLE_EQ(v.crossProduct(u), -u.crossProduct(v));
  EXPECT_DOUBLE_EQ(u.crossProduct(u), 0.0);

  EXPECT_DOUBLE_EQ(u.dot(v), 3.0 * (-2.0) + 4.0 * 5.0);
  EXPECT_DOUBLE_EQ(u.magn(), 5.0);
  EXPECT_DOUBLE_EQ(Vertex(0.0, 0.0).magn(), 0.0);
  EXPECT_DOUBLE_EQ(Vertex(-5.0, 12.0).magn(), 13.0);

  const std::unique_ptr<Vertex> scaled = u.times(-2.5);
  ASSERT_NE(scaled, nullptr);
  EXPECT_DOUBLE_EQ(scaled->getX(), -7.5);
  EXPECT_DOUBLE_EQ(scaled->getY(), -10.0);
  EXPECT_TRUE(std::isnan(scaled->getZ()));

  const std::unique_ptr<Vertex> zero_scaled = u.times(0.0);
  ASSERT_NE(zero_scaled, nullptr);
  EXPECT_DOUBLE_EQ(zero_scaled->getX(), 0.0);
  EXPECT_DOUBLE_EQ(zero_scaled->getY(), 0.0);

  const std::unique_ptr<Vertex> added = u.sum(v);
  ASSERT_NE(added, nullptr);
  EXPECT_DOUBLE_EQ(added->getX(), 1.0);
  EXPECT_DOUBLE_EQ(added->getY(), 9.0);
  EXPECT_TRUE(std::isnan(added->getZ()));

  const std::unique_ptr<Vertex> diff = u.sub(v);
  ASSERT_NE(diff, nullptr);
  EXPECT_DOUBLE_EQ(diff->getX(), 5.0);
  EXPECT_DOUBLE_EQ(diff->getY(), -1.0);
  EXPECT_TRUE(std::isnan(diff->getZ()));

  const std::unique_ptr<Vertex> perp = u.cross();
  ASSERT_NE(perp, nullptr);
  EXPECT_DOUBLE_EQ(perp->getX(), 4.0);
  EXPECT_DOUBLE_EQ(perp->getY(), -3.0);
  EXPECT_TRUE(std::isnan(perp->getZ()));
  EXPECT_DOUBLE_EQ(u.dot(*perp), 0.0);
  EXPECT_DOUBLE_EQ(perp->magn(), u.magn());
}

TEST(VertexTest, IsCCWAndIsInCircle) {
  const Vertex a(0.0, 0.0);
  const Vertex b(4.0, 0.0);
  const Vertex c(0.0, 4.0);

  EXPECT_TRUE(a.isCCW(b, c));
  EXPECT_FALSE(a.isCCW(c, b));
  EXPECT_FALSE(a.isCCW(Vertex(2.0, 2.0), Vertex(4.0, 4.0)));
  EXPECT_FALSE(a.isCCW(a, b));

  // Circumcircle of (0,0), (4,0), (0,4) has center (2,2) and contains (4,4) on
  // its boundary.
  EXPECT_TRUE(Vertex(2.0, 2.0).isInCircle(a, b, c));
  EXPECT_TRUE(Vertex(1.0, 1.0).isInCircle(a, b, c));
  EXPECT_FALSE(Vertex(4.0, 4.0).isInCircle(a, b, c));
  EXPECT_FALSE(Vertex(0.0, 0.0).isInCircle(a, b, c));
  EXPECT_FALSE(Vertex(5.0, 5.0).isInCircle(a, b, c));
  EXPECT_FALSE(Vertex(-1.0, -1.0).isInCircle(a, b, c));

  // Reversed (CW) triangle flips interior/exterior semantics.
  EXPECT_FALSE(Vertex(2.0, 2.0).isInCircle(a, c, b));
  EXPECT_TRUE(Vertex(5.0, 5.0).isInCircle(a, c, b));
}

TEST(VertexTest, LeftOfAndRightOfQuadEdge) {
  std::deque<QuadEdgeQuartet> edges;
  QuadEdge* edge =
      QuadEdge::makeEdge(Vertex(0.0, 0.0), Vertex(4.0, 2.0), edges);
  ASSERT_NE(edge, nullptr);

  const Vertex left_pt(1.0, 3.0);
  EXPECT_TRUE(left_pt.leftOf(*edge));
  EXPECT_FALSE(left_pt.rightOf(*edge));
  EXPECT_FALSE(left_pt.leftOf(edge->sym()));
  EXPECT_TRUE(left_pt.rightOf(edge->sym()));

  const Vertex right_pt(3.0, -1.0);
  EXPECT_FALSE(right_pt.leftOf(*edge));
  EXPECT_TRUE(right_pt.rightOf(*edge));
  EXPECT_TRUE(right_pt.leftOf(edge->sym()));
  EXPECT_FALSE(right_pt.rightOf(edge->sym()));

  // Collinear points (on edge, endpoints, and extensions) are neither left nor
  // right.
  for (const Vertex& collinear : {
           Vertex(0.0, 0.0),
           Vertex(2.0, 1.0),
           Vertex(4.0, 2.0),
           Vertex(-2.0, -1.0),
           Vertex(6.0, 3.0),
       }) {
    EXPECT_FALSE(collinear.leftOf(*edge));
    EXPECT_FALSE(collinear.rightOf(*edge));
  }
}

TEST(VertexTest, LessThanOrdering) {
  const Vertex v_1_10(1.0, 10.0, 99.0);
  const Vertex v_2_0(2.0, 0.0, -5.0);
  const Vertex v_2_5(2.0, 5.0, 1.0);
  const Vertex v_2_5_diff_z(2.0, 5.0, 100.0);

  EXPECT_TRUE(v_1_10 < v_2_0);
  EXPECT_FALSE(v_2_0 < v_1_10);
  EXPECT_TRUE(v_2_0 < v_2_5);
  EXPECT_FALSE(v_2_5 < v_2_0);
  EXPECT_FALSE(v_2_5 < v_2_5);
  EXPECT_FALSE(v_2_5 < v_2_5_diff_z);
  EXPECT_FALSE(v_2_5_diff_z < v_2_5);

  const std::set<Vertex> ordered = {v_2_5, v_1_10, v_2_5_diff_z, v_2_0};
  ASSERT_EQ(ordered.size(), 3u);
  auto it = ordered.begin();
  EXPECT_TRUE(it->equals(v_1_10));
  ++it;
  EXPECT_TRUE(it->equals(v_2_0));
  ++it;
  EXPECT_TRUE(it->equals(v_2_5));
}

TEST(VertexTest, BisectorComputesPerpendicularBisectorLine) {
  auto bisector = GetPrivate(BisectorTag{});

  // Horizontal segment (0,0) -> (4,0): perpendicular bisector is x = 2.
  const std::unique_ptr<HCoordinate> horiz =
      bisector(Vertex(0.0, 0.0), Vertex(4.0, 0.0));
  ASSERT_NE(horiz, nullptr);
  EXPECT_DOUBLE_EQ(horiz->x, -4.0);
  EXPECT_DOUBLE_EQ(horiz->y, 0.0);
  EXPECT_DOUBLE_EQ(horiz->w, 8.0);
  EXPECT_DOUBLE_EQ(horiz->x * 2.0 + horiz->y * 0.0 + horiz->w, 0.0);
  EXPECT_DOUBLE_EQ(horiz->x * 2.0 + horiz->y * 5.0 + horiz->w, 0.0);
  EXPECT_DOUBLE_EQ(horiz->x * 0.0 + horiz->y * 0.0 + horiz->w, 8.0);
  EXPECT_DOUBLE_EQ(horiz->x * 4.0 + horiz->y * 0.0 + horiz->w, -8.0);

  // Vertical segment (1,2) -> (1,8): perpendicular bisector is y = 5.
  const std::unique_ptr<HCoordinate> vert =
      bisector(Vertex(1.0, 2.0), Vertex(1.0, 8.0));
  ASSERT_NE(vert, nullptr);
  EXPECT_DOUBLE_EQ(vert->x, 0.0);
  EXPECT_DOUBLE_EQ(vert->y, -6.0);
  EXPECT_DOUBLE_EQ(vert->w, 30.0);
  EXPECT_DOUBLE_EQ(vert->x * 1.0 + vert->y * 5.0 + vert->w, 0.0);
  EXPECT_DOUBLE_EQ(vert->x * 10.0 + vert->y * 5.0 + vert->w, 0.0);

  // Diagonal segment (1,2) -> (5,6): midpoint (3,4), line x + y = 7.
  const std::unique_ptr<HCoordinate> diag =
      bisector(Vertex(1.0, 2.0), Vertex(5.0, 6.0));
  ASSERT_NE(diag, nullptr);
  EXPECT_DOUBLE_EQ(diag->x, -4.0);
  EXPECT_DOUBLE_EQ(diag->y, -4.0);
  EXPECT_DOUBLE_EQ(diag->w, 28.0);
  EXPECT_DOUBLE_EQ(diag->x * 3.0 + diag->y * 4.0 + diag->w, 0.0);
  EXPECT_DOUBLE_EQ(diag->x * 7.0 + diag->y * 0.0 + diag->w, 0.0);
  EXPECT_DOUBLE_EQ(diag->x * -1.0 + diag->y * 8.0 + diag->w, 0.0);

  // Intersecting two chord bisectors gives the triangle circumcenter.
  const std::unique_ptr<HCoordinate> chord_bc =
      bisector(Vertex(4.0, 0.0), Vertex(0.0, 4.0));
  ASSERT_NE(chord_bc, nullptr);
  const HCoordinate center(*horiz, *chord_bc);
  EXPECT_DOUBLE_EQ(center.getX(), 2.0);
  EXPECT_DOUBLE_EQ(center.getY(), 2.0);

  // Coincident vertices produce the zero homogeneous line (0, 0, 0).
  const std::unique_ptr<HCoordinate> degenerate =
      bisector(Vertex(3.0, -2.0), Vertex(3.0, -2.0));
  ASSERT_NE(degenerate, nullptr);
  EXPECT_DOUBLE_EQ(degenerate->x, 0.0);
  EXPECT_DOUBLE_EQ(degenerate->y, 0.0);
  EXPECT_DOUBLE_EQ(degenerate->w, 0.0);
}

TEST(VertexTest, MidPointAveragesXYZ) {
  auto mid_fn = GetPrivate(MidPointTag{});

  Vertex v1(1.0, -3.0, 5.0);
  Vertex v2(7.0, 5.0, -1.0);
  const std::unique_ptr<Vertex> mid = (v1.*mid_fn)(v2);
  ASSERT_NE(mid, nullptr);
  EXPECT_DOUBLE_EQ(mid->getX(), 4.0);
  EXPECT_DOUBLE_EQ(mid->getY(), 1.0);
  EXPECT_DOUBLE_EQ(mid->getZ(), 2.0);

  const std::unique_ptr<Vertex> mid_rev = (v2.*mid_fn)(v1);
  ASSERT_NE(mid_rev, nullptr);
  EXPECT_DOUBLE_EQ(mid_rev->getX(), 4.0);
  EXPECT_DOUBLE_EQ(mid_rev->getY(), 1.0);
  EXPECT_DOUBLE_EQ(mid_rev->getZ(), 2.0);

  const std::unique_ptr<Vertex> self_mid = (v1.*mid_fn)(v1);
  ASSERT_NE(self_mid, nullptr);
  EXPECT_DOUBLE_EQ(self_mid->getX(), 1.0);
  EXPECT_DOUBLE_EQ(self_mid->getY(), -3.0);
  EXPECT_DOUBLE_EQ(self_mid->getZ(), 5.0);

  // Missing Z yields NaN Z on the midpoint.
  Vertex a2d(0.0, 2.0);
  const Vertex b2d(4.0, 6.0);
  const std::unique_ptr<Vertex> mid2d = (a2d.*mid_fn)(b2d);
  ASSERT_NE(mid2d, nullptr);
  EXPECT_DOUBLE_EQ(mid2d->getX(), 2.0);
  EXPECT_DOUBLE_EQ(mid2d->getY(), 4.0);
  EXPECT_TRUE(std::isnan(mid2d->getZ()));

  const std::unique_ptr<Vertex> mid_mixed = (v1.*mid_fn)(b2d);
  ASSERT_NE(mid_mixed, nullptr);
  EXPECT_DOUBLE_EQ(mid_mixed->getX(), 2.5);
  EXPECT_DOUBLE_EQ(mid_mixed->getY(), 1.5);
  EXPECT_TRUE(std::isnan(mid_mixed->getZ()));
}

TEST(VertexTest, CircleCenterComputesCircumcenterAndHandlesCollinear) {
  auto circle_center = GetPrivate(CircleCenterTag{});

  const Vertex right_a(0.0, 0.0, 10.0);
  const Vertex right_b(4.0, 0.0, 20.0);
  const Vertex right_c(0.0, 4.0, 30.0);
  const std::unique_ptr<Vertex> right_cc =
      (right_a.*circle_center)(right_b, right_c);
  ASSERT_NE(right_cc, nullptr);
  EXPECT_DOUBLE_EQ(right_cc->getX(), 2.0);
  EXPECT_DOUBLE_EQ(right_cc->getY(), 2.0);
  EXPECT_TRUE(std::isnan(right_cc->getZ()));

  // Non-axis-aligned acute triangle; circumcenter is invariant to vertex order.
  const Vertex tri_a(1.0, 2.0);
  const Vertex tri_b(5.0, 2.0);
  const Vertex tri_c(3.0, 6.0);
  const std::array<std::unique_ptr<Vertex>, 3> centers = {
      (tri_a.*circle_center)(tri_b, tri_c),
      (tri_b.*circle_center)(tri_c, tri_a),
      (tri_c.*circle_center)(tri_a, tri_b),
  };
  for (const std::unique_ptr<Vertex>& cc : centers) {
    ASSERT_NE(cc, nullptr);
    EXPECT_NEAR(cc->getX(), 3.0, 1e-12);
    EXPECT_NEAR(cc->getY(), 3.5, 1e-12);
  }

  // Obtuse triangle whose circumcenter lies outside the triangle.
  const Vertex obt_a(-2.0, 0.0);
  const Vertex obt_b(2.0, 0.0);
  const Vertex obt_c(0.0, 1.0);
  const std::unique_ptr<Vertex> obt_cc = (obt_a.*circle_center)(obt_b, obt_c);
  ASSERT_NE(obt_cc, nullptr);
  EXPECT_NEAR(obt_cc->getX(), 0.0, 1e-12);
  EXPECT_NEAR(obt_cc->getY(), -1.5, 1e-12);

  // Collinear and coincident inputs trigger NotRepresentableException and
  // return nullptr.
  EXPECT_EQ(
      (Vertex(0.0, 0.0).*circle_center)(Vertex(1.0, 0.0), Vertex(2.0, 0.0)),
      nullptr);
  EXPECT_EQ(
      (Vertex(0.0, 0.0).*circle_center)(Vertex(1.0, 1.0), Vertex(2.0, 2.0)),
      nullptr);
  EXPECT_EQ(
      (Vertex(1.0, 2.0).*circle_center)(Vertex(1.0, 2.0), Vertex(4.0, 5.0)),
      nullptr);
}

TEST(VertexTest, CircumRadiusRatioComputesRatioForAllShortestEdgeCases) {
  auto ratio_fn = GetPrivate(CircumRadiusRatioTag{});

  // Edge a-b is shortest (neither subsequent edge update triggers).
  Vertex ab_a(0.0, 0.0);
  const Vertex ab_b(1.0, 0.0);
  const Vertex ab_c(0.5, 5.0);
  EXPECT_NEAR((ab_a.*ratio_fn)(ab_b, ab_c), 2.525, 1e-12);

  // Edge b-c is shortest (first update triggers, second does not).
  Vertex bc_a(0.0, 0.0);
  const Vertex bc_b(4.0, 0.0);
  const Vertex bc_c(4.0, 3.0);
  EXPECT_NEAR((bc_a.*ratio_fn)(bc_b, bc_c), 2.5 / 3.0, 1e-12);

  // Edge c-a is shortest while b-c > a-b (first update false, second true).
  Vertex ca_a(0.0, 0.0);
  const Vertex ca_b(4.0, 0.0);
  const Vertex ca_c(0.0, 3.0);
  EXPECT_NEAR((ca_a.*ratio_fn)(ca_b, ca_c), 2.5 / 3.0, 1e-12);

  // Strictly decreasing edge lengths (both updates trigger: ca < bc < ab).
  Vertex both_a(0.0, 0.0);
  const Vertex both_b(5.0, 0.0);
  const Vertex both_c(1.0, 1.0);
  EXPECT_NEAR((both_a.*ratio_fn)(both_b, both_c), std::sqrt(4.25), 1e-12);

  // Equilateral triangle has circumradius / side == 1 / sqrt(3).
  Vertex eq_a(0.0, 0.0);
  const Vertex eq_b(2.0, 0.0);
  const Vertex eq_c(1.0, std::sqrt(3.0));
  EXPECT_NEAR((eq_a.*ratio_fn)(eq_b, eq_c), 1.0 / std::sqrt(3.0), 1e-12);
}

TEST(VertexTest, InterpolateZValueOnTriangle) {
  auto interp_value = GetPrivate(InterpolateZValueTag{});

  const Vertex v0(0.0, 0.0, 10.0);
  const Vertex v1(4.0, 0.0, 20.0);
  const Vertex v2(0.0, 4.0, 30.0);

  EXPECT_DOUBLE_EQ((v0.*interp_value)(v0, v1, v2), 10.0);
  EXPECT_DOUBLE_EQ((v1.*interp_value)(v0, v1, v2), 20.0);
  EXPECT_DOUBLE_EQ((v2.*interp_value)(v0, v1, v2), 30.0);
  EXPECT_DOUBLE_EQ((Vertex(2.0, 2.0).*interp_value)(v0, v1, v2), 25.0);
  // Target vertex's own Z is ignored.
  EXPECT_DOUBLE_EQ((Vertex(1.0, 2.0, 999.0).*interp_value)(v0, v1, v2), 22.5);
  EXPECT_NEAR((Vertex(4.0 / 3.0, 4.0 / 3.0).*interp_value)(v0, v1, v2), 20.0,
              1e-12);

  // Non-axis-aligned triangle.
  const Vertex s0(1.0, 2.0, 10.0);
  const Vertex s1(5.0, 3.0, 18.0);
  const Vertex s2(2.0, 6.0, 22.0);
  EXPECT_NEAR((Vertex(3.0, 4.0).*interp_value)(s0, s1, s2), 18.0, 1e-12);

  // Missing Z on a triangle vertex yields NaN.
  const Vertex no_z_v2(0.0, 4.0);
  EXPECT_TRUE(std::isnan((Vertex(1.0, 1.0).*interp_value)(v0, v1, no_z_v2)));
}

TEST(VertexTest, InterpolateZStaticTriangle) {
  auto interp_tri = GetPrivate(InterpolateZTriangleTag{});

  const Coordinate v0(0.0, 0.0, 10.0);
  const Coordinate v1(4.0, 0.0, 20.0);
  const Coordinate v2(0.0, 4.0, 30.0);

  EXPECT_DOUBLE_EQ(interp_tri(v0, v0, v1, v2), 10.0);
  EXPECT_DOUBLE_EQ(interp_tri(v1, v0, v1, v2), 20.0);
  EXPECT_DOUBLE_EQ(interp_tri(v2, v0, v1, v2), 30.0);
  EXPECT_DOUBLE_EQ(interp_tri(Coordinate(1.0, 2.0, -50.0), v0, v1, v2), 22.5);
  EXPECT_NEAR(interp_tri(Coordinate(4.0 / 3.0, 4.0 / 3.0), v0, v1, v2), 20.0,
              1e-12);
  EXPECT_DOUBLE_EQ(interp_tri(Coordinate(8.0, 2.0), v0, v1, v2), 40.0);

  const Coordinate s0(1.0, 2.0, 10.0);
  const Coordinate s1(5.0, 3.0, 18.0);
  const Coordinate s2(2.0, 6.0, 22.0);
  EXPECT_NEAR(interp_tri(Coordinate(3.0, 4.0), s0, s1, s2), 18.0, 1e-12);

  // Negative Z values.
  EXPECT_DOUBLE_EQ(
      interp_tri(Coordinate(1.0, 1.0), Coordinate(0.0, 0.0, -5.0),
                 Coordinate(2.0, 0.0, 5.0), Coordinate(0.0, 2.0, -15.0)),
      -5.0);

  // Missing Z on a triangle vertex yields NaN.
  EXPECT_TRUE(std::isnan(
      interp_tri(Coordinate(1.0, 1.0), v0, Coordinate(4.0, 0.0), v2)));
}

TEST(VertexTest, InterpolateZStaticSegment) {
  auto interp_seg = GetPrivate(InterpolateZSegmentTag{});

  const Coordinate p0(0.0, 0.0, 10.0);
  const Coordinate p1(3.0, 4.0, 30.0);

  EXPECT_DOUBLE_EQ(interp_seg(p0, p0, p1), 10.0);
  EXPECT_DOUBLE_EQ(interp_seg(p1, p0, p1), 30.0);
  EXPECT_DOUBLE_EQ(interp_seg(Coordinate(1.5, 2.0, -999.0), p0, p1), 20.0);
  EXPECT_DOUBLE_EQ(interp_seg(Coordinate(0.75, 1.0), p0, p1), 15.0);
  EXPECT_DOUBLE_EQ(interp_seg(Coordinate(6.0, 8.0), p0, p1), 50.0);

  // Decreasing Z along a vertical segment.
  EXPECT_DOUBLE_EQ(interp_seg(Coordinate(1.0, 4.0), Coordinate(1.0, 2.0, 50.0),
                              Coordinate(1.0, 10.0, 10.0)),
                   40.0);

  // Missing Z on segment endpoint yields NaN.
  EXPECT_TRUE(
      std::isnan(interp_seg(Coordinate(1.5, 2.0), Coordinate(0.0, 0.0), p1)));
  EXPECT_TRUE(
      std::isnan(interp_seg(Coordinate(1.5, 2.0), p0, Coordinate(3.0, 4.0))));
}

}  // namespace
}  // namespace geos_google
