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
// third_party/geos/include/geos/operation/cluster/EnvelopeIntersectsClusterFinder.h.
//
// google3 notes:
// - Uses isolated GeometryFactory::create() instances per test.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/cluster/EnvelopeIntersectsClusterFinder.h

#include "third_party/geos/include/geos/operation/cluster/EnvelopeIntersectsClusterFinder.h"

#include <algorithm>
#include <cstddef>
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
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/operation/cluster/Clusters.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
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
using geos::operation::cluster::EnvelopeIntersectsClusterFinder;
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

TEST(EnvelopeIntersectsClusterFinderTest, EmptyInputsAndEmptyGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  EnvelopeIntersectsClusterFinder finder;

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

  // Empty geometries have null envelopes and remain separate singletons.
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<Point> p0 = factory->createPoint(CoordinateXY(1.0, 1.0));
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(1.0, 1.0));

  const Clusters mixed =
      finder.cluster({empty_pt.get(), empty_line.get(), p0.get(), p1.get()});
  ASSERT_EQ(mixed.getNumClusters(), 3);
  EXPECT_THAT(
      ExtractSortedClusters(mixed),
      UnorderedElementsAre(ElementsAre(0), ElementsAre(1), ElementsAre(2, 3)));
}

TEST(EnvelopeIntersectsClusterFinderTest,
     JoinsDisjointGeometriesWhoseEnvelopesIntersect) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Parallel disjoint diagonals with overlapping envelopes [0,10]x[0,10] and
  // [0,8]x[2,10].
  std::unique_ptr<LineString> diag1 =
      MakeLine(*factory, {{0.0, 0.0}, {10.0, 10.0}});
  std::unique_ptr<LineString> diag2 =
      MakeLine(*factory, {{0.0, 2.0}, {8.0, 10.0}});
  // Distant point outside both envelopes.
  std::unique_ptr<Point> distant =
      factory->createPoint(CoordinateXY(20.0, 20.0));

  // Concave C-shaped polygon [100,110]x[0,10] with pocket [102,110]x[2,8],
  // plus a point inside the pocket (disjoint from the polygon, overlapping
  // envelope).
  std::unique_ptr<Polygon> c_poly = MakePolygon(*factory, {
                                                              {100.0, 0.0},
                                                              {110.0, 0.0},
                                                              {110.0, 2.0},
                                                              {102.0, 2.0},
                                                              {102.0, 8.0},
                                                              {110.0, 8.0},
                                                              {110.0, 10.0},
                                                              {100.0, 10.0},
                                                              {100.0, 0.0},
                                                          });
  std::unique_ptr<Point> pocket_pt =
      factory->createPoint(CoordinateXY(106.0, 5.0));

  EnvelopeIntersectsClusterFinder finder;
  const Clusters clusters = finder.cluster(
      {diag1.get(), diag2.get(), distant.get(), c_poly.get(), pocket_pt.get()});

  ASSERT_EQ(clusters.getNumClusters(), 3);
  EXPECT_THAT(ExtractSortedClusters(clusters),
              UnorderedElementsAre(ElementsAre(0, 1), ElementsAre(2),
                                   ElementsAre(3, 4)));
}

TEST(EnvelopeIntersectsClusterFinderTest,
     BoundaryTouchingZeroAreaAndNearMissEnvelopes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Touching along an edge and at a single corner all join.
  std::unique_ptr<Polygon> base = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::unique_ptr<Polygon> edge_touch = MakeBox(*factory, 2.0, 0.0, 4.0, 2.0);
  std::unique_ptr<Polygon> corner_touch =
      MakeBox(*factory, -2.0, 2.0, 0.0, 4.0);
  // Overlaps in X only with `base` (gap 1e-9 in Y, 0.5 X-gap from neighbors)
  // and overlaps in Y only with `edge_touch` (gap 1e-9 in X).
  std::unique_ptr<Polygon> y_gap = MakeBox(*factory, 0.5, 2.0 + 1e-9, 1.5, 4.0);
  std::unique_ptr<Polygon> x_gap = MakeBox(*factory, 4.0 + 1e-9, 0.0, 6.0, 2.0);

  EnvelopeIntersectsClusterFinder finder;
  const Clusters box_clusters =
      finder.cluster({base.get(), edge_touch.get(), corner_touch.get(),
                      y_gap.get(), x_gap.get()});
  ASSERT_EQ(box_clusters.getNumClusters(), 3);
  EXPECT_THAT(ExtractSortedClusters(box_clusters),
              UnorderedElementsAre(ElementsAre(0, 1, 2), ElementsAre(3),
                                   ElementsAre(4)));

  // Zero-area envelopes: axis-aligned segments and point at (5, 5), plus
  // near-miss zero-area segments offset by 1e-6 that stay disjoint from both.
  std::unique_ptr<LineString> horiz =
      MakeLine(*factory, {{0.0, 5.0}, {10.0, 5.0}});
  std::unique_ptr<LineString> vert =
      MakeLine(*factory, {{5.0, 0.0}, {5.0, 10.0}});
  std::unique_ptr<Point> cross_pt =
      factory->createPoint(CoordinateXY(5.0, 5.0));
  std::unique_ptr<LineString> shifted_horiz =
      MakeLine(*factory, {{0.0, 5.0 + 1e-6}, {4.0, 5.0 + 1e-6}});
  std::unique_ptr<LineString> shifted_vert =
      MakeLine(*factory, {{5.0 + 1e-6, 0.0}, {5.0 + 1e-6, 4.0}});

  const Clusters zero_area_clusters =
      finder.cluster({horiz.get(), vert.get(), cross_pt.get(),
                      shifted_horiz.get(), shifted_vert.get()});
  ASSERT_EQ(zero_area_clusters.getNumClusters(), 3);
  EXPECT_THAT(ExtractSortedClusters(zero_area_clusters),
              UnorderedElementsAre(ElementsAre(0, 1, 2), ElementsAre(3),
                                   ElementsAre(4)));
}

TEST(EnvelopeIntersectsClusterFinderTest,
     TransitiveChainingMultiNodeSTRtreeAndCollections) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);

  // 24 geometries organized into 4 disjoint spatial blocks of 6 transitively
  // overlapping boxes, exercising multi-level STRtree splits (capacity 10).
  std::vector<std::unique_ptr<Geometry>> geoms;
  for (int block = 0; block < 4; ++block) {
    const double base_y = static_cast<double>(block) * 20.0;
    for (int step = 0; step < 6; ++step) {
      const double min_x = static_cast<double>(step) * 1.5;
      geoms.push_back(
          MakeBox(*factory, min_x, base_y, min_x + 2.0, base_y + 2.0));
    }
  }
  std::unique_ptr<GeometryCollection> coll =
      factory->createGeometryCollection(std::move(geoms));

  EnvelopeIntersectsClusterFinder finder;
  std::vector<std::unique_ptr<Geometry>> vec_clusters =
      finder.clusterToVector(*coll);
  ASSERT_EQ(vec_clusters.size(), 4);
  for (const auto& cluster_geom : vec_clusters) {
    EXPECT_EQ(cluster_geom->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(cluster_geom->getNumGeometries(), 6);
  }

  // Reuse the same finder on clusterToCollection (rvalue &&).
  std::unique_ptr<Geometry> coll_clusters =
      finder.clusterToCollection(std::move(coll));
  ASSERT_NE(coll_clusters, nullptr);
  EXPECT_EQ(coll_clusters->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(coll_clusters->getFactory(), factory.get());
  EXPECT_EQ(coll_clusters->getSRID(), 4326);
  EXPECT_EQ(coll_clusters->getNumGeometries(), 4);
}

class ExposedEnvelopeIntersectsClusterFinder final
    : public EnvelopeIntersectsClusterFinder {
 public:
  const Envelope& CallQueryEnvelope(const Geometry* a) {
    return queryEnvelope(a);
  }

  bool CallShouldJoin(const Geometry* a, const Geometry* b) {
    return shouldJoin(a, b);
  }
};

TEST(EnvelopeIntersectsClusterFinderTest, DirectProtectedHooksAndNullChecks) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> box1 = MakeBox(*factory, 0.0, 0.0, 2.0, 2.0);
  std::unique_ptr<Polygon> box2 = MakeBox(*factory, 2.0, 1.0, 4.0, 3.0);
  std::unique_ptr<Point> far_pt = factory->createPoint(CoordinateXY(5.0, 5.0));
  std::unique_ptr<Point> empty_pt = factory->createPoint();

  ExposedEnvelopeIntersectsClusterFinder exposed;
  EXPECT_EQ(&exposed.CallQueryEnvelope(box1.get()),
            box1->getEnvelopeInternal());
  EXPECT_TRUE(exposed.CallQueryEnvelope(empty_pt.get()).isNull());

  EXPECT_TRUE(exposed.CallShouldJoin(box1.get(), box2.get()));
  EXPECT_TRUE(exposed.CallShouldJoin(box2.get(), box1.get()));
  EXPECT_FALSE(exposed.CallShouldJoin(box1.get(), far_pt.get()));
  EXPECT_FALSE(exposed.CallShouldJoin(box1.get(), empty_pt.get()));
  EXPECT_FALSE(exposed.CallShouldJoin(empty_pt.get(), box1.get()));
  EXPECT_FALSE(exposed.CallShouldJoin(empty_pt.get(), empty_pt.get()));
}

TEST(EnvelopeIntersectsClusterFinderTest,
     AsymmetricVertexCountsNegativeZAndConstCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Small vertex-count geometries before a containing polygon still join,
  // and Z differences do not prevent 2D envelope clustering.
  std::unique_ptr<Point> pt_before =
      factory->createPoint(Coordinate(1.5, 1.5, 10.0));
  std::unique_ptr<Polygon> outer_box = MakeBox(*factory, 0.0, 0.0, 3.0, 3.0);
  std::unique_ptr<Point> pt_diff_z =
      factory->createPoint(Coordinate(1.5, 1.5, -25.0));

  // Negative-coordinate cluster where a point lies inside a diagonal
  // segment's 2D bounding box without touching the line.
  std::unique_ptr<LineString> neg_line =
      MakeLine(*factory, {{-10.0, -10.0}, {-5.0, -5.0}});
  std::unique_ptr<Point> neg_pt =
      factory->createPoint(CoordinateXY(-7.0, -8.0));

  EnvelopeIntersectsClusterFinder finder;
  const Clusters clusters =
      finder.cluster({pt_before.get(), outer_box.get(), pt_diff_z.get(),
                      neg_line.get(), neg_pt.get()});
  ASSERT_EQ(clusters.getNumClusters(), 2);
  EXPECT_THAT(ExtractSortedClusters(clusters),
              UnorderedElementsAre(ElementsAre(0, 1, 2), ElementsAre(3, 4)));

  // Const-ref clusterToCollection preserves the input heterogeneous collection.
  std::vector<std::unique_ptr<Geometry>> mixed;
  mixed.push_back(std::move(pt_before));
  mixed.push_back(std::move(outer_box));
  mixed.push_back(std::move(neg_line));
  mixed.push_back(std::move(neg_pt));
  std::unique_ptr<GeometryCollection> coll =
      factory->createGeometryCollection(std::move(mixed));

  std::unique_ptr<Geometry> grouped = finder.clusterToCollection(*coll);
  ASSERT_NE(grouped, nullptr);
  EXPECT_EQ(coll->getNumGeometries(), 4);
  EXPECT_EQ(grouped->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(grouped->getNumGeometries(), 2);
  EXPECT_EQ(grouped->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(grouped->getGeometryN(0)->getNumGeometries(), 2);
  EXPECT_EQ(grouped->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(grouped->getGeometryN(1)->getNumGeometries(), 2);
}

}  // namespace
}  // namespace geos_google
