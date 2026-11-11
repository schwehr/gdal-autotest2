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

// Unit tests for third_party/geos/src/operation/union/UnionStrategy.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/union/UnionStrategy.cpp

#include "third_party/geos/include/geos/operation/union/UnionStrategy.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNGRobust.h"
#include "third_party/geos/include/geos/operation/union/CascadedPolygonUnion.h"
#include "third_party/geos/include/geos/operation/union/UnaryUnionOp.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::geounion::CascadedPolygonUnion;
using geos::operation::geounion::ClassicUnionStrategy;
using geos::operation::geounion::UnaryUnionOp;
using geos::operation::geounion::UnionStrategy;
using geos::operation::overlayng::OverlayNGRobust;
using geos::util::IllegalArgumentException;

// Test strategy that leaves the rvalue Union overload at the base class
// default implementation so UnionStrategy::Union(unique_ptr&&, unique_ptr&&)
// delegates to the raw-pointer virtual method.
class RecordingUnionStrategy : public UnionStrategy {
 public:
  explicit RecordingUnionStrategy(bool floating_precision = true,
                                  bool throw_on_union = false)
      : floating_precision_(floating_precision),
        throw_on_union_(throw_on_union) {}

  std::unique_ptr<Geometry> Union(const Geometry* g0,
                                  const Geometry* g1) override {
    recorded_calls_.emplace_back(g0, g1);
    if (throw_on_union_) {
      throw IllegalArgumentException("Simulated union failure");
    }
    if (g0 == nullptr && g1 == nullptr) {
      return nullptr;
    }
    if (g0 == nullptr) {
      return g1->clone();
    }
    if (g1 == nullptr) {
      return g0->clone();
    }
    return g0->Union(g1);
  }

  bool isFloatingPrecision() const override { return floating_precision_; }

  const std::vector<std::pair<const Geometry*, const Geometry*>>&
  recorded_calls() const {
    return recorded_calls_;
  }

 private:
  bool floating_precision_;
  bool throw_on_union_;
  std::vector<std::pair<const Geometry*, const Geometry*>> recorded_calls_;
};

// Test strategy that overrides both Union overloads to verify virtual dispatch
// when a subclass provides a custom move-taking implementation.
class MoveConsumingUnionStrategy : public UnionStrategy {
 public:
  std::unique_ptr<Geometry> Union(const Geometry* g0,
                                  const Geometry* g1) override {
    ++raw_calls_;
    return g0->Union(g1);
  }

  std::unique_ptr<Geometry> Union(std::unique_ptr<Geometry>&& g0,
                                  std::unique_ptr<Geometry>&& g1) override {
    ++move_calls_;
    auto consumed0 = std::move(g0);
    auto consumed1 = std::move(g1);
    return consumed0->Union(consumed1.get());
  }

  bool isFloatingPrecision() const override { return false; }

  std::size_t raw_calls() const { return raw_calls_; }
  std::size_t move_calls() const { return move_calls_; }

 private:
  std::size_t raw_calls_ = 0;
  std::size_t move_calls_ = 0;
};

std::unique_ptr<Geometry> ReadGeom(const GeometryFactory& factory,
                                   std::string_view wkt) {
  WKTReader reader(factory);
  return reader.read(std::string(wkt));
}

TEST(UnionStrategyTest, DefaultRvalueOverloadForwardsRawPointers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Geometry> g0 =
      ReadGeom(*factory, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  std::unique_ptr<Geometry> g1 =
      ReadGeom(*factory, "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");

  const Geometry* expected_ptr0 = g0.get();
  const Geometry* expected_ptr1 = g1.get();

  RecordingUnionStrategy impl(/*floating_precision=*/false);
  UnionStrategy& strategy = impl;

  EXPECT_FALSE(strategy.isFloatingPrecision());

  std::unique_ptr<Geometry> result =
      strategy.Union(std::move(g0), std::move(g1));
  ASSERT_NE(result, nullptr);
  EXPECT_DOUBLE_EQ(result->getArea(), 7.0);

  // Base UnionStrategy::Union(unique_ptr&&, unique_ptr&&) borrows .get()
  // without resetting the caller's unique_ptr instances before returning.
  ASSERT_EQ(impl.recorded_calls().size(), 1);
  EXPECT_EQ(impl.recorded_calls()[0].first, expected_ptr0);
  EXPECT_EQ(impl.recorded_calls()[0].second, expected_ptr1);
}

TEST(UnionStrategyTest, DefaultRvalueOverloadHandlesNullInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RecordingUnionStrategy impl;
  UnionStrategy& strategy = impl;

  std::unique_ptr<Geometry> empty_left;
  std::unique_ptr<Geometry> right =
      ReadGeom(*factory, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  const Geometry* right_ptr = right.get();

  std::unique_ptr<Geometry> res_left_null =
      strategy.Union(std::move(empty_left), std::move(right));
  ASSERT_NE(res_left_null, nullptr);
  EXPECT_DOUBLE_EQ(res_left_null->getArea(), 4.0);

  std::unique_ptr<Geometry> left =
      ReadGeom(*factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  std::unique_ptr<Geometry> empty_right;
  const Geometry* left_ptr = left.get();

  std::unique_ptr<Geometry> res_right_null =
      strategy.Union(std::move(left), std::move(empty_right));
  ASSERT_NE(res_right_null, nullptr);
  EXPECT_DOUBLE_EQ(res_right_null->getArea(), 1.0);

  std::unique_ptr<Geometry> null0;
  std::unique_ptr<Geometry> null1;
  std::unique_ptr<Geometry> res_both_null =
      strategy.Union(std::move(null0), std::move(null1));
  EXPECT_EQ(res_both_null, nullptr);

  ASSERT_EQ(impl.recorded_calls().size(), 3);
  EXPECT_EQ(impl.recorded_calls()[0].first, nullptr);
  EXPECT_EQ(impl.recorded_calls()[0].second, right_ptr);
  EXPECT_EQ(impl.recorded_calls()[1].first, left_ptr);
  EXPECT_EQ(impl.recorded_calls()[1].second, nullptr);
  EXPECT_EQ(impl.recorded_calls()[2].first, nullptr);
  EXPECT_EQ(impl.recorded_calls()[2].second, nullptr);
}

TEST(UnionStrategyTest, DefaultRvalueOverloadPropagatesExceptions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Geometry> g0 = ReadGeom(*factory, "POINT (0 0)");
  std::unique_ptr<Geometry> g1 = ReadGeom(*factory, "POINT (1 1)");

  RecordingUnionStrategy impl(/*floating_precision=*/true,
                              /*throw_on_union=*/true);
  UnionStrategy& strategy = impl;

  EXPECT_THROW(strategy.Union(std::move(g0), std::move(g1)),
               IllegalArgumentException);
  EXPECT_EQ(impl.recorded_calls().size(), 1);
}

TEST(UnionStrategyTest, OverriddenRvalueOverloadDispatchesPolymorphically) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Geometry> g0 =
      ReadGeom(*factory, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  std::unique_ptr<Geometry> g1 =
      ReadGeom(*factory, "POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0))");

  MoveConsumingUnionStrategy impl;
  UnionStrategy& strategy = impl;

  EXPECT_FALSE(strategy.isFloatingPrecision());

  std::unique_ptr<Geometry> result =
      strategy.Union(std::move(g0), std::move(g1));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(g0, nullptr);
  EXPECT_EQ(g1, nullptr);
  EXPECT_DOUBLE_EQ(result->getArea(), 8.0);
  EXPECT_EQ(impl.move_calls(), 1);
  EXPECT_EQ(impl.raw_calls(), 0);
}

TEST(UnionStrategyTest, PolymorphicBuiltinStrategiesViaUniquePtr) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<UnionStrategy>> strategies;
  strategies.push_back(std::make_unique<ClassicUnionStrategy>());
  strategies.push_back(std::make_unique<OverlayNGRobust::SRUnionStrategy>());

  for (const auto& strategy : strategies) {
    EXPECT_TRUE(strategy->isFloatingPrecision());

    std::unique_ptr<Geometry> p0 =
        ReadGeom(*factory, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
    std::unique_ptr<Geometry> p1 =
        ReadGeom(*factory, "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");
    std::unique_ptr<Geometry> expected =
        ReadGeom(*factory, "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))");

    std::unique_ptr<Geometry> raw_union = strategy->Union(p0.get(), p1.get());
    std::unique_ptr<Geometry> move_union =
        strategy->Union(std::move(p0), std::move(p1));
    ASSERT_NE(raw_union, nullptr);
    ASSERT_NE(move_union, nullptr);
    EXPECT_DOUBLE_EQ(raw_union->getArea(), 6.0);
    EXPECT_DOUBLE_EQ(move_union->getArea(), 6.0);
    EXPECT_TRUE(raw_union->equals(expected.get()));
    EXPECT_TRUE(move_union->equals(expected.get()));

    // Verify empty and lineal inputs through the rvalue overload.
    std::unique_ptr<Geometry> l0 = ReadGeom(*factory, "LINESTRING (0 0, 2 0)");
    std::unique_ptr<Geometry> l1 = ReadGeom(*factory, "LINESTRING (1 0, 3 0)");
    std::unique_ptr<Geometry> line_union =
        strategy->Union(std::move(l0), std::move(l1));
    ASSERT_NE(line_union, nullptr);
    EXPECT_DOUBLE_EQ(line_union->getLength(), 3.0);

    std::unique_ptr<Geometry> poly =
        ReadGeom(*factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
    std::unique_ptr<Geometry> empty = ReadGeom(*factory, "POLYGON EMPTY");
    std::unique_ptr<Geometry> with_empty =
        strategy->Union(std::move(poly), std::move(empty));
    ASSERT_NE(with_empty, nullptr);
    EXPECT_DOUBLE_EQ(with_empty->getArea(), 1.0);
  }
}

TEST(UnionStrategyTest, WorksWithCascadedPolygonUnionAndUnaryUnionOp) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::vector<std::unique_ptr<Polygon>> owned_polys;
  std::vector<Polygon*> raw_polys;
  for (std::string_view wkt : {
           "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
           "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))",
           "POLYGON ((0 1, 2 1, 2 3, 0 3, 0 1))",
           "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))",
       }) {
    owned_polys.push_back(reader.read<Polygon>(std::string(wkt)));
    raw_polys.push_back(owned_polys.back().get());
  }

  RecordingUnionStrategy cascaded_strategy;
  std::unique_ptr<Geometry> cascaded =
      CascadedPolygonUnion::Union(&raw_polys, &cascaded_strategy);
  ASSERT_NE(cascaded, nullptr);
  EXPECT_DOUBLE_EQ(cascaded->getArea(), 9.0);
  EXPECT_EQ(cascaded_strategy.recorded_calls().size(), 3);

  std::unique_ptr<Geometry> collection = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)),"
      "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0)),"
      "LINESTRING (5 0, 7 0)"
      ")");
  RecordingUnionStrategy unary_strategy;
  UnaryUnionOp op(*collection);
  op.setUnionFunction(&unary_strategy);
  std::unique_ptr<Geometry> unary_result = op.Union();
  ASSERT_NE(unary_result, nullptr);
  EXPECT_DOUBLE_EQ(unary_result->getArea(), 6.0);
  EXPECT_DOUBLE_EQ(unary_result->getLength(), 12.0);
  EXPECT_GE(unary_strategy.recorded_calls().size(), 2);
}

}  // namespace
}  // namespace geos_google
