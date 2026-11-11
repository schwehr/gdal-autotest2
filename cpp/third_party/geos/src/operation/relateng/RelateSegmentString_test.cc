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

#include "third_party/geos/include/geos/operation/relateng/RelateSegmentString.h"

#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::relateng::NodeSection;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateSegmentString;

void ExpectCoord2DEq(const CoordinateXY* actual, double expected_x,
                     double expected_y) {
  ASSERT_NE(actual, nullptr);
  EXPECT_DOUBLE_EQ(actual->x, expected_x);
  EXPECT_DOUBLE_EQ(actual->y, expected_y);
}

class RelateSegmentStringTest : public ::testing::Test {
 protected:
  RelateSegmentStringTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<LineString> ReadLine(const std::string& wkt) const {
    return reader_.read<LineString>(wkt);
  }

  std::unique_ptr<Polygon> ReadPoly(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(RelateSegmentStringTest, CreateLineAndRingMetadata) {
  std::unique_ptr<LineString> line = ReadLine("LINESTRING (0 0, 10 0, 10 10)");
  std::unique_ptr<Polygon> poly = ReadPoly(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(4 4, 4 8, 8 8, 8 4, 4 4))");

  RelateGeometry relate_line(line.get());
  RelateGeometry relate_poly(poly.get());

  std::unique_ptr<const RelateSegmentString> line_ss(
      RelateSegmentString::createLine(line->getCoordinatesRO(),
                                      /*isA=*/true, /*elementId=*/7,
                                      &relate_line));
  ASSERT_NE(line_ss, nullptr);
  EXPECT_TRUE(line_ss->isA());
  EXPECT_EQ(line_ss->getGeometry(), &relate_line);
  EXPECT_EQ(line_ss->getPolygonal(), nullptr);
  EXPECT_FALSE(line_ss->isClosed());
  EXPECT_EQ(line_ss->size(), 3u);

  std::unique_ptr<const RelateSegmentString> shell_ss(
      RelateSegmentString::createRing(
          poly->getExteriorRing()->getCoordinatesRO(),
          /*isA=*/false, /*elementId=*/3,
          /*ringId=*/0, poly.get(), &relate_poly));
  ASSERT_NE(shell_ss, nullptr);
  EXPECT_FALSE(shell_ss->isA());
  EXPECT_EQ(shell_ss->getGeometry(), &relate_poly);
  EXPECT_EQ(shell_ss->getPolygonal(), poly.get());
  EXPECT_TRUE(shell_ss->isClosed());
  EXPECT_EQ(shell_ss->size(), 5u);

  std::unique_ptr<const RelateSegmentString> hole_ss(
      RelateSegmentString::createRing(
          poly->getInteriorRingN(0)->getCoordinatesRO(),
          /*isA=*/true, /*elementId=*/3, /*ringId=*/1, poly.get(),
          &relate_poly));
  ASSERT_NE(hole_ss, nullptr);
  EXPECT_TRUE(hole_ss->isA());
  EXPECT_EQ(hole_ss->getGeometry(), &relate_poly);
  EXPECT_EQ(hole_ss->getPolygonal(), poly.get());
  EXPECT_TRUE(hole_ss->isClosed());
  EXPECT_EQ(hole_ss->size(), 5u);
}

TEST_F(RelateSegmentStringTest, CreateNodeSectionOnOpenLine) {
  std::unique_ptr<LineString> line =
      ReadLine("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  RelateGeometry relate_geom(line.get());
  std::unique_ptr<const RelateSegmentString> ss(RelateSegmentString::createLine(
      line->getCoordinatesRO(),
      /*isA=*/true, /*elementId=*/4, &relate_geom));

  // Segment 0 start vertex (0,0): open start has no prev vertex.
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(0, CoordinateXY(0, 0)));
    EXPECT_TRUE(ns->isA());
    EXPECT_EQ(ns->dimension(), +Dimension::L);
    EXPECT_EQ(ns->id(), 4);
    EXPECT_EQ(ns->ringId(), -1);
    EXPECT_EQ(ns->getPolygonal(), nullptr);
    EXPECT_FALSE(ns->isArea());
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(&ns->nodePt(), 0, 0);
    EXPECT_EQ(ns->getVertex(0), nullptr);
    ExpectCoord2DEq(ns->getVertex(1), 10, 0);
  }

  // Segment 0 interior point (5,0): not at a vertex; prev=(0,0), next=(10,0).
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(0, CoordinateXY(5, 0)));
    EXPECT_FALSE(ns->isNodeAtVertex());
    ExpectCoord2DEq(&ns->nodePt(), 5, 0);
    ExpectCoord2DEq(ns->getVertex(0), 0, 0);
    ExpectCoord2DEq(ns->getVertex(1), 10, 0);
  }

  // Segment 0 end vertex (10,0) with segIndex < size() - 2: next=(10,10).
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(0, CoordinateXY(10, 0)));
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(&ns->nodePt(), 10, 0);
    ExpectCoord2DEq(ns->getVertex(0), 0, 0);
    ExpectCoord2DEq(ns->getVertex(1), 10, 10);
  }

  // Segment 1 start vertex (10,0) with segIndex > 0: prev=(0,0), next=(10,10).
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(1, CoordinateXY(10, 0)));
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(&ns->nodePt(), 10, 0);
    ExpectCoord2DEq(ns->getVertex(0), 0, 0);
    ExpectCoord2DEq(ns->getVertex(1), 10, 10);
  }

  // Final segment (segIndex == size() - 2 == 2) end vertex (20,10): next=null.
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(2, CoordinateXY(20, 10)));
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(&ns->nodePt(), 20, 10);
    ExpectCoord2DEq(ns->getVertex(0), 10, 10);
    EXPECT_EQ(ns->getVertex(1), nullptr);
  }
}

TEST_F(RelateSegmentStringTest, CreateNodeSectionOnSingleSegmentLine) {
  std::unique_ptr<LineString> line = ReadLine("LINESTRING (1 2, 5 6)");
  RelateGeometry relate_geom(line.get());
  std::unique_ptr<const RelateSegmentString> ss(RelateSegmentString::createLine(
      line->getCoordinatesRO(),
      /*isA=*/false, /*elementId=*/0, &relate_geom));
  ASSERT_EQ(ss->size(), 2u);

  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(0, CoordinateXY(1, 2)));
    EXPECT_FALSE(ns->isA());
    EXPECT_TRUE(ns->isNodeAtVertex());
    EXPECT_EQ(ns->getVertex(0), nullptr);
    ExpectCoord2DEq(ns->getVertex(1), 5, 6);
  }
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(0, CoordinateXY(3, 4)));
    EXPECT_FALSE(ns->isNodeAtVertex());
    ExpectCoord2DEq(ns->getVertex(0), 1, 2);
    ExpectCoord2DEq(ns->getVertex(1), 5, 6);
  }
  {
    std::unique_ptr<NodeSection> ns(
        ss->createNodeSection(0, CoordinateXY(5, 6)));
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(ns->getVertex(0), 1, 2);
    EXPECT_EQ(ns->getVertex(1), nullptr);
  }
}

TEST_F(RelateSegmentStringTest, CreateNodeSectionOnClosedRing) {
  std::unique_ptr<Polygon> poly = ReadPoly(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 2 4, 4 4, 4 2, 2 2))");
  RelateGeometry relate_geom(poly.get());

  std::unique_ptr<const RelateSegmentString> shell_ss(
      RelateSegmentString::createRing(
          poly->getExteriorRing()->getCoordinatesRO(),
          /*isA=*/true, /*elementId=*/1,
          /*ringId=*/0, poly.get(), &relate_geom));

  // Segment 0 start (0,0) on closed ring wraps prev via prevInRing(0) to
  // (0,10).
  {
    std::unique_ptr<NodeSection> ns(
        shell_ss->createNodeSection(0, CoordinateXY(0, 0)));
    EXPECT_TRUE(ns->isA());
    EXPECT_EQ(ns->dimension(), +Dimension::A);
    EXPECT_EQ(ns->id(), 1);
    EXPECT_EQ(ns->ringId(), 0);
    EXPECT_EQ(ns->getPolygonal(), poly.get());
    EXPECT_TRUE(ns->isShell());
    EXPECT_TRUE(ns->isArea());
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(ns->getVertex(0), 0, 10);
    ExpectCoord2DEq(ns->getVertex(1), 10, 0);
  }

  // Interior point on segment 1 (10,5).
  {
    std::unique_ptr<NodeSection> ns(
        shell_ss->createNodeSection(1, CoordinateXY(10, 5)));
    EXPECT_FALSE(ns->isNodeAtVertex());
    ExpectCoord2DEq(ns->getVertex(0), 10, 0);
    ExpectCoord2DEq(ns->getVertex(1), 10, 10);
  }

  // Final segment (segIndex == size() - 2 == 3) closing endpoint (2,2) wraps
  // next via SegmentString::nextInRing(4) to (2,4).
  std::unique_ptr<const RelateSegmentString> hole_ss(
      RelateSegmentString::createRing(
          poly->getInteriorRingN(0)->getCoordinatesRO(),
          /*isA=*/false, /*elementId=*/1, /*ringId=*/1, poly.get(),
          &relate_geom));
  const std::size_t hole_last_seg = hole_ss->size() - 2;
  {
    std::unique_ptr<NodeSection> ns(
        hole_ss->createNodeSection(hole_last_seg, CoordinateXY(2, 2)));
    EXPECT_FALSE(ns->isA());
    EXPECT_EQ(ns->ringId(), 1);
    EXPECT_FALSE(ns->isShell());
    EXPECT_TRUE(ns->isArea());
    EXPECT_TRUE(ns->isNodeAtVertex());
    ExpectCoord2DEq(ns->getVertex(0), 4, 2);
    ExpectCoord2DEq(ns->getVertex(1), 2, 4);
  }
}

TEST_F(RelateSegmentStringTest, IsContainingSegmentOpenLine) {
  std::unique_ptr<LineString> line =
      ReadLine("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  RelateGeometry relate_geom(line.get());
  std::unique_ptr<const RelateSegmentString> ss(RelateSegmentString::createLine(
      line->getCoordinatesRO(),
      /*isA=*/true, /*elementId=*/0, &relate_geom));

  const CoordinateXY p0(0, 0);
  const CoordinateXY p_mid0(5, 0);
  const CoordinateXY p1(10, 0);
  const CoordinateXY p2(10, 10);
  const CoordinateXY p_mid2(15, 10);
  const CoordinateXY p3(20, 10);

  // Segment 0: start and interior are contained; end (non-final) is not.
  EXPECT_TRUE(ss->isContainingSegment(0, &p0));
  EXPECT_TRUE(ss->isContainingSegment(0, &p_mid0));
  EXPECT_FALSE(ss->isContainingSegment(0, &p1));

  // Segment 1: start (10,0) is contained; end (10,10, non-final) is not.
  EXPECT_TRUE(ss->isContainingSegment(1, &p1));
  EXPECT_FALSE(ss->isContainingSegment(1, &p2));

  // Final segment 2: start, interior, AND final open end (20,10) are
  // contained.
  EXPECT_TRUE(ss->isContainingSegment(2, &p2));
  EXPECT_TRUE(ss->isContainingSegment(2, &p_mid2));
  EXPECT_TRUE(ss->isContainingSegment(2, &p3));
}

TEST_F(RelateSegmentStringTest, IsContainingSegmentClosedRing) {
  std::unique_ptr<Polygon> poly =
      ReadPoly("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  RelateGeometry relate_geom(poly.get());
  std::unique_ptr<const RelateSegmentString> ring_ss(
      RelateSegmentString::createRing(
          poly->getExteriorRing()->getCoordinatesRO(),
          /*isA=*/true, /*elementId=*/0,
          /*ringId=*/0, poly.get(), &relate_geom));

  const CoordinateXY v0(0, 0);
  const CoordinateXY v0_mid(5, 0);
  const CoordinateXY v1(10, 0);
  const CoordinateXY v3(0, 10);
  const std::size_t last_seg = ring_ss->size() - 2;

  // Segment 0: start and interior are contained; end v1 is not.
  EXPECT_TRUE(ring_ss->isContainingSegment(0, &v0));
  EXPECT_TRUE(ring_ss->isContainingSegment(0, &v0_mid));
  EXPECT_FALSE(ring_ss->isContainingSegment(0, &v1));

  // Final segment: start v3 is contained, while closing endpoint v0 is NOT
  // contained (segment 0 owns v0 instead).
  EXPECT_TRUE(ring_ss->isContainingSegment(last_seg, &v3));
  EXPECT_FALSE(ring_ss->isContainingSegment(last_seg, &v0));
}

}  // namespace
}  // namespace geos_google
