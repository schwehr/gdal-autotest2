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
// Unit tests for the OpenJPEG tag-tree coder (src/lib/openjp2/tgt.c).
//
// Caveats:
// - Inspects internal opj_tgt_tree_t and opj_tgt_node_t fields (nodes,
//   numnodes, nodes_size, numleafsh/v, value, low, known, parent).
// - Uses bio.c (opj_bio_*) as the bitstream transport; bio.c 0xFF byte stuffing
//   affects encoded byte lengths.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/tgt.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/tgt.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

// Magic unset threshold in opj_tgt_reset() (tgt.c) / t2.c imsbtree encoding.
constexpr int32_t kDefaultUnsetValue = 999;
constexpr size_t kBioBufferBytes = 1024;

OpjTgtTreePtr MakeTree(uint32_t num_leafs_h, uint32_t num_leafs_v,
                       opj_event_mgr_t* manager = nullptr) {
  return OpjTgtTreePtr(opj_tgt_create(num_leafs_h, num_leafs_v, manager));
}

OpjBioPtr MakeBio() { return OpjBioPtr(opj_bio_create()); }

// Mirrors tgt.c level-building loop to cross-check opj_tgt_create().
uint32_t ExpectedNodeCount(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return 0;
  }
  uint32_t total = 0;
  uint32_t level_nodes = 0;
  do {
    level_nodes = width * height;
    total += level_nodes;
    width = (width + 1) / 2;
    height = (height + 1) / 2;
  } while (level_nodes > 1);
  return total;
}

uint32_t ExpectedLevelCount(uint32_t width, uint32_t height) {
  uint32_t levels = 0;
  uint32_t level_nodes = 0;
  do {
    level_nodes = width * height;
    width = (width + 1) / 2;
    height = (height + 1) / 2;
    ++levels;
  } while (level_nodes > 1);
  return levels;
}

void ExpectTreeResetState(const opj_tgt_tree_t* tree) {
  ASSERT_NE(nullptr, tree);
  ASSERT_NE(nullptr, tree->nodes);
  ASSERT_GT(tree->numnodes, 0u);
  EXPECT_GE(tree->nodes_size,
            tree->numnodes * static_cast<uint32_t>(sizeof(opj_tgt_node_t)));

  const opj_tgt_node_t* const nodes_begin = tree->nodes;
  const opj_tgt_node_t* const nodes_end = tree->nodes + tree->numnodes;
  for (uint32_t i = 0; i < tree->numnodes; ++i) {
    const opj_tgt_node_t& node = tree->nodes[i];
    EXPECT_EQ(kDefaultUnsetValue, node.value) << "node " << i;
    EXPECT_EQ(0, node.low) << "node " << i;
    EXPECT_EQ(0u, node.known) << "node " << i;
    if (i + 1 < tree->numnodes) {
      EXPECT_GT(node.parent, &node) << "node " << i;
      EXPECT_GE(node.parent, nodes_begin) << "node " << i;
      EXPECT_LT(node.parent, nodes_end) << "node " << i;
    } else {
      EXPECT_EQ(nullptr, node.parent) << "root node";
    }
  }

  // Every leaf must reach the root node in (levels - 1) parent hops.
  const uint32_t expected_hops =
      ExpectedLevelCount(tree->numleafsh, tree->numleafsv) - 1;
  const uint32_t num_leaves = tree->numleafsh * tree->numleafsv;
  for (uint32_t leaf = 0; leaf < num_leaves; ++leaf) {
    const opj_tgt_node_t* cur = &tree->nodes[leaf];
    uint32_t hops = 0;
    while (cur->parent != nullptr && hops <= expected_hops) {
      cur = cur->parent;
      ++hops;
    }
    EXPECT_EQ(expected_hops, hops) << "leaf " << leaf;
    EXPECT_EQ(&tree->nodes[tree->numnodes - 1], cur) << "leaf " << leaf;
  }
}

// Encodes and decodes leaf inclusion progressively for thresholds
// 1..max_threshold, matching JPEG 2000 packet header inclusion semantics
// (skipping leaves once included). Returns the threshold - 1 where each leaf
// was included, or -1 if its value is >= max_threshold.
void RunProgressiveRoundTrip(opj_tgt_tree_t* enc_tree, opj_tgt_tree_t* dec_tree,
                             std::span<const int32_t> values,
                             int32_t max_threshold,
                             std::vector<int32_t>* out_decoded) {
  ASSERT_NE(nullptr, enc_tree);
  ASSERT_NE(nullptr, dec_tree);
  ASSERT_NE(nullptr, out_decoded);
  const uint32_t num_leaves = enc_tree->numleafsh * enc_tree->numleafsv;
  ASSERT_EQ(num_leaves, values.size());
  ASSERT_EQ(enc_tree->numleafsh, dec_tree->numleafsh);
  ASSERT_EQ(enc_tree->numleafsv, dec_tree->numleafsv);

  for (uint32_t i = 0; i < num_leaves; ++i) {
    opj_tgt_setvalue(enc_tree, i, values[i]);
  }

  OpjBioPtr enc_bio = MakeBio();
  ASSERT_NE(nullptr, enc_bio);
  std::vector<uint8_t> buffer(kBioBufferBytes, 0);
  opj_bio_init_enc(enc_bio.get(), buffer.data(),
                   static_cast<OPJ_UINT32>(buffer.size()));

  std::vector<bool> enc_included(num_leaves, false);
  for (int32_t threshold = 1; threshold <= max_threshold; ++threshold) {
    for (uint32_t leaf = 0; leaf < num_leaves; ++leaf) {
      if (!enc_included[leaf]) {
        opj_tgt_encode(enc_bio.get(), enc_tree, leaf, threshold);
        if (values[leaf] < threshold) {
          enc_included[leaf] = true;
        }
      }
    }
  }
  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(enc_bio.get()));
  const ptrdiff_t encoded_bytes = opj_bio_numbytes(enc_bio.get());
  ASSERT_GT(encoded_bytes, 0);
  ASSERT_LE(encoded_bytes, static_cast<ptrdiff_t>(buffer.size()));

  OpjBioPtr dec_bio = MakeBio();
  ASSERT_NE(nullptr, dec_bio);
  opj_bio_init_dec(dec_bio.get(), buffer.data(),
                   static_cast<OPJ_UINT32>(encoded_bytes));

  out_decoded->assign(num_leaves, -1);
  bool any_leaf_queried = false;
  for (int32_t threshold = 1; threshold <= max_threshold; ++threshold) {
    for (uint32_t leaf = 0; leaf < num_leaves; ++leaf) {
      if ((*out_decoded)[leaf] < 0) {
        any_leaf_queried = true;
        if (opj_tgt_decode(dec_bio.get(), dec_tree, leaf, threshold) != 0) {
          (*out_decoded)[leaf] = threshold - 1;
          EXPECT_EQ(values[leaf], dec_tree->nodes[leaf].value)
              << "leaf " << leaf;
        }
      }
    }
  }
  ASSERT_EQ(OPJ_TRUE, opj_bio_inalign(dec_bio.get()));
  if (any_leaf_queried) {
    EXPECT_EQ(encoded_bytes, opj_bio_numbytes(dec_bio.get()));
  }
}

void ExpectProgressiveMatchesValues(std::span<const int32_t> values,
                                    std::span<const int32_t> decoded,
                                    int32_t max_threshold) {
  ASSERT_EQ(values.size(), decoded.size());
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i] < max_threshold) {
      EXPECT_EQ(values[i], decoded[i]) << "leaf " << i;
    } else {
      EXPECT_EQ(-1, decoded[i]) << "leaf " << i;
    }
  }
}

TEST(TgtTest, CreateZeroDimensionsReturnsNull) {
  struct ZeroCase {
    uint32_t width;
    uint32_t height;
  };
  constexpr ZeroCase kCases[] = {
      {.width = 0, .height = 0},
      {.width = 0, .height = 5},
      {.width = 5, .height = 0},
  };

  EventCapture events;
  opj_event_mgr_t manager = MakeEventManager(&events);

  for (const ZeroCase& tc : kCases) {
    EXPECT_EQ(nullptr, MakeTree(tc.width, tc.height, nullptr))
        << tc.width << "x" << tc.height;
    EXPECT_EQ(nullptr, MakeTree(tc.width, tc.height, &manager))
        << tc.width << "x" << tc.height;
    EXPECT_TRUE(events.errors.empty());
  }
}

TEST(TgtTest, CreateValidDimensionsInitializesTopologyAndResetState) {
  struct TreeCase {
    uint32_t width;
    uint32_t height;
    uint32_t expected_nodes;
  };
  constexpr TreeCase kCases[] = {
      {.width = 1, .height = 1, .expected_nodes = 1},
      {.width = 1, .height = 5, .expected_nodes = 11},
      {.width = 5, .height = 1, .expected_nodes = 11},
      {.width = 2, .height = 2, .expected_nodes = 5},
      {.width = 4, .height = 4, .expected_nodes = 21},
      {.width = 5, .height = 5, .expected_nodes = 39},
      {.width = 9, .height = 7, .expected_nodes = 92},
  };

  for (const TreeCase& tc : kCases) {
    ASSERT_EQ(tc.expected_nodes, ExpectedNodeCount(tc.width, tc.height));
    OpjTgtTreePtr tree = MakeTree(tc.width, tc.height);
    ASSERT_NE(nullptr, tree) << tc.width << "x" << tc.height;
    EXPECT_EQ(tc.width, tree->numleafsh);
    EXPECT_EQ(tc.height, tree->numleafsv);
    EXPECT_EQ(tc.expected_nodes, tree->numnodes);
    EXPECT_EQ(tc.expected_nodes * static_cast<uint32_t>(sizeof(opj_tgt_node_t)),
              tree->nodes_size);
    ExpectTreeResetState(tree.get());
  }

  // Verify exact 2x2 leaf-to-parent grouping on 2x2 and 3x3 grids.
  OpjTgtTreePtr tree_2x2 = MakeTree(2, 2);
  ASSERT_NE(nullptr, tree_2x2);
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(&tree_2x2->nodes[4], tree_2x2->nodes[i].parent) << "leaf " << i;
  }
  EXPECT_EQ(nullptr, tree_2x2->nodes[4].parent);

  // 3x3 level layout: 0..8 = 3x3 leaves, 9..12 = 2x2 parents, 13 = root (-1).
  OpjTgtTreePtr tree_3x3 = MakeTree(3, 3);
  ASSERT_NE(nullptr, tree_3x3);
  constexpr auto kExpectedParents3x3 = std::to_array<int32_t>(
      {9, 9, 10, 9, 9, 10, 11, 11, 12, 13, 13, 13, 13, -1});
  for (uint32_t i = 0; i < kExpectedParents3x3.size(); ++i) {
    const int32_t expected_idx = kExpectedParents3x3[i];
    const opj_tgt_node_t* expected_ptr =
        expected_idx < 0 ? nullptr : &tree_3x3->nodes[expected_idx];
    EXPECT_EQ(expected_ptr, tree_3x3->nodes[i].parent) << "node " << i;
  }
}

TEST(TgtTest, InitNullAndZeroLeavesHandling) {
  EXPECT_EQ(nullptr, opj_tgt_init(nullptr, 4, 4, nullptr));

  struct ZeroCase {
    uint32_t width;
    uint32_t height;
  };
  constexpr ZeroCase kZeroCases[] = {
      {.width = 0, .height = 0},
      {.width = 0, .height = 5},
      {.width = 5, .height = 0},
  };

  EventCapture events;
  opj_event_mgr_t manager = MakeEventManager(&events);

  // When p_num_leafs_h * p_num_leafs_v == 0, opj_tgt_init destroys p_tree and
  // returns nullptr. Guard the raw pointer with absl::Cleanup against leaks.
  for (const ZeroCase& tc : kZeroCases) {
    opj_tgt_tree_t* raw_tree = opj_tgt_create(3, 3, &manager);
    ASSERT_NE(nullptr, raw_tree);
    absl::Cleanup destroy_if_nonnull = [&raw_tree] {
      opj_tgt_destroy(raw_tree);
    };
    raw_tree = opj_tgt_init(raw_tree, tc.width, tc.height, &manager);
    EXPECT_EQ(nullptr, raw_tree) << tc.width << "x" << tc.height;
    EXPECT_TRUE(events.errors.empty());
  }
}

TEST(TgtTest, InitSameDimensionsResetsWithoutReallocating) {
  OpjTgtTreePtr tree = MakeTree(3, 3);
  ASSERT_NE(nullptr, tree);
  opj_tgt_setvalue(tree.get(), 0, 2);

  OpjBioPtr bio = MakeBio();
  ASSERT_NE(nullptr, bio);
  std::vector<uint8_t> buf(64, 0);
  opj_bio_init_enc(bio.get(), buf.data(), static_cast<OPJ_UINT32>(buf.size()));
  opj_tgt_encode(bio.get(), tree.get(), 0, 4);
  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  ASSERT_EQ(2, tree->nodes[0].value);
  ASSERT_EQ(1u, tree->nodes[0].known);

  opj_tgt_tree_t* const orig_tree = tree.get();
  opj_tgt_node_t* const orig_nodes = tree->nodes;
  const uint32_t orig_nodes_size = tree->nodes_size;

  tree.reset(opj_tgt_init(tree.release(), 3, 3, nullptr));
  ASSERT_EQ(orig_tree, tree.get());
  EXPECT_EQ(orig_nodes, tree->nodes);
  EXPECT_EQ(orig_nodes_size, tree->nodes_size);
  EXPECT_EQ(14u, tree->numnodes);
  ExpectTreeResetState(tree.get());
}

TEST(TgtTest, InitShrinkReuseAndGrowReallocatesNodes) {
  EventCapture events;
  opj_event_mgr_t manager = MakeEventManager(&events);

  OpjTgtTreePtr tree = MakeTree(5, 5, &manager);
  ASSERT_NE(nullptr, tree);
  const uint32_t initial_capacity = tree->nodes_size;
  EXPECT_EQ(39u * sizeof(opj_tgt_node_t), initial_capacity);

  // Shrink height only (5x5 -> 5x2): reuses nodes without reallocating.
  opj_tgt_tree_t* const orig_tree = tree.get();
  tree.reset(opj_tgt_init(tree.release(), 5, 2, &manager));
  ASSERT_EQ(orig_tree, tree.get());
  EXPECT_EQ(5u, tree->numleafsh);
  EXPECT_EQ(2u, tree->numleafsv);
  EXPECT_EQ(16u, tree->numnodes);
  EXPECT_EQ(initial_capacity, tree->nodes_size);
  ExpectTreeResetState(tree.get());

  // Shrink down to 1x1: loop over levels executes zero times.
  tree.reset(opj_tgt_init(tree.release(), 1, 1, &manager));
  ASSERT_EQ(orig_tree, tree.get());
  EXPECT_EQ(1u, tree->numnodes);
  EXPECT_EQ(initial_capacity, tree->nodes_size);
  ExpectTreeResetState(tree.get());

  // Grow within existing high-water nodes_size (1x1 -> 4x3, 17 <= 39 nodes).
  tree.reset(opj_tgt_init(tree.release(), 4, 3, &manager));
  ASSERT_EQ(orig_tree, tree.get());
  EXPECT_EQ(17u, tree->numnodes);
  EXPECT_EQ(initial_capacity, tree->nodes_size);
  ExpectTreeResetState(tree.get());

  // Grow past nodes_size (4x3 -> 9x7, 92 > 39 nodes): triggers opj_realloc and
  // zero-initialization of the newly allocated tail.
  tree.reset(opj_tgt_init(tree.release(), 9, 7, &manager));
  ASSERT_EQ(orig_tree, tree.get());
  EXPECT_EQ(9u, tree->numleafsh);
  EXPECT_EQ(7u, tree->numleafsv);
  EXPECT_EQ(92u, tree->numnodes);
  EXPECT_EQ(92u * sizeof(opj_tgt_node_t), tree->nodes_size);
  ExpectTreeResetState(tree.get());

  // Grow width only past nodes_size (9x7 -> 10x7, 99 > 92 nodes).
  tree.reset(opj_tgt_init(tree.release(), 10, 7, &manager));
  ASSERT_EQ(orig_tree, tree.get());
  EXPECT_EQ(10u, tree->numleafsh);
  EXPECT_EQ(7u, tree->numleafsv);
  EXPECT_EQ(99u, tree->numnodes);
  EXPECT_EQ(99u * sizeof(opj_tgt_node_t), tree->nodes_size);
  ExpectTreeResetState(tree.get());
  EXPECT_TRUE(events.errors.empty());
}

TEST(TgtTest, DestroyAndResetHandleNullAndEmptyNodes) {
  opj_tgt_destroy(nullptr);
  opj_tgt_reset(nullptr);

  // Exercise the p_tree->nodes == nullptr path in opj_tgt_destroy safely.
  auto* bare_tree =
      static_cast<opj_tgt_tree_t*>(opj_calloc(1, sizeof(opj_tgt_tree_t)));
  ASSERT_NE(nullptr, bare_tree);
  opj_tgt_destroy(bare_tree);

  OpjTgtTreePtr tree = MakeTree(4, 4);
  ASSERT_NE(nullptr, tree);
  opj_tgt_setvalue(tree.get(), 5, 1);

  OpjBioPtr bio = MakeBio();
  ASSERT_NE(nullptr, bio);
  std::vector<uint8_t> buf(64, 0);
  opj_bio_init_enc(bio.get(), buf.data(), static_cast<OPJ_UINT32>(buf.size()));
  opj_tgt_encode(bio.get(), tree.get(), 5, 3);
  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  EXPECT_NE(kDefaultUnsetValue, tree->nodes[5].value);
  EXPECT_GT(tree->nodes[5].low, 0);
  EXPECT_EQ(1u, tree->nodes[5].known);

  opj_tgt_reset(tree.get());
  EXPECT_EQ(4u, tree->numleafsh);
  EXPECT_EQ(4u, tree->numleafsv);
  ExpectTreeResetState(tree.get());
}

TEST(TgtTest, SetValuePropagatesMinimumToAncestors) {
  OpjTgtTreePtr single = MakeTree(1, 1);
  ASSERT_NE(nullptr, single);
  opj_tgt_setvalue(single.get(), 0, 4);
  EXPECT_EQ(4, single->nodes[0].value);

  OpjTgtTreePtr tree = MakeTree(2, 2);
  ASSERT_NE(nullptr, tree);

  // Setting a value >= current 999 leaves nodes untouched.
  opj_tgt_setvalue(tree.get(), 0, kDefaultUnsetValue);
  EXPECT_EQ(kDefaultUnsetValue, tree->nodes[0].value);
  EXPECT_EQ(kDefaultUnsetValue, tree->nodes[4].value);

  // Propagate 5 up to root, then lower root to 2 via sibling leaf 1.
  opj_tgt_setvalue(tree.get(), 0, 5);
  EXPECT_EQ(5, tree->nodes[0].value);
  EXPECT_EQ(5, tree->nodes[4].value);

  opj_tgt_setvalue(tree.get(), 1, 2);
  EXPECT_EQ(5, tree->nodes[0].value);
  EXPECT_EQ(2, tree->nodes[1].value);
  EXPECT_EQ(2, tree->nodes[4].value);

  // Larger value on sibling leaf 2 must not overwrite smaller root value 2.
  opj_tgt_setvalue(tree.get(), 2, 7);
  EXPECT_EQ(7, tree->nodes[2].value);
  EXPECT_EQ(2, tree->nodes[4].value);

  // Decreasing leaf 0 to 1 updates both leaf 0 and root 4.
  opj_tgt_setvalue(tree.get(), 0, 1);
  EXPECT_EQ(1, tree->nodes[0].value);
  EXPECT_EQ(1, tree->nodes[4].value);
}

TEST(TgtTest, ProgressiveEncodeDecodeSingleLeafTree) {
  struct SingleCase {
    int32_t value;
    int32_t max_threshold;
  };
  constexpr SingleCase kCases[] = {
      {.value = 0, .max_threshold = 4},
      {.value = 3, .max_threshold = 6},
      {.value = kDefaultUnsetValue, .max_threshold = 5},
  };

  for (const SingleCase& tc : kCases) {
    OpjTgtTreePtr enc_tree = MakeTree(1, 1);
    OpjTgtTreePtr dec_tree = MakeTree(1, 1);
    ASSERT_NE(nullptr, enc_tree);
    ASSERT_NE(nullptr, dec_tree);

    opj_tgt_setvalue(enc_tree.get(), 0, tc.value);

    OpjBioPtr enc_bio = MakeBio();
    ASSERT_NE(nullptr, enc_bio);
    std::vector<uint8_t> buf(64, 0);
    opj_bio_init_enc(enc_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(buf.size()));

    // Threshold 0 emits no bits; higher thresholds exercise both < value,
    // == value + 1, and repeated calls when node->known is already 1.
    for (int32_t t = 0; t <= tc.max_threshold; ++t) {
      opj_tgt_encode(enc_bio.get(), enc_tree.get(), 0, t);
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_flush(enc_bio.get()));
    const ptrdiff_t encoded_bytes = opj_bio_numbytes(enc_bio.get());
    ASSERT_GT(encoded_bytes, 0);
    ASSERT_LE(encoded_bytes, static_cast<ptrdiff_t>(buf.size()));

    OpjBioPtr dec_bio = MakeBio();
    ASSERT_NE(nullptr, dec_bio);
    opj_bio_init_dec(dec_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(encoded_bytes));

    for (int32_t t = 0; t <= tc.max_threshold; ++t) {
      const uint32_t expected_bit = (tc.value < t) ? 1u : 0u;
      const uint32_t actual_bit =
          opj_tgt_decode(dec_bio.get(), dec_tree.get(), 0, t);
      EXPECT_EQ(expected_bit, actual_bit)
          << "value=" << tc.value << " threshold=" << t;
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_inalign(dec_bio.get()));
    EXPECT_EQ(encoded_bytes, opj_bio_numbytes(dec_bio.get()));
    EXPECT_EQ(tc.value < tc.max_threshold ? tc.value : kDefaultUnsetValue,
              dec_tree->nodes[0].value);
  }
}

TEST(TgtTest, ProgressiveEncodeDecodeMultiLevelTrees) {
  // 4x1 tree crafted so parent low lags behind leaf low across sibling paths,
  // exercising both branches of (low > node->low) in encode and decode.
  {
    OpjTgtTreePtr enc_tree = MakeTree(4, 1);
    OpjTgtTreePtr dec_tree = MakeTree(4, 1);
    ASSERT_NE(nullptr, enc_tree);
    ASSERT_NE(nullptr, dec_tree);
    constexpr auto kValues = std::to_array<int32_t>({2, 5, 1, 4});
    for (uint32_t i = 0; i < kValues.size(); ++i) {
      opj_tgt_setvalue(enc_tree.get(), i, kValues[i]);
    }

    OpjBioPtr enc_bio = MakeBio();
    ASSERT_NE(nullptr, enc_bio);
    std::vector<uint8_t> buf(64, 0);
    opj_bio_init_enc(enc_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(buf.size()));

    struct Step {
      uint32_t leaf;
      int32_t threshold;
      uint32_t expected_decoded;
    };
    constexpr Step kSteps[] = {
        {.leaf = 0, .threshold = 1, .expected_decoded = 0},
        {.leaf = 2, .threshold = 2, .expected_decoded = 1},
        {.leaf = 0, .threshold = 2, .expected_decoded = 0},
        {.leaf = 1, .threshold = 3, .expected_decoded = 0},
        {.leaf = 0, .threshold = 3, .expected_decoded = 1},
        {.leaf = 3, .threshold = 5, .expected_decoded = 1},
        {.leaf = 1, .threshold = 6, .expected_decoded = 1},
    };
    for (const Step& step : kSteps) {
      opj_tgt_encode(enc_bio.get(), enc_tree.get(), step.leaf, step.threshold);
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_flush(enc_bio.get()));
    const ptrdiff_t encoded_bytes = opj_bio_numbytes(enc_bio.get());
    ASSERT_GT(encoded_bytes, 0);
    ASSERT_LE(encoded_bytes, static_cast<ptrdiff_t>(buf.size()));

    OpjBioPtr dec_bio = MakeBio();
    ASSERT_NE(nullptr, dec_bio);
    opj_bio_init_dec(dec_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(encoded_bytes));
    for (const Step& step : kSteps) {
      EXPECT_EQ(step.expected_decoded,
                opj_tgt_decode(dec_bio.get(), dec_tree.get(), step.leaf,
                               step.threshold))
          << "leaf=" << step.leaf << " threshold=" << step.threshold;
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_inalign(dec_bio.get()));
    EXPECT_EQ(encoded_bytes, opj_bio_numbytes(dec_bio.get()));
    for (uint32_t i = 0; i < kValues.size(); ++i) {
      EXPECT_EQ(kValues[i], dec_tree->nodes[i].value);
    }
  }

  // 5x5 tree with >= 8 zero-valued leaves (exercises 0xFF bio bit-stuffing),
  // progressive thresholds, and un-included leaves (>= max_threshold).
  {
    constexpr int32_t kMaxThreshold = 6;
    OpjTgtTreePtr enc_tree = MakeTree(5, 5);
    OpjTgtTreePtr dec_tree = MakeTree(5, 5);
    ASSERT_NE(nullptr, enc_tree);
    ASSERT_NE(nullptr, dec_tree);

    std::vector<int32_t> values(25);
    for (size_t i = 0; i < values.size(); ++i) {
      if (i < 10) {
        values[i] = 0;
      } else if (i % 5 == 0) {
        values[i] = kDefaultUnsetValue;
      } else {
        values[i] = static_cast<int32_t>((i * 3) % 8);
      }
    }
    std::vector<int32_t> decoded;
    ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
        enc_tree.get(), dec_tree.get(), values, kMaxThreshold, &decoded));
    ExpectProgressiveMatchesValues(values, decoded, kMaxThreshold);
  }

  // 9x7 rectangular tree across progressive thresholds.
  {
    constexpr int32_t kMaxThreshold = 7;
    OpjTgtTreePtr enc_tree = MakeTree(9, 7);
    OpjTgtTreePtr dec_tree = MakeTree(9, 7);
    ASSERT_NE(nullptr, enc_tree);
    ASSERT_NE(nullptr, dec_tree);

    std::vector<int32_t> values(63);
    for (size_t i = 0; i < values.size(); ++i) {
      values[i] = (i % 11 == 10) ? kDefaultUnsetValue
                                 : static_cast<int32_t>((i * 5 + 1) % 9);
    }
    std::vector<int32_t> decoded;
    ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
        enc_tree.get(), dec_tree.get(), values, kMaxThreshold, &decoded));
    ExpectProgressiveMatchesValues(values, decoded, kMaxThreshold);
  }

  // Re-encoding and re-decoding already-known leaves on a 3x3 tree is stable.
  {
    OpjTgtTreePtr enc_tree = MakeTree(3, 3);
    OpjTgtTreePtr dec_tree = MakeTree(3, 3);
    ASSERT_NE(nullptr, enc_tree);
    ASSERT_NE(nullptr, dec_tree);
    constexpr auto kValues =
        std::to_array<int32_t>({0, 1, 2, 1, 3, 0, 2, 4, 1});
    for (uint32_t i = 0; i < kValues.size(); ++i) {
      opj_tgt_setvalue(enc_tree.get(), i, kValues[i]);
    }

    OpjBioPtr enc_bio = MakeBio();
    ASSERT_NE(nullptr, enc_bio);
    std::vector<uint8_t> buf(128, 0);
    opj_bio_init_enc(enc_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(buf.size()));
    for (int32_t threshold = 1; threshold <= 5; ++threshold) {
      for (uint32_t leaf = 0; leaf < kValues.size(); ++leaf) {
        opj_tgt_encode(enc_bio.get(), enc_tree.get(), leaf, threshold);
      }
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_flush(enc_bio.get()));
    const ptrdiff_t encoded_bytes = opj_bio_numbytes(enc_bio.get());
    ASSERT_GT(encoded_bytes, 0);
    ASSERT_LE(encoded_bytes, static_cast<ptrdiff_t>(buf.size()));

    OpjBioPtr dec_bio = MakeBio();
    ASSERT_NE(nullptr, dec_bio);
    opj_bio_init_dec(dec_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(encoded_bytes));
    for (int32_t threshold = 1; threshold <= 5; ++threshold) {
      for (uint32_t leaf = 0; leaf < kValues.size(); ++leaf) {
        const uint32_t expected_bit = (kValues[leaf] < threshold) ? 1u : 0u;
        const uint32_t actual_bit =
            opj_tgt_decode(dec_bio.get(), dec_tree.get(), leaf, threshold);
        EXPECT_EQ(expected_bit, actual_bit)
            << "leaf=" << leaf << " threshold=" << threshold;
      }
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_inalign(dec_bio.get()));
    EXPECT_EQ(encoded_bytes, opj_bio_numbytes(dec_bio.get()));
  }
}

TEST(TgtTest, ExactValueImsbEncodeDecodeRoundTrip) {
  struct GridCase {
    uint32_t width;
    uint32_t height;
  };
  constexpr GridCase kCases[] = {
      {.width = 5, .height = 5},
      {.width = 9, .height = 7},
  };

  for (const GridCase& tc : kCases) {
    const uint32_t num_leaves = tc.width * tc.height;
    OpjTgtTreePtr enc_tree = MakeTree(tc.width, tc.height);
    OpjTgtTreePtr dec_tree = MakeTree(tc.width, tc.height);
    ASSERT_NE(nullptr, enc_tree);
    ASSERT_NE(nullptr, dec_tree);

    std::vector<int32_t> values(num_leaves);
    for (uint32_t i = 0; i < num_leaves; ++i) {
      values[i] = static_cast<int32_t>((i * 7 + (i % 3)) % 16);
      opj_tgt_setvalue(enc_tree.get(), i, values[i]);
    }

    OpjBioPtr enc_bio = MakeBio();
    ASSERT_NE(nullptr, enc_bio);
    std::vector<uint8_t> buf(kBioBufferBytes, 0);
    opj_bio_init_enc(enc_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(buf.size()));

    // Matches t2.c imsbtree encoding: threshold 999 encodes exact values < 999.
    for (uint32_t leaf = 0; leaf < num_leaves; ++leaf) {
      opj_tgt_encode(enc_bio.get(), enc_tree.get(), leaf, kDefaultUnsetValue);
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_flush(enc_bio.get()));
    const ptrdiff_t encoded_bytes = opj_bio_numbytes(enc_bio.get());
    ASSERT_GT(encoded_bytes, 0);
    ASSERT_LE(encoded_bytes, static_cast<ptrdiff_t>(buf.size()));

    OpjBioPtr dec_bio = MakeBio();
    ASSERT_NE(nullptr, dec_bio);
    opj_bio_init_dec(dec_bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(encoded_bytes));
    for (uint32_t leaf = 0; leaf < num_leaves; ++leaf) {
      int32_t threshold = 0;
      while (opj_tgt_decode(dec_bio.get(), dec_tree.get(), leaf, threshold) ==
             0) {
        ++threshold;
        ASSERT_LE(threshold, 64) << "leaf " << leaf;
      }
      EXPECT_EQ(values[leaf], threshold - 1) << "leaf " << leaf;
      EXPECT_EQ(values[leaf], dec_tree->nodes[leaf].value) << "leaf " << leaf;
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_inalign(dec_bio.get()));
    EXPECT_EQ(encoded_bytes, opj_bio_numbytes(dec_bio.get()));
  }
}

TEST(TgtTest, ResetThenReEncodeAndReDecodeUsesNewValues) {
  constexpr int32_t kMaxThreshold = 6;
  OpjTgtTreePtr enc_tree = MakeTree(5, 5);
  OpjTgtTreePtr dec_tree = MakeTree(5, 5);
  ASSERT_NE(nullptr, enc_tree);
  ASSERT_NE(nullptr, dec_tree);

  std::vector<int32_t> first_values(25);
  for (size_t i = 0; i < first_values.size(); ++i) {
    first_values[i] = (i % 4 == 0) ? 0 : static_cast<int32_t>((i % 7) + 1);
  }
  std::vector<int32_t> decoded;
  ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
      enc_tree.get(), dec_tree.get(), first_values, kMaxThreshold, &decoded));
  ExpectProgressiveMatchesValues(first_values, decoded, kMaxThreshold);

  opj_tgt_reset(enc_tree.get());
  opj_tgt_reset(dec_tree.get());
  ExpectTreeResetState(enc_tree.get());
  ExpectTreeResetState(dec_tree.get());

  // Second pass flips small and unset values to verify zero stale node state.
  std::vector<int32_t> second_values(25);
  for (size_t i = 0; i < second_values.size(); ++i) {
    second_values[i] =
        (i % 4 == 0) ? kDefaultUnsetValue : static_cast<int32_t>((24 - i) % 5);
  }
  ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
      enc_tree.get(), dec_tree.get(), second_values, kMaxThreshold, &decoded));
  ExpectProgressiveMatchesValues(second_values, decoded, kMaxThreshold);
}

TEST(TgtTest, ReInitThenEncodeAndDecodeRoundTrip) {
  constexpr int32_t kMaxThreshold = 6;
  OpjTgtTreePtr enc_tree = MakeTree(2, 2);
  OpjTgtTreePtr dec_tree = MakeTree(2, 2);
  ASSERT_NE(nullptr, enc_tree);
  ASSERT_NE(nullptr, dec_tree);

  constexpr auto kValues2x2 = std::to_array<int32_t>({1, 4, 0, 7});
  std::vector<int32_t> decoded;
  ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
      enc_tree.get(), dec_tree.get(), kValues2x2, kMaxThreshold, &decoded));
  ExpectProgressiveMatchesValues(kValues2x2, decoded, kMaxThreshold);

  // Grow both trees via opj_tgt_init (triggers opj_realloc) and round-trip.
  enc_tree.reset(opj_tgt_init(enc_tree.release(), 9, 7, nullptr));
  dec_tree.reset(opj_tgt_init(dec_tree.release(), 9, 7, nullptr));
  ASSERT_NE(nullptr, enc_tree);
  ASSERT_NE(nullptr, dec_tree);
  std::vector<int32_t> values_9x7(63);
  for (size_t i = 0; i < values_9x7.size(); ++i) {
    values_9x7[i] = static_cast<int32_t>(i % 7);
  }
  ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
      enc_tree.get(), dec_tree.get(), values_9x7, kMaxThreshold, &decoded));
  ExpectProgressiveMatchesValues(values_9x7, decoded, kMaxThreshold);

  // Shrink both trees via opj_tgt_init (reuses buffer) and round-trip.
  enc_tree.reset(opj_tgt_init(enc_tree.release(), 3, 2, nullptr));
  dec_tree.reset(opj_tgt_init(dec_tree.release(), 3, 2, nullptr));
  ASSERT_NE(nullptr, enc_tree);
  ASSERT_NE(nullptr, dec_tree);
  constexpr auto kValues3x2A = std::to_array<int32_t>({5, 0, 2, 999, 1, 3});
  ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
      enc_tree.get(), dec_tree.get(), kValues3x2A, kMaxThreshold, &decoded));
  ExpectProgressiveMatchesValues(kValues3x2A, decoded, kMaxThreshold);

  // Same-dimension opj_tgt_init resets and allows a fresh round-trip.
  enc_tree.reset(opj_tgt_init(enc_tree.release(), 3, 2, nullptr));
  dec_tree.reset(opj_tgt_init(dec_tree.release(), 3, 2, nullptr));
  ASSERT_NE(nullptr, enc_tree);
  ASSERT_NE(nullptr, dec_tree);
  constexpr auto kValues3x2B = std::to_array<int32_t>({0, 4, 999, 2, 3, 1});
  ASSERT_NO_FATAL_FAILURE(RunProgressiveRoundTrip(
      enc_tree.get(), dec_tree.get(), kValues3x2B, kMaxThreshold, &decoded));
  ExpectProgressiveMatchesValues(kValues3x2B, decoded, kMaxThreshold);
}

// TODO(schwehr): Cover opj_tgt_create() and opj_tgt_init() OOM allocation
// failure branches via :openjpeg2_internal_no_malloc and
// ScopedOpjAllocFailCountdown.

}  // namespace
}  // namespace openjpeg2
