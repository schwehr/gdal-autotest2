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
// Tests v2 B-tree top-level lifecycle and operations: create, open, close,
// delete (immediate and pending), insert, update, iterate, find, index,
// neighbor, modify, remove, remove_by_idx, depend, and patch_file
// (third_party/hdf5/src/H5B2.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2.c

#include <cstdint>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5B2_hdr_t). Define NDEBUG around internal headers so C++
// test translation units match `:hdf5` struct layout.
#ifndef NDEBUG
#define H5B2_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#include "third_party/hdf5/src/H5B2pkg.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5B2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedB2 {
 public:
  ScopedB2() = default;
  explicit ScopedB2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedB2() { reset(); }

  ScopedB2(const ScopedB2&) = delete;
  ScopedB2& operator=(const ScopedB2&) = delete;

  void reset(H5B2_t* next = nullptr) {
    if (bt2_ != nullptr) {
      H5B2_close(bt2_);
    }
    bt2_ = next;
  }

  H5B2_t* release() {
    H5B2_t* tmp = bt2_;
    bt2_ = nullptr;
    return tmp;
  }

  H5B2_t* get() const { return bt2_; }

 private:
  H5B2_t* bt2_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

struct KVRec {
  hsize_t key = 0;
  hsize_t val = 0;
};

// Global fault toggles for custom H5B2_class_t error-injection tests. Always
// restored with absl::Cleanup before leaving each test scope.
struct FaultSwitches {
  bool fail_store = false;
  bool fail_compare = false;
  int compare_fail_after = -1;
  int compare_calls = 0;
  H5F_t* compare_measure_f = nullptr;
  haddr_t compare_first_int_addr = HADDR_UNDEF;
  int compare_last_first_int_call = 0;
  H5F_t* unprotect_on_compare_f = nullptr;
  int unprotect_compare_call = 0;
  bool unprotect_compare_int_only = false;
  H5F_t* track_decode_f = nullptr;
  int decode_calls = 0;
  int first_pinned_int_decode_call = 0;
  int decode_fail_at = 0;
  H5F_t* unpin_parent_on_decode_f = nullptr;
};

FaultSwitches& Faults() {
  static FaultSwitches* f = new FaultSwitches();
  return *f;
}

void* CustomCrtContext(void* udata) {
  return udata != nullptr ? udata : &Faults();
}

herr_t CustomDstContext(void* /*ctx*/) { return 0; }

herr_t CustomStore(void* nrecord, const void* udata) {
  if (Faults().fail_store) {
    return -1;
  }
  *static_cast<KVRec*>(nrecord) = *static_cast<const KVRec*>(udata);
  return 0;
}

herr_t CustomCompare(const void* rec1, const void* rec2, int* result) {
  ++Faults().compare_calls;
  if (Faults().compare_measure_f != nullptr) {
    H5C_t* cache = Faults().compare_measure_f->shared->cache;
    if (cache != nullptr && cache->pl_len == 1 &&
        cache->pl_head_ptr != nullptr &&
        cache->pl_head_ptr->type == H5AC_BT2_INT) {
      if (!H5_addr_defined(Faults().compare_first_int_addr)) {
        Faults().compare_first_int_addr = cache->pl_head_ptr->addr;
      }
      if (cache->pl_head_ptr->addr == Faults().compare_first_int_addr) {
        Faults().compare_last_first_int_call = Faults().compare_calls;
      }
    }
  }
  if (Faults().fail_compare) {
    if (Faults().compare_fail_after <= 0) {
      return -1;
    }
    --Faults().compare_fail_after;
  }
  if (Faults().unprotect_on_compare_f != nullptr &&
      Faults().unprotect_compare_call > 0 &&
      Faults().compare_calls == Faults().unprotect_compare_call) {
    H5C_t* cache = Faults().unprotect_on_compare_f->shared->cache;
    if (cache != nullptr && cache->pl_len == 1 &&
        cache->pl_head_ptr != nullptr) {
      H5C_cache_entry_t* e = cache->pl_head_ptr;
      if (!Faults().unprotect_compare_int_only || e->type == H5AC_BT2_INT) {
        H5AC_unprotect(Faults().unprotect_on_compare_f, e->type, e->addr, e,
                       H5AC__NO_FLAGS_SET);
      }
    }
  }
  const auto* a = static_cast<const KVRec*>(rec1);
  const auto* b = static_cast<const KVRec*>(rec2);
  *result = (a->key < b->key) ? -1 : ((a->key > b->key) ? 1 : 0);
  return 0;
}

herr_t CustomEncode(uint8_t* raw, const void* nrecord, void* /*ctx*/) {
  const auto* rec = static_cast<const KVRec*>(nrecord);
  auto encode_u64 = [](uint8_t* dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      dst[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffU);
    }
  };
  encode_u64(raw, static_cast<uint64_t>(rec->key));
  encode_u64(raw + 8, static_cast<uint64_t>(rec->val));
  return 0;
}

herr_t CustomDecode(const uint8_t* raw, void* nrecord, void* /*ctx*/) {
  ++Faults().decode_calls;
  if (Faults().track_decode_f != nullptr &&
      Faults().first_pinned_int_decode_call == 0) {
    H5C_t* cache = Faults().track_decode_f->shared->cache;
    if (cache != nullptr) {
      for (H5C_cache_entry_t* e = cache->pel_head_ptr; e != nullptr;
           e = e->next) {
        if (e->type == H5AC_BT2_INT && e->pinned_from_client) {
          Faults().first_pinned_int_decode_call = Faults().decode_calls;
          break;
        }
      }
    }
  }
  if (Faults().decode_fail_at > 0 &&
      Faults().decode_calls == Faults().decode_fail_at) {
    if (Faults().unpin_parent_on_decode_f != nullptr) {
      H5C_t* cache = Faults().unpin_parent_on_decode_f->shared->cache;
      if (cache != nullptr) {
        for (H5C_cache_entry_t* e = cache->pel_head_ptr; e != nullptr;
             e = e->next) {
          if (e->type == H5AC_BT2_INT && e->pinned_from_client) {
            H5AC_unpin_entry(e);
            break;
          }
        }
      }
    }
    return -1;
  }
  auto decode_u64 = [](const uint8_t* src) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(src[i]) << (i * 8);
    }
    return static_cast<hsize_t>(v);
  };
  auto* rec = static_cast<KVRec*>(nrecord);
  rec->key = decode_u64(raw);
  rec->val = decode_u64(raw + 8);
  return 0;
}

H5B2_class_t MakeKVClass(H5B2_subid_t id = H5B2_TEST2_ID) {
  H5B2_class_t cls{};
  cls.id = id;
  cls.name = "H5B2_test_kv";
  cls.nrec_size = sizeof(KVRec);
  cls.crt_context = CustomCrtContext;
  cls.dst_context = CustomDstContext;
  cls.store = CustomStore;
  cls.compare = CustomCompare;
  cls.encode = CustomEncode;
  cls.decode = CustomDecode;
  cls.debug = nullptr;
  return cls;
}

struct IterState {
  std::vector<KVRec> recs;
  int stop_after = -1;
  int error_after = -1;
};

int CollectKVOp(const void* record, void* op_data) {
  auto* st = static_cast<IterState*>(op_data);
  st->recs.push_back(*static_cast<const KVRec*>(record));
  const int n = static_cast<int>(st->recs.size());
  if (st->error_after > 0 && n >= st->error_after) {
    return H5_ITER_ERROR;
  }
  if (st->stop_after > 0 && n >= st->stop_after) {
    return H5_ITER_STOP;
  }
  return H5_ITER_CONT;
}

struct FoundState {
  KVRec rec{};
  bool fail = false;
  H5F_t* unprotect_f = nullptr;
};

herr_t CaptureFoundKVOp(const void* record, void* op_data) {
  auto* st = static_cast<FoundState*>(op_data);
  if (st->unprotect_f != nullptr) {
    H5C_t* cache = st->unprotect_f->shared->cache;
    if (cache != nullptr && cache->pl_len == 1 && cache->pl_head_ptr != nullptr) {
      H5C_cache_entry_t* e = cache->pl_head_ptr;
      H5AC_unprotect(st->unprotect_f, e->type, e->addr, e, H5AC__NO_FLAGS_SET);
    }
  }
  if (st->fail) {
    return -1;
  }
  st->rec = *static_cast<const KVRec*>(record);
  return 0;
}

struct ModifyState {
  hsize_t new_val = 0;
  bool should_change = true;
  bool fail = false;
  int calls = 0;
  H5F_t* unprotect_f = nullptr;
};

herr_t ModifyKVOp(void* record, void* op_data, bool* changed) {
  auto* st = static_cast<ModifyState*>(op_data);
  ++st->calls;
  if (st->unprotect_f != nullptr) {
    H5C_t* cache = st->unprotect_f->shared->cache;
    if (cache != nullptr && cache->pl_len == 1 && cache->pl_head_ptr != nullptr) {
      H5C_cache_entry_t* e = cache->pl_head_ptr;
      H5AC_unprotect(st->unprotect_f, e->type, e->addr, e, H5AC__NO_FLAGS_SET);
    }
  }
  if (st->fail) {
    *changed = false;
    return -1;
  }
  if (st->should_change) {
    static_cast<KVRec*>(record)->val = st->new_val;
    *changed = true;
  } else {
    *changed = false;
  }
  return 0;
}

struct RemoveState {
  std::vector<KVRec> removed;
  bool fail = false;
};

herr_t RemoveKVOp(const void* record, void* op_data) {
  auto* st = static_cast<RemoveState*>(op_data);
  if (st->fail) {
    return -1;
  }
  st->removed.push_back(*static_cast<const KVRec*>(record));
  return 0;
}

H5B2_create_t DefaultKVParams(const H5B2_class_t* cls,
                              uint32_t node_size = 160) {
  H5B2_create_t cparam{};
  cparam.cls = cls;
  cparam.node_size = node_size;
  cparam.rrec_size = 16;
  cparam.split_percent = 100;
  cparam.merge_percent = 30;
  return cparam;
}

TEST(H5B2Test, CreateOpenClosePendingDeleteAndImmediateDelete) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Test2 built-in class supports reopen and immediate/pending deletion.
  H5B2_create_t test2_cparam{};
  test2_cparam.cls = H5B2_TEST2;
  test2_cparam.node_size = 160;
  test2_cparam.rrec_size = 16;
  test2_cparam.split_percent = 100;
  test2_cparam.merge_percent = 30;

  haddr_t addr1 = HADDR_UNDEF;
  {
    ScopedB2 bt2(H5B2_create(f, &test2_cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &addr1), 0);
    ASSERT_TRUE(H5_addr_defined(addr1));

    for (hsize_t i = 0; i < 12; ++i) {
      H5B2_test_rec_t rec{i * 2, i + 100};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    hsize_t nrec = 0;
    ASSERT_GE(H5B2_get_nrec(bt2.get(), &nrec), 0);
    EXPECT_EQ(nrec, 12u);
  }

  // Delete immediately when file_rc == 0 (op == nullptr and op != nullptr).
  RemoveState rm_state;
  EXPECT_GE(H5B2_delete(f, addr1, f, RemoveKVOp, &rm_state), 0);
  EXPECT_EQ(rm_state.removed.size(), 12u);

  // Pending delete: open two handles, mark pending_delete, verify third open
  // rejects pending_delete, then close both to trigger actual deletion.
  ScopedB2 bt2_a(H5B2_create(f, &test2_cparam, f));
  ASSERT_NE(bt2_a.get(), nullptr);
  haddr_t addr2 = HADDR_UNDEF;
  ASSERT_GE(H5B2_get_addr(bt2_a.get(), &addr2), 0);
  for (hsize_t i = 0; i < 10; ++i) {
    H5B2_test_rec_t rec{i, i * 10};
    ASSERT_GE(H5B2_insert(bt2_a.get(), &rec), 0);
  }

  ScopedB2 bt2_b(H5B2_open(f, addr2, f));
  ASSERT_NE(bt2_b.get(), nullptr);
  EXPECT_GE(H5B2_delete(f, addr2, f, nullptr, nullptr), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5B2_open(f, addr2, f), nullptr);

    // Invalid address fails H5B2_open and H5B2_delete cleanly.
    haddr_t bad_addr = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
    ASSERT_TRUE(H5_addr_defined(bad_addr));
    EXPECT_EQ(H5B2_open(f, bad_addr, f), nullptr);
    EXPECT_LT(H5B2_delete(f, bad_addr, f, nullptr, nullptr), 0);
  }

  bt2_a.reset();  // file_rc drops 2 -> 1 (not deleted yet).
  bt2_b.reset();  // file_rc drops 1 -> 0 -> triggers pending deletion.

  // Deletion failure inside H5B2_delete leaves header clean/unprotected.
  ScopedB2 bt2_c(H5B2_create(f, &test2_cparam, f));
  ASSERT_NE(bt2_c.get(), nullptr);
  haddr_t addr3 = HADDR_UNDEF;
  ASSERT_GE(H5B2_get_addr(bt2_c.get(), &addr3), 0);
  H5B2_test_rec_t one{1, 1};
  ASSERT_GE(H5B2_insert(bt2_c.get(), &one), 0);
  bt2_c.reset();

  RemoveState fail_rm;
  fail_rm.fail = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_delete(f, addr3, f, RemoveKVOp, &fail_rm), 0);
  }
}

TEST(H5B2Test, UpdateInsertIterateAndErrorCallbacks) {
  H5B2_class_t cls = MakeKVClass();
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
  ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
  ASSERT_NE(bt2.get(), nullptr);

  // Empty tree iterate & store error on H5B2_insert.
  IterState empty_iter;
  EXPECT_EQ(H5B2_iterate(bt2.get(), CollectKVOp, &empty_iter), H5_ITER_CONT);
  EXPECT_TRUE(empty_iter.recs.empty());
  {
    ScopedErrorSilence silence;
    Faults().fail_store = true;
    auto reset = absl::MakeCleanup([] { Faults().fail_store = false; });
    KVRec rec{1, 1};
    EXPECT_LT(H5B2_insert(bt2.get(), &rec), 0);
  }

  // Update on empty tree allocates root leaf and inserts record.
  ModifyState mod_state;
  KVRec r0{10, 100};
  ASSERT_GE(H5B2_update(bt2.get(), &r0, ModifyKVOp, &mod_state), 0);
  EXPECT_EQ(mod_state.calls, 0);

  // Update existing record in leaf triggers modify callback.
  mod_state.new_val = 101;
  mod_state.should_change = true;
  ASSERT_GE(H5B2_update(bt2.get(), &r0, ModifyKVOp, &mod_state), 0);
  EXPECT_EQ(mod_state.calls, 1);

  // Update failure on existing record at depth == 0.
  {
    ScopedErrorSilence silence;
    mod_state.fail = true;
    EXPECT_LT(H5B2_update(bt2.get(), &r0, ModifyKVOp, &mod_state), 0);
    mod_state.fail = false;
  }

  // Fill leaf to capacity and trigger H5B2_UPDATE_INSERT_CHILD_FULL at depth 0,
  // splitting root leaf into an internal node + leaves.
  for (hsize_t i = 1; i <= 15; ++i) {
    KVRec rec{10 + i * 2, 200 + i};
    ASSERT_GE(H5B2_update(bt2.get(), &rec, ModifyKVOp, &mod_state), 0);
  }
  ASSERT_GE(bt2.get()->hdr->depth, 1);

  // Continue inserting until depth >= 2, hitting UPDATE_INSERT_CHILD_FULL on
  // depth > 0 and multi-level internal splits.
  for (hsize_t i = 16; i <= 180; ++i) {
    KVRec rec{10 + i * 2, 200 + i};
    ASSERT_GE(H5B2_update(bt2.get(), &rec, ModifyKVOp, &mod_state), 0);
  }
  EXPECT_GE(bt2.get()->hdr->depth, 2);

  // Modify an internal-node record and a leaf-node record via H5B2_update.
  KVRec int_key{};
  bool found_int = false;
  for (hsize_t i = 0; i <= 180; ++i) {
    KVRec probe{10 + i * 2, 0};
    if (H5B2__get_node_depth_test(bt2.get(), &probe) > 0) {
      int_key = probe;
      found_int = true;
      break;
    }
  }
  ASSERT_TRUE(found_int);
  mod_state.calls = 0;
  mod_state.new_val = 9999;
  ASSERT_GE(H5B2_update(bt2.get(), &int_key, ModifyKVOp, &mod_state), 0);
  EXPECT_EQ(mod_state.calls, 1);

  {
    ScopedErrorSilence silence;
    mod_state.fail = true;
    EXPECT_LT(H5B2_update(bt2.get(), &int_key, ModifyKVOp, &mod_state), 0);
    mod_state.fail = false;
  }

  // Verify H5B2_iterate full, early stop, and error callback.
  IterState full_iter;
  EXPECT_EQ(H5B2_iterate(bt2.get(), CollectKVOp, &full_iter), H5_ITER_CONT);
  EXPECT_EQ(full_iter.recs.size(), 181u);
  for (size_t i = 1; i < full_iter.recs.size(); ++i) {
    EXPECT_LT(full_iter.recs[i - 1].key, full_iter.recs[i].key);
  }

  IterState stop_iter;
  stop_iter.stop_after = 5;
  EXPECT_EQ(H5B2_iterate(bt2.get(), CollectKVOp, &stop_iter), H5_ITER_STOP);
  EXPECT_EQ(stop_iter.recs.size(), 5u);

  {
    ScopedErrorSilence silence;
    IterState err_iter;
    err_iter.error_after = 3;
    EXPECT_LT(H5B2_iterate(bt2.get(), CollectKVOp, &err_iter), 0);
    EXPECT_EQ(err_iter.recs.size(), 3u);
  }
}

TEST(H5B2Test, FindMinMaxCachingInternalPathsAndErrors) {
  H5B2_class_t cls = MakeKVClass();
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Single-leaf (depth == 0) empty + min/max caching + found/not-found.
  {
    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);

    bool found = true;
    KVRec q{50, 0};
    EXPECT_GE(H5B2_find(bt2.get(), &q, &found, nullptr, nullptr), 0);
    EXPECT_FALSE(found);

    for (hsize_t k : {10u, 30u, 50u, 70u}) {
      KVRec rec{k, k * 10};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }

    // Find min and max records to populate hdr->min_native_rec & max_native_rec.
    FoundState fs;
    KVRec q_min{10, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &q_min, &found, CaptureFoundKVOp, &fs), 0);
    EXPECT_TRUE(found);
    EXPECT_EQ(fs.rec.val, 100u);

    KVRec q_max{70, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &q_max, &found, CaptureFoundKVOp, &fs), 0);
    EXPECT_TRUE(found);
    EXPECT_EQ(fs.rec.val, 700u);

    // Hit cached min_native_rec / max_native_rec fast paths (less, equal, fail).
    KVRec below{5, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &below, &found, nullptr, nullptr), 0);
    EXPECT_FALSE(found);

    KVRec above{90, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &above, &found, nullptr, nullptr), 0);
    EXPECT_FALSE(found);

    ASSERT_GE(H5B2_find(bt2.get(), &q_min, &found, nullptr, nullptr), 0);
    EXPECT_TRUE(found);
    ASSERT_GE(H5B2_find(bt2.get(), &q_max, &found, nullptr, nullptr), 0);
    EXPECT_TRUE(found);

    KVRec missing{40, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &missing, &found, nullptr, nullptr), 0);
    EXPECT_FALSE(found);

    KVRec mid{30, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &mid, &found, CaptureFoundKVOp, &fs), 0);
    EXPECT_TRUE(found);
    EXPECT_EQ(fs.rec.val, 300u);

    {
      ScopedErrorSilence silence;
      fs.fail = true;
      EXPECT_LT(H5B2_find(bt2.get(), &q_min, &found, CaptureFoundKVOp, &fs), 0);
      EXPECT_LT(H5B2_find(bt2.get(), &q_max, &found, CaptureFoundKVOp, &fs), 0);
      EXPECT_LT(H5B2_find(bt2.get(), &mid, &found, CaptureFoundKVOp, &fs), 0);
    }
  }

  // Depth >= 2 tree: exercise every internal position transition, internal hit,
  // min/max population from depth >= 2, and compare errors.
  {
    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);

    constexpr hsize_t kCount = 190;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{(i * 2) + 2, i + 1000};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);

    // Search all existing keys plus odd non-existent keys BEFORE caching min/max
    // (Wait: min/max get cached as soon as min=2 and max=380 are queried, so
    // query middle keys first, then min & max!).
    FoundState fs;
    hsize_t some_int_key = 0;
    hsize_t some_leaf_key = 0;
    for (hsize_t i = 2; i + 2 < kCount; ++i) {
      bool found = false;
      KVRec q{(i * 2) + 2, 0};
      ASSERT_GE(H5B2_find(bt2.get(), &q, &found, CaptureFoundKVOp, &fs), 0);
      ASSERT_TRUE(found);
      EXPECT_EQ(fs.rec.val, i + 1000);
      const int d = H5B2__get_node_depth_test(bt2.get(), &q);
      if (d > 0 && some_int_key == 0) {
        some_int_key = q.key;
      }
      if (d == 0 && some_leaf_key == 0 && i > 10 && i < kCount - 10) {
        some_leaf_key = q.key;
      }

      KVRec gap{(i * 2) + 3, 0};
      ASSERT_GE(H5B2_find(bt2.get(), &gap, &found, nullptr, nullptr), 0);
      EXPECT_FALSE(found);
    }
    ASSERT_NE(some_int_key, 0u);
    ASSERT_NE(some_leaf_key, 0u);

    // Now query leftmost min and rightmost max in depth >= 2 to populate cache.
    bool found = false;
    KVRec q_min{2, 0};
    KVRec q_max{(kCount - 1) * 2 + 2, 0};
    ASSERT_GE(H5B2_find(bt2.get(), &q_min, &found, nullptr, nullptr), 0);
    EXPECT_TRUE(found);
    ASSERT_GE(H5B2_find(bt2.get(), &q_max, &found, nullptr, nullptr), 0);
    EXPECT_TRUE(found);
    EXPECT_NE(bt2.get()->hdr->min_native_rec, nullptr);
    EXPECT_NE(bt2.get()->hdr->max_native_rec, nullptr);

    // Internal-node op failure and compare errors at min, max, internal, leaf.
    {
      ScopedErrorSilence silence;
      auto reset = absl::MakeCleanup([] {
        Faults().fail_compare = false;
        Faults().compare_fail_after = -1;
      });
      fs.fail = true;
      KVRec q_int{some_int_key, 0};
      EXPECT_LT(H5B2_find(bt2.get(), &q_int, &found, CaptureFoundKVOp, &fs),
                0);
      fs.fail = false;

      // Fail on min_native_rec compare (0 calls), max_native_rec compare (1
      // call), internal node compare (2 calls), and leaf node compare.
      KVRec q_leaf{some_leaf_key, 0};
      Faults().fail_compare = false;
      Faults().compare_calls = 0;
      ASSERT_GE(H5B2_find(bt2.get(), &q_leaf, &found, nullptr, nullptr), 0);
      const int total_cmp = Faults().compare_calls;
      ASSERT_GE(total_cmp, 4);

      Faults().fail_compare = true;
      Faults().compare_fail_after = 0;
      EXPECT_LT(H5B2_find(bt2.get(), &q_leaf, &found, nullptr, nullptr), 0);

      Faults().compare_fail_after = 1;
      EXPECT_LT(H5B2_find(bt2.get(), &q_leaf, &found, nullptr, nullptr), 0);

      Faults().compare_fail_after = 2;
      EXPECT_LT(H5B2_find(bt2.get(), &q_leaf, &found, nullptr, nullptr), 0);

      Faults().compare_fail_after = total_cmp - 1;
      EXPECT_LT(H5B2_find(bt2.get(), &q_leaf, &found, nullptr, nullptr), 0);
    }
  }
}

TEST(H5B2Test, IndexIncDecInternalLeafAndErrors) {
  H5B2_class_t cls = MakeKVClass();
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
  ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
  ASSERT_NE(bt2.get(), nullptr);

  FoundState fs;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2_index(bt2.get(), H5_ITER_INC, 0, CaptureFoundKVOp, &fs), 0);
  }

  constexpr hsize_t kCount = 190;
  for (hsize_t i = 0; i < kCount; ++i) {
    KVRec rec{i * 3, i + 500};
    ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
  }
  ASSERT_GE(bt2.get()->hdr->depth, 2);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5B2_index(bt2.get(), H5_ITER_INC, kCount, CaptureFoundKVOp, &fs), 0);
  }

  hsize_t int_idx = kCount;
  hsize_t leaf_idx = kCount;
  for (hsize_t i = 0; i < kCount; ++i) {
    ASSERT_GE(H5B2_index(bt2.get(), H5_ITER_INC, i, CaptureFoundKVOp, &fs), 0);
    EXPECT_EQ(fs.rec.key, i * 3);
    EXPECT_EQ(fs.rec.val, i + 500);

    KVRec probe{i * 3, 0};
    const int d = H5B2__get_node_depth_test(bt2.get(), &probe);
    if (d > 0 && int_idx == kCount) {
      int_idx = i;
    }
    if (d == 0 && leaf_idx == kCount) {
      leaf_idx = i;
    }

    ASSERT_GE(
        H5B2_index(bt2.get(), H5_ITER_DEC, kCount - 1 - i, CaptureFoundKVOp, &fs),
        0);
    EXPECT_EQ(fs.rec.key, i * 3);
  }
  ASSERT_LT(int_idx, kCount);
  ASSERT_LT(leaf_idx, kCount);

  {
    ScopedErrorSilence silence;
    fs.fail = true;
    EXPECT_LT(
        H5B2_index(bt2.get(), H5_ITER_INC, int_idx, CaptureFoundKVOp, &fs), 0);
    EXPECT_LT(
        H5B2_index(bt2.get(), H5_ITER_INC, leaf_idx, CaptureFoundKVOp, &fs), 0);
  }
}

TEST(H5B2Test, ModifyDepthZeroAndMultiLevelAndErrors) {
  H5B2_class_t cls = MakeKVClass();
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Depth == 0 modify tests (empty, try=true/false, min/max caching, errors).
  {
    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);

    ModifyState ms;
    KVRec q{10, 0};
    EXPECT_GE(H5B2_modify(bt2.get(), &q, /*try=*/true, ModifyKVOp, &ms), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5B2_modify(bt2.get(), &q, /*try=*/false, ModifyKVOp, &ms), 0);
    }

    for (hsize_t k : {10u, 20u, 30u}) {
      KVRec rec{k, k};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }

    KVRec missing{15, 0};
    EXPECT_GE(H5B2_modify(bt2.get(), &missing, /*try=*/true, ModifyKVOp, &ms),
              0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5B2_modify(bt2.get(), &missing, /*try=*/false, ModifyKVOp, &ms), 0);
    }

    ms.should_change = true;
    ms.new_val = 111;
    KVRec q_min{10, 0};
    KVRec q_max{30, 0};
    ASSERT_GE(H5B2_modify(bt2.get(), &q_min, /*try=*/false, ModifyKVOp, &ms),
              0);
    ASSERT_GE(H5B2_modify(bt2.get(), &q_max, /*try=*/false, ModifyKVOp, &ms),
              0);
    EXPECT_NE(bt2.get()->hdr->min_native_rec, nullptr);
    EXPECT_NE(bt2.get()->hdr->max_native_rec, nullptr);

    ms.should_change = false;
    KVRec q_mid{20, 0};
    ASSERT_GE(H5B2_modify(bt2.get(), &q_mid, /*try=*/false, ModifyKVOp, &ms),
              0);

    {
      ScopedErrorSilence silence;
      ms.fail = true;
      EXPECT_LT(H5B2_modify(bt2.get(), &q_mid, /*try=*/false, ModifyKVOp, &ms),
                0);
      ms.fail = false;
    }
  }

  // Depth >= 2 modify tests.
  {
    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);

    constexpr hsize_t kCount = 190;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{(i * 2) + 2, i};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);

    ModifyState ms;
    ms.new_val = 777;
    hsize_t some_int_key = 0;
    hsize_t some_leaf_key = 0;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec q{(i * 2) + 2, 0};
      ms.should_change = (i % 2 == 0);
      ASSERT_GE(H5B2_modify(bt2.get(), &q, /*try=*/false, ModifyKVOp, &ms), 0);
      const int d = H5B2__get_node_depth_test(bt2.get(), &q);
      if (d > 0 && some_int_key == 0) {
        some_int_key = q.key;
      }
      if (d == 0 && some_leaf_key == 0 && i > 10 && i < kCount - 10) {
        some_leaf_key = q.key;
      }
    }
    ASSERT_NE(some_int_key, 0u);
    ASSERT_NE(some_leaf_key, 0u);
    EXPECT_NE(bt2.get()->hdr->min_native_rec, nullptr);
    EXPECT_NE(bt2.get()->hdr->max_native_rec, nullptr);

    KVRec gap{3, 0};
    EXPECT_GE(H5B2_modify(bt2.get(), &gap, /*try=*/true, ModifyKVOp, &ms), 0);

    {
      ScopedErrorSilence silence;
      auto reset = absl::MakeCleanup([] {
        Faults().fail_compare = false;
        Faults().compare_fail_after = -1;
      });
      EXPECT_LT(H5B2_modify(bt2.get(), &gap, /*try=*/false, ModifyKVOp, &ms),
                0);

      ms.fail = true;
      KVRec q_int{some_int_key, 0};
      KVRec q_leaf{some_leaf_key, 0};
      EXPECT_LT(H5B2_modify(bt2.get(), &q_int, /*try=*/false, ModifyKVOp, &ms),
                0);
      EXPECT_LT(
          H5B2_modify(bt2.get(), &q_leaf, /*try=*/false, ModifyKVOp, &ms), 0);
      ms.fail = false;

      // Fail compare in internal node and leaf node during modify.
      Faults().fail_compare = false;
      Faults().compare_calls = 0;
      ASSERT_GE(
          H5B2_modify(bt2.get(), &q_leaf, /*try=*/false, ModifyKVOp, &ms), 0);
      const int total_cmp = Faults().compare_calls;
      ASSERT_GE(total_cmp, 2);

      Faults().fail_compare = true;
      Faults().compare_fail_after = 0;
      EXPECT_LT(
          H5B2_modify(bt2.get(), &q_leaf, /*try=*/false, ModifyKVOp, &ms), 0);

      Faults().compare_fail_after = total_cmp - 1;
      EXPECT_LT(
          H5B2_modify(bt2.get(), &q_leaf, /*try=*/false, ModifyKVOp, &ms), 0);
    }
  }
}

TEST(H5B2Test, NeighborDepthZeroAndMultiLevel) {
  H5B2_class_t cls = MakeKVClass();
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
  ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
  ASSERT_NE(bt2.get(), nullptr);

  FoundState fs;
  {
    ScopedErrorSilence silence;
    KVRec q{10, 0};
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &q, CaptureFoundKVOp,
                            &fs),
              0);
  }

  // Depth == 0 neighbor queries.
  for (hsize_t k : {10u, 20u, 30u}) {
    KVRec rec{k, k};
    ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
  }
  KVRec q15{15, 0};
  ASSERT_GE(
      H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &q15, CaptureFoundKVOp, &fs),
      0);
  EXPECT_EQ(fs.rec.key, 10u);
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &q15,
                          CaptureFoundKVOp, &fs),
            0);
  EXPECT_EQ(fs.rec.key, 20u);
  {
    ScopedErrorSilence silence;
    KVRec too_small{5, 0};
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &too_small,
                            CaptureFoundKVOp, &fs),
              0);
  }

  // Grow to depth >= 2 and test H5B2_neighbor internal paths + boundary error.
  for (hsize_t i = 4; i <= 180; ++i) {
    KVRec rec{i * 10, i * 10};
    ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
  }
  ASSERT_GE(bt2.get()->hdr->depth, 2);

  KVRec q_mid{905, 0};
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_LESS, &q_mid,
                          CaptureFoundKVOp, &fs),
            0);
  EXPECT_EQ(fs.rec.key, 900u);
  ASSERT_GE(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &q_mid,
                          CaptureFoundKVOp, &fs),
            0);
  EXPECT_EQ(fs.rec.key, 910u);

  {
    ScopedErrorSilence silence;
    KVRec too_large{999999, 0};
    EXPECT_LT(H5B2_neighbor(bt2.get(), H5B2_COMPARE_GREATER, &too_large,
                            CaptureFoundKVOp, &fs),
              0);
  }
}

TEST(H5B2Test, RemoveAndRemoveByIdxDepthDecreaseAndErrors) {
  H5B2_class_t cls = MakeKVClass();
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Part 1: H5B2_remove empty, error, depth == 0, and depth 2 -> 0 shrink.
  {
    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);

    RemoveState rs;
    {
      ScopedErrorSilence silence;
      KVRec q{1, 0};
      EXPECT_LT(H5B2_remove(bt2.get(), &q, RemoveKVOp, &rs), 0);
    }

    KVRec single{42, 42};
    ASSERT_GE(H5B2_insert(bt2.get(), &single), 0);
    {
      ScopedErrorSilence silence;
      rs.fail = true;
      EXPECT_LT(H5B2_remove(bt2.get(), &single, RemoveKVOp, &rs), 0);
      rs.fail = false;
    }
    ASSERT_GE(H5B2_remove(bt2.get(), &single, RemoveKVOp, &rs), 0);

    constexpr hsize_t kCount = 190;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{i * 2, i};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);

    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec q{((i * 73) % kCount) * 2, 0};
      ASSERT_GE(H5B2_remove(bt2.get(), &q, RemoveKVOp, &rs), 0);
    }
    EXPECT_EQ(bt2.get()->hdr->depth, 0);
    hsize_t nrec = 999;
    ASSERT_GE(H5B2_get_nrec(bt2.get(), &nrec), 0);
    EXPECT_EQ(nrec, 0u);

    // Populate again to depth >= 2 and verify H5B2_remove error propagation.
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{i * 2, i};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);
    {
      ScopedErrorSilence silence;
      rs.fail = true;
      KVRec q{100, 0};
      EXPECT_LT(H5B2_remove(bt2.get(), &q, RemoveKVOp, &rs), 0);
      rs.fail = false;
    }
  }

  // Part 2: H5B2_remove_by_idx empty, out-of-range, error, depth 0, depth 2 -> 0.
  {
    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);

    RemoveState rs;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 0, RemoveKVOp, &rs), 0);
    }

    KVRec single{7, 7};
    ASSERT_GE(H5B2_insert(bt2.get(), &single), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 1, RemoveKVOp, &rs), 0);
      rs.fail = true;
      EXPECT_LT(
          H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 0, RemoveKVOp, &rs), 0);
      rs.fail = false;
    }
    ASSERT_GE(
        H5B2_remove_by_idx(bt2.get(), H5_ITER_DEC, 0, RemoveKVOp, &rs), 0);

    constexpr hsize_t kCount = 190;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{i * 5, i};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);

    for (hsize_t remaining = kCount; remaining > 0; --remaining) {
      const H5_iter_order_t order =
          (remaining % 2 == 0) ? H5_ITER_INC : H5_ITER_DEC;
      const hsize_t idx = remaining / 2;
      ASSERT_GE(H5B2_remove_by_idx(bt2.get(), order, idx, RemoveKVOp, &rs), 0);
    }
    EXPECT_EQ(bt2.get()->hdr->depth, 0);
    hsize_t nrec = 999;
    ASSERT_GE(H5B2_get_nrec(bt2.get(), &nrec), 0);
    EXPECT_EQ(nrec, 0u);

    // Repopulate to depth >= 2 and verify H5B2_remove_by_idx error propagation.
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{i * 5, i};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);
    {
      ScopedErrorSilence silence;
      rs.fail = true;
      EXPECT_LT(
          H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 0, RemoveKVOp, &rs), 0);
      rs.fail = false;
    }
  }
}

TEST(H5B2Test, SwmrDependFindIndexModifyUpdateAndPatchFile) {
  H5B2_class_t swmr_cls = MakeKVClass(H5B2_CDSET_ID);
  TempFile tmp_file("h5b2_swmr.h5");
  H5AC_proxy_entry_t* parent_proxy = nullptr;
  auto destroy_proxy = absl::MakeCleanup([&parent_proxy] {
    if (parent_proxy != nullptr) {
      H5AC_proxy_entry_dest(parent_proxy);
      parent_proxy = nullptr;
    }
  });

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    ScopedFile other_file = CreateInMemoryFile("patch_target.h5");
    ASSERT_TRUE(other_file.valid());

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    H5F_t* f_other = GetH5File(other_file.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f_other, nullptr);

    parent_proxy = H5AC_proxy_entry_create();
    ASSERT_NE(parent_proxy, nullptr);

    H5B2_create_t cparam = DefaultKVParams(&swmr_cls, /*node_size=*/160);
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_TRUE(bt2.get()->hdr->swmr_write);

    // First H5B2_depend sets hdr->parent; second call is idempotent.
    EXPECT_GE(H5B2_depend(bt2.get(), parent_proxy), 0);
    EXPECT_GE(H5B2_depend(bt2.get(), parent_proxy), 0);

    constexpr hsize_t kCount = 190;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{i * 2, i + 10};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);

    // Flush to increment shadow_epoch, then modify via H5B2_update (hits
    // H5B2_UPDATE_SHADOW_DONE), H5B2_find, H5B2_index, and H5B2_modify in SWMR
    // mode (pinning/unpinning parent entries along the tree path).
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    EXPECT_GE(bt2.get()->hdr->shadow_epoch, 1u);

    ModifyState ms;
    ms.new_val = 5555;
    KVRec q_leaf{2, 0};
    ASSERT_GE(H5B2_update(bt2.get(), &q_leaf, ModifyKVOp, &ms), 0);

    FoundState fs;
    bool found = false;
    ASSERT_GE(H5B2_find(bt2.get(), &q_leaf, &found, CaptureFoundKVOp, &fs), 0);
    EXPECT_TRUE(found);
    EXPECT_EQ(fs.rec.val, 5555u);

    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec q{i * 2, 0};
      ASSERT_GE(H5B2_find(bt2.get(), &q, &found, nullptr, nullptr), 0);
      EXPECT_TRUE(found);
      ASSERT_GE(H5B2_index(bt2.get(), H5_ITER_INC, i, CaptureFoundKVOp, &fs),
                0);
      EXPECT_EQ(fs.rec.key, i * 2);
      ms.new_val = 8000 + i;
      ASSERT_GE(H5B2_modify(bt2.get(), &q, /*try=*/false, ModifyKVOp, &ms), 0);
    }

    // Exercise H5B2_patch_file (both mismatch and no-op match), then restore
    // original f before teardown.
    EXPECT_GE(H5B2_patch_file(bt2.get(), f_other), 0);
    EXPECT_EQ(bt2.get()->f, f_other);
    EXPECT_GE(H5B2_patch_file(bt2.get(), f_other), 0);
    EXPECT_GE(H5B2_patch_file(bt2.get(), f), 0);

    bt2.reset();
  }
  std::move(destroy_proxy).Invoke();
}

TEST(H5B2Test, ReopenMinMaxCacheAndSwmrErrorPropagation) {
  H5B2_class_t cls = MakeKVClass();
  auto reset_faults = absl::MakeCleanup([] {
    Faults() = FaultSwitches{};
  });

  // 1. Header create failure, empty update space failure, update child-full
  // insert failure, and H5B2__hdr_dirty failures on an unpinned header.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);

    {
      const unsigned orig_flags = f->shared->flags;
      auto restore_flags =
          absl::MakeCleanup([&] { f->shared->flags = orig_flags; });
      f->shared->flags &= ~static_cast<unsigned>(H5F_ACC_RDWR);

      ScopedErrorSilence silence;
      EXPECT_EQ(H5B2_create(f, &cparam, &Faults()), nullptr);
    }

    // Fresh tree where H5_addr_defined(hdr->root.addr) is false.
    {
      ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
      ASSERT_NE(bt2.get(), nullptr);

      const hsize_t orig_aggr_size = f->shared->meta_aggr.size;
      const haddr_t orig_tmp_addr = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_TRUE(H5_addr_defined(eoa));
      ModifyState ms;
      KVRec rec{1, 10};
      {
        auto restore_space = absl::MakeCleanup([&] {
          f->shared->meta_aggr.size = orig_aggr_size;
          f->shared->tmp_addr = orig_tmp_addr;
        });
        f->shared->meta_aggr.size = 0;
        f->shared->tmp_addr = eoa;

        ScopedErrorSilence silence;
        EXPECT_LT(H5B2_update(bt2.get(), &rec, ModifyKVOp, &ms), 0);
      }

      // Now H5B2_update creates the root leaf on a fresh empty tree cleanly.
      EXPECT_GE(H5B2_update(bt2.get(), &rec, ModifyKVOp, &ms), 0);

      // Fill root leaf to max_nrec, then fail CustomStore during
      // H5B2_UPDATE_INSERT_CHILD_FULL -> H5B2__insert().
      const unsigned max_nrec = bt2.get()->hdr->node_info[0].max_nrec;
      for (hsize_t i = 2; i <= max_nrec; ++i) {
        KVRec fill{i * 10, i};
        ASSERT_GE(H5B2_insert(bt2.get(), &fill), 0);
      }
      ASSERT_EQ(bt2.get()->hdr->depth, 0u);
      ASSERT_EQ(bt2.get()->hdr->root.node_nrec, max_nrec);

      Faults().fail_store = true;
      {
        ScopedErrorSilence silence;
        KVRec overflow{9999, 9999};
        EXPECT_LT(H5B2_update(bt2.get(), &overflow, ModifyKVOp, &ms), 0);
      }
      Faults().fail_store = false;
    }

    // Flush clean header, unpin it, trigger H5B2__hdr_dirty failures in
    // H5B2_update, H5B2_remove, and H5B2_remove_by_idx, then re-pin cleanly.
    {
      ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
      ASSERT_NE(bt2.get(), nullptr);
      haddr_t hdr_addr = HADDR_UNDEF;
      ASSERT_GE(H5B2_get_addr(bt2.get(), &hdr_addr), 0);
      for (hsize_t k : {10u, 20u, 30u, 40u}) {
        KVRec rec{k, k};
        ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
      }
      ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      ASSERT_GE(H5AC_unpin_entry(bt2.get()->hdr), 0);

      ModifyState ms;
      RemoveState rs;
      {
        ScopedErrorSilence silence;
        KVRec add_rec{15, 15};
        EXPECT_LT(H5B2_update(bt2.get(), &add_rec, ModifyKVOp, &ms), 0);
        KVRec rm_rec{20, 0};
        EXPECT_LT(H5B2_remove(bt2.get(), &rm_rec, RemoveKVOp, &rs), 0);
        EXPECT_LT(H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 0, RemoveKVOp, &rs),
                  0);
      }

      H5B2_hdr_t* re_hdr =
          H5B2__hdr_protect(f, hdr_addr, &Faults(), H5AC__NO_FLAGS_SET);
      ASSERT_NE(re_hdr, nullptr);
      ASSERT_GE(
          H5B2__hdr_unprotect(re_hdr, H5AC__PIN_ENTRY_FLAG | H5AC__DIRTIED_FLAG),
          0);
    }
  }

  // 2. Reopen disk file with H5B2_TEST2 so min_native_rec & max_native_rec are
  // nullptr, then populate them lazily via H5B2_find().
  {
    TempFile reopen_tmp("h5b2_reopen_minmax.h5");
    haddr_t hdr_addr = HADDR_UNDEF;
    H5B2_create_t test2_cparam{H5B2_TEST2, 160, 16, 100, 30};

    {
      ScopedFile file(H5Fcreate(reopen_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      H5F_t* f = GetH5File(file.get());
      ASSERT_NE(f, nullptr);

      ScopedB2 bt2(H5B2_create(f, &test2_cparam, f));
      ASSERT_NE(bt2.get(), nullptr);
      ASSERT_GE(H5B2_get_addr(bt2.get(), &hdr_addr), 0);
      for (hsize_t k : {10u, 20u, 30u, 40u}) {
        H5B2_test_rec_t rec{k, k * 10};
        ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
      }
    }

    {
      ScopedFile file(
          H5Fopen(reopen_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      H5F_t* f = GetH5File(file.get());
      ASSERT_NE(f, nullptr);

      ScopedB2 bt2(H5B2_open(f, hdr_addr, f));
      ASSERT_NE(bt2.get(), nullptr);
      EXPECT_EQ(bt2.get()->hdr->min_native_rec, nullptr);
      EXPECT_EQ(bt2.get()->hdr->max_native_rec, nullptr);

      bool found = false;
      H5B2_test_rec_t q_min{10, 0};
      ASSERT_GE(H5B2_find(bt2.get(), &q_min, &found, nullptr, nullptr), 0);
      EXPECT_TRUE(found);
      EXPECT_NE(bt2.get()->hdr->min_native_rec, nullptr);

      found = false;
      H5B2_test_rec_t q_max{40, 0};
      ASSERT_GE(H5B2_find(bt2.get(), &q_max, &found, nullptr, nullptr), 0);
      EXPECT_TRUE(found);
      EXPECT_NE(bt2.get()->hdr->max_native_rec, nullptr);
    }
  }

  // 3. Cache H5AC_unprotect failures triggered cleanly inside callbacks and
  // final comparisons in H5B2_find, H5B2_index, and H5B2_modify.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_create_t cparam = DefaultKVParams(&cls, /*node_size=*/160);

    // Leaf not-found unprotect failures for find & modify.
    {
      ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
      ASSERT_NE(bt2.get(), nullptr);
      for (hsize_t k : {100u, 300u}) {
        KVRec rec{k, k};
        ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
      }

      KVRec q_miss{200, 0};
      bool found = true;
      Faults().compare_calls = 0;
      ASSERT_GE(H5B2_find(bt2.get(), &q_miss, &found, nullptr, nullptr), 0);
      EXPECT_FALSE(found);
      const int find_cmp = Faults().compare_calls;
      ASSERT_GE(find_cmp, 3);

      auto clear_cmp = absl::MakeCleanup([] {
        Faults().unprotect_on_compare_f = nullptr;
        Faults().unprotect_compare_call = 0;
        Faults().unprotect_compare_int_only = false;
      });
      Faults().compare_calls = 0;
      Faults().unprotect_on_compare_f = f;
      Faults().unprotect_compare_call = find_cmp;
      Faults().unprotect_compare_int_only = false;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5B2_find(bt2.get(), &q_miss, &found, nullptr, nullptr), 0);
      }

      Faults().unprotect_on_compare_f = nullptr;
      ModifyState ms;
      ms.should_change = false;
      Faults().compare_calls = 0;
      EXPECT_GE(H5B2_modify(bt2.get(), &q_miss, /*try=*/true, ModifyKVOp, &ms),
                0);
      const int mod_cmp = Faults().compare_calls;
      ASSERT_GE(mod_cmp, 1);

      Faults().compare_calls = 0;
      Faults().unprotect_on_compare_f = f;
      Faults().unprotect_compare_call = mod_cmp;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(
            H5B2_modify(bt2.get(), &q_miss, /*try=*/true, ModifyKVOp, &ms), 0);
      }
    }

    // Depth >= 2 internal & leaf unprotect failures in find, index, and modify.
    {
      ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
      ASSERT_NE(bt2.get(), nullptr);

      constexpr hsize_t kCount = 190;
      for (hsize_t i = 0; i < kCount; ++i) {
        KVRec rec{(i * 2) + 2, i + 100};
        ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
      }
      ASSERT_GE(bt2.get()->hdr->depth, 2);

      hsize_t int_idx = kCount;
      hsize_t leaf_idx = kCount;
      KVRec q_int{};
      KVRec q_leaf{};
      for (hsize_t i = 0; i < kCount; ++i) {
        KVRec q{(i * 2) + 2, 0};
        const int d = H5B2__get_node_depth_test(bt2.get(), &q);
        if (d > 0 && int_idx == kCount) {
          int_idx = i;
          q_int = q;
        }
        if (d == 0 && leaf_idx == kCount && i > 10 && i < kCount - 10) {
          leaf_idx = i;
          q_leaf = q;
        }
      }
      ASSERT_LT(int_idx, kCount);
      ASSERT_LT(leaf_idx, kCount);

      // Measure last compare in root internal node for gap key not in tree.
      KVRec q_gap{101, 0};
      bool found = false;
      Faults().compare_calls = 0;
      Faults().compare_measure_f = f;
      Faults().compare_first_int_addr = HADDR_UNDEF;
      Faults().compare_last_first_int_call = 0;
      ASSERT_GE(H5B2_find(bt2.get(), &q_gap, &found, nullptr, nullptr), 0);
      EXPECT_FALSE(found);
      Faults().compare_measure_f = nullptr;
      const int find_first_int_cmp = Faults().compare_last_first_int_call;
      ASSERT_GT(find_first_int_cmp, 0);

      ModifyState ms;
      ms.should_change = false;
      Faults().compare_calls = 0;
      Faults().compare_measure_f = f;
      Faults().compare_first_int_addr = HADDR_UNDEF;
      Faults().compare_last_first_int_call = 0;
      EXPECT_GE(H5B2_modify(bt2.get(), &q_gap, /*try=*/true, ModifyKVOp, &ms),
                0);
      Faults().compare_measure_f = nullptr;
      const int mod_first_int_cmp = Faults().compare_last_first_int_call;
      ASSERT_GT(mod_first_int_cmp, 0);

      {
        auto clear_cmp = absl::MakeCleanup([] {
          Faults().unprotect_on_compare_f = nullptr;
          Faults().unprotect_compare_call = 0;
          Faults().unprotect_compare_int_only = false;
        });
        ScopedErrorSilence silence;

        Faults().unprotect_on_compare_f = f;
        Faults().unprotect_compare_int_only = true;

        Faults().compare_calls = 0;
        Faults().unprotect_compare_call = find_first_int_cmp;
        EXPECT_LT(H5B2_find(bt2.get(), &q_gap, &found, nullptr, nullptr), 0);

        Faults().compare_calls = 0;
        Faults().unprotect_compare_call = mod_first_int_cmp;
        EXPECT_LT(
            H5B2_modify(bt2.get(), &q_gap, /*try=*/true, ModifyKVOp, &ms), 0);
      }

      {
        ScopedErrorSilence silence;
        FoundState fs;
        fs.unprotect_f = f;
        for (bool fail_op : {true, false}) {
          fs.fail = fail_op;
          EXPECT_LT(
              H5B2_find(bt2.get(), &q_int, &found, CaptureFoundKVOp, &fs), 0);
          EXPECT_LT(
              H5B2_find(bt2.get(), &q_leaf, &found, CaptureFoundKVOp, &fs), 0);
          EXPECT_LT(
              H5B2_index(bt2.get(), H5_ITER_INC, int_idx, CaptureFoundKVOp, &fs),
              0);
          EXPECT_LT(H5B2_index(bt2.get(), H5_ITER_INC, leaf_idx,
                               CaptureFoundKVOp, &fs),
                    0);
        }

        ModifyState mod_st;
        mod_st.should_change = false;
        mod_st.unprotect_f = f;
        for (bool fail_op : {true, false}) {
          mod_st.fail = fail_op;
          EXPECT_LT(
              H5B2_modify(bt2.get(), &q_int, /*try=*/false, ModifyKVOp, &mod_st),
              0);
          EXPECT_LT(H5B2_modify(bt2.get(), &q_leaf, /*try=*/false, ModifyKVOp,
                                &mod_st),
                    0);
        }
      }
    }

    // 4. H5FL_fac_term failures on root collapse in H5B2_remove & remove_by_idx.
    for (bool by_idx : {false, true}) {
      for (bool hold_ptr_fac : {false, true}) {
        ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
        ASSERT_NE(bt2.get(), nullptr);
        constexpr hsize_t kInitCount = 12;
        for (hsize_t i = 0; i < kInitCount; ++i) {
          KVRec rec{i * 10, i};
          ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
        }
        H5B2_hdr_t* hdr = bt2.get()->hdr;
        ASSERT_EQ(hdr->depth, 1u);

        H5FL_fac_head_t* held_fac = hold_ptr_fac ? hdr->node_info[1].node_ptr_fac
                                                 : hdr->node_info[1].nat_rec_fac;
        ASSERT_NE(held_fac, nullptr);
        void* held_blk = H5FL_FAC_MALLOC(held_fac);
        ASSERT_NE(held_blk, nullptr);
        auto release_held = absl::MakeCleanup([&] {
          if (held_blk != nullptr && held_fac != nullptr) {
            H5FL_FAC_FREE(held_fac, held_blk);
            held_blk = nullptr;
          }
        });

        bool hit_error = false;
        RemoveState rs;
        for (hsize_t i = 0; i < kInitCount; ++i) {
          ScopedErrorSilence silence;
          KVRec rm{i * 10, 0};
          herr_t rc = by_idx ? H5B2_remove_by_idx(bt2.get(), H5_ITER_INC, 0,
                                                  RemoveKVOp, &rs)
                             : H5B2_remove(bt2.get(), &rm, RemoveKVOp, &rs);
          if (rc < 0) {
            hit_error = true;
            break;
          }
        }
        ASSERT_TRUE(hit_error);
        EXPECT_EQ(hdr->depth, 1u);

        // Clean up the held block & depth-1 factories, then complete collapse
        // bookkeeping on hdr before closing bt2.
        if (hold_ptr_fac) {
          hdr->node_info[1].nat_rec_fac = nullptr;
          H5FL_FAC_FREE(hdr->node_info[1].node_ptr_fac, held_blk);
          held_blk = nullptr;
          EXPECT_GE(H5FL_fac_term(hdr->node_info[1].node_ptr_fac), 0);
          hdr->node_info[1].node_ptr_fac = nullptr;
        } else {
          H5FL_FAC_FREE(hdr->node_info[1].nat_rec_fac, held_blk);
          held_blk = nullptr;
          EXPECT_GE(H5FL_fac_term(hdr->node_info[1].nat_rec_fac), 0);
          hdr->node_info[1].nat_rec_fac = nullptr;
          EXPECT_GE(H5FL_fac_term(hdr->node_info[1].node_ptr_fac), 0);
          hdr->node_info[1].node_ptr_fac = nullptr;
        }
        hdr->depth = 0;
        hdr->root.all_nrec--;
        EXPECT_EQ(hdr->root.all_nrec, hdr->root.node_nrec);
        EXPECT_GE(H5B2__hdr_dirty(hdr), 0);
      }
    }
  }

  // 5. SWMR H5B2_depend failure and H5AC_evict + CustomDecode error paths
  // (internal child, leaf child, and unpinned-parent error in done blocks).
  H5B2_class_t swmr_cls = MakeKVClass(H5B2_CDSET_ID);
  TempFile swmr_tmp("h5b2_swmr_errs.h5");
  H5AC_proxy_entry_t* parent_proxy = nullptr;
  auto destroy_proxy = absl::MakeCleanup([&parent_proxy] {
    if (parent_proxy != nullptr) {
      H5AC_proxy_entry_dest(parent_proxy);
      parent_proxy = nullptr;
    }
  });

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(swmr_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_create_t cparam = DefaultKVParams(&swmr_cls, /*node_size=*/160);

    // H5B2_depend failure when H5AC_proxy_entry_add_child fails.
    {
      ScopedB2 bt2_dep(H5B2_create(f, &cparam, &Faults()));
      ASSERT_NE(bt2_dep.get(), nullptr);
      parent_proxy = H5AC_proxy_entry_create();
      ASSERT_NE(parent_proxy, nullptr);

      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_TRUE(H5_addr_defined(eoa));
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->tmp_addr = orig_tmp; });
      f->shared->tmp_addr = eoa + 1;

      ScopedErrorSilence silence;
      EXPECT_LT(H5B2_depend(bt2_dep.get(), parent_proxy), 0);
      EXPECT_EQ(bt2_dep.get()->hdr->parent, nullptr);
    }
    std::move(destroy_proxy).Invoke();

    // Populate depth >= 2 SWMR tree, flush cleanly, then test evicted node
    // decode failures across H5B2_find, H5B2_index, and H5B2_modify.
    ScopedB2 bt2(H5B2_create(f, &cparam, &Faults()));
    ASSERT_NE(bt2.get(), nullptr);
    constexpr hsize_t kCount = 190;
    for (hsize_t i = 0; i < kCount; ++i) {
      KVRec rec{i * 2, i + 10};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    ASSERT_GE(bt2.get()->hdr->depth, 2);

    hsize_t leaf_idx = kCount;
    KVRec q_leaf{};
    for (hsize_t i = 10; i + 10 < kCount; ++i) {
      KVRec q{i * 2, 0};
      if (H5B2__get_node_depth_test(bt2.get(), &q) == 0) {
        leaf_idx = i;
        q_leaf = q;
        break;
      }
    }
    ASSERT_LT(leaf_idx, kCount);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    auto measure_and_fail_decode = [&](auto&& run_op) {
      Faults().decode_calls = 0;
      Faults().first_pinned_int_decode_call = 0;
      Faults().decode_fail_at = 0;
      Faults().unpin_parent_on_decode_f = nullptr;
      Faults().track_decode_f = f;
      ASSERT_GE(H5AC_evict(f), 0);
      ASSERT_GE(run_op(), 0);
      Faults().track_decode_f = nullptr;

      const int depth1_call = Faults().first_pinned_int_decode_call;
      const int leaf_call = Faults().decode_calls;
      ASSERT_GT(depth1_call, 0);
      ASSERT_GT(leaf_call, depth1_call);

      auto clear_decode = absl::MakeCleanup([] {
        Faults().decode_fail_at = 0;
        Faults().unpin_parent_on_decode_f = nullptr;
      });
      ScopedErrorSilence silence;

      // Fail at depth-1 internal protect (unpins depth-2 parent cleanly).
      ASSERT_GE(H5AC_evict(f), 0);
      Faults().decode_calls = 0;
      Faults().decode_fail_at = depth1_call;
      EXPECT_LT(run_op(), 0);

      // Fail at depth-1 internal protect after unpinning depth-2 parent inside
      // CustomDecode (triggers HDONE_ERROR H5AC_unpin_entry in done:).
      ASSERT_GE(H5AC_evict(f), 0);
      Faults().decode_calls = 0;
      Faults().decode_fail_at = depth1_call;
      Faults().unpin_parent_on_decode_f = f;
      EXPECT_LT(run_op(), 0);
      Faults().unpin_parent_on_decode_f = nullptr;

      // Fail at depth-0 leaf protect (unpins depth-1 parent cleanly).
      ASSERT_GE(H5AC_evict(f), 0);
      Faults().decode_calls = 0;
      Faults().decode_fail_at = leaf_call;
      EXPECT_LT(run_op(), 0);
    };

    bool found = false;
    measure_and_fail_decode([&]() -> herr_t {
      return H5B2_find(bt2.get(), &q_leaf, &found, nullptr, nullptr);
    });
    EXPECT_TRUE(found);

    FoundState fs;
    measure_and_fail_decode([&]() -> herr_t {
      return H5B2_index(bt2.get(), H5_ITER_INC, leaf_idx, CaptureFoundKVOp,
                        &fs);
    });
    EXPECT_EQ(fs.rec.key, q_leaf.key);

    ModifyState ms;
    ms.should_change = false;
    measure_and_fail_decode([&]() -> herr_t {
      return H5B2_modify(bt2.get(), &q_leaf, /*try=*/false, ModifyKVOp, &ms);
    });
  }
}

}  // namespace
}  // namespace hdf5

