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
// third_party/geos/include/geos/operation/cluster/GeometryIntersectsClusterFinder.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/cluster/GeometryIntersectsClusterFinder.h

#include "third_party/geos/include/geos/operation/cluster/GeometryIntersectsClusterFinder.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
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
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/operation/cluster/Clusters.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::operation::cluster::Clusters;
using geos::operation::cluster::GeometryIntersectsClusterFinder;
using geos::util::UnsupportedOperationException;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

// Exposes protected virtual hooks for direct contract testing.
class ExposedGeometryIntersectsClusterFinder final
    : public GeometryIntersectsClusterFinder {
 public:
  using GeometryIntersectsClusterFinder::queryEnvelope;
  using GeometryIntersectsClusterFinder::shouldJoin;
};

std::unique_ptr<LineString> MakeLine(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<LinearRing> MakeRing(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<Polygon> MakeBox(const GeometryFactory& factory, double min_x,
                                 double min_y, double max_x, double max_y) {
  return factory.createPolygon(MakeRing(factory, {
                                                     {min_x, min_y},
                                                     {max_x, min_y},
                                                     {max_x, max_y},
                                                     {min_x, max_y},
                                                     {min_x, min_y},
                                                 }));
}

std::vector<std::vector<std::size_t>> GroupedClusterIndices(
    const Clusters& clusters) {
  std::vector<std::vector<std::size_t>> grouped;
  grouped.reserve(clusters.getNumClusters());
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    std::vector<std::size_t> elems(clusters.begin(c), clusters.end(c));
    EXPECT_EQ(clusters.getSize(c), elems.size());
    std::sort(elems.begin(), elems.end());
    grouped.push_back(std::move(elems));
  }
  return grouped;
}

TEST(GeometryIntersectsClusterFinderTest, EmptyInputAndEmptyGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();
  std::unique_ptr<MultiPoint> empty_mp = factory->createMultiPoint();

  // Empty geometries never join each other or intersecting non-empty ones.
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(2.0, 3.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(2.0, 3.0));

  GeometryIntersectsClusterFinder finder;

  const Clusters empty_clusters = finder.cluster({});
  EXPECT_EQ(empty_clusters.getNumClusters(), 0);
  EXPECT_THAT(empty_clusters.getClusterIds(), IsEmpty());

  EXPECT_THAT(finder.clusterToVector(*empty_gc), IsEmpty());
  std::unique_ptr<Geometry> empty_const_col =
      finder.clusterToCollection(*empty_gc);
  ASSERT_NE(empty_const_col, nullptr);
  EXPECT_TRUE(empty_const_col->isEmpty());
  EXPECT_EQ(empty_const_col->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_THAT(finder.clusterToVector(std::move(empty_gc)), IsEmpty());

  std::unique_ptr<Geometry> empty_collection =
      finder.clusterToCollection(std::move(empty_mp));
  ASSERT_NE(empty_collection, nullptr);
  EXPECT_TRUE(empty_collection->isEmpty());
  EXPECT_EQ(empty_collection->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  EXPECT_THAT(GroupedClusterIndices(finder.cluster({empty_pt.get()})),
              ElementsAre(ElementsAre(0)));

  const Clusters mixed = finder.cluster(
      {empty_pt.get(), p0.get(), empty_line.get(), p1.get(), empty_poly.get()});
  EXPECT_THAT(GroupedClusterIndices(mixed),
              UnorderedElementsAre(ElementsAre(0), ElementsAre(2),
                                   ElementsAre(4), ElementsAre(1, 3)));
}

TEST(GeometryIntersectsClusterFinderTest, SingleGeometryClusters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(4.0, 5.0));
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {{0.0, 0.0}, {1.0, 2.0}});
  std::unique_ptr<Polygon> poly = MakeBox(*factory, 0.0, 0.0, 3.0, 3.0);
  std::unique_ptr<Polygon> poly_copy = poly->clone();

  std::vector<std::unique_ptr<Geometry>> vec_const;
  std::vector<std::unique_ptr<Geometry>> vec_moved;
  GeometryIntersectsClusterFinder finder;

  EXPECT_THAT(GroupedClusterIndices(finder.cluster({pt.get()})),
              ElementsAre(ElementsAre(0)));

  vec_const = finder.clusterToVector(*line);
  ASSERT_EQ(vec_const.size(), 1);
  EXPECT_TRUE(vec_const[0]->equalsExact(line.get(), 1e-9));

  vec_moved = finder.clusterToVector(std::move(poly));
  ASSERT_EQ(vec_moved.size(), 1);
  EXPECT_TRUE(vec_moved[0]->equalsExact(poly_copy.get(), 1e-9));
}

TEST(GeometryIntersectsClusterFinderTest,
     IntersectingTouchingCrossingAndDisjointGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Coincident points join, whereas points separated by 1e-9 do not.
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(1e-9, 0.0));

  // Crossing, T-touching, endpoint-touching, and collinear-overlapping lines
  // cluster together; a nearby parallel line stays separate.
  std::unique_ptr<LineString> horiz =
      MakeLine(*factory, {{10.0, 0.0}, {20.0, 0.0}});
  std::unique_ptr<LineString> cross =
      MakeLine(*factory, {{15.0, -2.0}, {15.0, 2.0}});
  std::unique_ptr<LineString> t_touch =
      MakeLine(*factory, {{12.0, 0.0}, {12.0, 3.0}});
  std::unique_ptr<LineString> end_touch =
      MakeLine(*factory, {{20.0, 0.0}, {22.0, 2.0}});
  std::unique_ptr<LineString> collinear =
      MakeLine(*factory, {{18.0, 0.0}, {25.0, 0.0}});
  std::unique_ptr<LineString> parallel =
      MakeLine(*factory, {{10.0, 0.1}, {11.5, 0.1}});

  // Polygons: interior overlap, edge-touching, corner-touching, plus points on
  // interior, edge, and corner all join; a disjoint box stays separate.
  std::unique_ptr<Polygon> base_box = MakeBox(*factory, 40.0, 0.0, 44.0, 4.0);
  std::unique_ptr<Polygon> overlap_box =
      MakeBox(*factory, 39.0, 2.5, 41.0, 5.0);
  std::unique_ptr<Polygon> corner_box = MakeBox(*factory, 44.0, 4.0, 46.0, 6.0);
  std::unique_ptr<Polygon> edge_box = MakeBox(*factory, 38.0, 0.0, 40.0, 2.0);
  std::unique_ptr<Point> interior_pt =
      factory->createPoint(CoordinateXY(42.0, 2.0));
  std::unique_ptr<Point> boundary_pt =
      factory->createPoint(CoordinateXY(40.0, 3.0));
  std::unique_ptr<Point> corner_pt =
      factory->createPoint(CoordinateXY(44.0, 0.0));
  std::unique_ptr<Polygon> disjoint_box =
      MakeBox(*factory, 44.1, 0.0, 46.0, 3.9);

  GeometryIntersectsClusterFinder finder;
  EXPECT_THAT(
      GroupedClusterIndices(finder.cluster({p0.get(), p1.get(), p2.get()})),
      UnorderedElementsAre(ElementsAre(0, 1), ElementsAre(2)));

  EXPECT_THAT(GroupedClusterIndices(finder.cluster(
                  {horiz.get(), cross.get(), t_touch.get(), end_touch.get(),
                   collinear.get(), parallel.get()})),
              UnorderedElementsAre(ElementsAre(0, 1, 2, 3, 4), ElementsAre(5)));

  EXPECT_THAT(
      GroupedClusterIndices(
          finder.cluster({base_box.get(), overlap_box.get(), corner_box.get(),
                          edge_box.get(), interior_pt.get(), boundary_pt.get(),
                          corner_pt.get(), disjoint_box.get()})),
      UnorderedElementsAre(ElementsAre(0, 1, 2, 3, 4, 5, 6), ElementsAre(7)));
}

TEST(GeometryIntersectsClusterFinderTest,
     UsesTrueGeometryIntersectionRatherThanEnvelopeIntersection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Case 1: Parallel diagonal lines whose bounding boxes overlap.
  std::unique_ptr<LineString> diag1 =
      MakeLine(*factory, {{0.0, 0.0}, {10.0, 10.0}});
  std::unique_ptr<LineString> diag2 =
      MakeLine(*factory, {{0.0, 6.0}, {4.0, 10.0}});

  // Case 2: L-shaped polygon whose envelope covers a disjoint box and point in
  // the notch (while the notch box and notch point intersect each other).
  std::unique_ptr<Polygon> l_poly =
      factory->createPolygon(MakeRing(*factory, {{0.0, 0.0},
                                                 {10.0, 0.0},
                                                 {10.0, 2.0},
                                                 {2.0, 2.0},
                                                 {2.0, 10.0},
                                                 {0.0, 10.0},
                                                 {0.0, 0.0}}));
  std::unique_ptr<Polygon> notch_box = MakeBox(*factory, 4.0, 4.0, 8.0, 8.0);
  std::unique_ptr<Point> notch_pt =
      factory->createPoint(CoordinateXY(6.0, 6.0));

  // Case 3: Polygon with a hole: geometries strictly inside the hole are
  // disjoint from the outer polygon, while a point on the hole boundary joins.
  std::unique_ptr<LinearRing> shell = MakeRing(
      *factory,
      {{20.0, 0.0}, {30.0, 0.0}, {30.0, 10.0}, {20.0, 10.0}, {20.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing(
      *factory,
      {{22.0, 2.0}, {28.0, 2.0}, {28.0, 8.0}, {22.0, 8.0}, {22.0, 2.0}}));
  std::unique_ptr<Polygon> donut =
      factory->createPolygon(std::move(shell), std::move(holes));
  std::unique_ptr<Point> hole_pt =
      factory->createPoint(CoordinateXY(25.0, 5.0));
  std::unique_ptr<Polygon> hole_box = MakeBox(*factory, 23.0, 3.0, 27.0, 7.0);
  std::unique_ptr<Point> hole_edge_pt =
      factory->createPoint(CoordinateXY(22.0, 5.0));

  GeometryIntersectsClusterFinder finder;
  EXPECT_THAT(GroupedClusterIndices(finder.cluster({diag1.get(), diag2.get()})),
              UnorderedElementsAre(ElementsAre(0), ElementsAre(1)));
  EXPECT_THAT(GroupedClusterIndices(finder.cluster(
                  {l_poly.get(), notch_box.get(), notch_pt.get()})),
              UnorderedElementsAre(ElementsAre(0), ElementsAre(1, 2)));
  EXPECT_THAT(
      GroupedClusterIndices(finder.cluster(
          {donut.get(), hole_pt.get(), hole_box.get(), hole_edge_pt.get()})),
      UnorderedElementsAre(ElementsAre(0, 3), ElementsAre(1, 2)));
}

TEST(GeometryIntersectsClusterFinderTest, TransitiveIntersectionChaining) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Chain: s0 -> s1 -> s2 -> s3; s0 and s3 have disjoint envelopes.
  std::unique_ptr<LineString> s0 = MakeLine(*factory, {{0.0, 0.0}, {2.0, 0.0}});
  std::unique_ptr<LineString> s1 =
      MakeLine(*factory, {{1.5, -1.0}, {1.5, 2.0}});
  std::unique_ptr<LineString> s2 = MakeLine(*factory, {{1.0, 1.5}, {4.0, 1.5}});
  std::unique_ptr<LineString> s3 = MakeLine(*factory, {{3.5, 0.0}, {3.5, 3.0}});
  std::unique_ptr<LineString> isolated =
      MakeLine(*factory, {{10.0, 0.0}, {12.0, 0.0}});

  GeometryIntersectsClusterFinder finder;
  EXPECT_THAT(GroupedClusterIndices(finder.cluster(
                  {s0.get(), s1.get(), s2.get(), s3.get(), isolated.get()})),
              UnorderedElementsAre(ElementsAre(0, 1, 2, 3), ElementsAre(4)));
}

TEST(GeometryIntersectsClusterFinderTest,
     ExercisesAllPreparedGeometryBackendsAndCacheReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // 1. MultiPoint (PreparedPoint): 4 points hitting 2 disjoint Points and
  //    missing a third Point inside its bounding box.
  std::unique_ptr<MultiPoint> mp =
      factory->createMultiPoint(std::vector<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {0.0, 5.0}, {10.0, 5.0}});
  std::unique_ptr<Point> mp_hit1 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> mp_hit2 =
      factory->createPoint(CoordinateXY(10.0, 5.0));
  std::unique_ptr<Point> mp_miss = factory->createPoint(CoordinateXY(5.0, 2.5));

  // 2. MultiLineString (PreparedLineString) and LinearRing: hits 2 disjoint
  //    geometries and misses a disjoint diagonal in the envelope.
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLine(*factory, {{20.0, 0.0}, {30.0, 0.0}}));
  lines.push_back(MakeLine(*factory, {{20.0, 10.0}, {30.0, 10.0}}));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(lines));
  std::unique_ptr<Point> mls_hit1 =
      factory->createPoint(CoordinateXY(25.0, 0.0));
  std::unique_ptr<LinearRing> mls_ring = MakeRing(
      *factory,
      {{24.0, 9.0}, {26.0, 9.0}, {26.0, 11.0}, {24.0, 11.0}, {24.0, 9.0}});
  std::unique_ptr<Point> mls_miss =
      factory->createPoint(CoordinateXY(25.0, 5.0));

  // 3. MultiPolygon (PreparedPolygon): hits 2 disjoint geometries and misses a
  //    point in the gap between its two shells.
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeBox(*factory, 40.0, 0.0, 44.0, 4.0));
  polys.push_back(MakeBox(*factory, 48.0, 0.0, 52.0, 4.0));
  std::unique_ptr<MultiPolygon> mpoly =
      factory->createMultiPolygon(std::move(polys));
  std::unique_ptr<Point> mpoly_hit1 =
      factory->createPoint(CoordinateXY(42.0, 2.0));
  std::unique_ptr<LineString> mpoly_hit2 =
      MakeLine(*factory, {{50.0, -1.0}, {50.0, 2.0}});
  std::unique_ptr<Point> mpoly_miss =
      factory->createPoint(CoordinateXY(46.0, 2.0));

  // 4. GeometryCollection (BasicPreparedGeometry): hits 2 disjoint points and
  //    misses a third point inside its bounding box.
  std::vector<std::unique_ptr<Geometry>> gc_parts;
  gc_parts.push_back(factory->createPoint(CoordinateXY(70.0, 0.0)));
  gc_parts.push_back(MakeLine(*factory, {{75.0, 0.0}, {80.0, 5.0}}));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(gc_parts));
  std::unique_ptr<Point> gc_hit1 =
      factory->createPoint(CoordinateXY(70.0, 0.0));
  std::unique_ptr<Point> gc_hit2 =
      factory->createPoint(CoordinateXY(78.0, 3.0));
  std::unique_ptr<Point> gc_miss =
      factory->createPoint(CoordinateXY(72.0, 4.0));

  GeometryIntersectsClusterFinder finder;
  const Clusters clusters = finder.cluster({
      mp.get(),          // 0
      mp_hit1.get(),     // 1
      mp_hit2.get(),     // 2
      mp_miss.get(),     // 3
      mls.get(),         // 4
      mls_hit1.get(),    // 5
      mls_ring.get(),    // 6
      mls_miss.get(),    // 7
      mpoly.get(),       // 8
      mpoly_hit1.get(),  // 9
      mpoly_hit2.get(),  // 10
      mpoly_miss.get(),  // 11
      gc.get(),          // 12
      gc_hit1.get(),     // 13
      gc_hit2.get(),     // 14
      gc_miss.get(),     // 15
  });

  EXPECT_THAT(GroupedClusterIndices(clusters),
              UnorderedElementsAre(ElementsAre(0, 1, 2), ElementsAre(3),
                                   ElementsAre(4, 5, 6), ElementsAre(7),
                                   ElementsAre(8, 9, 10), ElementsAre(11),
                                   ElementsAre(12, 13, 14), ElementsAre(15)));
}

TEST(GeometryIntersectsClusterFinderTest, CurvedGeometryThrowsUnsupported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<CircularString> curve = factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 1.0));

  GeometryIntersectsClusterFinder finder;
  EXPECT_THAT(GroupedClusterIndices(finder.cluster({curve.get()})),
              ElementsAre(ElementsAre(0)));

  EXPECT_THROW(finder.cluster({curve.get(), pt.get()}),
               UnsupportedOperationException);
}

TEST(GeometryIntersectsClusterFinderTest,
     ClusterToVectorAndCollectionPreserveTypesAndSRID) {
  PrecisionModel pm;
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 4326);

  // Mixed collection:
  // - Cluster 1: Point(1,1) + Box(0..2, 0..2) -> GeometryCollection
  // - Cluster 2: 2 touching LineStrings -> MultiLineString
  // - Cluster 3: 2 overlapping Polygons -> MultiPolygon
  // - Cluster 4: isolated Point(50,50) -> Point
  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(custom_factory->createPoint(CoordinateXY(1.0, 1.0)));
  parts.push_back(MakeBox(*custom_factory, 0.0, 0.0, 2.0, 2.0));
  parts.push_back(MakeLine(*custom_factory, {{10.0, 0.0}, {12.0, 0.0}}));
  parts.push_back(MakeLine(*custom_factory, {{12.0, 0.0}, {14.0, 0.0}}));
  parts.push_back(MakeBox(*custom_factory, 20.0, 0.0, 22.0, 2.0));
  parts.push_back(MakeBox(*custom_factory, 21.0, 1.0, 23.0, 3.0));
  parts.push_back(custom_factory->createPoint(CoordinateXY(50.0, 50.0)));
  std::unique_ptr<GeometryCollection> gc =
      custom_factory->createGeometryCollection(std::move(parts));

  std::vector<std::unique_ptr<Geometry>> vec_clusters;
  std::unique_ptr<Geometry> const_col;
  std::unique_ptr<Geometry> moved_col;
  GeometryIntersectsClusterFinder finder;

  vec_clusters = finder.clusterToVector(*gc);
  EXPECT_EQ(gc->getNumGeometries(), 7);
  ASSERT_EQ(vec_clusters.size(), 4);

  std::size_t mixed_gc_count = 0;
  std::size_t mls_count = 0;
  std::size_t mpoly_count = 0;
  std::size_t point_count = 0;
  for (const auto& geom : vec_clusters) {
    EXPECT_EQ(geom->getFactory(), custom_factory.get());
    EXPECT_EQ(geom->getSRID(), 4326);
    switch (geom->getGeometryTypeId()) {
      case geos::geom::GEOS_GEOMETRYCOLLECTION:
        ++mixed_gc_count;
        EXPECT_EQ(geom->getNumGeometries(), 2);
        break;
      case geos::geom::GEOS_MULTILINESTRING:
        ++mls_count;
        EXPECT_EQ(geom->getNumGeometries(), 2);
        break;
      case geos::geom::GEOS_MULTIPOLYGON:
        ++mpoly_count;
        EXPECT_EQ(geom->getNumGeometries(), 2);
        break;
      case geos::geom::GEOS_POINT:
        ++point_count;
        break;
      default:
        ADD_FAILURE() << "Unexpected cluster geometry type: "
                      << geom->getGeometryType();
        break;
    }
  }
  EXPECT_EQ(mixed_gc_count, 1);
  EXPECT_EQ(mls_count, 1);
  EXPECT_EQ(mpoly_count, 1);
  EXPECT_EQ(point_count, 1);

  const_col = finder.clusterToCollection(*gc);
  ASSERT_NE(const_col, nullptr);
  EXPECT_EQ(const_col->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(const_col->getFactory(), custom_factory.get());
  EXPECT_EQ(const_col->getSRID(), 4326);
  EXPECT_EQ(const_col->getNumGeometries(), 4);

  // Reuse the same finder on a moved collection; vec_clusters and const_col
  // stay alive until after finder is destroyed so m_prep never dangles.
  moved_col = finder.clusterToCollection(std::move(gc));
  ASSERT_NE(moved_col, nullptr);
  EXPECT_EQ(moved_col->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(moved_col->getFactory(), custom_factory.get());
  EXPECT_EQ(moved_col->getSRID(), 4326);
  EXPECT_EQ(moved_col->getNumGeometries(), 4);
}

TEST(GeometryIntersectsClusterFinderTest,
     DirectProtectedQueryEnvelopeAndShouldJoinContract) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> box = MakeBox(*factory, 2.0, 3.0, 7.0, 9.0);
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<Point> inside_pt =
      factory->createPoint(CoordinateXY(4.0, 5.0));
  std::unique_ptr<Point> outside_pt =
      factory->createPoint(CoordinateXY(8.0, 5.0));

  ExposedGeometryIntersectsClusterFinder finder;

  const Envelope& env = finder.queryEnvelope(box.get());
  EXPECT_TRUE(env.equals(box->getEnvelopeInternal()));

  const Envelope& empty_env = finder.queryEnvelope(empty_pt.get());
  EXPECT_TRUE(empty_env.isNull());

  // First call: m_prep == nullptr -> prepares box.
  EXPECT_TRUE(finder.shouldJoin(box.get(), inside_pt.get()));
  // Second call with same `box`: reuses cached m_prep.
  EXPECT_FALSE(finder.shouldJoin(box.get(), outside_pt.get()));
  EXPECT_FALSE(finder.shouldJoin(box.get(), empty_pt.get()));
  // Third call with different geometry `inside_pt`: updates m_prep.
  EXPECT_TRUE(finder.shouldJoin(inside_pt.get(), box.get()));
  EXPECT_FALSE(finder.shouldJoin(inside_pt.get(), outside_pt.get()));
  // Empty prepared geometry never intersects empty or non-empty geometries.
  EXPECT_FALSE(finder.shouldJoin(empty_pt.get(), box.get()));
  EXPECT_FALSE(finder.shouldJoin(empty_pt.get(), empty_pt.get()));
}

}  // namespace
}  // namespace geos_google
