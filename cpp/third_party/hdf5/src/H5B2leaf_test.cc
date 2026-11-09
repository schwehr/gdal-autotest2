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
// Unit tests for v2 B-tree leaf node operations: creation, protection, neighbor
// search, insertion, update/modification, swap, SWMR shadowing, removal,
// freeing, and error-handling paths (third_party/hdf5/src/H5B2leaf.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2leaf.c

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`). Match `NDEBUG` around the internal
// prelude and headers so internal struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5B2LEAF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma push_macro("try")
#undef try
#define try h5_try_param
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#include "third_party/hdf5/src/H5B2pkg.h"
#define H5D_FRIEND
#include "third_party/hdf5/src/H5Dpkg.h"
#define H5F_FRIEND
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#ifdef H5B2LEAF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2LEAF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

class ScopedBtree2 {
 public:
  ScopedBtree2() = default;
  explicit ScopedBtree2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedBtree2() { reset(); }

  ScopedBtree2(const ScopedBtree2&) = delete;
  ScopedBtree2& operator=(const ScopedBtree2&) = delete;

  void reset(H5B2_t* next = nullptr) {
    if (bt2_ != nullptr) {
      H5B2_close(bt2_);
    }
    bt2_ = next;
  }

  H5B2_t* get() const { return bt2_; }

 private:
  H5B2_t* bt2_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// Injects file-space allocation failures by disabling free-space manager/aggr
// bypass and forcing tmp_addr = 0 while active, then restores original state.
class ScopedFailMetaAlloc {
 public:
  explicit ScopedFailMetaAlloc(H5F_t* f)
      : f_(f),
        orig_strategy_(f->shared->fs_strategy),
        orig_use_tmp_(f->shared->use_tmp_space),
        orig_tmp_addr_(f->shared->tmp_addr) {
    f_->shared->fs_strategy = H5F_FSPACE_STRATEGY_NONE;
    f_->shared->use_tmp_space = true;
    f_->shared->tmp_addr = 0;
  }

  ~ScopedFailMetaAlloc() {
    f_->shared->fs_strategy = orig_strategy_;
    f_->shared->use_tmp_space = orig_use_tmp_;
    f_->shared->tmp_addr = orig_tmp_addr_;
  }

  ScopedFailMetaAlloc(const ScopedFailMetaAlloc&) = delete;
  ScopedFailMetaAlloc& operator=(const ScopedFailMetaAlloc&) = delete;

 private:
  H5F_t* f_;
  H5F_fspace_strategy_t orig_strategy_;
  bool orig_use_tmp_;
  haddr_t orig_tmp_addr_;
};

struct TestRecCtx {
  bool fail_compare = false;
  bool fail_store = false;
};

// Context state used by fault-injectable custom H5B2_class_t callbacks.
TestRecCtx* g_test_rec_ctx = nullptr;

void* FaultCrtContext(void* udata) { return udata; }
herr_t FaultDstContext(void*) { return 0; }

herr_t FaultStore(void* nrecord, const void* udata) {
  if (g_test_rec_ctx != nullptr && g_test_rec_ctx->fail_store) {
    return -1;
  }
  *static_cast<hsize_t*>(nrecord) = *static_cast<const hsize_t*>(udata);
  return 0;
}

herr_t FaultCompare(const void* rec1, const void* rec2, int* result) {
  if (g_test_rec_ctx != nullptr && g_test_rec_ctx->fail_compare) {
    return -1;
  }
  const hsize_t k1 = *static_cast<const hsize_t*>(rec1);
  const hsize_t k2 = *static_cast<const hsize_t*>(rec2);
  *result = (k1 < k2) ? -1 : ((k1 > k2) ? 1 : 0);
  return 0;
}

herr_t FaultEncode(uint8_t* raw, const void* record, void*) {
  hsize_t val = *static_cast<const hsize_t*>(record);
  std::memcpy(raw, &val, sizeof(val));
  return 0;
}

herr_t FaultDecode(const uint8_t* raw, void* record, void*) {
  hsize_t val = 0;
  std::memcpy(&val, raw, sizeof(val));
  *static_cast<hsize_t*>(record) = val;
  return 0;
}

herr_t FoundCopyKey(const void* record, void* op_data) {
  *static_cast<hsize_t*>(op_data) = *static_cast<const hsize_t*>(record);
  return 0;
}

herr_t FoundFail(const void*, void*) { return -1; }

herr_t ModifyAddDelta(void* record, void* op_data, bool* changed) {
  const hsize_t delta = *static_cast<const hsize_t*>(op_data);
  if (delta == 0) {
    *changed = false;
    return 0;
  }
  *static_cast<hsize_t*>(record) += delta;
  *changed = true;
  return 0;
}

herr_t ModifyFail(void*, void*, bool* changed) {
  // H5B2__update_leaf asserts that changed remains false on callback error.
  *changed = false;
  return -1;
}

herr_t RemoveRecordOp(const void* record, void* op_data) {
  if (op_data != nullptr) {
    *static_cast<hsize_t*>(op_data) = *static_cast<const hsize_t*>(record);
  }
  return 0;
}

herr_t RemoveFailOp(const void*, void*) { return -1; }

// Tests H5B2__insert_leaf, H5B2__update_leaf, H5B2__neighbor_leaf,
// H5B2__remove_leaf, and H5B2__remove_leaf_by_idx across single-leaf and
// multi-level trees, covering min/max cache maintenance, record shifts, and
// edge outcomes.
TEST(H5B2LeafTest, InsertUpdateNeighborAndRemovePaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5B2_create_t cparam = {
      .cls = H5B2_TEST,
      .node_size = 128,
      .rrec_size = sizeof(hsize_t),
      .split_percent = 100,
      .merge_percent = 40,
  };
  ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
  ASSERT_NE(bt2.get(), nullptr);

  H5B2_hdr_t* hdr = bt2.get()->hdr;
  ASSERT_GE(H5B2__hdr_dirty(hdr), 0);
  const unsigned leaf_max = hdr->node_info[0].max_nrec;
  ASSERT_GE(leaf_max, 4u);

  // Depth 0: create root leaf and update empty leaf to insert first record.
  ASSERT_GE(H5B2__create_leaf(hdr, hdr, &hdr->root), 0);
  H5B2_update_status_t status = H5B2_UPDATE_UNKNOWN;
  hsize_t key = 100;
  ASSERT_GE(H5B2__update_leaf(hdr, &hdr->root, &status, H5B2_POS_ROOT, hdr,
                              &key, ModifyAddDelta, &key),
            0);
  EXPECT_EQ(status, H5B2_UPDATE_INSERT_DONE);
  EXPECT_NE(hdr->min_native_rec, nullptr);
  EXPECT_NE(hdr->max_native_rec, nullptr);

  // Depth 0: insert smaller key (updates min_native_rec and memmoves),
  // larger key (updates max_native_rec), and middle key (shifts elements).
  hsize_t k50 = 50;
  hsize_t k150 = 150;
  hsize_t k120 = 120;
  ASSERT_GE(H5B2_insert(bt2.get(), &k50), 0);
  ASSERT_GE(H5B2_insert(bt2.get(), &k150), 0);
  ASSERT_GE(H5B2_insert(bt2.get(), &k120), 0);
  EXPECT_EQ(*static_cast<const hsize_t*>(hdr->min_native_rec), 50u);
  EXPECT_EQ(*static_cast<const hsize_t*>(hdr->max_native_rec), 150u);

  // Update via direct call: new min (20) and new max (200) via update path,
  // plus existing key with changed=false and changed=true.
  hsize_t k20 = 20;
  ASSERT_GE(H5B2_update(bt2.get(), &k20, ModifyAddDelta, &k20), 0);
  EXPECT_EQ(*static_cast<const hsize_t*>(hdr->min_native_rec), 20u);

  hsize_t k200 = 200;
  ASSERT_GE(H5B2_update(bt2.get(), &k200, ModifyAddDelta, &k200), 0);
  EXPECT_EQ(*static_cast<const hsize_t*>(hdr->max_native_rec), 200u);

  hsize_t delta_zero = 0;
  hsize_t delta_one = 1;
  key = 120;
  ASSERT_GE(H5B2_update(bt2.get(), &key, ModifyAddDelta, &delta_zero), 0);
  ASSERT_GE(H5B2_update(bt2.get(), &key, ModifyAddDelta, &delta_one), 0);

  // Verify neighbor lookups on depth=0 leaf: exact, between, and out-of-range.
  hsize_t found = 0;
  hsize_t query = 121;
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &query, FoundCopyKey,
                          &found),
            0);
  EXPECT_EQ(found, 100u);

  query = 121;
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &query, FoundCopyKey,
                          &found),
            0);
  EXPECT_EQ(found, 150u);

  query = 130;
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &query, FoundCopyKey,
                          &found),
            0);
  EXPECT_EQ(found, 121u);
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &query, FoundCopyKey,
                          &found),
            0);
  EXPECT_EQ(found, 150u);

  {
    ScopedErrorSilence silence;
    // Key <= minimum with LESS and >= maximum with GREATER fail (NOTFOUND).
    query = 20;
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &query, FoundCopyKey,
                            &found),
              0);
    query = 10;
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &query, FoundCopyKey,
                            &found),
              0);
    query = 200;
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &query,
                            FoundCopyKey, &found),
              0);
    query = 250;
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &query,
                            FoundCopyKey, &found),
              0);
  }

  // Fill the root leaf to split_nrec to exercise H5B2_UPDATE_INSERT_CHILD_FULL.
  hsize_t nrec = 0;
  ASSERT_GE(H5B2_get_nrec(bt2.get(), &nrec), 0);
  hsize_t next_key = 300;
  while (hdr->root.node_nrec < hdr->node_info[0].split_nrec) {
    ASSERT_GE(H5B2_insert(bt2.get(), &next_key), 0);
    next_key += 10;
  }
  ASSERT_EQ(hdr->depth, 0u);
  // Test direct H5B2__update_leaf returning H5B2_UPDATE_INSERT_CHILD_FULL.
  hsize_t trigger_key = 99999;
  status = H5B2_UPDATE_UNKNOWN;
  ASSERT_GE(H5B2__update_leaf(hdr, &hdr->root, &status, H5B2_POS_ROOT, hdr,
                              &trigger_key, ModifyAddDelta, &delta_zero),
            0);
  EXPECT_EQ(status, H5B2_UPDATE_INSERT_CHILD_FULL);

  // Trigger actual split via H5B2_update so depth becomes >= 1, covering
  // H5B2_POS_LEFT / H5B2_POS_RIGHT / H5B2_POS_MIDDLE paths as well.
  for (int i = 0; i < 110; ++i) {
    hsize_t ins = 1000 + static_cast<hsize_t>(i) * 10;
    ASSERT_GE(H5B2_update(bt2.get(), &ins, ModifyAddDelta, &delta_zero), 0);
  }
  ASSERT_GE(hdr->depth, 2u);

  // Insert a new global min and global max at depth >= 2, hitting POS_LEFT &
  // POS_RIGHT with existing and freshly cleared min/max native records.
  hdr->min_native_rec = H5MM_xfree(hdr->min_native_rec);
  hdr->max_native_rec = H5MM_xfree(hdr->max_native_rec);
  hsize_t min_ins = 5;
  hsize_t max_ins = 99000;
  ASSERT_GE(H5B2_update(bt2.get(), &min_ins, ModifyAddDelta, &delta_zero), 0);
  ASSERT_GE(H5B2_update(bt2.get(), &max_ins, ModifyAddDelta, &delta_zero), 0);
  EXPECT_EQ(*static_cast<const hsize_t*>(hdr->min_native_rec), 5u);
  EXPECT_EQ(*static_cast<const hsize_t*>(hdr->max_native_rec), 99000u);

  hsize_t min_ins2 = 2;
  hsize_t max_ins2 = 99500;
  ASSERT_GE(H5B2_insert(bt2.get(), &min_ins2), 0);
  ASSERT_GE(H5B2_insert(bt2.get(), &max_ins2), 0);

  // Remove min, max, and middle records via both H5B2_remove and
  // H5B2_remove_by_idx (with op != nullptr and op == nullptr).
  hsize_t removed_key = 0;
  ASSERT_GE(H5B2_remove(bt2.get(), &min_ins2, RemoveRecordOp, &removed_key), 0);
  EXPECT_EQ(removed_key, 2u);
  EXPECT_EQ(hdr->min_native_rec, nullptr);

  ASSERT_GE(H5B2_remove(bt2.get(), &max_ins2, nullptr, nullptr), 0);
  EXPECT_EQ(hdr->max_native_rec, nullptr);

  // Populate min/max again then delete with H5B2_remove_by_idx (INC 0 and
  // DEC 0), plus middle elements via both remove variants.
  bool is_found = false;
  ASSERT_GE(H5B2_find(bt2.get(), &min_ins, &is_found, nullptr, nullptr), 0);
  EXPECT_TRUE(is_found);
  ASSERT_GE(H5B2_find(bt2.get(), &max_ins, &is_found, nullptr, nullptr), 0);
  EXPECT_TRUE(is_found);
  ASSERT_NE(hdr->min_native_rec, nullptr);
  ASSERT_NE(hdr->max_native_rec, nullptr);

  ASSERT_GE(
      H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 0, RemoveRecordOp, &removed_key),
      0);
  EXPECT_EQ(hdr->min_native_rec, nullptr);
  ASSERT_GE(H5B2_remove_by_idx(bt2.get(), H5_ITER_DEC, 0, nullptr, nullptr), 0);
  EXPECT_EQ(hdr->max_native_rec, nullptr);

  // Remove a middle element by key and by index.
  hsize_t mid_key = 1200;
  ASSERT_GE(H5B2_remove(bt2.get(), &mid_key, nullptr, nullptr), 0);
  ASSERT_GE(H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 15, RemoveRecordOp,
                               &removed_key),
            0);
  EXPECT_GT(removed_key, 0u);

  // Empty a fresh small tree down to nrec == 0 via both remove paths.
  {
    ScopedBtree2 small_remove(H5B2_create(f, &cparam, f));
    ASSERT_NE(small_remove.get(), nullptr);
    hsize_t k1 = 10;
    hsize_t k2 = 20;
    ASSERT_GE(H5B2_insert(small_remove.get(), &k1), 0);
    ASSERT_GE(H5B2_insert(small_remove.get(), &k2), 0);
    ASSERT_GE(H5B2_remove(small_remove.get(), &k2, nullptr, nullptr), 0);
    ASSERT_GE(H5B2_remove(small_remove.get(), &k1, RemoveRecordOp, &removed_key),
              0);
    EXPECT_EQ(removed_key, 10u);
    ASSERT_GE(H5B2_get_nrec(small_remove.get(), &nrec), 0);
    EXPECT_EQ(nrec, 0u);
  }
  {
    ScopedBtree2 small_idx(H5B2_create(f, &cparam, f));
    ASSERT_NE(small_idx.get(), nullptr);
    hsize_t k1 = 30;
    hsize_t k2 = 40;
    ASSERT_GE(H5B2_insert(small_idx.get(), &k1), 0);
    ASSERT_GE(H5B2_insert(small_idx.get(), &k2), 0);
    ASSERT_GE(H5B2_remove_by_idx(small_idx.get(), H5_ITER_INC, 1, nullptr,
                                 nullptr),
              0);
    ASSERT_GE(H5B2_remove_by_idx(small_idx.get(), H5_ITER_INC, 0,
                                 RemoveRecordOp, &removed_key),
              0);
    EXPECT_EQ(removed_key, 30u);
    ASSERT_GE(H5B2_get_nrec(small_idx.get(), &nrec), 0);
    EXPECT_EQ(nrec, 0u);
  }
}

// Tests direct H5B2__update_leaf / H5B2__insert_leaf with explicit
// H5B2_POS_LEFT / H5B2_POS_RIGHT / H5B2_POS_MIDDLE min/max clearing and
// H5B2__swap_leaf for both depth == 1 (leaf child) and depth > 1 (internal
// child), plus error recovery on unmappable child addresses.
TEST(H5B2LeafTest, DirectPosAndSwapLeafPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5B2_create_t cparam = {
      .cls = H5B2_TEST,
      .node_size = 128,
      .rrec_size = sizeof(hsize_t),
      .split_percent = 100,
      .merge_percent = 40,
  };
  ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
  ASSERT_NE(bt2.get(), nullptr);

  // Build depth >= 2 tree with enough spare capacity in leaves.
  for (int i = 1; i <= 95; ++i) {
    hsize_t v = static_cast<hsize_t>(i) * 100;
    ASSERT_GE(H5B2_insert(bt2.get(), &v), 0);
  }
  H5B2_hdr_t* hdr = bt2.get()->hdr;
  ASSERT_GE(hdr->depth, 2u);

  // Call H5B2__update_leaf directly on a fresh small leaf with POS_LEFT and
  // POS_RIGHT while min_native_rec/max_native_rec start NULL, hitting lines
  // 534-548 and then H5B2__remove_leaf / H5B2__remove_leaf_by_idx with
  // POS_LEFT and POS_RIGHT.
  {
    ScopedBtree2 leaf_tree(H5B2_create(f, &cparam, f));
    ASSERT_NE(leaf_tree.get(), nullptr);
    H5B2_hdr_t* lhdr = leaf_tree.get()->hdr;
    ASSERT_GE(H5B2__create_leaf(lhdr, lhdr, &lhdr->root), 0);

    H5B2_update_status_t st = H5B2_UPDATE_UNKNOWN;
    hsize_t k20 = 20;
    hsize_t k10 = 10;
    hsize_t k30 = 30;
    hsize_t delta = 0;
    ASSERT_GE(H5B2__update_leaf(lhdr, &lhdr->root, &st, H5B2_POS_LEFT, lhdr,
                                &k20, ModifyAddDelta, &delta),
              0);
    lhdr->min_native_rec = H5MM_xfree(lhdr->min_native_rec);
    ASSERT_GE(H5B2__update_leaf(lhdr, &lhdr->root, &st, H5B2_POS_RIGHT, lhdr,
                                &k30, ModifyAddDelta, &delta),
              0);
    ASSERT_GE(H5B2__update_leaf(lhdr, &lhdr->root, &st, H5B2_POS_LEFT, lhdr,
                                &k10, ModifyAddDelta, &delta),
              0);

    ASSERT_NE(lhdr->min_native_rec, nullptr);
    ASSERT_NE(lhdr->max_native_rec, nullptr);

    // Clear min via H5B2__remove_leaf with POS_LEFT and max with POS_RIGHT.
    ASSERT_GE(H5B2__remove_leaf(lhdr, &lhdr->root, H5B2_POS_LEFT, lhdr, &k10,
                                nullptr, nullptr),
              0);
    lhdr->root.all_nrec--;
    EXPECT_EQ(lhdr->min_native_rec, nullptr);

    ASSERT_GE(H5B2__remove_leaf(lhdr, &lhdr->root, H5B2_POS_RIGHT, lhdr, &k30,
                                nullptr, nullptr),
              0);
    lhdr->root.all_nrec--;
    EXPECT_EQ(lhdr->max_native_rec, nullptr);

    // Clear min/max via H5B2__remove_leaf_by_idx with POS_LEFT and POS_RIGHT.
    hsize_t k5 = 5;
    hsize_t k40 = 40;
    ASSERT_GE(
        H5B2__insert_leaf(lhdr, &lhdr->root, H5B2_POS_LEFT, lhdr, &k5), 0);
    ASSERT_GE(
        H5B2__insert_leaf(lhdr, &lhdr->root, H5B2_POS_RIGHT, lhdr, &k40), 0);
    ASSERT_GE(H5B2__remove_leaf_by_idx(lhdr, &lhdr->root, H5B2_POS_LEFT, lhdr,
                                       0, nullptr, nullptr),
              0);
    lhdr->root.all_nrec--;
    EXPECT_EQ(lhdr->min_native_rec, nullptr);
    ASSERT_GE(H5B2__remove_leaf_by_idx(lhdr, &lhdr->root, H5B2_POS_RIGHT, lhdr,
                                       lhdr->root.node_nrec - 1, nullptr,
                                       nullptr),
              0);
    lhdr->root.all_nrec--;
    EXPECT_EQ(lhdr->max_native_rec, nullptr);
  }

  // Exercise H5B2__swap_leaf at depth > 1 and depth == 1 on valid nodes, then
  // swap straight back to preserve B-tree ordering invariants.
  H5B2_internal_t* root_int = H5B2__protect_internal(
      hdr, hdr, &hdr->root, hdr->depth, false, H5AC__NO_FLAGS_SET);
  ASSERT_NE(root_int, nullptr);

  unsigned root_flags = H5AC__NO_FLAGS_SET;
  hsize_t temp_rec = 0;
  ASSERT_GE(
      H5B2__swap_leaf(hdr, hdr->depth, root_int, &root_flags, 0, &temp_rec), 0);
  EXPECT_NE(temp_rec, 0u);
  ASSERT_GE(
      H5B2__swap_leaf(hdr, hdr->depth, root_int, &root_flags, 0, &temp_rec), 0);
  EXPECT_EQ(temp_rec, 0u);

  // Protect internal child at depth - 1 == 1 and exercise depth == 1 swap path.
  if (hdr->depth == 2) {
    H5B2_internal_t* child_int = H5B2__protect_internal(
        hdr, root_int, &root_int->node_ptrs[0], 1, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(child_int, nullptr);
    unsigned child_flags = H5AC__NO_FLAGS_SET;
    ASSERT_GE(
        H5B2__swap_leaf(hdr, 1, child_int, &child_flags, 0, &temp_rec), 0);
    EXPECT_NE(temp_rec, 0u);
    ASSERT_GE(
        H5B2__swap_leaf(hdr, 1, child_int, &child_flags, 0, &temp_rec), 0);
    EXPECT_EQ(temp_rec, 0u);
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root_int->node_ptrs[0].addr,
                             child_int, child_flags),
              0);
  }

  // Test error path when child address is unmapped for depth > 1, then restore.
  {
    ScopedErrorSilence silence;
    const haddr_t saved_addr = root_int->node_ptrs[0].addr;
    root_int->node_ptrs[0].addr = 0x100000000ULL;
    const auto restore_addr =
        absl::MakeCleanup([root_int, saved_addr]() {
          root_int->node_ptrs[0].addr = saved_addr;
        });
    EXPECT_LT(
        H5B2__swap_leaf(hdr, hdr->depth, root_int, &root_flags, 0, &temp_rec),
        0);
  }

  // Test error path when leaf child address is unmapped at depth == 1.
  if (hdr->depth == 2) {
    H5B2_internal_t* child_int = H5B2__protect_internal(
        hdr, root_int, &root_int->node_ptrs[0], 1, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(child_int, nullptr);
    ScopedErrorSilence silence;
    const haddr_t saved_addr = child_int->node_ptrs[0].addr;
    child_int->node_ptrs[0].addr = 0x100000000ULL;
    const auto restore_addr =
        absl::MakeCleanup([child_int, saved_addr]() {
          child_int->node_ptrs[0].addr = saved_addr;
        });
    unsigned child_flags = H5AC__NO_FLAGS_SET;
    EXPECT_LT(
        H5B2__swap_leaf(hdr, 1, child_int, &child_flags, 0, &temp_rec), 0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root_int->node_ptrs[0].addr,
                             child_int, child_flags),
              0);
  }

  ASSERT_GE(
      H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root_int, root_flags),
      0);
}

// Tests failure branches in leaf routines: H5MF_alloc failure in
// H5B2__create_leaf, H5AC_protect failure on bad addresses, duplicate insert,
// callback compare/store/modify/found/remove failures, and missing records.
TEST(H5B2LeafTest, ErrorAndCallbackFailurePaths) {
  TestRecCtx fault_ctx;
  g_test_rec_ctx = &fault_ctx;
  const auto reset_global_ctx =
      absl::MakeCleanup([]() { g_test_rec_ctx = nullptr; });

  const H5B2_class_t fault_cls = {
      H5B2_TEST_ID,
      "H5B2_LEAF_FAULT_TEST",
      sizeof(hsize_t),
      FaultCrtContext,
      FaultDstContext,
      FaultStore,
      FaultCompare,
      FaultEncode,
      FaultDecode,
      nullptr,
  };

  // Declare ScopedBtree2/ScopedFile after fault_cls so teardown can call
  // encode while fault_cls is still alive.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5B2_create_t cparam = {
      .cls = &fault_cls,
      .node_size = 128,
      .rrec_size = sizeof(hsize_t),
      .split_percent = 100,
      .merge_percent = 40,
  };
  ScopedBtree2 bt2(H5B2_create(f, &cparam, &fault_ctx));
  ASSERT_NE(bt2.get(), nullptr);

  H5B2_hdr_t* hdr = bt2.get()->hdr;
  ASSERT_GE(H5B2__hdr_dirty(hdr), 0);

  // H5B2__create_leaf allocation failure via H5MF_alloc returning HADDR_UNDEF.
  {
    ScopedErrorSilence silence;
    ScopedFailMetaAlloc alloc_fail(f);
    H5B2_node_ptr_t bad_ptr{};
    bad_ptr.addr = HADDR_UNDEF;
    EXPECT_LT(H5B2__create_leaf(hdr, hdr, &bad_ptr), 0);
  }

  // Insert valid records so leaf has entries for subsequent failure paths.
  hsize_t k10 = 10;
  hsize_t k20 = 20;
  hsize_t k30 = 30;
  ASSERT_GE(H5B2_insert(bt2.get(), &k10), 0);
  ASSERT_GE(H5B2_insert(bt2.get(), &k30), 0);
  hsize_t nrec = 0;
  ASSERT_GE(H5B2_get_nrec(bt2.get(), &nrec), 0);
  EXPECT_EQ(nrec, 2u);

  {
    ScopedErrorSilence silence;
    const auto reset_flags = absl::MakeCleanup([&fault_ctx]() {
      fault_ctx.fail_compare = false;
      fault_ctx.fail_store = false;
    });

    // Protect failures on unmapped address across all leaf entry points.
    H5B2_node_ptr_t bogus = {
        .addr = 0x100000000ULL,
        .node_nrec = 1,
        .all_nrec = 1,
    };
    EXPECT_EQ(
        H5B2__protect_leaf(hdr, hdr, &bogus, false, H5AC__READ_ONLY_FLAG),
        nullptr);
    EXPECT_LT(H5B2__neighbor_leaf(hdr, &bogus, nullptr, H5B2_COMPARE_GREATER,
                                  hdr, &k10, FoundCopyKey, &k20),
              0);
    EXPECT_LT(H5B2__insert_leaf(hdr, &bogus, H5B2_POS_ROOT, hdr, &k20), 0);
    H5B2_update_status_t status = H5B2_UPDATE_UNKNOWN;
    hsize_t zero = 0;
    EXPECT_LT(H5B2__update_leaf(hdr, &bogus, &status, H5B2_POS_ROOT, hdr, &k20,
                                ModifyAddDelta, &zero),
              0);
    EXPECT_LT(
        H5B2__remove_leaf(hdr, &bogus, H5B2_POS_ROOT, hdr, &k10, nullptr, nullptr),
        0);
    EXPECT_LT(H5B2__remove_leaf_by_idx(hdr, &bogus, H5B2_POS_ROOT, hdr, 0,
                                       nullptr, nullptr),
              0);

    // Duplicate record in H5B2__insert_leaf.
    EXPECT_LT(H5B2__insert_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &k10), 0);

    // Store failure in H5B2__insert_leaf and H5B2__update_leaf.
    fault_ctx.fail_store = true;
    EXPECT_LT(H5B2__insert_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &k20), 0);
    EXPECT_LT(H5B2__update_leaf(hdr, &hdr->root, &status, H5B2_POS_ROOT, hdr,
                                &k20, ModifyAddDelta, &zero),
              0);
    fault_ctx.fail_store = false;

    // Compare failures across neighbor, insert, update, and remove.
    fault_ctx.fail_compare = true;
    EXPECT_LT(H5B2__neighbor_leaf(hdr, &hdr->root, nullptr, H5B2_COMPARE_LESS,
                                  hdr, &k20, FoundCopyKey, &k20),
              0);
    EXPECT_LT(H5B2__insert_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &k20), 0);
    EXPECT_LT(H5B2__update_leaf(hdr, &hdr->root, &status, H5B2_POS_ROOT, hdr,
                                &k20, ModifyAddDelta, &zero),
              0);
    EXPECT_LT(H5B2__remove_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &k10,
                                nullptr, nullptr),
              0);
    fault_ctx.fail_compare = false;

    // Callback failures: found (<0), modify (<0), remove (<0), not found.
    EXPECT_LT(H5B2__neighbor_leaf(hdr, &hdr->root, nullptr, H5B2_COMPARE_GREATER,
                                  hdr, &k10, FoundFail, nullptr),
              0);
    EXPECT_LT(H5B2__update_leaf(hdr, &hdr->root, &status, H5B2_POS_ROOT, hdr,
                                &k10, ModifyFail, nullptr),
              0);
    EXPECT_LT(H5B2__remove_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &k20,
                                nullptr, nullptr),
              0);
    EXPECT_LT(H5B2__remove_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &k10,
                                RemoveFailOp, nullptr),
              0);
    EXPECT_LT(H5B2__remove_leaf_by_idx(hdr, &hdr->root, H5B2_POS_ROOT, hdr, 0,
                                       RemoveFailOp, nullptr),
              0);
  }
}

// Helper to create a SWMR-capable v1.10+ Sec2 file and a 2D unlimited chunked
// dataset backed by a v2 B-tree with optional SWMR activation.
void CreateSwmrChunkedDataset(const std::string& path, bool start_swmr,
                              ScopedFile* out_file, ScopedDataset* out_dset) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  *out_file = ScopedFile(
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(out_file->valid());

  const std::array<hsize_t, 2> dims = {10, 1};
  const std::array<hsize_t, 2> maxdims = {H5S_UNLIMITED, H5S_UNLIMITED};
  ScopedSpace space(H5Screate_simple(2, dims.data(), maxdims.data()));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  const std::array<hsize_t, 2> chunk_dims = {1, 1};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk_dims.data()), 0);

  *out_dset = ScopedDataset(H5Dcreate2(out_file->get(), "chunks",
                                       H5T_NATIVE_INT32, space.get(),
                                       H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(out_dset->valid());

  if (start_swmr) {
    ASSERT_GE(H5Fstart_swmr_write(out_file->get()), 0);
  }
}

struct SwmrBtreeUdata {
  H5D_chunk_rec_t rec{};
  unsigned ndims = 2;
};

herr_t SwmrModifyChunkAddr(void* record, void*, bool* changed) {
  static_cast<H5D_chunk_rec_t*>(record)->chunk_addr += 8;
  *changed = true;
  return 0;
}

// Tests genuine SWMR v2 B-tree leaf operations: top_proxy attachment during
// creation and disk reload, shadowing on flush/epoch change vs same-epoch skip,
// H5B2__protect_leaf with shadow=true, and SWMR remove/remove_by_idx paths.
TEST(H5B2LeafTest, SwmrCreateProtectShadowAndRemovePaths) {
  TempFile temp_file("h5b2leaf_swmr.h5");

  {
    ScopedFile file;
    ScopedDataset dset;
    CreateSwmrChunkedDataset(temp_file.string(), /*start_swmr=*/true, &file,
                             &dset);

    // Write enough 1x1 chunks (> split_nrec ~ 84) to trigger leaf splits and
    // create multiple leaves with hdr->top_proxy != nullptr.
    const std::array<hsize_t, 2> new_dims = {96, 1};
    ASSERT_GE(H5Dset_extent(dset.get(), new_dims.data()), 0);
    std::vector<int32_t> data(96);
    for (int i = 0; i < 96; ++i) {
      data[i] = i + 1;
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Now write again (after flush) and verify SWMR B-tree shadowing & epoch
    // skip using direct B-tree operations on the dataset's live H5B2_t.
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5D_t* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(dset.get(), H5I_DATASET));
    ASSERT_NE(dset_ptr, nullptr);
    H5B2_t* bt2 = dset_ptr->shared->layout.storage.u.chunk.u.btree2.bt2;
    ASSERT_NE(bt2, nullptr);
    ASSERT_TRUE(bt2->hdr->swmr_write);
    ASSERT_NE(bt2->hdr->top_proxy, nullptr);

    // Exercise SWMR H5B2_insert -> H5B2__insert_leaf (line 409) & shadow.
    SwmrBtreeUdata ins_udata{};
    ins_udata.ndims = 2;
    ins_udata.rec.scaled[0] = 200;
    ins_udata.rec.scaled[1] = 0;
    ins_udata.rec.nbytes = sizeof(int32_t);
    ins_udata.rec.filter_mask = 0;
    ins_udata.rec.chunk_addr = 0x4000;
    ASSERT_GE(H5B2_insert(bt2, &ins_udata), 0);

    // Flush to increment shadow_epoch, then modify existing record via
    // H5B2_update -> H5B2__update_leaf (sets H5B2_UPDATE_SHADOW_DONE), then
    // modify again before flush so leaf->shadow_epoch > hdr->shadow_epoch.
    ASSERT_GE(H5Dflush(dset.get()), 0);
    ASSERT_GE(H5B2_update(bt2, &ins_udata, SwmrModifyChunkAddr, nullptr), 0);
    ASSERT_GE(H5B2_update(bt2, &ins_udata, SwmrModifyChunkAddr, nullptr), 0);

    // Flush, then test SWMR remove (nrec > 0 shadow) and remove_by_idx.
    ASSERT_GE(H5Dflush(dset.get()), 0);
    ASSERT_GE(H5B2_remove(bt2, &ins_udata, nullptr, nullptr), 0);
    ASSERT_GE(H5Dflush(dset.get()), 0);
    ASSERT_GE(H5B2_remove_by_idx(bt2, H5_ITER_DEC, 0, nullptr, nullptr), 0);
  }

  // Reopen the file with SWMR_WRITE, reload leaf from disk (leaf->top_proxy
  // starts NULL -> H5B2__protect_leaf lines 192-197 attaches it), and test
  // direct H5B2__protect_leaf with shadow = true (lines 200-202).
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);

    ScopedFile file(H5Fopen(temp_file.c_str(),
                            H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "chunks", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    // Trigger dataset B-tree load by reading an element.
    int32_t out = 0;
    const std::array<hsize_t, 2> one_dim = {1, 1};
    ScopedSpace mspace(H5Screate_simple(2, one_dim.data(), nullptr));
    ScopedSpace fspace(H5Dget_space(dset.get()));
    ASSERT_TRUE(mspace.valid());
    ASSERT_TRUE(fspace.valid());
    const std::array<hsize_t, 2> start = {0, 0};
    const std::array<hsize_t, 2> count = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                      H5P_DEFAULT, &out),
              0);
    EXPECT_EQ(out, 1);

    // Protect a leaf explicitly with shadow = true.
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5D_t* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(dset.get(), H5I_DATASET));
    ASSERT_NE(dset_ptr, nullptr);
    H5B2_t* bt2 = dset_ptr->shared->layout.storage.u.chunk.u.btree2.bt2;
    ASSERT_NE(bt2, nullptr);
    H5B2_hdr_t* hdr = bt2->hdr;
    ASSERT_GE(hdr->depth, 1u);

    H5B2_internal_t* root_int = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(root_int, nullptr);
    H5B2_leaf_t* leaf = H5B2__protect_leaf(
        hdr, root_int, &root_int->node_ptrs[0], /*shadow=*/true,
        H5AC__NO_FLAGS_SET);
    ASSERT_NE(leaf, nullptr);
    EXPECT_NE(leaf->top_proxy, nullptr);
    ASSERT_GE(H5AC_unprotect(hdr->f, H5AC_BT2_LEAF, root_int->node_ptrs[0].addr,
                             leaf, H5AC__DIRTIED_FLAG),
              0);
    ASSERT_GE(H5AC_unprotect(hdr->f, H5AC_BT2_INT, hdr->root.addr, root_int,
                             H5AC__DIRTIED_FLAG),
              0);
  }
}

// Tests SWMR shadow allocation failure cleanup paths in H5B2__protect_leaf,
// H5B2__shadow_leaf, H5B2__insert_leaf, H5B2__update_leaf, H5B2__remove_leaf,
// and H5B2__remove_leaf_by_idx, plus SWMR leaf deletion (nrec == 0).
TEST(H5B2LeafTest, SwmrShadowFailuresAndEmptyLeafDelete) {
  // 1. SWMR single-record leaf deletion via H5B2_remove (nrec == 0 branch).
  {
    TempFile tf("h5b2leaf_swmr_del1.h5");
    ScopedFile file;
    ScopedDataset dset;
    CreateSwmrChunkedDataset(tf.string(), /*start_swmr=*/true, &file, &dset);
    int32_t v = 7;
    const std::array<hsize_t, 2> one_dim = {1, 1};
    ScopedSpace mspace(H5Screate_simple(2, one_dim.data(), nullptr));
    ScopedSpace fspace(H5Dget_space(dset.get()));
    const std::array<hsize_t, 2> start = {0, 0};
    const std::array<hsize_t, 2> count = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                       H5P_DEFAULT, &v),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5D_t* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(dset.get(), H5I_DATASET));
    H5B2_t* bt2 = dset_ptr->shared->layout.storage.u.chunk.u.btree2.bt2;
    SwmrBtreeUdata udata{};
    udata.ndims = 2;
    udata.rec.scaled[0] = 0;
    udata.rec.scaled[1] = 0;
    ASSERT_GE(H5B2_remove(bt2, &udata, nullptr, nullptr), 0);
  }

  // 2. SWMR single-record leaf deletion via H5B2_remove_by_idx (nrec == 0).
  {
    TempFile tf("h5b2leaf_swmr_del2.h5");
    ScopedFile file;
    ScopedDataset dset;
    CreateSwmrChunkedDataset(tf.string(), /*start_swmr=*/true, &file, &dset);
    int32_t v = 9;
    const std::array<hsize_t, 2> one_dim = {1, 1};
    ScopedSpace mspace(H5Screate_simple(2, one_dim.data(), nullptr));
    ScopedSpace fspace(H5Dget_space(dset.get()));
    const std::array<hsize_t, 2> start = {0, 0};
    const std::array<hsize_t, 2> count = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                  nullptr, count.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                       H5P_DEFAULT, &v),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5D_t* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(dset.get(), H5I_DATASET));
    H5B2_t* bt2 = dset_ptr->shared->layout.storage.u.chunk.u.btree2.bt2;
    ASSERT_GE(H5B2_remove_by_idx(bt2, H5_ITER_INC, 0, nullptr, nullptr), 0);
  }

  // 3. Shadow failure in H5B2__protect_leaf(shadow=true), H5B2__insert_leaf,
  // H5B2__update_leaf, H5B2__remove_leaf, and H5B2__remove_leaf_by_idx.
  auto setup_three_chunk_swmr = [](TempFile* tf, ScopedFile* file,
                                   ScopedDataset* dset, H5B2_hdr_t** hdr_out) {
    CreateSwmrChunkedDataset(tf->string(), /*start_swmr=*/true, file, dset);
    const std::array<hsize_t, 2> one_dim = {1, 1};
    ScopedSpace mspace(H5Screate_simple(2, one_dim.data(), nullptr));
    ScopedSpace fspace(H5Dget_space(dset->get()));
    const std::array<hsize_t, 2> count = {1, 1};
    for (hsize_t r = 0; r < 3; ++r) {
      int32_t v = static_cast<int32_t>(r + 1);
      const std::array<hsize_t, 2> start = {r, 0};
      ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start.data(),
                                    nullptr, count.data(), nullptr),
                0);
      ASSERT_GE(H5Dwrite(dset->get(), H5T_NATIVE_INT32, mspace.get(),
                         fspace.get(), H5P_DEFAULT, &v),
                0);
    }
    ASSERT_GE(H5Dflush(dset->get()), 0);
    H5D_t* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(dset->get(), H5I_DATASET));
    ASSERT_NE(dset_ptr, nullptr);
    H5B2_t* bt2 = dset_ptr->shared->layout.storage.u.chunk.u.btree2.bt2;
    ASSERT_NE(bt2, nullptr);
    *hdr_out = bt2->hdr;
    ASSERT_EQ((*hdr_out)->depth, 0u);
  };

  {
    TempFile tf("h5b2leaf_swmr_sf_protect.h5");
    ScopedFile file;
    ScopedDataset dset;
    H5B2_hdr_t* hdr = nullptr;
    ASSERT_NO_FATAL_FAILURE(setup_three_chunk_swmr(&tf, &file, &dset, &hdr));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    ScopedFailMetaAlloc alloc_fail(GetH5File(file.get()));
    EXPECT_EQ(H5B2__protect_leaf(hdr, hdr, &hdr->root, /*shadow=*/true,
                                 H5AC__NO_FLAGS_SET),
              nullptr);
  }

  {
    TempFile tf("h5b2leaf_swmr_sf_rem.h5");
    ScopedFile file;
    ScopedDataset dset;
    H5B2_hdr_t* hdr = nullptr;
    ASSERT_NO_FATAL_FAILURE(setup_three_chunk_swmr(&tf, &file, &dset, &hdr));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    ScopedFailMetaAlloc alloc_fail(GetH5File(file.get()));
    SwmrBtreeUdata existing{};
    existing.ndims = 2;
    existing.rec.scaled[0] = 0;
    existing.rec.scaled[1] = 0;
    EXPECT_LT(H5B2__remove_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &existing,
                                nullptr, nullptr),
              0);
  }

  {
    TempFile tf("h5b2leaf_swmr_sf_remidx.h5");
    ScopedFile file;
    ScopedDataset dset;
    H5B2_hdr_t* hdr = nullptr;
    ASSERT_NO_FATAL_FAILURE(setup_three_chunk_swmr(&tf, &file, &dset, &hdr));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    ScopedFailMetaAlloc alloc_fail(GetH5File(file.get()));
    EXPECT_LT(H5B2__remove_leaf_by_idx(hdr, &hdr->root, H5B2_POS_ROOT, hdr, 0,
                                       nullptr, nullptr),
              0);
  }

  {
    TempFile tf("h5b2leaf_swmr_sf_ins.h5");
    ScopedFile file;
    ScopedDataset dset;
    H5B2_hdr_t* hdr = nullptr;
    ASSERT_NO_FATAL_FAILURE(setup_three_chunk_swmr(&tf, &file, &dset, &hdr));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    ScopedFailMetaAlloc alloc_fail(GetH5File(file.get()));
    SwmrBtreeUdata extra{};
    extra.ndims = 2;
    extra.rec.scaled[0] = 5;
    extra.rec.scaled[1] = 0;
    extra.rec.nbytes = sizeof(int32_t);
    extra.rec.chunk_addr = 0x5000;
    EXPECT_LT(H5B2__insert_leaf(hdr, &hdr->root, H5B2_POS_ROOT, hdr, &extra),
              0);
    hdr->root.all_nrec++;
  }

  {
    TempFile tf("h5b2leaf_swmr_sf_upd.h5");
    ScopedFile file;
    ScopedDataset dset;
    H5B2_hdr_t* hdr = nullptr;
    ASSERT_NO_FATAL_FAILURE(setup_three_chunk_swmr(&tf, &file, &dset, &hdr));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    ScopedFailMetaAlloc alloc_fail(GetH5File(file.get()));
    SwmrBtreeUdata existing{};
    existing.ndims = 2;
    existing.rec.scaled[0] = 0;
    existing.rec.scaled[1] = 0;
    H5B2_update_status_t st = H5B2_UPDATE_UNKNOWN;
    EXPECT_LT(H5B2__update_leaf(hdr, &hdr->root, &st, H5B2_POS_ROOT, hdr,
                                &existing, SwmrModifyChunkAddr, nullptr),
              0);
  }
}

}  // namespace
}  // namespace hdf5
