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

#include "third_party/geos/include/geos/operation/union/CascadedPolygonUnion.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/union/UnionStrategy.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::CascadedPolygonUnion;
using geos::operation::geounion::ClassicUnionStrategy;
using geos::operation::geounion::UnionStrategy;
using geos::util::TopologyException;

class FixedResultUnionStrategy : public UnionStrategy {
 public:
  explicit FixedResultUnionStrategy(const Geometry* result_template)
      : result_template_(result_template) {}

  std::unique_ptr<Geometry> Union(const Geometry* /*g0*/,
                                  const Geometry* /*g1*/) override {
    ++call_count_;
    return result_template_->clone();
  }

  bool isFloatingPrecision() const override { return true; }

  int call_count() const { return call_count_; }

 private:
  const Geometry* result_template_;
  int call_count_ = 0;
};

class CascadedPolygonUnionTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Polygon> ReadPolygon(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  std::unique_ptr<MultiPolygon> ReadMultiPolygon(const std::string& wkt) const {
    return reader_.read<MultiPolygon>(wkt);
  }

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }
};

TEST_F(CascadedPolygonUnionTest, EmptyVectorReturnsNull) {
  std::vector<Polygon*> polys;
  EXPECT_EQ(CascadedPolygonUnion::Union(&polys), nullptr);
}

TEST_F(CascadedPolygonUnionTest, OnlyEmptyPolygonsReturnsNull) {
  auto e1 = ReadPolygon("POLYGON EMPTY");
  auto e2 = ReadPolygon("POLYGON EMPTY");

  std::vector<Polygon*> one_empty{e1.get()};
  EXPECT_EQ(CascadedPolygonUnion::Union(&one_empty), nullptr);

  std::vector<Polygon*> two_empty{e1.get(), e2.get()};
  EXPECT_EQ(CascadedPolygonUnion::Union(&two_empty), nullptr);
}

TEST_F(CascadedPolygonUnionTest, MixedEmptyAndNonEmptyPolygons) {
  auto e1 = ReadPolygon("POLYGON EMPTY");
  auto p1 = ReadPolygon("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto e2 = ReadPolygon("POLYGON EMPTY");
  auto p2 = ReadPolygon("POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");
  auto expected = ReadPolygon("POLYGON ((0 0, 15 0, 15 10, 0 10, 0 0))");

  std::vector<Polygon*> polys{e1.get(), p1.get(), e2.get(), p2.get()};
  auto result = CascadedPolygonUnion::Union(&polys);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_TRUE(result->equals(expected.get()));
  EXPECT_DOUBLE_EQ(result->getArea(), 150.0);
}

TEST_F(CascadedPolygonUnionTest, SinglePolygonWithHoleReturnsEqualClone) {
  auto poly = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  std::vector<Polygon*> polys{poly.get()};

  auto result = CascadedPolygonUnion::Union(&polys);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(result.get(), poly.get());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(result->equals(poly.get()));
  EXPECT_DOUBLE_EQ(result->getArea(), 64.0);
}

TEST_F(CascadedPolygonUnionTest, TwoOverlappingAndDisjointPolygons) {
  auto p1 = ReadPolygon("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto p2 = ReadPolygon("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  auto p3 = ReadPolygon("POLYGON ((30 0, 40 0, 40 10, 30 10, 30 0))");

  std::vector<Polygon*> overlapping{p1.get(), p2.get()};
  auto overlap_union = CascadedPolygonUnion::Union(&overlapping);
  ASSERT_NE(overlap_union, nullptr);
  EXPECT_EQ(overlap_union->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(overlap_union->getArea(), 175.0);

  std::vector<Polygon*> disjoint{p1.get(), p3.get()};
  auto disjoint_union = CascadedPolygonUnion::Union(&disjoint);
  ASSERT_NE(disjoint_union, nullptr);
  EXPECT_EQ(disjoint_union->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(disjoint_union->getNumGeometries(), 2);
  EXPECT_DOUBLE_EQ(disjoint_union->getArea(), 200.0);
}

TEST_F(CascadedPolygonUnionTest, ThreePolygonsExercisesBinaryRecursion) {
  auto p1 = ReadPolygon("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto p2 = ReadPolygon("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");
  auto p3 = ReadPolygon("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  auto expected = ReadPolygon(
      "POLYGON ((0 0, 20 0, 20 10, 15 10, 15 15, 5 15, 5 10, 0 10, 0 0))");

  std::vector<Polygon*> polys{p1.get(), p2.get(), p3.get()};
  auto result = CascadedPolygonUnion::Union(&polys);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(result->getArea(), 250.0);
  EXPECT_TRUE(result->equals(expected.get()));
}

TEST_F(CascadedPolygonUnionTest, FourPolygonsEnclosingHole) {
  auto bottom = ReadPolygon("POLYGON ((0 0, 10 0, 10 2, 0 2, 0 0))");
  auto top = ReadPolygon("POLYGON ((0 8, 10 8, 10 10, 0 10, 0 8))");
  auto left = ReadPolygon("POLYGON ((0 2, 2 2, 2 8, 0 8, 0 2))");
  auto right = ReadPolygon("POLYGON ((8 2, 10 2, 10 8, 8 8, 8 2))");
  auto expected = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");

  std::vector<Polygon*> polys{bottom.get(), top.get(), left.get(), right.get()};
  auto result = CascadedPolygonUnion::Union(&polys);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  const auto* result_poly = dynamic_cast<const Polygon*>(result.get());
  ASSERT_NE(result_poly, nullptr);
  EXPECT_EQ(result_poly->getNumInteriorRing(), 1);
  EXPECT_DOUBLE_EQ(result_poly->getArea(), 64.0);
  EXPECT_TRUE(result_poly->equals(expected.get()));
}

TEST_F(CascadedPolygonUnionTest, LargeOverlappingGridAndDisjointIslands) {
  std::vector<std::unique_ptr<Polygon>> owned;
  std::vector<Polygon*> polys;

  // 4x4 grid of 2x2 squares with 1-unit overlap in x and y -> covers [0, 5]x[0,
  // 5].
  for (int ix = 0; ix < 4; ++ix) {
    for (int iy = 0; iy < 4; ++iy) {
      std::ostringstream wkt;
      wkt << "POLYGON ((" << ix << " " << iy << ", " << (ix + 2) << " " << iy
          << ", " << (ix + 2) << " " << (iy + 2) << ", " << ix << " "
          << (iy + 2) << ", " << ix << " " << iy << "))";
      owned.push_back(ReadPolygon(wkt.str()));
      polys.push_back(owned.back().get());
    }
  }
  owned.push_back(ReadPolygon("POLYGON ((20 20, 23 20, 23 23, 20 23, 20 20))"));
  polys.push_back(owned.back().get());
  owned.push_back(ReadPolygon("POLYGON ((30 30, 32 30, 32 32, 30 32, 30 30))"));
  polys.push_back(owned.back().get());

  auto expected = ReadMultiPolygon(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((20 20, 23 20, 23 23, 20 23, 20 20)), "
      "((30 30, 32 30, 32 32, 30 32, 30 30)))");

  auto result = CascadedPolygonUnion::Union(&polys);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 3);
  EXPECT_DOUBLE_EQ(result->getArea(), 25.0 + 9.0 + 4.0);
  EXPECT_TRUE(result->equals(expected.get()));
}

TEST_F(CascadedPolygonUnionTest, MultiPolygonOverload) {
  auto empty_mp = ReadMultiPolygon("MULTIPOLYGON EMPTY");
  EXPECT_EQ(CascadedPolygonUnion::Union(empty_mp.get()), nullptr);

  std::vector<std::unique_ptr<Polygon>> empty_children;
  empty_children.push_back(ReadPolygon("POLYGON EMPTY"));
  auto mp_of_empty = factory_->createMultiPolygon(std::move(empty_children));
  EXPECT_EQ(CascadedPolygonUnion::Union(mp_of_empty.get()), nullptr);

  auto single_mp =
      ReadMultiPolygon("MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)))");
  auto single_res = CascadedPolygonUnion::Union(single_mp.get());
  ASSERT_NE(single_res, nullptr);
  EXPECT_EQ(single_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(single_res->getArea(), 16.0);

  auto touching_mp = ReadMultiPolygon(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((4 0, 8 0, 8 4, 4 4, 4 0)), "
      "((12 0, 14 0, 14 2, 12 2, 12 0)))");
  auto expected = ReadMultiPolygon(
      "MULTIPOLYGON (((0 0, 8 0, 8 4, 0 4, 0 0)), "
      "((12 0, 14 0, 14 2, 12 2, 12 0)))");
  auto touching_res = CascadedPolygonUnion::Union(touching_mp.get());
  ASSERT_NE(touching_res, nullptr);
  EXPECT_TRUE(touching_res->isValid());
  EXPECT_EQ(touching_res->getNumGeometries(), 2);
  EXPECT_DOUBLE_EQ(touching_res->getArea(), 36.0);
  EXPECT_TRUE(touching_res->equals(expected.get()));
}

TEST_F(CascadedPolygonUnionTest, InstanceAndIteratorOverloads) {
  auto p1 = ReadPolygon("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto p2 = ReadPolygon("POLYGON ((2 0, 6 0, 6 4, 2 4, 2 0))");
  auto p3 = ReadPolygon("POLYGON ((4 0, 8 0, 8 4, 4 4, 4 0))");
  auto expected = ReadPolygon("POLYGON ((0 0, 8 0, 8 4, 0 4, 0 0))");

  std::vector<Polygon*> polys{p1.get(), p2.get(), p3.get()};
  CascadedPolygonUnion op(&polys);
  auto instance_res = op.Union();
  ASSERT_NE(instance_res, nullptr);
  EXPECT_TRUE(instance_res->equals(expected.get()));

  ClassicUnionStrategy strategy;
  CascadedPolygonUnion op_with_strategy(&polys, &strategy);
  auto strat_res = op_with_strategy.Union();
  ASSERT_NE(strat_res, nullptr);
  EXPECT_TRUE(strat_res->equals(expected.get()));

  std::vector<const Geometry*> geom_ptrs{p1.get(), p2.get(), p3.get()};
  auto iter_res = CascadedPolygonUnion::Union(geom_ptrs.begin(),
                                              geom_ptrs.end(), &strategy);
  ASSERT_NE(iter_res, nullptr);
  EXPECT_TRUE(iter_res->equals(expected.get()));
}

TEST_F(CascadedPolygonUnionTest, RestrictToPolygonsFiltersGeometryCollection) {
  auto p1 = ReadPolygon("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto p2 = ReadPolygon("POLYGON ((10 0, 12 0, 12 2, 10 2, 10 0))");
  auto p3 = ReadPolygon("POLYGON ((20 0, 22 0, 22 2, 20 2, 20 0))");

  auto mixed_gc = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0)), "
      "POINT (5 5), "
      "LINESTRING (6 6, 8 8), "
      "POLYGON ((10 0, 13 0, 13 3, 10 3, 10 0)))");
  auto expected_mp = ReadMultiPolygon(
      "MULTIPOLYGON (((0 0, 3 0, 3 3, 0 3, 0 0)), "
      "((10 0, 13 0, 13 3, 10 3, 10 0)))");

  // 2 polygons exercises unionActual(const Geometry*, const Geometry*).
  {
    FixedResultUnionStrategy strategy(mixed_gc.get());
    std::vector<Polygon*> polys{p1.get(), p2.get()};
    auto result = CascadedPolygonUnion::Union(&polys, &strategy);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(strategy.call_count(), 1);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(result->getNumGeometries(), 2);
    EXPECT_TRUE(result->equals(expected_mp.get()));
  }

  // 3 polygons exercises unionActual(unique_ptr&&, unique_ptr&&).
  {
    FixedResultUnionStrategy strategy(mixed_gc.get());
    std::vector<Polygon*> polys{p1.get(), p2.get(), p3.get()};
    auto result = CascadedPolygonUnion::Union(&polys, &strategy);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(strategy.call_count(), 2);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(result->getNumGeometries(), 2);
    EXPECT_TRUE(result->equals(expected_mp.get()));
  }

  // Collection with zero polygonal components becomes an empty MultiPolygon.
  {
    auto non_poly_gc =
        Read("GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 4 4))");
    FixedResultUnionStrategy strategy(non_poly_gc.get());
    std::vector<Polygon*> polys{p1.get(), p2.get()};
    auto result = CascadedPolygonUnion::Union(&polys, &strategy);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_TRUE(result->isEmpty());
  }
}

TEST_F(CascadedPolygonUnionTest,
       RestrictToPolygonsHandlesNonCollectionCollapse) {
  auto p1 = ReadPolygon("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto p2 = ReadPolygon("POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0))");

  {
    auto line_2d = Read("LINESTRING (0 0, 5 5)");
    FixedResultUnionStrategy strategy(line_2d.get());
    std::vector<Polygon*> polys{p1.get(), p2.get()};
    auto result = CascadedPolygonUnion::Union(&polys, &strategy);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_EQ(result->getCoordinateDimension(), 2);
  }

  {
    auto line_3d = Read("LINESTRING Z (0 0 1, 5 5 2)");
    FixedResultUnionStrategy strategy(line_3d.get());
    std::vector<Polygon*> polys{p1.get(), p2.get()};
    auto result = CascadedPolygonUnion::Union(&polys, &strategy);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_EQ(result->getCoordinateDimension(), 3);
  }
}

TEST_F(CascadedPolygonUnionTest, ClassicUnionStrategyBasicAndMoveOverload) {
  ClassicUnionStrategy strategy;
  EXPECT_TRUE(strategy.isFloatingPrecision());

  auto p1 = ReadPolygon("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto p2 = ReadPolygon("POLYGON ((2 0, 6 0, 6 4, 2 4, 2 0))");
  auto expected = ReadPolygon("POLYGON ((0 0, 6 0, 6 4, 0 4, 0 0))");

  auto raw_res = strategy.Union(p1.get(), p2.get());
  ASSERT_NE(raw_res, nullptr);
  EXPECT_TRUE(raw_res->equals(expected.get()));

  UnionStrategy& base_strategy = strategy;
  auto move_res = base_strategy.Union(p1->clone(), p2->clone());
  ASSERT_NE(move_res, nullptr);
  EXPECT_TRUE(move_res->equals(expected.get()));
}

TEST_F(CascadedPolygonUnionTest,
       ClassicUnionStrategyTopologyExceptionFallbackAndRethrow) {
  // With a fixed PrecisionModel, OverlayNGRobust runs single-pass fixed-grid
  // OverlayNG without snap/SR retries. A polygon whose hole crosses its shell
  // triggers a side-location TopologyException in OverlayLabeller.
  PrecisionModel fixed_pm(1.0);
  auto fixed_factory = GeometryFactory::create(&fixed_pm);
  WKTReader fixed_reader(*fixed_factory);

  auto invalid_poly = fixed_reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 2, 15 2, 15 8, 5 8, 5 2))");
  auto valid_poly =
      fixed_reader.read<Polygon>("POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))");
  auto line = fixed_reader.read("LINESTRING (20 5, 30 5)");

  ClassicUnionStrategy strategy;

  // When either operand is non-polygonal (dim != 2), TopologyException
  // rethrows.
  EXPECT_THROW(strategy.Union(invalid_poly.get(), line.get()),
               TopologyException);
  EXPECT_THROW(strategy.Union(line.get(), invalid_poly.get()),
               TopologyException);

  // When both operands have dimension 2, ClassicUnionStrategy falls back to
  // unionPolygonsByBuffer (buffer(0)), recovering a valid polygonal union.
  auto fallback_res = strategy.Union(invalid_poly.get(), valid_poly.get());
  ASSERT_NE(fallback_res, nullptr);
  EXPECT_TRUE(fallback_res->isPolygonal());
  EXPECT_TRUE(fallback_res->isValid());
  // (10x10 minus 5x6 inside shell) + 10x10 = 70 + 100 = 170.
  EXPECT_DOUBLE_EQ(fallback_res->getArea(), 170.0);

  std::vector<Polygon*> polys{invalid_poly.get(), valid_poly.get()};
  auto cascaded_res = CascadedPolygonUnion::Union(&polys);
  ASSERT_NE(cascaded_res, nullptr);
  EXPECT_TRUE(cascaded_res->isPolygonal());
  EXPECT_TRUE(cascaded_res->isValid());
  EXPECT_DOUBLE_EQ(cascaded_res->getArea(), 170.0);
}

}  // namespace
}  // namespace geos_google
