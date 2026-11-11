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

#include "third_party/geos/include/geos/triangulate/VoronoiDiagramBuilder.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryCollection;
using ::geos::geom::GeometryFactory;
using ::geos::io::WKTReader;
using ::geos::triangulate::VoronoiDiagramBuilder;

constexpr double kEps = 1e-6;

class VoronoiDiagramBuilderTest : public ::testing::Test {
 protected:
  VoronoiDiagramBuilderTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  static double TotalArea(const GeometryCollection& collection) {
    double area = 0.0;
    for (std::size_t i = 0; i < collection.getNumGeometries(); ++i) {
      area += collection.getGeometryN(i)->getArea();
    }
    return area;
  }

  static void ExpectValidVoronoiCells(const GeometryCollection& diagram,
                                      std::size_t expected_count,
                                      const Envelope& expected_env,
                                      double expected_total_area) {
    ASSERT_EQ(diagram.getGeometryTypeId(),
              ::geos::geom::GEOS_GEOMETRYCOLLECTION);
    ASSERT_EQ(diagram.getNumGeometries(), expected_count);

    for (std::size_t i = 0; i < diagram.getNumGeometries(); ++i) {
      const Geometry* cell = diagram.getGeometryN(i);
      ASSERT_NE(cell, nullptr);
      EXPECT_EQ(cell->getGeometryTypeId(), ::geos::geom::GEOS_POLYGON);
      EXPECT_EQ(cell->getUserData(), nullptr);
      EXPECT_TRUE(cell->isValid()) << "cell " << i << " invalid";
      EXPECT_GT(cell->getArea(), 0.0) << "cell " << i << " empty area";
      const Envelope* env = cell->getEnvelopeInternal();
      EXPECT_GE(env->getMinX(), expected_env.getMinX() - kEps);
      EXPECT_LE(env->getMaxX(), expected_env.getMaxX() + kEps);
      EXPECT_GE(env->getMinY(), expected_env.getMinY() - kEps);
      EXPECT_LE(env->getMaxY(), expected_env.getMaxY() + kEps);
    }

    for (std::size_t i = 0; i < diagram.getNumGeometries(); ++i) {
      for (std::size_t j = i + 1; j < diagram.getNumGeometries(); ++j) {
        EXPECT_FALSE(diagram.getGeometryN(i)->overlaps(diagram.getGeometryN(j)))
            << "cells " << i << " and " << j << " overlap";
      }
    }

    EXPECT_NEAR(TotalArea(diagram), expected_total_area, kEps);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(VoronoiDiagramBuilderTest, EmptyGeometryInputsReturnEmptyOutputs) {
  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",   "MULTIPOINT EMPTY",         "LINESTRING EMPTY",
      "POLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  for (const std::string& wkt : empty_wkts) {
    auto geom = reader_.read(wkt);
    ASSERT_NE(geom, nullptr);

    VoronoiDiagramBuilder builder;
    builder.setSites(*geom);

    auto diagram = builder.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr);
    EXPECT_TRUE(diagram->isEmpty()) << wkt;
    EXPECT_EQ(diagram->getNumGeometries(), 0u) << wkt;

    auto edges = builder.getDiagramEdges(*factory_);
    ASSERT_NE(edges, nullptr);
    EXPECT_TRUE(edges->isEmpty()) << wkt;
    EXPECT_EQ(edges->getNumGeometries(), 0u) << wkt;

    EXPECT_EQ(builder.getSubdivision(), nullptr) << wkt;
  }
}

TEST_F(VoronoiDiagramBuilderTest, EmptyCoordinateSequenceReturnsEmptyOutputs) {
  CoordinateSequence empty_seq(0u, 2u);
  VoronoiDiagramBuilder builder;
  builder.setSites(empty_seq);

  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  EXPECT_TRUE(diagram->isEmpty());
  EXPECT_EQ(diagram->getNumGeometries(), 0u);

  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_TRUE(edges->isEmpty());
  EXPECT_EQ(edges->getNumGeometries(), 0u);

  EXPECT_EQ(builder.getSubdivision(), nullptr);
}

TEST_F(VoronoiDiagramBuilderTest, CurvedGeometriesThrowUnsupportedOperation) {
  const std::vector<std::string> curved_wkts = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 2 0, 2 2, 0 2, 0 0))",
  };

  for (const std::string& wkt : curved_wkts) {
    auto geom = reader_.read(wkt);
    ASSERT_NE(geom, nullptr);

    VoronoiDiagramBuilder builder;
    EXPECT_THROW(builder.setSites(*geom),
                 ::geos::util::UnsupportedOperationException)
        << wkt;
  }
}

TEST_F(VoronoiDiagramBuilderTest, SingleSiteWithoutClipEnvelopeReturnsEmpty) {
  auto pt = reader_.read("POINT (5 5)");
  VoronoiDiagramBuilder builder;
  builder.setSites(*pt);

  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  EXPECT_TRUE(diagram->isEmpty());
  EXPECT_EQ(diagram->getNumGeometries(), 0u);

  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_TRUE(edges->isEmpty());
  EXPECT_EQ(edges->getNumGeometries(), 0u);

  auto subdiv = builder.getSubdivision();
  EXPECT_NE(subdiv, nullptr);
}

TEST_F(VoronoiDiagramBuilderTest, SingleSiteWithClipEnvelopeCreatesBoxCell) {
  auto pt = reader_.read("POINT (5 5)");
  Envelope clip_env(0.0, 10.0, 0.0, 10.0);

  VoronoiDiagramBuilder builder;
  builder.setSites(*pt);
  builder.setClipEnvelope(&clip_env);
  builder.setOrdered(true);

  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, 1u, clip_env, 100.0);
  EXPECT_TRUE(diagram->getGeometryN(0)->covers(pt.get()));

  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_TRUE(edges->isEmpty());
}

TEST_F(VoronoiDiagramBuilderTest, TwoSitesProduceBisectorAndTwoHalfCells) {
  auto sites = reader_.read("MULTIPOINT ((0 0), (10 0))");
  VoronoiDiagramBuilder builder;
  builder.setSites(*sites);

  // Site envelope is [0, 10] x [0, 0], expanded by max(10, 0) = 10
  // -> [-10, 20] x [-10, 10] (area = 30 * 20 = 600).
  const Envelope expected_env(-10.0, 20.0, -10.0, 10.0);

  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, 2u, expected_env, 600.0);

  // Edges clip down to a single LineString bisector along x = 5, y in [-10,
  // 10].
  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_EQ(edges->getGeometryTypeId(), ::geos::geom::GEOS_MULTILINESTRING);
  EXPECT_FALSE(edges->isEmpty());
  EXPECT_EQ(edges->getNumGeometries(), 1u);
  EXPECT_NEAR(edges->getLength(), 20.0, kEps);
  EXPECT_NEAR(edges->getEnvelopeInternal()->getMinX(), 5.0, kEps);
  EXPECT_NEAR(edges->getEnvelopeInternal()->getMaxX(), 5.0, kEps);
  EXPECT_NEAR(edges->getEnvelopeInternal()->getMinY(), -10.0, kEps);
  EXPECT_NEAR(edges->getEnvelopeInternal()->getMaxY(), 10.0, kEps);
}

TEST_F(VoronoiDiagramBuilderTest,
       SquareWithCenterCoversInteriorAndClippedCells) {
  auto sites =
      reader_.read("MULTIPOINT ((0 0), (10 0), (10 10), (0 10), (5 5))");
  VoronoiDiagramBuilder builder;
  builder.setSites(*sites);
  builder.setOrdered(true);

  // Site envelope [0, 10] x [0, 10] expanded by 10 -> [-10, 20] x [-10, 20].
  const Envelope expected_env(-10.0, 20.0, -10.0, 20.0);

  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, 5u, expected_env, 900.0);

  // Center site (5, 5) has an unclipped diamond cell bounded by
  // (5, 0), (10, 5), (5, 10), (0, 5) of area 50.0.
  auto center_pt = factory_->createPoint(CoordinateXY(5.0, 5.0));
  const Geometry* center_cell = diagram->getGeometryN(4);
  EXPECT_TRUE(center_cell->covers(center_pt.get()));
  EXPECT_NEAR(center_cell->getArea(), 50.0, kEps);

  // Diagram edges contain multiple segments.
  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_EQ(edges->getGeometryTypeId(), ::geos::geom::GEOS_MULTILINESTRING);
  EXPECT_FALSE(edges->isEmpty());
  EXPECT_GE(edges->getNumGeometries(), 4u);
}

TEST_F(VoronoiDiagramBuilderTest,
       ClipEnvelopeBehaviorForLargerSmallerAndReset) {
  auto sites = reader_.read("MULTIPOINT ((0 0), (10 0), (10 10), (0 10))");

  // Larger clip envelope expands diagram bounds to [-20, 30] x [-20, 30].
  Envelope large_env(-20.0, 30.0, -20.0, 30.0);
  {
    VoronoiDiagramBuilder builder;
    builder.setSites(*sites);
    builder.setClipEnvelope(&large_env);
    auto diagram = builder.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr);
    ExpectValidVoronoiCells(*diagram, 4u, large_env, 2500.0);
    EXPECT_NEAR(diagram->getEnvelopeInternal()->getMinX(), -20.0, kEps);
    EXPECT_NEAR(diagram->getEnvelopeInternal()->getMaxX(), 30.0, kEps);
  }

  // Smaller clip envelope is subsumed by expanded site envelope [-10, 20]^2.
  Envelope small_env(2.0, 8.0, 2.0, 8.0);
  const Envelope default_env(-10.0, 20.0, -10.0, 20.0);
  {
    VoronoiDiagramBuilder builder;
    builder.setSites(*sites);
    builder.setClipEnvelope(&small_env);
    auto diagram = builder.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr);
    ExpectValidVoronoiCells(*diagram, 4u, default_env, 900.0);
  }

  // Setting clip envelope and then clearing with nullptr uses default bounds.
  {
    VoronoiDiagramBuilder builder;
    builder.setSites(*sites);
    builder.setClipEnvelope(&large_env);
    builder.setClipEnvelope(nullptr);
    auto diagram = builder.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr);
    ExpectValidVoronoiCells(*diagram, 4u, default_env, 900.0);
  }
}

TEST_F(VoronoiDiagramBuilderTest, ReuseBuilderCachesAndTransfersSubdivision) {
  auto sites = reader_.read("MULTIPOINT ((0 0), (10 0), (5 10), (5 3))");
  VoronoiDiagramBuilder builder;
  builder.setSites(*sites);

  auto diagram1 = builder.getDiagram(*factory_);
  auto diagram2 = builder.getDiagram(*factory_);
  ASSERT_NE(diagram1, nullptr);
  ASSERT_NE(diagram2, nullptr);
  EXPECT_EQ(diagram1->getNumGeometries(), 4u);
  EXPECT_EQ(diagram2->getNumGeometries(), 4u);
  EXPECT_NEAR(TotalArea(*diagram1), TotalArea(*diagram2), kEps);

  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_FALSE(edges->isEmpty());

  auto subdiv = builder.getSubdivision();
  ASSERT_NE(subdiv, nullptr);
  auto triangles = subdiv->getTriangles(*factory_);
  ASSERT_NE(triangles, nullptr);
  EXPECT_EQ(triangles->getNumGeometries(), 3u);

  // After getSubdivision() moves ownership out, calling getDiagram recreates
  // it.
  auto diagram3 = builder.getDiagram(*factory_);
  ASSERT_NE(diagram3, nullptr);
  EXPECT_EQ(diagram3->getNumGeometries(), 4u);
  EXPECT_NEAR(TotalArea(*diagram3), TotalArea(*diagram1), kEps);
}

TEST_F(VoronoiDiagramBuilderTest, DuplicateSitesAreDeduplicatedWhenUnordered) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),   CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
      CoordinateXY(0.0, 0.0),
  };
  VoronoiDiagramBuilder builder;
  builder.setSites(seq);
  builder.setOrdered(false);

  const Envelope expected_env(-10.0, 20.0, -10.0, 20.0);
  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, 4u, expected_env, 900.0);

  // Also verify Polygon sites (closed ring repeats start vertex).
  auto poly = reader_.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  VoronoiDiagramBuilder poly_builder;
  poly_builder.setSites(*poly);
  auto poly_diagram = poly_builder.getDiagram(*factory_);
  ASSERT_NE(poly_diagram, nullptr);
  ExpectValidVoronoiCells(*poly_diagram, 4u, expected_env, 900.0);
}

TEST_F(VoronoiDiagramBuilderTest, SetToleranceSnapsNearbySites) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.05, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(5.0, 10.0),
  };

  {
    VoronoiDiagramBuilder no_tol;
    no_tol.setSites(seq);
    no_tol.setTolerance(0.0);
    auto diagram = no_tol.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr);
    EXPECT_EQ(diagram->getNumGeometries(), 4u);
  }

  {
    VoronoiDiagramBuilder with_tol;
    with_tol.setSites(seq);
    with_tol.setTolerance(0.5);
    auto diagram = with_tol.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr);
    EXPECT_EQ(diagram->getNumGeometries(), 3u);
  }
}

TEST_F(VoronoiDiagramBuilderTest, OrderedOutputFromCoordinateSequence) {
  CoordinateSequence seq = {
      CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 0.0),  CoordinateXY(5.0, 5.0),
      CoordinateXY(10.0, 0.0),  CoordinateXY(0.0, 10.0),
  };

  VoronoiDiagramBuilder builder;
  builder.setSites(seq);
  builder.setOrdered(true);

  const Envelope expected_env(-10.0, 20.0, -10.0, 20.0);
  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, seq.size(), expected_env, 900.0);

  for (std::size_t i = 0; i < seq.size(); ++i) {
    auto pt = factory_->createPoint(seq.getAt<CoordinateXY>(i));
    EXPECT_TRUE(diagram->getGeometryN(i)->covers(pt.get()))
        << "cell " << i << " did not cover input site";
  }
}

TEST_F(VoronoiDiagramBuilderTest, OrderedOutputFromDiverseGeometryTypes) {
  const std::vector<std::string> wkts = {
      "LINESTRING (10 10, 0 0, 10 0, 0 10)",
      "MULTIPOINT ((5 5), (10 0), (0 10), (0 0), (10 10))",
      "MULTILINESTRING ((10 10, 0 0), (10 0, 0 10))",
      "GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (10 10, 0 0), "
      "MULTIPOINT ((10 0), (0 10)))",
  };

  const Envelope expected_env(-10.0, 20.0, -10.0, 20.0);
  for (const std::string& wkt : wkts) {
    auto geom = reader_.read(wkt);
    ASSERT_NE(geom, nullptr) << wkt;
    auto coords = geom->getCoordinates();
    ASSERT_NE(coords, nullptr);

    VoronoiDiagramBuilder builder;
    builder.setSites(*geom);
    builder.setOrdered(true);

    auto diagram = builder.getDiagram(*factory_);
    ASSERT_NE(diagram, nullptr) << wkt;
    ExpectValidVoronoiCells(*diagram, coords->size(), expected_env, 900.0);

    for (std::size_t i = 0; i < coords->size(); ++i) {
      auto pt = factory_->createPoint(coords->getAt<CoordinateXY>(i));
      EXPECT_TRUE(diagram->getGeometryN(i)->covers(pt.get()))
          << wkt << " cell " << i;
    }
  }
}

TEST_F(VoronoiDiagramBuilderTest, OrderedOutputThrowsOnDuplicateInputSites) {
  {
    CoordinateSequence seq = {
        CoordinateXY(0.0, 0.0),
        CoordinateXY(10.0, 0.0),
        CoordinateXY(5.0, 10.0),
        CoordinateXY(0.0, 0.0),
    };
    VoronoiDiagramBuilder builder;
    builder.setSites(seq);
    builder.setOrdered(true);
    EXPECT_THROW(builder.getDiagram(*factory_), ::geos::util::GEOSException);
  }

  {
    auto closed_line = reader_.read("LINESTRING (0 0, 10 0, 5 10, 0 0)");
    VoronoiDiagramBuilder builder;
    builder.setSites(*closed_line);
    builder.setOrdered(true);
    EXPECT_THROW(builder.getDiagram(*factory_), ::geos::util::GEOSException);
  }
}

TEST_F(VoronoiDiagramBuilderTest, OrderedOutputThrowsWhenToleranceDropsSite) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.05, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(5.0, 10.0),
  };

  VoronoiDiagramBuilder builder;
  builder.setSites(seq);
  builder.setTolerance(0.5);
  builder.setOrdered(true);

  EXPECT_THROW(builder.getDiagram(*factory_), ::geos::util::GEOSException);
}

TEST_F(VoronoiDiagramBuilderTest,
       CollinearSitesProduceStripCellsAndParallelEdges) {
  auto sites = reader_.read("MULTIPOINT ((0 0), (4 0), (10 0))");
  VoronoiDiagramBuilder builder;
  builder.setSites(*sites);
  builder.setOrdered(true);

  // [0, 10] x [0, 0] expanded by 10 -> [-10, 20] x [-10, 10], area = 600.
  const Envelope expected_env(-10.0, 20.0, -10.0, 10.0);
  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, 3u, expected_env, 600.0);

  // Bisectors at x = 2 and x = 7 split width 30 into 12, 5, and 13 -> areas
  // 240, 100, 260.
  EXPECT_NEAR(diagram->getGeometryN(0)->getArea(), 240.0, kEps);
  EXPECT_NEAR(diagram->getGeometryN(1)->getArea(), 100.0, kEps);
  EXPECT_NEAR(diagram->getGeometryN(2)->getArea(), 260.0, kEps);

  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_EQ(edges->getNumGeometries(), 2u);
  EXPECT_NEAR(edges->getLength(), 40.0, kEps);
}

TEST_F(VoronoiDiagramBuilderTest, DirectReorderCellsToInput) {
  CoordinateSequence seq = {
      CoordinateXY(10.0, 10.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(0.0, 10.0),
  };
  VoronoiDiagramBuilder builder;
  builder.setSites(seq);

  auto subdiv = builder.getSubdivision();
  ASSERT_NE(subdiv, nullptr);
  auto polys = subdiv->getVoronoiCellPolygons(*factory_);
  ASSERT_EQ(polys.size(), seq.size());

  builder.reorderCellsToInput(polys);
  ASSERT_EQ(polys.size(), seq.size());
  for (std::size_t i = 0; i < seq.size(); ++i) {
    auto pt = factory_->createPoint(seq.getAt<CoordinateXY>(i));
    EXPECT_TRUE(polys[i]->covers(pt.get())) << "index " << i;
    polys[i]->setUserData(nullptr);
  }
}

TEST_F(VoronoiDiagramBuilderTest, SitesWithZCoordinatesProduceValid2DDiagram) {
  CoordinateSequence seq = {
      Coordinate(0.0, 0.0, 1.0),
      Coordinate(10.0, 0.0, 2.0),
      Coordinate(5.0, 10.0, 3.0),
  };
  VoronoiDiagramBuilder builder;
  builder.setSites(seq);

  const Envelope expected_env(-10.0, 20.0, -10.0, 20.0);
  auto diagram = builder.getDiagram(*factory_);
  ASSERT_NE(diagram, nullptr);
  ExpectValidVoronoiCells(*diagram, 3u, expected_env, 900.0);

  auto edges = builder.getDiagramEdges(*factory_);
  ASSERT_NE(edges, nullptr);
  EXPECT_FALSE(edges->isEmpty());
}

TEST_F(VoronoiDiagramBuilderTest, InfiniteCoordinateThrowsException) {
  const double inf = std::numeric_limits<double>::infinity();
  CoordinateSequence seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(inf, 5.0),
  });
  VoronoiDiagramBuilder builder;
  builder.setSites(seq);

  EXPECT_THROW(builder.getDiagram(*factory_), ::geos::util::GEOSException);
}

}  // namespace
}  // namespace geos_google
