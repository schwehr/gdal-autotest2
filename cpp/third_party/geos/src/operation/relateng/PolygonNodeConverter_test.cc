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

// Unit tests for
// third_party/geos/src/operation/relateng/PolygonNodeConverter.cpp.

#include "third_party/geos/include/geos/operation/relateng/PolygonNodeConverter.h"

#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"
#include "third_party/geos/include/geos/operation/relateng/RelateEdge.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNode.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geom::Position;
using geos::operation::relateng::NodeSection;
using geos::operation::relateng::PolygonNodeConverter;
using geos::operation::relateng::RelateEdge;
using geos::operation::relateng::RelateNode;
using geos::util::IllegalArgumentException;

struct ExpectedSection {
  bool is_a;
  int dim;
  int id;
  int ring_id;
  const Geometry* poly;
  bool is_node_at_vertex;
  CoordinateXY v0;
  CoordinateXY node_pt;
  CoordinateXY v1;
};

void ExpectSectionMatches(const NodeSection& actual,
                          const ExpectedSection& expected) {
  EXPECT_EQ(actual.isA(), expected.is_a);
  EXPECT_EQ(actual.dimension(), expected.dim);
  EXPECT_EQ(actual.id(), expected.id);
  EXPECT_EQ(actual.ringId(), expected.ring_id);
  EXPECT_EQ(actual.getPolygonal(), expected.poly);
  EXPECT_EQ(actual.isNodeAtVertex(), expected.is_node_at_vertex);
  ASSERT_NE(actual.getVertex(0), nullptr);
  ASSERT_NE(actual.getVertex(1), nullptr);
  EXPECT_TRUE(actual.getVertex(0)->equals2D(expected.v0))
      << "actual v0=" << actual.getVertex(0)->toString()
      << " expected v0=" << expected.v0.toString();
  EXPECT_TRUE(actual.nodePt().equals2D(expected.node_pt))
      << "actual nodePt=" << actual.nodePt().toString()
      << " expected nodePt=" << expected.node_pt.toString();
  EXPECT_TRUE(actual.getVertex(1)->equals2D(expected.v1))
      << "actual v1=" << actual.getVertex(1)->toString()
      << " expected v1=" << expected.v1.toString();
}

void ExpectSectionsMatch(
    const std::vector<std::unique_ptr<NodeSection>>& actual,
    const std::vector<ExpectedSection>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    SCOPED_TRACE(i);
    ASSERT_NE(actual[i], nullptr);
    ExpectSectionMatches(*actual[i], expected[i]);
  }
}

TEST(PolygonNodeConverterTest, SingleShellSectionReturnsCopyPreservingFields) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = factory->createPolygon();

  const CoordinateXY node_pt(5.0, 5.0);
  const CoordinateXY v0(10.0, 5.0);
  const CoordinateXY v1(5.0, 10.0);
  const NodeSection shell(true, Dimension::A, 7, 0, poly.get(), true, &v0,
                          node_pt, &v1);

  std::vector<const NodeSection*> input = {&shell};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ASSERT_EQ(converted.size(), 1u);
  EXPECT_NE(converted[0].get(), &shell);
  ExpectSectionMatches(*converted[0], {.is_a = true,
                                       .dim = Dimension::A,
                                       .id = 7,
                                       .ring_id = 0,
                                       .poly = poly.get(),
                                       .is_node_at_vertex = true,
                                       .v0 = v0,
                                       .node_pt = node_pt,
                                       .v1 = v1});
}

TEST(PolygonNodeConverterTest,
     SingleHoleSectionReturnsCopyPreservingRingIdAndDimension) {
  const CoordinateXY node_pt(1.0, 2.0);
  const CoordinateXY v0(0.0, 3.0);
  const CoordinateXY v1(3.0, 3.0);
  const NodeSection hole(false, Dimension::L, 3, 2, nullptr, false, &v0,
                         node_pt, &v1);

  std::vector<const NodeSection*> input = {&hole};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ASSERT_EQ(converted.size(), 1u);
  EXPECT_NE(converted[0].get(), &hole);
  // Single-section fast path copies the section directly instead of calling
  // createSection(), so ringId and dimension are preserved verbatim.
  ExpectSectionMatches(*converted[0], {.is_a = false,
                                       .dim = Dimension::L,
                                       .id = 3,
                                       .ring_id = 2,
                                       .poly = nullptr,
                                       .is_node_at_vertex = false,
                                       .v0 = v0,
                                       .node_pt = node_pt,
                                       .v1 = v1});
}

TEST(PolygonNodeConverterTest, DuplicateSingleSectionsCollapseToOne) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = factory->createPolygon();

  const CoordinateXY node_pt(5.0, 0.0);
  const CoordinateXY v0_a(10.0, 0.0);
  const CoordinateXY v1_a(0.0, 0.0);
  const CoordinateXY v0_b(10.0, 0.0);
  const CoordinateXY v1_b(0.0, 0.0);
  const NodeSection s1(true, Dimension::A, 4, 0, poly.get(), true, &v0_a,
                       node_pt, &v1_a);
  const NodeSection s2(true, Dimension::A, 4, 0, poly.get(), true, &v0_b,
                       node_pt, &v1_b);

  std::vector<const NodeSection*> input = {&s1, &s2, &s1};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 4,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = v0_a,
                                   .node_pt = node_pt,
                                   .v1 = v1_a}});
}

TEST(PolygonNodeConverterTest, ShellAndOneHoleConvertsToTwoInteriorWedges) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = factory->createPolygon();

  // CW shell bottom edge passing through (5, 0), interior in y > 0 ([0, 180]).
  const CoordinateXY node_pt(5.0, 0.0);
  const CoordinateXY s0_v0(10.0, 0.0);
  const CoordinateXY s0_v1(0.0, 0.0);
  const NodeSection shell(true, Dimension::A, 9, 0, poly.get(), false, &s0_v0,
                          node_pt, &s0_v1);

  // CCW hole inside y > 0 touching (5, 0): (5,0) -> (7,4) -> (3,4) -> (5,0).
  const CoordinateXY h1_v0(3.0, 4.0);
  const CoordinateXY h1_v1(7.0, 4.0);
  const NodeSection hole(true, Dimension::A, 9, 1, poly.get(), true, &h1_v0,
                         node_pt, &h1_v1);

  // Pass unsorted to verify comparator orders sections CCW by v0.
  std::vector<const NodeSection*> input = {&hole, &shell};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ASSERT_EQ(input.size(), 2u);
  EXPECT_EQ(input[0], &shell);
  EXPECT_EQ(input[1], &hole);

  // Converted corners carry the shell's metadata (isNodeAtVertex = false) and
  // reset ringId to 0.
  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 9,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = false,
                                   .v0 = s0_v0,
                                   .node_pt = node_pt,
                                   .v1 = h1_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 9,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = false,
                                   .v0 = h1_v0,
                                   .node_pt = node_pt,
                                   .v1 = s0_v1}});

  // Original input sections remain unmodified.
  EXPECT_EQ(hole.ringId(), 1);
  EXPECT_TRUE(hole.getVertex(0)->equals2D(h1_v0));
  EXPECT_TRUE(hole.getVertex(1)->equals2D(h1_v1));
}

TEST(PolygonNodeConverterTest,
     ShellAndThreeHolesConvertsAllWedgesAndDeduplicates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = factory->createPolygon();

  const CoordinateXY node_pt(5.0, 0.0);
  const CoordinateXY s0_v0(10.0, 0.0);
  const CoordinateXY s0_v1(0.0, 0.0);
  const NodeSection shell(true, Dimension::A, 1, 0, poly.get(), true, &s0_v0,
                          node_pt, &s0_v1);

  // Three CCW holes ordered CCW inside [0, 180] deg around (5, 0).
  const CoordinateXY h1_v1(9.0, 1.0);  // ~14.0 deg
  const CoordinateXY h1_v0(8.0, 2.0);  // ~33.7 deg
  const NodeSection hole1(true, Dimension::A, 1, 1, poly.get(), true, &h1_v0,
                          node_pt, &h1_v1);

  const CoordinateXY h2_v1(6.0, 3.0);  // ~71.6 deg
  const CoordinateXY h2_v0(5.0, 4.0);  // 90.0 deg
  const NodeSection hole2(true, Dimension::A, 1, 2, poly.get(), true, &h2_v0,
                          node_pt, &h2_v1);
  const CoordinateXY h2_dup_v1(6.0, 3.0);
  const CoordinateXY h2_dup_v0(5.0, 4.0);
  const NodeSection hole2_dup(true, Dimension::A, 1, 2, poly.get(), true,
                              &h2_dup_v0, node_pt, &h2_dup_v1);

  const CoordinateXY h3_v1(3.0, 3.0);  // ~123.7 deg
  const CoordinateXY h3_v0(1.0, 1.0);  // ~166.0 deg
  const NodeSection hole3(true, Dimension::A, 1, 3, poly.get(), true, &h3_v0,
                          node_pt, &h3_v1);

  std::vector<const NodeSection*> input = {&hole3, &hole1, &hole2, &shell,
                                           &hole2_dup};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = s0_v0,
                                   .node_pt = node_pt,
                                   .v1 = h1_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h1_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h2_v0,
                                   .node_pt = node_pt,
                                   .v1 = h3_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h3_v0,
                                   .node_pt = node_pt,
                                   .v1 = s0_v1}});
}

TEST(PolygonNodeConverterTest, ShellNotAtFirstSortedIndexWrapsAround) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly = factory->createPolygon();

  // Reflex CW shell around non-origin node (-10, 20): interior sweeps CCW
  // from 135 deg -> 270 deg -> 360/0 deg -> 45 deg.
  const CoordinateXY node_pt(-10.0, 20.0);
  const CoordinateXY s0_v0(-11.0, 21.0);  // 135 deg
  const CoordinateXY s0_v1(-9.0, 21.0);   // 45 deg
  const NodeSection shell(false, Dimension::A, 5, 0, poly.get(), true, &s0_v0,
                          node_pt, &s0_v1);

  // Hole 1 in [315, 0] deg -> v0 at 0 deg sorts BEFORE shell's v0 (135 deg).
  const CoordinateXY h1_v1(-9.0, 19.0);  // 315 deg
  const CoordinateXY h1_v0(-9.0, 20.0);  // 0 deg
  const NodeSection hole1(false, Dimension::A, 5, 1, poly.get(), false, &h1_v0,
                          node_pt, &h1_v1);

  // Hole 2 in [225, 270] deg -> v0 at 270 deg sorts AFTER shell's v0.
  const CoordinateXY h2_v1(-11.0, 19.0);  // 225 deg
  const CoordinateXY h2_v0(-10.0, 19.0);  // 270 deg
  const NodeSection hole2(false, Dimension::A, 5, 2, poly.get(), false, &h2_v0,
                          node_pt, &h2_v1);

  std::vector<const NodeSection*> input = {&hole2, &shell, &hole1};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  // Sorted input: [hole1 (0 deg), shell (135 deg), hole2 (270 deg)].
  // findShell() skips index 0 and starts at 1, wrapping past index 2 -> 0.
  ExpectSectionsMatch(converted, {{.is_a = false,
                                   .dim = Dimension::A,
                                   .id = 5,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = s0_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2_v1},
                                  {.is_a = false,
                                   .dim = Dimension::A,
                                   .id = 5,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h2_v0,
                                   .node_pt = node_pt,
                                   .v1 = h1_v1},
                                  {.is_a = false,
                                   .dim = Dimension::A,
                                   .id = 5,
                                   .ring_id = 0,
                                   .poly = poly.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h1_v0,
                                   .node_pt = node_pt,
                                   .v1 = s0_v1}});
}

TEST(PolygonNodeConverterTest,
     TwoTouchingHolesWithoutShellConvertsBetweenAdjacentHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly1 = factory->createPolygon();
  std::unique_ptr<Polygon> poly2 = factory->createPolygon();

  const CoordinateXY node_pt(0.0, 0.0);
  // East CCW hole: [333.4, 26.6] deg.
  const CoordinateXY h1_v1(2.0, -1.0);
  const CoordinateXY h1_v0(2.0, 1.0);
  const NodeSection hole1(false, Dimension::A, 11, 1, poly1.get(), true, &h1_v0,
                          node_pt, &h1_v1);

  // West CCW hole: [153.4, 206.6] deg.
  const CoordinateXY h2_v1(-2.0, 1.0);
  const CoordinateXY h2_v0(-2.0, -1.0);
  const NodeSection hole2(false, Dimension::A, 12, 2, poly2.get(), false,
                          &h2_v0, node_pt, &h2_v1);

  std::vector<const NodeSection*> input = {&hole2, &hole1};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  // convertHoles() copies metadata from sections[0] (hole1), setting ringId=0.
  ExpectSectionsMatch(converted, {{.is_a = false,
                                   .dim = Dimension::A,
                                   .id = 11,
                                   .ring_id = 0,
                                   .poly = poly1.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h1_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2_v1},
                                  {.is_a = false,
                                   .dim = Dimension::A,
                                   .id = 11,
                                   .ring_id = 0,
                                   .poly = poly1.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h2_v0,
                                   .node_pt = node_pt,
                                   .v1 = h1_v1}});
}

TEST(PolygonNodeConverterTest,
     ThreeTouchingHolesWithoutShellAndDuplicatesConvertsToThreeWedges) {
  const CoordinateXY node_pt(10.0, -10.0);

  // Three CCW holes around (10, -10) leaving 3 polygon interior wedges.
  const CoordinateXY h1_v1(13.0, -11.0);  // ~341.6 deg
  const CoordinateXY h1_v0(13.0, -9.0);   // ~18.4 deg
  const NodeSection hole1(true, Dimension::A, 2, 1, nullptr, true, &h1_v0,
                          node_pt, &h1_v1);

  const CoordinateXY h2_v1(10.0, -7.0);  // 90 deg
  const CoordinateXY h2_v0(8.0, -8.0);   // 135 deg
  const NodeSection hole2(true, Dimension::A, 2, 2, nullptr, true, &h2_v0,
                          node_pt, &h2_v1);

  const CoordinateXY h3_v1(8.0, -12.0);   // 225 deg
  const CoordinateXY h3_v0(10.0, -13.0);  // 270 deg
  const NodeSection hole3(true, Dimension::A, 2, 3, nullptr, true, &h3_v0,
                          node_pt, &h3_v1);

  std::vector<const NodeSection*> input = {&hole3, &hole1, &hole2, &hole1};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = h1_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = h2_v0,
                                   .node_pt = node_pt,
                                   .v1 = h3_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = h3_v0,
                                   .node_pt = node_pt,
                                   .v1 = h1_v1}});
}

TEST(PolygonNodeConverterTest,
     TwoTouchingShellsWithoutHolesPreserveBothShellsAndMetadata) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly1 = factory->createPolygon();
  std::unique_ptr<Polygon> poly2 = factory->createPolygon();

  const CoordinateXY node_pt(0.0, 0.0);
  // Right CW shell: [333.4, 26.6] deg.
  const CoordinateXY s1_v0(2.0, -1.0);
  const CoordinateXY s1_v1(2.0, 1.0);
  const NodeSection shell1(true, Dimension::A, 1, 0, poly1.get(), true, &s1_v0,
                           node_pt, &s1_v1);

  // Left CW shell: [153.4, 206.6] deg.
  const CoordinateXY s2_v0(-2.0, 1.0);
  const CoordinateXY s2_v1(-2.0, -1.0);
  const NodeSection shell2(false, Dimension::A, 2, 0, poly2.get(), false,
                           &s2_v0, node_pt, &s2_v1);

  std::vector<const NodeSection*> input = {&shell1, &shell2};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  // Sorted order starts with shell2 (153.4 deg) then shell1 (333.4 deg).
  ExpectSectionsMatch(converted, {{.is_a = false,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = poly2.get(),
                                   .is_node_at_vertex = false,
                                   .v0 = s2_v0,
                                   .node_pt = node_pt,
                                   .v1 = s2_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly1.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = s1_v0,
                                   .node_pt = node_pt,
                                   .v1 = s1_v1}});
}

TEST(PolygonNodeConverterTest, MultipleShellsWithHolesConvertPerShellAndWrap) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> poly1 = factory->createPolygon();
  std::unique_ptr<Polygon> poly2 = factory->createPolygon();

  const CoordinateXY node_pt(0.0, 0.0);

  // Shell 1 covers [90, 180] deg with Hole 1 inside [116.6, 146.3] deg.
  const CoordinateXY s1_v0(0.0, 2.0);   // 90 deg
  const CoordinateXY s1_v1(-2.0, 0.0);  // 180 deg
  const NodeSection shell1(true, Dimension::A, 1, 0, poly1.get(), true, &s1_v0,
                           node_pt, &s1_v1);
  const CoordinateXY h1_v1(-1.0, 2.0);  // ~116.6 deg
  const CoordinateXY h1_v0(-3.0, 2.0);  // ~146.3 deg
  const NodeSection hole1(true, Dimension::A, 1, 1, poly1.get(), false, &h1_v0,
                          node_pt, &h1_v1);

  // Shell 2 covers [315, 45] deg with Hole 2 inside [341.6, 18.4] deg.
  const CoordinateXY s2_v0(2.0, -2.0);  // 315 deg
  const CoordinateXY s2_v1(2.0, 2.0);   // 45 deg
  const NodeSection shell2(true, Dimension::A, 2, 0, poly2.get(), false, &s2_v0,
                           node_pt, &s2_v1);
  const CoordinateXY h2_v1(3.0, -1.0);  // ~341.6 deg
  const CoordinateXY h2_v0(3.0, 1.0);   // ~18.4 deg
  const NodeSection hole2(true, Dimension::A, 2, 1, poly2.get(), true, &h2_v0,
                          node_pt, &h2_v1);

  std::vector<const NodeSection*> input = {&hole2, &shell2, &hole1, &shell1};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  // Sorted: [hole2 (18.4), shell1 (90), hole1 (146.3), shell2 (315)].
  // Shell 1 takes hole1; shell 2 wraps around index 0 to take hole2.
  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly1.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = s1_v0,
                                   .node_pt = node_pt,
                                   .v1 = h1_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = poly1.get(),
                                   .is_node_at_vertex = true,
                                   .v0 = h1_v0,
                                   .node_pt = node_pt,
                                   .v1 = s1_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = poly2.get(),
                                   .is_node_at_vertex = false,
                                   .v0 = s2_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = poly2.get(),
                                   .is_node_at_vertex = false,
                                   .v0 = h2_v0,
                                   .node_pt = node_pt,
                                   .v1 = s2_v1}});
}

TEST(PolygonNodeConverterTest, ThreeShellsMixedZeroAndMultipleHoles) {
  const CoordinateXY node_pt(0.0, 0.0);

  // Shell 1: [0, 90] deg, no holes.
  const CoordinateXY s1_v0(3.0, 0.0);
  const CoordinateXY s1_v1(0.0, 3.0);
  const NodeSection shell1(true, Dimension::A, 1, 0, nullptr, true, &s1_v0,
                           node_pt, &s1_v1);

  // Shell 2: [123.7, 198.4] deg, containing 2 holes.
  const CoordinateXY s2_v0(-2.0, 3.0);   // ~123.7 deg
  const CoordinateXY s2_v1(-3.0, -1.0);  // ~198.4 deg
  const NodeSection shell2(true, Dimension::A, 2, 0, nullptr, true, &s2_v0,
                           node_pt, &s2_v1);
  const CoordinateXY h2a_v1(-3.0, 2.0);  // ~146.3 deg
  const CoordinateXY h2a_v0(-3.0, 1.0);  // ~161.6 deg
  const NodeSection hole2a(true, Dimension::A, 2, 1, nullptr, false, &h2a_v0,
                           node_pt, &h2a_v1);
  const CoordinateXY h2b_v1(-3.0, 0.0);   // 180.0 deg
  const CoordinateXY h2b_v0(-4.0, -1.0);  // ~194.0 deg
  const NodeSection hole2b(true, Dimension::A, 2, 2, nullptr, false, &h2b_v0,
                           node_pt, &h2b_v1);

  // Shell 3: [270, 333.4] deg, no holes.
  const CoordinateXY s3_v0(0.0, -3.0);
  const CoordinateXY s3_v1(2.0, -1.0);
  const NodeSection shell3(true, Dimension::A, 3, 0, nullptr, false, &s3_v0,
                           node_pt, &s3_v1);

  std::vector<const NodeSection*> input = {&hole2b, &shell3, &shell1, &hole2a,
                                           &shell2};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = s1_v0,
                                   .node_pt = node_pt,
                                   .v1 = s1_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = s2_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2a_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = h2a_v0,
                                   .node_pt = node_pt,
                                   .v1 = h2b_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 2,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = h2b_v0,
                                   .node_pt = node_pt,
                                   .v1 = s2_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 3,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = false,
                                   .v0 = s3_v0,
                                   .node_pt = node_pt,
                                   .v1 = s3_v1}});
}

TEST(PolygonNodeConverterTest, SameAngleV0WithDistinctRingIdIsRetained) {
  const CoordinateXY node_pt(0.0, 0.0);
  const CoordinateXY shared_v0(10.0, 0.0);
  const CoordinateXY s_v1(0.0, 10.0);
  const CoordinateXY h_v1(5.0, 5.0);

  const NodeSection shell(true, Dimension::A, 1, 0, nullptr, true, &shared_v0,
                          node_pt, &s_v1);
  const NodeSection hole(true, Dimension::A, 1, 1, nullptr, true, &shared_v0,
                         node_pt, &h_v1);

  std::vector<const NodeSection*> input = {&shell, &hole};
  std::vector<std::unique_ptr<NodeSection>> converted =
      PolygonNodeConverter::convert(input);

  ExpectSectionsMatch(converted, {{.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = shared_v0,
                                   .node_pt = node_pt,
                                   .v1 = h_v1},
                                  {.is_a = true,
                                   .dim = Dimension::A,
                                   .id = 1,
                                   .ring_id = 0,
                                   .poly = nullptr,
                                   .is_node_at_vertex = true,
                                   .v0 = shared_v0,
                                   .node_pt = node_pt,
                                   .v1 = s_v1}});
}

TEST(PolygonNodeConverterTest,
     ConvertedSectionsProduceValidRelateNodeSideLocations) {
  constexpr bool kIsGeomA = true;
  // Subcase A: Shell + 1 hole at (5, 0) -> 4 edges around node.
  {
    const CoordinateXY node_pt(5.0, 0.0);
    const CoordinateXY s0_v0(10.0, 0.0);
    const CoordinateXY s0_v1(0.0, 0.0);
    const CoordinateXY h1_v0(3.0, 4.0);
    const CoordinateXY h1_v1(7.0, 4.0);
    const NodeSection shell(kIsGeomA, Dimension::A, 1, 0, nullptr, false,
                            &s0_v0, node_pt, &s0_v1);
    const NodeSection hole(kIsGeomA, Dimension::A, 1, 1, nullptr, true, &h1_v0,
                           node_pt, &h1_v1);

    std::vector<const NodeSection*> input = {&hole, &shell};
    std::vector<std::unique_ptr<NodeSection>> converted =
        PolygonNodeConverter::convert(input);

    RelateNode node(&node_pt);
    node.addEdges(converted);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    // CCW order: (10,0) [0], (7,4) [1], (3,4) [2], (0,0) [3].
    const Location expected_left[4] = {Location::INTERIOR, Location::EXTERIOR,
                                       Location::INTERIOR, Location::EXTERIOR};
    const Location expected_right[4] = {Location::EXTERIOR, Location::INTERIOR,
                                        Location::EXTERIOR, Location::INTERIOR};
    for (std::size_t i = 0; i < edges.size(); ++i) {
      SCOPED_TRACE(i);
      EXPECT_EQ(edges[i]->location(kIsGeomA, Position::ON), Location::BOUNDARY);
      EXPECT_EQ(edges[i]->location(kIsGeomA, Position::LEFT), expected_left[i]);
      EXPECT_EQ(edges[i]->location(kIsGeomA, Position::RIGHT),
                expected_right[i]);
    }
  }

  // Subcase B: Two holes touching at (0, 0) -> 4 edges around node.
  {
    const CoordinateXY node_pt(0.0, 0.0);
    const CoordinateXY h1_v1(2.0, -1.0);
    const CoordinateXY h1_v0(2.0, 1.0);
    const CoordinateXY h2_v1(-2.0, 1.0);
    const CoordinateXY h2_v0(-2.0, -1.0);
    const NodeSection hole1(kIsGeomA, Dimension::A, 1, 1, nullptr, true, &h1_v0,
                            node_pt, &h1_v1);
    const NodeSection hole2(kIsGeomA, Dimension::A, 1, 2, nullptr, true, &h2_v0,
                            node_pt, &h2_v1);

    std::vector<const NodeSection*> input = {&hole2, &hole1};
    std::vector<std::unique_ptr<NodeSection>> converted =
        PolygonNodeConverter::convert(input);

    RelateNode node(&node_pt);
    node.addEdges(converted);

    const auto& edges = node.getEdges();
    ASSERT_EQ(edges.size(), 4u);
    // CCW order: (2,1) [0], (-2,1) [1], (-2,-1) [2], (2,-1) [3].
    // Polygon interior lies in upper gap [26.6, 153.4] and lower gap
    // [206.6, 333.4]; hole interiors stay EXTERIOR.
    const Location expected_left[4] = {Location::INTERIOR, Location::EXTERIOR,
                                       Location::INTERIOR, Location::EXTERIOR};
    const Location expected_right[4] = {Location::EXTERIOR, Location::INTERIOR,
                                        Location::EXTERIOR, Location::INTERIOR};
    for (std::size_t i = 0; i < edges.size(); ++i) {
      SCOPED_TRACE(i);
      EXPECT_EQ(edges[i]->location(kIsGeomA, Position::ON), Location::BOUNDARY);
      EXPECT_EQ(edges[i]->location(kIsGeomA, Position::LEFT), expected_left[i]);
      EXPECT_EQ(edges[i]->location(kIsGeomA, Position::RIGHT),
                expected_right[i]);
    }
  }
}

TEST(PolygonNodeConverterTest,
     DegenerateZeroLengthVectorThrowsIllegalArgument) {
  const CoordinateXY node_pt(5.0, 5.0);
  const CoordinateXY valid_v0(6.0, 5.0);
  const CoordinateXY v1(5.0, 6.0);

  const NodeSection valid_section(true, Dimension::A, 1, 0, nullptr, true,
                                  &valid_v0, node_pt, &v1);
  const NodeSection zero_len_section(true, Dimension::A, 1, 1, nullptr, true,
                                     &node_pt, node_pt, &v1);

  std::vector<const NodeSection*> input = {&valid_section, &zero_len_section};
  EXPECT_THROW(PolygonNodeConverter::convert(input), IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
