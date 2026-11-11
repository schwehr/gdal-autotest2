// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/distance/FacetSequenceTreeBuilder.h"

#include <algorithm>
#include <cstddef>
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
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/index/strtree/TemplateSTRtree.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/distance/FacetSequence.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryCollection;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LineString;
using ::geos::geom::MultiPoint;
using ::geos::geom::Point;
using ::geos::index::strtree::TemplateSTRtree;
using ::geos::io::WKTReader;
using ::geos::operation::distance::FacetSequence;
using ::geos::operation::distance::FacetSequenceTreeBuilder;
using ::testing::UnorderedElementsAreArray;

using FacetTree = TemplateSTRtree<const FacetSequence*>;

struct FacetSequenceDistance {
  double operator()(const FacetSequence* a, const FacetSequence* b) const {
    return a->distance(*b);
  }
};

std::unique_ptr<LineString> MakeMonotonicLineString(
    const GeometryFactory& factory, std::size_t num_points) {
  CoordinateSequence coords;
  coords.reserve(num_points);
  for (std::size_t i = 0; i < num_points; ++i) {
    coords.add(Coordinate(static_cast<double>(i), static_cast<double>(i % 2)));
  }
  return factory.createLineString(coords);
}

std::vector<const FacetSequence*> CollectSortedByStartX(FacetTree& tree) {
  std::vector<const FacetSequence*> facets;
  for (const FacetSequence* fs : tree.items()) {
    facets.push_back(fs);
  }
  std::sort(facets.begin(), facets.end(),
            [](const FacetSequence* a, const FacetSequence* b) {
              if (a->getCoordinate(0)->x != b->getCoordinate(0)->x) {
                return a->getCoordinate(0)->x < b->getCoordinate(0)->x;
              }
              return a->size() > b->size();
            });
  return facets;
}

TEST(FacetSequenceTreeBuilderTest, EmptyGeometriesProduceEmptyTrees) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  const char* kEmptyWkts[] = {
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  };

  for (const char* wkt : kEmptyWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = reader.read(wkt);
    std::unique_ptr<FacetTree> tree =
        FacetSequenceTreeBuilder::build(geom.get());
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->getRoot(), nullptr);
    EXPECT_FALSE(tree->built());

    std::vector<const FacetSequence*> items;
    for (const FacetSequence* fs : tree->items()) {
      items.push_back(fs);
    }
    EXPECT_TRUE(items.empty());

    std::vector<const FacetSequence*> results;
    tree->query(Envelope(-1e6, 1e6, -1e6, 1e6), results);
    EXPECT_TRUE(results.empty());
  }
}

TEST(FacetSequenceTreeBuilderTest, SinglePointBuildsOnePointFacetSequence) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(3.5, -7.25));

  std::unique_ptr<FacetTree> tree = FacetSequenceTreeBuilder::build(pt.get());
  ASSERT_NE(tree, nullptr);
  EXPECT_TRUE(tree->built());

  std::vector<const FacetSequence*> items = CollectSortedByStartX(*tree);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0]->size(), 1u);
  EXPECT_TRUE(items[0]->isPoint());
  EXPECT_DOUBLE_EQ(items[0]->getCoordinate(0)->x, 3.5);
  EXPECT_DOUBLE_EQ(items[0]->getCoordinate(0)->y, -7.25);
  EXPECT_EQ(*items[0]->getEnvelope(), Envelope(3.5, 3.5, -7.25, -7.25));
}

TEST(FacetSequenceTreeBuilderTest, LineStringPartitioningAtBoundarySizes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  struct Case {
    std::size_t num_points;
    std::vector<std::size_t> expected_sizes;
  };
  const Case kCases[] = {
      {2, {2}},    {6, {6}},        {7, {7, 1}},     {8, {8, 2}},
      {9, {7, 3}}, {13, {7, 7, 1}}, {14, {7, 8, 2}}, {15, {7, 7, 3}},
  };

  for (const Case& tc : kCases) {
    SCOPED_TRACE(tc.num_points);
    std::unique_ptr<LineString> line =
        MakeMonotonicLineString(*factory, tc.num_points);
    std::unique_ptr<FacetTree> tree =
        FacetSequenceTreeBuilder::build(line.get());
    ASSERT_NE(tree, nullptr);
    EXPECT_TRUE(tree->built());

    std::vector<const FacetSequence*> facets = CollectSortedByStartX(*tree);
    std::vector<std::size_t> actual_sizes;
    actual_sizes.reserve(facets.size());
    for (const FacetSequence* fs : facets) {
      actual_sizes.push_back(fs->size());
    }
    EXPECT_EQ(actual_sizes, tc.expected_sizes);

    // Every consecutive segment (k, k + 1) must appear in at least one facet.
    std::vector<bool> segment_covered(tc.num_points - 1, false);
    for (const FacetSequence* fs : facets) {
      for (std::size_t j = 0; j + 1 < fs->size(); ++j) {
        const auto seg_idx = static_cast<std::size_t>(fs->getCoordinate(j)->x);
        ASSERT_LT(seg_idx, segment_covered.size());
        EXPECT_DOUBLE_EQ(fs->getCoordinate(j + 1)->x,
                         static_cast<double>(seg_idx + 1));
        segment_covered[seg_idx] = true;
      }
    }
    for (std::size_t k = 0; k < segment_covered.size(); ++k) {
      EXPECT_TRUE(segment_covered[k]) << "Missing segment " << k;
    }
  }
}

TEST(FacetSequenceTreeBuilderTest, PolygonWithHoleIndexesShellAndHoleRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Shell has 9 points (8 segments -> facets of sizes 7 and 3);
  // hole has 5 points (4 segments -> facet of size 5).
  std::unique_ptr<Geometry> poly = reader.read(
      "POLYGON ((0 0, 5 0, 10 0, 10 5, 10 10, 5 10, 0 10, 0 5, 0 0),"
      "         (2 2, 4 2, 4 4, 2 4, 2 2))");

  std::unique_ptr<FacetTree> tree = FacetSequenceTreeBuilder::build(poly.get());
  ASSERT_NE(tree, nullptr);
  EXPECT_TRUE(tree->built());

  std::vector<std::size_t> sizes;
  for (const FacetSequence* fs : tree->items()) {
    sizes.push_back(fs->size());
    EXPECT_FALSE(fs->isPoint());
  }
  EXPECT_THAT(sizes, UnorderedElementsAreArray({7u, 3u, 5u}));
}

TEST(FacetSequenceTreeBuilderTest, MultiPointIncludingEmptyComponent) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  pts.push_back(factory->createPoint());  // Empty point inside collection.
  pts.push_back(factory->createPoint(CoordinateXY(10.0, 2.0)));
  pts.push_back(factory->createPoint(CoordinateXY(20.0, 4.0)));
  std::unique_ptr<MultiPoint> mp = factory->createMultiPoint(std::move(pts));

  std::unique_ptr<FacetTree> tree = FacetSequenceTreeBuilder::build(mp.get());
  ASSERT_NE(tree, nullptr);
  EXPECT_TRUE(tree->built());

  std::vector<const FacetSequence*> facets = CollectSortedByStartX(*tree);
  ASSERT_EQ(facets.size(), 3u);
  EXPECT_DOUBLE_EQ(facets[0]->getCoordinate(0)->x, 0.0);
  EXPECT_DOUBLE_EQ(facets[1]->getCoordinate(0)->x, 10.0);
  EXPECT_DOUBLE_EQ(facets[2]->getCoordinate(0)->x, 20.0);
  for (const FacetSequence* fs : facets) {
    EXPECT_TRUE(fs->isPoint());
  }
}

TEST(FacetSequenceTreeBuilderTest, MultiLineStringAndGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> mls = reader.read(
      "MULTILINESTRING ((0 0, 1 0, 2 0), (10 10, 11 10, 12 10, 13 10))");
  std::unique_ptr<FacetTree> mls_tree =
      FacetSequenceTreeBuilder::build(mls.get());
  std::vector<std::size_t> mls_sizes;
  for (const FacetSequence* fs : mls_tree->items()) {
    mls_sizes.push_back(fs->size());
  }
  EXPECT_THAT(mls_sizes, UnorderedElementsAreArray({3u, 4u}));

  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(factory->createPoint(CoordinateXY(-5.0, -5.0)));
  parts.push_back(MakeMonotonicLineString(*factory, 9));
  parts.push_back(reader.read("LINESTRING EMPTY"));
  parts.push_back(reader.read("POLYGON ((20 0, 22 0, 22 2, 20 2, 20 0))"));
  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(parts));

  std::unique_ptr<FacetTree> gc_tree =
      FacetSequenceTreeBuilder::build(gc.get());
  std::vector<std::size_t> gc_sizes;
  for (const FacetSequence* fs : gc_tree->items()) {
    gc_sizes.push_back(fs->size());
  }
  // 1 from Point, 7 + 3 from 9-pt LineString, 0 from empty LineString,
  // 5 from Polygon shell.
  EXPECT_THAT(gc_sizes, UnorderedElementsAreArray({1u, 7u, 3u, 5u}));
}

TEST(FacetSequenceTreeBuilderTest,
     SpatialQueryAndNearestNeighbourWithBuiltTrees) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> long_line = MakeMonotonicLineString(*factory, 25);
  std::unique_ptr<Point> target = factory->createPoint(CoordinateXY(15.0, 4.0));
  std::unique_ptr<Point> empty_pt = factory->createPoint();

  std::unique_ptr<FacetTree> line_tree =
      FacetSequenceTreeBuilder::build(long_line.get());
  std::unique_ptr<FacetTree> point_tree =
      FacetSequenceTreeBuilder::build(target.get());
  std::unique_ptr<FacetTree> empty_tree =
      FacetSequenceTreeBuilder::build(empty_pt.get());

  // Envelope query around x in [13.5, 16.5] matches only the [12, 19) facet.
  std::vector<const FacetSequence*> hits;
  line_tree->query(Envelope(13.5, 16.5, -1.0, 2.0), hits);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_DOUBLE_EQ(hits[0]->getCoordinate(0)->x, 12.0);
  EXPECT_EQ(hits[0]->size(), 7u);

  // Nearest neighbour between two built FacetSequence trees.
  auto nearest_pair =
      line_tree->nearestNeighbour<FacetSequenceDistance>(*point_tree);
  ASSERT_NE(nearest_pair.first, nullptr);
  ASSERT_NE(nearest_pair.second, nullptr);
  // Target is (15, 4); the unique nearest point on the sawtooth polyline is
  // peak vertex (15, 1) inside facet [12, 19), at exact distance 3.0.
  EXPECT_DOUBLE_EQ(nearest_pair.first->distance(*nearest_pair.second), 3.0);

  // Nearest neighbour with an empty tree returns null pair.
  auto empty_pair =
      line_tree->nearestNeighbour<FacetSequenceDistance>(*empty_tree);
  EXPECT_EQ(empty_pair.first, nullptr);
  EXPECT_EQ(empty_pair.second, nullptr);
}

}  // namespace
}  // namespace geos_google
