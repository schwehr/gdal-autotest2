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

#include "third_party/geos/include/geos/operation/union/PointGeometryUnion.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::PointGeometryUnion;

class PointGeometryUnionTest : public ::testing::Test {
 protected:
  PointGeometryUnionTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(PointGeometryUnionTest, DisjointPointAndPolygonViaStaticAndInstance) {
  auto pt = Read("POINT (20 20)");
  auto poly = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  auto static_res = PointGeometryUnion::Union(*pt, *poly);
  ASSERT_NE(static_res, nullptr);
  EXPECT_EQ(static_res->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(static_res->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(static_res->getArea(), 100.0);

  const PointGeometryUnion op(*pt, *poly);
  auto instance_res = op.Union();
  ASSERT_NE(instance_res, nullptr);
  EXPECT_TRUE(instance_res->equalsExact(static_res.get()));
}

TEST_F(PointGeometryUnionTest, PointsOnInteriorAndBoundaryReturnOtherClone) {
  auto poly_with_hole = Read(
      "POLYGON ("
      "(0 0, 10 0, 10 10, 0 10, 0 0),"
      "(2 2, 8 2, 8 8, 2 8, 2 2)"
      ")");

  // Inside polygon interior.
  auto interior_pt = Read("POINT (1 1)");
  auto res_interior = PointGeometryUnion::Union(*interior_pt, *poly_with_hole);
  ASSERT_NE(res_interior, nullptr);
  EXPECT_NE(res_interior.get(), poly_with_hole.get());
  EXPECT_EQ(res_interior->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_interior->equalsExact(poly_with_hole.get()));

  // On exterior ring boundary.
  auto shell_pt = Read("POINT (0 5)");
  auto res_shell = PointGeometryUnion::Union(*shell_pt, *poly_with_hole);
  ASSERT_NE(res_shell, nullptr);
  EXPECT_EQ(res_shell->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_shell->equalsExact(poly_with_hole.get()));

  // On interior hole boundary.
  auto hole_boundary_pt = Read("POINT (2 5)");
  auto res_hole_bdry =
      PointGeometryUnion::Union(*hole_boundary_pt, *poly_with_hole);
  ASSERT_NE(res_hole_bdry, nullptr);
  EXPECT_EQ(res_hole_bdry->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_hole_bdry->equalsExact(poly_with_hole.get()));

  // Inside hole (topologically exterior to polygon).
  auto in_hole_pt = Read("POINT (5 5)");
  auto res_in_hole = PointGeometryUnion::Union(*in_hole_pt, *poly_with_hole);
  ASSERT_NE(res_in_hole, nullptr);
  EXPECT_EQ(res_in_hole->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res_in_hole->getNumGeometries(), 2u);
}

TEST_F(PointGeometryUnionTest, EmptyPointsAndEmptyOtherGeometries) {
  auto empty_pt = Read("POINT EMPTY");
  auto empty_mp = Read("MULTIPOINT EMPTY");
  auto line = Read("LINESTRING (0 0, 10 0)");

  auto res_empty_pt = PointGeometryUnion::Union(*empty_pt, *line);
  ASSERT_NE(res_empty_pt, nullptr);
  EXPECT_EQ(res_empty_pt->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(res_empty_pt->equalsExact(line.get()));

  auto res_empty_mp = PointGeometryUnion::Union(*empty_mp, *line);
  ASSERT_NE(res_empty_mp, nullptr);
  EXPECT_EQ(res_empty_mp->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(res_empty_mp->equalsExact(line.get()));

  // MultiPoint containing an empty Point and an exterior Point.
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint());
  pts.push_back(factory_->createPoint(Coordinate(5.0, 5.0)));
  auto mixed_mp = factory_->createMultiPoint(std::move(pts));
  auto empty_gc = Read("GEOMETRYCOLLECTION EMPTY");

  auto res_vs_empty_gc = PointGeometryUnion::Union(*mixed_mp, *empty_gc);
  ASSERT_NE(res_vs_empty_gc, nullptr);
  EXPECT_EQ(res_vs_empty_gc->getGeometryTypeId(), geos::geom::GEOS_POINT);
  auto expected_pt = Read("POINT (5 5)");
  EXPECT_TRUE(res_vs_empty_gc->equalsExact(expected_pt.get()));
}

TEST_F(PointGeometryUnionTest, MultiPointFiltersAndDeduplicatesExteriorCoords) {
  auto line = Read("LINESTRING (0 0, 10 0)");
  auto mp = Read("MULTIPOINT ((0 0), (5 0), (3 3), (3 3), (-2 1))");

  // (0 0) is on line boundary, (5 0) is on line interior, (3 3) is duplicated,
  // and (-2 1) is exterior -> 2 exterior coords combined with line.
  auto res = PointGeometryUnion::Union(*mp, *line);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getNumGeometries(), 3u);

  auto expected = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (-2 1),"
      "POINT (3 3),"
      "LINESTRING (0 0, 10 0)"
      ")");
  EXPECT_TRUE(res->equals(expected.get()));
}

TEST_F(PointGeometryUnionTest, DuplicateExteriorPointsCollapseToSinglePoint) {
  auto line = Read("LINESTRING (0 0, 10 0)");
  auto mp = Read("MULTIPOINT ((5 0), (10 10), (10 10))");

  auto res = PointGeometryUnion::Union(*mp, *line);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getNumGeometries(), 2u);

  auto expected = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (10 10),"
      "LINESTRING (0 0, 10 0)"
      ")");
  EXPECT_TRUE(res->equals(expected.get()));
}

TEST_F(PointGeometryUnionTest, PuntalAndCollectionOtherGeometries) {
  auto pt_same = Read("POINT (1 2)");
  auto pt_other = Read("POINT (1 2)");
  auto same_res = PointGeometryUnion::Union(*pt_same, *pt_other);
  EXPECT_EQ(same_res->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(same_res->equalsExact(pt_same.get()));

  auto pt_diff = Read("POINT (3 4)");
  auto pair_res = PointGeometryUnion::Union(*pt_same, *pt_diff);
  EXPECT_EQ(pair_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(pair_res->getNumGeometries(), 2u);

  auto mp_a = Read("MULTIPOINT ((1 2), (3 4))");
  auto mp_b = Read("MULTIPOINT ((3 4), (5 6))");
  auto mp_res = PointGeometryUnion::Union(*mp_a, *mp_b);
  EXPECT_EQ(mp_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp_res->getNumGeometries(), 3u);

  auto mp_c = Read("MULTIPOINT ((1 1), (5 5), (20 20))");
  auto gc = Read("GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 5, 10 5))");
  auto gc_res = PointGeometryUnion::Union(*mp_c, *gc);
  EXPECT_EQ(gc_res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_res->getNumGeometries(), 3u);
}

TEST_F(PointGeometryUnionTest, UsesOtherGeometryFactoryAndPreservesZ) {
  PrecisionModel pm(100.0);
  auto other_factory = GeometryFactory::create(&pm, 4326);
  WKTReader other_reader(*other_factory);

  auto pt_z = Read("POINT Z (1.25 2.5 99.0)");
  auto poly_other =
      other_reader.read("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");

  auto res = PointGeometryUnion::Union(*pt_z, *poly_other);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getFactory(), other_factory.get());
  ASSERT_EQ(res->getNumGeometries(), 2u);

  const Point* out_pt = dynamic_cast<const Point*>(res->getGeometryN(0));
  ASSERT_NE(out_pt, nullptr);
  EXPECT_DOUBLE_EQ(out_pt->getX(), 1.25);
  EXPECT_DOUBLE_EQ(out_pt->getY(), 2.5);
  EXPECT_DOUBLE_EQ(out_pt->getZ(), 99.0);
}

}  // namespace
}  // namespace geos_google
