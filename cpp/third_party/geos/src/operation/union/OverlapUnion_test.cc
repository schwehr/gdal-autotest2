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

#include "third_party/geos/include/geos/operation/union/OverlapUnion.h"

#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/operation/union/CascadedPolygonUnion.h"
#include "third_party/geos/include/geos/operation/union/UnionStrategy.h"

namespace geos_google {

struct UnionBufferTag {
  using type = std::unique_ptr<geos::geom::Geometry> (*)(
      const geos::geom::Geometry*, const geos::geom::Geometry*);
};

template <typename Tag, typename Tag::type M>
struct OverlapUnionAccessor {
  friend typename Tag::type GetOverlapUnionFn(Tag) { return M; }
};

typename UnionBufferTag::type GetOverlapUnionFn(UnionBufferTag);
template struct OverlapUnionAccessor<
    UnionBufferTag, &geos::operation::geounion::OverlapUnion::unionBuffer>;

namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::ClassicUnionStrategy;
using geos::operation::geounion::OverlapUnion;
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
    ++call_count_;
    return OverlayNG::overlay(g0, g1, OverlayNG::UNION, &pm_);
  }

  bool isFloatingPrecision() const override { return false; }

  int call_count() const { return call_count_; }

 private:
  PrecisionModel pm_;
  int call_count_ = 0;
};

class OverlapUnionTest : public ::testing::Test {
 protected:
  OverlapUnionTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(OverlapUnionTest, DisjointEnvelopesCombineDirectly) {
  auto g0 = Read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto g1 = Read("POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10))");

  auto result = OverlapUnion(g0.get(), g1.get()).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(result->getArea(), 8.0);

  CountingClassicUnionStrategy strategy;
  auto result_with_strategy =
      OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result_with_strategy, nullptr);
  EXPECT_EQ(strategy.call_count(), 0);
  EXPECT_TRUE(result_with_strategy->equals(result.get()));
}

TEST_F(OverlapUnionTest, EmptyInputsCombineWithoutUnionCalls) {
  auto empty_poly_a = Read("POLYGON EMPTY");
  auto empty_poly_b = Read("POLYGON EMPTY");
  CountingClassicUnionStrategy strategy;

  auto both_empty =
      OverlapUnion(empty_poly_a.get(), empty_poly_b.get(), &strategy).doUnion();
  ASSERT_NE(both_empty, nullptr);
  EXPECT_TRUE(both_empty->isEmpty());
  EXPECT_EQ(strategy.call_count(), 0);

  auto square = Read("POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0))");
  auto empty_mp = Read("MULTIPOLYGON EMPTY");

  auto left_empty = OverlapUnion(empty_mp.get(), square.get()).doUnion();
  ASSERT_NE(left_empty, nullptr);
  EXPECT_DOUBLE_EQ(left_empty->getArea(), 9.0);
  EXPECT_TRUE(left_empty->equals(square.get()));

  auto right_empty = OverlapUnion(square.get(), empty_mp.get()).doUnion();
  ASSERT_NE(right_empty, nullptr);
  EXPECT_DOUBLE_EQ(right_empty->getArea(), 9.0);
  EXPECT_TRUE(right_empty->equals(square.get()));

  auto empty_gc_a = Read("GEOMETRYCOLLECTION EMPTY");
  auto empty_gc_b = Read("GEOMETRYCOLLECTION EMPTY");
  auto gc_res = OverlapUnion(empty_gc_a.get(), empty_gc_b.get()).doUnion();
  ASSERT_NE(gc_res, nullptr);
  EXPECT_TRUE(gc_res->isEmpty());
}

TEST_F(OverlapUnionTest, OverlappingPolygonsTriggerFallbackWhenBordersChange) {
  auto g0 = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto g1 = Read("POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");
  CountingClassicUnionStrategy strategy;

  // Border segment counts differ before vs after union (6 vs 0), forcing
  // fallback to unionFull(g0, g1).
  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(strategy.call_count(), 2);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(result->getArea(), 150.0);

  auto expected = Read("POLYGON ((0 0, 15 0, 15 10, 0 10, 0 0))");
  EXPECT_TRUE(result->equals(expected.get()));
}

TEST_F(OverlapUnionTest, FastPathWhenBorderSegmentsMatchAndNoDisjointPolys) {
  // Both polygons have CW exterior rings (matching OverlayNG output order) and
  // are spatially disjoint while their envelopes overlap on [3, 10] x [3, 10].
  // g0 also includes segment (5,6)->(4,4) strictly inside (3,10)x(3,10).
  auto g0 = Read("POLYGON ((0 0, 0 10, 3 8, 5 6, 4 4, 8 3, 10 0, 0 0))");
  auto g1 = Read("POLYGON ((10 10, 10 3, 7 7, 3 10, 10 10))");
  ASSERT_TRUE(g0->isValid());
  ASSERT_TRUE(g1->isValid());

  CountingClassicUnionStrategy strategy;
  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(strategy.call_count(), 1);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(result->getArea(), 74.0);

  auto default_result = OverlapUnion(g0.get(), g1.get()).doUnion();
  ASSERT_NE(default_result, nullptr);
  EXPECT_TRUE(default_result->equals(result.get()));
}

TEST_F(OverlapUnionTest, FastPathCombinesDisjointSubPolygonsWithMergedOverlap) {
  // Overlap envelope is [0, 12] x [0, 12].
  // All perimeter components are CW and disjoint from each other; the two
  // interior components [3,7]x[3,7] and [5,9]x[5,9] lie strictly inside
  // (0,12)x(0,12) and merge without altering border segments. The far-left and
  // far-right components sit completely outside the overlap envelope.
  auto g0 = Read(
      "MULTIPOLYGON ("
      "((0 11, 0 12, 2 12, 2 11, 0 11)),"
      "((0 4, 0 8, 1 8, 1 4, 0 4)),"
      "((11 0, 11 2, 12 2, 12 0, 11 0)),"
      "((3 3, 3 7, 7 7, 7 3, 3 3)),"
      "((-20 0, -20 12, -10 12, -10 0, -20 0))"
      ")");
  auto g1 = Read(
      "MULTIPOLYGON ("
      "((0 9, 0 10, 1 10, 1 9, 0 9)),"
      "((4 0, 4 1, 8 1, 8 0, 4 0)),"
      "((4 11, 4 12, 8 12, 8 11, 4 11)),"
      "((11 4, 11 8, 12 8, 12 4, 11 4)),"
      "((5 5, 5 9, 9 9, 9 5, 5 5)),"
      "((20 0, 20 12, 30 12, 30 0, 20 0))"
      ")");
  ASSERT_TRUE(g0->isValid());
  ASSERT_TRUE(g1->isValid());

  CountingClassicUnionStrategy strategy;
  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(strategy.call_count(), 1);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 10u);
  EXPECT_DOUBLE_EQ(result->getArea(), 289.0);

  auto full_union = g0->Union(g1.get());
  EXPECT_TRUE(result->equals(full_union.get()));
}

TEST_F(OverlapUnionTest, CrossShapedEnvelopesYieldEmptyOverlapExtraction) {
  // Envelopes intersect on [4, 6] x [4, 6], yet none of the four component
  // polygons intersect [4, 6] x [4, 6].
  auto g0 = Read(
      "MULTIPOLYGON ("
      "((0 4, 0 6, 1 6, 1 4, 0 4)),"
      "((9 4, 9 6, 10 6, 10 4, 9 4))"
      ")");
  auto g1 = Read(
      "MULTIPOLYGON ("
      "((4 0, 4 1, 6 1, 6 0, 4 0)),"
      "((4 9, 4 10, 6 10, 6 9, 4 9))"
      ")");
  CountingClassicUnionStrategy strategy;

  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(strategy.call_count(), 0);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 4u);
  EXPECT_DOUBLE_EQ(result->getArea(), 8.0);
}

TEST_F(OverlapUnionTest,
       OneSidedEmptyOverlapExtractionInvokesUnionOnceWithoutFallback) {
  // Overlap envelope is [4, 6] x [4, 6]. Both components of |spread| lie
  // outside [4, 6] x [4, 6], while |center| lies completely inside it.
  // unionFull() only clones when *both* overlap extracts have 0 geometries, so
  // one-sided empty extraction invokes unionFunction->Union() once and still
  // combines with the disjoint components without falling back to full union.
  auto spread = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 10, 2 10, 2 0, 0 0)),"
      "((8 0, 8 10, 10 10, 10 0, 8 0))"
      ")");
  auto center = Read("POLYGON ((4 4, 4 6, 6 6, 6 4, 4 4))");

  {
    CountingClassicUnionStrategy strategy;
    auto result = OverlapUnion(spread.get(), center.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(strategy.call_count(), 1);
    EXPECT_EQ(result->getNumGeometries(), 3u);
    EXPECT_DOUBLE_EQ(result->getArea(), 44.0);
  }

  {
    CountingClassicUnionStrategy strategy;
    auto result = OverlapUnion(center.get(), spread.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(strategy.call_count(), 1);
    EXPECT_EQ(result->getNumGeometries(), 3u);
    EXPECT_DOUBLE_EQ(result->getArea(), 44.0);
  }
}

TEST_F(OverlapUnionTest, BorderSegmentEqualityChecksAllCoordinateDiffs) {
  // 1. p0.x differs: CCW g0 causes OverlayNG output segments to be reversed.
  {
    auto g0 = Read("POLYGON ((0 0, 10 0, 8 3, 4 4, 5 6, 3 8, 0 10, 0 0))");
    auto g1 = Read("POLYGON ((10 10, 10 3, 7 7, 3 10, 10 10))");
    CountingClassicUnionStrategy strategy;
    auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(strategy.call_count(), 2);
    EXPECT_DOUBLE_EQ(result->getArea(), 74.0);
  }

  // 2. p0.x matches, p0.y differs: fixed-precision snapping shifts the
  // minimum-p0 border segment (3, 5.2)->(5, 3) to (3, 5)->(5, 3) on
  // overlapEnv [3, 10] x [3, 10], leaving p0.x equal while p0.y differs.
  {
    auto g0 =
        Read("POLYGON ((0 0, 0 10, 2 10, 2 2, 4 2, 3 5.2, 5 3, 10 0, 0 0))");
    auto g1 = Read("POLYGON ((10 10, 10 3, 7 7, 3 10, 10 10))");
    ASSERT_TRUE(g0->isValid());
    ASSERT_TRUE(g1->isValid());
    FixedPrecisionUnionStrategy snap_strategy(1.0);
    auto result = OverlapUnion(g0.get(), g1.get(), &snap_strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(snap_strategy.call_count(), 2);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), 57.0);
  }

  // 3. p0 matches, p1.x differs due to fixed-precision snapping.
  {
    auto g0 = Read("POLYGON ((0 0, 0 10, 4.2 5, 10 0, 0 0))");
    auto g1 = Read("POLYGON ((10 10, 10 3, 7 7, 3 10, 10 10))");
    FixedPrecisionUnionStrategy snap_strategy(1.0);
    auto result = OverlapUnion(g0.get(), g1.get(), &snap_strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(snap_strategy.call_count(), 2);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), 66.0);
  }

  // 4. p0 and p1.x match, p1.y differs due to fixed-precision snapping.
  {
    auto g0 = Read("POLYGON ((0 0, 0 10, 4 5.2, 10 0, 0 0))");
    auto g1 = Read("POLYGON ((10 10, 10 3, 7 7, 3 10, 10 10))");
    FixedPrecisionUnionStrategy snap_strategy(1.0);
    auto result = OverlapUnion(g0.get(), g1.get(), &snap_strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(snap_strategy.call_count(), 2);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), 66.0);
  }
}

TEST_F(OverlapUnionTest, SupportsMultiLineStringsAndMultiPoints) {
  auto ml0 = Read("MULTILINESTRING ((0 5, 10 5), (50 50, 52 50))");
  auto ml1 = Read("MULTILINESTRING ((5 0, 5 10), (3 3, 4 3))");

  auto ml_result = OverlapUnion(ml0.get(), ml1.get()).doUnion();
  ASSERT_NE(ml_result, nullptr);
  EXPECT_DOUBLE_EQ(ml_result->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(ml_result->getLength(), 23.0);

  auto mp0 = Read("MULTIPOINT ((5 5), (20 20))");
  auto mp1 = Read("MULTIPOINT ((5 5), (0 0))");
  auto expected_mp = Read("MULTIPOINT ((0 0), (5 5), (20 20))");

  auto mp_result = OverlapUnion(mp0.get(), mp1.get()).doUnion();
  ASSERT_NE(mp_result, nullptr);
  EXPECT_EQ(mp_result->getNumGeometries(), 3u);
  EXPECT_TRUE(mp_result->equals(expected_mp.get()));
}

TEST_F(OverlapUnionTest, SupportsPolygonsWithInteriorHoles) {
  // Exterior shell CW, interior hole CCW, plus a disjoint CW polygon.
  auto g0 = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 20, 20 20, 20 0, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2)),"
      "((30 0, 30 10, 40 10, 40 0, 30 0))"
      ")");
  auto g1 = Read("POLYGON ((15 5, 15 25, 25 25, 25 5, 15 5))");

  auto result = OverlapUnion(g0.get(), g1.get()).doUnion();
  ASSERT_NE(result, nullptr);
  // Area: (400 - 36) + 100 + 200 - (5 * 15) = 589.
  EXPECT_DOUBLE_EQ(result->getArea(), 589.0);
  EXPECT_TRUE(result->contains(Read("POINT (18 10)").get()));
  EXPECT_FALSE(result->contains(Read("POINT (5 5)").get()));
}

TEST_F(OverlapUnionTest, PolygonWithHoleAndIslandTakesFastPath) {
  // Exterior shell CW, hole [10,18]x[10,18] CCW, disjoint polygon CW.
  // |g1| is a CW island strictly inside the hole, so overlapEnv=[12,16]x[12,16]
  // preserves all border segments and avoids full-union fallback.
  auto g0 = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 20, 20 20, 20 0, 0 0), (10 10, 18 10, 18 18, 10 18, 10 10)),"
      "((30 0, 30 10, 40 10, 40 0, 30 0))"
      ")");
  auto g1 = Read("POLYGON ((12 12, 12 16, 16 16, 16 12, 12 12))");
  ASSERT_TRUE(g0->isValid());
  ASSERT_TRUE(g1->isValid());

  CountingClassicUnionStrategy strategy;
  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(strategy.call_count(), 1);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 3u);
  // Area: (400 - 64) + 100 + 16 = 452.
  EXPECT_DOUBLE_EQ(result->getArea(), 452.0);
  EXPECT_TRUE(result->contains(Read("POINT (14 14)").get()));
  EXPECT_FALSE(result->contains(Read("POINT (11 11)").get()));
  EXPECT_TRUE(result->contains(Read("POINT (35 5)").get()));
}

TEST_F(OverlapUnionTest, TouchingEnvelopesDegenerateOverlap) {
  // Shared vertical edge: degenerate line overlapEnv [5, 5] x [0, 5] changes
  // border segments from 6 to 0 and triggers fallback merge.
  {
    auto left = Read("POLYGON ((0 0, 0 5, 5 5, 5 0, 0 0))");
    auto right = Read("POLYGON ((5 0, 5 5, 10 5, 10 0, 5 0))");
    CountingClassicUnionStrategy strategy;
    auto result = OverlapUnion(left.get(), right.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(strategy.call_count(), 2);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(result->getArea(), 50.0);
  }

  // Shared corner point: degenerate point overlapEnv [5, 5] x [5, 5] preserves
  // CW border segments and takes the fast path.
  {
    auto bottom_left = Read("POLYGON ((0 0, 0 5, 5 5, 5 0, 0 0))");
    auto top_right = Read("POLYGON ((5 5, 5 10, 10 10, 10 5, 5 5))");
    CountingClassicUnionStrategy strategy;
    auto result =
        OverlapUnion(bottom_left.get(), top_right.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_EQ(strategy.call_count(), 1);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(result->getNumGeometries(), 2u);
    EXPECT_DOUBLE_EQ(result->getArea(), 50.0);
  }
}

TEST_F(OverlapUnionTest, FallbackIncludesDisjointComponentsFromBothInputs) {
  auto g0 = Read(
      "MULTIPOLYGON ("
      "((0 0, 0 10, 10 10, 10 0, 0 0)),"
      "((-20 0, -20 5, -15 5, -15 0, -20 0))"
      ")");
  auto g1 = Read(
      "MULTIPOLYGON ("
      "((5 0, 5 10, 15 10, 15 0, 5 0)),"
      "((25 0, 25 5, 30 5, 30 0, 25 0))"
      ")");
  ASSERT_TRUE(g0->isValid());
  ASSERT_TRUE(g1->isValid());

  CountingClassicUnionStrategy strategy;
  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(strategy.call_count(), 2);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(result->getNumGeometries(), 3u);
  // Area: merged [0,15]x[0,10] (150) + [-20,-15]x[0,5] (25) + [25,30]x[0,5]
  // (25) = 200.
  EXPECT_DOUBLE_EQ(result->getArea(), 200.0);

  auto full_union = g0->Union(g1.get());
  EXPECT_TRUE(result->equals(full_union.get()));
}

TEST_F(OverlapUnionTest,
       HeterogeneousGeometryCollectionWithDisjointPointTakesFastPath) {
  auto g0 = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 0 10, 3 8, 5 6, 4 4, 8 3, 10 0, 0 0)),"
      "POINT (50 50)"
      ")");
  auto g1 = Read("POLYGON ((10 10, 10 3, 7 7, 3 10, 10 10))");

  CountingClassicUnionStrategy strategy;
  auto result = OverlapUnion(g0.get(), g1.get(), &strategy).doUnion();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(strategy.call_count(), 1);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(result->getArea(), 74.0);

  bool found_point = false;
  std::size_t polygon_count = 0;
  for (std::size_t i = 0; i < result->getNumGeometries(); ++i) {
    const Geometry* elem = result->getGeometryN(i);
    if (elem->getGeometryTypeId() == geos::geom::GEOS_POINT) {
      found_point = true;
      ASSERT_NE(elem->getCoordinate(), nullptr);
      EXPECT_DOUBLE_EQ(elem->getCoordinate()->x, 50.0);
      EXPECT_DOUBLE_EQ(elem->getCoordinate()->y, 50.0);
    } else if (elem->getGeometryTypeId() == geos::geom::GEOS_POLYGON) {
      ++polygon_count;
    }
  }
  EXPECT_TRUE(found_point);
  EXPECT_EQ(polygon_count, 2u);
}

TEST_F(OverlapUnionTest, CoincidentPointsAndCrossingLineStrings) {
  {
    auto p0 = Read("POINT (5 5)");
    auto p1 = Read("POINT (5 5)");
    CountingClassicUnionStrategy strategy;
    auto result = OverlapUnion(p0.get(), p1.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(strategy.call_count(), 1);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_TRUE(result->equals(p0.get()));
  }

  {
    auto l0 = Read("LINESTRING (0 5, 10 5)");
    auto l1 = Read("LINESTRING (5 0, 5 10)");
    CountingClassicUnionStrategy strategy;
    auto result = OverlapUnion(l0.get(), l1.get(), &strategy).doUnion();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(strategy.call_count(), 2);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
    EXPECT_EQ(result->getNumGeometries(), 4u);
    EXPECT_DOUBLE_EQ(result->getLength(), 20.0);
  }
}

TEST_F(OverlapUnionTest, UnionBufferHelperBuildsCollectionAndBuffersZero) {
  auto p0 = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto p1 = Read("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  auto union_buffer = GetOverlapUnionFn(UnionBufferTag{});
  auto result = union_buffer(p0.get(), p1.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(result->getArea(), 175.0);
}

}  // namespace
}  // namespace geos_google
