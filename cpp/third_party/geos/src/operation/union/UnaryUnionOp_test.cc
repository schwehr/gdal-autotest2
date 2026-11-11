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

#include "third_party/geos/include/geos/operation/union/UnaryUnionOp.h"

#include <cmath>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/operation/union/CascadedPolygonUnion.h"
#include "third_party/geos/include/geos/operation/union/UnionStrategy.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::ClassicUnionStrategy;
using geos::operation::geounion::UnaryUnionOp;
using geos::operation::geounion::UnionStrategy;
using geos::operation::overlayng::OverlayNG;

class CountingClassicUnionStrategy : public UnionStrategy {
 public:
  std::unique_ptr<Geometry> Union(const Geometry* g0,
                                  const Geometry* g1) override {
    ++call_count_;
    return classic_.Union(g0, g1);
  }

  bool isFloatingPrecision() const override {
    return classic_.isFloatingPrecision();
  }

  int call_count() const { return call_count_; }

 private:
  ClassicUnionStrategy classic_;
  int call_count_ = 0;
};

class FixedPrecisionUnionStrategy : public UnionStrategy {
 public:
  explicit FixedPrecisionUnionStrategy(double scale) : pm_(scale) {}

  std::unique_ptr<Geometry> Union(const Geometry* g0,
                                  const Geometry* g1) override {
    return OverlayNG::overlay(g0, g1, OverlayNG::UNION, &pm_);
  }

  bool isFloatingPrecision() const override { return false; }

 private:
  PrecisionModel pm_;
};

class UnaryUnionOpTest : public ::testing::Test {
 protected:
  UnaryUnionOpTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(UnaryUnionOpTest, EmptyContainerWithoutFactoryReturnsNullptr) {
  std::vector<const Geometry*> geoms;
  EXPECT_EQ(UnaryUnionOp::Union(geoms), nullptr);

  UnaryUnionOp op(geoms);
  EXPECT_EQ(op.Union(), nullptr);
}

TEST_F(UnaryUnionOpTest, EmptyContainerWithFactoryReturnsEmptyCollection) {
  std::vector<const Geometry*> geoms;
  auto res = UnaryUnionOp::Union(geoms, *factory_);
  ASSERT_NE(res, nullptr);
  EXPECT_TRUE(res->isEmpty());
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getFactory(), factory_.get());

  UnaryUnionOp op(geoms, *factory_);
  auto res_op = op.Union();
  ASSERT_NE(res_op, nullptr);
  EXPECT_TRUE(res_op->isEmpty());
  EXPECT_EQ(res_op->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
}

TEST_F(UnaryUnionOpTest, EmptyGeometriesReturnExpectedEmptyResults) {
  for (const std::string& wkt :
       {"GEOMETRYCOLLECTION EMPTY", "MULTIPOINT EMPTY", "MULTILINESTRING EMPTY",
        "MULTIPOLYGON EMPTY"}) {
    auto g = Read(wkt);
    auto res = UnaryUnionOp::Union(*g);
    ASSERT_NE(res, nullptr) << wkt;
    EXPECT_TRUE(res->isEmpty()) << wkt;
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION)
        << wkt;
  }

  for (const std::string& wkt :
       {"POINT EMPTY", "LINESTRING EMPTY", "POLYGON EMPTY"}) {
    auto g = Read(wkt);
    auto res = UnaryUnionOp::Union(*g);
    ASSERT_NE(res, nullptr) << wkt;
    EXPECT_TRUE(res->isEmpty()) << wkt;
  }
}

TEST_F(UnaryUnionOpTest, PointsOnlyDeduplicatesPoints) {
  auto mp = Read("MULTIPOINT ((0 0), (1 1), (0 0), (2 2), (1 1))");
  auto res_mp = UnaryUnionOp::Union(*mp);
  ASSERT_NE(res_mp, nullptr);
  EXPECT_EQ(res_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(res_mp->getNumGeometries(), 3u);

  auto p0 = Read("POINT (5 5)");
  auto p1 = Read("POINT (5 5)");
  std::vector<const Geometry*> pts{p0.get(), p1.get()};
  auto res_pt = UnaryUnionOp::Union(pts);
  ASSERT_NE(res_pt, nullptr);
  EXPECT_EQ(res_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(res_pt->equals(p0.get()));
}

TEST_F(UnaryUnionOpTest, LinesOnlyNodesCrossingsAndDissolvesDuplicates) {
  auto l0 = Read("LINESTRING (0 0, 10 10)");
  auto l1 = Read("LINESTRING (0 10, 10 0)");
  std::list<const Geometry*> lines{l0.get(), l1.get()};

  auto res_cross = UnaryUnionOp::Union(lines);
  ASSERT_NE(res_cross, nullptr);
  EXPECT_EQ(res_cross->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(res_cross->getNumGeometries(), 4u);
  EXPECT_NEAR(res_cross->getLength(), 20.0 * std::sqrt(2.0), 1e-9);

  auto overlap_lines =
      Read("MULTILINESTRING ((0 0, 10 0), (5 0, 15 0), (0 0, 10 0))");
  auto res_dissolved = UnaryUnionOp::Union(*overlap_lines);
  ASSERT_NE(res_dissolved, nullptr);
  EXPECT_DOUBLE_EQ(res_dissolved->getLength(), 15.0);

  auto ring_and_line = Read(
      "GEOMETRYCOLLECTION ("
      "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0),"
      "LINESTRING (0 5, 10 5)"
      ")");
  auto res_ring = UnaryUnionOp::Union(*ring_and_line);
  ASSERT_NE(res_ring, nullptr);
  EXPECT_DOUBLE_EQ(res_ring->getLength(), 50.0);
}

TEST_F(UnaryUnionOpTest, PolygonsOnlyMergesOverlapsAndCleansMultiPolygon) {
  // Invalid MultiPolygon whose component polygons overlap.
  auto overlapping_mp = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)),"
      "((5 5, 15 5, 15 15, 5 15, 5 5))"
      ")");
  auto res = UnaryUnionOp::Union(*overlapping_mp);
  ASSERT_NE(res, nullptr);
  EXPECT_TRUE(res->isValid());
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(res->getArea(), 175.0);

  // Polygon with a hole partially filled by a second polygon.
  auto with_hole = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))");
  auto patch = Read("POLYGON ((2 2, 5 2, 5 8, 2 8, 2 2))");
  std::vector<const Geometry*> polys{with_hole.get(), patch.get()};
  auto res_hole = UnaryUnionOp::Union(polys);
  ASSERT_NE(res_hole, nullptr);
  EXPECT_TRUE(res_hole->isValid());
  EXPECT_DOUBLE_EQ(res_hole->getArea(), 82.0);
}

TEST_F(UnaryUnionOpTest, LinesAndPolygonsCombinedAbsorbsCoveredLinePortion) {
  auto gc = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)),"
      "LINESTRING (5 5, 15 5)"
      ")");
  auto res = UnaryUnionOp::Union(*gc);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(res->getArea(), 100.0);
  // Boundary of square (40) + uncovered line segment (10,5)-(15,5) (5).
  EXPECT_DOUBLE_EQ(res->getLength(), 45.0);
}

TEST_F(UnaryUnionOpTest, PointsAndLinesCombinedAbsorbsCoincidentPoints) {
  auto gc = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 0),"
      "POINT (5 5),"
      "LINESTRING (0 0, 10 0)"
      ")");
  auto res = UnaryUnionOp::Union(*gc);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getNumGeometries(), 2u);

  auto expected = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 5),"
      "LINESTRING (0 0, 10 0)"
      ")");
  EXPECT_TRUE(res->equals(expected.get()));
}

TEST_F(UnaryUnionOpTest,
       PointsAndPolygonsCombinedAbsorbsInteriorAndBoundaryPts) {
  auto gc = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (5 5),"
      "POINT (0 5),"
      "POINT (20 20),"
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"
      ")");
  auto res = UnaryUnionOp::Union(*gc);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getNumGeometries(), 2u);

  auto expected = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (20 20),"
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"
      ")");
  EXPECT_TRUE(res->equals(expected.get()));
}

TEST_F(UnaryUnionOpTest, NestedHeterogeneousCollectionUnionsAllDimensions) {
  auto nested = Read(
      "GEOMETRYCOLLECTION ("
      "MULTIPOINT ((5 5), (15 5), (30 30)),"
      "GEOMETRYCOLLECTION ("
      "LINESTRING (5 5, 20 5),"
      "LINESTRING (12 0, 12 10)"
      "),"
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)),"
      "POLYGON ((2 2, 12 2, 12 8, 2 8, 2 2))"
      ")");

  auto res = UnaryUnionOp::Union(*nested);
  ASSERT_NE(res, nullptr);
  EXPECT_TRUE(res->isValid());
  EXPECT_DOUBLE_EQ(res->getArea(), 112.0);

  // Exterior point (30 30) survives while (5 5) (in polygon) and (15 5)
  // (on line) are absorbed.
  auto pt_ext = Read("POINT (30 30)");
  auto pt_line = Read("POINT (15 5)");
  EXPECT_TRUE(res->covers(pt_ext.get()));
  EXPECT_TRUE(res->covers(pt_line.get()));
}

TEST_F(UnaryUnionOpTest, SetUnionFunctionOverridesUnionStrategy) {
  auto pt = Read("POINT (30 30)");
  auto l0 = Read("LINESTRING (0 0, 10 0)");
  auto p0 = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto p1 = Read("POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");
  std::vector<const Geometry*> geoms{pt.get(), l0.get(), p0.get(), p1.get()};

  CountingClassicUnionStrategy counting;
  UnaryUnionOp op(geoms);
  op.setUnionFunction(&counting);
  auto res = op.Union();
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(counting.call_count(), 3);
  EXPECT_DOUBLE_EQ(res->getArea(), 150.0);

  auto snap_p0 =
      Read("POLYGON ((0.1 0.1, 10.1 0.1, 10.1 10.1, 0.1 10.1, 0.1 0.1))");
  auto snap_p1 =
      Read("POLYGON ((5.2 0.2, 15.2 0.2, 15.2 10.2, 5.2 10.2, 5.2 0.2))");
  std::vector<const Geometry*> snap_geoms{snap_p0.get(), snap_p1.get()};
  FixedPrecisionUnionStrategy snap_strategy(1.0);
  UnaryUnionOp snap_op(snap_geoms, *factory_);
  snap_op.setUnionFunction(&snap_strategy);
  auto snap_res = snap_op.Union();
  ASSERT_NE(snap_res, nullptr);
  EXPECT_TRUE(snap_res->isValid());
  EXPECT_DOUBLE_EQ(snap_res->getArea(), 150.0);
}

TEST_F(UnaryUnionOpTest, CurvedComponentsThrowUnsupportedOperation) {
  auto curved = Read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(UnaryUnionOp::Union(*curved),
               geos::util::UnsupportedOperationException);

  auto poly = Read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  std::vector<const Geometry*> mixed{poly.get(), curved.get()};
  EXPECT_THROW(UnaryUnionOp::Union(mixed),
               geos::util::UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
