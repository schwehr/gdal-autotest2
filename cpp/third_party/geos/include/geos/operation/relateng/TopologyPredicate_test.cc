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

// Unit tests for
// third_party/geos/include/geos/operation/relateng/TopologyPredicate.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/relateng/TopologyPredicate.h

#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/RelateMatrixPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::RelateMatrixPredicate;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyPredicate;

static_assert(std::is_abstract_v<TopologyPredicate>);
static_assert(std::has_virtual_destructor_v<TopologyPredicate>);
static_assert(!std::is_copy_constructible_v<TopologyPredicate>);

struct DimensionUpdate {
  Location loc_a;
  Location loc_b;
  int dimension;

  bool operator==(const DimensionUpdate&) const = default;
};

// Concrete minimal implementation inheriting all non-pure virtual defaults
// directly from TopologyPredicate.
class MinimalTopologyPredicate final : public TopologyPredicate {
 public:
  explicit MinimalTopologyPredicate(bool* destroyed_flag = nullptr)
      : destroyed_flag_(destroyed_flag) {}

  ~MinimalTopologyPredicate() override {
    if (destroyed_flag_ != nullptr) {
      *destroyed_flag_ = true;
    }
  }

  std::string name() const override { return "minimal"; }

  void finish() override {
    finish_called_ = true;
    known_ = true;
  }

  bool isKnown() const override { return known_; }

  bool value() const override { return result_; }

  void updateDimension(geos::geom::Location locA, geos::geom::Location locB,
                       int dimension) override {
    updates_.push_back(
        DimensionUpdate{.loc_a = locA, .loc_b = locB, .dimension = dimension});
  }

  void setResult(bool result) { result_ = result; }
  bool finishCalled() const { return finish_called_; }
  const std::vector<DimensionUpdate>& updates() const { return updates_; }

 private:
  bool known_ = false;
  bool result_ = false;
  bool finish_called_ = false;
  bool* destroyed_flag_ = nullptr;
  std::vector<DimensionUpdate> updates_;
};

// Configurable predicate observing RelateNG's short-circuiting and capability
// hooks.
class ConfigurablePredicate final : public TopologyPredicate {
 public:
  struct Config {
    bool require_self_noding = true;
    bool require_interaction = true;
    bool require_covers_a = false;
    bool require_covers_b = false;
    bool require_exterior_a = true;
    bool require_exterior_b = true;
    bool resolve_in_dim_init = false;
    bool resolve_in_dim_init_value = false;
    bool resolve_in_env_init = false;
    bool resolve_in_env_init_value = false;
    bool resolve_on_ii = false;
    bool resolve_on_ii_value = false;
    bool finish_value = false;
  };

  explicit ConfigurablePredicate(Config config) : config_(config) {}

  std::string name() const override { return "configurable"; }

  void finish() override {
    finish_calls_++;
    if (!known_) {
      known_ = true;
      result_ = config_.finish_value;
    }
  }

  bool isKnown() const override { return known_; }

  bool value() const override { return result_; }

  bool requireSelfNoding() const override {
    require_self_noding_calls_++;
    return config_.require_self_noding;
  }

  bool requireInteraction() const override {
    require_interaction_calls_++;
    return config_.require_interaction;
  }

  bool requireCovers(bool isSourceA) override {
    require_covers_calls_++;
    return isSourceA ? config_.require_covers_a : config_.require_covers_b;
  }

  bool requireExteriorCheck(bool isSourceA) const override {
    require_exterior_calls_++;
    return isSourceA ? config_.require_exterior_a : config_.require_exterior_b;
  }

  void init(int dimA, int dimB) override {
    init_dim_calls_++;
    last_dim_a_ = dimA;
    last_dim_b_ = dimB;
    if (config_.resolve_in_dim_init) {
      known_ = true;
      result_ = config_.resolve_in_dim_init_value;
    }
  }

  void init(const geos::geom::Envelope& /*envA*/,
            const geos::geom::Envelope& /*envB*/) override {
    init_env_calls_++;
    if (config_.resolve_in_env_init) {
      known_ = true;
      result_ = config_.resolve_in_env_init_value;
    }
  }

  void updateDimension(geos::geom::Location locA, geos::geom::Location locB,
                       int dimension) override {
    updates_.push_back(
        DimensionUpdate{.loc_a = locA, .loc_b = locB, .dimension = dimension});
    if (config_.resolve_on_ii && locA == geos::geom::Location::INTERIOR &&
        locB == geos::geom::Location::INTERIOR && dimension >= Dimension::P) {
      known_ = true;
      result_ = config_.resolve_on_ii_value;
    }
  }

  int initDimCalls() const { return init_dim_calls_; }
  int initEnvCalls() const { return init_env_calls_; }
  int finishCalls() const { return finish_calls_; }
  int requireSelfNodingCalls() const { return require_self_noding_calls_; }
  int requireInteractionCalls() const { return require_interaction_calls_; }
  int requireCoversCalls() const { return require_covers_calls_; }
  int requireExteriorCalls() const { return require_exterior_calls_; }
  int lastDimA() const { return last_dim_a_; }
  int lastDimB() const { return last_dim_b_; }
  const std::vector<DimensionUpdate>& updates() const { return updates_; }

 private:
  Config config_;
  bool known_ = false;
  bool result_ = false;
  int init_dim_calls_ = 0;
  int init_env_calls_ = 0;
  int finish_calls_ = 0;
  mutable int require_self_noding_calls_ = 0;
  mutable int require_interaction_calls_ = 0;
  int require_covers_calls_ = 0;
  mutable int require_exterior_calls_ = 0;
  int last_dim_a_ = Dimension::DONTCARE;
  int last_dim_b_ = Dimension::DONTCARE;
  std::vector<DimensionUpdate> updates_;
};

TEST(TopologyPredicateTest, DefaultVirtualBehaviorAndStreamFormatting) {
  MinimalTopologyPredicate concrete;
  TopologyPredicate& pred = concrete;

  EXPECT_EQ(pred.name(), "minimal");
  EXPECT_TRUE(pred.requireSelfNoding());
  EXPECT_TRUE(pred.requireInteraction());
  EXPECT_FALSE(pred.requireCovers(true));
  EXPECT_FALSE(pred.requireCovers(false));
  EXPECT_TRUE(pred.requireExteriorCheck(true));
  EXPECT_TRUE(pred.requireExteriorCheck(false));
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  // Default init overloads are safe no-ops that do not resolve the predicate.
  pred.init(Dimension::L, Dimension::A);
  pred.init(Dimension::False, Dimension::False);
  EXPECT_FALSE(pred.isKnown());

  const Envelope env_a(0.0, 2.0, 0.0, 2.0);
  const Envelope env_b(5.0, 7.0, 5.0, 7.0);
  const Envelope null_env;
  pred.init(env_a, env_b);
  pred.init(null_env, null_env);
  EXPECT_FALSE(pred.isKnown());

  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  ASSERT_EQ(concrete.updates().size(), 1u);
  EXPECT_EQ(
      concrete.updates()[0],
      (DimensionUpdate{Location::INTERIOR, Location::INTERIOR, Dimension::P}));

  concrete.setResult(true);
  pred.finish();
  EXPECT_TRUE(concrete.finishCalled());
  EXPECT_TRUE(pred.isKnown());
  EXPECT_TRUE(pred.value());

  std::ostringstream oss;
  oss << pred << " / " << pred;
  EXPECT_EQ(oss.str(), "minimal / minimal");
}

TEST(TopologyPredicateTest, PolymorphicDestructionViaBasePointer) {
  bool destroyed = false;
  {
    std::unique_ptr<TopologyPredicate> pred =
        std::make_unique<MinimalTopologyPredicate>(&destroyed);
    EXPECT_FALSE(destroyed);
    EXPECT_EQ(pred->name(), "minimal");
    pred->finish();
    EXPECT_TRUE(pred->isKnown());
  }
  EXPECT_TRUE(destroyed);
}

TEST(TopologyPredicateTest, DefaultInterfaceDrivenByRelateNG) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> pt_a = reader.read("POINT (0 0)");
  std::unique_ptr<Geometry> pt_b = reader.read("POINT (10 10)");

  // Default requireInteraction() is true, so disjoint envelopes short-circuit
  // before init() or finish().
  MinimalTopologyPredicate disjoint_pred;
  EXPECT_FALSE(RelateNG::relate(pt_a.get(), pt_b.get(), disjoint_pred));
  EXPECT_FALSE(disjoint_pred.finishCalled());
  EXPECT_FALSE(disjoint_pred.isKnown());
  EXPECT_TRUE(disjoint_pred.updates().empty());

  // Intersecting geometries drive default init(dimA, dimB), init(envA, envB),
  // requireSelfNoding(), requireExteriorCheck(), updateDimension(), and
  // finish().
  std::unique_ptr<Geometry> line_a = reader.read("LINESTRING (0 0, 2 2)");
  std::unique_ptr<Geometry> line_b = reader.read("LINESTRING (0 2, 2 0)");

  MinimalTopologyPredicate crossing_pred;
  crossing_pred.setResult(true);
  EXPECT_TRUE(RelateNG::relate(line_a.get(), line_b.get(), crossing_pred));
  EXPECT_TRUE(crossing_pred.finishCalled());
  EXPECT_TRUE(crossing_pred.isKnown());
  EXPECT_TRUE(crossing_pred.value());
  EXPECT_NE(
      std::find(crossing_pred.updates().begin(), crossing_pred.updates().end(),
                DimensionUpdate{Location::INTERIOR, Location::INTERIOR,
                                Dimension::P}),
      crossing_pred.updates().end());
}

TEST(TopologyPredicateTest, OverridesControlRelateNGShortCircuiting) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> small_poly =
      reader.read("POLYGON ((1 1, 2 1, 2 2, 1 2, 1 1))");
  std::unique_ptr<Geometry> large_poly =
      reader.read("POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  std::unique_ptr<Geometry> inner_pt = reader.read("POINT (2 2)");
  std::unique_ptr<Geometry> disjoint_pt = reader.read("POINT (20 20)");
  std::unique_ptr<Geometry> line_a = reader.read("LINESTRING (0 0, 4 4)");
  std::unique_ptr<Geometry> line_b = reader.read("LINESTRING (0 4, 4 0)");

  // requireCovers(GEOM_A) rejects when envA does not cover envB.
  {
    ConfigurablePredicate::Config cfg;
    cfg.require_covers_a = true;
    ConfigurablePredicate pred(cfg);
    EXPECT_FALSE(RelateNG::relate(small_poly.get(), large_poly.get(), pred));
    EXPECT_GT(pred.requireCoversCalls(), 0);
    EXPECT_EQ(pred.initDimCalls(), 0);
    EXPECT_EQ(pred.finishCalls(), 0);
  }

  // requireCovers(GEOM_B) passes when envB covers envA.
  {
    ConfigurablePredicate::Config cfg;
    cfg.require_covers_b = true;
    cfg.finish_value = true;
    ConfigurablePredicate pred(cfg);
    EXPECT_TRUE(RelateNG::relate(small_poly.get(), large_poly.get(), pred));
    EXPECT_GT(pred.requireCoversCalls(), 0);
    EXPECT_EQ(pred.initDimCalls(), 1);
    EXPECT_EQ(pred.initEnvCalls(), 1);
    EXPECT_EQ(pred.finishCalls(), 1);
  }

  // requireInteraction() == false lets disjoint geometries run init & finish.
  {
    ConfigurablePredicate::Config cfg;
    cfg.require_interaction = false;
    cfg.finish_value = true;
    ConfigurablePredicate pred(cfg);
    EXPECT_TRUE(RelateNG::relate(small_poly.get(), disjoint_pt.get(), pred));
    EXPECT_GT(pred.requireInteractionCalls(), 0);
    EXPECT_EQ(pred.initDimCalls(), 1);
    EXPECT_EQ(pred.lastDimA(), Dimension::A);
    EXPECT_EQ(pred.lastDimB(), Dimension::P);
    EXPECT_EQ(pred.initEnvCalls(), 1);
    EXPECT_EQ(pred.finishCalls(), 1);
    EXPECT_FALSE(pred.updates().empty());
  }

  // Resolving in init(dimA, dimB) skips init(envA, envB) and updateDimension().
  {
    ConfigurablePredicate::Config cfg;
    cfg.resolve_in_dim_init = true;
    cfg.resolve_in_dim_init_value = true;
    ConfigurablePredicate pred(cfg);
    EXPECT_TRUE(RelateNG::relate(line_a.get(), line_b.get(), pred));
    EXPECT_EQ(pred.initDimCalls(), 1);
    EXPECT_EQ(pred.initEnvCalls(), 0);
    EXPECT_EQ(pred.finishCalls(), 1);
    EXPECT_TRUE(pred.updates().empty());
  }

  // Resolving in init(envA, envB) skips TopologyComputer / updateDimension().
  {
    ConfigurablePredicate::Config cfg;
    cfg.resolve_in_env_init = true;
    cfg.resolve_in_env_init_value = true;
    ConfigurablePredicate pred(cfg);
    EXPECT_TRUE(RelateNG::relate(line_a.get(), line_b.get(), pred));
    EXPECT_EQ(pred.initDimCalls(), 1);
    EXPECT_EQ(pred.initEnvCalls(), 1);
    EXPECT_EQ(pred.finishCalls(), 1);
    EXPECT_TRUE(pred.updates().empty());
  }

  // Resolving inside updateDimension() during computeAtPoints() returns before
  // finish() is invoked.
  {
    ConfigurablePredicate::Config cfg;
    cfg.resolve_on_ii = true;
    cfg.resolve_on_ii_value = true;
    ConfigurablePredicate pred(cfg);
    EXPECT_TRUE(RelateNG::relate(inner_pt.get(), large_poly.get(), pred));
    EXPECT_TRUE(pred.isKnown());
    EXPECT_TRUE(pred.value());
    EXPECT_EQ(pred.finishCalls(), 0);
    EXPECT_FALSE(pred.updates().empty());
  }

  // Resolving inside updateDimension() during computeAtEdges() queries
  // requireSelfNoding() and requireExteriorCheck(), and finish() does not
  // overwrite the already-known result.
  {
    ConfigurablePredicate::Config cfg;
    cfg.require_self_noding = false;
    cfg.require_exterior_a = false;
    cfg.require_exterior_b = false;
    cfg.resolve_on_ii = true;
    cfg.resolve_on_ii_value = true;
    cfg.finish_value = false;
    ConfigurablePredicate pred(cfg);
    EXPECT_TRUE(RelateNG::relate(line_a.get(), line_b.get(), pred));
    EXPECT_GT(pred.requireSelfNodingCalls(), 0);
    EXPECT_GT(pred.requireExteriorCalls(), 0);
    EXPECT_TRUE(pred.isKnown());
    EXPECT_TRUE(pred.value());
    EXPECT_EQ(pred.finishCalls(), 1);
    EXPECT_FALSE(pred.updates().empty());
  }
}

TEST(TopologyPredicateTest, StreamInsertionFormatsStandardPredicates) {
  struct NamedPredicate {
    std::unique_ptr<TopologyPredicate> predicate;
    std::string_view expected_name;
  };

  std::vector<NamedPredicate> cases;
  cases.push_back({RelatePredicate::intersects(), "intersects"});
  cases.push_back({RelatePredicate::disjoint(), "disjoint"});
  cases.push_back({RelatePredicate::contains(), "contains"});
  cases.push_back({RelatePredicate::within(), "within"});
  cases.push_back({RelatePredicate::covers(), "covers"});
  cases.push_back({RelatePredicate::coveredBy(), "coveredBy"});
  cases.push_back({RelatePredicate::crosses(), "crosses"});
  cases.push_back({RelatePredicate::equalsTopo(), "equals"});
  cases.push_back({RelatePredicate::overlaps(), "overlaps"});
  cases.push_back({RelatePredicate::touches(), "touches"});
  cases.push_back({RelatePredicate::matches("T*F**FFF*"), "IMPattern"});
  cases.push_back({std::make_unique<RelateMatrixPredicate>(), "relateMatrix"});

  for (const auto& entry : cases) {
    ASSERT_NE(entry.predicate, nullptr);
    EXPECT_EQ(entry.predicate->name(), entry.expected_name);
    std::ostringstream oss;
    oss << *entry.predicate;
    EXPECT_EQ(oss.str(), entry.expected_name);
  }
}

}  // namespace
}  // namespace geos_google
