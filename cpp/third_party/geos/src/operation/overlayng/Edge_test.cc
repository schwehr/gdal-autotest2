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

// Unit tests for third_party/geos/src/operation/overlayng/Edge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/Edge.cpp

#include "third_party/geos/include/geos/operation/overlayng/Edge.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/io/WKBReader.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Position;
using geos::io::WKBReader;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeComparator;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::OverlayLabel;
using geos::util::GEOSException;

std::unique_ptr<CoordinateSequence> MakeSeq(
    std::initializer_list<Coordinate> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

TEST(EdgeTest, DefaultConstructorInitializesUnknownDimensionsAndNullCoords) {
  Edge edge;
  EXPECT_EQ(edge.getCoordinatesRO(), nullptr);
  EXPECT_EQ(edge.dimension(0), static_cast<int>(OverlayLabel::DIM_UNKNOWN));
  EXPECT_EQ(edge.dimension(1), static_cast<int>(OverlayLabel::DIM_UNKNOWN));

  OverlayLabel label;
  edge.populateLabel(label);
  EXPECT_TRUE(label.isNotPart(0));
  EXPECT_TRUE(label.isNotPart(1));
  EXPECT_FALSE(label.isLine());
  EXPECT_FALSE(label.isBoundaryEither());
}

TEST(EdgeTest, ConstructsFromLineSourceInfoAndPopulatesLabel) {
  const EdgeSourceInfo info0(0);
  Edge edge0(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info0);
  EXPECT_EQ(edge0.dimension(0), static_cast<int>(Dimension::L));
  EXPECT_EQ(edge0.dimension(1), static_cast<int>(OverlayLabel::DIM_UNKNOWN));

  OverlayLabel lbl0;
  edge0.populateLabel(lbl0);
  EXPECT_TRUE(lbl0.isLine());
  EXPECT_TRUE(lbl0.isLine(0));
  EXPECT_TRUE(lbl0.isNotPart(1));

  const EdgeSourceInfo info1(1);
  Edge edge1(MakeSeq({Coordinate(0, 0), Coordinate(0, 2)}), &info1);
  EXPECT_EQ(edge1.dimension(0), static_cast<int>(OverlayLabel::DIM_UNKNOWN));
  EXPECT_EQ(edge1.dimension(1), static_cast<int>(Dimension::L));

  OverlayLabel lbl1;
  edge1.populateLabel(lbl1);
  EXPECT_TRUE(lbl1.isLine());
  EXPECT_TRUE(lbl1.isNotPart(0));
  EXPECT_TRUE(lbl1.isLine(1));
}

TEST(EdgeTest, ConstructsFromAreaSourceInfoAndPopulatesBoundaryAndCollapse) {
  // Shell boundary from geometry 0 with positive depthDelta (+1 -> L:EXT,
  // R:INT).
  {
    const EdgeSourceInfo info(0, 1, false);
    Edge edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}), &info);
    EXPECT_EQ(edge.dimension(0), static_cast<int>(Dimension::A));
    EXPECT_EQ(edge.dimension(1), static_cast<int>(OverlayLabel::DIM_UNKNOWN));

    OverlayLabel lbl;
    edge.populateLabel(lbl);
    EXPECT_TRUE(lbl.isBoundary(0));
    EXPECT_TRUE(lbl.isNotPart(1));
    EXPECT_FALSE(lbl.isHole(0));
    EXPECT_EQ(lbl.getLocation(0, Position::LEFT, true), Location::EXTERIOR);
    EXPECT_EQ(lbl.getLocation(0, Position::RIGHT, true), Location::INTERIOR);
  }

  // Hole boundary from geometry 0 with negative depthDelta (-1 -> L:INT,
  // R:EXT).
  {
    const EdgeSourceInfo info(0, -1, true);
    Edge edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}), &info);

    OverlayLabel lbl;
    edge.populateLabel(lbl);
    EXPECT_TRUE(lbl.isBoundary(0));
    EXPECT_TRUE(lbl.isHole(0));
    EXPECT_EQ(lbl.getLocation(0, Position::LEFT, true), Location::INTERIOR);
    EXPECT_EQ(lbl.getLocation(0, Position::RIGHT, true), Location::EXTERIOR);
  }

  // Hole boundary from geometry 1 with positive depthDelta.
  {
    const EdgeSourceInfo info(1, 1, true);
    Edge edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}), &info);
    EXPECT_EQ(edge.dimension(1), static_cast<int>(Dimension::A));

    OverlayLabel lbl;
    edge.populateLabel(lbl);
    EXPECT_TRUE(lbl.isNotPart(0));
    EXPECT_TRUE(lbl.isBoundary(1));
    EXPECT_TRUE(lbl.isHole(1));
    EXPECT_EQ(lbl.getLocation(1, Position::LEFT, true), Location::EXTERIOR);
    EXPECT_EQ(lbl.getLocation(1, Position::RIGHT, true), Location::INTERIOR);
  }

  // Collapsed area edge (depthDelta == 0) preserves shell vs hole flag.
  {
    const EdgeSourceInfo shell_collapse(0, 0, false);
    Edge shell_edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}),
                    &shell_collapse);
    OverlayLabel shell_lbl;
    shell_edge.populateLabel(shell_lbl);
    EXPECT_TRUE(shell_lbl.isCollapse(0));
    EXPECT_FALSE(shell_lbl.isHole(0));

    const EdgeSourceInfo hole_collapse(1, 0, true);
    Edge hole_edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}),
                   &hole_collapse);
    OverlayLabel hole_lbl;
    hole_edge.populateLabel(hole_lbl);
    EXPECT_TRUE(hole_lbl.isCollapse(1));
    EXPECT_TRUE(hole_lbl.isHole(1));
  }
}

TEST(EdgeTest, CoordinateAccessorsCloneAndRelease) {
  const EdgeSourceInfo info(0);
  Edge edge(MakeSeq({Coordinate(1, 2), Coordinate(3, 4), Coordinate(5, 6)}),
            &info);

  EXPECT_EQ(edge.size(), 3u);
  EXPECT_TRUE(edge.getCoordinate(0).equals2D(Coordinate(1, 2)));
  EXPECT_TRUE(edge.getCoordinate(1).equals2D(Coordinate(3, 4)));
  EXPECT_TRUE(edge.getCoordinate(2).equals2D(Coordinate(5, 6)));

  const CoordinateSequence* ro = edge.getCoordinatesRO();
  ASSERT_NE(ro, nullptr);
  EXPECT_EQ(ro->size(), 3u);

  std::unique_ptr<CoordinateSequence> cloned = edge.getCoordinates();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), ro);
  EXPECT_EQ(cloned->size(), 3u);
  cloned->setAt(Coordinate(99, 99), 0);
  EXPECT_TRUE(edge.getCoordinate(0).equals2D(Coordinate(1, 2)));

  std::unique_ptr<CoordinateSequence> released(edge.releaseCoordinates());
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released->size(), 3u);
  EXPECT_EQ(edge.getCoordinatesRO(), nullptr);
}

TEST(EdgeTest, IsCollapsedDetectsDegenerateAndValidSequences) {
  EXPECT_TRUE(Edge::isCollapsed(MakeSeq({}).get()));
  EXPECT_TRUE(Edge::isCollapsed(MakeSeq({Coordinate(1, 2)}).get()));
  EXPECT_TRUE(Edge::isCollapsed(
      MakeSeq({Coordinate(1, 2, 0), Coordinate(1, 2, 9)}).get()));
  EXPECT_TRUE(Edge::isCollapsed(
      MakeSeq({Coordinate(1, 2), Coordinate(1, 2), Coordinate(3, 4)}).get()));
  EXPECT_TRUE(Edge::isCollapsed(
      MakeSeq({Coordinate(0, 0), Coordinate(3, 4), Coordinate(3, 4)}).get()));

  EXPECT_FALSE(
      Edge::isCollapsed(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}).get()));
  EXPECT_FALSE(Edge::isCollapsed(
      MakeSeq({Coordinate(0, 0), Coordinate(1, 1), Coordinate(2, 0)}).get()));
  EXPECT_FALSE(Edge::isCollapsed(MakeSeq({Coordinate(0, 0), Coordinate(1, 0),
                                          Coordinate(1, 1), Coordinate(0, 0)})
                                     .get()));
}

TEST(EdgeTest, DirectionHandlesForwardBackwardClosedAndThrowsOnDegenerate) {
  const EdgeSourceInfo info(0);

  // Open edges ordered by endpoints.
  EXPECT_TRUE(
      Edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}), &info).direction());
  EXPECT_TRUE(
      Edge(MakeSeq({Coordinate(0, 0), Coordinate(0, 1)}), &info).direction());
  EXPECT_FALSE(
      Edge(MakeSeq({Coordinate(1, 0), Coordinate(0, 0)}), &info).direction());
  EXPECT_FALSE(
      Edge(MakeSeq({Coordinate(0, 1), Coordinate(0, 0)}), &info).direction());

  // Closed edges where p0 == pn0 fall back to comparing p1 vs pn1.
  Edge closed_forward(
      MakeSeq({Coordinate(0, 0), Coordinate(1, 0), Coordinate(1, 1),
               Coordinate(2, 0), Coordinate(0, 0)}),
      &info);
  EXPECT_TRUE(closed_forward.direction());

  Edge closed_backward(
      MakeSeq({Coordinate(0, 0), Coordinate(2, 0), Coordinate(1, 1),
               Coordinate(1, 0), Coordinate(0, 0)}),
      &info);
  EXPECT_FALSE(closed_backward.direction());

  // Too few points throws GEOSException.
  EXPECT_THROW(Edge(MakeSeq({}), &info).direction(), GEOSException);
  EXPECT_THROW(Edge(MakeSeq({Coordinate(1, 2)}), &info).direction(),
               GEOSException);

  // Equal endpoint pairs (p0 == pn0 and p1 == pn1) throw GEOSException.
  EXPECT_THROW(
      Edge(MakeSeq({Coordinate(1, 1), Coordinate(1, 1)}), &info).direction(),
      GEOSException);
  EXPECT_THROW(
      Edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 1), Coordinate(0, 0)}),
           &info)
          .direction(),
      GEOSException);
  EXPECT_THROW(Edge(MakeSeq({Coordinate(0, 0), Coordinate(1, 0),
                             Coordinate(1, 0), Coordinate(0, 0)}),
                    &info)
                   .direction(),
               GEOSException);
}

TEST(EdgeTest, RelativeDirectionComparesFirstTwoCoordinates) {
  const EdgeSourceInfo info(0);
  Edge base(MakeSeq({Coordinate(0, 0), Coordinate(1, 0), Coordinate(1, 1),
                     Coordinate(2, 0), Coordinate(0, 0)}),
            &info);
  Edge same_dir(MakeSeq({Coordinate(0, 0), Coordinate(1, 0), Coordinate(5, 5)}),
                &info);
  Edge diff_p0(MakeSeq({Coordinate(1, 0), Coordinate(0, 0)}), &info);
  Edge diff_p1(MakeSeq({Coordinate(0, 0), Coordinate(2, 0), Coordinate(1, 1),
                        Coordinate(1, 0), Coordinate(0, 0)}),
               &info);

  EXPECT_TRUE(base.relativeDirection(&same_dir));
  EXPECT_FALSE(base.relativeDirection(&diff_p0));
  EXPECT_FALSE(base.relativeDirection(&diff_p1));
}

TEST(EdgeTest, MergeCombinesDimensionsDepthDeltasAndHoleFlags) {
  // Same-direction shells reinforce depthDelta (1 + 1 = 2 -> BOUNDARY).
  {
    const EdgeSourceInfo info_a(0, 1, false);
    const EdgeSourceInfo info_b(0, 1, false);
    Edge e1(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info_a);
    Edge e2(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info_b);
    e1.merge(&e2);

    OverlayLabel lbl;
    e1.populateLabel(lbl);
    EXPECT_TRUE(lbl.isBoundary(0));
    EXPECT_FALSE(lbl.isHole(0));
    EXPECT_EQ(lbl.getLocation(0, Position::LEFT, true), Location::EXTERIOR);
    EXPECT_EQ(lbl.getLocation(0, Position::RIGHT, true), Location::INTERIOR);
  }

  // Opposite-direction shells with equal depthDelta cancel to COLLAPSE.
  {
    const EdgeSourceInfo info_a(0, 1, false);
    const EdgeSourceInfo info_b(0, 1, false);
    Edge e1(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info_a);
    Edge e2(MakeSeq({Coordinate(2, 0), Coordinate(0, 0)}), &info_b);
    e1.merge(&e2);

    OverlayLabel lbl;
    e1.populateLabel(lbl);
    EXPECT_TRUE(lbl.isCollapse(0));
    EXPECT_FALSE(lbl.isHole(0));
  }

  // Two hole edges stay marked as hole; hole merged with shell becomes shell.
  {
    const EdgeSourceInfo hole_info(0, 1, true);
    const EdgeSourceInfo shell_info(0, 1, false);
    Edge h1(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &hole_info);
    Edge h2(MakeSeq({Coordinate(2, 0), Coordinate(0, 0)}), &hole_info);
    h1.merge(&h2);

    OverlayLabel hole_lbl;
    h1.populateLabel(hole_lbl);
    EXPECT_TRUE(hole_lbl.isCollapse(0));
    EXPECT_TRUE(hole_lbl.isHole(0));

    Edge s1(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &shell_info);
    h1.merge(&s1);
    OverlayLabel mixed_lbl;
    h1.populateLabel(mixed_lbl);
    EXPECT_TRUE(mixed_lbl.isBoundary(0));
    EXPECT_FALSE(mixed_lbl.isHole(0));
  }

  // Merging geometry 0 area edge with opposite geometry 1 area edge populates
  // both.
  {
    const EdgeSourceInfo info_a(0, 1, false);
    const EdgeSourceInfo info_b(1, 1, true);
    Edge e_a(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info_a);
    Edge e_b(MakeSeq({Coordinate(2, 0), Coordinate(0, 0)}), &info_b);
    e_a.merge(&e_b);

    EXPECT_EQ(e_a.dimension(0), static_cast<int>(Dimension::A));
    EXPECT_EQ(e_a.dimension(1), static_cast<int>(Dimension::A));

    OverlayLabel lbl;
    e_a.populateLabel(lbl);
    EXPECT_TRUE(lbl.isBoundaryBoth());
    EXPECT_FALSE(lbl.isHole(0));
    EXPECT_TRUE(lbl.isHole(1));
    EXPECT_EQ(lbl.getLocation(0, Position::LEFT, true), Location::EXTERIOR);
    EXPECT_EQ(lbl.getLocation(0, Position::RIGHT, true), Location::INTERIOR);
    EXPECT_EQ(lbl.getLocation(1, Position::LEFT, true), Location::INTERIOR);
    EXPECT_EQ(lbl.getLocation(1, Position::RIGHT, true), Location::EXTERIOR);
  }

  // Merging geometry 1 line into geometry 0 line keeps both dimensions as Line.
  {
    const EdgeSourceInfo line0(0);
    const EdgeSourceInfo line1(1);
    Edge e0(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &line0);
    Edge e1(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &line1);
    e0.merge(&e1);

    EXPECT_EQ(e0.dimension(0), static_cast<int>(Dimension::L));
    EXPECT_EQ(e0.dimension(1), static_cast<int>(Dimension::L));
    OverlayLabel lbl;
    e0.populateLabel(lbl);
    EXPECT_TRUE(lbl.isLine(0));
    EXPECT_TRUE(lbl.isLine(1));
  }
}

TEST(EdgeTest, CompareToAndEdgeComparatorProvideStrictWeakOrdering) {
  const EdgeSourceInfo info(0);
  Edge e_00_10(MakeSeq({Coordinate(0, 0), Coordinate(1, 0)}), &info);
  Edge e_00_20(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info);
  Edge e_00_20_dup(MakeSeq({Coordinate(0, 0), Coordinate(2, 0)}), &info);
  Edge e_10_00(MakeSeq({Coordinate(1, 0), Coordinate(0, 0)}), &info);

  EXPECT_TRUE(e_00_10.compareTo(e_10_00));
  EXPECT_FALSE(e_10_00.compareTo(e_00_10));
  EXPECT_TRUE(e_00_10.compareTo(e_00_20));
  EXPECT_FALSE(e_00_20.compareTo(e_00_10));
  EXPECT_FALSE(e_00_20.compareTo(e_00_20_dup));
  EXPECT_FALSE(EdgeComparator(&e_00_20, &e_00_20));

  std::vector<const Edge*> edges = {&e_10_00, &e_00_20, &e_00_10};
  std::sort(edges.begin(), edges.end(), EdgeComparator);
  ASSERT_EQ(edges.size(), 3u);
  EXPECT_EQ(edges[0], &e_00_10);
  EXPECT_EQ(edges[1], &e_00_20);
  EXPECT_EQ(edges[2], &e_10_00);
}

TEST(EdgeTest, StreamOperatorWritesValidHexWkbLineString) {
  const EdgeSourceInfo info(0);
  Edge edge(MakeSeq({Coordinate(1.5, 2.5, 10.0), Coordinate(3.0, 4.0, 20.0),
                     Coordinate(5.5, -1.0, 30.0)}),
            &info);

  std::ostringstream oss;
  oss << edge;
  const std::string hex = oss.str();
  ASSERT_FALSE(hex.empty());

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKBReader reader(*factory);
  std::istringstream iss(hex);
  Geometry::Ptr parsed = reader.readHEX(iss);
  ASSERT_NE(parsed, nullptr);
  EXPECT_EQ(parsed->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(parsed->getNumPoints(), 3u);

  std::unique_ptr<CoordinateSequence> parsed_coords = parsed->getCoordinates();
  ASSERT_NE(parsed_coords, nullptr);
  EXPECT_TRUE(parsed_coords->equalsIdentical(*edge.getCoordinatesRO()));
}

}  // namespace
}  // namespace geos_google
