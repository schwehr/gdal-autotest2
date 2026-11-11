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

// Unit tests for third_party/geos/src/coverage/CoverageUnion.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for TopologyException
//   assertions.
// - Allocates isolated GeometryFactory instances via GeometryFactory::create().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoverageUnion.cpp

#include "third_party/geos/include/geos/coverage/CoverageUnion.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageUnion;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::util::TopologyException;

std::vector<std::unique_ptr<Geometry>> ReadAllWkt(
    const WKTReader& reader, const std::vector<std::string_view>& wkts) {
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.reserve(wkts.size());
  for (std::string_view wkt : wkts) {
    geoms.push_back(reader.read(std::string(wkt)));
  }
  return geoms;
}

std::vector<const Geometry*> AsRawPtrs(
    const std::vector<std::unique_ptr<Geometry>>& geoms) {
  std::vector<const Geometry*> ptrs;
  ptrs.reserve(geoms.size());
  for (const auto& g : geoms) {
    ptrs.push_back(g.get());
  }
  return ptrs;
}

void ExpectNormalizedExact(const Geometry* actual, const Geometry* expected,
                           double tolerance = 1e-12) {
  ASSERT_NE(actual, nullptr);
  ASSERT_NE(expected, nullptr);
  std::unique_ptr<Geometry> norm_actual = actual->clone();
  std::unique_ptr<Geometry> norm_expected = expected->clone();
  norm_actual->normalize();
  norm_expected->normalize();
  EXPECT_TRUE(norm_actual->equalsExact(norm_expected.get(), tolerance))
      << "actual: " << norm_actual->toText()
      << "\nexpected: " << norm_expected->toText();
}

TEST(CoverageUnionTest, ReturnsNullOnEmptyVectorAndNonCollectionPointer) {
  std::vector<const Geometry*> empty_vec;
  EXPECT_EQ(CoverageUnion::Union(empty_vec), nullptr);

  EXPECT_EQ(CoverageUnion::Union(static_cast<const Geometry*>(nullptr)),
            nullptr);

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> point = reader.read("POINT (1 2)");
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (0 0, 1 1)");
  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  std::unique_ptr<Geometry> ring =
      reader.read("LINEARRING (0 0, 1 0, 1 1, 0 1, 0 0)");

  EXPECT_EQ(CoverageUnion::Union(point.get()), nullptr);
  EXPECT_EQ(CoverageUnion::Union(line.get()), nullptr);
  EXPECT_EQ(CoverageUnion::Union(poly.get()), nullptr);
  EXPECT_EQ(CoverageUnion::Union(ring.get()), nullptr);
}

TEST(CoverageUnionTest, VectorUnionSingleEmptyAndAdjacentRingWithHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // All-empty polygonal inputs yield a non-null empty Polygon.
  std::vector<std::unique_ptr<Geometry>> all_empty =
      ReadAllWkt(reader, {"POLYGON EMPTY", "POLYGON EMPTY"});
  std::vector<const Geometry*> all_empty_ptrs = AsRawPtrs(all_empty);
  std::unique_ptr<Geometry> all_empty_res =
      CoverageUnion::Union(all_empty_ptrs);
  ASSERT_NE(all_empty_res, nullptr);
  EXPECT_TRUE(all_empty_res->isEmpty());
  EXPECT_EQ(all_empty_res->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_EQ(all_empty_res->getFactory(), factory.get());

  // Single polygon plus an empty polygon element.
  std::vector<std::unique_ptr<Geometry>> single_with_empty =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                             "POLYGON EMPTY",
                         });
  std::vector<const Geometry*> single_ptrs = AsRawPtrs(single_with_empty);
  std::unique_ptr<Geometry> single_res = CoverageUnion::Union(single_ptrs);
  ASSERT_NE(single_res, nullptr);
  EXPECT_TRUE(single_res->isValid());
  EXPECT_TRUE(single_res->equals(single_with_empty[0].get()));
  ExpectNormalizedExact(single_res.get(), single_with_empty[0].get());

  // Four vector-clean polygons forming a 3x3 square with a 1x1 interior hole.
  // BoundaryChainNoder preserves boundary vertices where merged chains join.
  std::vector<std::unique_ptr<Geometry>> ring_cov =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 3 0, 3 1, 2 1, 1 1, 0 1, 0 0))",
                             "POLYGON ((0 2, 1 2, 2 2, 3 2, 3 3, 0 3, 0 2))",
                             "POLYGON ((0 1, 1 1, 1 2, 0 2, 0 1))",
                             "POLYGON ((2 1, 3 1, 3 2, 2 2, 2 1))",
                         });
  std::vector<const Geometry*> ring_ptrs = AsRawPtrs(ring_cov);
  std::unique_ptr<Geometry> ring_union = CoverageUnion::Union(ring_ptrs);
  ASSERT_NE(ring_union, nullptr);
  EXPECT_TRUE(ring_union->isValid());
  EXPECT_EQ(ring_union->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(ring_union->getArea(), 8.0);

  std::unique_ptr<Geometry> expected_donut_topo = reader.read(
      "POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1))");
  EXPECT_TRUE(ring_union->equals(expected_donut_topo.get()));

  std::unique_ptr<Geometry> expected_donut_exact = reader.read(
      "POLYGON ((0 0, 3 0, 3 1, 3 2, 3 3, 0 3, 0 2, 0 1, 0 0), "
      "(1 1, 1 2, 2 2, 2 1, 1 1))");
  ExpectNormalizedExact(ring_union.get(), expected_donut_exact.get());
}

TEST(CoverageUnionTest, VectorUnionDisjointMultiPolygonAndLinearCoverage) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::vector<std::unique_ptr<Geometry>> disjoint =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                             "POLYGON ((2 0, 3 0, 3 1, 2 1, 2 0))",
                         });
  std::vector<const Geometry*> disjoint_ptrs = AsRawPtrs(disjoint);
  std::unique_ptr<Geometry> disjoint_union =
      CoverageUnion::Union(disjoint_ptrs);
  ASSERT_NE(disjoint_union, nullptr);
  EXPECT_TRUE(disjoint_union->isValid());
  EXPECT_EQ(disjoint_union->getGeometryTypeId(),
            GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(disjoint_union->getArea(), 2.0);
  std::unique_ptr<Geometry> expected_mp = reader.read(
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((2 0, 3 0, 3 1, 2 1, 2 0)))");
  EXPECT_TRUE(disjoint_union->equals(expected_mp.get()));
  ExpectNormalizedExact(disjoint_union.get(), expected_mp.get());

  // MultiPolygon whose components are bridged into a single Polygon by a third
  // square.
  std::vector<std::unique_ptr<Geometry>> bridged =
      ReadAllWkt(reader, {
                             "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
                             "((2 0, 3 0, 3 1, 2 1, 2 0)))",
                             "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))",
                         });
  std::vector<const Geometry*> bridged_ptrs = AsRawPtrs(bridged);
  std::unique_ptr<Geometry> bridged_union = CoverageUnion::Union(bridged_ptrs);
  ASSERT_NE(bridged_union, nullptr);
  EXPECT_TRUE(bridged_union->isValid());
  EXPECT_EQ(bridged_union->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(bridged_union->getArea(), 3.0);
  std::unique_ptr<Geometry> expected_bar_topo =
      reader.read("POLYGON ((0 0, 3 0, 3 1, 0 1, 0 0))");
  EXPECT_TRUE(bridged_union->equals(expected_bar_topo.get()));
  std::unique_ptr<Geometry> expected_bar_exact =
      reader.read("POLYGON ((0 0, 1 0, 2 0, 3 0, 3 1, 2 1, 1 1, 0 1, 0 0))");
  ExpectNormalizedExact(bridged_union.get(), expected_bar_exact.get());

  // Vector of LineStrings exercises the SegmentExtractingNoder linear path.
  std::vector<std::unique_ptr<Geometry>> line_vec =
      ReadAllWkt(reader, {
                             "LINESTRING (0 0, 1 0, 2 0)",
                             "LINESTRING (1 0, 2 0, 3 0)",
                         });
  std::vector<const Geometry*> line_ptrs = AsRawPtrs(line_vec);
  std::unique_ptr<Geometry> line_union = CoverageUnion::Union(line_ptrs);
  ASSERT_NE(line_union, nullptr);
  EXPECT_EQ(line_union->getDimension(), 1);
  EXPECT_DOUBLE_EQ(line_union->getLength(), 3.0);
  std::unique_ptr<Geometry> expected_line =
      reader.read("LINESTRING (0 0, 3 0)");
  EXPECT_TRUE(line_union->equals(expected_line.get()));
}

TEST(CoverageUnionTest, CollectionOverloadPolygonsEmptyAndLinearNetwork) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0)))");
  std::unique_ptr<Geometry> expected_rect_topo =
      reader.read("POLYGON ((0 0, 2 0, 2 1, 0 1, 0 0))");
  std::unique_ptr<Geometry> expected_rect_exact =
      reader.read("POLYGON ((0 0, 1 0, 2 0, 2 1, 1 1, 0 1, 0 0))");

  std::unique_ptr<Geometry> gc_res = CoverageUnion::Union(gc.get());
  ASSERT_NE(gc_res, nullptr);
  EXPECT_TRUE(gc_res->isValid());
  EXPECT_TRUE(gc_res->equals(expected_rect_topo.get()));
  ExpectNormalizedExact(gc_res.get(), expected_rect_exact.get());

  // MultiPolygon derives from GeometryCollection and is accepted directly.
  std::unique_ptr<Geometry> mp = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((1 0, 2 0, 2 1, 1 1, 1 0)))");
  std::unique_ptr<Geometry> mp_res = CoverageUnion::Union(mp.get());
  ASSERT_NE(mp_res, nullptr);
  EXPECT_TRUE(mp_res->isValid());
  EXPECT_TRUE(mp_res->equals(expected_rect_topo.get()));
  ExpectNormalizedExact(mp_res.get(), expected_rect_exact.get());

  // Empty collections produce non-null empty geometries.
  std::unique_ptr<Geometry> empty_gc = reader.read("GEOMETRYCOLLECTION EMPTY");
  std::unique_ptr<Geometry> empty_gc_res = CoverageUnion::Union(empty_gc.get());
  ASSERT_NE(empty_gc_res, nullptr);
  EXPECT_TRUE(empty_gc_res->isEmpty());

  std::unique_ptr<Geometry> empty_mp = reader.read("MULTIPOLYGON EMPTY");
  std::unique_ptr<Geometry> empty_mp_res = CoverageUnion::Union(empty_mp.get());
  ASSERT_NE(empty_mp_res, nullptr);
  EXPECT_TRUE(empty_mp_res->isEmpty());

  // Linear coverage (dimension < 2) dissolves coincident segments.
  std::unique_ptr<Geometry> lines =
      reader.read("MULTILINESTRING ((0 0, 1 0, 2 0), (1 0, 2 0, 3 0))");
  std::unique_ptr<Geometry> lines_res = CoverageUnion::Union(lines.get());
  ASSERT_NE(lines_res, nullptr);
  EXPECT_EQ(lines_res->getDimension(), 1);
  EXPECT_DOUBLE_EQ(lines_res->getLength(), 3.0);
  std::unique_ptr<Geometry> expected_line =
      reader.read("LINESTRING (0 0, 3 0)");
  EXPECT_TRUE(lines_res->equals(expected_line.get()));
}

TEST(CoverageUnionTest, PreservesFactoryAndThrowsOnInvalidCoverage) {
  PrecisionModel pm(1000.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 3857);
  WKTReader reader(*factory);

  std::vector<std::unique_ptr<Geometry>> valid =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 1.25 0, 1.25 1, 0 1, 0 0))",
                             "POLYGON ((1.25 0, 2.5 0, 2.5 1, 1.25 1, 1.25 0))",
                         });
  std::vector<const Geometry*> valid_ptrs = AsRawPtrs(valid);
  std::unique_ptr<Geometry> valid_res = CoverageUnion::Union(valid_ptrs);
  ASSERT_NE(valid_res, nullptr);
  EXPECT_TRUE(valid_res->isValid());
  EXPECT_EQ(valid_res->getFactory(), factory.get());
  EXPECT_EQ(valid_res->getSRID(), 3857);
  EXPECT_DOUBLE_EQ(valid_res->getArea(), 2.5);
  std::unique_ptr<Geometry> expected_valid_exact =
      reader.read("POLYGON ((0 0, 1.25 0, 2.5 0, 2.5 1, 1.25 1, 0 1, 0 0))");
  ExpectNormalizedExact(valid_res.get(), expected_valid_exact.get());

  // Overlapping polygons sharing boundary segments create a side-location
  // conflict and throw TopologyException via both overloads.
  std::vector<std::unique_ptr<Geometry>> overlapping =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 1 0, 2 0, 2 2, 0 2, 0 1, 0 0))",
                             "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                         });
  std::vector<const Geometry*> overlap_ptrs = AsRawPtrs(overlapping);
  EXPECT_THROW(CoverageUnion::Union(overlap_ptrs), TopologyException);

  std::unique_ptr<GeometryCollection> overlap_gc =
      factory->createGeometryCollection(overlap_ptrs);
  EXPECT_THROW(CoverageUnion::Union(overlap_gc.get()), TopologyException);

  // Duplicate identical polygons cancel all boundary segments and throw
  // TopologyException via both overloads due to area mismatch.
  std::vector<const Geometry*> duplicate_ptrs = {overlapping[0].get(),
                                                 overlapping[0].get()};
  EXPECT_THROW(CoverageUnion::Union(duplicate_ptrs), TopologyException);

  std::unique_ptr<GeometryCollection> duplicate_gc =
      factory->createGeometryCollection(duplicate_ptrs);
  EXPECT_THROW(CoverageUnion::Union(duplicate_gc.get()), TopologyException);
}

}  // namespace
}  // namespace geos_google
