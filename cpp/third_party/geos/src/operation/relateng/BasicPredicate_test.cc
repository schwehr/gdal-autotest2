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

// Unit tests for third_party/geos/src/operation/relateng/BasicPredicate.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/BasicPredicate.cpp

#include "third_party/geos/include/geos/operation/relateng/BasicPredicate.h"

#include <array>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::geom::Location;
using geos::operation::relateng::BasicPredicate;
using geos::operation::relateng::TopologyPredicate;

class TestBasicPredicate final : public BasicPredicate {
 public:
  std::string name() const override { return "TestBasicPredicate"; }

  void finish() override { setValue(false); }

  void updateDimension(Location /*locA*/, Location /*locB*/,
                       int /*dimension*/) override {}

  using BasicPredicate::require;
  using BasicPredicate::requireCovers;
  using BasicPredicate::setValue;
  using BasicPredicate::setValueIf;
};

struct IntersectionCase {
  Location loc_a;
  Location loc_b;
  bool expected;
};

TEST(BasicPredicateTest, InitialStateIsUnknownAndValueIsFalse) {
  TestBasicPredicate pred;
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  TopologyPredicate& iface = pred;
  EXPECT_EQ(iface.name(), "TestBasicPredicate");
  EXPECT_FALSE(iface.requireCovers(true));
  EXPECT_FALSE(iface.requireCovers(false));

  std::ostringstream oss;
  oss << iface;
  EXPECT_EQ(oss.str(), "TestBasicPredicate");
}

TEST(BasicPredicateTest, IsIntersection) {
  constexpr auto kCases = std::to_array<IntersectionCase>({
      {.loc_a = Location::INTERIOR,
       .loc_b = Location::INTERIOR,
       .expected = true},
      {.loc_a = Location::INTERIOR,
       .loc_b = Location::BOUNDARY,
       .expected = true},
      {.loc_a = Location::INTERIOR,
       .loc_b = Location::EXTERIOR,
       .expected = false},
      {.loc_a = Location::INTERIOR, .loc_b = Location::NONE, .expected = true},
      {.loc_a = Location::BOUNDARY,
       .loc_b = Location::INTERIOR,
       .expected = true},
      {.loc_a = Location::BOUNDARY,
       .loc_b = Location::BOUNDARY,
       .expected = true},
      {.loc_a = Location::BOUNDARY,
       .loc_b = Location::EXTERIOR,
       .expected = false},
      {.loc_a = Location::BOUNDARY, .loc_b = Location::NONE, .expected = true},
      {.loc_a = Location::EXTERIOR,
       .loc_b = Location::INTERIOR,
       .expected = false},
      {.loc_a = Location::EXTERIOR,
       .loc_b = Location::BOUNDARY,
       .expected = false},
      {.loc_a = Location::EXTERIOR,
       .loc_b = Location::EXTERIOR,
       .expected = false},
      {.loc_a = Location::EXTERIOR, .loc_b = Location::NONE, .expected = false},
      {.loc_a = Location::NONE, .loc_b = Location::INTERIOR, .expected = true},
      {.loc_a = Location::NONE, .loc_b = Location::BOUNDARY, .expected = true},
      {.loc_a = Location::NONE, .loc_b = Location::EXTERIOR, .expected = false},
      {.loc_a = Location::NONE, .loc_b = Location::NONE, .expected = true},
  });
  for (const auto& tc : kCases) {
    EXPECT_EQ(BasicPredicate::isIntersection(tc.loc_a, tc.loc_b), tc.expected);
  }
}

TEST(BasicPredicateTest, SetValueBoolSetsOnceAndDoesNotOverwrite) {
  TestBasicPredicate pred_true;
  pred_true.setValue(true);
  EXPECT_TRUE(pred_true.isKnown());
  EXPECT_TRUE(pred_true.value());

  pred_true.setValue(false);
  EXPECT_TRUE(pred_true.isKnown());
  EXPECT_TRUE(pred_true.value());

  TestBasicPredicate pred_false;
  pred_false.setValue(false);
  EXPECT_TRUE(pred_false.isKnown());
  EXPECT_FALSE(pred_false.value());

  pred_false.setValue(true);
  EXPECT_TRUE(pred_false.isKnown());
  EXPECT_FALSE(pred_false.value());
}

TEST(BasicPredicateTest, SetValueIntHandlesUnknownAndLocksOnceKnown) {
  TestBasicPredicate pred;
  pred.setValue(-1);
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  pred.setValue(-2);
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  pred.setValue(1);
  EXPECT_TRUE(pred.isKnown());
  EXPECT_TRUE(pred.value());

  pred.setValue(0);
  pred.setValue(-1);
  EXPECT_TRUE(pred.isKnown());
  EXPECT_TRUE(pred.value());

  TestBasicPredicate pred_zero;
  pred_zero.setValue(0);
  EXPECT_TRUE(pred_zero.isKnown());
  EXPECT_FALSE(pred_zero.value());

  pred_zero.setValue(1);
  EXPECT_TRUE(pred_zero.isKnown());
  EXPECT_FALSE(pred_zero.value());

  TestBasicPredicate pred_non_one;
  pred_non_one.setValue(2);
  EXPECT_TRUE(pred_non_one.isKnown());
  EXPECT_FALSE(pred_non_one.value());
  pred_non_one.setValue(1);
  EXPECT_FALSE(pred_non_one.value());
}

TEST(BasicPredicateTest, SetValueIfRespectsConditionAndLocking) {
  TestBasicPredicate pred;
  pred.setValueIf(true, false);
  EXPECT_FALSE(pred.isKnown());

  pred.setValueIf(false, false);
  EXPECT_FALSE(pred.isKnown());

  pred.setValueIf(true, true);
  EXPECT_TRUE(pred.isKnown());
  EXPECT_TRUE(pred.value());

  pred.setValueIf(false, true);
  EXPECT_TRUE(pred.value());

  TestBasicPredicate pred_false;
  pred_false.setValueIf(false, true);
  EXPECT_TRUE(pred_false.isKnown());
  EXPECT_FALSE(pred_false.value());
}

TEST(BasicPredicateTest, RequireSetsFalseOnlyWhenConditionFails) {
  TestBasicPredicate pred;
  pred.require(true);
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  pred.require(false);
  EXPECT_TRUE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  pred.require(true);
  EXPECT_TRUE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  TestBasicPredicate pred_true;
  pred_true.setValue(true);
  pred_true.require(false);
  EXPECT_TRUE(pred_true.isKnown());
  EXPECT_TRUE(pred_true.value());
}

TEST(BasicPredicateTest, RequireCoversEnvelopes) {
  const Envelope unit_box(0.0, 10.0, 0.0, 10.0);
  const Envelope inner_box(2.0, 8.0, 2.0, 8.0);
  const Envelope overlapping_box(5.0, 15.0, 2.0, 8.0);
  const Envelope disjoint_box(20.0, 30.0, 20.0, 30.0);
  const Envelope null_env;

  TestBasicPredicate covering;
  covering.requireCovers(unit_box, inner_box);
  EXPECT_FALSE(covering.isKnown());

  covering.requireCovers(unit_box, unit_box);
  EXPECT_FALSE(covering.isKnown());

  TestBasicPredicate partial;
  partial.requireCovers(unit_box, overlapping_box);
  EXPECT_TRUE(partial.isKnown());
  EXPECT_FALSE(partial.value());

  TestBasicPredicate disjoint;
  disjoint.requireCovers(unit_box, disjoint_box);
  EXPECT_TRUE(disjoint.isKnown());
  EXPECT_FALSE(disjoint.value());

  TestBasicPredicate null_target;
  null_target.requireCovers(unit_box, null_env);
  EXPECT_TRUE(null_target.isKnown());
  EXPECT_FALSE(null_target.value());

  TestBasicPredicate null_source;
  null_source.requireCovers(null_env, inner_box);
  EXPECT_TRUE(null_source.isKnown());
  EXPECT_FALSE(null_source.value());

  TestBasicPredicate already_true;
  already_true.setValue(true);
  already_true.requireCovers(unit_box, disjoint_box);
  EXPECT_TRUE(already_true.isKnown());
  EXPECT_TRUE(already_true.value());
}

TEST(BasicPredicateTest, FinishResolvesUnknownWithoutOverwritingKnownTrue) {
  TestBasicPredicate pred;
  pred.finish();
  EXPECT_TRUE(pred.isKnown());
  EXPECT_FALSE(pred.value());

  TestBasicPredicate pred_true;
  pred_true.setValue(true);
  pred_true.finish();
  EXPECT_TRUE(pred_true.isKnown());
  EXPECT_TRUE(pred_true.value());
}

}  // namespace
}  // namespace geos_google
