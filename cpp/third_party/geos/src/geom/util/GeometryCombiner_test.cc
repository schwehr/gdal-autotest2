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

#include "third_party/geos/include/geos/geom/util/GeometryCombiner.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::PrecisionModel;
using ::geos::geom::util::GeometryCombiner;
using ::geos::io::WKTReader;

std::unique_ptr<Geometry> ReadWkt(const GeometryFactory* factory,
                                  const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read(wkt);
}

TEST(GeometryCombinerTest, CombineEmptyInputsReturnsEmptyCollection) {
  std::vector<const Geometry*> empty_ptrs;
  auto res_ptrs = GeometryCombiner::combine(empty_ptrs);
  ASSERT_NE(res_ptrs, nullptr);
  EXPECT_TRUE(res_ptrs->isEmpty());
  EXPECT_EQ(res_ptrs->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res_ptrs->getFactory(), GeometryFactory::getDefaultInstance());

  std::vector<std::unique_ptr<Geometry>> empty_owned;
  auto res_owned = GeometryCombiner::combine(std::move(empty_owned));
  ASSERT_NE(res_owned, nullptr);
  EXPECT_TRUE(res_owned->isEmpty());
  EXPECT_EQ(res_owned->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res_owned->getFactory(), GeometryFactory::getDefaultInstance());

  // Empty collections expand to 0 components.
  PrecisionModel pm(10.0);
  auto custom_factory = GeometryFactory::create(&pm, 4326);
  auto empty_mp = ReadWkt(custom_factory.get(), "MULTIPOINT EMPTY");
  std::vector<const Geometry*> only_empty_coll = {empty_mp.get()};
  auto res_empty_coll = GeometryCombiner::combine(only_empty_coll);
  EXPECT_TRUE(res_empty_coll->isEmpty());
  EXPECT_EQ(res_empty_coll->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res_empty_coll->getFactory(),
            GeometryFactory::getDefaultInstance());
}

TEST(GeometryCombinerTest, CombineTwoConstPointers) {
  auto factory = GeometryFactory::create();
  auto p1 = ReadWkt(factory.get(), "POINT (1 2)");
  auto p2 = ReadWkt(factory.get(), "POINT (3 4)");
  auto mp = GeometryCombiner::combine(p1.get(), p2.get());
  EXPECT_EQ(mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp->getNumGeometries(), 2u);
  EXPECT_TRUE(mp->getGeometryN(0)->equalsExact(p1.get(), 0.0));
  EXPECT_TRUE(mp->getGeometryN(1)->equalsExact(p2.get(), 0.0));

  auto l1 = ReadWkt(factory.get(), "LINESTRING (0 0, 1 1)");
  auto l2 = ReadWkt(factory.get(), "LINESTRING (2 2, 3 3)");
  auto mls = GeometryCombiner::combine(l1.get(), l2.get());
  EXPECT_EQ(mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mls->getNumGeometries(), 2u);

  auto poly1 = ReadWkt(factory.get(), "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  auto poly2 = ReadWkt(factory.get(), "POLYGON ((5 5, 6 5, 6 6, 5 6, 5 5))");
  auto mpoly = GeometryCombiner::combine(poly1.get(), poly2.get());
  EXPECT_EQ(mpoly->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(mpoly->getNumGeometries(), 2u);

  auto mixed = GeometryCombiner::combine(p1.get(), l1.get());
  EXPECT_EQ(mixed->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(mixed->getNumGeometries(), 2u);

  // Combining Point with MultiPoint flattens into a single MultiPoint.
  auto mpt_expanded = GeometryCombiner::combine(p1.get(), mp.get());
  EXPECT_EQ(mpt_expanded->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mpt_expanded->getNumGeometries(), 3u);
}

TEST(GeometryCombinerTest, CombineTwoUniquePointers) {
  auto factory = GeometryFactory::create();

  auto pts = GeometryCombiner::combine(ReadWkt(factory.get(), "POINT (0 0)"),
                                       ReadWkt(factory.get(), "POINT (1 1)"));
  EXPECT_EQ(pts->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(pts->getNumGeometries(), 2u);

  auto lines = GeometryCombiner::combine(
      ReadWkt(factory.get(), "LINESTRING (0 0, 1 0)"),
      ReadWkt(factory.get(), "MULTILINESTRING ((2 0, 3 0), (4 0, 5 0))"));
  EXPECT_EQ(lines->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(lines->getNumGeometries(), 3u);

  auto mixed = GeometryCombiner::combine(
      ReadWkt(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"),
      ReadWkt(factory.get(),
              "GEOMETRYCOLLECTION (POINT (5 5), "
              "POLYGON ((10 0, 12 0, 12 2, 10 2, 10 0)))"));
  EXPECT_EQ(mixed->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(mixed->getNumGeometries(), 3u);
}

TEST(GeometryCombinerTest, CombineThreeConstPointers) {
  auto factory = GeometryFactory::create();
  auto p1 = ReadWkt(factory.get(), "POINT (1 1)");
  auto p2 = ReadWkt(factory.get(), "POINT (2 2)");
  auto p3 = ReadWkt(factory.get(), "POINT (3 3)");

  auto mp = GeometryCombiner::combine(p1.get(), p2.get(), p3.get());
  EXPECT_EQ(mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp->getNumGeometries(), 3u);

  auto line = ReadWkt(factory.get(), "LINESTRING (0 0, 5 5)");
  auto poly = ReadWkt(factory.get(), "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  auto gc = GeometryCombiner::combine(p1.get(), line.get(), poly.get());
  EXPECT_EQ(gc->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc->getNumGeometries(), 3u);
}

TEST(GeometryCombinerTest, CombineThreeUniquePointers) {
  auto factory = GeometryFactory::create();

  auto mls = GeometryCombiner::combine(
      ReadWkt(factory.get(), "LINESTRING (0 0, 1 0)"),
      ReadWkt(factory.get(), "LINESTRING (2 0, 3 0)"),
      ReadWkt(factory.get(), "LINESTRING (4 0, 5 0)"));
  EXPECT_EQ(mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mls->getNumGeometries(), 3u);

  auto gc = GeometryCombiner::combine(
      ReadWkt(factory.get(), "MULTIPOINT ((0 0), (1 1))"),
      ReadWkt(factory.get(), "POINT (2 2)"),
      ReadWkt(factory.get(), "MULTILINESTRING ((3 3, 4 4), (5 5, 6 6))"));
  EXPECT_EQ(gc->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc->getNumGeometries(), 5u);
}

TEST(GeometryCombinerTest, CombineSingleElementReturnsAtomicGeometry) {
  auto factory = GeometryFactory::create();
  auto poly = ReadWkt(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  std::vector<const Geometry*> single = {poly.get()};
  auto res1 = GeometryCombiner::combine(single);
  EXPECT_EQ(res1->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res1->equalsExact(poly.get(), 0.0));

  auto single_mp =
      ReadWkt(factory.get(), "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)))");
  std::vector<std::unique_ptr<Geometry>> owned;
  owned.push_back(std::move(single_mp));
  auto res2 = GeometryCombiner::combine(std::move(owned));
  EXPECT_EQ(res2->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res2->equalsExact(poly.get(), 0.0));
}

TEST(GeometryCombinerTest, NestedGeometryCollectionFlattensOneLevel) {
  auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(ReadWkt(factory.get(), "POINT (1 1)"));
  inner_geoms.push_back(ReadWkt(factory.get(), "LINESTRING (0 0, 2 2)"));
  auto inner = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(std::move(inner));
  outer_geoms.push_back(ReadWkt(factory.get(), "POINT (3 3)"));
  auto outer = factory->createGeometryCollection(std::move(outer_geoms));

  std::vector<std::unique_ptr<Geometry>> inputs;
  inputs.push_back(std::move(outer));
  inputs.push_back(ReadWkt(factory.get(), "POINT (4 4)"));

  auto combined = GeometryCombiner::combine(std::move(inputs));
  EXPECT_EQ(combined->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(combined->getNumGeometries(), 3u);
  EXPECT_EQ(combined->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(combined->getGeometryN(0)->getNumGeometries(), 2u);
  EXPECT_EQ(combined->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(combined->getGeometryN(2)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
}

TEST(GeometryCombinerTest, PreservesCustomFactoryAndSrid) {
  PrecisionModel pm(100.0);
  auto factory = GeometryFactory::create(&pm, 4326);

  auto p1 = ReadWkt(factory.get(), "POINT (1 2)");
  auto p2 = ReadWkt(factory.get(), "POINT (3 4)");

  auto res_const = GeometryCombiner::combine(p1.get(), p2.get());
  EXPECT_EQ(res_const->getFactory(), factory.get());
  EXPECT_EQ(res_const->getSRID(), 4326);

  auto res_owned = GeometryCombiner::combine(std::move(p1), std::move(p2));
  EXPECT_EQ(res_owned->getFactory(), factory.get());
  EXPECT_EQ(res_owned->getSRID(), 4326);
}

TEST(GeometryCombinerTest, SkipEmptyControlsRetentionOfEmptyElements) {
  PrecisionModel pm(10.0);
  auto factory = GeometryFactory::create(&pm, 3857);

  auto empty_pt = ReadWkt(factory.get(), "POINT EMPTY");
  auto pt = ReadWkt(factory.get(), "POINT (1 2)");
  auto empty_line = ReadWkt(factory.get(), "LINESTRING EMPTY");

  // Default skipEmpty = false retains empty atomic geometries.
  std::vector<const Geometry*> with_empty = {empty_pt.get(), pt.get(),
                                             empty_pt.get()};
  GeometryCombiner retain_combiner(with_empty);
  auto retained = retain_combiner.combine();
  EXPECT_EQ(retained->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  ASSERT_EQ(retained->getNumGeometries(), 3u);
  EXPECT_TRUE(retained->getGeometryN(0)->isEmpty());
  EXPECT_FALSE(retained->getGeometryN(1)->isEmpty());
  EXPECT_TRUE(retained->getGeometryN(2)->isEmpty());

  // setSkipEmpty(true) drops empty components and simplifies to a single Point.
  GeometryCombiner skip_const_combiner(with_empty);
  skip_const_combiner.setSkipEmpty(true);
  auto skipped_single = skip_const_combiner.combine();
  EXPECT_EQ(skipped_single->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(skipped_single->equalsExact(pt.get(), 0.0));
  EXPECT_EQ(skipped_single->getFactory(), factory.get());

  // setSkipEmpty(true) with unique_ptr constructor drops empty geometries.
  std::vector<std::unique_ptr<Geometry>> mixed_owned;
  mixed_owned.push_back(ReadWkt(factory.get(), "POLYGON EMPTY"));
  mixed_owned.push_back(
      ReadWkt(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"));
  mixed_owned.push_back(ReadWkt(factory.get(), "POINT (5 5)"));
  GeometryCombiner skip_owned_combiner(std::move(mixed_owned));
  skip_owned_combiner.setSkipEmpty(true);
  auto skipped_gc = skip_owned_combiner.combine();
  EXPECT_EQ(skipped_gc->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(skipped_gc->getNumGeometries(), 2u);
  EXPECT_EQ(skipped_gc->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_EQ(skipped_gc->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);

  // When all inputs are empty and skipped, original factory is still retained.
  std::vector<const Geometry*> all_empty = {empty_pt.get(), empty_line.get()};
  GeometryCombiner all_empty_combiner(all_empty);
  all_empty_combiner.setSkipEmpty(true);
  auto empty_result = all_empty_combiner.combine();
  EXPECT_TRUE(empty_result->isEmpty());
  EXPECT_EQ(empty_result->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(empty_result->getFactory(), factory.get());
}

}  // namespace
}  // namespace geos_google
