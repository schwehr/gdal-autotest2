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
// third_party/geos/include/geos/operation/cluster/EnvelopeDistanceClusterFinder.h.
//
// google3 notes:
// - Uses isolated GeometryFactory::create() instances per test.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/cluster/EnvelopeDistanceClusterFinder.h

#include "third_party/geos/include/geos/operation/cluster/EnvelopeDistanceClusterFinder.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/operation/cluster/Clusters.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::operation::cluster::Clusters;
using geos::operation::cluster::EnvelopeDistanceClusterFinder;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

std::unique_ptr<LineString> MakeLine(const GeometryFactory& factory,
                                     std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<Polygon> MakeBox(const GeometryFactory& factory, double min_x,
                                 double min_y, double max_x, double max_y) {
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {min_x, min_y},
          {max_x, min_y},
          {max_x, max_y},
          {min_x, max_y},
          {min_x, min_y},
      });
  std::unique_ptr<LinearRing> ring = factory.createLinearRing(std::move(seq));
  return factory.createPolygon(std::move(ring));
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
  result.reserve(clusters.getNumClusters());
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    std::vector<std::size_t> elems(clusters.begin(c), clusters.end(c));
    std::sort(elems.begin(), elems.end());
    result.push_back(std::move(elems));
  }
  return result;
}

TEST(EnvelopeDistanceClusterFinderTest, EmptyInputsAndEmptyGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  EnvelopeDistanceClusterFinder finder(1.0);

  const Clusters empty_clusters = finder.cluster({});
  EXPECT_EQ(empty_clusters.getNumClusters(), 0);
  EXPECT_THAT(empty_clusters.getClusterIds(), IsEmpty());

  std::unique_ptr<MultiPoint> empty_mp = factory->createMultiPoint();
  EXPECT_THAT(finder.clusterToVector(*empty_mp), IsEmpty());

  std::unique_ptr<Geometry> empty_coll =
      finder.clusterToCollection(std::move(empty_mp));
  ASSERT_NE(empty_coll, nullptr);
  EXPECT_TRUE(empty_coll->isEmpty());
  EXPECT_EQ(empty_coll->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(empty_coll->getNumGeometries(), 0);

  // Empty geometries have null envelopes and stay isolated singleton clusters.
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.5, 0.0));

  const Clusters mixed =
      finder.cluster({empty_pt.get(), empty_poly.get(), p0.get(), p1.get()});
  ASSERT_EQ(mixed.getNumClusters(), 3);
  EXPECT_THAT(
      ExtractSortedClusters(mixed),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1), ElementsAre(2, 3)));
}

TEST(EnvelopeDistanceClusterFinderTest,
     ClustersByEnvelopeDistanceNotGeometryDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Diagonal line (0,0)-(10,10) has envelope [0,10]x[0,10], which contains
  // (1,9) even though true geometry distance is 4*sqrt(2) ~= 5.657.
  std::unique_ptr<LineString> diag1 =
      MakeLine(*factory, {{0.0, 0.0}, {10.0, 10.0}});
  std::unique_ptr<Point> in_env = factory->createPoint(CoordinateXY(1.0, 9.0));

  {
    EnvelopeDistanceClusterFinder zero_finder(0.0);
    const Clusters clusters = zero_finder.cluster({diag1.get(), in_env.get()});
    ASSERT_EQ(clusters.getNumClusters(), 1);
    EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0));
  }

  // Parallel disjoint diagonal (11,0)-(21,10) has envelope [11,21]x[0,10],
  // giving envelope distance 1.0 despite true geometry distance > 7.7.
  std::unique_ptr<LineString> diag2 =
      MakeLine(*factory, {{11.0, 0.0}, {21.0, 10.0}});
  {
    EnvelopeDistanceClusterFinder below(0.99);
    EXPECT_EQ(below.cluster({diag1.get(), diag2.get()}).getNumClusters(), 2);

    EnvelopeDistanceClusterFinder exact(1.0);
    EXPECT_EQ(exact.cluster({diag1.get(), diag2.get()}).getNumClusters(), 1);
  }

  // Concave C-shaped polygon [0,10]x[0,10] with pocket [2,10]x[2,8] joins with
  // a point at (6, 5) inside the pocket because its envelope covers (6, 5).
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
  std::unique_ptr<Point> pocket_pt =
      factory->createPoint(CoordinateXY(6.0, 5.0));
  EnvelopeDistanceClusterFinder zero_finder(0.0);
  EXPECT_EQ(
      zero_finder.cluster({c_poly.get(), pocket_pt.get()}).getNumClusters(), 1);
}

TEST(EnvelopeDistanceClusterFinderTest,
     EuclideanEnvelopeDistanceRejectsChebyshevOnlyCandidates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Exact 3-4-5 triangle: distanceSquared((0,0), (3,4)) == 25 == 5.0^2.
  // (4,4) lies inside the expanded queryEnvelope [-5,5]x[-5,5] (Chebyshev
  // distance 4 <= 5), but has distanceSquared == 32 > 25, so shouldJoin
  // rejects.
  std::unique_ptr<Point> origin = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> exact_pt =
      factory->createPoint(CoordinateXY(3.0, 4.0));
  std::unique_ptr<Point> chebyshev_only_pt =
      factory->createPoint(CoordinateXY(4.0, 4.0));

  EnvelopeDistanceClusterFinder exact_finder(5.0);
  EXPECT_EQ(
      exact_finder.cluster({origin.get(), exact_pt.get()}).getNumClusters(), 1);
  EXPECT_EQ(exact_finder.cluster({origin.get(), chebyshev_only_pt.get()})
                .getNumClusters(),
            2);

  EnvelopeDistanceClusterFinder slightly_below(4.999999);
  EXPECT_EQ(
      slightly_below.cluster({origin.get(), exact_pt.get()}).getNumClusters(),
      2);

  // Same 3-4-5 vs (4,4) distinction between box envelopes.
  std::unique_ptr<Polygon> box_a = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::unique_ptr<Polygon> box_b = MakeBox(*factory, 5.0, 6.0, 7.0, 8.0);
  std::unique_ptr<Polygon> box_c = MakeBox(*factory, 6.0, 6.0, 8.0, 8.0);

  EXPECT_EQ(exact_finder.cluster({box_a.get(), box_b.get()}).getNumClusters(),
            1);
  EXPECT_EQ(slightly_below.cluster({box_a.get(), box_b.get()}).getNumClusters(),
            2);
  EXPECT_EQ(exact_finder.cluster({box_a.get(), box_c.get()}).getNumClusters(),
            2);
}

TEST(EnvelopeDistanceClusterFinderTest,
     ZeroDistanceClustersTouchingAndOverlappingOnly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> base = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::unique_ptr<Polygon> edge_touch = MakeBox(*factory, 2.0, 0.0, 4.0, 2.0);
  std::unique_ptr<Polygon> corner_touch = MakeBox(*factory, 2.0, 2.0, 4.0, 4.0);
  std::unique_ptr<Polygon> separated =
      MakeBox(*factory, 4.0 + 1e-9, 0.0, 6.0, 2.0);

  EnvelopeDistanceClusterFinder finder(0.0);
  const Clusters clusters = finder.cluster(
      {base.get(), edge_touch.get(), corner_touch.get(), separated.get()});
  ASSERT_EQ(clusters.getNumClusters(), 2);
  EXPECT_THAT(ExtractSortedClusters(clusters),
              UnorderedElementsAre(ElementsAre(0, 1, 2), ElementsAre(3)));
}

TEST(EnvelopeDistanceClusterFinderTest,
     TransitiveChainingMultiNodeSTRtreeAndCollectionReuse) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 3857);

  // 25 boxes arranged as 5 chains of 5 boxes. Within each chain, consecutive
  // boxes have gap 1.0 (end-to-end span 9.0); consecutive chains have gap 5.0.
  // Exceeds STRtree default nodeCapacity (10) so internal nodes are exercised.
  std::vector<std::unique_ptr<Geometry>> geoms;
  for (int group = 0; group < 5; ++group) {
    const double base_y = static_cast<double>(group) * 10.0;
    for (int step = 0; step < 5; ++step) {
      const double min_x = static_cast<double>(step) * 2.0;
      geoms.push_back(
          MakeBox(*factory, min_x, base_y, min_x + 1.0, base_y + 1.0));
    }
  }
  std::unique_ptr<GeometryCollection> coll =
      factory->createGeometryCollection(std::move(geoms));

  EnvelopeDistanceClusterFinder finder(1.0);
  std::vector<std::unique_ptr<Geometry>> vec_clusters =
      finder.clusterToVector(*coll);
  ASSERT_EQ(vec_clusters.size(), 5);
  for (const auto& cluster_geom : vec_clusters) {
    EXPECT_EQ(cluster_geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(cluster_geom->getNumGeometries(), 5);
  }

  // Reuse the same finder instance on clusterToCollection (rvalue &&).
  std::unique_ptr<Geometry> coll_clusters =
      finder.clusterToCollection(std::move(coll));
  ASSERT_NE(coll_clusters, nullptr);
  EXPECT_EQ(coll_clusters->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(coll_clusters->getFactory(), factory.get());
  EXPECT_EQ(coll_clusters->getSRID(), 3857);
  EXPECT_EQ(coll_clusters->getNumGeometries(), 5);
}

class ExposedEnvelopeDistanceClusterFinder final
    : public EnvelopeDistanceClusterFinder {
 public:
  using EnvelopeDistanceClusterFinder::EnvelopeDistanceClusterFinder;

  const Envelope& CallQueryEnvelope(const Geometry* a) {
    return queryEnvelope(a);
  }

  bool CallShouldJoin(const Geometry* a, const Geometry* b) {
    return shouldJoin(a, b);
  }
};

TEST(EnvelopeDistanceClusterFinderTest,
     SpecialDistancesAndDirectProtectedHooks) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(100.0, 200.0));
  std::unique_ptr<Point> empty_pt = factory->createPoint();

  // NaN distance yields null query envelopes and false comparisons.
  {
    EnvelopeDistanceClusterFinder nan_finder(
        std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(nan_finder.cluster({p0.get(), p1.get()}).getNumClusters(), 2);
  }

  // Negative distance collapses point query envelopes to null.
  {
    EnvelopeDistanceClusterFinder neg_finder(-1.0);
    EXPECT_EQ(neg_finder.cluster({p0.get(), p1.get()}).getNumClusters(), 2);
  }

  // +Inf distance clusters all non-empty geometries while keeping empty apart.
  {
    EnvelopeDistanceClusterFinder inf_finder(
        std::numeric_limits<double>::infinity());
    const Clusters clusters =
        inf_finder.cluster({p0.get(), p2.get(), empty_pt.get()});
    ASSERT_EQ(clusters.getNumClusters(), 2);
    EXPECT_THAT(ExtractSortedClusters(clusters),
                UnorderedElementsAre(ElementsAre(0, 1), ElementsAre(2)));
  }

  // Direct verification of queryEnvelope expansion and shouldJoin symmetry.
  ExposedEnvelopeDistanceClusterFinder exposed(2.5);
  std::unique_ptr<Polygon> box = MakeBox(*factory, 1.0, 2.0, 3.0, 5.0);
  const Envelope& expanded = exposed.CallQueryEnvelope(box.get());
  ASSERT_FALSE(expanded.isNull());
  EXPECT_DOUBLE_EQ(expanded.getMinX(), -1.5);
  EXPECT_DOUBLE_EQ(expanded.getMaxX(), 5.5);
  EXPECT_DOUBLE_EQ(expanded.getMinY(), -0.5);
  EXPECT_DOUBLE_EQ(expanded.getMaxY(), 7.5);

  EXPECT_TRUE(exposed.CallQueryEnvelope(empty_pt.get()).isNull());

  std::unique_ptr<Point> near_pt = factory->createPoint(CoordinateXY(5.5, 5.0));
  std::unique_ptr<Point> far_pt = factory->createPoint(CoordinateXY(5.6, 5.0));
  EXPECT_TRUE(exposed.CallShouldJoin(box.get(), near_pt.get()));
  EXPECT_TRUE(exposed.CallShouldJoin(near_pt.get(), box.get()));
  EXPECT_FALSE(exposed.CallShouldJoin(box.get(), far_pt.get()));
  EXPECT_FALSE(exposed.CallShouldJoin(far_pt.get(), box.get()));
}

}  // namespace
}  // namespace geos_google
