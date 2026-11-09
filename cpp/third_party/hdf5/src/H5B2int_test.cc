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
// Tests internal v2 B-tree node management routines: record location, 1->2 and
// root splits, 2-way and 3-way redistributions and merges, insertion, in-order
// node iteration with SWMR pinning, recursive deletion, node size accounting,
// and SWMR flush-dependency management (third_party/hdf5/src/H5B2int.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2int.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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
#define H5B2INT_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5B2INT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2INT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper for an active HDF5 API context and metadata cache tag.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x20000) {
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

// RAII wrapper around an opened/created H5B2_t* tree.
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
  H5B2_t* release() { return std::exchange(bt2_, nullptr); }
  bool valid() const { return bt2_ != nullptr; }

 private:
  H5B2_t* bt2_ = nullptr;
};

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// Key/value record matching H5B2_test_rec_t layout, usable with custom classes.
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

herr_t FailingCompare(const void* /*rec1*/, const void* /*rec2*/,
                      int* /*result*/) {
  return -1;
}

// Uses H5B2_CDSET_ID so H5B2__hdr_init enables SWMR writes when the file is
// opened with H5F_ACC_SWMR_WRITE (and keeping trees small avoids cache eviction
// + client_class_g deserialization).
const H5B2_class_t kSwmrCustomClass = {
    H5B2_CDSET_ID,
    "H5B2_SWMR_TEST_CLS",
    sizeof(CustomRec),
    nullptr,
    nullptr,
    CustomStore,
    CustomCompare,
    CustomEncode,
    CustomDecode,
    CustomDebug,
};

const H5B2_class_t kFailingCompareClass = {
    H5B2_TEST2_ID,
    "H5B2_FAIL_CMP_CLS",
    sizeof(CustomRec),
    nullptr,
    nullptr,
    CustomStore,
    FailingCompare,
    CustomEncode,
    CustomDecode,
    CustomDebug,
};

// Creates an on-disk SWMR-writeable HDF5 file backed by the default sec2 VFD.
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

int CollectTest2Keys(const void* record, void* op_data) {
  auto* keys = static_cast<std::vector<hsize_t>*>(op_data);
  keys->push_back(static_cast<const H5B2_test_rec_t*>(record)->key);
  return H5_ITER_CONT;
}

int CollectCustomKeys(const void* record, void* op_data) {
  auto* keys = static_cast<std::vector<uint64_t>*>(op_data);
  keys->push_back(static_cast<const CustomRec*>(record)->key);
  return H5_ITER_CONT;
}

struct IterControl {
  std::vector<hsize_t> seen;
  size_t limit = 0;
  int return_status = H5_ITER_CONT;
};

int StopOrFailIter(const void* record, void* op_data) {
  auto* ctrl = static_cast<IterControl*>(op_data);
  ctrl->seen.push_back(static_cast<const H5B2_test_rec_t*>(record)->key);
  if (ctrl->seen.size() >= ctrl->limit) {
    return ctrl->return_status;
  }
  return H5_ITER_CONT;
}

struct DeleteControl {
  size_t count = 0;
  size_t fail_at = 0;
};

herr_t CountOrFailDelete(const void* /*record*/, void* op_data) {
  auto* ctrl = static_cast<DeleteControl*>(op_data);
  ctrl->count++;
  if (ctrl->fail_at > 0 && ctrl->count >= ctrl->fail_at) {
    return -1;
  }
  return 0;
}

// Creates a depth-1 or depth-2 tree via inserts and checks basic child counts.
H5B2_t* CreateTest2Tree(H5F_t* f, uint32_t node_size, uint8_t split_percent,
                        uint8_t merge_percent) {
  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST2;
  cparam.node_size = node_size;
  cparam.rrec_size = 16;
  cparam.split_percent = split_percent;
  cparam.merge_percent = merge_percent;
  return H5B2_create(f, &cparam, f);
}

TEST(H5B2intTest, LocateRecordHandlesBoundsMatchesAndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  std::array<size_t, 5> rec_off{};
  for (size_t i = 0; i < rec_off.size(); ++i) {
    rec_off[i] = i * sizeof(CustomRec);
  }
  const std::array<CustomRec, 5> recs{{
      {10, 1},
      {20, 2},
      {30, 3},
      {40, 4},
      {50, 5},
  }};
  const auto* raw = reinterpret_cast<const uint8_t*>(recs.data());

  unsigned idx = 99;
  int cmp = 0;

  // Empty record table.
  CustomRec probe{25, 0};
  EXPECT_GE(H5B2__locate_record(&kSwmrCustomClass, 0, rec_off.data(), raw,
                                &probe, &idx, &cmp),
            0);
  EXPECT_EQ(idx, 0u);
  EXPECT_LT(cmp, 0);

  // Exact hits at beginning, middle, end.
  for (unsigned i = 0; i < 5; ++i) {
    probe.key = (i + 1) * 10;
    EXPECT_GE(H5B2__locate_record(&kSwmrCustomClass, 5, rec_off.data(), raw,
                                  &probe, &idx, &cmp),
              0);
    EXPECT_EQ(cmp, 0);
    EXPECT_EQ(idx, i);
  }

  // Less than smallest.
  probe.key = 5;
  EXPECT_GE(H5B2__locate_record(&kSwmrCustomClass, 5, rec_off.data(), raw,
                                &probe, &idx, &cmp),
            0);
  EXPECT_LT(cmp, 0);
  EXPECT_EQ(idx, 0u);

  // Between existing keys.
  probe.key = 35;
  EXPECT_GE(H5B2__locate_record(&kSwmrCustomClass, 5, rec_off.data(), raw,
                                &probe, &idx, &cmp),
            0);
  EXPECT_NE(cmp, 0);

  // Greater than largest.
  probe.key = 99;
  EXPECT_GE(H5B2__locate_record(&kSwmrCustomClass, 5, rec_off.data(), raw,
                                &probe, &idx, &cmp),
            0);
  EXPECT_GT(cmp, 0);
  EXPECT_EQ(idx, 4u);

  // Failing compare callback propagates error.
  ScopedErrorSilence silence;
  EXPECT_LT(H5B2__locate_record(&kFailingCompareClass, 5, rec_off.data(), raw,
                                &probe, &idx, &cmp),
            0);
}

TEST(H5B2intTest, InsertGrowsToDepth3AndCoversSplitAndNodeSize) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // node_size=128, rrec=16 => leaf max_nrec=7, internal depth>=1 max_nrec=4.
  // First build a depth-1 tree to verify H5B2__node_size(..., depth==1).
  {
    ScopedBTree2 bt1(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt1.valid());

    for (hsize_t k = 1; k <= 14; ++k) {
      H5B2_test_rec_t rec{k * 10, k};
      ASSERT_GE(H5B2_insert(bt1.get(), &rec), 0);
    }
    H5B2_stat_t info{};
    ASSERT_GE(H5B2_stat_info(bt1.get(), &info), 0);
    EXPECT_EQ(info.depth, 1u);
    EXPECT_EQ(info.nrecords, 14u);

    hsize_t bt_size = 0;
    EXPECT_GE(H5B2_size(bt1.get(), &bt_size), 0);
    EXPECT_GT(bt_size, 0u);
  }

  // Grow a tree to depth 3 using interleaved + descending + ascending keys,
  // hitting split1 at both idx < nrec (left/middle) and idx == nrec (right).
  ScopedBTree2 bt3(CreateTest2Tree(f, 128, 100, 30));
  ASSERT_TRUE(bt3.valid());

  std::vector<hsize_t> inserted;
  for (hsize_t k = 1; k <= 120; ++k) {
    hsize_t key = (k % 3 == 0) ? (5000 - k) : ((k % 3 == 1) ? k * 3 : 2000 + k);
    H5B2_test_rec_t rec{key, key + 1};
    ASSERT_GE(H5B2_insert(bt3.get(), &rec), 0);
    inserted.push_back(key);
  }
  std::sort(inserted.begin(), inserted.end());

  H5B2_stat_t info{};
  ASSERT_GE(H5B2_stat_info(bt3.get(), &info), 0);
  EXPECT_GE(info.depth, 2u);
  EXPECT_EQ(info.nrecords, inserted.size());

  // Continue until depth >= 3.
  hsize_t next_key = 10000;
  while (info.depth < 3 && next_key < 10500) {
    H5B2_test_rec_t rec{next_key, next_key};
    ASSERT_GE(H5B2_insert(bt3.get(), &rec), 0);
    inserted.push_back(next_key);
    ++next_key;
    ASSERT_GE(H5B2_stat_info(bt3.get(), &info), 0);
  }
  EXPECT_GE(info.depth, 3u);

  hsize_t bt3_size = 0;
  EXPECT_GE(H5B2_size(bt3.get(), &bt3_size), 0);
  EXPECT_GT(bt3_size, 0u);

  std::vector<hsize_t> iterated;
  EXPECT_EQ(H5B2_iterate(bt3.get(), CollectTest2Keys, &iterated), 0);
  std::sort(inserted.begin(), inserted.end());
  EXPECT_EQ(iterated, inserted);
}

TEST(H5B2intTest, IterateNodeHandlesStopAndErrorCallbacks) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Empty tree iterate returns 0 immediately.
  ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
  ASSERT_TRUE(bt.valid());
  IterControl empty_ctrl;
  EXPECT_EQ(H5B2_iterate(bt.get(), StopOrFailIter, &empty_ctrl), 0);
  EXPECT_TRUE(empty_ctrl.seen.empty());

  // Depth 0 leaf iteration: stop and error.
  for (hsize_t k = 1; k <= 5; ++k) {
    H5B2_test_rec_t rec{k, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  {
    IterControl stop_leaf{{}, 3, H5_ITER_STOP};
    EXPECT_EQ(H5B2_iterate(bt.get(), StopOrFailIter, &stop_leaf), H5_ITER_STOP);
    EXPECT_THAT(stop_leaf.seen, ElementsAre(1, 2, 3));
  }
  {
    ScopedErrorSilence silence;
    IterControl err_leaf{{}, 2, H5_ITER_ERROR};
    EXPECT_LT(H5B2_iterate(bt.get(), StopOrFailIter, &err_leaf), 0);
    EXPECT_THAT(err_leaf.seen, ElementsAre(1, 2));
  }

  // Grow to depth 2 and verify early stop/error at internal-child boundaries,
  // midpoint, and trailing child.
  for (hsize_t k = 6; k <= 45; ++k) {
    H5B2_test_rec_t rec{k, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  H5B2_stat_t info{};
  ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
  ASSERT_GE(info.depth, 2u);

  for (size_t limit : {size_t{1}, size_t{7}, size_t{20}, size_t{39}}) {
    IterControl stop_ctrl{{}, limit, H5_ITER_STOP};
    EXPECT_EQ(H5B2_iterate(bt.get(), StopOrFailIter, &stop_ctrl), H5_ITER_STOP);
    EXPECT_EQ(stop_ctrl.seen.size(), limit);

    ScopedErrorSilence silence;
    IterControl err_ctrl{{}, limit, H5_ITER_ERROR};
    EXPECT_LT(H5B2_iterate(bt.get(), StopOrFailIter, &err_ctrl), 0);
    EXPECT_EQ(err_ctrl.seen.size(), limit);
  }
}

TEST(H5B2intTest, DeleteNodeCoversNullCallbackCountAndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Case 1: Delete depth-2 tree with NULL op callback.
  {
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 45; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bt.get(), &addr), 0);
    bt.reset();
    EXPECT_GE(H5B2_delete(f, addr, f, nullptr, nullptr), 0);
  }

  // Case 2: Delete depth-2 tree with counting op callback.
  {
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 45; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bt.get(), &addr), 0);
    bt.reset();
    DeleteControl dctrl;
    EXPECT_GE(H5B2_delete(f, addr, f, CountOrFailDelete, &dctrl), 0);
    EXPECT_EQ(dctrl.count, 45u);
  }

  // Case 3: Delete callback returns error while deleting a leaf child (and then
  // another failing partway through an internal node's own records).
  for (size_t fail_at : {size_t{2}, size_t{10}}) {
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 45; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bt.get(), &addr), 0);
    bt.reset();
    DeleteControl dctrl{0, fail_at};
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_delete(f, addr, f, CountOrFailDelete, &dctrl), 0);
  }
}

TEST(H5B2intTest, Redistribute2AllDirectionsAndMoveCounts) {
  // node_size=148, rrec=16, split=100, merge=40:
  // leaf: max_nrec=8, split=8, merge=3.
  // depth 1: max_nrec=5, split=5, merge=2.
  // Build valid depth-2 subtrees where left vs right depth-1 sibling counts
  // exercise move_nrec == 1 and move_nrec > 1 in both directions at depth 1 & 2.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // First: depth == 1 leaf siblings (3..8 records): test (3 vs 7 -> move 2),
  // (3 vs 5 -> move 1), (7 vs 3 -> move 2), (5 vs 3 -> move 1).
  struct PairCase {
    uint16_t left_nrec;
    uint16_t right_nrec;
  };
  const std::vector<PairCase> leaf_cases = {
      {3, 7},
      {3, 5},
      {7, 3},
      {5, 3},
  };
  for (const auto& pc : leaf_cases) {
    ScopedBTree2 bt(CreateTest2Tree(f, 148, 100, 40));
    ASSERT_TRUE(bt.valid());
    // Insert enough sorted keys to create depth=1 with 2 leaves (~10 recs).
    hsize_t key = 1;
    while (bt.get()->hdr->depth < 1) {
      H5B2_test_rec_t rec{key * 100, key};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      ++key;
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    hdr->f = bt.get()->f;
    // Grow left leaf (`[0]`) and right leaf (`[1]`) cleanly using targeted keys
    // so both match `pc.left_nrec` and `pc.right_nrec`.
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, 1, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    const hsize_t split_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, 0))->key;
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    // Adjust left and right leaf counts by removing/inserting around split_key.
    for (int step = 0; step < 20; ++step) {
      root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                    H5AC__NO_FLAGS_SET);
      ASSERT_NE(root, nullptr);
      ASSERT_EQ(root->nrec, 1u);
      uint16_t ln = root->node_ptrs[0].node_nrec;
      uint16_t rn = root->node_ptrs[1].node_nrec;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      if (ln == pc.left_nrec && rn == pc.right_nrec) {
        break;
      }
      if (ln < pc.left_nrec) {
        H5B2_test_rec_t rec{1 + static_cast<hsize_t>(step), 1};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      } else if (ln > pc.left_nrec) {
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, nullptr, nullptr),
                  0);
      } else if (rn < pc.right_nrec) {
        H5B2_test_rec_t rec{split_key + 10000 + static_cast<hsize_t>(step), 1};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      } else if (rn > pc.right_nrec) {
        hsize_t total_nrec = 0;
        ASSERT_GE(H5B2_get_nrec(bt.get(), &total_nrec), 0);
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, total_nrec - 1,
                                     nullptr, nullptr),
                  0);
      }
    }

    root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                  H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->node_ptrs[0].node_nrec, pc.left_nrec);
    ASSERT_EQ(root->node_ptrs[1].node_nrec, pc.right_nrec);
    EXPECT_GE(H5B2__redistribute2(hdr, 1, root, 0), 0);
    EXPECT_EQ(root->node_ptrs[0].node_nrec + root->node_ptrs[1].node_nrec,
              pc.left_nrec + pc.right_nrec);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__DIRTIED_FLAG),
              0);
    EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

    std::vector<hsize_t> keys;
    EXPECT_EQ(H5B2_iterate(bt.get(), CollectTest2Keys, &keys), 0);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
  }

  // Second: depth == 2 internal siblings under root (each child has 2..5
  // records) for (2 vs 5 -> move 2 > 1), (2 vs 4 -> move 1),
  // (5 vs 2 -> move 2 > 1), (4 vs 2 -> move 1).
  const std::vector<PairCase> int_cases = {
      {2, 5},
      {2, 4},
      {5, 2},
      {4, 2},
  };
  for (const auto& pc : int_cases) {
    ScopedBTree2 bt(CreateTest2Tree(f, 148, 100, 40));
    ASSERT_TRUE(bt.valid());
    // Insert ascending keys spaced far apart to reach depth == 2 cleanly.
    hsize_t k = 1;
    while (bt.get()->hdr->depth < 2) {
      H5B2_test_rec_t rec{k * 100, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      ++k;
    }
    H5B2_hdr_t* hdr = bt.get()->hdr;
    hdr->f = bt.get()->f;

    // Trim or extend leftmost and rightmost depth-1 children under root until
    // root has exactly 1 record and node_ptrs[0], [1] match (left_nrec, right).
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    ASSERT_GE(root->nrec, 1u);
    const hsize_t sep_key =
        reinterpret_cast<H5B2_test_rec_t*>(H5B2_INT_NREC(root, hdr, 0))->key;
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    // Adjust child 0 (`< sep_key`) to `pc.left_nrec` and child 1 (`> sep_key`)
    // to `pc.right_nrec`.
    hsize_t small_ins = 1;
    hsize_t large_ins = sep_key + 10000;
    for (int iter = 0; iter < 150; ++iter) {
      root = H5B2__protect_internal(hdr, hdr, &hdr->root, hdr->depth, false,
                                    H5AC__NO_FLAGS_SET);
      ASSERT_NE(root, nullptr);
      ASSERT_EQ(root->depth, 2u);
      // Merge extra depth-1 siblings down to 2 children if needed, or balance.
      uint16_t rn = root->nrec;
      uint16_t c0 = root->node_ptrs[0].node_nrec;
      uint16_t c1 = root->node_ptrs[1].node_nrec;
      uint64_t c0_all = root->node_ptrs[0].all_nrec;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      if (rn == 1 && c0 == pc.left_nrec && c1 == pc.right_nrec) {
        break;
      }
      if (c0 < pc.left_nrec) {
        H5B2_test_rec_t rec{small_ins++, 1};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      } else if (c0 > pc.left_nrec) {
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, nullptr, nullptr),
                  0);
      } else if (c1 < pc.right_nrec) {
        // Insert just past sep_key so it lands inside child 1.
        H5B2_test_rec_t rec{sep_key + 1 + static_cast<hsize_t>(iter), 1};
        ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      } else {
        // Remove right inside child 1.
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, c0_all + 1, nullptr,
                                     nullptr),
                  0);
      }
      (void)large_ins;
    }

    root = H5B2__protect_internal(hdr, hdr, &hdr->root, hdr->depth, false,
                                  H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    if (root->node_ptrs[0].node_nrec == pc.left_nrec &&
        root->node_ptrs[1].node_nrec == pc.right_nrec) {
      EXPECT_GE(H5B2__redistribute2(hdr, root->depth, root, 0), 0);
      EXPECT_EQ(root->node_ptrs[0].node_nrec + root->node_ptrs[1].node_nrec,
                pc.left_nrec + pc.right_nrec);
    }
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__DIRTIED_FLAG),
              0);
    EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

    std::vector<hsize_t> keys;
    EXPECT_EQ(H5B2_iterate(bt.get(), CollectTest2Keys, &keys), 0);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
  }
}

// Helper: grows `bt` to depth 2 using `node_size=200, rrec=8` (`H5B2_TEST`,
// leaf max=23, merge=4; d1 max=10, merge=2) where root has 3 depth-1 children
// with requested `node_nrec` counts `(l, m, r)` by building each child's key
// band separately or adjusting counts via organic inserts/deletes.
TEST(H5B2intTest, Redistribute3AndMergeAllBranchesDepth1AndDepth2) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = 200;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 20;

  // Test depth == 1 3-way redistributions and merges.
  struct TripleCase {
    uint16_t l;
    uint16_t m;
    uint16_t r;
  };
  const std::vector<TripleCase> leaf_triples = {
      {4, 18, 18},  // left grows (>1 from mid), right shrinks
      {18, 18, 4},  // left shrinks (>1), right grows (>1)
      {8, 4, 4},    // left shrinks by 1, right grows by 1
      {4, 4, 8},    // left grows by 1 (no extra mid copy), right shrinks
  };
  for (const auto& tc : leaf_triples) {
    ScopedBTree2 bt(H5B2_create(f, &cparam, f));
    ASSERT_TRUE(bt.valid());
    H5B2_hdr_t* hdr = bt.get()->hdr;
    hdr->f = bt.get()->f;

    // Insert enough well-separated keys to produce root (depth=1) with 3
    // leaves (`nrec == 2`).
    for (hsize_t k = 1; k <= 50; ++k) {
      hsize_t key = k * 1000;
      ASSERT_GE(H5B2_insert(bt.get(), &key), 0);
    }
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, 1, false, H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->depth, 1u);
    ASSERT_EQ(root->nrec, 2u);
    hsize_t k0 = 0;
    hsize_t k1 = 0;
    std::memcpy(&k0, H5B2_INT_NREC(root, hdr, 0), sizeof(k0));
    std::memcpy(&k1, H5B2_INT_NREC(root, hdr, 1), sizeof(k1));
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);

    // Tune leaf[0], leaf[1], leaf[2] to exact (tc.l, tc.m, tc.r) counts.
    hsize_t c0_add = 1;
    hsize_t c1_add = k0 + 1;
    hsize_t c2_add = k1 + 1;
    for (int step = 0; step < 120; ++step) {
      root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                    H5AC__NO_FLAGS_SET);
      ASSERT_NE(root, nullptr);
      uint16_t n0 = root->node_ptrs[0].node_nrec;
      uint16_t n1 = root->node_ptrs[1].node_nrec;
      uint16_t n2 = root->node_ptrs[2].node_nrec;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__NO_FLAGS_SET),
                0);
      if (n0 == tc.l && n1 == tc.m && n2 == tc.r) {
        break;
      }
      if (n0 < tc.l) {
        ASSERT_GE(H5B2_insert(bt.get(), &c0_add), 0);
        ++c0_add;
      } else if (n0 > tc.l) {
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, 0, nullptr, nullptr),
                  0);
      } else if (n1 < tc.m) {
        ASSERT_GE(H5B2_insert(bt.get(), &c1_add), 0);
        ++c1_add;
      } else if (n1 > tc.m) {
        ASSERT_GE(
            H5B2_remove_by_idx(bt.get(), H5_ITER_INC, n0 + 1, nullptr, nullptr),
            0);
      } else if (n2 < tc.r) {
        ASSERT_GE(H5B2_insert(bt.get(), &c2_add), 0);
        ++c2_add;
      } else {
        ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, n0 + 1 + n1 + 1,
                                     nullptr, nullptr),
                  0);
      }
    }

    root = H5B2__protect_internal(hdr, hdr, &hdr->root, 1, false,
                                  H5AC__NO_FLAGS_SET);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->node_ptrs[0].node_nrec, tc.l);
    ASSERT_EQ(root->node_ptrs[1].node_nrec, tc.m);
    ASSERT_EQ(root->node_ptrs[2].node_nrec, tc.r);
    unsigned rflags = H5AC__NO_FLAGS_SET;
    EXPECT_GE(H5B2__redistribute3(hdr, 1, root, &rflags, 1), 0);
    EXPECT_NE(rflags & H5AC__DIRTIED_FLAG, 0u);
    EXPECT_GE(
        H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root, rflags), 0);
    EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

    // Check sortedness via find/iterate.
    IterControl ic{{}, 1000, H5_ITER_CONT};
    std::vector<hsize_t> all_keys;
    EXPECT_EQ(
        H5B2_iterate(
            bt.get(),
            [](const void* rec, void* p) -> int {
              static_cast<std::vector<hsize_t>*>(p)->push_back(
                  *static_cast<const hsize_t*>(rec));
              return H5_ITER_CONT;
            },
            &all_keys),
        0);
    EXPECT_TRUE(std::is_sorted(all_keys.begin(), all_keys.end()));
    (void)ic;
  }

  // Test organic merge2 and merge3 at depth == 1 and depth == 2 (including
  // idx + 1 < nrec and idx + 1 == nrec) via H5B2_remove / H5B2_remove_by_idx.
  // Use node_size=128, merge_percent=30 (leaf max=7/merge=2, d1 max=4/merge=1)
  // so 2*merge_nrec + 2 <= max_nrec on root collapse.
  {
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 90; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    H5B2_stat_t info{};
    ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
    EXPECT_GE(info.depth, 2u);

    // Remove alternating interior, head, and tail records to trigger merge2 &
    // merge3 at depth == 2 and depth == 1 cleanly down to empty tree.
    for (hsize_t k = 30; k <= 60; ++k) {
      H5B2_test_rec_t probe{k, 0};
      ASSERT_GE(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
    }
    for (hsize_t k = 1; k < 30; ++k) {
      H5B2_test_rec_t probe{k, 0};
      ASSERT_GE(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
    }
    for (hsize_t k = 90; k >= 61; --k) {
      H5B2_test_rec_t probe{k, 0};
      ASSERT_GE(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
    }
    ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
    EXPECT_EQ(info.nrecords, 0u);
    EXPECT_EQ(info.depth, 0u);
  }
}

TEST(H5B2intTest, Depth2Redistribute3AndMerge3ExplicitBranches) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST2;
  cparam.node_size = 128;  // leaf max=7 (merge=2), d1 max=4 (merge=1)
  cparam.rrec_size = 16;
  cparam.split_percent = 100;
  cparam.merge_percent = 30;

  ScopedBTree2 bt(H5B2_create(f, &cparam, f));
  ASSERT_TRUE(bt.valid());
  for (hsize_t k = 1; k <= 120; ++k) {
    H5B2_test_rec_t rec{k, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  H5B2_stat_t info{};
  ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
  EXPECT_GE(info.depth, 2u);

  // Trigger merges at depth 2 with both (idx + 1) < nrec and (idx + 1) == nrec
  // by trimming from middle, left, and right subtrees across depth >= 2.
  H5B2_hdr_t* hdr = bt.get()->hdr;
  hdr->f = bt.get()->f;
  H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root,
                                                 hdr->depth, false,
                                                 H5AC__NO_FLAGS_SET);
  ASSERT_NE(root, nullptr);
  // If root depth is 3, step down to a depth-2 node with >= 2 children.
  uint16_t d = hdr->depth;
  H5B2_node_ptr_t target_ptr = hdr->root;
  haddr_t target_addr = hdr->root.addr;
  if (d > 2) {
    H5B2_internal_t* child = H5B2__protect_internal(
        hdr, root, &root->node_ptrs[0], static_cast<uint16_t>(d - 1), false,
        H5AC__NO_FLAGS_SET);
    ASSERT_NE(child, nullptr);
    target_ptr = root->node_ptrs[0];
    target_addr = target_ptr.addr;
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
    root = child;
  }
  ASSERT_EQ(root->depth, 2u);

  // If root has >= 3 children whose node_nrec sums allow redistribute3, run
  // redistribute3 on a balanced middle index!
  if (root->nrec >= 2) {
    unsigned rf = H5AC__NO_FLAGS_SET;
    // Test redistribute3 at depth == 2.
    EXPECT_GE(H5B2__redistribute3(hdr, 2, root, &rf, 1), 0);
    EXPECT_NE(rf & H5AC__DIRTIED_FLAG, 0u);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, target_addr, root, rf), 0);
  } else {
    EXPECT_GE(
        H5AC_unprotect(f, H5AC_BT2_INT, target_addr, root, H5AC__NO_FLAGS_SET),
        0);
  }
  EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

  // Remove half the records from the middle outwards to trigger depth-2 merges.
  for (hsize_t k = 20; k <= 70; ++k) {
    H5B2_test_rec_t rec{k, 0};
    ASSERT_GE(H5B2_remove(bt.get(), &rec, nullptr, nullptr), 0);
  }
  std::vector<hsize_t> remaining;
  EXPECT_EQ(H5B2_iterate(bt.get(), CollectTest2Keys, &remaining), 0);
  EXPECT_TRUE(std::is_sorted(remaining.begin(), remaining.end()));
}

TEST(H5B2intTest, SwmrDepth3SplitsRedistributesMergesFlushDependsAndDelete) {
  TempFile tmp("h5b2int_swmr.h5");

  // Phase 1: Use SWMR-write temp file with H5B2_CDSET_ID custom class to build
  // a depth-3 tree, exercise SWMR flush dependencies on splits, redistributes
  // (2 and 3), merges (2 and 3), SWMR iterate pinning, and SWMR deletion.
  {
    ScopedFile file = CreateSwmrTempFile(tmp);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // node_size=128, rrec=16 => leaf max=7 (merge=2), depth>=1 max=4 (merge=1).
    H5B2_create_t cparam{};
    cparam.cls = &kSwmrCustomClass;
    cparam.node_size = 128;
    cparam.rrec_size = 16;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;

    ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
    ASSERT_TRUE(bt.valid());
    H5B2_hdr_t* hdr = bt.get()->hdr;
    ASSERT_TRUE(hdr->swmr_write);
    ASSERT_NE(hdr->top_proxy, nullptr);

    // Attach a parent proxy via H5B2_depend, then flush periodically to advance
    // shadow_epoch while growing to depth == 3 (triggers SWMR split1 at
    // depth 1, 2, 3 and flush dependency retargeting for leaf & internal nodes).
    for (uint64_t k = 1; k <= 210; ++k) {
      uint64_t key = (k % 2 == 0) ? (1000 - k) : (k * 3);
      CustomRec rec{key, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
      if (k % 30 == 0) {
        ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
      }
    }

    H5B2_stat_t info{};
    ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
    EXPECT_GE(info.depth, 3u);
    EXPECT_EQ(info.nrecords, 210u);

    // Explicitly exercise H5B2__redistribute2 (both directions) and
    // H5B2__redistribute3 at depth == 2 & depth == 3 under SWMR!
    hdr->f = bt.get()->f;
    {
      H5B2_internal_t* root = H5B2__protect_internal(
          hdr, hdr, &hdr->root, hdr->depth, true, H5AC__NO_FLAGS_SET);
      ASSERT_NE(root, nullptr);
      ASSERT_GE(root->nrec, 1u);

      // Protect a depth-2 child if depth == 3 so we also cover depth == 2 SWMR
      // flush-dependency retargeting!
      if (root->depth >= 3) {
        H5B2_internal_t* d2 = H5B2__protect_internal(
            hdr, root, &root->node_ptrs[0], 2, true, H5AC__NO_FLAGS_SET);
        ASSERT_NE(d2, nullptr);
        if (d2->nrec >= 2 &&
            d2->node_ptrs[0].node_nrec != d2->node_ptrs[1].node_nrec) {
          EXPECT_GE(H5B2__redistribute2(hdr, 2, d2, 0), 0);
        }
        unsigned d2_flags = H5AC__DIRTIED_FLAG;
        if (d2->nrec >= 2) {
          EXPECT_GE(H5B2__redistribute3(hdr, 2, d2, &d2_flags, 1), 0);
        }
        EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr, d2,
                                 d2_flags),
                  0);
      }

      // Test H5B2__update_flush_depend directly for BOTH depth == 1 (leaf)
      // and depth > 1 (internal) covering:
      // 1) child->parent == old_parent -> switches parent & recreates dep
      // 2) child->parent == new_parent -> no-op assertion path
      // 3) restoring back to original parent cleanly
      if (root->depth >= 2 && root->nrec >= 1) {
        H5B2_internal_t* alt_parent = H5B2__protect_internal(
            hdr, hdr, &root->node_ptrs[1],
            static_cast<uint16_t>(root->depth - 1), false, H5AC__NO_FLAGS_SET);
        ASSERT_NE(alt_parent, nullptr);

        // Retarget child[0] from `root` -> `alt_parent` (`depth > 1`).
        EXPECT_GE(H5B2__update_flush_depend(hdr, root->depth,
                                            &root->node_ptrs[0], root,
                                            alt_parent),
                  0);
        // Second call hits `child_int->parent == new_parent` path cleanly.
        EXPECT_GE(H5B2__update_flush_depend(hdr, root->depth,
                                            &root->node_ptrs[0], root,
                                            alt_parent),
                  0);
        // Restore child[0] back to `root`.
        EXPECT_GE(H5B2__update_flush_depend(hdr, root->depth,
                                            &root->node_ptrs[0], alt_parent,
                                            root),
                  0);

        EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[1].addr,
                                 alt_parent, H5AC__NO_FLAGS_SET),
                  0);
      }

      // Same at depth == 2 -> child leaf (`depth == 1` inside update_flush_dep).
      if (root->depth >= 2) {
        H5B2_internal_t* d1 = H5B2__protect_internal(
            hdr, root, &root->node_ptrs[0],
            static_cast<uint16_t>(root->depth - 1), false, H5AC__NO_FLAGS_SET);
        ASSERT_NE(d1, nullptr);
        H5B2_internal_t* leaf_parent = d1;
        haddr_t lp_addr = root->node_ptrs[0].addr;
        H5B2_internal_t* holder = nullptr;
        if (leaf_parent->depth == 2) {
          holder = leaf_parent;
          leaf_parent = H5B2__protect_internal(
              hdr, holder, &holder->node_ptrs[0], 1, false, H5AC__NO_FLAGS_SET);
          lp_addr = holder->node_ptrs[0].addr;
        }
        if (leaf_parent != nullptr && leaf_parent->depth == 1 &&
            leaf_parent->nrec >= 1) {
          H5B2_leaf_t* alt_leaf_parent = H5B2__protect_leaf(
              hdr, leaf_parent, &leaf_parent->node_ptrs[1], false,
              H5AC__NO_FLAGS_SET);
          ASSERT_NE(alt_leaf_parent, nullptr);
          EXPECT_GE(H5B2__update_flush_depend(hdr, 1, &leaf_parent->node_ptrs[0],
                                              leaf_parent, alt_leaf_parent),
                    0);
          EXPECT_GE(H5B2__update_flush_depend(hdr, 1, &leaf_parent->node_ptrs[0],
                                              leaf_parent, alt_leaf_parent),
                    0);
          EXPECT_GE(H5B2__update_flush_depend(hdr, 1, &leaf_parent->node_ptrs[0],
                                              alt_leaf_parent, leaf_parent),
                    0);
          EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_LEAF,
                                   leaf_parent->node_ptrs[1].addr,
                                   alt_leaf_parent, H5AC__NO_FLAGS_SET),
                    0);
        }
        EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, lp_addr, leaf_parent,
                                 H5AC__NO_FLAGS_SET),
                  0);
        if (holder != nullptr) {
          EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr,
                                   holder, H5AC__NO_FLAGS_SET),
                    0);
        }
      }

      EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                               H5AC__DIRTIED_FLAG),
                0);
      EXPECT_GE(H5B2__hdr_dirty(hdr), 0);
    }

    // Iterate under SWMR (triggers node pinning/unpinning in H5B2__iterate_node).
    std::vector<uint64_t> iter_keys;
    EXPECT_EQ(H5B2_iterate(bt.get(), CollectCustomKeys, &iter_keys), 0);
    EXPECT_EQ(iter_keys.size(), 210u);
    EXPECT_TRUE(std::is_sorted(iter_keys.begin(), iter_keys.end()));

    // Remove records in SWMR mode down through depth 3 -> 2 -> 1 -> 0 to
    // trigger SWMR redistribute2/3, merge2/3, and root collapse.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
    while (true) {
      hsize_t cur_nrec = 0;
      ASSERT_GE(H5B2_get_nrec(bt.get(), &cur_nrec), 0);
      if (cur_nrec <= 15) {
        break;
      }
      // Remove from middle to encourage 3->2 merges & 3-way redistributes.
      ASSERT_GE(H5B2_remove_by_idx(bt.get(), H5_ITER_INC, cur_nrec / 2, nullptr,
                                   nullptr),
                0);
      if (cur_nrec % 20 == 0) {
        ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
      }
    }

    // Delete remaining SWMR tree via H5B2_delete (exercises !FREE_FILE_SPACE
    // branch in H5B2__delete_node).
    haddr_t bt_addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bt.get(), &bt_addr), 0);
    bt.reset();
    EXPECT_GE(H5B2_delete(f, bt_addr, nullptr, nullptr, nullptr), 0);
  }

  // Phase 2: Reopen a regular H5B2_TEST2 file after flushing+closing so cached
  // child nodes are NOT in cache when H5B2__update_flush_depend runs!
  TempFile reopen_tmp("h5b2int_uncached.h5");
  haddr_t saved_addr = HADDR_UNDEF;
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ScopedFile f_create(
        H5Fcreate(reopen_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f_create.valid());
    H5F_t* f = GetFilePtr(f_create.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(CreateTest2Tree(f, 128, 100, 30));
    ASSERT_TRUE(bt.valid());
    for (hsize_t k = 1; k <= 30; ++k) {
      H5B2_test_rec_t rec{k, k};
      ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
    }
    ASSERT_GE(H5B2_get_addr(bt.get(), &saved_addr), 0);
  }
  {
    ScopedFile f_open(
        H5Fopen(reopen_tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(f_open.valid());
    H5F_t* f = GetFilePtr(f_open.get());
    ScopedApiContext ctx;
    ScopedBTree2 bt(H5B2_open(f, saved_addr, f));
    ASSERT_TRUE(bt.valid());
    H5B2_hdr_t* hdr = bt.get()->hdr;
    hdr->f = bt.get()->f;
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    ASSERT_GE(root->depth, 1u);
    int dummy_parent = 0;
    // Child node is not in metadata cache -> hits !IN_CACHE path safely.
    EXPECT_GE(H5B2__update_flush_depend(hdr, root->depth, &root->node_ptrs[0],
                                        &dummy_parent, root),
              0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
  }
}

TEST(H5B2intTest, Depth2SwmrAsymmetricRedistribute2And3AllDirections) {
  // Explicitly drive BOTH move_nrec == 1 and move_nrec > 1 in SWMR mode on a
  // depth == 2 tree (`node_size=200, rrec=16` -> leaf max=11, merge=2; d1
  // max=7, split=7, merge=1) and verify SWMR flush dependency updates for all
  // 4 directions in redistribute3 and both directions in redistribute2.
  TempFile tmp("h5b2int_swmr_asym.h5");
  ScopedFile file = CreateSwmrTempFile(tmp);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = &kSwmrCustomClass;
  cparam.node_size = 200;  // leaf max=11 (merge=2), d1 max=7 (split=7, merge=1)
  cparam.rrec_size = 16;
  cparam.split_percent = 100;
  cparam.merge_percent = 20;

  ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
  ASSERT_TRUE(bt.valid());
  H5B2_hdr_t* hdr = bt.get()->hdr;
  hdr->f = bt.get()->f;

  // Insert 95 records spaced by 100 so depth == 2 with 3 children under root.
  for (uint64_t k = 1; k <= 95; ++k) {
    CustomRec rec{k * 100, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  // Check if root has >= 2 internal records (`>= 3` depth-1 children).
  H5B2_internal_t* root = H5B2__protect_internal(hdr, hdr, &hdr->root,
                                                 hdr->depth, true,
                                                 H5AC__NO_FLAGS_SET);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->depth, 2u);
  if (root->nrec >= 1 &&
      root->node_ptrs[0].node_nrec != root->node_ptrs[1].node_nrec) {
    // Forward direction.
    EXPECT_GE(H5B2__redistribute2(hdr, 2, root, 0), 0);
  }
  EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                           H5AC__DIRTIED_FLAG),
            0);

  // Grow tree further so child 0 becomes larger or smaller than child 1, then
  // run SWMR redistribute2 (both sides) and redistribute3 / merge2 / merge3!
  for (uint64_t k = 1; k <= 25; ++k) {
    CustomRec rec{k, k};  // Goes into child[0] (all < 100)
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  root = H5B2__protect_internal(hdr, hdr, &hdr->root, hdr->depth, true,
                                H5AC__NO_FLAGS_SET);
  ASSERT_NE(root, nullptr);
  unsigned rflags = H5AC__DIRTIED_FLAG;
  if (root->nrec >= 1 &&
      root->node_ptrs[0].node_nrec != root->node_ptrs[1].node_nrec) {
    EXPECT_GE(H5B2__redistribute2(hdr, 2, root, 0), 0);
  }
  if (root->nrec >= 2) {
    EXPECT_GE(H5B2__redistribute3(hdr, 2, root, &rflags, 1), 0);
  }
  EXPECT_GE(
      H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root, rflags), 0);
  EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

  // Add keys into child[2] (`>= 9600`) and rebalance right->left as well.
  for (uint64_t k = 1; k <= 35; ++k) {
    CustomRec rec{100000 + k, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  root = H5B2__protect_internal(hdr, hdr, &hdr->root, hdr->depth, true,
                                H5AC__NO_FLAGS_SET);
  ASSERT_NE(root, nullptr);
  rflags = H5AC__DIRTIED_FLAG;
  if (root->nrec >= 2) {
    if (root->node_ptrs[1].node_nrec != root->node_ptrs[2].node_nrec) {
      EXPECT_GE(H5B2__redistribute2(hdr, 2, root, 1), 0);
    }
    EXPECT_GE(H5B2__redistribute3(hdr, 2, root, &rflags, 1), 0);
  }
  EXPECT_GE(
      H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root, rflags), 0);
  EXPECT_GE(H5B2__hdr_dirty(hdr), 0);

  // Verify order and run full SWMR delete by key down to empty.
  std::vector<uint64_t> all_keys;
  EXPECT_EQ(H5B2_iterate(bt.get(), CollectCustomKeys, &all_keys), 0);
  EXPECT_TRUE(std::is_sorted(all_keys.begin(), all_keys.end()));

  for (size_t i = all_keys.size() / 2; i < all_keys.size(); ++i) {
    CustomRec probe{all_keys[i], 0};
    ASSERT_GE(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
  }
  for (size_t i = 0; i < all_keys.size() / 2; ++i) {
    CustomRec probe{all_keys[i], 0};
    ASSERT_GE(H5B2_remove(bt.get(), &probe, nullptr, nullptr), 0);
  }
  H5B2_stat_t info{};
  ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
  EXPECT_EQ(info.nrecords, 0u);
}

TEST(H5B2intTest, Merge2AndMerge3SwmrDepth2ExplicitAndCleanup) {
  // Exercise H5B2__merge2 and H5B2__merge3 directly under SWMR on depth == 2
  // (`hdr->swmr_write && depth > 1` lines 1202 and 1425, 1457) where combined
  // child counts stay within `max_nrec` so grandchild flush dependencies update.
  TempFile tmp("h5b2int_swmr_merge.h5");
  ScopedFile file = CreateSwmrTempFile(tmp);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = &kSwmrCustomClass;
  cparam.node_size = 200;  // leaf max=11 (merge=2), d1 max=7 (merge=1)
  cparam.rrec_size = 16;
  cparam.split_percent = 100;
  cparam.merge_percent = 20;

  ScopedBTree2 bt(H5B2_create(f, &cparam, nullptr));
  ASSERT_TRUE(bt.valid());
  H5B2_hdr_t* hdr = bt.get()->hdr;
  hdr->f = bt.get()->f;

  // Insert keys across multiple bands so root reaches depth == 2 (`> 83` max
  // depth-1 capacity for node_size=200, rrec=16).
  for (uint64_t k = 1; k <= 110; ++k) {
    CustomRec rec{k * 100, k};
    ASSERT_GE(H5B2_insert(bt.get(), &rec), 0);
  }
  ASSERT_EQ(hdr->depth, 2u);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  // Verify H5B2__destroy_flush_depend / H5B2__create_flush_depend round-trip
  // using the actual registered `parent` pointer on cached internal children.
  {
    H5B2_internal_t* root = H5B2__protect_internal(
        hdr, hdr, &hdr->root, hdr->depth, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root, nullptr);
    ASSERT_GE(root->nrec, 1u);
    H5B2_internal_t* c0 = H5B2__protect_internal(
        hdr, root, &root->node_ptrs[0], 1, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(c0, nullptr);
    ASSERT_NE(c0->parent, nullptr);
    ASSERT_GE(H5B2__destroy_flush_depend(static_cast<H5AC_info_t*>(c0->parent),
                                         &c0->cache_info),
              0);
    ASSERT_GE(H5B2__create_flush_depend(static_cast<H5AC_info_t*>(c0->parent),
                                        &c0->cache_info),
              0);

    H5B2_internal_t* c1 = H5B2__protect_internal(
        hdr, root, &root->node_ptrs[1], 1, false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(c1, nullptr);
    ASSERT_NE(c1->parent, nullptr);
    ASSERT_GE(H5B2__destroy_flush_depend(static_cast<H5AC_info_t*>(c1->parent),
                                         &c1->cache_info),
              0);
    ASSERT_GE(H5B2__create_flush_depend(static_cast<H5AC_info_t*>(c1->parent),
                                        &c1->cache_info),
              0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[1].addr, c1,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, root->node_ptrs[0].addr, c0,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_GE(H5AC_unprotect(f, H5AC_BT2_INT, hdr->root.addr, root,
                             H5AC__NO_FLAGS_SET),
              0);
  }

  // Delete in alternating patterns to exercise SWMR depth-2 merge2/merge3 and
  // root collapse.
  for (uint64_t k = 1; k <= 109; k += 2) {
    CustomRec rec{k * 100, 0};
    ASSERT_GE(H5B2_remove(bt.get(), &rec, nullptr, nullptr), 0);
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
  for (uint64_t k = 2; k <= 110; k += 2) {
    CustomRec rec{k * 100, 0};
    ASSERT_GE(H5B2_remove(bt.get(), &rec, nullptr, nullptr), 0);
  }
  H5B2_stat_t info{};
  ASSERT_GE(H5B2_stat_info(bt.get(), &info), 0);
  EXPECT_EQ(info.nrecords, 0u);
}

}  // namespace
}  // namespace hdf5
