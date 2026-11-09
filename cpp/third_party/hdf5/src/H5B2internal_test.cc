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
// Tests internal v2 B-tree node lifecycle and algorithms: creation, protection,
// neighbor lookup, insertion, update (modify/shadow/could_split), SWMR
// shadowing across epochs, key and index removal with root collapse, and
// internal node deallocation (third_party/hdf5/src/H5B2internal.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2internal.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t (H5AC_info_t), the first member of H5B2_hdr_t /
// H5B2_internal_t / H5B2_leaf_t. NDEBUG must be defined around both the prelude
// and internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5B2INTERNAL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
// H5B2private.h uses `try` as a parameter name, which collides with libc++'s
// -fno-exceptions `#define try if (1)` macro.
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#include "third_party/hdf5/src/H5B2pkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5B2INTERNAL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2INTERNAL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x30000) {
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

class ScopedBTree2 {
 public:
  ScopedBTree2() = default;
  explicit ScopedBTree2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedBTree2() { reset(); }

  ScopedBTree2(const ScopedBTree2&) = delete;
  ScopedBTree2& operator=(const ScopedBTree2&) = delete;

  void reset(H5B2_t* next = nullptr) {
    if (bt2_ != nullptr) {
      H5B2_close(bt2_);
    }
    bt2_ = next;
  }

  H5B2_t* get() const { return bt2_; }
  bool valid() const { return bt2_ != nullptr; }

 private:
  H5B2_t* bt2_ = nullptr;
};

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

struct CustomRec {
  uint64_t key;
  uint64_t val;
};

herr_t CustomStore(void* nrecord, const void* udata) {
  *static_cast<CustomRec*>(nrecord) = *static_cast<const CustomRec*>(udata);
  return 0;
}

herr_t CustomCompare(const void* rec1, const void* rec2, int* result) {
  const uint64_t k1 = static_cast<const CustomRec*>(rec1)->key;
  const uint64_t k2 = static_cast<const CustomRec*>(rec2)->key;
  *result = (k1 < k2) ? -1 : ((k1 > k2) ? 1 : 0);
  return 0;
}

herr_t CustomEncode(uint8_t* raw, const void* nrecord, void* /*ctx*/) {
  const auto* rec = static_cast<const CustomRec*>(nrecord);
  std::memcpy(raw, &rec->key, sizeof(uint64_t));
  std::memcpy(raw + sizeof(uint64_t), &rec->val, sizeof(uint64_t));
  return 0;
}

herr_t CustomDecode(const uint8_t* raw, void* nrecord, void* /*ctx*/) {
  auto* rec = static_cast<CustomRec*>(nrecord);
  std::memcpy(&rec->key, raw, sizeof(uint64_t));
  std::memcpy(&rec->val, raw + sizeof(uint64_t), sizeof(uint64_t));
  return 0;
}

herr_t CustomDebug(FILE* stream, int indent, int fwidth, const void* record,
                   const void* /*ctx*/) {
  const auto* rec = static_cast<const CustomRec*>(record);
  fprintf(stream, "%*s%-*s (%llu, %llu)\n", indent, "", fwidth, "CustomRec:",
          static_cast<unsigned long long>(rec->key),
          static_cast<unsigned long long>(rec->val));
  return 0;
}

bool g_fail_compare = false;
int g_fail_compare_after = -1;

herr_t ConditionalFailCompare(const void* rec1, const void* rec2, int* result) {
  if (g_fail_compare) {
    return -1;
  }
  if (g_fail_compare_after >= 0) {
    if (g_fail_compare_after == 0) {
      return -1;
    }
    --g_fail_compare_after;
  }
  return CustomCompare(rec1, rec2, result);
}

const H5B2_class_t kSwmrCustomClass = {
    H5B2_CDSET_ID,
    "H5B2_SWMR_INTERNAL_TEST",
    sizeof(CustomRec),
    nullptr,
    nullptr,
    CustomStore,
    CustomCompare,
    CustomEncode,
    CustomDecode,
    CustomDebug,
};

const H5B2_class_t kConditionalFailClass = {
    H5B2_TEST2_ID,
    "H5B2_COND_FAIL_CLS",
    sizeof(CustomRec),
    nullptr,
    nullptr,
    CustomStore,
    ConditionalFailCompare,
    CustomEncode,
    CustomDecode,
    CustomDebug,
};

ScopedFile CreateSwmrTempFile(const TempFile& tmp) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) <
      0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                              H5P_DEFAULT, fapl.get()));
}

H5B2_t* CreateTest2Tree(H5F_t* f, uint32_t node_size = 128,
                        uint8_t split_percent = 100,
                        uint8_t merge_percent = 30) {
  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST2;
  cparam.node_size = node_size;
  cparam.rrec_size = 16;
  cparam.split_percent = split_percent;
  cparam.merge_percent = merge_percent;
  return H5B2_create(f, &cparam, f);
}

herr_t CaptureFoundTest2(const void* record, void* op_data) {
  *static_cast<H5B2_test_rec_t*>(op_data) =
      *static_cast<const H5B2_test_rec_t*>(record);
  return 0;
}

herr_t FailingFound(const void* /*record*/, void* /*op_data*/) { return -1; }

struct ModifyParams {
  hsize_t new_val = 0;
  bool should_change = true;
  bool should_fail = false;
};

herr_t ModifyTest2Op(void* record, void* op_data, bool* changed) {
  auto* params = static_cast<ModifyParams*>(op_data);
  if (params->should_fail) {
    *changed = false;
    return -1;
  }
  if (params->should_change) {
    static_cast<H5B2_test_rec_t*>(record)->val = params->new_val;
    *changed = true;
  } else {
    *changed = false;
  }
  return 0;
}

herr_t ModifyCustomOp(void* record, void* op_data, bool* changed) {
  auto* params = static_cast<ModifyParams*>(op_data);
  if (params->should_fail) {
    *changed = false;
    return -1;
  }
  if (params->should_change) {
    static_cast<CustomRec*>(record)->val = params->new_val;
    *changed = true;
  } else {
    *changed = false;
  }
  return 0;
}

herr_t FailingRemoveOp(const void* /*record*/, void* /*op_data*/) {
  return -1;
}

int CollectTest2Keys(const void* record, void* op_data) {
  static_cast<std::vector<hsize_t>*>(op_data)->push_back(
      static_cast<const H5B2_test_rec_t*>(record)->key);
  return H5_ITER_CONT;
}

int CollectCustomKeys(const void* record, void* op_data) {
  static_cast<std::vector<uint64_t>*>(op_data)->push_back(
      static_cast<const CustomRec*>(record)->key);
  return H5_ITER_CONT;
}

// Helper: build a depth-1 H5B2_TEST2 tree (`node_size=128`, leaf `split_nrec=7`,
// `merge_nrec=2`; internal `split_nrec=4`, `merge_nrec=1`) where root has
// `nrec == 4` (`5` leaf children) and leaf record counts match
// `target_counts[0..4]` (each in `[3..7]`), returned via `*out_seps`.
void BuildFiveLeafRootTree(H5B2_t* bt, H5F_t* f,
                           const std::array<uint16_t, 5>& target_counts,
                           std::array<hsize_t, 4>* out_seps) {
  H5B2_hdr_t* hdr = bt->hdr;
  hdr->f = f;
  hsize_t base_k = 1000;
  while (true) {
    if (hdr->depth == 1 && hdr->root.node_nrec == 4) {
      break;
    }
    H5B2_test_rec_t rec{base_k, base_k};
    ASSERT_GE(H5B2_insert(bt, &rec), 0);
    base_k += 1000;
    ASSERT_LT(base_k, 100000u);
  }

  // Snapshot the 4 root separator keys (spaced >= 1000 apart).
  H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1,
                                                 false, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(root, nullptr);
  const uint16_t init_nrec = root->nrec;
  const uint16_t init_depth = root->depth;
  std::array<hsize_t, 4> seps{};
  if (init_depth == 1 && init_nrec == 4) {
    for (size_t i = 0; i < 4; ++i) {
      seps[i] =
          reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, i))->key;
    }
  }
  ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                           H5AC__NO_FLAGS_SET),
            0);
  ASSERT_EQ(init_depth, 1u);
  ASSERT_EQ(init_nrec, 4u);
  *out_seps = seps;

  // Pass 1: shrink any leaf above target BEFORE growing any leaf, so we never
  // split a full leaf or merge below merge_nrec (all targets >= 3 > merge=2).
  for (int step = 0; step < 60; ++step) {
    root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                  H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    const uint16_t d = root->depth;
    const uint16_t n = root->nrec;
    std::array<uint16_t, 5> cur{};
    if (d == 1 && n == 4) {
      for (size_t i = 0; i < 5; ++i) {
        cur[i] = root->node_ptrs[i].node_nrec;
      }
    }
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_EQ(d, 1u);
    ASSERT_EQ(n, 4u);

    bool shrunk = false;
    hsize_t offset = 0;
    for (size_t i = 0; i < 5; ++i) {
      if (cur[i] > target_counts[i]) {
        ASSERT_GE(
            H5B2_remove_by_idx(bt, H5_ITER_INC, offset, nullptr, nullptr), 0);
        shrunk = true;
        break;
      }
      offset += cur[i] + 1;
    }
    if (!shrunk) {
      break;
    }
  }

  // Pass 2: grow leaves up to target_counts[0..4] (distinct disjoint ranges).
  std::array<hsize_t, 5> ins_cursors{
      100,
      seps[0] + 1,
      seps[1] + 1,
      seps[2] + 1,
      seps[3] + 1,
  };
  for (int step = 0; step < 60; ++step) {
    root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                  H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    const uint16_t d = root->depth;
    const uint16_t n = root->nrec;
    std::array<uint16_t, 5> cur{};
    if (d == 1 && n == 4) {
      for (size_t i = 0; i < 5; ++i) {
        cur[i] = root->node_ptrs[i].node_nrec;
      }
    }
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_EQ(d, 1u);
    ASSERT_EQ(n, 4u);
    if (cur == target_counts) {
      return;
    }
    bool grew = false;
    ModifyParams noop_mp{0, false, false};
    for (size_t i = 0; i < 5; ++i) {
      if (cur[i] < target_counts[i]) {
        // Use H5B2_update rather than H5B2_insert because H5B2_insert splits
        // the root proactively whenever root->nrec == split_nrec, whereas
        // H5B2_update inserts directly into non-full leaves (`cur[i] < 7`).
        H5B2_test_rec_t rec{ins_cursors[i]++, 1};
        ASSERT_GE(H5B2_update(bt, &rec, ModifyTest2Op, &noop_mp), 0);
        grew = true;
        break;
      }
    }
    ASSERT_TRUE(grew);
  }
  ADD_FAILURE() << "Did not converge to target leaf counts";
}

TEST(H5B2internalTest, CreateProtectShadowAndFreeInternalLifecycle) {
  TempFile tmp("h5b2internal_swmr_shadow.h5");
  ScopedFile file = CreateSwmrTempFile(tmp);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = &kSwmrCustomClass;
  cparam.node_size = 128;
  cparam.rrec_size = 16;
  cparam.split_percent = 100;
  cparam.merge_percent = 30;

  ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
  ASSERT_TRUE(bt.valid());
  H5B2_hdr_t* hdr = bt.get()->hdr;
  hdr->f = f;
  ASSERT_TRUE(hdr->swmr_write);
  ASSERT_NE(hdr->top_proxy, nullptr);

  for (uint64_t k = 1; k <= 55; ++k) {
    CustomRec rec{k * 10, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  ASSERT_GE(hdr->depth, 2u);

  // Flush so hdr->shadow_epoch increases; then shadow on protect (1st call
  // shadows because shadow_epoch <= hdr->shadow_epoch; 2nd call skips copy!).
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  H5B2_internal_t* root1 = H5B2__protect_internal(hdr, hdr, &hdr->root,
                                                  hdr->depth, true,
                                                  H5AC__NO_FLAGS_SET);
  ASSERT_NE(root1, nullptr);
  EXPECT_EQ(root1->shadow_epoch, hdr->shadow_epoch + 1);
  EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root1,
                           H5AC__DIRTIED_FLAG),
            0);
  EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

  // Second shadow request in the same epoch does NOT re-move the node.
  haddr_t addr_before = hdr->root.addr;
  H5B2_internal_t* root2 = H5B2__protect_internal(hdr, hdr, &hdr->root,
                                                  hdr->depth, true,
                                                  H5AC__NO_FLAGS_SET);
  ASSERT_NE(root2, nullptr);
  EXPECT_EQ(hdr->root.addr, addr_before);
  EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root2,
                           H5AC__NO_FLAGS_SET),
            0);

  // Delete cleanly to exercise H5B2__internal_free via H5B2_delete.
  haddr_t bt_addr = HADDR_UNDEF;
  ASSERT_GE(H5B2_get_addr(bt.get(), &bt_addr), 0);
  bt.reset();
  EXPECT_GE(H5B2_delete(f, bt_addr, nullptr, nullptr, nullptr), 0);
}

TEST(H5B2internalTest, NeighborInternalAllDirectionsDepth1And2AndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
  ASSERT_TRUE(bt.valid());
  for (hsize_t k = 1; k <= 55; ++k) {
    H5B2_test_rec_t rec{k * 100, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  H5B2_hdr_t* hdr = bt.get()->hdr;
  ASSERT_GE(hdr->depth, 2u);

  // Extract keys stored in the depth-2 root and its depth-1 children to craft
  // exact neighbor queries hitting cmp == 0, cmp > 0, and cmp < 0 at both
  // depth == 2 and depth == 1.
  H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root,
                                                 hdr->depth, false,
                                                 H5AC__READ_ONLY_FLAG);
  ASSERT_NE(root, nullptr);
  const hsize_t root_key0 =
      reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, 0))->key;
  H5B2_internal_t* d1_left = H5B2__protect_internal(
      hdr, root, &root->node_ptrs[0], static_cast<uint16_t>(hdr->depth - 1),
      false, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(d1_left, nullptr);
  const hsize_t d1_key0 =
      reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(d1_left, hdr, 0))->key;
  ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr, d1_left,
                           H5AC__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                           H5AC__NO_FLAGS_SET),
            0);

  H5B2_test_rec_t found{};

  // LESS than minimum key -> leftmost path (idx == 0 everywhere), not found.
  {
    ScopedErrorSilence silence;
    H5B2_test_rec_t probe{1, 0};
    EXPECT_LT(H5B2_neighbor(bt.get(), H5B2_COMPARE_LESS, &probe,
                            CaptureFoundTest2, &found),
              0);
  }

  // GREATER than maximum key -> rightmost path (idx == nrec everywhere), fail.
  {
    ScopedErrorSilence silence;
    H5B2_test_rec_t probe{999999, 0};
    EXPECT_LT(H5B2_neighbor(bt.get(), H5B2_COMPARE_GREATER, &probe,
                            CaptureFoundTest2, &found),
              0);
  }

  // Query just above root_key0 for COMPARE_LESS (idx > 0 at depth 2 -> sets
  // neighbor_loc = root_key0 and descends to leftmost leaf of child 1).
  {
    H5B2_test_rec_t probe{root_key0 + 1, 0};
    EXPECT_GE(H5B2_neighbor(bt.get(), H5B2_COMPARE_LESS, &probe,
                            CaptureFoundTest2, &found),
              0);
    EXPECT_EQ(found.key, root_key0);
  }

  // Query just below root_key0 for COMPARE_GREATER (idx < nrec at depth 2).
  {
    H5B2_test_rec_t probe{root_key0 - 1, 0};
    EXPECT_GE(H5B2_neighbor(bt.get(), H5B2_COMPARE_GREATER, &probe,
                            CaptureFoundTest2, &found),
              0);
    EXPECT_EQ(found.key, root_key0);
  }

  // Query at depth-1 internal key for COMPARE_LESS and COMPARE_GREATER.
  {
    H5B2_test_rec_t probe_eq{d1_key0, 0};
    EXPECT_GE(H5B2_neighbor(bt.get(), H5B2_COMPARE_LESS, &probe_eq,
                            CaptureFoundTest2, &found),
              0);
    EXPECT_EQ(found.key, d1_key0 - 100);

    EXPECT_GE(H5B2_neighbor(bt.get(), H5B2_COMPARE_GREATER, &probe_eq,
                            CaptureFoundTest2, &found),
              0);
    EXPECT_EQ(found.key, d1_key0);

    H5B2_test_rec_t probe_gt{d1_key0 + 1, 0};
    EXPECT_GE(H5B2_neighbor(bt.get(), H5B2_COMPARE_LESS, &probe_gt,
                            CaptureFoundTest2, &found),
              0);
    EXPECT_EQ(found.key, d1_key0);

    H5B2_test_rec_t probe_lt{d1_key0 - 1, 0};
    EXPECT_GE(H5B2_neighbor(bt.get(), H5B2_COMPARE_GREATER, &probe_lt,
                            CaptureFoundTest2, &found),
              0);
    EXPECT_EQ(found.key, d1_key0);
  }

  // Propagating error when `found` op fails inside depth-2 -> depth-1 -> leaf.
  {
    ScopedErrorSilence silence;
    H5B2_test_rec_t probe{root_key0, 0};
    EXPECT_LT(
        H5B2_neighbor(bt.get(), H5B2_COMPARE_LESS, &probe, FailingFound, &found),
        0);
  }

  // Failing compare callback in H5B2__neighbor_internal.
  {
    g_fail_compare = false;
    auto cleanup_cmp = absl::MakeCleanup([] { g_fail_compare = false; });

    H5B2_create_t fail_cparam{};
    fail_cparam.cls = &kConditionalFailClass;
    fail_cparam.node_size = 128;
    fail_cparam.rrec_size = 16;
    fail_cparam.split_percent = 100;
    fail_cparam.merge_percent = 30;
    ScopedBTree2 fail_bt(H5B2_create(f, &fail_cparam, nullptr));
    ASSERT_TRUE(fail_bt.valid());
    for (uint64_t k = 1; k <= 20; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(fail_bt.get(), &rec), 0);
    }
    g_fail_compare = true;
    CustomRec probe{50, 0};
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_neighbor(fail_bt.get(), H5B2_COMPARE_LESS, &probe,
                            CaptureFoundTest2, &found),
              0);
  }
}

TEST(H5B2internalTest, InsertInternalDuplicatesCompareFailAndSwmrShadow) {
  // Part 1: In-memory depth-3 tree duplicate inserts hitting depth 2 internal,
  // depth 1 internal propagation, and leaf propagation.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 95; ++k) {
      // Interleave keys to trigger idx == 0, middle, idx == nrec paths.
      hsize_t key = (k % 2 == 0) ? (5000 - k * 10) : (k * 10);
      H5B2_test_rec_t rec{key, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_GE(hdr->depth, 2u);

    // Inspect keys at root (depth 2+), depth 1 internal, and leaf.
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    const hsize_t root_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, 0))->key;
    H5B2_internal_t* d1 = H5B2__protect_internal(
        hdr, root, &root->node_ptrs[0], static_cast<uint16_t>(hdr->depth - 1),
        false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(d1, nullptr);
    const hsize_t d1_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(d1, hdr, 0))->key;
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr, d1,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    ScopedErrorSilence silence;
    // Duplicate matching internal root record (cmp == 0).
    H5B2_test_rec_t dup_root{root_key, 999};
    EXPECT_LT(H5B2_insert(bt.get(), &dup_root), 0);
    // Duplicate propagating up from lower internal node.
    H5B2_test_rec_t dup_d1{d1_key, 999};
    EXPECT_LT(H5B2_insert(bt.get(), &dup_d1), 0);
    // Duplicate propagating up from leaf (`10`).
    H5B2_test_rec_t dup_leaf{10, 999};
    EXPECT_LT(H5B2_insert(bt.get(), &dup_leaf), 0);
  }

  // Part 2: Failing compare inside H5B2__insert_internal.
  {
    g_fail_compare = false;
    auto cleanup_cmp = absl::MakeCleanup([] { g_fail_compare = false; });

    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;

    H5B2_create_t fail_cparam{};
    fail_cparam.cls = &kConditionalFailClass;
    fail_cparam.node_size = 128;
    fail_cparam.rrec_size = 16;
    fail_cparam.split_percent = 100;
    fail_cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &fail_cparam, nullptr));
    ASSERT_TRUE(bt.valid());
    for (uint64_t k = 1; k <= 20; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    g_fail_compare = true;
    CustomRec rec{99, 99};
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_insert(bt.get(), &rec), 0);
  }

  // Part 3: SWMR H5B2__insert_internal across flush epochs -> shadows internal.
  {
    TempFile tmp("h5b2internal_swmr_ins.h5");
    ScopedFile file = CreateSwmrTempFile(tmp);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;

    H5B2_create_t cparam{};
    cparam.cls = &kSwmrCustomClass;
    cparam.node_size = 128;
    cparam.rrec_size = 16;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
    ASSERT_TRUE(bt.valid());

    for (uint64_t k = 1; k <= 45; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    for (uint64_t k = 46; k <= 85; ++k) {
      CustomRec rec{k * 10 + 5, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    std::vector<uint64_t> keys;
    EXPECT_EQ(H5B2_iterate(bt.get(), CollectCustomKeys, &keys), 0);
    EXPECT_EQ(keys.size(), 85u);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
  }
}

TEST(H5B2internalTest, UpdateInternalCouldSplitMatrixStatusesAndSwmr) {
  // Part 1: Exercise all could_split cases at depth == 1 (`root->nrec == 4`,
  // leaves at [0, 1, 2, 3, 4], max=7) via H5B2_update on new keys:
  //   (7, 7, 3, 3, 3) + ins leaf 0 -> idx == 0 could_split = true (punt)
  //   (7, 3, 3, 3, 3) + ins leaf 0 -> idx == 0 could_split = false (dodge)
  //   (3, 3, 3, 7, 7) + ins leaf 4 -> idx == nrec could_split = true (punt)
  //   (3, 3, 3, 3, 7) + ins leaf 4 -> idx == nrec could_split = false (dodge)
  //   (7, 7, 3, 3, 3) + ins leaf 1 -> middle left-neighbor full -> true
  //   (3, 7, 7, 3, 3) + ins leaf 1 -> middle right-neighbor full -> true
  //   (3, 7, 3, 3, 3) + ins leaf 1 -> middle neither full -> could_split=false
  struct UpdateCase {
    std::array<uint16_t, 5> leaf_counts;
    size_t target_leaf;
    bool expect_split;
  };
  const std::vector<UpdateCase> update_cases = {
      {{7, 7, 3, 3, 3}, 0, true},
      {{7, 3, 3, 3, 3}, 0, false},
      {{3, 3, 3, 7, 7}, 4, true},
      {{3, 3, 3, 3, 7}, 4, false},
      {{7, 7, 3, 3, 3}, 1, true},
      {{3, 7, 7, 3, 3}, 1, true},
      {{3, 7, 3, 3, 3}, 1, false},
  };

  for (const auto& uc : update_cases) {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    std::array<hsize_t, 4> seps{};
    BuildFiveLeafRootTree(bt.get(), f, uc.leaf_counts, &seps);

    const hsize_t ins_key =
        (uc.target_leaf == 0) ? 50 : (seps[uc.target_leaf - 1] + 50);

    ModifyParams mp{777, true, false};
    H5B2_test_rec_t rec{ins_key, 777};
    ASSERT_GE(H5B2_update(bt.get(), &rec, ModifyTest2Op, &mp), 0);
    if (uc.expect_split) {
      EXPECT_EQ(bt.get()->hdr->depth, 2u);
    } else {
      EXPECT_EQ(bt.get()->hdr->depth, 1u);
    }
  }

  // Part 2: Depth >= 2 update paths: modify internal node record (`changed`
  // true & false), callback error inside internal node, child update errors
  // at depth == 1 and depth == 2, and `INSERT_CHILD_FULL` propagating up.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());

    for (hsize_t k = 1; k <= 85; ++k) {
      H5B2_test_rec_t rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_GE(hdr->depth, 2u);

    // Get one depth-2 key and one depth-1 key.
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    const hsize_t d2_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, 0))->key;
    H5B2_internal_t* d1 = H5B2__protect_internal(
        hdr, root, &root->node_ptrs[0], static_cast<uint16_t>(hdr->depth - 1),
        false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(d1, nullptr);
    const hsize_t d1_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(d1, hdr, 0))->key;
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr, d1,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    // Modify depth-2 key with changed=false and changed=true.
    ModifyParams no_change{0, false, false};
    H5B2_test_rec_t p2{d2_key, 0};
    EXPECT_GE(H5B2_update(bt.get(), &p2, ModifyTest2Op, &no_change), 0);
    ModifyParams do_change{4242, true, false};
    EXPECT_GE(H5B2_update(bt.get(), &p2, ModifyTest2Op, &do_change), 0);

    // Modify depth-1 key (bubbles H5B2_UPDATE_MODIFY_DONE through depth 2).
    H5B2_test_rec_t p1{d1_key, 0};
    EXPECT_GE(H5B2_update(bt.get(), &p1, ModifyTest2Op, &do_change), 0);

    // Modify leaf key (`10`) and insert new keys via H5B2_update until splitting.
    H5B2_test_rec_t pleaf{10, 0};
    EXPECT_GE(H5B2_update(bt.get(), &pleaf, ModifyTest2Op, &do_change), 0);
    for (hsize_t k = 86; k <= 105; ++k) {
      H5B2_test_rec_t ins{k * 10 + 3, k};
      EXPECT_GE(H5B2_update(bt.get(), &ins, ModifyTest2Op, &do_change), 0);
    }

    // Failing callback inside depth-2 internal node, depth-1 internal node, and
    // leaf node.
    ScopedErrorSilence silence;
    ModifyParams fail_p{0, false, true};
    EXPECT_LT(H5B2_update(bt.get(), &p2, ModifyTest2Op, &fail_p), 0);
    EXPECT_LT(H5B2_update(bt.get(), &p1, ModifyTest2Op, &fail_p), 0);
    EXPECT_LT(H5B2_update(bt.get(), &pleaf, ModifyTest2Op, &fail_p), 0);
  }

  // Part 3: Compare failure in H5B2__update_internal.
  {
    g_fail_compare = false;
    auto cleanup_cmp = absl::MakeCleanup([] { g_fail_compare = false; });
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    H5B2_create_t fail_cparam{};
    fail_cparam.cls = &kConditionalFailClass;
    fail_cparam.node_size = 128;
    fail_cparam.rrec_size = 16;
    fail_cparam.split_percent = 100;
    fail_cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &fail_cparam, nullptr));
    ASSERT_TRUE(bt.valid());
    for (uint64_t k = 1; k <= 20; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    g_fail_compare = true;
    CustomRec probe{20, 0};
    ModifyParams mp{1, true, false};
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_update(bt.get(), &probe, ModifyCustomOp, &mp), 0);
  }

  // Part 4: SWMR H5B2_update across epochs (`H5B2_UPDATE_SHADOW_DONE` bubbling
  // through depth 1 & depth 2).
  {
    TempFile tmp("h5b2internal_swmr_upd.h5");
    ScopedFile file = CreateSwmrTempFile(tmp);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    H5B2_create_t cparam{};
    cparam.cls = &kSwmrCustomClass;
    cparam.node_size = 128;
    cparam.rrec_size = 16;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
    ASSERT_TRUE(bt.valid());

    for (uint64_t k = 1; k <= 55; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_GE(hdr->depth, 2u);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    const uint64_t d2_key =
        reinterpret_cast<CustomRec*>(H5B2_INT_NREC(root, hdr, 0))->key;
    H5B2_internal_t* d1 = H5B2__protect_internal(
        hdr, root, &root->node_ptrs[0], static_cast<uint16_t>(hdr->depth - 1),
        false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(d1, nullptr);
    const uint64_t d1_key =
        reinterpret_cast<CustomRec*>(H5B2_INT_NREC(d1, hdr, 0))->key;
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr, d1,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    // Modify leaf key (`10`) -> shadows leaf -> bubbles H5B2_UPDATE_SHADOW_DONE
    // to d1 (marks d1 dirty & shadows d1!) -> bubbles to d2 (marks d2 dirty,
    // downgrades to H5B2_UPDATE_MODIFY_DONE, and shadows d2 on exit!).
    ModifyParams mp{8888, true, false};
    CustomRec leaf_rec{10, 0};
    EXPECT_GE(H5B2_update(bt.get(), &leaf_rec, ModifyCustomOp, &mp), 0);

    // Flush and modify d1 key & d2 key directly in new epochs.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    CustomRec d1_rec{d1_key, 0};
    EXPECT_GE(H5B2_update(bt.get(), &d1_rec, ModifyCustomOp, &mp), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    CustomRec d2_rec{d2_key, 0};
    EXPECT_GE(H5B2_update(bt.get(), &d2_rec, ModifyCustomOp, &mp), 0);
  }
}

TEST(H5B2internalTest, RemoveInternalKeysSwapLocCollapseAndSwmr) {
  // Part 1: Non-SWMR depth >= 2 key removals covering:
  // - deleting key residing in depth-2 root (swap_loc passes through depth 1
  //   both directly and when child needs redistribute/merge in while loop)
  // - leftmost, rightmost, and middle child redistribute2/3 and merge2/3
  // - root collapse depth 2 -> 1 -> 0
  // - error paths (missing key at depth 1 & depth 2, callback error, compare)
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());

    for (hsize_t k = 1; k <= 95; ++k) {
      H5B2_test_rec_t rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_GE(hdr->depth, 2u);

    // Error paths while tree has depth >= 2.
    {
      ScopedErrorSilence silence;
      H5B2_test_rec_t missing_low{5, 0};
      EXPECT_LT(H5B2_remove(bt.get(), &missing_low, nullptr, nullptr), 0);
      H5B2_test_rec_t missing_high{9999, 0};
      EXPECT_LT(H5B2_remove(bt.get(), &missing_high, nullptr, nullptr), 0);
      H5B2_test_rec_t fail_rec{10, 0};
      EXPECT_LT(H5B2_remove(bt.get(), &fail_rec, FailingRemoveOp, nullptr), 0);
    }

    // Repeatedly remove the current root's internal keys until tree drops to
    // depth 1, exercising swap_loc propagation from depth 2 -> 1 (and depth 3
    // if depth == 3) across multiple rebalance states!
    while (hdr->depth >= 2) {
      H5B2_internal_t* root = H5B2__protect_internal(
          hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(root, nullptr);
      const hsize_t target_key =
          reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, 0))->key;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      H5B2_test_rec_t rem{target_key, 0};
      ASSERT_GE(H5B2_remove(bt.get(), &rem, nullptr, nullptr), 0);
    }

    // Also test non-monotonic interior/head/tail deletion on a fresh depth-2
    // tree to collapse cleanly down to 0 records.
    ScopedBTree2 bt2(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt2.valid());
    for (hsize_t k = 1; k <= 85; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    for (hsize_t k = 42; k <= 85; ++k) {
      H5B2_test_rec_t rem{k, 0};
      ASSERT_GE(H5B2_remove(bt2.get(), &rem, nullptr, nullptr), 0);
    }
    for (hsize_t k = 41; k >= 1; --k) {
      H5B2_test_rec_t rem{k, 0};
      ASSERT_GE(H5B2_remove(bt2.get(), &rem, nullptr, nullptr), 0);
    }
    H5B2_stat_t info{};
    ASSERT_GE(H5B2_stat_info(bt2.get(), &info), 0);
    EXPECT_EQ(info.nrecords, 0u);
    EXPECT_EQ(info.depth, 0u);
  }

  // Part 2: Failing compare inside H5B2__remove_internal.
  {
    g_fail_compare = false;
    auto cleanup_cmp = absl::MakeCleanup([] { g_fail_compare = false; });
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    H5B2_create_t fail_cparam{};
    fail_cparam.cls = &kConditionalFailClass;
    fail_cparam.node_size = 128;
    fail_cparam.rrec_size = 16;
    fail_cparam.split_percent = 100;
    fail_cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &fail_cparam, nullptr));
    ASSERT_TRUE(bt.valid());
    for (uint64_t k = 1; k <= 20; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    g_fail_compare = true;
    CustomRec probe{20, 0};
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
  }

  // Part 3: SWMR H5B2_remove from depth 2 -> 1 -> 0 across flushes.
  {
    TempFile tmp("h5b2internal_swmr_rem.h5");
    ScopedFile file = CreateSwmrTempFile(tmp);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    H5B2_create_t cparam{};
    cparam.cls = &kSwmrCustomClass;
    cparam.node_size = 128;
    cparam.rrec_size = 16;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
    ASSERT_TRUE(bt.valid());

    for (uint64_t k = 1; k <= 65; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    ASSERT_GE(bt.get()->hdr->depth, 2u);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

    // Remove middle-to-ends in SWMR mode down to empty (hits SWMR shadowing,
    // SWMR root collapse depth 2->1 and 1->0 with flush depend update).
    for (uint64_t k = 32; k <= 65; ++k) {
      CustomRec rem{k * 10, 0};
      ASSERT_GE(H5B2_remove(bt.get(), &rem, nullptr, nullptr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    for (uint64_t k = 31; k >= 1; --k) {
      CustomRec rem{k * 10, 0};
      ASSERT_GE(H5B2_remove(bt.get(), &rem, nullptr, nullptr), 0);
    }
    H5B2_stat_t info{};
    ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
    EXPECT_EQ(info.nrecords, 0u);
    EXPECT_EQ(info.depth, 0u);
  }
}

TEST(H5B2internalTest, RemoveInternalByIdxSwapLocRebalanceCollapseAndSwmr) {
  // Part 1: Non-SWMR remove_by_idx targeting internal node indices (`found`
  // inside depth-2 and depth-1 internal nodes -> swap_loc), rightmost child
  // traversal (`idx == internal->nrec`), middle child (`0 < idx < nrec`),
  // both H5_ITER_INC and H5_ITER_DEC, callback error propagation, and full
  // root collapse depth 2 -> 1 -> 0.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());

    for (hsize_t k = 1; k <= 95; ++k) {
      H5B2_test_rec_t rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_GE(hdr->depth, 2u);

    // Error callback on remove_by_idx (head, middle, and tail).
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, FailingRemoveOp, nullptr),
          0);
      EXPECT_LT(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 35, FailingRemoveOp,
                                   nullptr),
                0);
      EXPECT_LT(H5B2_remove_by_idx(bt.get(), H5_ITER_DEC, 0, FailingRemoveOp,
                                   nullptr),
                0);
    }

    // Explicitly target the exact global index `node_ptrs[0].all_nrec` stored
    // inside the depth-2/3 root (`found = true` inside remove_internal_by_idx,
    // passing swap_loc into depth 1) repeatedly until depth drops below 2.
    while (hdr->depth >= 2) {
      H5B2_internal_t* root = H5B2__protect_internal(
          hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(root, nullptr);
      const hsize_t root_key_idx = root->node_ptrs[0].all_nrec;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, root_key_idx, nullptr,
                                   nullptr),
                0);
    }

    // Also test a scenario where remove_by_idx enters a child subtree where
    // internal->node_ptrs[0].node_nrec == merge_nrec while swap_loc is active,
    // hitting the swap_loc branch inside the while-retries loop too!
    ScopedBTree2 bt_swap(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt_swap.valid());
    for (hsize_t k = 1; k <= 75; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt_swap.get(), &rec), 0);
    }
    H5B2_hdr_t* h2 = bt_swap.get()->hdr;
    ASSERT_GE(h2->depth, 2u);
    // Repeatedly delete in reverse order from DEC midpoint, head, and tail.
    bool flip = false;
    while (h2->root.all_nrec > 0) {
      hsize_t n = h2->root.all_nrec;
      H5_iter_order_t order = flip ? H5_ITER_DEC : H5_ITER_INC;
      hsize_t target = (n > 4) ? (n / 3) : 0;
      ASSERT_GE(H5B2_remove_by_idx(bt_swap.get(), order, target, nullptr,
                                   nullptr),
                0);
      flip = !flip;
    }
    EXPECT_EQ(h2->depth, 0u);
  }

  // Part 2: SWMR H5B2_remove_by_idx across flushes from depth 2 -> 1 -> 0.
  {
    TempFile tmp("h5b2internal_swmr_remidx.h5");
    ScopedFile file = CreateSwmrTempFile(tmp);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    H5B2_create_t cparam{};
    cparam.cls = &kSwmrCustomClass;
    cparam.node_size = 128;
    cparam.rrec_size = 16;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
    ASSERT_TRUE(bt.valid());

    for (uint64_t k = 1; k <= 75; ++k) {
      CustomRec rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_GE(hdr->depth, 2u);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

    // First delete the depth-2 root keys by exact index under SWMR, then
    // collapse all the way down from head/middle/tail.
    while (hdr->depth >= 2) {
      H5B2_internal_t* root = H5B2__protect_internal(
          hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(root, nullptr);
      const hsize_t root_key_idx = root->node_ptrs[0].all_nrec;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, root_key_idx, nullptr,
                                   nullptr),
                0);
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    }
    while (hdr->root.all_nrec > 0) {
      ASSERT_GE(
          H5B2_remove_by_idx(bt.get(), H5_ITER_DEC, 0, nullptr, nullptr), 0);
    }
    EXPECT_EQ(hdr->depth, 0u);
  }
}

void BuildThreeLeafRootTree(H5B2_t* bt, H5F_t* f,
                            const std::array<uint16_t, 3>& target_counts,
                            std::array<hsize_t, 2>* out_seps) {
  H5B2_hdr_t* hdr = bt->hdr;
  hdr->f = f;
  hsize_t base_k = 1000;
  while (!(hdr->depth == 1 && hdr->root.node_nrec == 2)) {
    H5B2_test_rec_t rec{base_k, base_k};
    ASSERT_GE(H5B2_insert(bt, &rec), 0);
    base_k += 1000;
    ASSERT_LT(base_k, 100000u);
  }

  H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1,
                                                 false, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(root, nullptr);
  std::array<hsize_t, 2> seps{};
  for (size_t i = 0; i < 2; ++i) {
    seps[i] =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, i))->key;
  }
  ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                           H5AC__NO_FLAGS_SET),
            0);
  *out_seps = seps;

  // Pass 1: shrink above-target leaves (`>= 3`).
  for (int step = 0; step < 40; ++step) {
    root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                  H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->depth, 1u);
    ASSERT_EQ(root->nrec, 2u);
    std::array<uint16_t, 3> cur{
        root->node_ptrs[0].node_nrec,
        root->node_ptrs[1].node_nrec,
        root->node_ptrs[2].node_nrec,
    };
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
    bool shrunk = false;
    hsize_t offset = 0;
    for (size_t i = 0; i < 3; ++i) {
      if (cur[i] > target_counts[i]) {
        ASSERT_GE(
            H5B2_remove_by_idx(bt, H5_ITER_INC, offset, nullptr, nullptr), 0);
        shrunk = true;
        break;
      }
      offset += cur[i] + 1;
    }
    if (!shrunk) {
      break;
    }
  }

  // Pass 2: grow below-target leaves using H5B2_update so root does not split.
  std::array<hsize_t, 3> ins_cursors{100, seps[0] + 1, seps[1] + 1};
  ModifyParams noop_mp{0, false, false};
  for (int step = 0; step < 40; ++step) {
    root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                  H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->depth, 1u);
    ASSERT_EQ(root->nrec, 2u);
    std::array<uint16_t, 3> cur{
        root->node_ptrs[0].node_nrec,
        root->node_ptrs[1].node_nrec,
        root->node_ptrs[2].node_nrec,
    };
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
    if (cur == target_counts) {
      return;
    }
    bool grew = false;
    for (size_t i = 0; i < 3; ++i) {
      if (cur[i] < target_counts[i]) {
        H5B2_test_rec_t rec{ins_cursors[i]++, 1};
        ASSERT_GE(H5B2_update(bt, &rec, ModifyTest2Op, &noop_mp), 0);
        grew = true;
        break;
      }
    }
    ASSERT_TRUE(grew);
  }
  ADD_FAILURE() << "BuildThreeLeafRootTree did not converge";
}

void AdjustFiveLeafToTarget(H5B2_t* bt, H5F_t* f,
                            const std::array<uint16_t, 5>& initial_min3,
                            const std::array<uint16_t, 5>& final_counts,
                            std::array<hsize_t, 4>* out_seps) {
  BuildFiveLeafRootTree(bt, f, initial_min3, out_seps);
  H5B2_hdr_t* hdr = bt->hdr;
  // Shrink leaves with final_counts[i] == 2 individually from `3 -> 2`.
  for (size_t i = 0; i < 5; ++i) {
    if (final_counts[i] >= initial_min3[i]) {
      continue;
    }
    H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1,
                                                   false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    hsize_t offset = 0;
    for (size_t j = 0; j < i; ++j) {
      offset += root->node_ptrs[j].node_nrec + 1;
    }
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5B2_remove_by_idx(bt, H5_ITER_INC, offset, nullptr, nullptr), 0);
  }
}

TEST(H5B2internalTest, MiddleLeftMoreRemoveDuplicatePromotedAndRetryCompare) {
  // Part 1: H5B2__remove_internal middle child with right neighbor == merge
  // (`== 2`) and left neighbor > merge (`== 3`) -> evaluates line 920.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    std::array<hsize_t, 4> seps{};
    AdjustFiveLeafToTarget(bt.get(), f, {5, 3, 3, 3, 3}, {5, 2, 2, 3, 3}, &seps);

    // Protect leaf 1 R/O to read its first key, then remove that key.
    H5B2_hdr_t* hdr = bt.get()->hdr;
    H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1,
                                                   false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    H5B2_leaf_t* l1 = H5B2__protect_leaf(hdr, root, &root->node_ptrs[1], false,
                                         H5AC__READ_ONLY_FLAG);
    ASSERT_NE(l1, nullptr);
    const hsize_t target_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_LEAF_NREC(l1, hdr, 0))->key;
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, root->node_ptrs[1].addr, l1,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    H5B2_test_rec_t rem{target_key, 0};
    EXPECT_GE(H5B2_remove(bt.get(), &rem, nullptr, nullptr), 0);
  }

  // Part 2: Duplicate promoted record in H5B2__insert_internal retry loop
  // (`H5E_EXISTS` after `split1` promotes child's middle key into internal).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    std::array<hsize_t, 2> seps{};
    BuildThreeLeafRootTree(bt.get(), f, {7, 7, 3}, &seps);

    H5B2_hdr_t* hdr = bt.get()->hdr;
    H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1,
                                                   false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    H5B2_leaf_t* l0 = H5B2__protect_leaf(hdr, root, &root->node_ptrs[0], false,
                                         H5AC__READ_ONLY_FLAG);
    ASSERT_NE(l0, nullptr);
    const hsize_t mid_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_LEAF_NREC(l0, hdr, 7 / 2))
            ->key;
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, root->node_ptrs[0].addr, l0,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    // Calling H5B2_insert on `mid_key` splits full leaf[0] and promotes
    // `mid_key` into root; post-split locate_record detects `cmp == 0`!
    H5B2_test_rec_t dup{mid_key, 99};
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_insert(bt.get(), &dup), 0);
  }

  // Part 3: Compare failure inside retry loop of H5B2__insert_internal AND
  // H5B2__remove_internal.
  {
    g_fail_compare = false;
    g_fail_compare_after = -1;
    auto cleanup_cmp = absl::MakeCleanup([] {
      g_fail_compare = false;
      g_fail_compare_after = -1;
    });

    // Sub-case A: insert_internal retry compare failure.
    {
      ScopedFile file = CreateInMemoryFile();
      ASSERT_TRUE(file.valid());
      H5F_t* f = GetFilePtr(file.get());
      ScopedApiContext ctx;
      H5B2_create_t cparam{};
      cparam.cls = &kConditionalFailClass;
      cparam.node_size = 128;
      cparam.rrec_size = 16;
      cparam.split_percent = 100;
      cparam.merge_percent = 30;
      ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
      ASSERT_TRUE(bt.valid());
      for (uint64_t k = 1; k <= 10; ++k) {
        CustomRec rec{k * 100, k};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      }
      H5B2_hdr_t* hdr = bt.get()->hdr;
      // Top up leaf[0] to full (`7`) without splitting root (`nrec == 1`).
      for (int i = 0; i < 6; ++i) {
        H5B2_internal_t* root = H5B2__protect_internal(
            hdr, hdr, &hdr->root, 1, false, H5AC__READ_ONLY_FLAG);
        ASSERT_NE(root, nullptr);
        uint16_t n0 = root->node_ptrs[0].node_nrec;
        ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                                 H5AC__NO_FLAGS_SET),
                  0);
        if (n0 >= 7) {
          break;
        }
        CustomRec rec{static_cast<uint64_t>(10 + i), 1};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      }
      // root->nrec == 1 -> initial locate_record makes 1 compare call, then
      // redistribute2 (0 compares), then retry locate_record fails on call 2.
      ASSERT_EQ(bt.get()->hdr->root.node_nrec, 1u);
      g_fail_compare_after = 1;
      CustomRec probe{1, 1};
      ScopedErrorSilence silence;
      EXPECT_LT(H5B2_insert(bt.get(), &probe), 0);
      g_fail_compare_after = -1;
    }

    // Sub-case B: remove_internal retry compare failure (`root->nrec == 1`,
    // left == 2 == merge_nrec, right == 5 > 2 -> `left + right = 7 > 5`).
    {
      ScopedFile file = CreateInMemoryFile();
      ASSERT_TRUE(file.valid());
      H5F_t* f = GetFilePtr(file.get());
      ScopedApiContext ctx;
      H5B2_create_t cparam{};
      cparam.cls = &kConditionalFailClass;
      cparam.node_size = 128;
      cparam.rrec_size = 16;
      cparam.split_percent = 100;
      cparam.merge_percent = 30;
      ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
      ASSERT_TRUE(bt.valid());
      for (uint64_t k = 1; k <= 8; ++k) {
        CustomRec rec{k * 100, k};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      }
      H5B2_hdr_t* hdr = bt.get()->hdr;
      ASSERT_EQ(hdr->depth, 1u);
      ASSERT_EQ(hdr->root.node_nrec, 1u);
      // Adjust to left=2, right=5 using H5B2_remove_by_idx & H5B2_insert.
      for (int step = 0; step < 10; ++step) {
        H5B2_internal_t* root = H5B2__protect_internal(
            hdr, hdr, &hdr->root, 1, false, H5AC__READ_ONLY_FLAG);
        ASSERT_NE(root, nullptr);
        uint16_t n0 = root->node_ptrs[0].node_nrec;
        uint16_t n1 = root->node_ptrs[1].node_nrec;
        ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                                 H5AC__NO_FLAGS_SET),
                  0);
        if (n0 == 2 && n1 == 5) {
          break;
        }
        if (n0 > 2) {
          ASSERT_GE(
              H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, nullptr, nullptr),
              0);
        } else if (n1 < 5) {
          CustomRec ins{10000 + static_cast<uint64_t>(step), 1};
          ASSERT_GE(H5B2_insert(bt.get(), &ins), 0);
        }
      }
      g_fail_compare_after = 1;
      CustomRec probe{100, 0};
      ScopedErrorSilence silence;
      EXPECT_LT(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
      g_fail_compare_after = -1;
    }
  }
}

TEST(H5B2internalTest, ProtectShadowErrorCleanupAndProxyAttach) {
  TempFile tmp("h5b2internal_shadow_ro.h5");
  ScopedFile file = CreateSwmrTempFile(tmp);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = &kSwmrCustomClass;
  cparam.node_size = 128;
  cparam.rrec_size = 16;
  cparam.split_percent = 100;
  cparam.merge_percent = 30;
  ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
  ASSERT_TRUE(bt.valid());
  H5B2_hdr_t* hdr = bt.get()->hdr;
  hdr->f = f;

  for (uint64_t k = 1; k <= 55; ++k) {
    CustomRec rec{k * 10, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  ASSERT_GE(hdr->depth, 2u);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  // Protect depth-2 root R/O so we have a parent, then protect child[0] with
  // shadow=true and H5AC__READ_ONLY_FLAG: H5AC_move_entry rejects moving a
  // read-only entry, exercising H5B2__shadow_internal failure AND cleanup in
  // H5B2__protect_internal (detaching top_proxy + unprotecting internal).
  H5B2_internal_t* root = H5B2__protect_internal(
      hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(root, nullptr);
  H5B2_node_ptr_t c0_ptr = root->node_ptrs[0];
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5B2__protect_internal(hdr, root, &c0_ptr,
                               static_cast<uint16_t>(hdr->depth - 1), true,
                               H5AC__READ_ONLY_FLAG),
        nullptr);
  }

  // Because cleanup cleared `c0->top_proxy`, protecting `c0` again attaches it
  // back to `hdr->top_proxy` cleanly.
  H5B2_internal_t* c0 = H5B2__protect_internal(
      hdr, root, &c0_ptr, static_cast<uint16_t>(hdr->depth - 1), false,
      H5AC__NO_FLAGS_SET);
  ASSERT_NE(c0, nullptr);
  EXPECT_EQ(c0->top_proxy, hdr->top_proxy);
  EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, c0_ptr.addr, c0,
                           H5AC__NO_FLAGS_SET),
            0);
  EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                           H5AC__NO_FLAGS_SET),
            0);
}

TEST(H5B2internalTest, InternalErrorPropagationAndPreProtectedNodes) {
  // Part 1: Protect root R/W -> all entry points fail at H5B2__protect_internal
  // cleanly without mutating state.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 20; ++k) {
      H5B2_test_rec_t rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    hdr->f = f;
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5B2__protect_internal(hdr, hdr, &hdr->root, hdr->depth, false,
                                       H5AC__NO_FLAGS_SET),
                nullptr);
      H5B2_test_rec_t rec{15, 0};
      H5B2_test_rec_t out{};
      EXPECT_LT(H5B2_neighbor(bt.get(), H5B2_COMPARE_GREATER, &rec,
                              CaptureFoundTest2, &out),
                0);
      EXPECT_LT(H5B2_insert(bt.get(), &rec), 0);
      ModifyParams mp{1, true, false};
      EXPECT_LT(H5B2_update(bt.get(), &rec, ModifyTest2Op, &mp), 0);
      EXPECT_LT(H5B2_remove(bt.get(), &rec, nullptr, nullptr), 0);
      EXPECT_LT(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, nullptr, nullptr),
                0);
    }
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
  }

  // Helper: on a 3-leaf depth-1 tree (`nrec == 2`), pre-protect `lock_leaf` R/W
  // then run `fn(bt)` inside ScopedErrorSilence, unprotecting cleanly after.
  auto run_with_three_leaf_locked = [](const std::array<uint16_t, 3>& counts,
                                       size_t lock_leaf, auto fn) {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    std::array<hsize_t, 2> seps{};
    BuildThreeLeafRootTree(bt.get(), f, counts, &seps);

    H5B2_hdr_t* hdr = bt.get()->hdr;
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, 1, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    H5B2_node_ptr_t leaf_ptr = root->node_ptrs[lock_leaf];
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    H5B2_leaf_t* leaf =
        H5B2__protect_leaf(hdr, hdr, &leaf_ptr, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(leaf, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(fn(bt.get(), seps), 0);
    }
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, leaf_ptr.addr, leaf,
                             H5AC__NO_FLAGS_SET),
              0);
    bt.reset();
    EXPECT_GE(H5Fclose(file.release()), 0);
  };

  // Part 2: Pre-protected child failures inside H5B2__insert_internal (all 6
  // redistribute/split branches at lines 401, 407, 413, 419, 426, 432).
  // For `{3, 3, 7}`, `H5B2__redistribute2(idx-1 = 1)` protects `left_leaf`
  // (`leaf[1]`) first, so lock `leaf[1]` to avoid an upstream double-protect
  // error path leaving `leaf[1]` unprotected in cache.
  run_with_three_leaf_locked({7, 3, 3}, 0, [](H5B2_t* bt, auto& seps) {
    H5B2_test_rec_t rec{50, 1};
    return H5B2_insert(bt, &rec);
  });
  run_with_three_leaf_locked({7, 7, 3}, 0, [](H5B2_t* bt, auto& seps) {
    H5B2_test_rec_t rec{50, 1};
    return H5B2_insert(bt, &rec);
  });
  run_with_three_leaf_locked({3, 3, 7}, 1, [](H5B2_t* bt, auto& seps) {
    H5B2_test_rec_t rec{seps[1] + 50, 1};
    return H5B2_insert(bt, &rec);
  });
  run_with_three_leaf_locked({3, 7, 7}, 2, [](H5B2_t* bt, auto& seps) {
    H5B2_test_rec_t rec{seps[1] + 50, 1};
    return H5B2_insert(bt, &rec);
  });
  run_with_three_leaf_locked({3, 7, 3}, 0, [](H5B2_t* bt, auto& seps) {
    H5B2_test_rec_t rec{seps[0] + 50, 1};
    return H5B2_insert(bt, &rec);
  });
  run_with_three_leaf_locked({7, 7, 7}, 1, [](H5B2_t* bt, auto& seps) {
    H5B2_test_rec_t rec{seps[0] + 50, 1};
    return H5B2_insert(bt, &rec);
  });

  // Part 3: H5B2__update_internal sideways dodge -> H5B2__insert_internal fail
  // (`leaf[0]` full (`7`), triggering `INSERT_CHILD_FULL` dodge to
  // `H5B2__insert_internal`, where comparison fails cleanly inside
  // `insert_internal` with no leaked cache protections).
  {
    g_fail_compare = false;
    g_fail_compare_after = -1;
    auto cleanup_cmp = absl::MakeCleanup([] {
      g_fail_compare = false;
      g_fail_compare_after = -1;
    });

    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    H5B2_create_t cparam{};
    cparam.cls = &kConditionalFailClass;
    cparam.node_size = 128;
    cparam.rrec_size = 16;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;
    ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
    ASSERT_TRUE(bt.valid());
    for (uint64_t k = 1; k <= 10; ++k) {
      CustomRec rec{k * 100, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    uint16_t leaf0_nrec = 0;
    for (int i = 0; i < 6; ++i) {
      H5B2_internal_t* root = H5B2__protect_internal(
          hdr, hdr, &hdr->root, 1, false, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(root, nullptr);
      leaf0_nrec = root->node_ptrs[0].node_nrec;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      if (leaf0_nrec >= 7) {
        break;
      }
      CustomRec rec{static_cast<uint64_t>(10 + i), 1};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    ASSERT_EQ(hdr->depth, 1u);
    ASSERT_EQ(hdr->root.node_nrec, 1u);
    ASSERT_EQ(leaf0_nrec, 7u);

    // For `root->nrec == 1` (1 compare) and `leaf[0]->nrec == 7` (3 compares),
    // `update_internal` + `update_leaf` perform 4 compares before returning
    // `H5B2_UPDATE_INSERT_CHILD_FULL`; the 5th compare fails inside
    // `H5B2__insert_internal`, exercising H5B2internal.c line 672 cleanly.
    g_fail_compare_after = 4;
    ModifyParams mp{99, true, false};
    CustomRec rec{1, 99};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5B2_update(bt.get(), &rec, ModifyCustomOp, &mp), 0);
    }
    g_fail_compare_after = -1;
    bt.reset();
    EXPECT_GE(H5Fclose(file.release()), 0);
  }

  // Part 4: Pre-protected child failures inside H5B2__remove_internal AND
  // H5B2__remove_internal_by_idx (root collapse, all 6 rebalance branches, and
  // swap_leaf).
  auto run_with_five_leaf_locked = [](const std::array<uint16_t, 5>& init3,
                                      const std::array<uint16_t, 5>& counts,
                                      size_t lock_leaf, auto fn) {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    std::array<hsize_t, 4> seps{};
    AdjustFiveLeafToTarget(bt.get(), f, init3, counts, &seps);

    H5B2_hdr_t* hdr = bt.get()->hdr;
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, 1, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    H5B2_node_ptr_t leaf_ptr = root->node_ptrs[lock_leaf];
    H5B2_leaf_t* target_leaf = H5B2__protect_leaf(
        hdr, root, &root->node_ptrs[lock_leaf], false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(target_leaf, nullptr);
    const hsize_t locked_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_LEAF_NREC(target_leaf, hdr, 0))
            ->key;
    // Also grab first key of every leaf for removing when lock_leaf != target.
    std::array<hsize_t, 5> leaf_first_keys{};
    for (size_t i = 0; i < 5; ++i) {
      if (i == lock_leaf) {
        leaf_first_keys[i] = locked_key;
        continue;
      }
      H5B2_leaf_t* li = H5B2__protect_leaf(hdr, root, &root->node_ptrs[i],
                                           false, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(li, nullptr);
      leaf_first_keys[i] =
          reinterpret_cast<H5B2_test_rec_t*>(H5B2_LEAF_NREC(li, hdr, 0))->key;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, root->node_ptrs[i].addr, li,
                               H5AC__NO_FLAGS_SET),
                0);
    }
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, leaf_ptr.addr, target_leaf,
                             H5AC__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    H5B2_leaf_t* leaf =
        H5B2__protect_leaf(hdr, hdr, &leaf_ptr, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(leaf, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(fn(bt.get(), seps, leaf_first_keys), 0);
    }
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, leaf_ptr.addr, leaf,
                             H5AC__NO_FLAGS_SET),
              0);
    bt.reset();
    EXPECT_GE(H5Fclose(file.release()), 0);
  };

  // idx == 0 redistribute2 / merge2 fail in remove & remove_by_idx.
  run_with_five_leaf_locked(
      {3, 4, 3, 3, 3}, {2, 4, 3, 3, 3}, 0, [](H5B2_t* bt, auto&, auto& keys) {
        H5B2_test_rec_t rec{keys[0], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 4, 3, 3, 3}, {2, 4, 3, 3, 3}, 0, [](H5B2_t* bt, auto&, auto&) {
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 0, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {2, 2, 3, 3, 3}, 0, [](H5B2_t* bt, auto&, auto& keys) {
        H5B2_test_rec_t rec{keys[0], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {2, 2, 3, 3, 3}, 0, [](H5B2_t* bt, auto&, auto&) {
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 0, nullptr, nullptr);
      });

  // idx == nrec redistribute2 / merge2 fail in remove & remove_by_idx.
  run_with_five_leaf_locked(
      {3, 3, 3, 4, 3}, {3, 3, 3, 4, 2}, 3, [](H5B2_t* bt, auto&, auto& keys) {
        H5B2_test_rec_t rec{keys[4], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 4, 3}, {3, 3, 3, 4, 2}, 3, [](H5B2_t* bt, auto&, auto&) {
        // index inside leaf[4]: 3+1 + 3+1 + 3+1 + 4+1 = 17
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 17, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {3, 3, 3, 2, 2}, 3, [](H5B2_t* bt, auto&, auto& keys) {
        H5B2_test_rec_t rec{keys[4], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {3, 3, 3, 2, 2}, 3, [](H5B2_t* bt, auto&, auto&) {
        // index inside leaf[4]: 3+1 + 3+1 + 3+1 + 2+1 = 15
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 15, nullptr, nullptr);
      });

  // 0 < idx < nrec redistribute3 / merge3 fail in remove & remove_by_idx.
  run_with_five_leaf_locked(
      {4, 3, 3, 3, 3}, {4, 2, 3, 3, 3}, 0, [](H5B2_t* bt, auto&, auto& keys) {
        H5B2_test_rec_t rec{keys[1], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {4, 3, 3, 3, 3}, {4, 2, 3, 3, 3}, 0, [](H5B2_t* bt, auto&, auto&) {
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 5, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {2, 2, 2, 3, 3}, 0, [](H5B2_t* bt, auto&, auto& keys) {
        H5B2_test_rec_t rec{keys[1], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {2, 2, 2, 3, 3}, 0, [](H5B2_t* bt, auto&, auto&) {
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 3, nullptr, nullptr);
      });

  // swap_leaf fail (958 & 1249) when removing separator seps[0] with leaf[1]
  // pre-protected R/W.
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {3, 3, 3, 3, 3}, 1, [](H5B2_t* bt, auto& seps, auto&) {
        H5B2_test_rec_t rec{seps[0], 0};
        return H5B2_remove(bt, &rec, nullptr, nullptr);
      });
  run_with_five_leaf_locked(
      {3, 3, 3, 3, 3}, {3, 3, 3, 3, 3}, 1, [](H5B2_t* bt, auto&, auto&) {
        return H5B2_remove_by_idx(bt, H5_ITER_INC, 3, nullptr, nullptr);
      });

  // Root-collapse merge2 fail (823 & 1067): build nrec == 1 with 2+2 leaves.
  auto run_root_collapse_locked = [](auto fn) {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 8; ++k) {
      H5B2_test_rec_t rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    // Shrink to left=2, right=3 (`nrec == 1`, `2+3=5 <= (merge_nrec*2)+1` ->
    // next remove triggers root collapse!).
    while (true) {
      H5B2_internal_t* root = H5B2__protect_internal(
          hdr, hdr, &hdr->root, 1, false, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(root, nullptr);
      ASSERT_EQ(root->nrec, 1u);
      const uint16_t n0 = root->node_ptrs[0].node_nrec;
      const uint16_t n1 = root->node_ptrs[1].node_nrec;
      H5B2_node_ptr_t leaf0_ptr = root->node_ptrs[0];
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      if (n0 > 2) {
        ASSERT_GE(
            H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, nullptr, nullptr), 0);
      } else if (n1 > 3) {
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, n0 + 1, nullptr,
                                     nullptr),
                  0);
      } else {
        H5B2_leaf_t* leaf0 = H5B2__protect_leaf(hdr, hdr, &leaf0_ptr, false,
                                                H5AC__NO_FLAGS_SET);
        ASSERT_NE(leaf0, nullptr);
        {
          ScopedErrorSilence silence;
          EXPECT_LT(fn(bt.get()), 0);
        }
        EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF, leaf0_ptr.addr, leaf0,
                                 H5AC__NO_FLAGS_SET),
                  0);
        break;
      }
    }
    bt.reset();
    EXPECT_GE(H5Fclose(file.release()), 0);
  };

  run_root_collapse_locked([](H5B2_t* bt) {
    H5B2_test_rec_t rec{10, 0};
    return H5B2_remove(bt, &rec, nullptr, nullptr);
  });
  run_root_collapse_locked([](H5B2_t* bt) {
    return H5B2_remove_by_idx(bt, H5_ITER_INC, 0, nullptr, nullptr);
  });
}

}  // namespace
}  // namespace hdf5
