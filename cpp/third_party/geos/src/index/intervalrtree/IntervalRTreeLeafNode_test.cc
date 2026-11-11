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
// third_party/geos/src/index/intervalrtree/IntervalRTreeLeafNode.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/intervalrtree/IntervalRTreeLeafNode.cpp

#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeLeafNode.h"

#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/intervalrtree/IntervalRTreeNode.h"

namespace geos_google {
namespace {

using geos::index::ItemVisitor;
using geos::index::intervalrtree::IntervalRTreeLeafNode;
using geos::index::intervalrtree::IntervalRTreeNode;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class RecordingVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }
  void clear() { items_.clear(); }

 private:
  std::vector<void*> items_;
};

struct QueryCase {
  std::string_view name;
  double leaf_min;
  double leaf_max;
  double query_min;
  double query_max;
  bool expected_hit;
};

constexpr auto kQueryCases = std::to_array<QueryCase>({
    {
        .name = "strictly_left",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 0.0,
        .query_max = 1.5,
        .expected_hit = false,
    },
    {
        .name = "strictly_right",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 5.5,
        .query_max = 8.0,
        .expected_hit = false,
    },
    {
        .name = "touches_min_boundary",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 0.0,
        .query_max = 2.0,
        .expected_hit = true,
    },
    {
        .name = "touches_max_boundary",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 5.0,
        .query_max = 7.0,
        .expected_hit = true,
    },
    {
        .name = "point_at_min",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 2.0,
        .query_max = 2.0,
        .expected_hit = true,
    },
    {
        .name = "point_at_max",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 5.0,
        .query_max = 5.0,
        .expected_hit = true,
    },
    {
        .name = "point_inside",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 3.5,
        .query_max = 3.5,
        .expected_hit = true,
    },
    {
        .name = "partial_overlap_left",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 1.0,
        .query_max = 3.0,
        .expected_hit = true,
    },
    {
        .name = "partial_overlap_right",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 4.0,
        .query_max = 6.0,
        .expected_hit = true,
    },
    {
        .name = "contained_inside_leaf",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 2.5,
        .query_max = 4.5,
        .expected_hit = true,
    },
    {
        .name = "contains_leaf",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 0.0,
        .query_max = 10.0,
        .expected_hit = true,
    },
    {
        .name = "exact_match",
        .leaf_min = 2.0,
        .leaf_max = 5.0,
        .query_min = 2.0,
        .query_max = 5.0,
        .expected_hit = true,
    },
    {
        .name = "negative_interval_miss_left",
        .leaf_min = -10.0,
        .leaf_max = -3.0,
        .query_min = -15.0,
        .query_max = -10.5,
        .expected_hit = false,
    },
    {
        .name = "negative_interval_miss_right",
        .leaf_min = -10.0,
        .leaf_max = -3.0,
        .query_min = -2.5,
        .query_max = 0.0,
        .expected_hit = false,
    },
    {
        .name = "negative_interval_hit",
        .leaf_min = -10.0,
        .leaf_max = -3.0,
        .query_min = -6.0,
        .query_max = -1.0,
        .expected_hit = true,
    },
    {
        .name = "point_leaf_exact_hit",
        .leaf_min = 4.25,
        .leaf_max = 4.25,
        .query_min = 4.25,
        .query_max = 4.25,
        .expected_hit = true,
    },
    {
        .name = "point_leaf_spanning_hit",
        .leaf_min = 4.25,
        .leaf_max = 4.25,
        .query_min = 0.0,
        .query_max = 5.0,
        .expected_hit = true,
    },
    {
        .name = "point_leaf_miss_left",
        .leaf_min = 4.25,
        .leaf_max = 4.25,
        .query_min = 0.0,
        .query_max = 4.0,
        .expected_hit = false,
    },
    {
        .name = "point_leaf_miss_right",
        .leaf_min = 4.25,
        .leaf_max = 4.25,
        .query_min = 4.5,
        .query_max = 5.0,
        .expected_hit = false,
    },
});

TEST(IntervalRTreeLeafNodeTest, ConstructsWithBoundsAndItem) {
  int payload = 42;
  const IntervalRTreeLeafNode leaf(1.5, 4.5, &payload);

  EXPECT_DOUBLE_EQ(leaf.getMin(), 1.5);
  EXPECT_DOUBLE_EQ(leaf.getMax(), 4.5);

  RecordingVisitor visitor;
  leaf.query(2.0, 3.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(&payload));
}

TEST(IntervalRTreeLeafNodeTest, VisitsNullItemPointerWhenIntersecting) {
  const IntervalRTreeLeafNode leaf(0.0, 2.0, nullptr);
  RecordingVisitor visitor;

  leaf.query(3.0, 4.0, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  leaf.query(1.0, 3.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(nullptr));
}

TEST(IntervalRTreeLeafNodeTest, PolymorphicOwnershipViaBaseNode) {
  int payload = 7;
  std::unique_ptr<IntervalRTreeNode> node =
      std::make_unique<IntervalRTreeLeafNode>(-3.0, 2.0, &payload);

  EXPECT_DOUBLE_EQ(node->getMin(), -3.0);
  EXPECT_DOUBLE_EQ(node->getMax(), 2.0);

  RecordingVisitor visitor;
  node->query(-1.0, 0.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(&payload));
}

TEST(IntervalRTreeLeafNodeTest, QueryIntersectionAndDisjointCases) {
  int payload = 99;
  for (const auto& test_case : kQueryCases) {
    SCOPED_TRACE(test_case.name);
    const IntervalRTreeLeafNode leaf(test_case.leaf_min, test_case.leaf_max,
                                     &payload);
    RecordingVisitor visitor;
    leaf.query(test_case.query_min, test_case.query_max, &visitor);

    if (test_case.expected_hit) {
      EXPECT_THAT(visitor.items(), ElementsAre(&payload));
    } else {
      EXPECT_THAT(visitor.items(), IsEmpty());
    }
  }
}

TEST(IntervalRTreeLeafNodeTest, QueryBoundaryAdjacentEpsilon) {
  int payload = 11;
  const IntervalRTreeLeafNode leaf(2.0, 5.0, &payload);
  RecordingVisitor visitor;

  const double just_below_min = std::nextafter(2.0, 0.0);
  leaf.query(0.0, just_below_min, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  const double just_above_max = std::nextafter(5.0, 6.0);
  leaf.query(just_above_max, 6.0, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  leaf.query(just_below_min, 2.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(&payload));
}

TEST(IntervalRTreeLeafNodeTest, QueryWithInfiniteBounds) {
  int left_payload = 1;
  int right_payload = 2;
  const IntervalRTreeLeafNode left_unbounded(geos::DoubleNegInfinity, 0.0,
                                             &left_payload);
  const IntervalRTreeLeafNode right_unbounded(0.0, geos::DoubleInfinity,
                                              &right_payload);

  RecordingVisitor visitor;
  left_unbounded.query(geos::DoubleNegInfinity, -1000.0, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(&left_payload));

  visitor.clear();
  left_unbounded.query(1.0, geos::DoubleInfinity, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());

  visitor.clear();
  right_unbounded.query(1000.0, geos::DoubleInfinity, &visitor);
  EXPECT_THAT(visitor.items(), ElementsAre(&right_payload));

  visitor.clear();
  right_unbounded.query(geos::DoubleNegInfinity, -1.0, &visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());
}

TEST(IntervalRTreeLeafNodeTest, RepeatedQueriesAreConsistent) {
  int payload = 5;
  const IntervalRTreeLeafNode leaf(10.0, 20.0, &payload);
  RecordingVisitor visitor;

  leaf.query(12.0, 15.0, &visitor);
  leaf.query(21.0, 25.0, &visitor);
  leaf.query(10.0, 10.0, &visitor);

  EXPECT_THAT(visitor.items(), ElementsAre(&payload, &payload));
}

}  // namespace
}  // namespace geos_google
