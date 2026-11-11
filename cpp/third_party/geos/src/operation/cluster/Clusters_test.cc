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

// Unit tests for third_party/geos/src/operation/cluster/Clusters.cpp and
// third_party/geos/include/geos/operation/cluster/Clusters.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/cluster/Clusters.cpp

#include "third_party/geos/include/geos/operation/cluster/Clusters.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/operation/cluster/UnionFind.h"

namespace geos_google {
namespace {

using geos::operation::cluster::Clusters;
using geos::operation::cluster::UnionFind;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

std::vector<std::size_t> ClusterElements(const Clusters& clusters,
                                         std::size_t cluster_idx) {
  return std::vector<std::size_t>(clusters.begin(cluster_idx),
                                  clusters.end(cluster_idx));
}

std::vector<std::size_t> SortedClusterElements(const Clusters& clusters,
                                               std::size_t cluster_idx) {
  std::vector<std::size_t> elems = ClusterElements(clusters, cluster_idx);
  std::sort(elems.begin(), elems.end());
  return elems;
}

TEST(ClustersTest, EmptyElementsZeroNumElems) {
  UnionFind uf(0);
  const Clusters clusters(uf, {}, 0);

  EXPECT_EQ(clusters.getNumClusters(), 0);
  EXPECT_THAT(clusters.getClusterIds(), IsEmpty());
  EXPECT_THAT(clusters.getClusterIds(42), IsEmpty());
}

TEST(ClustersTest, EmptyElementsWithNonZeroNumElemsUsesSentinel) {
  UnionFind uf(4);
  const Clusters clusters(uf, {}, 4);

  EXPECT_EQ(clusters.getNumClusters(), 0);
  constexpr std::size_t kDefaultSentinel =
      std::numeric_limits<std::size_t>::max();
  EXPECT_THAT(clusters.getClusterIds(),
              ElementsAre(kDefaultSentinel, kDefaultSentinel, kDefaultSentinel,
                          kDefaultSentinel));
  EXPECT_THAT(clusters.getClusterIds(999), ElementsAre(999, 999, 999, 999));
}

TEST(ClustersTest, SingleElementFormsOneCluster) {
  UnionFind uf(1);
  const Clusters clusters(uf, {0}, 1);

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_EQ(clusters.getSize(0), 1);
  EXPECT_THAT(ClusterElements(clusters, 0), ElementsAre(0));
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0));
}

TEST(ClustersTest, AllElementsInSingleClusterPreserveInputOrder) {
  UnionFind uf(4);
  uf.join(0, 1);
  uf.join(2, 3);
  uf.join(1, 2);

  const Clusters clusters(uf, {3, 0, 2, 1}, 4);

  ASSERT_EQ(clusters.getNumClusters(), 1);
  EXPECT_EQ(clusters.getSize(0), 4);
  EXPECT_THAT(ClusterElements(clusters, 0), ElementsAre(3, 0, 2, 1));
  EXPECT_THAT(clusters.getClusterIds(), ElementsAre(0, 0, 0, 0));
}

TEST(ClustersTest, MultipleClustersPartitionElementsAndMapIds) {
  UnionFind uf(6);
  uf.join(0, 4);
  uf.join(1, 3);
  uf.join(3, 5);
  // Element 2 remains an isolated singleton cluster.

  const Clusters clusters(uf, {5, 2, 4, 1, 0, 3}, 6);

  ASSERT_EQ(clusters.getNumClusters(), 3);
  const std::vector<std::size_t> ids = clusters.getClusterIds();
  ASSERT_EQ(ids.size(), 6);

  std::vector<std::vector<std::size_t>> grouped;
  std::size_t total_size = 0;
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    const std::vector<std::size_t> elems = SortedClusterElements(clusters, c);
    EXPECT_EQ(clusters.getSize(c), elems.size());
    total_size += elems.size();
    for (std::size_t elem : elems) {
      EXPECT_EQ(ids[elem], c);
    }
    grouped.push_back(elems);
  }
  EXPECT_EQ(total_size, 6);
  EXPECT_THAT(grouped,
              UnorderedElementsAre(ElementsAre(0, 4), ElementsAre(1, 3, 5),
                                   ElementsAre(2)));
}

TEST(ClustersTest, SubsetOfElementsLeavesExcludedAsNoCluster) {
  UnionFind uf(7);
  uf.join(1, 4);
  uf.join(3, 6);

  // Elements 0, 2, and 5 are excluded (representing unclustered noise).
  const Clusters clusters(uf, {6, 4, 1, 3}, 7);

  ASSERT_EQ(clusters.getNumClusters(), 2);

  std::vector<std::vector<std::size_t>> grouped;
  for (std::size_t c = 0; c < clusters.getNumClusters(); ++c) {
    const std::vector<std::size_t> elems = SortedClusterElements(clusters, c);
    EXPECT_EQ(clusters.getSize(c), elems.size());
    grouped.push_back(elems);
  }
  EXPECT_THAT(grouped,
              UnorderedElementsAre(ElementsAre(1, 4), ElementsAre(3, 6)));

  const std::vector<std::size_t> custom_ids = clusters.getClusterIds(77);
  ASSERT_EQ(custom_ids.size(), 7);
  EXPECT_EQ(custom_ids[0], 77);
  EXPECT_EQ(custom_ids[2], 77);
  EXPECT_EQ(custom_ids[5], 77);
  EXPECT_EQ(custom_ids[1], custom_ids[4]);
  EXPECT_EQ(custom_ids[3], custom_ids[6]);
  EXPECT_NE(custom_ids[1], custom_ids[3]);
  EXPECT_LT(custom_ids[1], 2);
  EXPECT_LT(custom_ids[3], 2);

  constexpr std::size_t kDefaultSentinel =
      std::numeric_limits<std::size_t>::max();
  const std::vector<std::size_t> default_ids = clusters.getClusterIds();
  EXPECT_EQ(default_ids[0], kDefaultSentinel);
  EXPECT_EQ(default_ids[2], kDefaultSentinel);
  EXPECT_EQ(default_ids[5], kDefaultSentinel);
}

TEST(ClustersTest, BuiltViaUnionFindGetClusters) {
  UnionFind uf(4);
  uf.join(0, 2);

  const Clusters all = uf.getClusters();
  ASSERT_EQ(all.getNumClusters(), 3);
  const std::vector<std::size_t> all_ids = all.getClusterIds();
  EXPECT_EQ(all_ids[0], all_ids[2]);
  EXPECT_NE(all_ids[0], all_ids[1]);
  EXPECT_NE(all_ids[0], all_ids[3]);
  EXPECT_NE(all_ids[1], all_ids[3]);

  const Clusters subset = uf.getClusters({2, 0, 3});
  ASSERT_EQ(subset.getNumClusters(), 2);
  const std::vector<std::size_t> subset_ids = subset.getClusterIds(100);
  EXPECT_EQ(subset_ids[1], 100);
  EXPECT_EQ(subset_ids[0], subset_ids[2]);
  EXPECT_NE(subset_ids[0], subset_ids[3]);
}

}  // namespace
}  // namespace geos_google
