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

// Unit tests for third_party/geos/src/operation/cluster/DBSCANClusterFinder.cpp
// and third_party/geos/include/geos/operation/cluster/DBSCANClusterFinder.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for std::runtime_error checks.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/cluster/DBSCANClusterFinder.cpp

#include "third_party/geos/include/geos/operation/cluster/DBSCANClusterFinder.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/operation/cluster/Clusters.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::operation::cluster::Clusters;
using geos::operation::cluster::DBSCANClusterFinder;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

constexpr std::size_t kNoCluster = std::numeric_limits<std::size_t>::max();

std::vector<const Geometry*> AsRawPtrs(
    const std::vector<std::unique_ptr<Point>>& pts) {
  std::vector<const Geometry*> raw;
  raw.reserve(pts.size());
  for (const auto& pt : pts) {
    raw.push_back(pt.get());
  }
  return raw;
}

std::vector<std::unique_ptr<Point>> MakePoints(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  std::vector<std::unique_ptr<Point>> pts;
  pts.reserve(coords.size());
  for (const CoordinateXY& c : coords) {
    pts.push_back(factory.createPoint(c));
  }
  return pts;
}

std::unique_ptr<LineString> MakeLine(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<Polygon> MakePolygon(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  std::unique_ptr<LinearRing> ring = factory.createLinearRing(std::move(seq));
  return factory.createPolygon(std::move(ring));
}

std::vector<std::vector<std::size_t>> ExtractSortedClusters(
    const Clusters& clusters) {
  std::vector<std::vector<std::size_t>> result;
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    std::vector<std::size_t> elems(clusters.begin(c), clusters.end(c));
    std::sort(elems.begin(), elems.end());
    result.push_back(std::move(elems));
  }
  return result;
}

TEST(DBSCANClusterFinderTest, EmptyInputsYieldZeroClusters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  DBSCANClusterFinder finder(1.0, 2);

  const Clusters clusters = finder.cluster({});
  EXPECT_EQ(clusters.getNumClusters(), 0);
  EXPECT_THAT(clusters.getClusterIds(), IsEmpty());

  std::unique_ptr<MultiPoint> empty_mp = factory->createMultiPoint();
  EXPECT_THAT(finder.clusterToVector(*empty_mp), IsEmpty());

  std::unique_ptr<Geometry> empty_col =
      finder.clusterToCollection(std::move(empty_mp));
  ASSERT_NE(empty_col, nullptr);
  EXPECT_TRUE(empty_col->isEmpty());
  EXPECT_EQ(empty_col->getNumGeometries(), 0);
}

TEST(DBSCANClusterFinderTest, EmptyGeometriesRemainUnclustered) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.0, 0.0));

  DBSCANClusterFinder finder(1.0, 1);
  const Clusters clusters =
      finder.cluster({empty_pt.get(), p0.get(), empty_line.get(), p1.get()});

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_THAT(ExtractSortedClusters(clusters), ElementsAre(ElementsAre(1, 3)));
  EXPECT_THAT(clusters.getClusterIds(),
              ElementsAre(kNoCluster, 0, kNoCluster, 0));

  // All-empty collection drops all noise geometries via clusterToVector.
  std::vector<std::unique_ptr<Geometry>> empties;
  empties.push_back(factory->createPoint());
  empties.push_back(factory->createLineString());
  std::unique_ptr<GeometryCollection> all_empty =
      factory->createGeometryCollection(std::move(empties));
  EXPECT_THAT(finder.clusterToVector(std::move(all_empty)), IsEmpty());
}

TEST(DBSCANClusterFinderTest, SinglePointAndDegenerateParameters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 2.0));

  {
    DBSCANClusterFinder finder(1.0, 1);
    const Clusters clusters = finder.cluster({pt.get()});
    ASSERT_EQ(clusters.getNumClusters(), 1);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0));
  }
  {
    DBSCANClusterFinder finder(1.0, 2);
    const Clusters clusters = finder.cluster({pt.get()});
    EXPECT_EQ(clusters.getNumClusters(), 0);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(kNoCluster));
  }
  {
    DBSCANClusterFinder finder(-0.1, 1);
    const Clusters clusters = finder.cluster({pt.get()});
    EXPECT_EQ(clusters.getNumClusters(), 0);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(kNoCluster));
  }
  {
    DBSCANClusterFinder finder(std::numeric_limits<double>::quiet_NaN(), 1);
    const Clusters clusters = finder.cluster({pt.get()});
    EXPECT_EQ(clusters.getNumClusters(), 0);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(kNoCluster));
  }
}

TEST(DBSCANClusterFinderTest, NegativeEpsOnLargePolygonsRejectsZeroDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> p0 =
      MakePolygon(*factory, {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  std::unique_ptr<Polygon> p1 =
      MakePolygon(*factory, {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});

  // Expanded envelope shrinks to [1, 9] x [1, 9], so STRtree returns hits,
  // distance is 0.0, and dist <= -1.0 is false.
  DBSCANClusterFinder finder(-1.0, 1);
  const Clusters clusters = finder.cluster({p0.get(), p1.get()});
  EXPECT_EQ(clusters.getNumClusters(), 0);
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(kNoCluster, kNoCluster));
}

TEST(DBSCANClusterFinderTest, ZeroEpsClustersOnlyZeroDistanceGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> l0 = MakeLine(*factory, {{0.0, 0.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> l1 = MakeLine(*factory, {{0.0, 2.0}, {2.0, 0.0}});
  std::unique_ptr<Point> on_cross =
      factory->createPoint(CoordinateXY(1.0, 1.0));
  std::unique_ptr<Point> off_cross =
      factory->createPoint(CoordinateXY(1.0, 1.0 + 1e-6));

  DBSCANClusterFinder finder(0.0, 2);
  const Clusters clusters =
      finder.cluster({l0.get(), l1.get(), on_cross.get(), off_cross.get()});

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_THAT(ExtractSortedClusters(clusters),
              ElementsAre(ElementsAre(0, 1, 2)));
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, 0, kNoCluster));
}

TEST(DBSCANClusterFinderTest, ExactEpsAndTrueGeometryDistanceThreshold) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Diagonal segment (0,0)-(10,10) has envelope [0,10]x[0,10] which contains
  // (1,9), so envelope distance is 0 while true distance is 4*sqrt(2) ~= 5.657.
  std::unique_ptr<LineString> diag =
      MakeLine(*factory, {{0.0, 0.0}, {10.0, 10.0}});
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 9.0));

  {
    DBSCANClusterFinder below_eps(5.65, 2);
    const Clusters clusters = below_eps.cluster({diag.get(), pt.get()});
    EXPECT_EQ(clusters.getNumClusters(), 0);
  }
  {
    DBSCANClusterFinder above_eps(5.66, 2);
    const Clusters clusters = above_eps.cluster({diag.get(), pt.get()});
    ASSERT_EQ(clusters.getNumClusters(), 1);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0));
  }

  // Inclusive boundary: dist == eps is included; slightly above eps is noise.
  auto pts = MakePoints(*factory, {{0.0, 0.0}, {1.0, 0.0}, {2.0001, 0.0}});
  DBSCANClusterFinder boundary_finder(1.0, 2);
  const Clusters boundary_clusters = boundary_finder.cluster(AsRawPtrs(pts));
  ASSERT_EQ(boundary_clusters.getNumClusters(), 1);
  EXPECT_THAT(boundary_clusters.getClusterIds(), ElementsAre(0, 0, kNoCluster));
}

TEST(DBSCANClusterFinderTest, DensityConnectedChainWithBorderAndNoise) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // With eps = 1.0, minPoints = 3:
  // - pts[1], pts[2], pts[3] each have 3 neighbors (core points)
  // - pts[0] and pts[4] each have 2 neighbors (border points)
  // - pts[5] has 1 neighbor (noise)
  auto pts = MakePoints(*factory, {{0.0, 0.0},
                                   {0.9, 0.0},
                                   {1.8, 0.0},
                                   {2.7, 0.0},
                                   {3.6, 0.0},
                                   {10.0, 10.0}});

  DBSCANClusterFinder finder(1.0, 3);
  const Clusters clusters = finder.cluster(AsRawPtrs(pts));

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_THAT(ExtractSortedClusters(clusters),
              ElementsAre(ElementsAre(0, 1, 2, 3, 4)));
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, 0, 0, 0, kNoCluster));
}

TEST(DBSCANClusterFinderTest, SharedBorderPointDoesNotMergeTwoClusters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Two dense core clusters (indices 0..3 and 5..8) separated by a single
  // non-core border point (index 4) with only 3 neighbors (< minPoints = 4).
  // Testing both positive and negative Y offsets on point 4 exercises both:
  // - skipping an already-claimed border point after minPoints hits
  // - encountering an already-claimed border point inside unionIfAvailable.
  for (double border_y : {0.1, -0.1}) {
    SCOPED_TRACE(border_y);
    auto pts = MakePoints(*factory, {
                                        {0.0, 0.0},
                                        {-0.5, 0.0},
                                        {-0.6, 0.0},
                                        {-0.7, 0.0},
                                        {0.9, border_y},
                                        {1.8, 0.0},
                                        {2.3, 0.0},
                                        {2.4, 0.0},
                                        {2.5, 0.0},
                                    });

    DBSCANClusterFinder finder(1.0, 4);
    const Clusters clusters = finder.cluster(AsRawPtrs(pts));

    ASSERT_EQ(clusters.getNumClusters(), 2);
    EXPECT_THAT(ExtractSortedClusters(clusters),
                UnorderedElementsAre(ElementsAre(0, 1, 2, 3, 4),
                                     ElementsAre(5, 6, 7, 8)));
  }
}

TEST(DBSCANClusterFinderTest, LaterCorePointMergesTwoPreviouslyFormedClusters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Cluster A (0, 1, 2, 6, 7) claims point 8 as a border point before 8 runs.
  // Cluster B (3, 4, 5) encounters 8, sees it is not yet marked core, and keeps
  // Cluster B separate. When p = 8 later runs with minPoints = 3, hits sorted
  // by Y visit {7, 6, 0} first to mark 8 core, skip self (8), and merge
  // Cluster A with Cluster B via core point 3.
  auto pts = MakePoints(*factory, {
                                      {0.0, 0.0},
                                      {-0.6, -0.1},
                                      {-0.6, -0.2},
                                      {1.8, 0.2},
                                      {2.4, 0.2},
                                      {2.5, 0.2},
                                      {0.9, -0.3},
                                      {0.9, -0.4},
                                      {0.9, 0.01},
                                  });

  DBSCANClusterFinder finder(1.0, 3);
  const Clusters clusters = finder.cluster(AsRawPtrs(pts));

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_THAT(ExtractSortedClusters(clusters),
              ElementsAre(ElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8)));
}

TEST(DBSCANClusterFinderTest, DenseClusterAboveMinPointsAndPreparedReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto pts = MakePoints(*factory, {
                                      {0.0, 0.0},
                                      {0.2, 0.1},
                                      {0.4, 0.2},
                                      {0.6, 0.3},
                                      {0.8, 0.4},
                                      {1.0, 0.5},
                                  });

  DBSCANClusterFinder finder(2.0, 3);
  const Clusters clusters = finder.cluster(AsRawPtrs(pts));

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_EQ(clusters.getSize(0), 6);
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, 0, 0, 0, 0));
}

TEST(DBSCANClusterFinderTest, ClusterToVectorAndCollectionWithConcavePolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // MultiPoint with two clusters and one isolated noise point.
  std::unique_ptr<MultiPoint> mp =
      factory->createMultiPoint(std::vector<CoordinateXY>{
          {0.0, 0.0}, {0.2, 0.0}, {5.0, 5.0}, {5.2, 5.0}, {100.0, 100.0}});
  DBSCANClusterFinder mp_finder(0.5, 2);
  std::vector<std::unique_ptr<Geometry>> mp_clusters =
      mp_finder.clusterToVector(*mp);
  ASSERT_EQ(mp_clusters.size(), 2);
  for (const auto& g : mp_clusters) {
    EXPECT_EQ(g->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
    EXPECT_EQ(g->getNumGeometries(), 2);
  }
  std::unique_ptr<Geometry> mp_collection =
      mp_finder.clusterToCollection(std::move(mp));
  ASSERT_NE(mp_collection, nullptr);
  EXPECT_EQ(mp_collection->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(mp_collection->getNumGeometries(), 2);

  // Non-convex C-shaped polygon [0,10]x[0,10] with pocket [2,10]x[2,8].
  // Point (6, 5) sits inside the envelope (envelope distance 0) but at true
  // distance 3.0 from the polygon boundary, while (1, 1) lies inside it.
  std::unique_ptr<Polygon> c_poly = MakePolygon(*factory, {
                                                              {0.0, 0.0},
                                                              {10.0, 0.0},
                                                              {10.0, 2.0},
                                                              {2.0, 2.0},
                                                              {2.0, 8.0},
                                                              {10.0, 8.0},
                                                              {10.0, 10.0},
                                                              {0.0, 10.0},
                                                              {0.0, 0.0},
                                                          });
  std::unique_ptr<Point> inside_wall =
      factory->createPoint(CoordinateXY(1.0, 1.0));
  std::unique_ptr<Point> in_pocket =
      factory->createPoint(CoordinateXY(6.0, 5.0));

  {
    DBSCANClusterFinder tight_finder(1.0, 2);
    const Clusters clusters = tight_finder.cluster(
        {c_poly.get(), inside_wall.get(), in_pocket.get()});
    ASSERT_EQ(clusters.getNumClusters(), 1);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, kNoCluster));
  }
  {
    DBSCANClusterFinder wide_finder(3.0, 2);
    const Clusters clusters =
        wide_finder.cluster({c_poly.get(), inside_wall.get(), in_pocket.get()});
    ASSERT_EQ(clusters.getNumClusters(), 1);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, 0));
  }
}

class ExposedDBSCANClusterFinder final : public DBSCANClusterFinder {
 public:
  using DBSCANClusterFinder::DBSCANClusterFinder;
  bool CallShouldJoin(const Geometry* a, const Geometry* b) {
    return shouldJoin(a, b);
  }
};

TEST(DBSCANClusterFinderTest, ProtectedShouldJoinThrowsSentinelError) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.0, 0.0));
  ExposedDBSCANClusterFinder finder(1.0, 1);

  EXPECT_THROW(finder.CallShouldJoin(p0.get(), p1.get()), std::runtime_error);
}

}  // namespace
}  // namespace geos_google
