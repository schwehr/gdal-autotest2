// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests internal Sort-Tile-Recursive (STR) R-tree spatial index creation,
// spatial bounding-box search, deep copying, leaf lifecycle, and cleanup
// (third_party/hdf5/src/H5RT.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5RT.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5RTprivate.h
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5RTpkg.h

// clang-format off
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// clang-format on

#include <dirent.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#define H5RT_FRIEND
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5RTpkg.h"

__attribute__((weak)) void
MallocExtension_Internal_SetBackgroundProcessActionsEnabled(bool value);
__attribute__((weak)) void MallocExtension_Internal_SetProfileSamplingInterval(
    int64_t value);
}  // extern "C"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5RT_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5RT_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5RT_TEST_HAS_ASAN_OR_LSAN) &&                         \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5RT_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5RT_TEST_HAS_OTHER_SAN) &&                       \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5RT_TEST_HAS_OTHER_SAN 1
#endif

namespace hdf5 {
namespace {

using ::testing::UnorderedElementsAreArray;

struct H5RTDeleter {
  void operator()(H5RT_t* rtree) const {
    if (rtree != nullptr) {
      H5RT_free(rtree);
    }
  }
};

struct H5RTResultsDeleter {
  void operator()(H5RT_result_set_t* results) const {
    if (results != nullptr) {
      H5RT_free_results(results);
    }
  }
};

using ScopedRTree = std::unique_ptr<H5RT_t, H5RTDeleter>;
using ScopedRTResults = std::unique_ptr<H5RT_result_set_t, H5RTResultsDeleter>;

struct LeafSpec {
  std::vector<hsize_t> min;
  std::vector<hsize_t> max;
  void* record = nullptr;
};

ScopedRTree CreateTreeFromSpecs(int rank, std::span<const LeafSpec> specs) {
  if (specs.empty()) {
    return nullptr;
  }
  H5RT_leaf_t* leaves =
      static_cast<H5RT_leaf_t*>(std::calloc(specs.size(), sizeof(H5RT_leaf_t)));
  if (leaves == nullptr) {
    return nullptr;
  }
  size_t init_count = 0;
  bool ownership_transferred = false;
  absl::Cleanup cleanup_leaves = [&] {
    if (!ownership_transferred) {
      for (size_t i = 0; i < init_count; ++i) {
        H5RT_leaf_cleanup(&leaves[i]);
      }
      std::free(leaves);
    }
  };

  for (size_t i = 0; i < specs.size(); ++i) {
    if (H5RT_leaf_init(&leaves[i], rank, specs[i].record) < 0) {
      return nullptr;
    }
    ++init_count;
    for (int d = 0; d < rank; ++d) {
      leaves[i].min[d] = specs[i].min[d];
      leaves[i].max[d] = specs[i].max[d];
      leaves[i].mid[d] =
          specs[i].min[d] + (specs[i].max[d] - specs[i].min[d]) / 2;
    }
  }

  H5RT_t* raw_tree = H5RT_create(rank, leaves, specs.size());
  if (raw_tree == nullptr) {
    return nullptr;
  }
  ownership_transferred = true;
  return ScopedRTree(raw_tree);
}

std::vector<void*> SearchRecords(H5RT_t* rtree, std::vector<hsize_t> min,
                                 std::vector<hsize_t> max,
                                 size_t* out_capacity = nullptr) {
  H5RT_result_set_t* raw_results = nullptr;
  EXPECT_GE(H5RT_search(rtree, min.data(), max.data(), &raw_results), 0);
  ScopedRTResults results(raw_results);
  if (!results) {
    return {};
  }
  if (out_capacity != nullptr) {
    *out_capacity = results->capacity;
  }
  std::vector<void*> records;
  records.reserve(results->count);
  for (size_t i = 0; i < results->count; ++i) {
    records.push_back(results->results[i]->record);
  }
  return records;
}

void VerifyNodeInvariants(const H5RT_node_t* node, int rank,
                          const H5RT_leaf_t* leaves_begin,
                          const H5RT_leaf_t* leaves_end, size_t* leaf_counter) {
  ASSERT_NE(node, nullptr);
  EXPECT_GT(node->nchildren, 0);
  EXPECT_LE(node->nchildren, H5RT_MAX_NODE_SIZE);

  if (node->children_are_leaves) {
    ASSERT_GE(node->children.leaves, leaves_begin);
    ASSERT_LE(node->children.leaves + node->nchildren, leaves_end);
    for (int i = 0; i < node->nchildren; ++i) {
      const H5RT_leaf_t& leaf = node->children.leaves[i];
      for (int d = 0; d < rank; ++d) {
        EXPECT_LE(node->min[d], leaf.min[d]);
        EXPECT_GE(node->max[d], leaf.max[d]);
      }
      ++(*leaf_counter);
    }
  } else {
    for (int i = 0; i < node->nchildren; ++i) {
      const H5RT_node_t* child = node->children.nodes[i];
      ASSERT_NE(child, nullptr);
      for (int d = 0; d < rank; ++d) {
        EXPECT_LE(node->min[d], child->min[d]);
        EXPECT_GE(node->max[d], child->max[d]);
      }
      VerifyNodeInvariants(child, rank, leaves_begin, leaves_end, leaf_counter);
    }
  }
}

TEST(H5RTTest, LeafInitAndCleanupValidAndInvalidInputs) {
  ASSERT_GE(H5open(), 0);

  // 1. Validate invalid leaf arguments. Zero-initialize `leaf` so the error
  // cleanup path inside H5RT_leaf_init inspects null `_coords`.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5RT_leaf_init(nullptr, 1, nullptr), 0);
    EXPECT_LT(H5RT_leaf_cleanup(nullptr), 0);

    H5RT_leaf_t bad_leaf{};
    EXPECT_LT(H5RT_leaf_init(&bad_leaf, 0, nullptr), 0);
    EXPECT_LT(H5RT_leaf_init(&bad_leaf, -3, nullptr), 0);
    EXPECT_LT(H5RT_leaf_init(&bad_leaf, H5S_MAX_RANK + 1, nullptr), 0);
  }

  // 2. Initialize leaves across boundary and intermediate ranks, verify
  // contiguous non-overlapping min/max/mid coordinate slices, and clean up.
  int payload = 42;
  for (int rank : {1, 4, H5S_MAX_RANK}) {
    H5RT_leaf_t leaf{};
    ASSERT_GE(H5RT_leaf_init(&leaf, rank, &payload), 0);
    absl::Cleanup cleanup = [&leaf] { H5RT_leaf_cleanup(&leaf); };

    EXPECT_EQ(leaf.rank, rank);
    EXPECT_EQ(leaf.record, &payload);
    ASSERT_NE(leaf._coords, nullptr);
    EXPECT_EQ(leaf.min, leaf._coords);
    EXPECT_EQ(leaf.max, leaf._coords + rank);
    EXPECT_EQ(leaf.mid, leaf._coords + 2 * rank);

    // H5RT_leaf_init uses malloc() for `_coords`, so populate each slice before
    // reading back to confirm min/max/mid do not alias.
    for (int d = 0; d < rank; ++d) {
      leaf.min[d] = static_cast<hsize_t>(d + 1);
      leaf.max[d] = static_cast<hsize_t>((d + 1) * 10);
      leaf.mid[d] = static_cast<hsize_t>((d + 1) * 5);
    }
    for (int d = 0; d < rank; ++d) {
      EXPECT_EQ(leaf.min[d], static_cast<hsize_t>(d + 1));
      EXPECT_EQ(leaf.max[d], static_cast<hsize_t>((d + 1) * 10));
      EXPECT_EQ(leaf.mid[d], static_cast<hsize_t>((d + 1) * 5));
    }

    EXPECT_GE(H5RT_leaf_cleanup(&leaf), 0);
    EXPECT_EQ(leaf._coords, nullptr);
    EXPECT_EQ(leaf.min, nullptr);
    EXPECT_EQ(leaf.max, nullptr);
    EXPECT_EQ(leaf.mid, nullptr);

    // Cleaning up an already-cleaned leaf is a safe no-op.
    EXPECT_GE(H5RT_leaf_cleanup(&leaf), 0);
  }
}

TEST(H5RTTest, CreateFreeAndCopyRejectInvalidInputs) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  H5RT_leaf_t* dummy_leaf =
      static_cast<H5RT_leaf_t*>(std::calloc(1, sizeof(H5RT_leaf_t)));
  ASSERT_NE(dummy_leaf, nullptr);
  ASSERT_GE(H5RT_leaf_init(dummy_leaf, 1, nullptr), 0);
  absl::Cleanup cleanup_dummy = [&] {
    H5RT_leaf_cleanup(dummy_leaf);
    std::free(dummy_leaf);
  };

  EXPECT_EQ(H5RT_create(0, dummy_leaf, 1), nullptr);
  EXPECT_EQ(H5RT_create(-1, dummy_leaf, 1), nullptr);
  EXPECT_EQ(H5RT_create(H5S_MAX_RANK + 1, dummy_leaf, 1), nullptr);
  EXPECT_EQ(H5RT_create(1, nullptr, 0), nullptr);
  EXPECT_EQ(H5RT_create(1, dummy_leaf, 0), nullptr);

  EXPECT_LT(H5RT_free(nullptr), 0);
  EXPECT_EQ(H5RT_copy(nullptr), nullptr);
}

TEST(H5RTTest, SingleNodeTreeSearchAndDeepCopy) {
  ASSERT_GE(H5open(), 0);

  // 1. Minimal 1-leaf 2D tree.
  int rec0 = 100;
  std::vector<LeafSpec> single_spec = {
      {.min = {10, 20}, .max = {15, 25}, .record = &rec0},
  };
  ScopedRTree single_tree = CreateTreeFromSpecs(/*rank=*/2, single_spec);
  ASSERT_NE(single_tree, nullptr);
  EXPECT_EQ(single_tree->rank, 2);
  EXPECT_EQ(single_tree->nleaves, 1);
  EXPECT_TRUE(single_tree->root.children_are_leaves);
  EXPECT_EQ(single_tree->root.nchildren, 1);

  EXPECT_THAT(SearchRecords(single_tree.get(), {10, 20}, {15, 25}),
              UnorderedElementsAreArray({static_cast<void*>(&rec0)}));
  // Touching corner counts as intersecting (H5RT__leaves_intersect uses >).
  EXPECT_THAT(SearchRecords(single_tree.get(), {15, 25}, {30, 30}),
              UnorderedElementsAreArray({static_cast<void*>(&rec0)}));
  EXPECT_THAT(SearchRecords(single_tree.get(), {0, 0}, {9, 25}),
              UnorderedElementsAreArray(std::vector<void*>{}));
  EXPECT_THAT(SearchRecords(single_tree.get(), {10, 26}, {15, 30}),
              UnorderedElementsAreArray(std::vector<void*>{}));

  // 2. Maximum single-node capacity (H5RT_MAX_NODE_SIZE == 16) with
  // intentionally unsorted bounding boxes so root min/max shrink and expand.
  std::vector<int> payloads(H5RT_MAX_NODE_SIZE);
  std::vector<LeafSpec> specs;
  specs.reserve(H5RT_MAX_NODE_SIZE);
  for (int i = 0; i < H5RT_MAX_NODE_SIZE; ++i) {
    payloads[i] = i;
    const int idx = (i * 7 + 5) % H5RT_MAX_NODE_SIZE;
    const hsize_t base = static_cast<hsize_t>(idx * 10);
    specs.push_back({
        .min = {base + 1, base + 2},
        .max = {base + 5, base + 6},
        .record = &payloads[idx],
    });
  }

  ScopedRTree full_leaf_tree = CreateTreeFromSpecs(/*rank=*/2, specs);
  ASSERT_NE(full_leaf_tree, nullptr);
  EXPECT_TRUE(full_leaf_tree->root.children_are_leaves);
  EXPECT_EQ(full_leaf_tree->root.nchildren, H5RT_MAX_NODE_SIZE);

  size_t visited_leaves = 0;
  VerifyNodeInvariants(&full_leaf_tree->root, 2, full_leaf_tree->leaves,
                       full_leaf_tree->leaves + full_leaf_tree->nleaves,
                       &visited_leaves);
  EXPECT_EQ(visited_leaves, H5RT_MAX_NODE_SIZE);

  // Copy, verify deep-copy pointers, then destroy original before querying
  // copy.
  ScopedRTree copied_tree(H5RT_copy(full_leaf_tree.get()));
  ASSERT_NE(copied_tree, nullptr);
  EXPECT_NE(copied_tree->leaves, full_leaf_tree->leaves);
  EXPECT_TRUE(copied_tree->root.children_are_leaves);
  EXPECT_EQ(copied_tree->root.children.leaves, copied_tree->leaves);
  for (size_t i = 0; i < copied_tree->nleaves; ++i) {
    EXPECT_NE(copied_tree->leaves[i]._coords,
              full_leaf_tree->leaves[i]._coords);
    EXPECT_EQ(copied_tree->leaves[i].record, full_leaf_tree->leaves[i].record);
  }
  full_leaf_tree.reset();

  size_t capacity = 0;
  EXPECT_THAT(SearchRecords(copied_tree.get(), {20, 20}, {45, 46}, &capacity),
              UnorderedElementsAreArray({static_cast<void*>(&payloads[2]),
                                         static_cast<void*>(&payloads[3]),
                                         static_cast<void*>(&payloads[4])}));
  EXPECT_EQ(capacity, 32);
}

TEST(H5RTTest, MultiLevelBulkLoadSortAndPruningSearch) {
  ASSERT_GE(H5open(), 0);

  // Use 40 2D leaves (> H5RT_MAX_NODE_SIZE): slab_size=3, num_slabs=14, with
  // the last slab holding 1 leaf (< slab_size). Include duplicate midpoints
  // along dim 0 so H5RT__leaf_compare exercises <, >, and == branches.
  constexpr size_t kCount = 40;
  std::vector<int> payloads(kCount);
  std::vector<LeafSpec> specs;
  specs.reserve(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    payloads[i] = static_cast<int>(i);
    const size_t perm = (i * 17 + 11) % kCount;
    const hsize_t x = static_cast<hsize_t>((perm % 8) * 20);
    const hsize_t y = static_cast<hsize_t>((perm / 8) * 20);
    specs.push_back({
        .min = {x, y},
        .max = {x + 10, y + 10},
        .record = &payloads[perm],
    });
  }

  ScopedRTree tree = CreateTreeFromSpecs(/*rank=*/2, specs);
  ASSERT_NE(tree, nullptr);
  EXPECT_FALSE(tree->root.children_are_leaves);
  EXPECT_EQ(tree->root.nchildren, 14);

  size_t visited_leaves = 0;
  VerifyNodeInvariants(&tree->root, 2, tree->leaves,
                       tree->leaves + tree->nleaves, &visited_leaves);
  EXPECT_EQ(visited_leaves, kCount);

  // 1. Disjoint query in the gap between boxes prunes internal nodes and
  // leaves.
  EXPECT_TRUE(SearchRecords(tree.get(), {12, 12}, {18, 18}).empty());
  // Query outside global bounds.
  EXPECT_TRUE(SearchRecords(tree.get(), {500, 500}, {600, 600}).empty());

  // 2. Targeted query hitting one exact leaf while pruning other slabs.
  EXPECT_THAT(SearchRecords(tree.get(), {41, 21}, {49, 29}),
              UnorderedElementsAreArray({static_cast<void*>(&payloads[10])}));

  // 3. Query covering all 40 leaves grows result buffer beyond initial 32.
  size_t capacity = 0;
  std::vector<void*> expected_all;
  expected_all.reserve(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    expected_all.push_back(&payloads[i]);
  }
  EXPECT_THAT(SearchRecords(tree.get(), {0, 0}, {200, 200}, &capacity),
              UnorderedElementsAreArray(expected_all));
  EXPECT_GE(capacity, 64);
}

TEST(H5RTTest, ThreeLevelTreesCoverRepeatedDimensionAndDeepCopy) {
  ASSERT_GE(H5open(), 0);

  // 300 leaves (> 16*16 = 256) creates a 3-level tree.
  // With rank == 1, level-1 internal nodes have prev_sort_dim == rank - 1 (0),
  // exercising the `else { sort_dim = prev_sort_dim; }` branch in
  // H5RT__bulk_load, and full search grows results 32 -> 64 -> 128 -> 256 ->
  // 512.
  constexpr size_t kCount = 300;
  std::vector<int> payloads(kCount);
  std::vector<LeafSpec> specs_1d;
  specs_1d.reserve(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    payloads[i] = static_cast<int>(i);
    const size_t perm = (i * 73 + 19) % kCount;
    const hsize_t pos = static_cast<hsize_t>(perm * 10);
    specs_1d.push_back({
        .min = {pos},
        .max = {pos + 5},
        .record = &payloads[perm],
    });
  }

  ScopedRTree tree_1d = CreateTreeFromSpecs(/*rank=*/1, specs_1d);
  ASSERT_NE(tree_1d, nullptr);
  EXPECT_FALSE(tree_1d->root.children_are_leaves);
  ASSERT_GT(tree_1d->root.nchildren, 0);
  EXPECT_FALSE(tree_1d->root.children.nodes[0]->children_are_leaves);

  size_t visited_1d = 0;
  VerifyNodeInvariants(&tree_1d->root, 1, tree_1d->leaves,
                       tree_1d->leaves + tree_1d->nleaves, &visited_1d);
  EXPECT_EQ(visited_1d, kCount);

  // Deep copy the 3-level tree, drop the original, and verify search on copy.
  ScopedRTree copy_1d(H5RT_copy(tree_1d.get()));
  ASSERT_NE(copy_1d, nullptr);
  EXPECT_NE(copy_1d->root.children.nodes[0], tree_1d->root.children.nodes[0]);
  tree_1d.reset();

  size_t visited_copy = 0;
  VerifyNodeInvariants(&copy_1d->root, 1, copy_1d->leaves,
                       copy_1d->leaves + copy_1d->nleaves, &visited_copy);
  EXPECT_EQ(visited_copy, kCount);

  EXPECT_THAT(SearchRecords(copy_1d.get(), {1500}, {1525}),
              UnorderedElementsAreArray({static_cast<void*>(&payloads[150]),
                                         static_cast<void*>(&payloads[151]),
                                         static_cast<void*>(&payloads[152])}));
  EXPECT_TRUE(SearchRecords(copy_1d.get(), {1506}, {1509}).empty());

  size_t capacity = 0;
  std::vector<void*> all_results =
      SearchRecords(copy_1d.get(), {0}, {kCount * 10}, &capacity);
  EXPECT_EQ(all_results.size(), kCount);
  EXPECT_EQ(capacity, 512);
  std::unordered_set<void*> unique_ptrs(all_results.begin(), all_results.end());
  EXPECT_EQ(unique_ptrs.size(), kCount);

  // Also build a 300-leaf 3D tree so recursive levels advance sort_dim across
  // multiple dimensions (0 -> 1).
  std::vector<LeafSpec> specs_3d;
  specs_3d.reserve(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    const size_t perm = (i * 101 + 7) % kCount;
    const hsize_t x = static_cast<hsize_t>((perm % 10) * 10);
    const hsize_t y = static_cast<hsize_t>(((perm / 10) % 10) * 10);
    const hsize_t z = static_cast<hsize_t>((perm / 100) * 10);
    specs_3d.push_back({
        .min = {x, y, z},
        .max = {x + 4, y + 4, z + 4},
        .record = &payloads[perm],
    });
  }
  ScopedRTree tree_3d = CreateTreeFromSpecs(/*rank=*/3, specs_3d);
  ASSERT_NE(tree_3d, nullptr);
  size_t visited_3d = 0;
  VerifyNodeInvariants(&tree_3d->root, 3, tree_3d->leaves,
                       tree_3d->leaves + tree_3d->nleaves, &visited_3d);
  EXPECT_EQ(visited_3d, kCount);

  EXPECT_THAT(SearchRecords(tree_3d.get(), {20, 30, 10}, {24, 34, 14}),
              UnorderedElementsAreArray({static_cast<void*>(&payloads[132])}));
}

TEST(H5RTTest, MaxRankAndLargeCoordinatesIntersectAndSearch) {
  ASSERT_GE(H5open(), 0);

  constexpr int kRank = H5S_MAX_RANK;
  constexpr size_t kCount = 20;
  const hsize_t kBase = std::numeric_limits<hsize_t>::max() - 1000;

  std::vector<int> payloads(kCount);
  std::vector<LeafSpec> specs;
  specs.reserve(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    payloads[i] = static_cast<int>(i);
    std::vector<hsize_t> min_c(kRank, kBase);
    std::vector<hsize_t> max_c(kRank, kBase + 5);
    min_c[0] = kBase + static_cast<hsize_t>(i * 10);
    max_c[0] = min_c[0] + 5;
    min_c[kRank - 1] = kBase + static_cast<hsize_t>(i * 20);
    max_c[kRank - 1] = min_c[kRank - 1] + 5;
    specs.push_back({
        .min = std::move(min_c),
        .max = std::move(max_c),
        .record = &payloads[i],
    });
  }

  ScopedRTree tree = CreateTreeFromSpecs(kRank, specs);
  ASSERT_NE(tree, nullptr);

  // Match leaf 7 across all 32 dimensions.
  std::vector<hsize_t> qmin(kRank, kBase);
  std::vector<hsize_t> qmax(kRank, kBase + 5);
  qmin[0] = kBase + 70;
  qmax[0] = kBase + 75;
  qmin[kRank - 1] = kBase + 140;
  qmax[kRank - 1] = kBase + 145;
  EXPECT_THAT(SearchRecords(tree.get(), qmin, qmax),
              UnorderedElementsAreArray({static_cast<void*>(&payloads[7])}));

  // Differ only in the final dimension (kRank - 1) -> must miss.
  qmin[kRank - 1] = kBase + 146;
  qmax[kRank - 1] = kBase + 150;
  EXPECT_TRUE(SearchRecords(tree.get(), qmin, qmax).empty());

  // Direct H5RT__leaves_intersect boundary checks.
  std::vector<hsize_t> a_min = {0, 10, 20};
  std::vector<hsize_t> a_max = {5, 15, 25};
  std::vector<hsize_t> b_min = {5, 10, 25};
  std::vector<hsize_t> b_max = {9, 15, 30};
  EXPECT_TRUE(H5RT__leaves_intersect(3, a_min.data(), a_max.data(),
                                     b_min.data(), b_max.data()));
  b_min[2] = 26;
  EXPECT_FALSE(H5RT__leaves_intersect(3, a_min.data(), a_max.data(),
                                      b_min.data(), b_max.data()));
}

#if !defined(H5RT_TEST_HAS_ASAN_OR_LSAN) && !defined(H5RT_TEST_HAS_OTHER_SAN)
TEST(H5RTTest, OomErrorPathsCleanUpCleanly) {
  // Run in an isolated threadsafe death-test child process with RLIMIT_AS
  // tightened so allocations fail without affecting the parent process or
  // running under sanitizers. Note: H5RT_search(nullptr, ...) /
  // results_out==nullptr and multi-leaf H5RT_copy mid-loop _coords OOM are not
  // invoked because upstream H5RT.c has NULL-dereference / double-free bugs on
  // those paths.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    if (H5open() < 0) {
      std::exit(1);
    }

    // Stop TCMalloc's MemoryReleaser background thread and sampling, and wait
    // until InitGoogle's background threads have finished installing their
    // alternate signal stacks (which call mmap) before we cap RLIMIT_AS.
    if (&MallocExtension_Internal_SetProfileSamplingInterval != nullptr) {
      MallocExtension_Internal_SetProfileSamplingInterval(0);
    }
    if (&MallocExtension_Internal_SetBackgroundProcessActionsEnabled !=
        nullptr) {
      MallocExtension_Internal_SetBackgroundProcessActionsEnabled(false);
    }
    auto read_comm = [](const char* path) {
      std::ifstream in(path);
      std::string comm;
      std::getline(in, comm);
      return comm;
    };
    const std::string self_comm = read_comm("/proc/self/comm");
    const std::string self_tid = std::to_string(::getpid());
    for (int attempt = 0; attempt < 50; ++attempt) {
      bool pending_thread = false;
      DIR* task_dir = ::opendir("/proc/self/task");
      if (task_dir != nullptr) {
        absl::Cleanup close_dir = [&] { ::closedir(task_dir); };
        while (struct dirent* ent = ::readdir(task_dir)) {
          const std::string_view tid(ent->d_name);
          if (tid == "." || tid == ".." || tid == self_tid) {
            continue;
          }
          const std::string comm_path =
              "/proc/self/task/" + std::string(tid) + "/comm";
          if (!self_comm.empty() && read_comm(comm_path.c_str()) == self_comm) {
            pending_thread = true;
            break;
          }
        }
      }
      if (!pending_thread) {
        break;
      }
      struct timespec wait_ts = {.tv_sec = 0, .tv_nsec = 2'000'000};
      ::nanosleep(&wait_ts, nullptr);
    }
    struct timespec settle_ts = {.tv_sec = 0, .tv_nsec = 20'000'000};
    ::nanosleep(&settle_ts, nullptr);

    // Pin to one CPU so tcmalloc per-CPU caches stay local to this thread.
    const int cpu = ::sched_getcpu();
    if (cpu >= 0 && cpu < CPU_SETSIZE) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(cpu, &cpuset);
      static_cast<void>(::sched_setaffinity(0, sizeof(cpuset), &cpuset));
    }

    // Pre-warm H5E_DEFAULT so error-stack machinery does not allocate later.
    ScopedErrorSilence silence;
    H5RT_leaf_t warm_leaf{};
    if (H5RT_leaf_init(&warm_leaf, 0, nullptr) >= 0) {
      std::exit(21);
    }
    H5Eclear2(H5E_DEFAULT);

    // Build fixtures before tightening RLIMIT_AS.
    // 1) Large 1D tree (17000 leaves): new_leaves ~816 KB (> tcmalloc small
    // cache), and full search grows results to 256 KB.
    constexpr size_t kBigCount = 17000;
    std::vector<int> big_payloads(kBigCount, 1);
    std::vector<LeafSpec> big_specs;
    big_specs.reserve(kBigCount);
    for (size_t i = 0; i < kBigCount; ++i) {
      const hsize_t pos = static_cast<hsize_t>(i * 10);
      big_specs.push_back(
          {.min = {pos}, .max = {pos + 5}, .record = &big_payloads[i]});
    }
    ScopedRTree tree_large = CreateTreeFromSpecs(/*rank=*/1, big_specs);
    if (!tree_large) {
      std::exit(2);
    }

    // 2) Single-leaf rank-32 tree (_coords = 768 bytes) for H5RT_copy _coords
    // failure on nleaves == 1 (avoids upstream multi-leaf double-free).
    int rec1 = 7;
    std::vector<LeafSpec> specs_1_32d = {{
        .min = std::vector<hsize_t>(H5S_MAX_RANK, 1),
        .max = std::vector<hsize_t>(H5S_MAX_RANK, 2),
        .record = &rec1,
    }};
    ScopedRTree tree_1_32d = CreateTreeFromSpecs(H5S_MAX_RANK, specs_1_32d);
    if (!tree_1_32d) {
      std::exit(3);
    }

    // 3) 20-leaf 1D tree (2 levels, 12 internal children, 960B new_leaves).
    constexpr size_t kMedCount = 20;
    std::vector<int> med_payloads(kMedCount, 2);
    std::vector<LeafSpec> med_specs;
    med_specs.reserve(kMedCount);
    for (size_t i = 0; i < kMedCount; ++i) {
      const hsize_t pos = static_cast<hsize_t>(i * 10);
      med_specs.push_back(
          {.min = {pos}, .max = {pos + 5}, .record = &med_payloads[i]});
    }
    ScopedRTree tree_20 = CreateTreeFromSpecs(/*rank=*/1, med_specs);
    if (!tree_20) {
      std::exit(4);
    }

    // 4) Pre-initialized 17 leaves for H5RT_create OOM checks.
    constexpr size_t kCreateCount = 17;
    H5RT_leaf_t* create_leaves = static_cast<H5RT_leaf_t*>(
        std::calloc(kCreateCount, sizeof(H5RT_leaf_t)));
    if (create_leaves == nullptr) {
      std::exit(5);
    }
    for (size_t i = 0; i < kCreateCount; ++i) {
      if (H5RT_leaf_init(&create_leaves[i], 1, &med_payloads[i]) < 0) {
        std::exit(6);
      }
      create_leaves[i].min[0] = static_cast<hsize_t>(i * 10);
      create_leaves[i].max[0] = static_cast<hsize_t>(i * 10 + 5);
      create_leaves[i].mid[0] = static_cast<hsize_t>(i * 10 + 2);
    }

    // Flush all HDF5 free lists so H5FL_MALLOC cannot reuse cached blocks or
    // reclaim other free lists during OOM.
    if (H5FL_garbage_coll() < 0) {
      std::exit(7);
    }

    // Pre-warm small allocator size classes needed on non-failing sub-steps
    // (24B for H5RT_result_set_t / 1D _coords, 48B for 1-leaf new_leaves,
    // 960B for 20-leaf new_leaves, plus small H5E strings) before RLIMIT_AS.
    {
      std::vector<void*> warm;
      std::vector<hsize_t*> warm_coords;
      warm.reserve(128);
      warm_coords.reserve(64);
      for (int i = 0; i < 64; ++i) {
        warm.push_back(std::malloc(sizeof(H5RT_result_set_t)));
        warm.push_back(std::malloc(sizeof(H5RT_leaf_t)));
        warm.push_back(std::malloc(64));
        warm_coords.push_back(
            static_cast<hsize_t*>(std::malloc(3 * sizeof(hsize_t))));
      }
      for (int i = 0; i < 4; ++i) {
        warm.push_back(std::malloc(kMedCount * sizeof(H5RT_leaf_t)));
      }
      for (void* p : warm) {
        std::free(p);
      }
      for (hsize_t* p : warm_coords) {
        std::free(p);
      }
    }

    // Reserve drain vectors before setting RLIMIT_AS so push_back never
    // allocates while address space is constrained.
    std::vector<void*> drain_1m;
    std::vector<void*> drain_256k;
    std::vector<void*> drain_4k;
    std::vector<void*> drain_256;
    std::vector<hsize_t*> drain_coords_1m;
    std::vector<hsize_t*> drain_coords_256k;
    std::vector<hsize_t*> drain_coords_4k;
    std::vector<hsize_t*> drain_768;
    std::vector<void*> drain_node;
    std::vector<void*> drain_tree;
    drain_1m.reserve(4096);
    drain_256k.reserve(4096);
    drain_4k.reserve(8192);
    drain_256.reserve(8192);
    drain_coords_1m.reserve(4096);
    drain_coords_256k.reserve(4096);
    drain_coords_4k.reserve(8192);
    drain_768.reserve(8192);
    drain_node.reserve(8192);
    drain_tree.reserve(8192);

    std::ifstream statm("/proc/self/statm");
    unsigned long vm_pages = 0;
    if (!(statm >> vm_pages)) {
      std::exit(8);
    }
    const long page_sz = ::sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) {
      std::exit(9);
    }
    const rlim_t vm_bytes =
        static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

    struct rlimit old_lim{};
    if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
      std::exit(10);
    }
    struct rlimit tight_lim = old_lim;
    tight_lim.rlim_cur = std::min<rlim_t>(
        old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
    if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
      std::exit(11);
    }

    auto drain_size = [](size_t sz, std::vector<void*>& out,
                         bool zero_fill = false) {
      int consecutive_nulls = 0;
      while (out.size() < out.capacity() && consecutive_nulls < 4) {
        void* p = std::malloc(sz);
        if (p == nullptr) {
          ++consecutive_nulls;
          continue;
        }
        consecutive_nulls = 0;
        if (zero_fill) {
          std::memset(p, 0, sz);
        }
        out.push_back(p);
      }
    };

    auto drain_coords = [](size_t sz, std::vector<hsize_t*>& out) {
      int consecutive_nulls = 0;
      while (out.size() < out.capacity() && consecutive_nulls < 4) {
        hsize_t* p = static_cast<hsize_t*>(std::malloc(sz));
        if (p == nullptr) {
          ++consecutive_nulls;
          continue;
        }
        consecutive_nulls = 0;
        out.push_back(p);
      }
    };

    // Exhaust page heap and huge-region arenas in both the pointer-containing
    // partition (void*) and the scalar/pointer-free partition (hsize_t*).
    drain_size(1ULL << 20, drain_1m);
    drain_size(256ULL << 10, drain_256k);
    drain_size(4096, drain_4k);
    drain_coords(1ULL << 20, drain_coords_1m);
    drain_coords(256ULL << 10, drain_coords_256k);
    drain_coords(4096, drain_coords_4k);

    // Step 1: H5RT_copy(tree_large) fails on new_leaves (816 KB).
    if (H5RT_copy(tree_large.get()) != nullptr) {
      std::exit(12);
    }

    // Step 2: H5RT_search matching all 17000 leaves fails inside
    // H5RT__result_set_grow() while descending multi-level tree.
    std::array<hsize_t, 1> qmin_big = {0};
    std::array<hsize_t, 1> qmax_big = {kBigCount * 10};
    H5RT_result_set_t* res_big = nullptr;
    if (H5RT_search(tree_large.get(), qmin_big.data(), qmax_big.data(),
                    &res_big) >= 0 ||
        res_big != nullptr) {
      std::exit(13);
    }

    // Now exhaust small object caches for 256B, 768B, H5RT_node_t, and H5RT_t.
    drain_size(32 * sizeof(H5RT_leaf_t*), drain_256);
    drain_coords(3 * static_cast<size_t>(H5S_MAX_RANK) * sizeof(hsize_t),
                 drain_768);
    drain_size(sizeof(H5RT_node_t), drain_node, /*zero_fill=*/true);
    drain_size(sizeof(H5RT_t), drain_tree, /*zero_fill=*/true);

    // Step 3: H5RT__result_set_init() fails on 256B buffer allocation.
    std::array<hsize_t, H5S_MAX_RANK> qmin_32{};
    std::array<hsize_t, H5S_MAX_RANK> qmax_32{};
    qmin_32.fill(1);
    qmax_32.fill(2);
    H5RT_result_set_t* res_small = nullptr;
    if (H5RT_search(tree_1_32d.get(), qmin_32.data(), qmax_32.data(),
                    &res_small) >= 0 ||
        res_small != nullptr) {
      std::exit(14);
    }

    // Step 4: H5RT_leaf_init() fails on 768B _coords allocation.
    drain_size(4096, drain_4k);
    drain_coords(4096, drain_coords_4k);
    drain_coords(3 * static_cast<size_t>(H5S_MAX_RANK) * sizeof(hsize_t),
                 drain_768);
    H5RT_leaf_t oom_leaf{};
    if (H5RT_leaf_init(&oom_leaf, H5S_MAX_RANK, nullptr) >= 0) {
      std::exit(15);
    }

    // Step 5: H5RT_copy(tree_1_32d) fails on new_leaves[0]._coords (768B).
    drain_coords(3 * static_cast<size_t>(H5S_MAX_RANK) * sizeof(hsize_t),
                 drain_768);
    if (H5RT_copy(tree_1_32d.get()) != nullptr) {
      std::exit(16);
    }

    // Step 6: H5RT_copy(tree_20) fails on H5FL_MALLOC(H5RT_t) after allocating
    // all 20 leaves and their 24B coords, testing H5RT_copy leaf cleanup.
    drain_size(sizeof(H5RT_node_t), drain_node, /*zero_fill=*/true);
    drain_size(sizeof(H5RT_t), drain_tree, /*zero_fill=*/true);
    if (H5RT_copy(tree_20.get()) != nullptr) {
      std::exit(17);
    }

    // Step 7: H5RT_create fails on H5FL_MALLOC(H5RT_t) before taking ownership
    // of `create_leaves`.
    drain_size(sizeof(H5RT_node_t), drain_node, /*zero_fill=*/true);
    drain_size(sizeof(H5RT_t), drain_tree, /*zero_fill=*/true);
    if (H5RT_create(1, create_leaves, kCreateCount) != nullptr) {
      std::exit(18);
    }

    // Step 8: Allow 1 H5RT_t and 1 H5RT_node_t so H5RT_copy(tree_20) allocates
    // new_tree and child[0] then fails on child[1], testing H5RT__node_copy
    // and H5RT_copy partial-tree cleanup. Handle both shared and distinct
    // tcmalloc size classes for H5RT_t vs H5RT_node_t.
    auto pop_free = [](std::vector<void*>& vec) {
      if (!vec.empty()) {
        std::free(vec.back());
        vec.pop_back();
      }
    };
    const bool distinct_tree_class = !drain_tree.empty();
    if (distinct_tree_class) {
      pop_free(drain_tree);
      pop_free(drain_node);
    } else {
      pop_free(drain_node);
      pop_free(drain_node);
    }
    if (H5RT_copy(tree_20.get()) != nullptr) {
      std::exit(19);
    }
    if (H5FL_garbage_coll() < 0) {
      std::exit(22);
    }
    drain_size(sizeof(H5RT_node_t), drain_node, /*zero_fill=*/true);
    drain_size(sizeof(H5RT_t), drain_tree, /*zero_fill=*/true);

    // Step 9: Allow 1 H5RT_t and 1 H5RT_node_t so H5RT_create allocates rtree
    // and child[0] then fails on child[1], testing H5RT__bulk_load and
    // H5RT_create partial-tree cleanup (which takes ownership of and frees
    // `create_leaves`).
    if (distinct_tree_class || !drain_tree.empty()) {
      pop_free(drain_tree);
      pop_free(drain_node);
    } else {
      pop_free(drain_node);
      pop_free(drain_node);
    }
    if (H5RT_create(1, create_leaves, kCreateCount) != nullptr) {
      std::exit(20);
    }

    // Restore RLIMIT_AS and release all drained allocations cleanly.
    static_cast<void>(::setrlimit(RLIMIT_AS, &old_lim));
    for (auto* cvec :
         {&drain_768, &drain_coords_4k, &drain_coords_256k, &drain_coords_1m}) {
      for (hsize_t* p : *cvec) {
        std::free(p);
      }
    }
    for (auto* vec : {&drain_tree, &drain_node, &drain_256, &drain_4k,
                      &drain_256k, &drain_1m}) {
      for (void* p : *vec) {
        std::free(p);
      }
    }
    std::exit(0);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
