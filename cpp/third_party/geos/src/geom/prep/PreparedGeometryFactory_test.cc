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

#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"

#include <memory>
#include <string>
#include <typeinfo>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/BasicPreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedLineString.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPoint.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::prep::BasicPreparedGeometry;
using geos::geom::prep::PreparedGeometry;
using geos::geom::prep::PreparedGeometryFactory;
using geos::geom::prep::PreparedLineString;
using geos::geom::prep::PreparedPoint;
using geos::geom::prep::PreparedPolygon;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

class PreparedGeometryFactoryTest : public ::testing::Test {
 protected:
  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(PreparedGeometryFactoryTest, NullGeometryThrowsIllegalArgument) {
  PreparedGeometryFactory prep_factory;
  EXPECT_THROW(prep_factory.create(nullptr), IllegalArgumentException);
  EXPECT_THROW(PreparedGeometryFactory::prepare(nullptr),
               IllegalArgumentException);
}

TEST_F(PreparedGeometryFactoryTest, CurvedGeometryThrowsUnsupportedOperation) {
  PreparedGeometryFactory prep_factory;
  const char* const kCurvedWkts[] = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "CIRCULARSTRING EMPTY",
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "MULTICURVE (CIRCULARSTRING (0 0, 1 1, 2 0))",
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)))",
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))",
  };

  for (const char* wkt : kCurvedWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    ASSERT_NE(geom, nullptr);
    EXPECT_THROW(prep_factory.create(geom.get()),
                 UnsupportedOperationException);
    EXPECT_THROW(PreparedGeometryFactory::prepare(geom.get()),
                 UnsupportedOperationException);
  }
}

TEST_F(PreparedGeometryFactoryTest, CreatesPreparedPointForPuntalTypes) {
  PreparedGeometryFactory prep_factory;

  auto pt = ReadWkt("POINT (1 2)");
  auto pt_empty = ReadWkt("POINT EMPTY");
  auto mpt = ReadWkt("MULTIPOINT ((0 0), (2 2))");
  auto mpt_empty = ReadWkt("MULTIPOINT EMPTY");
  auto hit = ReadWkt("POINT (2 2)");
  auto miss = ReadWkt("POINT (9 9)");

  for (const Geometry* g :
       {pt.get(), pt_empty.get(), mpt.get(), mpt_empty.get()}) {
    std::unique_ptr<PreparedGeometry> pg = prep_factory.create(g);
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(&pg->getGeometry(), g);
    EXPECT_NE(dynamic_cast<const PreparedPoint*>(pg.get()), nullptr);
    EXPECT_EQ(typeid(*pg), typeid(PreparedPoint));
  }

  std::unique_ptr<PreparedGeometry> pg_pt =
      PreparedGeometryFactory::prepare(pt.get());
  EXPECT_TRUE(pg_pt->intersects(pt.get()));
  EXPECT_FALSE(pg_pt->intersects(miss.get()));

  std::unique_ptr<PreparedGeometry> pg_mpt = prep_factory.create(mpt.get());
  EXPECT_TRUE(pg_mpt->intersects(hit.get()));
  EXPECT_FALSE(pg_mpt->intersects(miss.get()));
}

TEST_F(PreparedGeometryFactoryTest, CreatesPreparedLineStringForLinealTypes) {
  PreparedGeometryFactory prep_factory;

  auto ls = ReadWkt("LINESTRING (0 0, 4 0)");
  auto ls_empty = ReadWkt("LINESTRING EMPTY");
  auto lr = ReadWkt("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  auto lr_empty = ReadWkt("LINEARRING EMPTY");
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 2 0), (0 2, 2 2))");
  auto mls_empty = ReadWkt("MULTILINESTRING EMPTY");
  auto cross = ReadWkt("LINESTRING (1 -1, 1 1)");
  auto far_pt = ReadWkt("POINT (10 10)");

  for (const Geometry* g : {ls.get(), ls_empty.get(), lr.get(), lr_empty.get(),
                            mls.get(), mls_empty.get()}) {
    std::unique_ptr<PreparedGeometry> pg = prep_factory.create(g);
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(&pg->getGeometry(), g);
    EXPECT_NE(dynamic_cast<const PreparedLineString*>(pg.get()), nullptr);
    EXPECT_EQ(typeid(*pg), typeid(PreparedLineString));
  }

  std::unique_ptr<PreparedGeometry> pg_ls = prep_factory.create(ls.get());
  EXPECT_TRUE(pg_ls->intersects(cross.get()));
  EXPECT_FALSE(pg_ls->intersects(far_pt.get()));

  std::unique_ptr<PreparedGeometry> pg_lr = prep_factory.create(lr.get());
  EXPECT_TRUE(pg_lr->intersects(cross.get()));
  EXPECT_FALSE(pg_lr->intersects(far_pt.get()));

  std::unique_ptr<PreparedGeometry> pg_mls =
      PreparedGeometryFactory::prepare(mls.get());
  EXPECT_TRUE(pg_mls->intersects(cross.get()));
  EXPECT_FALSE(pg_mls->intersects(far_pt.get()));
}

TEST_F(PreparedGeometryFactoryTest, CreatesPreparedPolygonForPolygonalTypes) {
  PreparedGeometryFactory prep_factory;

  auto poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto poly_empty = ReadWkt("POLYGON EMPTY");
  auto mpoly = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((5 5, 7 5, 7 7, 5 7, 5 5)))");
  auto mpoly_empty = ReadWkt("MULTIPOLYGON EMPTY");
  auto inside_pt = ReadWkt("POINT (1 1)");
  auto outside_pt = ReadWkt("POINT (10 10)");

  for (const Geometry* g :
       {poly.get(), poly_empty.get(), mpoly.get(), mpoly_empty.get()}) {
    std::unique_ptr<PreparedGeometry> pg = prep_factory.create(g);
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(&pg->getGeometry(), g);
    EXPECT_NE(dynamic_cast<const PreparedPolygon*>(pg.get()), nullptr);
    EXPECT_EQ(typeid(*pg), typeid(PreparedPolygon));
  }

  std::unique_ptr<PreparedGeometry> pg_poly = prep_factory.create(poly.get());
  EXPECT_TRUE(pg_poly->contains(inside_pt.get()));
  EXPECT_FALSE(pg_poly->intersects(outside_pt.get()));

  std::unique_ptr<PreparedGeometry> pg_mpoly =
      PreparedGeometryFactory::prepare(mpoly.get());
  EXPECT_TRUE(pg_mpoly->contains(inside_pt.get()));
  EXPECT_FALSE(pg_mpoly->intersects(outside_pt.get()));
}

TEST_F(PreparedGeometryFactoryTest,
       CreatesBasicPreparedGeometryForDefaultTypes) {
  PreparedGeometryFactory prep_factory;

  const char* const kDefaultWkts[] = {
      "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (1 1, 3 3))",
      "GEOMETRYCOLLECTION EMPTY",
      "COMPOUNDCURVE ((0 0, 1 0), (1 0, 1 1))",
      "COMPOUNDCURVE EMPTY",
      "CURVEPOLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
      "CURVEPOLYGON EMPTY",
      "MULTICURVE ((0 0, 1 1))",
      "MULTICURVE EMPTY",
      "MULTISURFACE (((0 0, 2 0, 2 2, 0 2, 0 0)))",
      "MULTISURFACE EMPTY",
  };

  for (const char* wkt : kDefaultWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    std::unique_ptr<PreparedGeometry> pg = prep_factory.create(geom.get());
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(&pg->getGeometry(), geom.get());
    EXPECT_NE(dynamic_cast<const BasicPreparedGeometry*>(pg.get()), nullptr);
    EXPECT_EQ(dynamic_cast<const PreparedPoint*>(pg.get()), nullptr);
    EXPECT_EQ(dynamic_cast<const PreparedLineString*>(pg.get()), nullptr);
    EXPECT_EQ(dynamic_cast<const PreparedPolygon*>(pg.get()), nullptr);
    EXPECT_EQ(typeid(*pg), typeid(BasicPreparedGeometry));
  }

  auto gc = ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (1 1, 3 3))");
  auto gc_empty = ReadWkt("GEOMETRYCOLLECTION EMPTY");
  auto hit = ReadWkt("POINT (2 2)");
  auto miss = ReadWkt("POINT (9 9)");

  std::unique_ptr<PreparedGeometry> pg_gc = prep_factory.create(gc.get());
  EXPECT_TRUE(pg_gc->intersects(hit.get()));
  EXPECT_FALSE(pg_gc->intersects(miss.get()));
  EXPECT_TRUE(pg_gc->disjoint(miss.get()));

  std::unique_ptr<PreparedGeometry> pg_gc_empty =
      PreparedGeometryFactory::prepare(gc_empty.get());
  EXPECT_FALSE(pg_gc_empty->intersects(hit.get()));
  EXPECT_TRUE(pg_gc_empty->disjoint(hit.get()));
}

TEST_F(PreparedGeometryFactoryTest, StaticPrepareAndDestroyManageLifecycle) {
  auto line = ReadWkt("LINESTRING (0 0, 5 5)");
  auto pt = ReadWkt("POINT (2 2)");

  const PreparedGeometry* raw =
      PreparedGeometryFactory::prepare(line.get()).release();
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(&raw->getGeometry(), line.get());
  EXPECT_TRUE(raw->intersects(pt.get()));

  PreparedGeometryFactory::destroy(raw);
  PreparedGeometryFactory::destroy(nullptr);
}

}  // namespace
}  // namespace geos_google
