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

#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"

#include <memory>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::io::WKTWriter;
using geos::operation::relateng::NodeSection;

constexpr int kDimP = Dimension::P;
constexpr int kDimL = Dimension::L;
constexpr int kDimA = Dimension::A;

TEST(NodeSectionTest, PrimaryAndPointerCopyConstructorsAndAccessors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  std::unique_ptr<Polygon> poly =
      reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  const CoordinateXY v0(0.0, 0.0);
  const CoordinateXY node_pt(5.0, 0.0);
  const CoordinateXY v1(10.0, 0.0);

  NodeSection shell_a(true, kDimA, 3, 0, poly.get(), true, &v0, node_pt, &v1);
  EXPECT_TRUE(shell_a.isA());
  EXPECT_EQ(shell_a.dimension(), kDimA);
  EXPECT_EQ(shell_a.id(), 3);
  EXPECT_EQ(shell_a.ringId(), 0);
  EXPECT_EQ(shell_a.getPolygonal(), poly.get());
  EXPECT_TRUE(shell_a.isShell());
  EXPECT_TRUE(shell_a.isArea());
  EXPECT_TRUE(shell_a.isNodeAtVertex());
  EXPECT_FALSE(shell_a.isProper());
  EXPECT_EQ(shell_a.getVertex(0), &v0);
  EXPECT_EQ(shell_a.getVertex(1), &v1);
  EXPECT_EQ(shell_a.getVertex(2), &v1);
  EXPECT_TRUE(shell_a.nodePt().equals2D(node_pt));

  // Pointer copy constructor preserves every field, including null pointers.
  NodeSection copy(&shell_a);
  EXPECT_EQ(copy.isA(), shell_a.isA());
  EXPECT_EQ(copy.dimension(), shell_a.dimension());
  EXPECT_EQ(copy.id(), shell_a.id());
  EXPECT_EQ(copy.ringId(), shell_a.ringId());
  EXPECT_EQ(copy.getPolygonal(), shell_a.getPolygonal());
  EXPECT_EQ(copy.isNodeAtVertex(), shell_a.isNodeAtVertex());
  EXPECT_EQ(copy.getVertex(0), shell_a.getVertex(0));
  EXPECT_EQ(copy.getVertex(1), shell_a.getVertex(1));
  EXPECT_TRUE(copy.nodePt().equals2D(shell_a.nodePt()));
  EXPECT_EQ(copy.compareTo(shell_a), 0);

  NodeSection hole_b(false, kDimA, 3, 1, poly.get(), false, &v0, node_pt, &v1);
  EXPECT_FALSE(hole_b.isA());
  EXPECT_FALSE(hole_b.isShell());
  EXPECT_TRUE(hole_b.isArea());
  EXPECT_FALSE(hole_b.isNodeAtVertex());
  EXPECT_TRUE(hole_b.isProper());

  NodeSection line_a(true, kDimL, -1, -1, nullptr, false, nullptr, node_pt,
                     &v1);
  EXPECT_EQ(line_a.dimension(), kDimL);
  EXPECT_EQ(line_a.id(), -1);
  EXPECT_EQ(line_a.ringId(), -1);
  EXPECT_EQ(line_a.getPolygonal(), nullptr);
  EXPECT_FALSE(line_a.isShell());
  EXPECT_FALSE(line_a.isArea());
  EXPECT_EQ(line_a.getVertex(0), nullptr);
  EXPECT_EQ(line_a.getVertex(1), &v1);

  NodeSection line_copy(&line_a);
  EXPECT_EQ(line_copy.getPolygonal(), nullptr);
  EXPECT_EQ(line_copy.getVertex(0), nullptr);
  EXPECT_EQ(line_copy.getVertex(1), &v1);

  NodeSection point_a(true, kDimP, 0, 0, nullptr, true, nullptr, node_pt,
                      nullptr);
  EXPECT_FALSE(point_a.isArea());
}

TEST(NodeSectionTest, PredicatesAreaGeometryPolygonAndProper) {
  const CoordinateXY v0(0.0, 0.0);
  const CoordinateXY node_pt(1.0, 1.0);
  const CoordinateXY v1(2.0, 2.0);

  NodeSection a_area_vertex(true, kDimA, 1, 0, nullptr, true, &v0, node_pt,
                            &v1);
  NodeSection a_area_hole_proper(true, kDimA, 1, 2, nullptr, false, &v0,
                                 node_pt, &v1);
  NodeSection a_other_poly(true, kDimA, 2, 0, nullptr, false, &v0, node_pt,
                           &v1);
  NodeSection b_area_same_id(false, kDimA, 1, 0, nullptr, true, &v0, node_pt,
                             &v1);
  NodeSection b_line_proper(false, kDimL, -1, 0, nullptr, false, &v0, node_pt,
                            &v1);
  NodeSection a_line_vertex(true, kDimL, -1, 0, nullptr, true, &v0, node_pt,
                            &v1);

  // isAreaArea across all 4 combinations.
  EXPECT_TRUE(NodeSection::isAreaArea(a_area_vertex, b_area_same_id));
  EXPECT_FALSE(NodeSection::isAreaArea(a_area_vertex, b_line_proper));
  EXPECT_FALSE(NodeSection::isAreaArea(a_line_vertex, b_area_same_id));
  EXPECT_FALSE(NodeSection::isAreaArea(a_line_vertex, b_line_proper));

  // isSameGeometry across all 4 combinations.
  EXPECT_TRUE(a_area_vertex.isSameGeometry(a_other_poly));
  EXPECT_TRUE(b_area_same_id.isSameGeometry(b_line_proper));
  EXPECT_FALSE(a_area_vertex.isSameGeometry(b_area_same_id));
  EXPECT_FALSE(b_area_same_id.isSameGeometry(a_area_vertex));

  // isSamePolygon: same (isA, id) even with different ringId -> true.
  EXPECT_TRUE(a_area_vertex.isSamePolygon(a_area_hole_proper));
  EXPECT_FALSE(a_area_vertex.isSamePolygon(a_other_poly));
  EXPECT_FALSE(a_area_vertex.isSamePolygon(b_area_same_id));
  EXPECT_FALSE(a_other_poly.isSamePolygon(b_area_same_id));

  // Static isProper across all 4 combinations.
  EXPECT_TRUE(NodeSection::isProper(a_area_hole_proper, b_line_proper));
  EXPECT_FALSE(NodeSection::isProper(a_area_hole_proper, a_area_vertex));
  EXPECT_FALSE(NodeSection::isProper(a_area_vertex, b_line_proper));
  EXPECT_FALSE(NodeSection::isProper(a_area_vertex, a_line_vertex));
}

TEST(NodeSectionTest, EdgeRepHandlesNullAndCoordinates) {
  const CoordinateXY p0(1.5, 2.5);
  const CoordinateXY p1(3.5, 4.5);

  EXPECT_EQ(NodeSection::edgeRep(nullptr, nullptr), "null");
  EXPECT_EQ(NodeSection::edgeRep(nullptr, &p1), "null");
  EXPECT_EQ(NodeSection::edgeRep(&p0, nullptr), "null");
  EXPECT_EQ(NodeSection::edgeRep(&p0, &p1), WKTWriter::toLineString(p0, p1));
  EXPECT_EQ(NodeSection::edgeRep(&p0, &p1), "LINESTRING (1.5 2.5, 3.5 4.5)");
}

TEST(NodeSectionTest, CompareToOrdersByIsADimIdRingIdV0AndV1) {
  const CoordinateXY node_pt(5.0, 5.0);
  const CoordinateXY v_lo(1.0, 2.0);
  const CoordinateXY v_lo_copy(1.0, 2.0);
  const CoordinateXY v_hi(2.0, 2.0);
  const CoordinateXY v_hi_copy(2.0, 2.0);

  // 1. isA: A sorts before B.
  NodeSection a_base(true, kDimA, 1, 0, nullptr, true, &v_lo, node_pt, &v_hi);
  NodeSection b_base(false, kDimA, 1, 0, nullptr, true, &v_lo, node_pt, &v_hi);
  EXPECT_EQ(a_base.compareTo(b_base), -1);
  EXPECT_EQ(b_base.compareTo(a_base), 1);

  // 2. dimension: L (1) sorts before A (2).
  NodeSection a_line(true, kDimL, 1, 0, nullptr, true, &v_lo, node_pt, &v_hi);
  EXPECT_EQ(a_line.compareTo(a_base), -1);
  EXPECT_EQ(a_base.compareTo(a_line), 1);

  // 3. id.
  NodeSection a_id0(true, kDimA, 0, 0, nullptr, true, &v_lo, node_pt, &v_hi);
  EXPECT_EQ(a_id0.compareTo(a_base), -1);
  EXPECT_EQ(a_base.compareTo(a_id0), 1);

  // 4. ringId.
  NodeSection a_ring1(true, kDimA, 1, 1, nullptr, true, &v_lo, node_pt, &v_hi);
  EXPECT_EQ(a_base.compareTo(a_ring1), -1);
  EXPECT_EQ(a_ring1.compareTo(a_base), 1);

  // 5. v0 with nullptr and non-null coordinates.
  NodeSection a_v0_null(true, kDimA, 1, 0, nullptr, true, nullptr, node_pt,
                        &v_hi);
  NodeSection a_v0_hi(true, kDimA, 1, 0, nullptr, true, &v_hi, node_pt, &v_hi);
  EXPECT_EQ(a_v0_null.compareTo(a_base), -1);
  EXPECT_EQ(a_base.compareTo(a_v0_null), 1);
  EXPECT_EQ(a_base.compareTo(a_v0_hi), -1);
  EXPECT_EQ(a_v0_hi.compareTo(a_base), 1);

  // Equal v0 (both null) falls through to v1.
  NodeSection a_both_null(true, kDimA, 1, 0, nullptr, true, nullptr, node_pt,
                          nullptr);
  EXPECT_EQ(a_both_null.compareTo(a_v0_null), -1);
  EXPECT_EQ(a_v0_null.compareTo(a_both_null), 1);
  EXPECT_EQ(a_both_null.compareTo(a_both_null), 0);

  // Equal non-null v0 at different memory addresses falls through to v1.
  NodeSection a_v1_null(true, kDimA, 1, 0, nullptr, true, &v_lo_copy, node_pt,
                        nullptr);
  NodeSection a_v1_lo(true, kDimA, 1, 0, nullptr, true, &v_lo_copy, node_pt,
                      &v_lo);
  EXPECT_EQ(a_v1_null.compareTo(a_v1_lo), -1);
  EXPECT_EQ(a_v1_lo.compareTo(a_v1_null), 1);
  EXPECT_EQ(a_v1_lo.compareTo(a_base), -1);
  EXPECT_EQ(a_base.compareTo(a_v1_lo), 1);

  // Equal across all compared keys (different nodePt and isNodeAtVertex).
  const CoordinateXY other_node_pt(99.0, 99.0);
  NodeSection a_equal(true, kDimA, 1, 0, nullptr, false, &v_lo_copy,
                      other_node_pt, &v_hi_copy);
  EXPECT_EQ(a_base.compareTo(a_equal), 0);
}

TEST(NodeSectionTest, ToStringAndStreamInsertion) {
  const CoordinateXY v0(0.0, 0.0);
  const CoordinateXY node_pt(5.0, 5.0);
  const CoordinateXY v1(10.0, 0.0);

  // id >= 0, node at vertex, both endpoints present.
  NodeSection poly_vertex(true, kDimA, 0, 2, nullptr, true, &v0, node_pt, &v1);
  const std::string expected_poly =
      "2[0:2]: LINESTRING (0 0, 5 5)-V- LINESTRING (5 5, 10 0)";
  EXPECT_EQ(poly_vertex.toString(), expected_poly);

  std::ostringstream oss_poly;
  oss_poly << poly_vertex;
  EXPECT_EQ(oss_poly.str(), expected_poly);

  // id < 0, proper crossing, v0 null, v1 present.
  NodeSection line_proper(false, kDimL, -1, 0, nullptr, false, nullptr, node_pt,
                          &v1);
  const std::string expected_line = "1: null--- LINESTRING (5 5, 10 0)";
  EXPECT_EQ(line_proper.toString(), expected_line);

  std::ostringstream oss_line;
  oss_line << line_proper;
  EXPECT_EQ(oss_line.str(), expected_line);

  // id >= 0, proper crossing, v0 present, v1 null.
  NodeSection line_end(true, kDimL, 1, 0, nullptr, false, &v0, node_pt,
                       nullptr);
  EXPECT_EQ(line_end.toString(), "1[1:0]: LINESTRING (0 0, 5 5)--- null");
}

}  // namespace
}  // namespace geos_google
