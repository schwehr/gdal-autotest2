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
// third_party/geos/include/geos/operation/cluster/GeometryDistanceClusterFinder.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/cluster/GeometryDistanceClusterFinder.h

#include "third_party/geos/include/geos/operation/cluster/GeometryDistanceClusterFinder.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
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
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::DoubleNotANumber;
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
using geos::operation::cluster::GeometryDistanceClusterFinder;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

// Exposes protected virtual hooks for direct contract testing.
class ExposedGeometryDistanceClusterFinder final
    : public GeometryDistanceClusterFinder {
 public:
  using GeometryDistanceClusterFinder::GeometryDistanceClusterFinder;
  using GeometryDistanceClusterFinder::queryEnvelope;
  using GeometryDistanceClusterFinder::shouldJoin;
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

TEST(GeometryDistanceClusterFinderTest, EmptyInputAndEmptyGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometryDistanceClusterFinder finder(10.0);

  const Clusters empty_clusters = finder.cluster({});
  EXPECT_EQ(empty_clusters.getNumClusters(), 0);
  EXPECT_THAT(empty_clusters.getClusterIds(), IsEmpty());

  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();
  EXPECT_THAT(finder.clusterToVector(*empty_gc), IsEmpty());
  EXPECT_THAT(finder.clusterToVector(std::move(empty_gc)), IsEmpty());

  std::unique_ptr<MultiPoint> empty_mp = factory->createMultiPoint();
  std::unique_ptr<Geometry> empty_collection =
      finder.clusterToCollection(std::move(empty_mp));
  ASSERT_NE(empty_collection, nullptr);
  EXPECT_TRUE(empty_collection->isEmpty());
  EXPECT_EQ(empty_collection->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  // Empty geometries never join each other or nearby non-empty geometries.
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.5, 0.0));

  const Clusters single_empty = finder.cluster({empty_pt.get()});
  EXPECT_THAT(GroupedClusterIndices(single_empty), ElementsAre(ElementsAre(0)));

  const Clusters mixed = finder.cluster(
      {empty_pt.get(), p0.get(), empty_line.get(), p1.get(), empty_poly.get()});
  EXPECT_THAT(GroupedClusterIndices(mixed),
              UnorderedElementsAre(ElementsAre(0), ElementsAre(2),
                                   ElementsAre(4), ElementsAre(1, 3)));
}

TEST(GeometryDistanceClusterFinderTest, SingleGeometryClusters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  GeometryDistanceClusterFinder finder(2.5);

  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {{0.0, 0.0}, {3.0, 4.0}});
  std::unique_ptr<Polygon> poly = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);

  EXPECT_THAT(GroupedClusterIndices(finder.cluster({pt.get()})),
              ElementsAre(ElementsAre(0)));

  std::vector<std::unique_ptr<Geometry>> vec_const =
      finder.clusterToVector(*line);
  ASSERT_EQ(vec_const.size(), 1);
  EXPECT_TRUE(vec_const[0]->equalsExact(line.get(), 1e-9));

  std::unique_ptr<Polygon> poly_copy = poly->clone();
  std::vector<std::unique_ptr<Geometry>> vec_moved =
      finder.clusterToVector(std::move(poly));
  ASSERT_EQ(vec_moved.size(), 1);
  EXPECT_TRUE(vec_moved[0]->equalsExact(poly_copy.get(), 1e-9));
}

TEST(GeometryDistanceClusterFinderTest, ExactDistanceThresholdInclusion) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // p0 to p1 is an exact 3-4-5 triangle (distance == 5.0).
  // p2 is on the opposite side of p0 at distance > 5.0 from p0 and > 10 from
  // p1.
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(3.0, 4.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(-3.001, -4.0));

  GeometryDistanceClusterFinder exact_finder(5.0);
  EXPECT_THAT(GroupedClusterIndices(
                  exact_finder.cluster({p0.get(), p1.get(), p2.get()})),
              UnorderedElementsAre(ElementsAre(0, 1), ElementsAre(2)));

  GeometryDistanceClusterFinder tighter_finder(4.999);
  EXPECT_THAT(
      GroupedClusterIndices(
          tighter_finder.cluster({p0.get(), p1.get(), p2.get()})),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1), ElementsAre(2)));
}

TEST(GeometryDistanceClusterFinderTest, ZeroNegativeInfiniteAndNaNDistances) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Distance == 0.0 clusters touching/intersecting geometries only.
  std::unique_ptr<Polygon> box_a = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::unique_ptr<Polygon> box_b = MakeBox(*factory, 2.0, 0.0, 4.0, 2.0);
  std::unique_ptr<Polygon> box_c = MakeBox(*factory, 4.0001, 0.0, 6.0, 2.0);
  GeometryDistanceClusterFinder zero_finder(0.0);
  EXPECT_THAT(GroupedClusterIndices(
                  zero_finder.cluster({box_a.get(), box_b.get(), box_c.get()})),
              UnorderedElementsAre(ElementsAre(0, 1), ElementsAre(2)));

  // Negative distance collapses point envelopes to null, preventing joins even
  // between coincident points.
  std::unique_ptr<Point> pt0 = factory->createPoint(CoordinateXY(1.0, 1.0));
  std::unique_ptr<Point> pt1 = factory->createPoint(CoordinateXY(1.0, 1.0));
  std::unique_ptr<Point> pt2 = factory->createPoint(CoordinateXY(1.5, 1.0));
  GeometryDistanceClusterFinder negative_finder(-1.0);
  EXPECT_THAT(
      GroupedClusterIndices(
          negative_finder.cluster({pt0.get(), pt1.get(), pt2.get()})),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1), ElementsAre(2)));

  // Infinite distance clusters all non-empty geometries while leaving empty
  // geometries isolated.
  std::unique_ptr<Point> far_pt = factory->createPoint(CoordinateXY(1e6, -1e6));
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  GeometryDistanceClusterFinder inf_finder(DoubleInfinity);
  EXPECT_THAT(GroupedClusterIndices(inf_finder.cluster(
                  {pt0.get(), box_c.get(), far_pt.get(), empty_pt.get()})),
              UnorderedElementsAre(ElementsAre(0, 1, 2), ElementsAre(3)));

  // NaN distance yields a null query envelope and performs no joins.
  GeometryDistanceClusterFinder nan_finder(DoubleNotANumber);
  EXPECT_THAT(
      GroupedClusterIndices(
          nan_finder.cluster({pt0.get(), pt1.get(), box_a.get()})),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1), ElementsAre(2)));
}

TEST(GeometryDistanceClusterFinderTest,
     UsesTrueGeometryDistanceRatherThanEnvelopeDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Case 1: Point inside a diagonal line's bounding box (envelope distance ==
  // 0, actual distance == 5 * sqrt(2) ~= 7.0710678).
  std::unique_ptr<LineString> diag =
      MakeLine(*factory, {{0.0, 0.0}, {10.0, 10.0}});
  std::unique_ptr<Point> corner_pt =
      factory->createPoint(CoordinateXY(0.0, 10.0));

  GeometryDistanceClusterFinder line_miss(5.0);
  EXPECT_THAT(
      GroupedClusterIndices(line_miss.cluster({diag.get(), corner_pt.get()})),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1)));

  GeometryDistanceClusterFinder line_hit(7.1);
  EXPECT_THAT(
      GroupedClusterIndices(line_hit.cluster({diag.get(), corner_pt.get()})),
      ElementsAre(ElementsAre(0, 1)));

  // Case 2: Point inside the notch of an L-shaped polygon's bounding box
  // (envelope distance == 0, actual distance == 4.0).
  std::unique_ptr<Polygon> l_poly =
      factory->createPolygon(MakeRing(*factory, {{0.0, 0.0},
                                                 {10.0, 0.0},
                                                 {10.0, 2.0},
                                                 {2.0, 2.0},
                                                 {2.0, 10.0},
                                                 {0.0, 10.0},
                                                 {0.0, 0.0}}));
  std::unique_ptr<Point> notch_pt =
      factory->createPoint(CoordinateXY(6.0, 6.0));

  GeometryDistanceClusterFinder notch_miss(3.99);
  EXPECT_THAT(
      GroupedClusterIndices(notch_miss.cluster({l_poly.get(), notch_pt.get()})),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1)));

  GeometryDistanceClusterFinder notch_hit(4.0);
  EXPECT_THAT(
      GroupedClusterIndices(notch_hit.cluster({l_poly.get(), notch_pt.get()})),
      ElementsAre(ElementsAre(0, 1)));

  // Case 3: Point inside a polygon hole (envelope distance == 0, actual
  // distance to boundary == 8.0).
  std::unique_ptr<LinearRing> shell = MakeRing(
      *factory,
      {{0.0, 0.0}, {20.0, 0.0}, {20.0, 20.0}, {0.0, 20.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing(
      *factory,
      {{2.0, 2.0}, {18.0, 2.0}, {18.0, 18.0}, {2.0, 18.0}, {2.0, 2.0}}));
  std::unique_ptr<Polygon> donut =
      factory->createPolygon(std::move(shell), std::move(holes));
  std::unique_ptr<Point> hole_pt =
      factory->createPoint(CoordinateXY(10.0, 10.0));

  GeometryDistanceClusterFinder hole_miss(7.99);
  EXPECT_THAT(
      GroupedClusterIndices(hole_miss.cluster({donut.get(), hole_pt.get()})),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1)));

  GeometryDistanceClusterFinder hole_hit(8.0);
  EXPECT_THAT(
      GroupedClusterIndices(hole_hit.cluster({donut.get(), hole_pt.get()})),
      ElementsAre(ElementsAre(0, 1)));
}

TEST(GeometryDistanceClusterFinderTest, TransitiveChainingAndSeparateClusters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Chain: p0(0,0) - p1(1,0) - p2(2,0) - p3(3,0) within 1.0 of neighbors,
  // even though p0 to p3 is 3.0. q0(5,0) - q1(5.5,0) form a second cluster.
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(1.0, 0.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(2.0, 0.0));
  std::unique_ptr<Point> p3 = factory->createPoint(CoordinateXY(3.0, 0.0));
  std::unique_ptr<Point> q0 = factory->createPoint(CoordinateXY(5.0, 0.0));
  std::unique_ptr<Point> q1 = factory->createPoint(CoordinateXY(5.5, 0.0));

  GeometryDistanceClusterFinder finder(1.0);
  const Clusters clusters = finder.cluster(
      {p0.get(), p1.get(), p2.get(), p3.get(), q0.get(), q1.get()});
  EXPECT_THAT(GroupedClusterIndices(clusters),
              UnorderedElementsAre(ElementsAre(0, 1, 2, 3), ElementsAre(4, 5)));
}

TEST(GeometryDistanceClusterFinderTest,
     ExercisesAllPreparedGeometryBackendsAndCacheReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // 1. MultiPoint (PreparedPoint): 4 points, hits two disjoint nearby Points
  //    (reuses m_prep for mp across multiple candidates) plus a third Point
  //    outside distance threshold inside expanded envelope.
  std::unique_ptr<MultiPoint> mp =
      factory->createMultiPoint(std::vector<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {0.0, 1.0}, {10.0, 1.0}});
  std::unique_ptr<Point> mp_hit1 =
      factory->createPoint(CoordinateXY(0.0, -0.5));
  std::unique_ptr<Point> mp_hit2 =
      factory->createPoint(CoordinateXY(10.0, 1.5));
  std::unique_ptr<Point> mp_miss =
      factory->createPoint(CoordinateXY(5.0, 0.5));  // dist 5.0 > 1.0

  // 2. MultiLineString (PreparedLineString): hits two disjoint points and a
  //    LinearRing (also PreparedLineString).
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLine(*factory, {{30.0, 0.0}, {40.0, 0.0}}));
  lines.push_back(MakeLine(*factory, {{30.0, 10.0}, {40.0, 10.0}}));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(lines));
  std::unique_ptr<Point> mls_hit1 =
      factory->createPoint(CoordinateXY(35.0, 0.8));
  std::unique_ptr<Point> mls_hit2 =
      factory->createPoint(CoordinateXY(35.0, 9.2));
  std::unique_ptr<LinearRing> mls_ring = MakeRing(
      *factory,
      {{39.5, 0.5}, {40.5, 0.5}, {40.5, 1.5}, {39.5, 1.5}, {39.5, 0.5}});

  // 3. MultiPolygon (PreparedPolygon): contains a point inside one polygon and
  //    is within distance of another polygon, while missing a far point.
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeBox(*factory, 60.0, 0.0, 65.0, 5.0));
  polys.push_back(MakeBox(*factory, 70.0, 0.0, 75.0, 5.0));
  std::unique_ptr<MultiPolygon> mpoly =
      factory->createMultiPolygon(std::move(polys));
  std::unique_ptr<Point> poly_inside =
      factory->createPoint(CoordinateXY(62.0, 2.0));
  std::unique_ptr<Polygon> poly_near = MakeBox(*factory, 75.5, 0.0, 77.0, 5.0);

  // 4. GeometryCollection (BasicPreparedGeometry): nested collection with >= 3
  //    points hitting two disjoint points within distance.
  std::vector<std::unique_ptr<Geometry>> gc_parts;
  gc_parts.push_back(factory->createPoint(CoordinateXY(100.0, 0.0)));
  gc_parts.push_back(MakeLine(*factory, {{105.0, 0.0}, {108.0, 0.0}}));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(gc_parts));
  std::unique_ptr<Point> gc_hit1 =
      factory->createPoint(CoordinateXY(100.0, 0.7));
  std::unique_ptr<Point> gc_hit2 =
      factory->createPoint(CoordinateXY(107.0, -0.7));

  GeometryDistanceClusterFinder finder(1.0);
  const Clusters clusters = finder.cluster({
      mp.get(),           // 0
      mp_hit1.get(),      // 1
      mp_hit2.get(),      // 2
      mp_miss.get(),      // 3
      mls.get(),          // 4
      mls_hit1.get(),     // 5
      mls_hit2.get(),     // 6
      mls_ring.get(),     // 7
      mpoly.get(),        // 8
      poly_inside.get(),  // 9
      poly_near.get(),    // 10
      gc.get(),           // 11
      gc_hit1.get(),      // 12
      gc_hit2.get(),      // 13
  });

  EXPECT_THAT(GroupedClusterIndices(clusters),
              UnorderedElementsAre(
                  ElementsAre(0, 1, 2), ElementsAre(3), ElementsAre(4, 5, 6, 7),
                  ElementsAre(8, 9, 10), ElementsAre(11, 12, 13)));
}

TEST(GeometryDistanceClusterFinderTest, CurvedGeometryThrowsUnsupported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<CircularString> curve = factory->createCircularString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 0.5));

  GeometryDistanceClusterFinder finder(1.0);
  // Single curved component never calls shouldJoin.
  EXPECT_THAT(GroupedClusterIndices(finder.cluster({curve.get()})),
              ElementsAre(ElementsAre(0)));

  // Overlapping curved component triggers PreparedGeometryFactory::prepare.
  EXPECT_THROW(finder.cluster({curve.get(), pt.get()}),
               UnsupportedOperationException);
}

TEST(GeometryDistanceClusterFinderTest,
     ClusterToCollectionAndFinderReusePreserveSRID) {
  PrecisionModel pm;
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 3857);
  GeometryDistanceClusterFinder finder(1.0);

  // First call geometries stay alive until after the second call because
  // m_prep holds a pointer to the previously prepared geometry.
  std::vector<std::unique_ptr<Geometry>> first_parts;
  first_parts.push_back(custom_factory->createPoint(CoordinateXY(0.0, 0.0)));
  first_parts.push_back(custom_factory->createPoint(CoordinateXY(0.5, 0.0)));
  first_parts.push_back(MakeLine(*custom_factory, {{10.0, 0.0}, {11.0, 0.0}}));
  first_parts.push_back(MakeBox(*custom_factory, 11.5, 0.0, 12.5, 1.0));
  std::unique_ptr<GeometryCollection> gc1 =
      custom_factory->createGeometryCollection(std::move(first_parts));

  std::unique_ptr<Geometry> res1_const = finder.clusterToCollection(*gc1);
  ASSERT_NE(res1_const, nullptr);
  EXPECT_EQ(res1_const->getFactory(), custom_factory.get());
  EXPECT_EQ(res1_const->getSRID(), 3857);
  ASSERT_EQ(res1_const->getNumGeometries(), 2);

  std::size_t multi_point_count = 0;
  std::size_t mixed_gc_count = 0;
  for (std::size_t i = 0; i < res1_const->getNumGeometries(); ++i) {
    const Geometry* sub = res1_const->getGeometryN(i);
    if (sub->getGeometryTypeId() == geos::geom::GEOS_MULTIPOINT) {
      ++multi_point_count;
      EXPECT_EQ(sub->getNumGeometries(), 2);
    } else if (sub->getGeometryTypeId() ==
               geos::geom::GEOS_GEOMETRYCOLLECTION) {
      ++mixed_gc_count;
      EXPECT_EQ(sub->getNumGeometries(), 2);
    } else {
      ADD_FAILURE() << "Unexpected cluster type: " << sub->getGeometryType();
    }
  }
  EXPECT_EQ(multi_point_count, 1);
  EXPECT_EQ(mixed_gc_count, 1);

  // Second call on the same finder with a moved collection.
  std::vector<std::unique_ptr<Polygon>> second_polys;
  second_polys.push_back(MakeBox(*custom_factory, 20.0, 0.0, 22.0, 2.0));
  second_polys.push_back(MakeBox(*custom_factory, 22.8, 0.0, 24.0, 2.0));
  second_polys.push_back(MakeBox(*custom_factory, 30.0, 0.0, 32.0, 2.0));
  std::unique_ptr<MultiPolygon> mp2 =
      custom_factory->createMultiPolygon(std::move(second_polys));

  std::unique_ptr<Geometry> res2_moved =
      finder.clusterToCollection(std::move(mp2));
  ASSERT_NE(res2_moved, nullptr);
  EXPECT_EQ(res2_moved->getFactory(), custom_factory.get());
  EXPECT_EQ(res2_moved->getSRID(), 3857);
  ASSERT_EQ(res2_moved->getNumGeometries(), 2);

  std::size_t multi_poly_count = 0;
  std::size_t single_poly_count = 0;
  for (std::size_t i = 0; i < res2_moved->getNumGeometries(); ++i) {
    const Geometry* sub = res2_moved->getGeometryN(i);
    if (sub->getGeometryTypeId() == geos::geom::GEOS_MULTIPOLYGON) {
      ++multi_poly_count;
      EXPECT_EQ(sub->getNumGeometries(), 2);
    } else if (sub->getGeometryTypeId() == geos::geom::GEOS_POLYGON) {
      ++single_poly_count;
    } else {
      ADD_FAILURE() << "Unexpected cluster type: " << sub->getGeometryType();
    }
  }
  EXPECT_EQ(multi_poly_count, 1);
  EXPECT_EQ(single_poly_count, 1);
}

TEST(GeometryDistanceClusterFinderTest,
     DirectProtectedQueryEnvelopeAndShouldJoinContract) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  ExposedGeometryDistanceClusterFinder finder(2.5);

  std::unique_ptr<Polygon> box = MakeBox(*factory, 1.0, 2.0, 4.0, 6.0);
  const Envelope& expanded = finder.queryEnvelope(box.get());
  EXPECT_DOUBLE_EQ(expanded.getMinX(), -1.5);
  EXPECT_DOUBLE_EQ(expanded.getMaxX(), 6.5);
  EXPECT_DOUBLE_EQ(expanded.getMinY(), -0.5);
  EXPECT_DOUBLE_EQ(expanded.getMaxY(), 8.5);

  std::unique_ptr<Point> empty_pt = factory->createPoint();
  const Envelope& empty_env = finder.queryEnvelope(empty_pt.get());
  EXPECT_EQ(&expanded, &empty_env);
  EXPECT_TRUE(empty_env.isNull());

  // First call: m_prep == nullptr -> prepares `box`.
  std::unique_ptr<Point> near_pt = factory->createPoint(CoordinateXY(6.5, 4.0));
  std::unique_ptr<Point> far_pt = factory->createPoint(CoordinateXY(6.6, 4.0));
  std::unique_ptr<Point> distant_pt =
      factory->createPoint(CoordinateXY(9.1, 4.0));
  EXPECT_TRUE(finder.shouldJoin(box.get(), near_pt.get()));
  // Second call with same geometry `box`: reuses cached m_prep.
  EXPECT_FALSE(finder.shouldJoin(box.get(), far_pt.get()));
  // Third call with different geometry `near_pt`: updates m_prep.
  EXPECT_TRUE(finder.shouldJoin(near_pt.get(), near_pt.get()));
  // `far_pt` is 2.6 units from `box`, but only 0.1 units from `near_pt`.
  EXPECT_TRUE(finder.shouldJoin(near_pt.get(), far_pt.get()));
  // `distant_pt` is 2.6 units from `near_pt` (> 2.5 threshold).
  EXPECT_FALSE(finder.shouldJoin(near_pt.get(), distant_pt.get()));
  // Empty geometries on either side never join.
  EXPECT_FALSE(finder.shouldJoin(near_pt.get(), empty_pt.get()));
  EXPECT_FALSE(finder.shouldJoin(empty_pt.get(), near_pt.get()));
  // Null LHS triggers PreparedGeometryFactory::prepare validation.
  EXPECT_THROW(finder.shouldJoin(nullptr, near_pt.get()),
               IllegalArgumentException);

  // Subsequent queryEnvelope updates internal m_envelope to the new point.
  const Envelope& pt_env = finder.queryEnvelope(near_pt.get());
  EXPECT_EQ(&pt_env, &expanded);
  EXPECT_DOUBLE_EQ(pt_env.getMinX(), 4.0);
  EXPECT_DOUBLE_EQ(pt_env.getMaxX(), 9.0);
  EXPECT_DOUBLE_EQ(pt_env.getMinY(), 1.5);
  EXPECT_DOUBLE_EQ(pt_env.getMaxY(), 6.5);

  // Negative distance shrinks non-point envelopes while collapsing points.
  ExposedGeometryDistanceClusterFinder negative_finder(-1.0);
  const Envelope& shrunk_box = negative_finder.queryEnvelope(box.get());
  EXPECT_FALSE(shrunk_box.isNull());
  EXPECT_DOUBLE_EQ(shrunk_box.getMinX(), 2.0);
  EXPECT_DOUBLE_EQ(shrunk_box.getMaxX(), 3.0);
  EXPECT_DOUBLE_EQ(shrunk_box.getMinY(), 3.0);
  EXPECT_DOUBLE_EQ(shrunk_box.getMaxY(), 5.0);
  EXPECT_TRUE(negative_finder.queryEnvelope(near_pt.get()).isNull());
}

}  // namespace
}  // namespace geos_google
