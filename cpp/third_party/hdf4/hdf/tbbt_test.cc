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
// Tests threaded, balanced, binary tree routines (hdf/src/tbbt.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/tbbt.c

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/tbbt_priv.h"

extern "C" {
void tbbt_printNode(TBBT_NODE* node, void (*key_dump)(void*, void*));
void tbbt_dumpNode(TBBT_NODE* node, void (*key_dump)(void*, void*), int method);
void tbbt_dump(TBBT_TREE* ptree, void (*key_dump)(void*, void*), int method);
}

namespace hdf4 {
namespace {

using ::testing::ElementsAre;

int IntCompare(void* k1, void* k2, int /*cmparg*/) {
  const int v1 = *static_cast<const int*>(k1);
  const int v2 = *static_cast<const int*>(k2);
  if (v1 < v2) return -1;
  if (v1 > v2) return 1;
  return 0;
}

class TbbtTest : public ::testing::Test {
 protected:
  void TearDown() override {
    // tbbt_shutdown frees the process-global tbbt_free_list.
    tbbt_shutdown();
  }
};

TEST_F(TbbtTest, TbbtdmakeAndCountOperations) {
  EXPECT_EQ(-1, tbbtcount(nullptr));

  TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  EXPECT_EQ(0, tbbtcount(tree));
}

TEST_F(TbbtTest, InsertFindAndDuplicateRejection) {
  TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  int k1 = 10, v1 = 100;
  int k2 = 5, v2 = 50;
  int k3 = 20, v3 = 200;

  TBBT_NODE* n1 = tbbtdins(tree, &v1, &k1);
  ASSERT_NE(nullptr, n1);
  EXPECT_EQ(&v1, n1->data);
  EXPECT_EQ(&k1, n1->key);
  EXPECT_EQ(1, tbbtcount(tree));

  TBBT_NODE* n2 = tbbtdins(tree, &v2, &k2);
  ASSERT_NE(nullptr, n2);
  EXPECT_EQ(2, tbbtcount(tree));

  TBBT_NODE* n3 = tbbtdins(tree, &v3, &k3);
  ASSERT_NE(nullptr, n3);
  EXPECT_EQ(3, tbbtcount(tree));

  // Duplicate key insertion is rejected (returns nullptr).
  int v_dup = 999;
  EXPECT_EQ(nullptr, tbbtdins(tree, &v_dup, &k1));
  EXPECT_EQ(3, tbbtcount(tree));

  // Find operations.
  TBBT_NODE* parent = nullptr;
  EXPECT_EQ(n2, tbbtdfind(tree, &k2, &parent));
  EXPECT_EQ(n1, parent);

  // Missing key lookup.
  int missing_key = 15;
  TBBT_NODE* missing_parent = nullptr;
  EXPECT_EQ(nullptr, tbbtdfind(tree, &missing_key, &missing_parent));
  EXPECT_NE(nullptr, missing_parent);

  // tbbtfind directly on root node.
  EXPECT_EQ(n3, tbbtfind(tree->root, &k3, IntCompare, sizeof(int), nullptr));
  EXPECT_EQ(nullptr, tbbtfind(tree->root, &missing_key, IntCompare, sizeof(int),
                              nullptr));
  EXPECT_EQ(nullptr, tbbtdfind(nullptr, &k1, nullptr));
}

TEST_F(TbbtTest, FastCompareUint16AndInt32) {
  auto uint16_cmp = [](void* k1, void* k2, int /*cmparg*/) -> int {
    const uint16_t v1 = *static_cast<const uint16_t*>(k1);
    const uint16_t v2 = *static_cast<const uint16_t*>(k2);
    if (v1 < v2) return -1;
    if (v1 > v2) return 1;
    return 0;
  };

  // 1. TBBT_FAST_UINT16_COMPARE
  {
    TBBT_TREE* tree =
        tbbtdmake(uint16_cmp, sizeof(uint16_t), TBBT_FAST_UINT16_COMPARE);
    ASSERT_NE(nullptr, tree);
    absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

    uint16_t k1 = 100, k2 = 50, k3 = 200;
    int v1 = 1, v2 = 2, v3 = 3;

    ASSERT_NE(nullptr, tbbtdins(tree, &v1, &k1));
    ASSERT_NE(nullptr, tbbtdins(tree, &v2, &k2));
    ASSERT_NE(nullptr, tbbtdins(tree, &v3, &k3));

    TBBT_NODE* parent = nullptr;
    TBBT_NODE* found = tbbtdfind(tree, &k2, &parent);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(&v2, found->data);

    uint16_t missing = 75;
    EXPECT_EQ(nullptr, tbbtdfind(tree, &missing, &parent));
  }

  // 2. TBBT_FAST_INT32_COMPARE
  {
    TBBT_TREE* tree =
        tbbtdmake(IntCompare, sizeof(int32_t), TBBT_FAST_INT32_COMPARE);
    ASSERT_NE(nullptr, tree);
    absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

    int32_t k1 = 1000, k2 = 500, k3 = 2000;
    int v1 = 1, v2 = 2, v3 = 3;

    ASSERT_NE(nullptr, tbbtdins(tree, &v1, &k1));
    ASSERT_NE(nullptr, tbbtdins(tree, &v2, &k2));
    ASSERT_NE(nullptr, tbbtdins(tree, &v3, &k3));

    TBBT_NODE* parent = nullptr;
    TBBT_NODE* found = tbbtdfind(tree, &k3, &parent);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(&v3, found->data);

    int32_t missing = 1500;
    EXPECT_EQ(nullptr, tbbtdfind(tree, &missing, &parent));
  }

  // 3. Unknown fast_compare returns nullptr on search.
  {
    TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int32_t), 999);
    ASSERT_NE(nullptr, tree);
    absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

    int32_t k = 10;
    int v = 1;
    ASSERT_NE(nullptr, tbbtdins(tree, &v, &k));
    EXPECT_EQ(nullptr, tbbtdfind(tree, &k, nullptr));
  }
}

TEST_F(TbbtTest, LessSearchFindsLessOrEqualNode) {
  TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  int k10 = 10, k20 = 20, k30 = 30;
  int v10 = 10, v20 = 20, v30 = 30;

  tbbtdins(tree, &v10, &k10);
  tbbtdins(tree, &v20, &k20);
  tbbtdins(tree, &v30, &k30);

  // Exact match returns node.
  TBBT_NODE* found = tbbtdless(tree, &k20, nullptr);
  ASSERT_NE(nullptr, found);
  EXPECT_EQ(&v20, found->data);

  // Note: Upstream quirk in tbbtless (tbbt.c:280):
  // When searching up the parent chain for a non-exact match, it checks
  // `if (cmp < 0)` instead of `if (cmp > 0)`. Thus searching for 5 (less than
  // all keys) ascends to parent node 20 and stops because 5 < 20.
  int target_5 = 5;
  TBBT_NODE* less_5 = tbbtdless(tree, &target_5, nullptr);
  ASSERT_NE(nullptr, less_5);
  EXPECT_EQ(&v20, less_5->data);

  // Null tree returns nullptr.
  int target_25 = 25;
  EXPECT_EQ(nullptr, tbbtdless(nullptr, &target_25, nullptr));
}

TEST_F(TbbtTest, IndxLookupReturnsOrderedElements) {
  TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  std::vector<int> keys = {40, 20, 60, 10, 30, 50, 70};
  std::vector<int> vals = {40, 20, 60, 10, 30, 50, 70};
  for (size_t i = 0; i < keys.size(); ++i) {
    tbbtdins(tree, &vals[i], &keys[i]);
  }

  // tbbtindx uses 1-based indexing.
  EXPECT_EQ(nullptr, tbbtindx(tree->root, 0));
  EXPECT_EQ(nullptr, tbbtindx(tree->root, -1));
  EXPECT_EQ(nullptr, tbbtindx(nullptr, 1));

  std::vector<int> sorted_keys;
  for (int32_t i = 1; i <= 7; ++i) {
    TBBT_NODE* node = tbbtindx(tree->root, i);
    ASSERT_NE(nullptr, node);
    sorted_keys.push_back(*static_cast<int*>(node->key));
  }
  EXPECT_THAT(sorted_keys, ElementsAre(10, 20, 30, 40, 50, 60, 70));

  // Out of bounds index.
  EXPECT_EQ(nullptr, tbbtindx(tree->root, 8));
}

TEST_F(TbbtTest, TraversalsFirstLastNextPrev) {
  EXPECT_EQ(nullptr, tbbtfirst(nullptr));
  EXPECT_EQ(nullptr, tbbtlast(nullptr));

  TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  std::vector<int> keys = {30, 10, 50, 20, 40};
  std::vector<int> vals = {30, 10, 50, 20, 40};
  for (size_t i = 0; i < keys.size(); ++i) {
    tbbtdins(tree, &vals[i], &keys[i]);
  }

  // Forward traversal with tbbtfirst and tbbtnext.
  std::vector<int> forward_order;
  for (TBBT_NODE* curr = tbbtfirst(tree->root); curr != nullptr;
       curr = tbbtnext(curr)) {
    forward_order.push_back(*static_cast<int*>(curr->key));
  }
  EXPECT_THAT(forward_order, ElementsAre(10, 20, 30, 40, 50));

  // Backward traversal with tbbtlast and tbbtprev.
  std::vector<int> backward_order;
  for (TBBT_NODE* curr = tbbtlast(tree->root); curr != nullptr;
       curr = tbbtprev(curr)) {
    backward_order.push_back(*static_cast<int*>(curr->key));
  }
  EXPECT_THAT(backward_order, ElementsAre(50, 40, 30, 20, 10));
}

TEST_F(TbbtTest, RotationsAndTreeBalancing) {
  // 1. Ascending insertions trigger left rotations.
  {
    TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
    ASSERT_NE(nullptr, tree);
    absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

    std::vector<int> keys(15);
    for (int i = 0; i < 15; ++i) {
      keys[i] = i + 1;
      ASSERT_NE(nullptr, tbbtdins(tree, &keys[i], &keys[i]));
    }
    EXPECT_EQ(15, tbbtcount(tree));
  }

  // 2. Descending insertions trigger right rotations.
  {
    TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
    ASSERT_NE(nullptr, tree);
    absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

    std::vector<int> keys(15);
    for (int i = 0; i < 15; ++i) {
      keys[i] = 15 - i;
      ASSERT_NE(nullptr, tbbtdins(tree, &keys[i], &keys[i]));
    }
    EXPECT_EQ(15, tbbtcount(tree));
  }

  // 3. Zigzag insertions trigger double rotations.
  {
    TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
    ASSERT_NE(nullptr, tree);
    absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

    std::vector<int> keys = {10, 30, 20, 5, 2, 4, 25, 28};
    for (int& k : keys) {
      ASSERT_NE(nullptr, tbbtdins(tree, &k, &k));
    }
    EXPECT_EQ(keys.size(), tbbtcount(tree));
  }
}

TEST_F(TbbtTest, NodeRemovalOperations) {
  TBBT_TREE* tree = tbbtdmake(IntCompare, sizeof(int), 0);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  EXPECT_EQ(nullptr, tbbtrem(&tree->root, nullptr, nullptr));

  std::vector<int> keys = {50, 30, 70, 20, 40, 60, 80};
  std::vector<int> vals = {50, 30, 70, 20, 40, 60, 80};
  for (size_t i = 0; i < keys.size(); ++i) {
    tbbtdins(tree, &vals[i], &keys[i]);
  }

  // 1. Remove leaf node (20).
  int target_20 = 20;
  TBBT_NODE* node_20 = tbbtdfind(tree, &target_20, nullptr);
  ASSERT_NE(nullptr, node_20);
  void* kp = nullptr;
  void* data = tbbtrem(&tree->root, node_20, &kp);
  EXPECT_EQ(&vals[3], data);
  EXPECT_EQ(&keys[3], kp);
  EXPECT_EQ(6, tbbtcount(tree));
  EXPECT_EQ(nullptr, tbbtdfind(tree, &target_20, nullptr));

  // 2. Remove internal node with 2 children (root: 50).
  int target_50 = 50;
  TBBT_NODE* node_50 = tbbtdfind(tree, &target_50, nullptr);
  ASSERT_NE(nullptr, node_50);
  data = tbbtrem(&tree->root, node_50, nullptr);
  EXPECT_EQ(&vals[0], data);
  EXPECT_EQ(5, tbbtcount(tree));
  EXPECT_EQ(nullptr, tbbtdfind(tree, &target_50, nullptr));

  // 3. Remove remaining nodes one by one until empty.
  // Note: Upstream quirk in tbbtrem (tbbt.c:633-644): Case 3 (removing the root
  // of a 1- or 2-node tree) returns directly without decrementing tree->Count.
  while (tree->root != nullptr) {
    tbbtrem(&tree->root, tree->root, nullptr);
  }
  EXPECT_EQ(nullptr, tree->root);
}

TEST_F(TbbtTest, TbbtPrintAndDumpTraversals) {
  // tbbtprint handles nullptr.
  tbbtprint(nullptr);

  TBBT_TREE* tree = tbbtdmake(nullptr, 0, TBBT_FAST_INT32_COMPARE);
  ASSERT_NE(nullptr, tree);
  absl::Cleanup clean_tree = [tree] { tbbtdfree(tree, nullptr, nullptr); };

  int32_t k1 = 10, k2 = 5, k3 = 15;
  int v1 = 1, v2 = 2, v3 = 3;
  tbbtdins(tree, &v1, &k1);
  tbbtdins(tree, &v2, &k2);
  tbbtdins(tree, &v3, &k3);

  tbbtprint(tree->root);

  // Test traversals: pre-order (-1), in-order (0), post-order (1).
  tbbtdump(tree, -1);
  tbbtdump(tree, 0);
  tbbtdump(tree, 1);
  tbbtdump(nullptr, 0);

  // Test tbbt_printNode, tbbt_dumpNode, and tbbt_dump.
  tbbt_printNode(nullptr, nullptr);
  tbbt_printNode(tree->root, nullptr);

  auto key_dump = [](void* k, void* d) {
    printf(" [key=%d, data=%d]", *static_cast<int32_t*>(k),
           *static_cast<int*>(d));
  };
  tbbt_printNode(tree->root, key_dump);

  tbbt_dumpNode(nullptr, nullptr, 0);
  tbbt_dumpNode(tree->root, nullptr, -1);
  tbbt_dumpNode(tree->root, nullptr, 0);
  tbbt_dumpNode(tree->root, nullptr, 1);
  tbbt_dumpNode(tree->root, key_dump, -1);
  tbbt_dumpNode(tree->root, key_dump, 0);
  tbbt_dumpNode(tree->root, key_dump, 1);

  tbbt_dump(tree, nullptr, 0);
  tbbt_dump(tree, key_dump, -1);
  tbbt_dump(tree, key_dump, 0);
  tbbt_dump(tree, key_dump, 1);

  // Null checks.
  EXPECT_EQ(nullptr, tbbtdins(nullptr, &v1, &k1));
  EXPECT_EQ(nullptr, tbbtdfree(nullptr, nullptr, nullptr));
}

TEST_F(TbbtTest, TbbtfreeWithCustomDeleters) {
  TBBT_NODE* root = nullptr;

  int* data1 = static_cast<int*>(std::malloc(sizeof(int)));
  int* key1 = static_cast<int*>(std::malloc(sizeof(int)));
  *data1 = 100;
  *key1 = 10;

  int* data2 = static_cast<int*>(std::malloc(sizeof(int)));
  int* key2 = static_cast<int*>(std::malloc(sizeof(int)));
  *data2 = 200;
  *key2 = 20;

  ASSERT_NE(nullptr, tbbtins(&root, data1, key1, IntCompare, sizeof(int)));
  ASSERT_NE(nullptr, tbbtins(&root, data2, key2, IntCompare, sizeof(int)));

  // Free with std::free for both data and keys.
  tbbtfree(&root, std::free, std::free);
  EXPECT_EQ(nullptr, root);
}

}  // namespace
}  // namespace hdf4
