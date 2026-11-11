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

#include "third_party/geos/include/geos/triangulate/DelaunayTriangulationBuilder.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/triangulate/IncrementalDelaunayTriangulator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::triangulate::DelaunayTriangulationBuilder;
using geos::triangulate::IncrementalDelaunayTriangulator;

class DelaunayTriangulationBuilderTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(DelaunayTriangulationBuilderTest,
       ExtractUniqueCoordinatesRemovesDuplicatesAndSorts) {
  auto multi_pt = factory_->createMultiPoint(CoordinateSequence{
      CoordinateXY{2.0, 1.0}, CoordinateXY{0.0, 2.0}, CoordinateXY{2.0, 1.0},
      CoordinateXY{0.0, 0.0}, CoordinateXY{0.0, 2.0}});

  auto unique_coords =
      DelaunayTriangulationBuilder::extractUniqueCoordinates(*multi_pt);
  ASSERT_NE(unique_coords, nullptr);
  ASSERT_EQ(unique_coords->size(), 3u);
  EXPECT_EQ(unique_coords->getAt<CoordinateXY>(0), (CoordinateXY{0.0, 0.0}));
  EXPECT_EQ(unique_coords->getAt<CoordinateXY>(1), (CoordinateXY{0.0, 2.0}));
  EXPECT_EQ(unique_coords->getAt<CoordinateXY>(2), (CoordinateXY{2.0, 1.0}));

  auto empty_pt = factory_->createPoint();
  auto empty_coords =
      DelaunayTriangulationBuilder::extractUniqueCoordinates(*empty_pt);
  ASSERT_NE(empty_coords, nullptr);
  EXPECT_TRUE(empty_coords->isEmpty());
}

TEST_F(DelaunayTriangulationBuilderTest,
       UniqueSortsWithAndWithoutRepeatedPoints) {
  CoordinateSequence with_dups{CoordinateXY{3.0, 2.0}, CoordinateXY{1.0, 5.0},
                               CoordinateXY{3.0, 2.0}, CoordinateXY{1.0, 1.0},
                               CoordinateXY{1.0, 5.0}};
  auto deduped = DelaunayTriangulationBuilder::unique(&with_dups);
  ASSERT_NE(deduped, nullptr);
  ASSERT_EQ(deduped->size(), 3u);
  EXPECT_EQ(deduped->getAt<CoordinateXY>(0), (CoordinateXY{1.0, 1.0}));
  EXPECT_EQ(deduped->getAt<CoordinateXY>(1), (CoordinateXY{1.0, 5.0}));
  EXPECT_EQ(deduped->getAt<CoordinateXY>(2), (CoordinateXY{3.0, 2.0}));

  CoordinateSequence already_unique{
      CoordinateXY{3.0, 2.0}, CoordinateXY{1.0, 5.0}, CoordinateXY{1.0, 1.0}};
  auto sorted_unique = DelaunayTriangulationBuilder::unique(&already_unique);
  ASSERT_NE(sorted_unique, nullptr);
  ASSERT_EQ(sorted_unique->size(), 3u);
  EXPECT_EQ(sorted_unique->getAt<CoordinateXY>(0), (CoordinateXY{1.0, 1.0}));
  EXPECT_EQ(sorted_unique->getAt<CoordinateXY>(1), (CoordinateXY{1.0, 5.0}));
  EXPECT_EQ(sorted_unique->getAt<CoordinateXY>(2), (CoordinateXY{3.0, 2.0}));

  CoordinateSequence empty_seq;
  auto empty_unique = DelaunayTriangulationBuilder::unique(&empty_seq);
  ASSERT_NE(empty_unique, nullptr);
  EXPECT_TRUE(empty_unique->isEmpty());

  CoordinateSequence single_seq{Coordinate{7.0, -2.0, 4.5}};
  auto single_unique = DelaunayTriangulationBuilder::unique(&single_seq);
  ASSERT_NE(single_unique, nullptr);
  ASSERT_EQ(single_unique->size(), 1u);
  EXPECT_DOUBLE_EQ(single_unique->getAt<Coordinate>(0).x, 7.0);
  EXPECT_DOUBLE_EQ(single_unique->getAt<Coordinate>(0).y, -2.0);
  EXPECT_DOUBLE_EQ(single_unique->getAt<Coordinate>(0).z, 4.5);
}

TEST_F(DelaunayTriangulationBuilderTest,
       ToVerticesConvertsEmptyAndXYZSequences) {
  CoordinateSequence empty_seq;
  IncrementalDelaunayTriangulator::VertexList empty_verts =
      DelaunayTriangulationBuilder::toVertices(empty_seq);
  EXPECT_TRUE(empty_verts.empty());

  CoordinateSequence coords{Coordinate{1.0, 2.0, 3.5},
                            Coordinate{4.0, 5.0, 6.25}};
  IncrementalDelaunayTriangulator::VertexList verts =
      DelaunayTriangulationBuilder::toVertices(coords);
  ASSERT_EQ(verts.size(), 2u);
  EXPECT_DOUBLE_EQ(verts[0].getX(), 1.0);
  EXPECT_DOUBLE_EQ(verts[0].getY(), 2.0);
  EXPECT_DOUBLE_EQ(verts[0].getZ(), 3.5);
  EXPECT_DOUBLE_EQ(verts[1].getX(), 4.0);
  EXPECT_DOUBLE_EQ(verts[1].getY(), 5.0);
  EXPECT_DOUBLE_EQ(verts[1].getZ(), 6.25);
}

TEST_F(DelaunayTriangulationBuilderTest,
       EnvelopeComputesBoundsForEmptySingleAndMultiCoords) {
  CoordinateSequence empty_seq;
  EXPECT_TRUE(DelaunayTriangulationBuilder::envelope(empty_seq).isNull());

  CoordinateSequence single_seq{CoordinateXY{3.0, -4.0}};
  Envelope single_env = DelaunayTriangulationBuilder::envelope(single_seq);
  EXPECT_FALSE(single_env.isNull());
  EXPECT_DOUBLE_EQ(single_env.getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(single_env.getMaxX(), 3.0);
  EXPECT_DOUBLE_EQ(single_env.getMinY(), -4.0);
  EXPECT_DOUBLE_EQ(single_env.getMaxY(), -4.0);

  CoordinateSequence multi_seq{CoordinateXY{1.0, 5.0}, CoordinateXY{-2.0, 3.0},
                               CoordinateXY{4.0, -1.0}};
  Envelope multi_env = DelaunayTriangulationBuilder::envelope(multi_seq);
  EXPECT_DOUBLE_EQ(multi_env.getMinX(), -2.0);
  EXPECT_DOUBLE_EQ(multi_env.getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(multi_env.getMinY(), -1.0);
  EXPECT_DOUBLE_EQ(multi_env.getMaxY(), 5.0);
}

TEST_F(DelaunayTriangulationBuilderTest,
       UnsetOrEmptySitesProduceEmptyEdgesAndTriangles) {
  DelaunayTriangulationBuilder unset_builder;
  auto unset_edges = unset_builder.getEdges(*factory_);
  auto unset_tris = unset_builder.getTriangles(*factory_);
  ASSERT_NE(unset_edges, nullptr);
  ASSERT_NE(unset_tris, nullptr);
  EXPECT_TRUE(unset_edges->isEmpty());
  EXPECT_EQ(unset_edges->getNumGeometries(), 0u);
  EXPECT_TRUE(unset_tris->isEmpty());
  EXPECT_EQ(unset_tris->getNumGeometries(), 0u);

  DelaunayTriangulationBuilder empty_geom_builder;
  auto empty_mp = factory_->createMultiPoint();
  empty_geom_builder.setSites(*empty_mp);
  EXPECT_TRUE(empty_geom_builder.getEdges(*factory_)->isEmpty());
  EXPECT_TRUE(empty_geom_builder.getTriangles(*factory_)->isEmpty());

  DelaunayTriangulationBuilder empty_seq_builder;
  CoordinateSequence empty_seq;
  empty_seq_builder.setSites(empty_seq);
  EXPECT_TRUE(empty_seq_builder.getEdges(*factory_)->isEmpty());
  EXPECT_TRUE(empty_seq_builder.getTriangles(*factory_)->isEmpty());
}

TEST_F(DelaunayTriangulationBuilderTest, SetSitesRejectsCurvedGeometry) {
  auto circular = factory_->createCircularString(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 0.0}});
  DelaunayTriangulationBuilder builder;
  EXPECT_THROW(builder.setSites(*circular),
               geos::util::UnsupportedOperationException);
}

TEST_F(DelaunayTriangulationBuilderTest,
       TwoAndCollinearSitesProduceEdgesWithoutTriangles) {
  DelaunayTriangulationBuilder two_builder;
  two_builder.setSites(
      CoordinateSequence{CoordinateXY{0.0, 0.0}, CoordinateXY{10.0, 0.0}});
  auto two_edges = two_builder.getEdges(*factory_);
  auto two_tris = two_builder.getTriangles(*factory_);
  EXPECT_EQ(two_edges->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(two_edges->getLength(), 10.0);
  EXPECT_EQ(two_tris->getNumGeometries(), 0u);

  DelaunayTriangulationBuilder collinear_builder;
  auto line = factory_->createLineString(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 0.0}, CoordinateXY{10.0, 0.0}});
  collinear_builder.setSites(*line);
  auto collinear_edges = collinear_builder.getEdges(*factory_);
  auto collinear_tris = collinear_builder.getTriangles(*factory_);
  EXPECT_EQ(collinear_edges->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(collinear_edges->getLength(), 10.0);
  EXPECT_EQ(collinear_tris->getNumGeometries(), 0u);
}

TEST_F(DelaunayTriangulationBuilderTest,
       TriangleFromPolygonAndRepeatedQueriesReuseSubdivision) {
  auto poly = factory_->createPolygon(
      CoordinateSequence{CoordinateXY{0.0, 0.0}, CoordinateXY{6.0, 0.0},
                         CoordinateXY{0.0, 4.0}, CoordinateXY{0.0, 0.0}});

  DelaunayTriangulationBuilder builder;
  builder.setSites(*poly);

  auto tris = builder.getTriangles(*factory_);
  ASSERT_EQ(tris->getNumGeometries(), 1u);
  EXPECT_TRUE(tris->getGeometryN(0)->isValid());
  EXPECT_DOUBLE_EQ(tris->getArea(), 12.0);

  auto edges = builder.getEdges(*factory_);
  ASSERT_EQ(edges->getNumGeometries(), 3u);
  EXPECT_NEAR(edges->getLength(), 6.0 + 4.0 + std::hypot(6.0, 4.0), 1e-12);

  geos::triangulate::quadedge::QuadEdgeSubdivision& subdiv =
      builder.getSubdivision();
  EXPECT_DOUBLE_EQ(subdiv.getTolerance(), 0.0);
  EXPECT_NE(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{6.0, 0.0}), nullptr);
  EXPECT_NE(subdiv.locate(Coordinate{6.0, 0.0}, Coordinate{0.0, 4.0}), nullptr);
  EXPECT_NE(subdiv.locate(Coordinate{0.0, 4.0}, Coordinate{0.0, 0.0}), nullptr);
}

TEST_F(DelaunayTriangulationBuilderTest,
       ThinDiamondAndGridSatisfyDelaunayCondition) {
  DelaunayTriangulationBuilder diamond_builder;
  diamond_builder.setSites(
      CoordinateSequence{CoordinateXY{0.0, 0.0}, CoordinateXY{5.0, 1.0},
                         CoordinateXY{10.0, 0.0}, CoordinateXY{5.0, -1.0}});

  auto diamond_tris = diamond_builder.getTriangles(*factory_);
  auto diamond_edges = diamond_builder.getEdges(*factory_);
  EXPECT_EQ(diamond_tris->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(diamond_tris->getArea(), 10.0);
  EXPECT_EQ(diamond_edges->getNumGeometries(), 5u);

  // Delaunay selects the short diagonal (5,-1)-(5,1) over (0,0)-(10,0).
  auto& subdiv = diamond_builder.getSubdivision();
  EXPECT_NE(subdiv.locate(Coordinate{5.0, -1.0}, Coordinate{5.0, 1.0}),
            nullptr);
  EXPECT_EQ(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{10.0, 0.0}),
            nullptr);

  CoordinateSequence grid_coords;
  for (int x = 0; x < 3; ++x) {
    for (int y = 0; y < 3; ++y) {
      grid_coords.add(
          CoordinateXY{static_cast<double>(x), static_cast<double>(y)});
    }
  }
  DelaunayTriangulationBuilder grid_builder;
  grid_builder.setSites(grid_coords);
  auto grid_tris = grid_builder.getTriangles(*factory_);
  auto grid_edges = grid_builder.getEdges(*factory_);
  EXPECT_EQ(grid_tris->getNumGeometries(), 8u);
  EXPECT_EQ(grid_edges->getNumGeometries(), 16u);
  EXPECT_DOUBLE_EQ(grid_tris->getArea(), 4.0);
  for (std::size_t i = 0; i < grid_tris->getNumGeometries(); ++i) {
    EXPECT_TRUE(grid_tris->getGeometryN(i)->isValid());
    EXPECT_DOUBLE_EQ(grid_tris->getGeometryN(i)->getArea(), 0.5);
  }
}

TEST_F(DelaunayTriangulationBuilderTest, SetToleranceSnapsNearbySites) {
  CoordinateSequence sites{CoordinateXY{0.0, 0.0}, CoordinateXY{0.05, 0.05},
                           CoordinateXY{10.0, 0.0}, CoordinateXY{5.0, 10.0}};

  DelaunayTriangulationBuilder exact_builder;
  exact_builder.setSites(sites);
  auto exact_tris = exact_builder.getTriangles(*factory_);
  auto exact_edges = exact_builder.getEdges(*factory_);
  EXPECT_EQ(exact_tris->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(exact_tris->getArea(), 50.0);
  EXPECT_EQ(exact_edges->getNumGeometries(), 6u);

  DelaunayTriangulationBuilder snapped_builder;
  snapped_builder.setTolerance(0.2);
  snapped_builder.setSites(sites);
  auto snapped_tris = snapped_builder.getTriangles(*factory_);
  auto snapped_edges = snapped_builder.getEdges(*factory_);
  EXPECT_DOUBLE_EQ(snapped_builder.getSubdivision().getTolerance(), 0.2);
  EXPECT_EQ(snapped_tris->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(snapped_tris->getArea(), 50.0);
  EXPECT_EQ(snapped_edges->getNumGeometries(), 3u);
}

TEST_F(DelaunayTriangulationBuilderTest,
       SingleSiteProducesEmptyEdgesAndTriangles) {
  auto single_pt = factory_->createPoint(CoordinateXY{5.0, 5.0});
  DelaunayTriangulationBuilder builder;
  builder.setSites(*single_pt);

  auto edges = builder.getEdges(*factory_);
  auto tris = builder.getTriangles(*factory_);
  EXPECT_TRUE(edges->isEmpty());
  EXPECT_EQ(edges->getNumGeometries(), 0u);
  EXPECT_TRUE(tris->isEmpty());
  EXPECT_EQ(tris->getNumGeometries(), 0u);
}

TEST_F(DelaunayTriangulationBuilderTest,
       ExtractsSitesFromGeometryCollectionAndPolygonWithHole) {
  std::vector<std::unique_ptr<geos::geom::Geometry>> parts;
  parts.push_back(factory_->createPoint(CoordinateXY{1.0, 2.0}));
  parts.push_back(factory_->createLineString(
      CoordinateSequence{CoordinateXY{1.0, 2.0}, CoordinateXY{4.0, 0.0}}));
  parts.push_back(factory_->createPolygon(
      CoordinateSequence{CoordinateXY{0.0, 0.0}, CoordinateXY{4.0, 0.0},
                         CoordinateXY{0.0, 3.0}, CoordinateXY{0.0, 0.0}}));
  auto collection = factory_->createGeometryCollection(std::move(parts));

  auto extracted =
      DelaunayTriangulationBuilder::extractUniqueCoordinates(*collection);
  ASSERT_EQ(extracted->size(), 4u);

  DelaunayTriangulationBuilder collection_builder;
  collection_builder.setSites(*collection);
  auto collection_tris = collection_builder.getTriangles(*factory_);
  auto collection_edges = collection_builder.getEdges(*factory_);
  EXPECT_EQ(collection_tris->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(collection_tris->getArea(), 6.0);
  EXPECT_EQ(collection_edges->getNumGeometries(), 6u);

  auto shell = factory_->createLinearRing(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{10.0, 0.0}, CoordinateXY{10.0, 10.0},
      CoordinateXY{0.0, 10.0}, CoordinateXY{0.0, 0.0}});
  std::vector<std::unique_ptr<geos::geom::LinearRing>> holes;
  holes.push_back(factory_->createLinearRing(CoordinateSequence{
      CoordinateXY{2.0, 5.0}, CoordinateXY{4.0, 7.0}, CoordinateXY{6.0, 5.0},
      CoordinateXY{4.0, 3.0}, CoordinateXY{2.0, 5.0}}));
  auto poly_with_hole =
      factory_->createPolygon(std::move(shell), std::move(holes));

  DelaunayTriangulationBuilder hole_builder;
  hole_builder.setSites(*poly_with_hole);
  auto hole_tris = hole_builder.getTriangles(*factory_);
  auto hole_edges = hole_builder.getEdges(*factory_);
  EXPECT_EQ(hole_tris->getNumGeometries(), 10u);
  EXPECT_DOUBLE_EQ(hole_tris->getArea(), 100.0);
  EXPECT_EQ(hole_edges->getNumGeometries(), 17u);
  for (std::size_t i = 0; i < hole_tris->getNumGeometries(); ++i) {
    EXPECT_TRUE(hole_tris->getGeometryN(i)->isValid());
  }
}

TEST_F(DelaunayTriangulationBuilderTest,
       SetSitesReplacesPreviousSitesAndGetSubdivisionBuildsFirst) {
  DelaunayTriangulationBuilder builder;
  auto initial_pt = factory_->createPoint(CoordinateXY{99.0, 99.0});
  builder.setSites(*initial_pt);

  builder.setSites(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{2.0, 0.0}, CoordinateXY{0.0, 2.0}});

  auto& subdiv = builder.getSubdivision();
  EXPECT_NE(subdiv.locate(Coordinate{0.0, 0.0}, Coordinate{2.0, 0.0}), nullptr);

  auto edges = builder.getEdges(*factory_);
  auto tris = builder.getTriangles(*factory_);
  EXPECT_EQ(edges->getNumGeometries(), 3u);
  ASSERT_EQ(tris->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(tris->getArea(), 2.0);
}

TEST_F(DelaunayTriangulationBuilderTest, PreservesZCoordinatesInOutput) {
  DelaunayTriangulationBuilder builder;
  builder.setSites(CoordinateSequence{Coordinate{0.0, 0.0, 10.0},
                                      Coordinate{4.0, 0.0, 20.0},
                                      Coordinate{0.0, 3.0, 30.0}});
  auto tris = builder.getTriangles(*factory_);
  ASSERT_EQ(tris->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(tris->getArea(), 6.0);

  auto coords = tris->getGeometryN(0)->getCoordinates();
  ASSERT_EQ(coords->size(), 4u);
  for (std::size_t i = 0; i < coords->size(); ++i) {
    const Coordinate& c = coords->getAt<Coordinate>(i);
    if (c.equals2D(CoordinateXY{0.0, 0.0})) {
      EXPECT_DOUBLE_EQ(c.z, 10.0);
    } else if (c.equals2D(CoordinateXY{4.0, 0.0})) {
      EXPECT_DOUBLE_EQ(c.z, 20.0);
    } else if (c.equals2D(CoordinateXY{0.0, 3.0})) {
      EXPECT_DOUBLE_EQ(c.z, 30.0);
    } else {
      ADD_FAILURE() << "Unexpected vertex in output triangle: " << c.toString();
    }
  }
}

}  // namespace
}  // namespace geos_google
