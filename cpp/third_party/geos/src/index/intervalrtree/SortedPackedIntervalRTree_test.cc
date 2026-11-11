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
// third_party/geos/src/index/intervalrtree/SortedPackedIntervalRTree.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/intervalrtree/SortedPackedIntervalRTree.cpp

#include "third_party/geos/include/geos/index/intervalrtree/SortedPackedIntervalRTree.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::index::ItemVisitor;
using geos::index::intervalrtree::SortedPackedIntervalRTree;
using geos::util::UnsupportedOperationException;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAreArray;

class IdVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override {
    ids_.push_back(*static_cast<const int*>(item));
  }

  const std::vector<int>& ids() const { return ids_; }
  void clear() { ids_.clear(); }

 private:
  std::vector<int> ids_;
};

class PointerVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { ptrs_.push_back(item); }

  const std::vector<void*>& ptrs() const { return ptrs_; }

 private:
  std::vector<void*> ptrs_;
};

struct IntervalSpec {
  double min;
  double max;
  int id;
};

struct QueryRange {
  std::string_view name;
  double min;
  double max;
};

std::vector<int> BruteForceQuery(const std::vector<IntervalSpec>& specs,
                                 double query_min, double query_max) {
  std::vector<int> hits;
  for (const auto& spec : specs) {
    if (!(spec.min > query_max || spec.max < query_min)) {
      hits.push_back(spec.id);
    }
  }
  return hits;
}

TEST(SortedPackedIntervalRTreeTest, EmptyTreeQueryVisitsNothing) {
  SortedPackedIntervalRTree tree;
  IdVisitor visitor;

  tree.query(0.0, 10.0, &visitor);
  tree.query(geos::DoubleNegInfinity, geos::DoubleInfinity, &visitor);

  EXPECT_THAT(visitor.ids(), IsEmpty());
}

TEST(SortedPackedIntervalRTreeTest, EmptyTreeRemainsOpenAfterQuery) {
  SortedPackedIntervalRTree tree(4);
  IdVisitor visitor;

  // Querying an empty tree must leave the index open for subsequent inserts.
  tree.query(-10.0, 10.0, &visitor);
  EXPECT_THAT(visitor.ids(), IsEmpty());

  int item1 = 101;
  int item2 = 202;
  int item3 = 303;
  tree.insert(1.0, 3.0, &item1);
  tree.insert(5.0, 7.0, &item2);

  tree.query(0.0, 4.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(101));

  // Once a non-empty tree has been queried, further inserts must fail.
  EXPECT_THROW(tree.insert(8.0, 9.0, &item3), UnsupportedOperationException);
}

TEST(SortedPackedIntervalRTreeTest, SingleItemTreeSupportsQueryAndLocksInsert) {
  SortedPackedIntervalRTree tree;
  int item = 7;
  int extra = 8;
  tree.insert(2.0, 4.0, &item);

  IdVisitor visitor;
  tree.query(0.0, 1.5, &visitor);
  EXPECT_THAT(visitor.ids(), IsEmpty());

  tree.query(4.5, 6.0, &visitor);
  EXPECT_THAT(visitor.ids(), IsEmpty());

  tree.query(2.0, 2.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(7));

  visitor.clear();
  tree.query(4.0, 4.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(7));

  try {
    tree.insert(10.0, 12.0, &extra);
    FAIL() << "Expected UnsupportedOperationException after querying";
  } catch (const UnsupportedOperationException& e) {
    EXPECT_FALSE(std::string(e.what()).empty());
  }

  // Tree remains queryable after rejected insert.
  visitor.clear();
  tree.query(3.0, 5.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(7));
}

TEST(SortedPackedIntervalRTreeTest, TwoItemTreeBuildsSingleBranchLevel) {
  SortedPackedIntervalRTree tree(2);
  int first = 1;
  int second = 2;
  // Inserted out of midpoint order so std::sort comparator is exercised.
  tree.insert(10.0, 12.0, &first);
  tree.insert(1.0, 3.0, &second);

  IdVisitor visitor;
  tree.query(4.0, 9.0, &visitor);
  EXPECT_THAT(visitor.ids(), IsEmpty());

  tree.query(0.0, 2.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(2));

  visitor.clear();
  tree.query(11.0, 15.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(1));

  visitor.clear();
  tree.query(0.0, 20.0, &visitor);
  EXPECT_THAT(visitor.ids(), UnorderedElementsAreArray({1, 2}));
}

TEST(SortedPackedIntervalRTreeTest, ThreeItemTreePromotesOddNodeAcrossLevels) {
  SortedPackedIntervalRTree tree;
  int a = 1;
  int b = 2;
  int c = 3;
  tree.insert(1.0, 2.0, &a);
  tree.insert(10.0, 12.0, &b);
  tree.insert(5.0, 7.0, &c);

  IdVisitor visitor;
  tree.query(0.0, 1.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(1));

  visitor.clear();
  tree.query(6.0, 6.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(3));

  visitor.clear();
  tree.query(12.0, 15.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(2));

  visitor.clear();
  tree.query(3.0, 4.0, &visitor);
  EXPECT_THAT(visitor.ids(), IsEmpty());

  visitor.clear();
  tree.query(1.5, 11.0, &visitor);
  EXPECT_THAT(visitor.ids(), UnorderedElementsAreArray({1, 2, 3}));
}

TEST(SortedPackedIntervalRTreeTest,
     ZeroInitialCapacityGrowsAndBuildsCorrectly) {
  SortedPackedIntervalRTree tree(0);
  std::array<int, 6> ids = {0, 1, 2, 3, 4, 5};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const double base = static_cast<double>(ids.size() - i) * 3.0;
    tree.insert(base, base + 1.0, &ids[i]);
  }

  IdVisitor visitor;
  tree.query(6.5, 12.5, &visitor);
  EXPECT_THAT(visitor.ids(), UnorderedElementsAreArray({2, 3, 4}));
}

TEST(SortedPackedIntervalRTreeTest, NullItemPointerIsPreserved) {
  SortedPackedIntervalRTree tree;
  int value = 1;
  tree.insert(1.0, 2.0, nullptr);
  tree.insert(5.0, 6.0, &value);

  PointerVisitor visitor;
  tree.query(1.5, 1.5, &visitor);
  EXPECT_THAT(visitor.ptrs(), ElementsAre(nullptr));
}

TEST(SortedPackedIntervalRTreeTest, DiverseIntervalsMatchBruteForceOracle) {
  constexpr auto kSpecs = std::to_array<IntervalSpec>({
      {.min = 25.0, .max = 30.0, .id = 1},
      {.min = -50.0, .max = -40.0, .id = 2},
      {.min = 7.0, .max = 7.0, .id = 3},
      {.min = 10.0, .max = 20.0, .id = 4},
      {.min = 14.0, .max = 16.0, .id = 5},
      {.min = 25.0, .max = 30.0, .id = 6},
      {.min = -5.0, .max = 5.0, .id = 7},
      {.min = 0.0, .max = 0.0, .id = 8},
      {.min = 1.0, .max = 2.0, .id = 9},
      {.min = 2.0, .max = 3.0, .id = 10},
      {.min = -1e6, .max = -999990.0, .id = 11},
      {.min = 1e6, .max = 1000010.0, .id = 12},
      {.min = 3.5, .max = 4.5, .id = 13},
      {.min = -20.0, .max = -15.0, .id = 14},
      {.min = 100.0, .max = 105.0, .id = 15},
  });

  SortedPackedIntervalRTree tree(4);
  std::vector<IntervalSpec> specs(kSpecs.begin(), kSpecs.end());
  for (auto& spec : specs) {
    tree.insert(spec.min, spec.max, &spec.id);
  }

  constexpr auto kQueries = std::to_array<QueryRange>({
      {.name = "below_all", .min = -2e6, .max = -1.5e6},
      {.name = "above_all", .min = 2e6, .max = 3e6},
      {.name = "gap_between_clusters", .min = 31.0, .max = 99.0},
      {.name = "point_at_zero", .min = 0.0, .max = 0.0},
      {.name = "point_at_seven", .min = 7.0, .max = 7.0},
      {.name = "shared_boundary_two", .min = 2.0, .max = 2.0},
      {.name = "equal_midpoint_nested", .min = 15.0, .max = 15.0},
      {.name = "duplicate_intervals", .min = 26.0, .max = 29.0},
      {.name = "negative_span", .min = -45.0, .max = -10.0},
      {.name = "spanning_all", .min = -1e7, .max = 1e7},
  });

  for (const auto& query_range : kQueries) {
    SCOPED_TRACE(query_range.name);
    IdVisitor visitor;
    tree.query(query_range.min, query_range.max, &visitor);
    EXPECT_THAT(visitor.ids(), UnorderedElementsAreArray(BruteForceQuery(
                                   specs, query_range.min, query_range.max)));
  }
}

TEST(SortedPackedIntervalRTreeTest, MultiLevelPackingMatchesAcrossSizes) {
  constexpr auto kSizes =
      std::to_array<std::size_t>({4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33});

  for (const std::size_t count : kSizes) {
    SCOPED_TRACE(count);
    SortedPackedIntervalRTree tree(count);
    std::vector<IntervalSpec> specs;
    specs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      // Interleave order so sorting is nontrivial at every size.
      const std::size_t perm = (i % 2 == 0) ? (count - 1 - i) : i;
      const double start = static_cast<double>(perm) * 2.0;
      specs.push_back(
          {.min = start, .max = start + 1.5, .id = static_cast<int>(perm)});
    }
    for (auto& spec : specs) {
      tree.insert(spec.min, spec.max, &spec.id);
    }

    const double max_coord = static_cast<double>(count) * 2.0 + 2.0;
    for (double q = -1.0; q <= max_coord; q += 1.25) {
      IdVisitor visitor;
      tree.query(q, q + 2.5, &visitor);
      EXPECT_THAT(visitor.ids(), UnorderedElementsAreArray(
                                     BruteForceQuery(specs, q, q + 2.5)));
    }
  }
}

TEST(SortedPackedIntervalRTreeTest, SingleSidedInfiniteIntervalsSortAndQuery) {
  SortedPackedIntervalRTree tree;
  int left = 1;
  int middle = 2;
  int right = 3;

  // Avoid [-inf, +inf] in a single leaf since (-inf) + (+inf) produces NaN.
  tree.insert(geos::DoubleNegInfinity, -10.0, &left);
  tree.insert(10.0, geos::DoubleInfinity, &right);
  tree.insert(-2.0, 2.0, &middle);

  IdVisitor visitor;
  tree.query(-5.0, 5.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(2));

  visitor.clear();
  tree.query(-20.0, -10.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(1));

  visitor.clear();
  tree.query(10.0, 20.0, &visitor);
  EXPECT_THAT(visitor.ids(), ElementsAre(3));

  visitor.clear();
  tree.query(geos::DoubleNegInfinity, geos::DoubleInfinity, &visitor);
  EXPECT_THAT(visitor.ids(), UnorderedElementsAreArray({1, 2, 3}));
}

}  // namespace
}  // namespace geos_google
