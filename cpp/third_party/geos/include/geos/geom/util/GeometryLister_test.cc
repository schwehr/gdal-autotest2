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

#include "third_party/geos/include/geos/geom/util/GeometryLister.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

class GeometryListerTest : public ::testing::Test {
 protected:
  geos::geom::GeometryFactory::Ptr factory_ =
      geos::geom::GeometryFactory::create();
  geos::io::WKTReader reader_{*factory_};
};

TEST_F(GeometryListerTest, ListsSingletonGeometriesDirectly) {
  auto pt = reader_.read("POINT (1 2)");
  auto empty_pt = reader_.read("POINT EMPTY");
  auto line = reader_.read("LINESTRING (0 0, 3 4)");
  auto poly = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))");
  auto empty_poly = reader_.read("POLYGON EMPTY");
  auto arc = reader_.read("CIRCULARSTRING (0 0, 1 1, 2 0)");

  auto ring_coords = std::make_unique<geos::geom::CoordinateSequence>(
      std::initializer_list<geos::geom::CoordinateXY>{
          {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});
  auto ring = factory_->createLinearRing(std::move(ring_coords));

  const std::vector<const geos::geom::Geometry*> singletons = {
      pt.get(),   empty_pt.get(),   line.get(), ring.get(),
      poly.get(), empty_poly.get(), arc.get(),
  };

  for (const geos::geom::Geometry* geom : singletons) {
    std::vector<const geos::geom::Geometry*> listed;
    geos::geom::util::GeometryLister::list(geom, listed);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0], geom);
  }
}

TEST_F(GeometryListerTest, ListsHomogeneousMultiGeometries) {
  auto mpt = reader_.read("MULTIPOINT ((1 2), (3 4), (5 6))");
  std::vector<const geos::geom::Geometry*> listed;
  geos::geom::util::GeometryLister::list(mpt.get(), listed);
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0], mpt->getGeometryN(0));
  EXPECT_EQ(listed[1], mpt->getGeometryN(1));
  EXPECT_EQ(listed[2], mpt->getGeometryN(2));

  auto mls = reader_.read("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))");
  listed.clear();
  geos::geom::util::GeometryLister::list(mls.get(), listed);
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0], mls->getGeometryN(0));
  EXPECT_EQ(listed[1], mls->getGeometryN(1));

  auto mpoly = reader_.read(
      "MULTIPOLYGON ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((2 2, 4 2, 4 4, 2 4, 2 2)))");
  listed.clear();
  geos::geom::util::GeometryLister::list(mpoly.get(), listed);
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0], mpoly->getGeometryN(0));
  EXPECT_EQ(listed[1], mpoly->getGeometryN(1));
}

TEST_F(GeometryListerTest, FlattensNestedHeterogeneousCollectionsInOrder) {
  auto geom = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 2), "
      "MULTIPOINT ((3 4), (5 6)), "
      "GEOMETRYCOLLECTION ("
      "  LINESTRING (0 0, 2 2), "
      "  MULTIPOLYGON ("
      "    ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "    ((2 2, 3 2, 3 3, 2 3, 2 2)))), "
      "POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10)))");

  std::vector<const geos::geom::Geometry*> listed;
  geos::geom::util::GeometryLister::list(geom.get(), listed);
  ASSERT_EQ(listed.size(), 7u);

  const geos::geom::Geometry* pt0 = geom->getGeometryN(0);
  const geos::geom::Geometry* mp1 = geom->getGeometryN(1);
  const geos::geom::Geometry* inner_gc = geom->getGeometryN(2);
  const geos::geom::Geometry* inner_mpoly = inner_gc->getGeometryN(1);
  const geos::geom::Geometry* trailing_poly = geom->getGeometryN(3);

  EXPECT_EQ(listed[0], pt0);
  EXPECT_EQ(listed[1], mp1->getGeometryN(0));
  EXPECT_EQ(listed[2], mp1->getGeometryN(1));
  EXPECT_EQ(listed[3], inner_gc->getGeometryN(0));
  EXPECT_EQ(listed[4], inner_mpoly->getGeometryN(0));
  EXPECT_EQ(listed[5], inner_mpoly->getGeometryN(1));
  EXPECT_EQ(listed[6], trailing_poly);

  for (const geos::geom::Geometry* item : listed) {
    EXPECT_FALSE(item->isCollection());
  }
}

TEST_F(GeometryListerTest, HandlesEmptyCollectionsAndEmptyLeafComponents) {
  for (const char* wkt : {"MULTIPOINT EMPTY", "MULTILINESTRING EMPTY",
                          "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY"}) {
    auto empty_collection = reader_.read(wkt);
    std::vector<const geos::geom::Geometry*> listed;
    geos::geom::util::GeometryLister::list(empty_collection.get(), listed);
    EXPECT_TRUE(listed.empty()) << wkt;
  }

  auto mixed_empty = reader_.read(
      "GEOMETRYCOLLECTION ("
      "GEOMETRYCOLLECTION EMPTY, "
      "POINT EMPTY, "
      "MULTIPOINT (EMPTY, (7 8)))");
  std::vector<const geos::geom::Geometry*> listed;
  geos::geom::util::GeometryLister::list(mixed_empty.get(), listed);
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0], mixed_empty->getGeometryN(1));
  EXPECT_TRUE(listed[0]->isEmpty());
  EXPECT_EQ(listed[1], mixed_empty->getGeometryN(2)->getGeometryN(0));
  EXPECT_TRUE(listed[1]->isEmpty());
  EXPECT_EQ(listed[2], mixed_empty->getGeometryN(2)->getGeometryN(1));
  EXPECT_DOUBLE_EQ(listed[2]->getCoordinate()->x, 7.0);
  EXPECT_DOUBLE_EQ(listed[2]->getCoordinate()->y, 8.0);
}

TEST_F(GeometryListerTest, AppendsToExistingVectorWithoutClearing) {
  auto initial = reader_.read("POINT (0 0)");
  auto mpt = reader_.read("MULTIPOINT ((10 20), (30 40))");

  std::vector<const geos::geom::Geometry*> listed = {initial.get()};
  geos::geom::util::GeometryLister::list(mpt.get(), listed);
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0], initial.get());
  EXPECT_EQ(listed[1], mpt->getGeometryN(0));
  EXPECT_EQ(listed[2], mpt->getGeometryN(1));
}

}  // namespace
}  // namespace geos_google
