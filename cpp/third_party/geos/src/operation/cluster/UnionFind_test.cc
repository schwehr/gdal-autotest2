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

// Unit tests for third_party/geos/src/operation/cluster/UnionFind.cpp and
// include/geos/operation/cluster/UnionFind.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/cluster/UnionFind.cpp

#include "third_party/geos/include/geos/operation/cluster/UnionFind.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/operation/cluster/Clusters.h"

namespace geos_google {
namespace {

using geos::operation::cluster::Clusters;
using geos::operation::cluster::UnionFind;
using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

TEST(UnionFindTest, ZeroElements) {
  UnionFind uf(0);
  EXPECT_EQ(uf.getNumClusters(), 0);

  Clusters all = uf.getClusters();
  EXPECT_EQ(all.getNumClusters(), 0);
  EXPECT_TRUE(all.getClusterIds().empty());

  Clusters subset = uf.getClusters({});
  EXPECT_EQ(subset.getNumClusters(), 0);
  EXPECT_TRUE(subset.getClusterIds().empty());

  std::vector<std::size_t> empty;
  uf.sortByCluster(empty.begin(), empty.end());
  EXPECT_TRUE(empty.empty());
}

TEST(UnionFindTest, SingleElement) {
  UnionFind uf(1);
  EXPECT_EQ(uf.getNumClusters(), 1);
  EXPECT_EQ(uf.find(0), 0);
  EXPECT_TRUE(uf.same(0, 0));
  EXPECT_FALSE(uf.different(0, 0));

  uf.join(0, 0);
  EXPECT_EQ(uf.getNumClusters(), 1);
  EXPECT_EQ(uf.find(0), 0);

  Clusters clusters = uf.getClusters();
  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_EQ(clusters.getSize(0), 1);
  EXPECT_THAT(std::vector<std::size_t>(clusters.begin(0), clusters.end(0)),
              ElementsAre(0));
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0));
}

TEST(UnionFindTest, InitialStateAllDisjoint) {
  constexpr std::size_t kCount = 5;
  UnionFind uf(kCount);
  EXPECT_EQ(uf.getNumClusters(), kCount);

  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(uf.find(i), i);
    for (std::size_t j = 0; j < kCount; ++j) {
      EXPECT_EQ(uf.same(i, j), i == j);
      EXPECT_EQ(uf.different(i, j), i != j);
    }
  }
}

TEST(UnionFindTest, JoinMergesClustersAndIsIdempotent) {
  UnionFind uf(6);
  EXPECT_EQ(uf.getNumClusters(), 6);

  uf.join(0, 1);
  EXPECT_EQ(uf.getNumClusters(), 5);
  EXPECT_TRUE(uf.same(0, 1));
  EXPECT_FALSE(uf.different(0, 1));
  EXPECT_EQ(uf.find(0), uf.find(1));

  uf.join(1, 2);
  EXPECT_EQ(uf.getNumClusters(), 4);
  EXPECT_TRUE(uf.same(0, 2));

  // Redundant join within the same cluster must not change cluster count.
  uf.join(0, 2);
  uf.join(2, 0);
  uf.join(1, 1);
  EXPECT_EQ(uf.getNumClusters(), 4);

  uf.join(3, 4);
  EXPECT_EQ(uf.getNumClusters(), 3);
  EXPECT_TRUE(uf.different(0, 3));
  EXPECT_TRUE(uf.different(0, 5));

  uf.join(0, 4);
  EXPECT_EQ(uf.getNumClusters(), 2);
  for (std::size_t i = 0; i <= 4; ++i) {
    EXPECT_TRUE(uf.same(0, i));
    EXPECT_TRUE(uf.different(i, 5));
    EXPECT_EQ(uf.find(uf.find(i)), uf.find(i));
  }
}

TEST(UnionFindTest, JoinCoversAllSizeAndIndexTieBreakBranches) {
  UnionFind uf(6);

  // Equal sizes with a < b.
  uf.join(0, 1);
  // Equal sizes with a > b.
  uf.join(3, 2);
  EXPECT_EQ(uf.getNumClusters(), 4);

  // Unequal sizes: smaller first, larger second (sizes[b] > sizes[a]).
  uf.join(4, 0);
  EXPECT_EQ(uf.getNumClusters(), 3);
  EXPECT_TRUE(uf.same(4, 1));

  // Unequal sizes: larger first, smaller second (sizes[a] > sizes[b]).
  uf.join(2, 5);
  EXPECT_EQ(uf.getNumClusters(), 2);
  EXPECT_TRUE(uf.same(3, 5));
  EXPECT_TRUE(uf.different(0, 2));

  // Equal sizes after merging ({0,1,4} size 3 and {2,3,5} size 3).
  uf.join(1, 3);
  EXPECT_EQ(uf.getNumClusters(), 1);
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_TRUE(uf.same(0, i));
  }
}

TEST(UnionFindTest, PathCompressionAcrossDeepChains) {
  constexpr std::size_t kCount = 16;
  UnionFind forward(kCount);
  UnionFind reverse(kCount);

  for (std::size_t i = 0; i + 1 < kCount; ++i) {
    forward.join(i, i + 1);
    reverse.join(kCount - 1 - i, kCount - 2 - i);
  }

  EXPECT_EQ(forward.getNumClusters(), 1);
  EXPECT_EQ(reverse.getNumClusters(), 1);

  const std::size_t forward_root = forward.find(0);
  const std::size_t reverse_root = reverse.find(kCount - 1);
  EXPECT_LT(forward_root, kCount);
  EXPECT_LT(reverse_root, kCount);

  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(forward.find(i), forward_root);
    EXPECT_EQ(reverse.find(i), reverse_root);
    // Second call verifies path-compressed lookups remain consistent.
    EXPECT_EQ(forward.find(i), forward_root);
    EXPECT_EQ(reverse.find(i), reverse_root);
  }
}

TEST(UnionFindTest, SortByClusterIsStableWithinEachCluster) {
  UnionFind uf(6);
  // Clusters: {0, 3, 5}, {1, 4}, {2}.
  uf.join(0, 3);
  uf.join(3, 5);
  uf.join(1, 4);

  std::vector<std::size_t> elems = {5, 4, 2, 3, 1, 0};
  uf.sortByCluster(elems.begin(), elems.end());

  EXPECT_THAT(elems, UnorderedElementsAre(0, 1, 2, 3, 4, 5));

  for (std::size_t i = 0; i + 1 < elems.size(); ++i) {
    EXPECT_LE(uf.find(elems[i]), uf.find(elems[i + 1]));
  }

  auto index_of = [&elems](std::size_t value) {
    return std::distance(elems.begin(),
                         std::find(elems.begin(), elems.end(), value));
  };

  // Relative order inside {5, 3, 0} and {4, 1} must be preserved (stable_sort).
  EXPECT_EQ(index_of(3), index_of(5) + 1);
  EXPECT_EQ(index_of(0), index_of(3) + 1);
  EXPECT_EQ(index_of(1), index_of(4) + 1);

  // Also verify sorting a subrange leaves bounding elements untouched.
  std::vector<std::size_t> subrange = {99, 5, 4, 0, 1, 88};
  uf.sortByCluster(subrange.begin() + 1, subrange.end() - 1);
  EXPECT_EQ(subrange.front(), 99);
  EXPECT_EQ(subrange.back(), 88);
  EXPECT_TRUE(uf.same(subrange[1], subrange[2]));
  EXPECT_TRUE(uf.same(subrange[3], subrange[4]));
  EXPECT_LE(uf.find(subrange[1]), uf.find(subrange[3]));
}

TEST(UnionFindTest, GetClustersAllElements) {
  UnionFind uf(6);
  // Clusters: {0, 2, 4}, {1, 5}, {3}.
  uf.join(0, 2);
  uf.join(2, 4);
  uf.join(1, 5);

  Clusters clusters = uf.getClusters();
  ASSERT_EQ(clusters.getNumClusters(), 3);

  std::vector<std::vector<std::size_t>> grouped;
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    std::vector<std::size_t> members(clusters.begin(c), clusters.end(c));
    EXPECT_EQ(members.size(), clusters.getSize(c));
    for (std::size_t m : members) {
      EXPECT_TRUE(uf.same(members.front(), m));
    }
    grouped.push_back(std::move(members));
  }

  EXPECT_THAT(grouped, UnorderedElementsAre(ElementsAre(0, 2, 4),
                                            ElementsAre(1, 5), ElementsAre(3)));

  const std::vector<std::size_t> ids = clusters.getClusterIds();
  ASSERT_EQ(ids.size(), 6);
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    for (auto it = clusters.begin(c); it != clusters.end(c); ++it) {
      EXPECT_EQ(ids[*it], c);
    }
  }
  EXPECT_EQ(ids[0], ids[2]);
  EXPECT_EQ(ids[2], ids[4]);
  EXPECT_EQ(ids[1], ids[5]);
  EXPECT_NE(ids[0], ids[1]);
  EXPECT_NE(ids[0], ids[3]);
  EXPECT_NE(ids[1], ids[3]);
}

TEST(UnionFindTest, GetClustersSubsetAndCustomOrder) {
  UnionFind uf(6);
  // Clusters: {0, 2, 4}, {1, 5}, {3}.
  uf.join(0, 2);
  uf.join(2, 4);
  uf.join(1, 5);

  // Elements 2 and 3 are omitted; element order within each cluster should be
  // preserved from {4, 5, 0, 1}.
  Clusters clusters = uf.getClusters({4, 5, 0, 1});
  ASSERT_EQ(clusters.getNumClusters(), 2);

  std::vector<std::vector<std::size_t>> grouped;
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    std::vector<std::size_t> members(clusters.begin(c), clusters.end(c));
    EXPECT_EQ(members.size(), clusters.getSize(c));
    grouped.push_back(std::move(members));
  }

  EXPECT_THAT(grouped,
              UnorderedElementsAre(ElementsAre(4, 0), ElementsAre(5, 1)));

  constexpr std::size_t kNoCluster = std::numeric_limits<std::size_t>::max();
  const std::vector<std::size_t> default_ids = clusters.getClusterIds();
  ASSERT_EQ(default_ids.size(), 6);
  EXPECT_EQ(default_ids[2], kNoCluster);
  EXPECT_EQ(default_ids[3], kNoCluster);
  EXPECT_EQ(default_ids[4], default_ids[0]);
  EXPECT_EQ(default_ids[5], default_ids[1]);
  EXPECT_NE(default_ids[0], default_ids[1]);
  EXPECT_LT(default_ids[0], 2);
  EXPECT_LT(default_ids[1], 2);

  constexpr std::size_t kCustomSentinel = 999;
  const std::vector<std::size_t> custom_ids =
      clusters.getClusterIds(kCustomSentinel);
  ASSERT_EQ(custom_ids.size(), 6);
  EXPECT_EQ(custom_ids[2], kCustomSentinel);
  EXPECT_EQ(custom_ids[3], kCustomSentinel);
  EXPECT_EQ(custom_ids[0], default_ids[0]);
  EXPECT_EQ(custom_ids[1], default_ids[1]);
}

TEST(UnionFindTest, GetClustersEmptySubsetOnNonEmptyUnionFind) {
  UnionFind uf(4);
  uf.join(0, 1);

  Clusters empty_subset = uf.getClusters({});
  EXPECT_EQ(empty_subset.getNumClusters(), 0);

  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  EXPECT_THAT(empty_subset.getClusterIds(),
              ElementsAre(kMax, kMax, kMax, kMax));
  EXPECT_THAT(empty_subset.getClusterIds(42), ElementsAre(42, 42, 42, 42));
}

}  // namespace
}  // namespace geos_google
