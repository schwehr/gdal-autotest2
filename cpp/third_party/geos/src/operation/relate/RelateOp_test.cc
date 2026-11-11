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

// Unit tests for third_party/geos/src/operation/relate/RelateOp.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for
// UnsupportedOperationException.
// - Each test allocates its own GeometryFactory::create() for isolation.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relate/RelateOp.cpp

#include "third_party/geos/include/geos/operation/relate/RelateOp.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/GeometryGraphOperation.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::GeometryGraphOperation;
using geos::operation::relate::RelateOp;
using geos::util::UnsupportedOperationException;

struct RelateCase {
  std::string_view name;
  std::string_view wkt_a;
  std::string_view wkt_b;
  std::string_view expected_im;
};

constexpr auto kDefaultRuleCases = std::to_array<RelateCase>({
    {
        .name = "point_point_equal",
        .wkt_a = "POINT (1 2)",
        .wkt_b = "POINT (1 2)",
        .expected_im = "0FFFFFFF2",
    },
    {
        .name = "point_point_disjoint",
        .wkt_a = "POINT (1 2)",
        .wkt_b = "POINT (3 4)",
        .expected_im = "FF0FFF0F2",
    },
    {
        .name = "point_on_line_interior",
        .wkt_a = "POINT (5 0)",
        .wkt_b = "LINESTRING (0 0, 10 0)",
        .expected_im = "0FFFFF102",
    },
    {
        .name = "point_on_line_endpoint",
        .wkt_a = "POINT (0 0)",
        .wkt_b = "LINESTRING (0 0, 10 0)",
        .expected_im = "F0FFFF102",
    },
    {
        .name = "point_disjoint_from_line",
        .wkt_a = "POINT (20 20)",
        .wkt_b = "LINESTRING (0 0, 10 0)",
        .expected_im = "FF0FFF102",
    },
    {
        .name = "point_at_closed_line_seam_mod2",
        .wkt_a = "POINT (0 0)",
        .wkt_b = "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
        .expected_im = "0FFFFF1F2",
    },
    {
        .name = "point_inside_polygon",
        .wkt_a = "POINT (5 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "0FFFFF212",
    },
    {
        .name = "point_on_polygon_boundary",
        .wkt_a = "POINT (0 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "F0FFFF212",
    },
    {
        .name = "point_inside_polygon_hole",
        .wkt_a = "POINT (5 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, "
                 "2 2))",
        .expected_im = "FF0FFF212",
    },
    {
        .name = "point_on_polygon_hole_boundary",
        .wkt_a = "POINT (2 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, "
                 "2 2))",
        .expected_im = "F0FFFF212",
    },
    {
        .name = "point_in_concavity_with_overlapping_envelope",
        .wkt_a = "POINT (2 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 8, 8 8, 8 2, 0 2, 0 0))",
        .expected_im = "FF0FFF212",
    },
    {
        .name = "multipoint_straddling_polygon",
        .wkt_a = "MULTIPOINT ((5 5), (0 5), (20 20))",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "000FFF212",
    },
    {
        .name = "lines_crossing",
        .wkt_a = "LINESTRING (0 5, 10 5)",
        .wkt_b = "LINESTRING (5 0, 5 10)",
        .expected_im = "0F1FF0102",
    },
    {
        .name = "lines_touch_endpoints",
        .wkt_a = "LINESTRING (0 0, 5 0)",
        .wkt_b = "LINESTRING (5 0, 10 0)",
        .expected_im = "FF1F00102",
    },
    {
        .name = "lines_t_junction",
        .wkt_a = "LINESTRING (0 0, 10 0)",
        .wkt_b = "LINESTRING (5 0, 5 5)",
        .expected_im = "F01FF0102",
    },
    {
        .name = "lines_collinear_overlap",
        .wkt_a = "LINESTRING (0 0, 6 0)",
        .wkt_b = "LINESTRING (4 0, 10 0)",
        .expected_im = "1010F0102",
    },
    {
        .name = "lines_equal",
        .wkt_a = "LINESTRING (0 0, 10 0)",
        .wkt_b = "LINESTRING (0 0, 10 0)",
        .expected_im = "1FFF0FFF2",
    },
    {
        .name = "line_within_line",
        .wkt_a = "LINESTRING (2 0, 8 0)",
        .wkt_b = "LINESTRING (0 0, 10 0)",
        .expected_im = "1FF0FF102",
    },
    {
        .name = "lines_disjoint",
        .wkt_a = "LINESTRING (0 0, 10 0)",
        .wkt_b = "LINESTRING (0 5, 10 5)",
        .expected_im = "FF1FF0102",
    },
    {
        .name = "line_inside_polygon",
        .wkt_a = "LINESTRING (2 2, 8 8)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "1FF0FF212",
    },
    {
        .name = "line_crosses_through_polygon",
        .wkt_a = "LINESTRING (-2 5, 12 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "101FF0212",
    },
    {
        .name = "line_starts_inside_polygon_exits",
        .wkt_a = "LINESTRING (5 5, 12 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "1010F0212",
    },
    {
        .name = "line_along_polygon_boundary",
        .wkt_a = "LINESTRING (2 0, 8 0)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "F1FF0F212",
    },
    {
        .name = "line_touches_polygon_at_endpoint",
        .wkt_a = "LINESTRING (-2 5, 0 5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "FF1F00212",
    },
    {
        .name = "line_grazes_polygon_corner_in_interior",
        .wkt_a = "LINESTRING (-5 5, 5 -5)",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "F01FF0212",
    },
    {
        .name = "line_disjoint_from_polygon_overlapping_envelope",
        .wkt_a = "LINESTRING (0 0, 10 10)",
        .wkt_b = "POLYGON ((6 0, 9 0, 9 3, 6 3, 6 0))",
        .expected_im = "FF1FF0212",
    },
    {
        .name = "polygons_equal",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "2FFF1FFF2",
    },
    {
        .name = "polygons_disjoint",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))",
        .expected_im = "FF2FF1212",
    },
    {
        .name = "polygons_touch_edge",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
        .expected_im = "FF2F11212",
    },
    {
        .name = "polygons_touch_vertex",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((10 10, 20 10, 20 20, 10 20, 10 10))",
        .expected_im = "FF2F01212",
    },
    {
        .name = "polygons_overlap",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))",
        .expected_im = "212101212",
    },
    {
        .name = "polygon_contains_strictly",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
        .expected_im = "212FF1FF2",
    },
    {
        .name = "polygon_covers_touching_edge",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .wkt_b = "POLYGON ((0 2, 8 2, 8 8, 0 8, 0 2))",
        .expected_im = "212F11FF2",
    },
    {
        .name = "polygon_with_hole_and_polygon_filling_hole",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, "
                 "2 2))",
        .wkt_b = "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
        .expected_im = "FF2F112F2",
    },
    {
        .name = "polygon_with_hole_and_polygon_disjoint_in_hole",
        .wkt_a = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, "
                 "2 2))",
        .wkt_b = "POLYGON ((3 3, 7 3, 7 7, 3 7, 3 3))",
        .expected_im = "FF2FF1212",
    },
    {
        .name = "multilinestring_valence2_node_is_interior_mod2",
        .wkt_a = "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))",
        .wkt_b = "POINT (5 0)",
        .expected_im = "0F1FF0FF2",
    },
    {
        .name = "multilinestring_valence3_node_is_boundary_mod2",
        .wkt_a = "MULTILINESTRING ((0 0, 5 0), (5 0, 10 0), (5 0, 5 5))",
        .wkt_b = "POINT (5 0)",
        .expected_im = "FF10F0FF2",
    },
    {
        .name = "geometrycollection_with_polygon",
        .wkt_a = "GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (0 0, 2 0))",
        .wkt_b = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
        .expected_im = "01FF0F212",
    },
    {
        .name = "empty_both",
        .wkt_a = "POINT EMPTY",
        .wkt_b = "POINT EMPTY",
        .expected_im = "FFFFFFFF2",
    },
    {
        .name = "empty_first_with_polygon",
        .wkt_a = "POINT EMPTY",
        .wkt_b = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .expected_im = "FFFFFF212",
    },
    {
        .name = "empty_second_with_polygon",
        .wkt_a = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .wkt_b = "POINT EMPTY",
        .expected_im = "FF2FF1FF2",
    },
    {
        .name = "empty_line_with_closed_line_mod2",
        .wkt_a = "LINESTRING EMPTY",
        .wkt_b = "LINESTRING (0 0, 2 0, 2 2, 0 2, 0 0)",
        .expected_im = "FFFFFF1F2",
    },
});

TEST(RelateOpTest, StaticRelateComputesExpectedMatricesAndTransposeSymmetry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const RelateCase& tc : kDefaultRuleCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr a = reader.read(std::string(tc.wkt_a));
    Geometry::Ptr b = reader.read(std::string(tc.wkt_b));

    std::unique_ptr<IntersectionMatrix> im_ab =
        RelateOp::relate(a.get(), b.get());
    ASSERT_NE(im_ab, nullptr);
    EXPECT_EQ(im_ab->toString(), tc.expected_im);

    std::unique_ptr<IntersectionMatrix> im_ba =
        RelateOp::relate(b.get(), a.get());
    ASSERT_NE(im_ba, nullptr);
    IntersectionMatrix expected_ba(*im_ab);
    expected_ba.transpose();
    EXPECT_EQ(im_ba->toString(), expected_ba.toString());
  }
}

void ExpectRelateWithRule(const Geometry* a, const Geometry* b,
                          const BoundaryNodeRule& rule,
                          std::string_view expected_im) {
  SCOPED_TRACE(rule.toString());
  std::unique_ptr<IntersectionMatrix> im = RelateOp::relate(a, b, rule);
  ASSERT_NE(im, nullptr);
  EXPECT_EQ(im->toString(), expected_im);
}

TEST(RelateOpTest, StaticRelateWithBoundaryNodeRules) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr pt_at_seam = reader.read("POINT (0 0)");
  Geometry::Ptr pt_disjoint = reader.read("POINT (50 50)");
  Geometry::Ptr closed_line =
      reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  Geometry::Ptr open_line = reader.read("LINESTRING (0 0, 10 0)");
  Geometry::Ptr open_mls =
      reader.read("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");
  Geometry::Ptr diag_line = reader.read("LINESTRING (0 0, 10 10)");
  Geometry::Ptr pt_in_env = reader.read("POINT (2 8)");
  Geometry::Ptr mls_2 =
      reader.read("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))");
  Geometry::Ptr pt_mid = reader.read("POINT (5 0)");
  Geometry::Ptr half_edge = reader.read("LINESTRING (0 0, 5 0)");

  // Closed line seam: interior under Mod2 / OGC SFS, boundary under EndPoint.
  ExpectRelateWithRule(pt_at_seam.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryOGCSFS(), "0FFFFF1F2");
  ExpectRelateWithRule(pt_at_seam.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryRuleMod2(), "0FFFFF1F2");
  ExpectRelateWithRule(pt_at_seam.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryEndPoint(), "F0FFFF1F2");

  // Disjoint point vs closed line: EB reflects whether closed line has a
  // boundary under each BoundaryNodeRule.
  ExpectRelateWithRule(pt_disjoint.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryRuleMod2(), "FF0FFF1F2");
  ExpectRelateWithRule(pt_disjoint.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryEndPoint(), "FF0FFF102");
  ExpectRelateWithRule(pt_disjoint.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
                       "FF0FFF102");
  ExpectRelateWithRule(pt_disjoint.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
                       "FF0FFF1F2");

  // Disjoint point vs open single LineString: the disjoint-envelope fast path
  // uses BoundaryOp::boundaryLineString, which treats non-closed LineString
  // endpoints as a point boundary without checking isInBoundary(1).
  ExpectRelateWithRule(pt_disjoint.get(), open_line.get(),
                       BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
                       "FF0FFF102");
  ExpectRelateWithRule(pt_disjoint.get(), open_line.get(),
                       BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
                       "FF0FFF102");

  // Disjoint point vs open MultiLineString: BoundaryOp::boundaryMultiLineString
  // applies the BoundaryNodeRule to valence-1 endpoints.
  ExpectRelateWithRule(pt_disjoint.get(), open_mls.get(),
                       BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
                       "FF0FFF1F2");
  ExpectRelateWithRule(pt_disjoint.get(), open_mls.get(),
                       BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
                       "FF0FFF102");

  // Overlapping envelopes go through GeometryGraph, which applies the
  // BoundaryNodeRule to single LineString valence-1 endpoints as well.
  ExpectRelateWithRule(pt_in_env.get(), diag_line.get(),
                       BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
                       "FF0FFF1F2");
  ExpectRelateWithRule(pt_in_env.get(), diag_line.get(),
                       BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
                       "FF0FFF102");
  ExpectRelateWithRule(pt_at_seam.get(), open_line.get(),
                       BoundaryNodeRule::getBoundaryRuleMod2(), "F0FFFF102");
  ExpectRelateWithRule(pt_at_seam.get(), open_line.get(),
                       BoundaryNodeRule::getBoundaryEndPoint(), "F0FFFF102");
  ExpectRelateWithRule(pt_at_seam.get(), open_line.get(),
                       BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
                       "0FFFFF1F2");
  ExpectRelateWithRule(pt_at_seam.get(), open_line.get(),
                       BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
                       "F0FFFF102");

  // Valence-2 shared node in MultiLineString: interior under Mod2 and
  // Monovalent, boundary under EndPoint.
  ExpectRelateWithRule(pt_mid.get(), mls_2.get(),
                       BoundaryNodeRule::getBoundaryRuleMod2(), "0FFFFF102");
  ExpectRelateWithRule(pt_mid.get(), mls_2.get(),
                       BoundaryNodeRule::getBoundaryEndPoint(), "F0FFFF102");
  ExpectRelateWithRule(pt_mid.get(), mls_2.get(),
                       BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
                       "0FFFFF102");

  // Open segment touching closed ring seam: BB is F under Mod2 vs 0 under
  // EndPoint.
  ExpectRelateWithRule(half_edge.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryRuleMod2(), "1FF0FF1F2");
  ExpectRelateWithRule(half_edge.get(), closed_line.get(),
                       BoundaryNodeRule::getBoundaryEndPoint(), "1FF00F1F2");
}

TEST(RelateOpTest, InstanceConstructorsAndGetIntersectionMatrix) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr line = reader.read("LINESTRING (0 5, 10 5)");
  Geometry::Ptr poly = reader.read("POLYGON ((2 0, 8 0, 8 10, 2 10, 2 0))");

  {
    RelateOp op(line.get(), poly.get());
    EXPECT_EQ(op.getArgGeometry(0), line.get());
    EXPECT_EQ(op.getArgGeometry(1), poly.get());

    std::unique_ptr<IntersectionMatrix> im = op.getIntersectionMatrix();
    ASSERT_NE(im, nullptr);
    EXPECT_EQ(im->toString(), "101FF0212");
    EXPECT_TRUE(im->isIntersects());
    EXPECT_FALSE(im->isDisjoint());
    EXPECT_TRUE(im->isCrosses(Dimension::L, Dimension::A));
    EXPECT_FALSE(im->isWithin());
    EXPECT_FALSE(im->isContains());
    EXPECT_FALSE(im->isTouches(Dimension::L, Dimension::A));
    EXPECT_EQ(im->get(Location::INTERIOR, Location::INTERIOR), Dimension::L);
    EXPECT_EQ(im->get(Location::INTERIOR, Location::BOUNDARY), Dimension::P);
    EXPECT_EQ(im->get(Location::BOUNDARY, Location::EXTERIOR), Dimension::P);
  }

  {
    Geometry::Ptr pt = reader.read("POINT (0 0)");
    Geometry::Ptr ring =
        reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");

    // Exercise polymorphic destruction via base pointer.
    auto op = std::make_unique<RelateOp>(
        pt.get(), ring.get(), BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_EQ(op->getArgGeometry(0), pt.get());
    EXPECT_EQ(op->getArgGeometry(1), ring.get());

    std::unique_ptr<IntersectionMatrix> im = op->getIntersectionMatrix();
    ASSERT_NE(im, nullptr);
    EXPECT_EQ(im->toString(), "F0FFFF1F2");
    EXPECT_TRUE(im->isTouches(Dimension::P, Dimension::L));

    std::unique_ptr<GeometryGraphOperation> base_op = std::move(op);
    base_op.reset();
  }
}

TEST(RelateOpTest, MixedPrecisionModelsChooseFinerComputationPrecision) {
  PrecisionModel fixed_pm(10.0);
  PrecisionModel float_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  GeometryFactory::Ptr float_factory = GeometryFactory::create(&float_pm);

  WKTReader fixed_reader(fixed_factory.get());
  WKTReader float_reader(float_factory.get());

  Geometry::Ptr fixed_poly =
      fixed_reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr float_line = float_reader.read("LINESTRING (-1 5, 11 5)");

  std::unique_ptr<IntersectionMatrix> im_fixed_first =
      RelateOp::relate(fixed_poly.get(), float_line.get());
  ASSERT_NE(im_fixed_first, nullptr);
  EXPECT_EQ(im_fixed_first->toString(), "1F20F1102");

  std::unique_ptr<IntersectionMatrix> im_float_first =
      RelateOp::relate(float_line.get(), fixed_poly.get());
  ASSERT_NE(im_float_first, nullptr);
  EXPECT_EQ(im_float_first->toString(), "101FF0212");
}

TEST(RelateOpTest, ThrowsOnCurvedGeometryComponents) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr curve = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  Geometry::Ptr pt = reader.read("POINT (1 1)");

  EXPECT_THROW(RelateOp::relate(curve.get(), pt.get()),
               UnsupportedOperationException);
  EXPECT_THROW(RelateOp::relate(pt.get(), curve.get(),
                                BoundaryNodeRule::getBoundaryEndPoint()),
               UnsupportedOperationException);
  EXPECT_THROW(RelateOp(curve.get(), pt.get()), UnsupportedOperationException);
  EXPECT_THROW(
      RelateOp(pt.get(), curve.get(), BoundaryNodeRule::getBoundaryEndPoint()),
      UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
