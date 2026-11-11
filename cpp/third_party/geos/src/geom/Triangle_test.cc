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

// Unit tests for third_party/geos/src/geom/Triangle.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Triangle.cpp

#include "third_party/geos/include/geos/geom/Triangle.h"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Triangle;

struct Point2D {
  double x;
  double y;
};

CoordinateXY ToCoord(const Point2D& p) { return CoordinateXY(p.x, p.y); }

TEST(TriangleTest, ConstructorExposesVertices) {
  const CoordinateXY a(1.0, 2.0);
  const CoordinateXY b(4.0, 2.0);
  const CoordinateXY c(2.5, 6.0);
  Triangle tri(a, b, c);
  EXPECT_EQ(tri.p0, a);
  EXPECT_EQ(tri.p1, b);
  EXPECT_EQ(tri.p2, c);
}

TEST(TriangleTest, IsIsoceles) {
  struct IsoscelesCase {
    std::string_view name;
    Point2D p0;
    Point2D p1;
    Point2D p2;
    bool expected;
  };

  constexpr auto kCases = std::to_array<IsoscelesCase>({
      {
          .name = "equal_sides_p1p2_and_p0p2",
          .p0 = {0.0, 0.0},
          .p1 = {2.0, 0.0},
          .p2 = {1.0, 2.0},
          .expected = true,
      },
      {
          .name = "equal_sides_p0p2_and_p0p1",
          .p0 = {0.0, 0.0},
          .p1 = {3.0, 0.0},
          .p2 = {0.0, 3.0},
          .expected = true,
      },
      {
          .name = "equal_sides_p0p1_and_p1p2",
          .p0 = {0.0, 0.0},
          .p1 = {4.0, 0.0},
          .p2 = {4.0, 4.0},
          .expected = true,
      },
      {
          .name = "scalene",
          .p0 = {0.0, 0.0},
          .p1 = {4.0, 0.0},
          .p2 = {1.0, 2.0},
          .expected = false,
      },
      {
          .name = "degenerate_coincident",
          .p0 = {1.0, 2.0},
          .p1 = {1.0, 2.0},
          .p2 = {1.0, 2.0},
          .expected = true,
      },
      {
          .name = "degenerate_collinear_unequal",
          .p0 = {0.0, 0.0},
          .p1 = {1.0, 0.0},
          .p2 = {3.0, 0.0},
          .expected = false,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    Triangle tri(ToCoord(tc.p0), ToCoord(tc.p1), ToCoord(tc.p2));
    EXPECT_EQ(tri.isIsoceles(), tc.expected);
  }
}

TEST(TriangleTest, InCentre) {
  struct InCentreCase {
    std::string_view name;
    Point2D p0;
    Point2D p1;
    Point2D p2;
    Point2D expected;
  };

  constexpr auto kCases = std::to_array<InCentreCase>({
      {
          .name = "right_3_4_5",
          .p0 = {0.0, 0.0},
          .p1 = {3.0, 0.0},
          .p2 = {0.0, 4.0},
          .expected = {1.0, 1.0},
      },
      {
          .name = "shifted_negative_3_4_5",
          .p0 = {-3.0, -1.0},
          .p1 = {1.0, -1.0},
          .p2 = {-3.0, 2.0},
          .expected = {-2.0, 0.0},
      },
      {
          .name = "symmetric_isosceles_6_5_5",
          .p0 = {0.0, 0.0},
          .p1 = {6.0, 0.0},
          .p2 = {3.0, 4.0},
          .expected = {3.0, 1.5},
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY a = ToCoord(tc.p0);
    const CoordinateXY b = ToCoord(tc.p1);
    const CoordinateXY c = ToCoord(tc.p2);

    Triangle tri(a, b, c);
    CoordinateXY instance_result;
    tri.inCentre(instance_result);
    EXPECT_DOUBLE_EQ(instance_result.x, tc.expected.x);
    EXPECT_DOUBLE_EQ(instance_result.y, tc.expected.y);
    EXPECT_TRUE(tri.intersects(instance_result));

    const CoordinateXY static_result = Triangle::inCentre(a, b, c);
    EXPECT_DOUBLE_EQ(static_result.x, tc.expected.x);
    EXPECT_DOUBLE_EQ(static_result.y, tc.expected.y);
  }
}

TEST(TriangleTest, CircumcentreAndCircumcentreDD) {
  struct CircumcentreCase {
    std::string_view name;
    Point2D p0;
    Point2D p1;
    Point2D p2;
    Point2D expected_centre;
    double expected_radius;
    bool inside_triangle;
  };

  const double sqrt5 = std::sqrt(5.0);
  const double sqrt13 = std::sqrt(13.0);

  const auto kCases = std::to_array<CircumcentreCase>({
      {
          .name = "right_triangle_midpoint_of_hypotenuse",
          .p0 = {0.0, 0.0},
          .p1 = {4.0, 0.0},
          .p2 = {0.0, 6.0},
          .expected_centre = {2.0, 3.0},
          .expected_radius = sqrt13,
          .inside_triangle = true,
      },
      {
          .name = "acute_isosceles",
          .p0 = {0.0, 0.0},
          .p1 = {6.0, 0.0},
          .p2 = {3.0, 4.0},
          .expected_centre = {3.0, 0.875},
          .expected_radius = 3.125,
          .inside_triangle = true,
      },
      {
          .name = "obtuse_circumcentre_outside",
          .p0 = {0.0, 0.0},
          .p1 = {2.0, 0.0},
          .p2 = {3.0, 1.0},
          .expected_centre = {1.0, 2.0},
          .expected_radius = sqrt5,
          .inside_triangle = false,
      },
      {
          .name = "large_translated_coordinates",
          .p0 = {1e9 + 0.0, 1e9 + 0.0},
          .p1 = {1e9 + 6.0, 1e9 + 0.0},
          .p2 = {1e9 + 3.0, 1e9 + 4.0},
          .expected_centre = {1e9 + 3.0, 1e9 + 0.875},
          .expected_radius = 3.125,
          .inside_triangle = true,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY a = ToCoord(tc.p0);
    const CoordinateXY b = ToCoord(tc.p1);
    const CoordinateXY c = ToCoord(tc.p2);
    Triangle tri(a, b, c);

    CoordinateXY cc_instance;
    tri.circumcentre(cc_instance);
    EXPECT_DOUBLE_EQ(cc_instance.x, tc.expected_centre.x);
    EXPECT_DOUBLE_EQ(cc_instance.y, tc.expected_centre.y);

    const CoordinateXY cc_static = Triangle::circumcentre(a, b, c);
    EXPECT_DOUBLE_EQ(cc_static.x, tc.expected_centre.x);
    EXPECT_DOUBLE_EQ(cc_static.y, tc.expected_centre.y);

    CoordinateXY cc_dd;
    tri.circumcentreDD(cc_dd);
    EXPECT_DOUBLE_EQ(cc_dd.x, tc.expected_centre.x);
    EXPECT_DOUBLE_EQ(cc_dd.y, tc.expected_centre.y);

    EXPECT_NEAR(cc_instance.distance(a), tc.expected_radius, 1e-9);
    EXPECT_NEAR(cc_instance.distance(b), tc.expected_radius, 1e-9);
    EXPECT_NEAR(cc_instance.distance(c), tc.expected_radius, 1e-9);
    EXPECT_EQ(tri.intersects(cc_instance), tc.inside_triangle);
  }
}

TEST(TriangleTest, Circumradius) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(3.0, 0.0);
  const CoordinateXY c(0.0, 4.0);
  EXPECT_DOUBLE_EQ(Triangle::circumradius(a, b, c), 2.5);
  EXPECT_DOUBLE_EQ(Triangle(a, b, c).circumradius(), 2.5);

  // Collinear triangle has zero area -> infinite circumradius.
  const CoordinateXY col0(0.0, 0.0);
  const CoordinateXY col1(1.0, 0.0);
  const CoordinateXY col2(2.0, 0.0);
  const double col_radius = Triangle::circumradius(col0, col1, col2);
  EXPECT_TRUE(std::isinf(col_radius));
  EXPECT_GT(col_radius, 0.0);
  EXPECT_EQ(Triangle(col0, col1, col2).circumradius(),
            std::numeric_limits<double>::infinity());

  // Coincident vertices also have zero area -> infinite circumradius.
  const CoordinateXY pt(2.0, 3.0);
  EXPECT_EQ(Triangle::circumradius(pt, pt, pt),
            std::numeric_limits<double>::infinity());
}

TEST(TriangleTest, IsAcute) {
  struct AcuteCase {
    std::string_view name;
    Point2D p0;
    Point2D p1;
    Point2D p2;
    bool expected;
  };

  constexpr auto kCases = std::to_array<AcuteCase>({
      {
          .name = "all_angles_acute",
          .p0 = {0.0, 0.0},
          .p1 = {4.0, 0.0},
          .p2 = {2.0, 3.0},
          .expected = true,
      },
      {
          .name = "right_angle_at_p1",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {0.0, 1.0},
          .expected = false,
      },
      {
          .name = "right_angle_at_p2",
          .p0 = {0.0, 1.0},
          .p1 = {1.0, 0.0},
          .p2 = {0.0, 0.0},
          .expected = false,
      },
      {
          .name = "right_angle_at_p0",
          .p0 = {0.0, 0.0},
          .p1 = {0.0, 1.0},
          .p2 = {1.0, 0.0},
          .expected = false,
      },
      {
          .name = "obtuse_at_p1",
          .p0 = {1.0, 0.0},
          .p1 = {0.0, 0.0},
          .p2 = {-1.0, 1.0},
          .expected = false,
      },
      {
          .name = "obtuse_at_p2",
          .p0 = {-1.0, 1.0},
          .p1 = {1.0, 0.0},
          .p2 = {0.0, 0.0},
          .expected = false,
      },
      {
          .name = "obtuse_at_p0",
          .p0 = {0.0, 0.0},
          .p1 = {-1.0, 1.0},
          .p2 = {1.0, 0.0},
          .expected = false,
      },
      {
          .name = "degenerate_collinear",
          .p0 = {0.0, 0.0},
          .p1 = {1.0, 0.0},
          .p2 = {2.0, 0.0},
          .expected = false,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY a = ToCoord(tc.p0);
    const CoordinateXY b = ToCoord(tc.p1);
    const CoordinateXY c = ToCoord(tc.p2);
    EXPECT_EQ(Triangle::isAcute(a, b, c), tc.expected);
    EXPECT_EQ(Triangle(a, b, c).isAcute(), tc.expected);
  }
}

TEST(TriangleTest, IsCCW) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(1.0, 0.0);
  const CoordinateXY c(0.0, 1.0);

  EXPECT_TRUE(Triangle::isCCW(a, b, c));
  EXPECT_TRUE(Triangle(a, b, c).isCCW());

  EXPECT_FALSE(Triangle::isCCW(a, c, b));
  EXPECT_FALSE(Triangle(a, c, b).isCCW());

  const CoordinateXY col0(0.0, 0.0);
  const CoordinateXY col1(1.0, 0.0);
  const CoordinateXY col2(2.0, 0.0);
  EXPECT_FALSE(Triangle::isCCW(col0, col1, col2));
  EXPECT_FALSE(Triangle(col0, col1, col2).isCCW());
}

TEST(TriangleTest, IntersectsPoint) {
  struct IntersectsCase {
    std::string_view name;
    Point2D pt;
    bool expected;
  };

  // Triangle with vertices (0, 0), (4, 0), (0, 4).
  constexpr auto kCases = std::to_array<IntersectsCase>({
      {.name = "strictly_interior", .pt = {1.0, 1.0}, .expected = true},
      {.name = "vertex_a", .pt = {0.0, 0.0}, .expected = true},
      {.name = "vertex_b", .pt = {4.0, 0.0}, .expected = true},
      {.name = "vertex_c", .pt = {0.0, 4.0}, .expected = true},
      {.name = "midpoint_ab", .pt = {2.0, 0.0}, .expected = true},
      {.name = "midpoint_bc", .pt = {2.0, 2.0}, .expected = true},
      {.name = "midpoint_ca", .pt = {0.0, 2.0}, .expected = true},
      {.name = "outside_bottom", .pt = {1.0, -1.0}, .expected = false},
      {.name = "outside_hypotenuse", .pt = {3.0, 3.0}, .expected = false},
      {.name = "outside_left", .pt = {-1.0, 1.0}, .expected = false},
      {.name = "far_outside", .pt = {-5.0, -5.0}, .expected = false},
  });

  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(4.0, 0.0);
  const CoordinateXY c(0.0, 4.0);
  Triangle ccw_tri(a, b, c);
  Triangle cw_tri(a, c, b);

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY p = ToCoord(tc.pt);
    EXPECT_EQ(Triangle::intersects(a, b, c, p), tc.expected);
    EXPECT_EQ(ccw_tri.intersects(p), tc.expected);
    EXPECT_EQ(Triangle::intersects(a, c, b, p), tc.expected);
    EXPECT_EQ(cw_tri.intersects(p), tc.expected);
  }
}

TEST(TriangleTest, LengthAndArea) {
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(3.0, 0.0);
  const CoordinateXY c(0.0, 4.0);
  EXPECT_DOUBLE_EQ(Triangle::length(a, b, c), 12.0);
  EXPECT_DOUBLE_EQ(Triangle(a, b, c).length(), 12.0);
  EXPECT_DOUBLE_EQ(Triangle::area(a, b, c), 6.0);
  EXPECT_DOUBLE_EQ(Triangle(a, b, c).area(), 6.0);

  // Clockwise orientation preserves non-negative area.
  EXPECT_DOUBLE_EQ(Triangle::area(a, c, b), 6.0);
  EXPECT_DOUBLE_EQ(Triangle(a, c, b).area(), 6.0);

  // Shifted arbitrary triangle: (-2, -3), (4, -1), (1, 5) -> area 21.
  const CoordinateXY s0(-2.0, -3.0);
  const CoordinateXY s1(4.0, -1.0);
  const CoordinateXY s2(1.0, 5.0);
  EXPECT_DOUBLE_EQ(Triangle::area(s0, s1, s2), 21.0);
  EXPECT_DOUBLE_EQ(Triangle(s0, s1, s2).area(), 21.0);

  // Collinear triangle has zero area and perimeter equal to 2 * span.
  const CoordinateXY col0(0.0, 0.0);
  const CoordinateXY col1(1.0, 0.0);
  const CoordinateXY col2(4.0, 0.0);
  EXPECT_DOUBLE_EQ(Triangle::area(col0, col1, col2), 0.0);
  EXPECT_DOUBLE_EQ(Triangle(col0, col1, col2).area(), 0.0);
  EXPECT_DOUBLE_EQ(Triangle::length(col0, col1, col2), 8.0);
  EXPECT_DOUBLE_EQ(Triangle(col0, col1, col2).length(), 8.0);

  // Coincident vertices -> zero length and zero area.
  const CoordinateXY pt(5.0, 5.0);
  EXPECT_DOUBLE_EQ(Triangle::length(pt, pt, pt), 0.0);
  EXPECT_DOUBLE_EQ(Triangle::area(pt, pt, pt), 0.0);
}

TEST(TriangleTest, LongestSideLength) {
  struct LongestSideCase {
    std::string_view name;
    Point2D a;
    Point2D b;
    Point2D c;
    double expected;
  };

  constexpr auto kCases = std::to_array<LongestSideCase>({
      {
          .name = "ab_longest",
          .a = {0.0, 0.0},
          .b = {5.0, 0.0},
          .c = {2.0, 1.0},
          .expected = 5.0,
      },
      {
          .name = "bc_longest",
          .a = {2.0, 1.0},
          .b = {0.0, 0.0},
          .c = {5.0, 0.0},
          .expected = 5.0,
      },
      {
          .name = "ca_longest_after_bc_larger_than_ab",
          .a = {0.0, 0.0},
          .b = {2.0, 1.0},
          .c = {5.0, 0.0},
          .expected = 5.0,
      },
      {
          .name = "ca_longest_with_ab_larger_than_bc",
          .a = {5.0, 0.0},
          .b = {2.0, 1.0},
          .c = {0.0, 0.0},
          .expected = 5.0,
      },
      {
          .name = "degenerate_zero",
          .a = {1.0, 1.0},
          .b = {1.0, 1.0},
          .c = {1.0, 1.0},
          .expected = 0.0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_DOUBLE_EQ(Triangle::longestSideLength(ToCoord(tc.a), ToCoord(tc.b),
                                                 ToCoord(tc.c)),
                     tc.expected);
  }
}

}  // namespace
}  // namespace geos_google
