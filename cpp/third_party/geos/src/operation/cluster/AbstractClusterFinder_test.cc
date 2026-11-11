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
// third_party/geos/src/operation/cluster/AbstractClusterFinder.cpp and
// third_party/geos/include/geos/operation/cluster/AbstractClusterFinder.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/cluster/AbstractClusterFinder.cpp

#include "third_party/geos/include/geos/operation/cluster/AbstractClusterFinder.h"

#include <cstddef>
#include <functional>
#include <initializer_list>
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
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
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
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::operation::cluster::AbstractClusterFinder;
using geos::operation::cluster::Clusters;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

// Concrete test subclass recording queryEnvelope and shouldJoin calls so we can
// verify AbstractClusterFinder's indexing, sorting, point-count guard, and
// union-find short-circuiting directly.
class RecordingClusterFinder final : public AbstractClusterFinder {
 public:
  using Predicate = std::function<bool(const Geometry*, const Geometry*)>;

  RecordingClusterFinder(double expansion, Predicate predicate)
      : expansion_(expansion), predicate_(std::move(predicate)) {}

  const std::vector<const Geometry*>& queried_geoms() const {
    return queried_geoms_;
  }

  const std::vector<std::pair<const Geometry*, const Geometry*>>& join_calls()
      const {
    return join_calls_;
  }

 protected:
  bool shouldJoin(const Geometry* a, const Geometry* b) override {
    join_calls_.emplace_back(a, b);
    return predicate_(a, b);
  }

  const Envelope& queryEnvelope(const Geometry* a) override {
    queried_geoms_.push_back(a);
    envelope_ = *a->getEnvelopeInternal();
    envelope_.expandBy(expansion_);
    return envelope_;
  }

 private:
  double expansion_;
  Predicate predicate_;
  Envelope envelope_;
  std::vector<const Geometry*> queried_geoms_;
  std::vector<std::pair<const Geometry*, const Geometry*>> join_calls_;
};

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

TEST(AbstractClusterFinderTest, ClusterEmptyVector) {
  RecordingClusterFinder finder(
      0.0, [](const Geometry*, const Geometry*) { return true; });

  const Clusters clusters = finder.cluster({});
  EXPECT_EQ(clusters.getNumClusters(), 0);
  EXPECT_THAT(clusters.getClusterIds(), IsEmpty());
  EXPECT_THAT(finder.queried_geoms(), IsEmpty());
  EXPECT_THAT(finder.join_calls(), IsEmpty());
}

TEST(AbstractClusterFinderTest, ClusterSingleComponentSkipsSelfJoin) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 2.0));

  RecordingClusterFinder finder(
      1.0, [](const Geometry*, const Geometry*) { return true; });

  const Clusters clusters = finder.cluster({pt.get()});
  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_EQ(clusters.getSize(0), 1);
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0));
  EXPECT_THAT(finder.queried_geoms(), ElementsAre(pt.get()));
  EXPECT_THAT(finder.join_calls(), IsEmpty());
}

TEST(AbstractClusterFinderTest,
     ProcessEnforcesPointCountOrderAndSkipsAlreadyJoined) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Distinct point counts and envelope areas, all overlapping at (1, 1):
  // - pt:   1 point, area 0
  // - poly: 5 points, area 4
  // - line: 2 points, area 9
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 1.0));
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {{0.0, 0.0}, {3.0, 3.0}});
  std::unique_ptr<Polygon> poly = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);

  RecordingClusterFinder finder(
      0.0, [](const Geometry*, const Geometry*) { return true; });

  // Input order: pt (1), line (2), poly (5).
  // i = 0 (pt): skipped for line (2) and poly (5) because 1 < 2, 1 < 5.
  // i = 1 (line): calls shouldJoin(line, pt) -> joins {0, 1};
  //   skips poly (2 < 5).
  // i = 2 (poly): sorted hits are {pt (area 0), poly (area 4), line (area 9)}.
  //   Tests shouldJoin(poly, pt) first -> joins {0, 1, 2}.
  //   Then line is already in cluster {0, 1, 2}, so uf.different(2, 1) is false
  //   and shouldJoin(poly, line) is skipped.
  const Clusters clusters = finder.cluster({pt.get(), line.get(), poly.get()});

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, 0));
  EXPECT_THAT(finder.queried_geoms(),
              ElementsAre(pt.get(), line.get(), poly.get()));
  EXPECT_THAT(finder.join_calls(), ElementsAre(Pair(line.get(), pt.get()),
                                               Pair(poly.get(), pt.get())));
}

TEST(AbstractClusterFinderTest, ProcessEvaluatesHitsInAscendingEnvelopeArea) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // All polygons have 5 points; areas are 100, 25, 9, 1.
  std::unique_ptr<Polygon> container = MakeBox(*factory, 0.0, 0.0, 10.0, 10.0);
  std::unique_ptr<Polygon> large_sq = MakeBox(*factory, 1.0, 1.0, 6.0, 6.0);
  std::unique_ptr<Polygon> med_sq = MakeBox(*factory, 1.0, 1.0, 4.0, 4.0);
  std::unique_ptr<Polygon> small_sq = MakeBox(*factory, 1.0, 1.0, 2.0, 2.0);

  // Returning false prevents joining so every hit pair is evaluated.
  RecordingClusterFinder finder(
      0.0, [](const Geometry*, const Geometry*) { return false; });

  const Clusters clusters = finder.cluster(
      {container.get(), large_sq.get(), med_sq.get(), small_sq.get()});

  EXPECT_EQ(clusters.getNumClusters(), 4);
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 1, 2, 3));

  // For i = 0 (container), candidates are small_sq (1), med_sq (9),
  // large_sq (25).
  ASSERT_GE(finder.join_calls().size(), 3);
  EXPECT_THAT(finder.join_calls()[0], Pair(container.get(), small_sq.get()));
  EXPECT_THAT(finder.join_calls()[1], Pair(container.get(), med_sq.get()));
  EXPECT_THAT(finder.join_calls()[2], Pair(container.get(), large_sq.get()));
}

TEST(AbstractClusterFinderTest, EqualPointCountsJoinTransitively) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(1.0, 0.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(2.0, 0.0));
  std::unique_ptr<Point> p3 = factory->createPoint(CoordinateXY(10.0, 10.0));

  RecordingClusterFinder finder(1.1, [](const Geometry* a, const Geometry* b) {
    return a->distance(b) <= 1.1;
  });

  const Clusters clusters =
      finder.cluster({p0.get(), p1.get(), p2.get(), p3.get()});

  ASSERT_EQ(clusters.getNumClusters(), 2);
  const std::vector<std::size_t> ids = clusters.getClusterIds();
  ASSERT_EQ(ids.size(), 4);
  EXPECT_EQ(ids[0], ids[1]);
  EXPECT_EQ(ids[1], ids[2]);
  EXPECT_NE(ids[0], ids[3]);
}

TEST(AbstractClusterFinderTest, ClusterToVectorNonCollectionGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RecordingClusterFinder finder(
      0.0, [](const Geometry*, const Geometry*) { return true; });

  // Point via const& overload leaves input intact.
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(3.0, 4.0));
  std::vector<std::unique_ptr<Geometry>> pt_out = finder.clusterToVector(*pt);
  ASSERT_EQ(pt_out.size(), 1);
  EXPECT_TRUE(pt_out[0]->equalsExact(pt.get(), 1e-9));

  // LineString via rvalue && overload.
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {{0.0, 0.0}, {2.0, 0.0}});
  std::unique_ptr<LineString> line_copy = line->clone();
  std::vector<std::unique_ptr<Geometry>> line_out =
      finder.clusterToVector(std::move(line));
  ASSERT_EQ(line_out.size(), 1);
  EXPECT_TRUE(line_out[0]->equalsExact(line_copy.get(), 1e-9));

  // Polygon via rvalue && overload.
  std::unique_ptr<Polygon> poly = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::unique_ptr<Polygon> poly_copy = poly->clone();
  std::vector<std::unique_ptr<Geometry>> poly_out =
      finder.clusterToVector(std::move(poly));
  ASSERT_EQ(poly_out.size(), 1);
  EXPECT_TRUE(poly_out[0]->equalsExact(poly_copy.get(), 1e-9));

  // Empty Point (non-collection empty geometry).
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::vector<std::unique_ptr<Geometry>> empty_pt_out =
      finder.clusterToVector(std::move(empty_pt));
  ASSERT_EQ(empty_pt_out.size(), 1);
  EXPECT_TRUE(empty_pt_out[0]->isEmpty());
  EXPECT_EQ(empty_pt_out[0]->getGeometryTypeId(), geos::geom::GEOS_POINT);
}

TEST(AbstractClusterFinderTest, ClusterToVectorEmptyCollectionsReturnEmpty) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RecordingClusterFinder finder(
      0.0, [](const Geometry*, const Geometry*) { return true; });

  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();
  EXPECT_THAT(finder.clusterToVector(*empty_gc), IsEmpty());
  EXPECT_THAT(finder.clusterToVector(std::move(empty_gc)), IsEmpty());

  std::unique_ptr<MultiPoint> empty_mp = factory->createMultiPoint();
  EXPECT_THAT(finder.clusterToVector(std::move(empty_mp)), IsEmpty());
}

TEST(AbstractClusterFinderTest,
     ClusterToVectorHomogeneousAndHeterogeneousCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RecordingClusterFinder finder(0.0, [](const Geometry* a, const Geometry* b) {
    return a->intersects(b);
  });

  // Homogeneous MultiPoint: 3 identical points + 1 distant point ->
  // 1 MultiPoint (3 points) and 1 unwrapped Point.
  std::unique_ptr<MultiPoint> mp =
      factory->createMultiPoint(std::vector<CoordinateXY>{
          {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {5.0, 5.0}});
  std::vector<std::unique_ptr<Geometry>> mp_clusters =
      finder.clusterToVector(std::move(mp));
  ASSERT_EQ(mp_clusters.size(), 2);
  std::size_t multi_count = 0;
  std::size_t single_count = 0;
  for (const auto& geom : mp_clusters) {
    if (geom->getGeometryTypeId() == geos::geom::GEOS_MULTIPOINT) {
      ++multi_count;
      EXPECT_EQ(geom->getNumGeometries(), 3);
    } else if (geom->getGeometryTypeId() == geos::geom::GEOS_POINT) {
      ++single_count;
      EXPECT_EQ(geom->getNumPoints(), 1);
    } else {
      ADD_FAILURE() << "Unexpected geometry type: " << geom->getGeometryType();
    }
  }
  EXPECT_EQ(multi_count, 1);
  EXPECT_EQ(single_count, 1);

  // Heterogeneous GeometryCollection:
  // - Cluster 1: Point(1, 1) + Polygon(0..2, 0..2) -> GeometryCollection
  // - Cluster 2: 2 touching LineStrings -> MultiLineString
  // - Cluster 3: isolated Point(20, 20) -> Point
  // - Cluster 4: POINT EMPTY -> empty Point
  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(factory->createPoint(CoordinateXY(1.0, 1.0)));
  parts.push_back(MakeBox(*factory, 0.0, 0.0, 2.0, 2.0));
  parts.push_back(MakeLine(*factory, {{10.0, 0.0}, {11.0, 0.0}}));
  parts.push_back(MakeLine(*factory, {{11.0, 0.0}, {12.0, 0.0}}));
  parts.push_back(factory->createPoint(CoordinateXY(20.0, 20.0)));
  parts.push_back(factory->createPoint());
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(parts));

  std::vector<std::unique_ptr<Geometry>> gc_clusters =
      finder.clusterToVector(*gc);
  // Input gc should remain unmodified when passed by const&.
  EXPECT_EQ(gc->getNumGeometries(), 6);
  ASSERT_EQ(gc_clusters.size(), 4);

  std::size_t mixed_gc_count = 0;
  std::size_t mls_count = 0;
  std::size_t non_empty_pt_count = 0;
  std::size_t empty_pt_count = 0;
  for (const auto& geom : gc_clusters) {
    switch (geom->getGeometryTypeId()) {
      case geos::geom::GEOS_GEOMETRYCOLLECTION:
        ++mixed_gc_count;
        EXPECT_EQ(geom->getNumGeometries(), 2);
        break;
      case geos::geom::GEOS_MULTILINESTRING:
        ++mls_count;
        EXPECT_EQ(geom->getNumGeometries(), 2);
        break;
      case geos::geom::GEOS_POINT:
        if (geom->isEmpty()) {
          ++empty_pt_count;
        } else {
          ++non_empty_pt_count;
        }
        break;
      default:
        ADD_FAILURE() << "Unexpected cluster geometry type: "
                      << geom->getGeometryType();
        break;
    }
  }
  EXPECT_EQ(mixed_gc_count, 1);
  EXPECT_EQ(mls_count, 1);
  EXPECT_EQ(non_empty_pt_count, 1);
  EXPECT_EQ(empty_pt_count, 1);
}

TEST(AbstractClusterFinderTest, ClusterToCollectionBothOverloadsAndFactory) {
  PrecisionModel pm;
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 4326);
  RecordingClusterFinder finder(0.0, [](const Geometry* a, const Geometry* b) {
    return a->intersects(b);
  });

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakeBox(*custom_factory, 0.0, 0.0, 2.0, 2.0));
  polys.push_back(MakeBox(*custom_factory, 1.0, 1.0, 3.0, 3.0));
  polys.push_back(MakeBox(*custom_factory, 10.0, 10.0, 12.0, 12.0));
  std::unique_ptr<MultiPolygon> mp =
      custom_factory->createMultiPolygon(std::move(polys));

  // const& overload keeps input intact and propagates factory/SRID.
  std::unique_ptr<Geometry> const_result = finder.clusterToCollection(*mp);
  ASSERT_NE(const_result, nullptr);
  EXPECT_EQ(mp->getNumGeometries(), 3);
  EXPECT_EQ(const_result->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(const_result->getFactory(), custom_factory.get());
  EXPECT_EQ(const_result->getSRID(), 4326);
  ASSERT_EQ(const_result->getNumGeometries(), 2);

  std::size_t multi_poly_count = 0;
  std::size_t single_poly_count = 0;
  for (std::size_t i = 0; i < const_result->getNumGeometries(); ++i) {
    const Geometry* sub = const_result->getGeometryN(i);
    if (sub->getGeometryTypeId() == geos::geom::GEOS_MULTIPOLYGON) {
      ++multi_poly_count;
      EXPECT_EQ(sub->getNumGeometries(), 2);
    } else if (sub->getGeometryTypeId() == geos::geom::GEOS_POLYGON) {
      ++single_poly_count;
    } else {
      ADD_FAILURE() << "Unexpected type: " << sub->getGeometryType();
    }
  }
  EXPECT_EQ(multi_poly_count, 1);
  EXPECT_EQ(single_poly_count, 1);

  // rvalue && overload produces identical cluster collection.
  std::unique_ptr<Geometry> moved_result =
      finder.clusterToCollection(std::move(mp));
  ASSERT_NE(moved_result, nullptr);
  EXPECT_EQ(moved_result->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(moved_result->getFactory(), custom_factory.get());
  EXPECT_EQ(moved_result->getSRID(), 4326);
  EXPECT_TRUE(moved_result->equalsExact(const_result.get(), 1e-9));

  // Empty collection yields an empty GeometryCollection.
  std::unique_ptr<GeometryCollection> empty_gc =
      custom_factory->createGeometryCollection();
  std::unique_ptr<Geometry> empty_result =
      finder.clusterToCollection(std::move(empty_gc));
  ASSERT_NE(empty_result, nullptr);
  EXPECT_TRUE(empty_result->isEmpty());
  EXPECT_EQ(empty_result->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(empty_result->getNumGeometries(), 0);
}

}  // namespace
}  // namespace geos_google
