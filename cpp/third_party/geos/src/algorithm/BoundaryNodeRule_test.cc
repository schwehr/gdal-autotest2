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

// Unit tests for third_party/geos/src/algorithm/BoundaryNodeRule.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/BoundaryNodeRule.cpp

#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"

#include <array>
#include <cstddef>
#include <string_view>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;

TEST(BoundaryNodeRuleTest, SingletonInstancesAreStableAndDistinct) {
  const BoundaryNodeRule& mod2 = BoundaryNodeRule::getBoundaryRuleMod2();
  const BoundaryNodeRule& ogc_sfs = BoundaryNodeRule::getBoundaryOGCSFS();
  const BoundaryNodeRule& endpoint = BoundaryNodeRule::getBoundaryEndPoint();
  const BoundaryNodeRule& multivalent =
      BoundaryNodeRule::getBoundaryMultivalentEndPoint();
  const BoundaryNodeRule& monovalent =
      BoundaryNodeRule::getBoundaryMonovalentEndPoint();

  // Repeated lookups return the exact same singleton objects.
  EXPECT_EQ(&mod2, &BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_EQ(&endpoint, &BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_EQ(&multivalent, &BoundaryNodeRule::getBoundaryMultivalentEndPoint());
  EXPECT_EQ(&monovalent, &BoundaryNodeRule::getBoundaryMonovalentEndPoint());

  // OGC SFS is defined as the Mod-2 rule.
  EXPECT_EQ(&ogc_sfs, &mod2);

  // The four concrete rules are distinct instances.
  EXPECT_NE(&mod2, &endpoint);
  EXPECT_NE(&mod2, &multivalent);
  EXPECT_NE(&mod2, &monovalent);
  EXPECT_NE(&endpoint, &multivalent);
  EXPECT_NE(&endpoint, &monovalent);
  EXPECT_NE(&multivalent, &monovalent);
}

TEST(BoundaryNodeRuleTest, ToStringAndIsInBoundaryValencyBehavior) {
  struct RuleExpectation {
    const BoundaryNodeRule& (*getter)();
    std::string_view expected_name;
    // Expected results for boundaryCount = {-1, 0, 1, 2, 3, 4, 5}.
    std::array<bool, 7> expected_in_boundary;
  };

  constexpr auto kCounts = std::to_array<int>({-1, 0, 1, 2, 3, 4, 5});
  constexpr auto kRules = std::to_array<RuleExpectation>({
      {
          .getter = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_name = "Mod2BoundaryNodeRule",
          .expected_in_boundary = {false, false, true, false, true, false,
                                   true},
      },
      {
          .getter = &BoundaryNodeRule::getBoundaryOGCSFS,
          .expected_name = "Mod2BoundaryNodeRule",
          .expected_in_boundary = {false, false, true, false, true, false,
                                   true},
      },
      {
          .getter = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_name = "EndPointBoundaryNodeRule",
          .expected_in_boundary = {false, false, true, true, true, true, true},
      },
      {
          .getter = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_name = "MultiValentEndPointBoundaryNodeRule",
          .expected_in_boundary = {false, false, false, true, true, true, true},
      },
      {
          .getter = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_name = "MonoValentEndPointBoundaryNodeRule",
          .expected_in_boundary = {false, false, true, false, false, false,
                                   false},
      },
  });

  for (const auto& rule_spec : kRules) {
    const BoundaryNodeRule& rule = rule_spec.getter();
    SCOPED_TRACE(rule_spec.expected_name);
    EXPECT_EQ(rule.toString(), rule_spec.expected_name);
    for (std::size_t i = 0; i < kCounts.size(); ++i) {
      SCOPED_TRACE(kCounts[i]);
      EXPECT_EQ(rule.isInBoundary(kCounts[i]),
                rule_spec.expected_in_boundary[i]);
    }
  }
}

}  // namespace
}  // namespace geos_google
