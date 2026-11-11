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

#include "third_party/geos/include/geos/coverage/CoverageRingEdges.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/coverage/CoverageEdge.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageEdge;
using geos::coverage::CoverageRingEdges;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;

class CoverageRingEdgesTest : public ::testing::Test {
 protected:
  CoverageRingEdgesTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::vector<std::unique_ptr<Geometry>> ReadAll(
      const std::vector<std::string>& wkts) const {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.reserve(wkts.size());
    for (const std::string& wkt : wkts) {
      geoms.push_back(Read(wkt));
    }
    return geoms;
  }

  static std::vector<const Geometry*> AsConstPtrs(
      const std::vector<std::unique_ptr<Geometry>>& geoms) {
    std::vector<const Geometry*> ptrs;
    ptrs.reserve(geoms.size());
    for (const auto& geom : geoms) {
      ptrs.push_back(geom.get());
    }
    return ptrs;
  }

  static std::unique_ptr<Geometry> UnionAll(
      const std::vector<std::unique_ptr<Geometry>>& geoms) {
    if (geoms.empty()) {
      return nullptr;
    }
    std::unique_ptr<Geometry> acc = geoms[0]->clone();
    for (std::size_t i = 1; i < geoms.size(); ++i) {
      acc = acc->Union(geoms[i].get());
    }
    return acc;
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(CoverageRingEdgesTest, EmptyCoverage) {
  std::vector<const Geometry*> coverage;
  CoverageRingEdges edges(coverage);

  EXPECT_TRUE(edges.getEdges().empty());
  EXPECT_TRUE(edges.selectEdges(0).empty());
  EXPECT_TRUE(edges.selectEdges(1).empty());
  EXPECT_TRUE(edges.selectEdges(2).empty());
  EXPECT_TRUE(edges.buildCoverage().empty());
}

TEST_F(CoverageRingEdgesTest, EmptyPolygonAndEmptyMultiPolygon) {
  auto geoms = ReadAll({"POLYGON EMPTY", "MULTIPOLYGON EMPTY"});
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  EXPECT_TRUE(edges.getEdges().empty());
  EXPECT_TRUE(edges.selectEdges(1).empty());
  EXPECT_TRUE(edges.selectEdges(2).empty());

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_EQ(rebuilt[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(rebuilt[0]->isEmpty());
  EXPECT_EQ(rebuilt[1]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(rebuilt[1]->isEmpty());
}

TEST_F(CoverageRingEdgesTest, MultiPolygonWithEmptyComponentAndEmptyHole) {
  auto shellRing = factory_->createLinearRing(CoordinateSequence{
      Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
      Coordinate(0, 10), Coordinate(0, 0)});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing());
  auto polyWithEmptyHole =
      factory_->createPolygon(std::move(shellRing), std::move(holes));

  std::vector<std::unique_ptr<Polygon>> mpParts;
  mpParts.push_back(factory_->createPolygon());
  mpParts.push_back(std::unique_ptr<Polygon>(static_cast<Polygon*>(
      Read("POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))").release())));
  auto mpWithEmptyPart = factory_->createMultiPolygon(std::move(mpParts));

  std::vector<const Geometry*> coverage = {polyWithEmptyHole.get(),
                                           mpWithEmptyPart.get()};
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 2u);
  EXPECT_EQ(edges.selectEdges(1).size(), 2u);
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_TRUE(edge->isFreeRing());
    EXPECT_EQ(edge->getRingCount(), 1u);
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);

  ASSERT_EQ(rebuilt[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* rebuiltPoly = static_cast<const Polygon*>(rebuilt[0].get());
  EXPECT_EQ(rebuiltPoly->getNumInteriorRing(), 1u);
  EXPECT_TRUE(rebuiltPoly->getInteriorRingN(0)->isEmpty());
  EXPECT_TRUE(rebuiltPoly->equalsExact(polyWithEmptyHole.get()));

  ASSERT_EQ(rebuilt[1]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(rebuilt[1]->getNumGeometries(), 2u);
  EXPECT_TRUE(rebuilt[1]->getGeometryN(0)->isEmpty());
  EXPECT_FALSE(rebuilt[1]->getGeometryN(1)->isEmpty());
  EXPECT_TRUE(rebuilt[1]->equalsExact(mpWithEmptyPart.get()));
}

TEST_F(CoverageRingEdgesTest, DegenerateCollapsedRingIsSkippedAndCloned) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 0 0, 0 0, 0 0))",
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 1u);
  EXPECT_TRUE(edges.getEdges()[0]->isFreeRing());
  EXPECT_EQ(edges.getEdges()[0]->getRingCount(), 1u);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_TRUE(rebuilt[0]->equalsExact(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));
}

TEST_F(CoverageRingEdgesTest, SingleIsolatedPolygonFreeRingAndMutation) {
  auto geoms = ReadAll({"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"});
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 1u);
  CoverageEdge* edge = edges.getEdges()[0];
  EXPECT_TRUE(edge->isFreeRing());
  EXPECT_EQ(edge->getRingCount(), 1u);
  EXPECT_EQ(edges.selectEdges(1).size(), 1u);
  EXPECT_TRUE(edges.selectEdges(2).empty());

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 1u);
  EXPECT_TRUE(rebuilt[0]->isValid());
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));

  CoordinateSequence updated{
      Coordinate(0, 0),  Coordinate(10, 0), Coordinate(10, 10),
      Coordinate(5, 14), Coordinate(0, 10), Coordinate(0, 0),
  };
  edge->setCoordinates(&updated);

  auto rebuiltMutated = edges.buildCoverage();
  ASSERT_EQ(rebuiltMutated.size(), 1u);
  EXPECT_TRUE(rebuiltMutated[0]->isValid());
  EXPECT_DOUBLE_EQ(rebuiltMutated[0]->getArea(), 120.0);
}

TEST_F(CoverageRingEdgesTest, SinglePolygonWithDisjointHole) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(4 4, 4 12, 12 12, 12 4, 4 4))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 2u);
  EXPECT_EQ(edges.selectEdges(1).size(), 2u);
  EXPECT_TRUE(edges.selectEdges(2).empty());
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_TRUE(edge->isFreeRing());
    EXPECT_EQ(edge->getRingCount(), 1u);
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 1u);
  EXPECT_TRUE(rebuilt[0]->isValid());
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  const auto* rebuiltPoly = static_cast<const Polygon*>(rebuilt[0].get());
  EXPECT_EQ(rebuiltPoly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(rebuiltPoly->getArea(), 400.0 - 64.0);
}

TEST_F(CoverageRingEdgesTest, IslandFillingHoleSharesFreeRingEdge) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(5 5, 5 15, 15 15, 15 5, 5 5))",
      "POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 2u);
  auto outerEdges = edges.selectEdges(1);
  auto innerEdges = edges.selectEdges(2);
  ASSERT_EQ(outerEdges.size(), 1u);
  ASSERT_EQ(innerEdges.size(), 1u);
  EXPECT_TRUE(outerEdges[0]->isFreeRing());
  EXPECT_TRUE(innerEdges[0]->isFreeRing());
  EXPECT_EQ(innerEdges[0]->getRingCount(), 2u);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));

  // Expand the shared hole/island free ring from 10x10 to 12x12.
  CoordinateSequence expandedHole{
      Coordinate(4, 4),  Coordinate(4, 16), Coordinate(16, 16),
      Coordinate(16, 4), Coordinate(4, 4),
  };
  innerEdges[0]->setCoordinates(&expandedHole);

  auto rebuiltMutated = edges.buildCoverage();
  ASSERT_EQ(rebuiltMutated.size(), 2u);
  EXPECT_TRUE(rebuiltMutated[0]->isValid());
  EXPECT_TRUE(rebuiltMutated[1]->isValid());
  EXPECT_DOUBLE_EQ(rebuiltMutated[0]->getArea(), 400.0 - 144.0);
  EXPECT_DOUBLE_EQ(rebuiltMutated[1]->getArea(), 144.0);
  EXPECT_TRUE(rebuiltMutated[0]->touches(rebuiltMutated[1].get()));

  auto expectedSquare = Read("POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0))");
  auto unioned = UnionAll(rebuiltMutated);
  EXPECT_TRUE(unioned->equals(expectedSquare.get()));
}

TEST_F(CoverageRingEdgesTest, TwoAdjacentPolygonsSharedEdgeAndMutation) {
  // Start the second polygon at (10, 0) to also cover wrap-around boundary
  // transition checking at index 0.
  auto geoms = ReadAll({
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 3u);
  auto outerEdges = edges.selectEdges(1);
  auto sharedEdges = edges.selectEdges(2);
  EXPECT_EQ(outerEdges.size(), 2u);
  ASSERT_EQ(sharedEdges.size(), 1u);
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->isFreeRing());
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));

  CoverageEdge* shared = sharedEdges[0];
  CoordinateSequence bentShared{
      shared->getStartCoordinate(),
      Coordinate(12, 5),
      shared->getEndCoordinate(),
  };
  shared->setCoordinates(&bentShared);

  auto rebuiltMutated = edges.buildCoverage();
  ASSERT_EQ(rebuiltMutated.size(), 2u);
  EXPECT_TRUE(rebuiltMutated[0]->isValid());
  EXPECT_TRUE(rebuiltMutated[1]->isValid());
  EXPECT_DOUBLE_EQ(rebuiltMutated[0]->getArea(), 110.0);
  EXPECT_DOUBLE_EQ(rebuiltMutated[1]->getArea(), 90.0);
  EXPECT_TRUE(rebuiltMutated[0]->touches(rebuiltMutated[1].get()));

  auto expectedRect = Read("POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0))");
  auto unioned = UnionAll(rebuiltMutated);
  EXPECT_TRUE(unioned->equals(expectedRect.get()));
}

TEST_F(CoverageRingEdgesTest, TwoPolygonsTouchingAtSingleCornerVertex) {
  // Single touch vertex at (10, 10) creates a 1-node ring edge per polygon
  // (start == end), and for the first polygon the node is at index 2 > 0.
  auto geoms = ReadAll({
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((10 10, 20 10, 20 20, 10 20, 10 10))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 2u);
  EXPECT_EQ(edges.selectEdges(1).size(), 2u);
  EXPECT_TRUE(edges.selectEdges(2).empty());
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->isFreeRing());
    EXPECT_EQ(edge->getRingCount(), 1u);
    EXPECT_TRUE(edge->getStartCoordinate().equals2D(Coordinate(10, 10)));
    EXPECT_TRUE(edge->getEndCoordinate().equals2D(Coordinate(10, 10)));
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_TRUE(rebuilt[0]->isValid());
  EXPECT_TRUE(rebuilt[1]->isValid());
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));
}

TEST_F(CoverageRingEdgesTest, HoleTouchingShellAtSingleVertex) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 5 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 3 4, 7 4, 5 0))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 2u);
  EXPECT_EQ(edges.selectEdges(1).size(), 2u);
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->isFreeRing());
    EXPECT_EQ(edge->getRingCount(), 1u);
    EXPECT_TRUE(edge->getStartCoordinate().equals2D(Coordinate(5, 0)));
    EXPECT_TRUE(edge->getEndCoordinate().equals2D(Coordinate(5, 0)));
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 1u);
  EXPECT_TRUE(rebuilt[0]->isValid());
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
}

TEST_F(CoverageRingEdgesTest, TwoByTwoGridExercisesMultiRingNodesAndEdgeDirs) {
  // The fourth square starts at (20, 10) so edge 0 is forward (outer) while
  // edge 1 is stored reversed relative to the ring, exercising both endpoints
  // of ringEdges[1] during ring reconstruction.
  auto geoms = ReadAll({
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
      "POLYGON ((0 10, 10 10, 10 20, 0 20, 0 10))",
      "POLYGON ((20 10, 20 20, 10 20, 10 10, 20 10))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 8u);
  EXPECT_EQ(edges.selectEdges(1).size(), 4u);
  auto innerEdges = edges.selectEdges(2);
  EXPECT_EQ(innerEdges.size(), 4u);
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->isFreeRing());
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 4u);
  for (std::size_t i = 0; i < geoms.size(); ++i) {
    EXPECT_TRUE(rebuilt[i]->isValid());
    EXPECT_TRUE(rebuilt[i]->equals(geoms[i].get()));
  }

  // Perturb the inner edge between (10, 0) and (10, 10) at its midpoint.
  CoverageEdge* verticalBottomEdge = nullptr;
  for (CoverageEdge* edge : innerEdges) {
    bool hasBottom = edge->getStartCoordinate().equals2D(Coordinate(10, 0)) ||
                     edge->getEndCoordinate().equals2D(Coordinate(10, 0));
    bool hasCenter = edge->getStartCoordinate().equals2D(Coordinate(10, 10)) ||
                     edge->getEndCoordinate().equals2D(Coordinate(10, 10));
    if (hasBottom && hasCenter) {
      verticalBottomEdge = edge;
      break;
    }
  }
  ASSERT_NE(verticalBottomEdge, nullptr);

  CoordinateSequence bentInner{
      verticalBottomEdge->getStartCoordinate(),
      Coordinate(11, 5),
      verticalBottomEdge->getEndCoordinate(),
  };
  verticalBottomEdge->setCoordinates(&bentInner);

  auto rebuiltMutated = edges.buildCoverage();
  ASSERT_EQ(rebuiltMutated.size(), 4u);
  EXPECT_DOUBLE_EQ(rebuiltMutated[0]->getArea(), 105.0);
  EXPECT_DOUBLE_EQ(rebuiltMutated[1]->getArea(), 95.0);
  EXPECT_DOUBLE_EQ(rebuiltMutated[2]->getArea(), 100.0);
  EXPECT_DOUBLE_EQ(rebuiltMutated[3]->getArea(), 100.0);

  auto expectedGrid = Read("POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0))");
  auto unioned = UnionAll(rebuiltMutated);
  EXPECT_TRUE(unioned->equals(expectedGrid.get()));
}

TEST_F(CoverageRingEdgesTest, SurroundedCenterPolygonHasFourInnerEdges) {
  auto geoms = ReadAll({
      "POLYGON ((10 10, 20 10, 20 20, 10 20, 10 10))",
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
      "POLYGON ((20 10, 30 10, 30 20, 20 20, 20 10))",
      "POLYGON ((10 20, 20 20, 20 30, 10 30, 10 20))",
      "POLYGON ((0 10, 10 10, 10 20, 0 20, 0 10))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 8u);
  EXPECT_EQ(edges.selectEdges(1).size(), 4u);
  EXPECT_EQ(edges.selectEdges(2).size(), 4u);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 5u);
  for (std::size_t i = 0; i < geoms.size(); ++i) {
    EXPECT_TRUE(rebuilt[i]->isValid());
    EXPECT_TRUE(rebuilt[i]->equals(geoms[i].get()));
  }
}

TEST_F(CoverageRingEdgesTest, MultiPolygonCoveragePreservesFactoryAndSRID) {
  PrecisionModel pm;
  constexpr int kSrid = 4326;
  GeometryFactory::Ptr customFactory = GeometryFactory::create(&pm, kSrid);
  WKTReader customReader(*customFactory);

  auto mp0 = customReader.read(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0)))");
  auto mp1 = customReader.read(
      "MULTIPOLYGON (((0 10, 10 10, 10 20, 0 20, 0 10)), "
      "((30 0, 40 0, 40 10, 30 10, 30 0)))");

  std::vector<const Geometry*> coverage = {mp0.get(), mp1.get()};
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 6u);
  EXPECT_EQ(edges.selectEdges(1).size(), 4u);
  EXPECT_EQ(edges.selectEdges(2).size(), 2u);

  std::size_t freeRingCount = 0;
  for (const CoverageEdge* edge : edges.getEdges()) {
    if (edge->isFreeRing()) {
      ++freeRingCount;
    }
  }
  EXPECT_EQ(freeRingCount, 1u);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_EQ(rebuilt[0]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(rebuilt[1]->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(rebuilt[0]->getSRID(), kSrid);
  EXPECT_EQ(rebuilt[1]->getSRID(), kSrid);
  EXPECT_EQ(rebuilt[0]->getFactory(), customFactory.get());
  EXPECT_EQ(rebuilt[1]->getFactory(), customFactory.get());
  EXPECT_TRUE(rebuilt[0]->equals(mp0.get()));
  EXPECT_TRUE(rebuilt[1]->equals(mp1.get()));
}

TEST_F(CoverageRingEdgesTest, RepeatedConsecutiveVerticesAreCompacted) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 0 0, 5 0, 5 0, 10 0, 10 10, 10 10, 0 10, 0 0))",
      "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  // Repeated consecutive vertices produce zero-length boundary segments before
  // edge extraction, making (0, 0) and (5, 0) degree-4 boundary nodes in
  // addition to (10, 0) and (10, 10). Edge extraction then compacts duplicates.
  EXPECT_EQ(edges.getEdges().size(), 5u);
  EXPECT_EQ(edges.selectEdges(1).size(), 4u);
  EXPECT_EQ(edges.selectEdges(2).size(), 1u);
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->getCoordinates()->hasRepeatedPoints());
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_EQ(rebuilt[0]->getNumPoints(), 6u);
  EXPECT_EQ(rebuilt[1]->getNumPoints(), 5u);
  for (const auto& geom : rebuilt) {
    auto coords = geom->getCoordinates();
    EXPECT_FALSE(coords->hasRepeatedPoints());
  }
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));
}

TEST_F(CoverageRingEdgesTest, SinglePolygonWithRepeatedVertexAndMultipleHoles) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 2 6, 6 6, 6 2, 2 2), "
      "(12 12, 12 16, 16 16, 16 12, 12 12))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 3u);
  EXPECT_EQ(edges.selectEdges(1).size(), 3u);
  EXPECT_TRUE(edges.selectEdges(2).empty());

  std::size_t freeRings = 0;
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->getCoordinates()->hasRepeatedPoints());
    if (edge->isFreeRing()) {
      ++freeRings;
    }
  }
  EXPECT_EQ(freeRings, 2u);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 1u);
  const auto* rebuiltPoly = static_cast<const Polygon*>(rebuilt[0].get());
  EXPECT_EQ(rebuiltPoly->getNumInteriorRing(), 2u);
  EXPECT_EQ(rebuiltPoly->getExteriorRing()->getNumPoints(), 5u);
  EXPECT_FALSE(rebuiltPoly->getCoordinates()->hasRepeatedPoints());
  EXPECT_TRUE(rebuiltPoly->isValid());
  EXPECT_TRUE(rebuiltPoly->equals(geoms[0].get()));
}

TEST_F(CoverageRingEdgesTest,
       IslandFillingHoleWithPinchVertexSharesOneNodeEdge) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 5 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 3 4, 7 4, 5 0))",
      "POLYGON ((5 0, 7 4, 3 4, 5 0))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  ASSERT_EQ(edges.getEdges().size(), 2u);
  auto outerEdges = edges.selectEdges(1);
  auto sharedEdges = edges.selectEdges(2);
  ASSERT_EQ(outerEdges.size(), 1u);
  ASSERT_EQ(sharedEdges.size(), 1u);
  for (const CoverageEdge* edge : edges.getEdges()) {
    EXPECT_FALSE(edge->isFreeRing());
    EXPECT_TRUE(edge->getStartCoordinate().equals2D(Coordinate(5, 0)));
    EXPECT_TRUE(edge->getEndCoordinate().equals2D(Coordinate(5, 0)));
  }

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_TRUE(rebuilt[0]->isValid());
  EXPECT_TRUE(rebuilt[1]->isValid());
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));
}

TEST_F(CoverageRingEdgesTest, ThreeIdenticalPolygonsProduceRingCountThree) {
  auto geoms = ReadAll({
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  EXPECT_EQ(edges.getEdges().size(), 4u);
  EXPECT_TRUE(edges.selectEdges(1).empty());
  EXPECT_TRUE(edges.selectEdges(2).empty());
  EXPECT_EQ(edges.selectEdges(3).size(), 4u);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 3u);
  for (std::size_t i = 0; i < rebuilt.size(); ++i) {
    EXPECT_TRUE(rebuilt[i]->equals(geoms[i].get()));
  }
}

TEST_F(CoverageRingEdgesTest, PreservesZCoordinatesAcrossRebuild) {
  auto geoms = ReadAll({
      "POLYGON Z ((0 0 10, 10 0 20, 10 10 30, 0 10 40, 0 0 10))",
      "POLYGON Z ((10 0 20, 20 0 50, 20 10 60, 10 10 30, 10 0 20))",
  });
  std::vector<const Geometry*> coverage = AsConstPtrs(geoms);
  CoverageRingEdges edges(coverage);

  auto rebuilt = edges.buildCoverage();
  ASSERT_EQ(rebuilt.size(), 2u);
  EXPECT_TRUE(rebuilt[0]->hasZ());
  EXPECT_TRUE(rebuilt[1]->hasZ());
  EXPECT_TRUE(rebuilt[0]->equals(geoms[0].get()));
  EXPECT_TRUE(rebuilt[1]->equals(geoms[1].get()));

  auto coords0 = rebuilt[0]->getCoordinates();
  bool foundZ40 = false;
  for (std::size_t i = 0; i < coords0->size(); ++i) {
    const Coordinate& c = coords0->getAt(i);
    EXPECT_FALSE(std::isnan(c.z));
    if (c.equals2D(Coordinate(0, 10))) {
      EXPECT_DOUBLE_EQ(c.z, 40.0);
      foundZ40 = true;
    }
  }
  EXPECT_TRUE(foundZ40);
}

TEST_F(CoverageRingEdgesTest, CurvedGeometryThrowsUnsupportedOperation) {
  auto curvePoly =
      Read("CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))");
  std::vector<const Geometry*> curveCov = {curvePoly.get()};
  EXPECT_THROW(CoverageRingEdges edges(curveCov),
               geos::util::UnsupportedOperationException);

  auto multiSurface = Read(
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 "
      "0)))");
  std::vector<const Geometry*> msCov = {multiSurface.get()};
  EXPECT_THROW(CoverageRingEdges edges(msCov),
               geos::util::UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
