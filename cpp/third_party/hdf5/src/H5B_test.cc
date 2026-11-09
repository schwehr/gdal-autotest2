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
// Tests v1 B-link tree creation, insertion, node splitting, lookup, iteration,
// key removal, whole-tree deletion, shared info management, and public HDF5
// chunked-dataset / symbol-table callers (third_party/hdf5/src/H5B.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t (H5AC_info_t), the first member of H5B_t. Because
// `hdf5_internal_prelude.h` -> `H5private.h` pulls `H5CXprivate.h` ->
// `H5ACprivate.h` -> `H5Cprivate.h`, NDEBUG must be defined around both the
// prelude and internal headers to match `:hdf5` struct offsets.
#ifndef NDEBUG
#define H5B_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B_FRIEND
#include "third_party/hdf5/src/H5Bpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5B_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper for pushing/popping an HDF5 API context and restoring tag state.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
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

// RAII wrapper around a ref-counted H5B_shared_t (H5UC_t*).
class ScopedShared {
 public:
  ScopedShared() = default;
  explicit ScopedShared(H5UC_t* rc) : rc_(rc) {}
  ~ScopedShared() { reset(); }

  ScopedShared(const ScopedShared&) = delete;
  ScopedShared& operator=(const ScopedShared&) = delete;

  void reset(H5UC_t* next = nullptr) {
    if (rc_ != nullptr) {
      H5UC_DEC(rc_);
    }
    rc_ = next;
  }

  H5UC_t* get() const { return rc_; }

 private:
  H5UC_t* rc_ = nullptr;
};

struct TestKey {
  int64_t value = 0;
  int64_t bound = 0;
};
static_assert(sizeof(TestKey) == 16);

struct TreeContext {
  H5UC_t* rc_shared = nullptr;
  int64_t key = 0;
  haddr_t next_leaf_addr = 0x4000;

  // Behavior switches for exercising specific H5B.c branches.
  bool return_null_shared = false;
  int get_shared_remaining = -1;
  bool fail_new_node = false;
  bool gap_below_rt = false;
  bool fail_found = false;
  bool exact_match_only = false;

  // Insert callback overrides. Note: H5B_INS_FIRST means default behavior.
  H5B_ins_t forced_insert_op = H5B_INS_FIRST;
  int64_t split_mid_value = 0;
  bool change_lt_key = false;
  bool change_rt_key = false;

  // Remove/delete callback overrides.
  H5B_ins_t forced_remove_op = H5B_INS_REMOVE;
  bool fail_remove_on_key = false;
  int64_t fail_remove_key = 0;
  bool remove_change_lt_key = false;
  bool remove_change_rt_key = false;

  // Iteration controls & recorded callbacks.
  std::vector<int64_t> visited_keys;
  std::vector<int64_t> deleted_keys;
  int stop_after_visits = -1;
  int error_after_visits = -1;
  int insert_calls = 0;
  int remove_calls = 0;
};

// H5B_valid passes udata=nullptr to type->get_shared(f, NULL). Tests of
// H5B_valid temporarily register a per-file H5UC_t* here and clean it up.
std::unordered_map<const H5F_t*, H5UC_t*>& ValidSharedRegistry() {
  static auto* registry = new std::unordered_map<const H5F_t*, H5UC_t*>();
  return *registry;
}

H5UC_t* TestGetShared(const H5F_t* f, const void* udata) {
  if (udata == nullptr) {
    const auto& reg = ValidSharedRegistry();
    auto it = reg.find(f);
    return it != reg.end() ? it->second : nullptr;
  }
  auto* ctx = static_cast<TreeContext*>(const_cast<void*>(udata));
  if (ctx->return_null_shared) {
    return nullptr;
  }
  if (ctx->get_shared_remaining == 0) {
    return nullptr;
  }
  if (ctx->get_shared_remaining > 0) {
    --ctx->get_shared_remaining;
  }
  return ctx->rc_shared;
}

herr_t TestNewNode(H5F_t* /*f*/, H5B_ins_t op, void* lt_key, void* udata,
                   void* rt_key, haddr_t* addr_p) {
  auto* ctx = static_cast<TreeContext*>(udata);
  if (ctx->fail_new_node) {
    return -1;
  }
  auto* lt = static_cast<TestKey*>(lt_key);
  auto* rt = static_cast<TestKey*>(rt_key);
  lt->value = ctx->key;
  lt->bound = ctx->key + 1;
  if (op != H5B_INS_LEFT) {
    rt->value = ctx->key + 1;
    rt->bound = ctx->key + 2;
  }
  *addr_p = ctx->next_leaf_addr;
  ctx->next_leaf_addr += 16;
  return 0;
}

int TestCmp2(void* lt_key, void* /*udata*/, void* rt_key) {
  const auto* lt = static_cast<const TestKey*>(lt_key);
  const auto* rt = static_cast<const TestKey*>(rt_key);
  if (lt->value < rt->value) return -1;
  if (lt->value > rt->value) return 1;
  return 0;
}

int TestCmp3(void* lt_key, void* udata, void* rt_key) {
  const auto* lt = static_cast<const TestKey*>(lt_key);
  const auto* rt = static_cast<const TestKey*>(rt_key);
  const auto* ctx = static_cast<const TreeContext*>(udata);
  if (ctx->key < lt->value) return -1;
  if (ctx->key >= rt->value) return 1;
  if (ctx->gap_below_rt && ctx->key >= lt->bound) return 1;
  return 0;
}

htri_t TestFound(H5F_t* /*f*/, haddr_t /*addr*/, const void* lt_key,
                 bool* found, void* udata) {
  const auto* lt = static_cast<const TestKey*>(lt_key);
  const auto* ctx = static_cast<const TreeContext*>(udata);
  if (ctx->fail_found) {
    return -1;
  }
  *found =
      ctx->exact_match_only ? (lt->value == ctx->key) : (ctx->key >= lt->value);
  return 0;
}

H5B_ins_t TestInsert(H5F_t* /*f*/, haddr_t /*addr*/, void* lt_key,
                     bool* lt_key_changed, void* md_key, void* udata,
                     void* rt_key, bool* rt_key_changed, haddr_t* new_node_p) {
  auto* ctx = static_cast<TreeContext*>(udata);
  auto* lt = static_cast<TestKey*>(lt_key);
  auto* rt = static_cast<TestKey*>(rt_key);
  auto* md = static_cast<TestKey*>(md_key);
  ++ctx->insert_calls;

  if (ctx->forced_insert_op == H5B_INS_ERROR) {
    return H5B_INS_ERROR;
  }
  if (ctx->change_lt_key) {
    if (ctx->key < lt->value) {
      lt->value = ctx->key;
    }
    lt->bound += 1;
    *lt_key_changed = true;
  }
  if (ctx->change_rt_key) {
    if (ctx->key >= rt->value) {
      rt->value = ctx->key + 1;
    }
    rt->bound += 1;
    *rt_key_changed = true;
  }
  if (ctx->forced_insert_op == H5B_INS_NOOP) {
    return H5B_INS_NOOP;
  }
  if (ctx->forced_insert_op == H5B_INS_CHANGE) {
    *new_node_p = ctx->next_leaf_addr;
    ctx->next_leaf_addr += 16;
    return H5B_INS_CHANGE;
  }
  if (ctx->forced_insert_op == H5B_INS_LEFT) {
    md->value = ctx->split_mid_value != 0 ? ctx->split_mid_value : ctx->key;
    md->bound = md->value + 1;
    *new_node_p = ctx->next_leaf_addr;
    ctx->next_leaf_addr += 16;
    return H5B_INS_LEFT;
  }
  if (ctx->forced_insert_op == H5B_INS_RIGHT) {
    md->value = ctx->split_mid_value != 0 ? ctx->split_mid_value : ctx->key;
    md->bound = md->value + 1;
    *new_node_p = ctx->next_leaf_addr;
    ctx->next_leaf_addr += 16;
    return H5B_INS_RIGHT;
  }
  if (ctx->key > lt->value) {
    md->value = ctx->key;
    md->bound = ctx->key + 1;
    *new_node_p = ctx->next_leaf_addr;
    ctx->next_leaf_addr += 16;
    return H5B_INS_RIGHT;
  }
  return H5B_INS_NOOP;
}

H5B_ins_t TestRemove(H5F_t* /*f*/, haddr_t /*addr*/, void* lt_key,
                     bool* lt_key_changed, void* udata, void* rt_key,
                     bool* rt_key_changed) {
  auto* ctx = static_cast<TreeContext*>(udata);
  auto* lt = static_cast<TestKey*>(lt_key);
  auto* rt = static_cast<TestKey*>(rt_key);
  ++ctx->remove_calls;
  ctx->deleted_keys.push_back(lt->value);

  if (ctx->forced_remove_op == H5B_INS_ERROR ||
      (ctx->fail_remove_on_key && lt->value == ctx->fail_remove_key)) {
    return H5B_INS_ERROR;
  }
  if (ctx->forced_remove_op == H5B_INS_NOOP) {
    if (ctx->remove_change_lt_key) {
      lt->bound += 1;
      *lt_key_changed = true;
    }
    if (ctx->remove_change_rt_key) {
      rt->bound += 1;
      *rt_key_changed = true;
    }
    return H5B_INS_NOOP;
  }
  return H5B_INS_REMOVE;
}

herr_t TestDecodeKey(const H5B_shared_t* /*shared*/, const uint8_t* raw,
                     void* key_out) {
  auto decode_i64 = [](const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return static_cast<int64_t>(v);
  };
  auto* k = static_cast<TestKey*>(key_out);
  k->value = decode_i64(raw);
  k->bound = decode_i64(raw + 8);
  return 0;
}

herr_t TestEncodeKey(const H5B_shared_t* /*shared*/, uint8_t* raw,
                     const void* key_in) {
  auto encode_i64 = [](uint8_t* p, int64_t sval) {
    const uint64_t v = static_cast<uint64_t>(sval);
    for (int i = 0; i < 8; ++i) {
      p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffU);
    }
  };
  const auto* k = static_cast<const TestKey*>(key_in);
  encode_i64(raw, k->value);
  encode_i64(raw + 8, k->bound);
  return 0;
}

int TestIterateOp(H5F_t* /*f*/, const void* lt_key, haddr_t /*addr*/,
                  const void* /*rt_key*/, void* udata) {
  auto* ctx = static_cast<TreeContext*>(udata);
  const auto* lt = static_cast<const TestKey*>(lt_key);
  ctx->visited_keys.push_back(lt->value);
  const int count = static_cast<int>(ctx->visited_keys.size());
  if (ctx->error_after_visits > 0 && count >= ctx->error_after_visits) {
    return H5_ITER_ERROR;
  }
  if (ctx->stop_after_visits > 0 && count >= ctx->stop_after_visits) {
    return H5_ITER_STOP;
  }
  return H5_ITER_CONT;
}

H5B_class_t MakeTestClass(H5B_dir_t critical_key = H5B_LEFT,
                          bool with_remove = false, bool follow_min = false,
                          bool follow_max = false) {
  H5B_class_t cls{};
  cls.id = H5B_CHUNK_ID;
  cls.sizeof_nkey = sizeof(TestKey);
  cls.get_shared = TestGetShared;
  cls.new_node = TestNewNode;
  cls.cmp2 = TestCmp2;
  cls.cmp3 = TestCmp3;
  cls.found = TestFound;
  cls.insert = TestInsert;
  cls.follow_min = follow_min;
  cls.follow_max = follow_max;
  cls.critical_key = critical_key;
  cls.remove = with_remove ? TestRemove : nullptr;
  cls.decode = TestDecodeKey;
  cls.encode = TestEncodeKey;
  cls.debug_key = nullptr;
  return cls;
}

ScopedFile CreateFileWithK(unsigned ik = 2, unsigned sym_ik = 2) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedFile();
  if (H5Pset_istore_k(fcpl.get(), ik) < 0 ||
      H5Pset_sym_k(fcpl.get(), sym_ik, sym_ik) < 0) {
    return ScopedFile();
  }
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) <
      0) {
    return ScopedFile();
  }
  return ScopedFile(
      H5Fcreate("h5b_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

struct TreeFixture {
  // Declare `cls`, `ctx`, and `shared` before `file` so `H5Fclose()` during
  // teardown flushes/frees cached B-tree nodes while `cls` (`H5B_shared_t::
  // type`), `ctx`, and `rc_shared` remain valid.
  H5B_class_t cls{};
  TreeContext ctx;
  H5B_shared_t* raw_shared = nullptr;
  H5F_t* f = nullptr;
  ScopedShared shared;
  ScopedFile file;

  ~TreeFixture() {
    file.reset();
    shared.reset();
    f = nullptr;
    raw_shared = nullptr;
  }

  bool Init(unsigned ik = 2, H5B_dir_t critical_key = H5B_LEFT,
            bool with_remove = false, bool follow_min = false,
            bool follow_max = false) {
    file = CreateFileWithK(ik, ik);
    if (!file.valid()) return false;
    f = GetH5File(file.get());
    if (f == nullptr) return false;
    cls = MakeTestClass(critical_key, with_remove, follow_min, follow_max);
    raw_shared = H5B_shared_new(f, &cls, sizeof(TestKey));
    if (raw_shared == nullptr) return false;
    shared.reset(H5UC_create(raw_shared, H5B_shared_free));
    if (shared.get() == nullptr) return false;
    ctx.rc_shared = shared.get();
    return true;
  }
};

TEST(H5BTest, CreateSplitInsertFindIterateInfoAndDeleteMultiLevelTree) {
  TreeFixture tf;
  ASSERT_TRUE(tf.Init(/*ik=*/2));

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  // Use custom split ratios to exercise right, left, middle splits and
  // nleft clamping (left=0.0 -> nleft++ at 0, mid=1.0 -> --nleft at two_k).
  ASSERT_GE(H5Pset_btree_ratios(dxpl.get(), /*left=*/0.0, /*middle=*/1.0,
                                /*right=*/0.5),
            0);

  haddr_t root_addr = HADDR_UNDEF;
  {
    ScopedApiContext api_ctx(dxpl.get());
    ASSERT_TRUE(api_ctx.valid());

    // 1. Create empty tree and verify empty iteration returns H5_ITER_CONT.
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    EXPECT_EQ(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx),
              H5_ITER_CONT);
    EXPECT_TRUE(tf.ctx.visited_keys.empty());

    // 2. Insert sequential, descending, and interleaved keys across >=3 levels
    //    so root, left, middle, and right nodes all split.
    for (int64_t k = 100; k <= 600; k += 20) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }
    for (int64_t k = 90; k >= 10; k -= 20) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }
    for (int64_t k = 150; k <= 450; k += 20) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    // 3. Re-inserting an existing minimum key follows internal/leaf branches
    //    without splitting (NOOP).
    tf.ctx.key = 10;
    tf.ctx.forced_insert_op = H5B_INS_NOOP;
    EXPECT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    EXPECT_GT(tf.ctx.insert_calls, 0);

    // 4. Verify lookups across found, too-small, too-large, and exact-miss.
    tf.ctx.exact_match_only = true;
    for (int64_t k : {10, 150, 300, 600}) {
      bool found = false;
      tf.ctx.key = k;
      ASSERT_GE(H5B_find(tf.f, &tf.cls, root_addr, &found, &tf.ctx), 0);
      EXPECT_TRUE(found);
    }
    for (int64_t missing : {5, 11, 999}) {
      bool found = true;
      tf.ctx.key = missing;
      ASSERT_GE(H5B_find(tf.f, &tf.cls, root_addr, &found, &tf.ctx), 0);
      EXPECT_FALSE(found);
    }

    // 5. Iterate full tree, early stop, and error return.
    tf.ctx.visited_keys.clear();
    EXPECT_EQ(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx),
              H5_ITER_CONT);
    EXPECT_TRUE(
        std::is_sorted(tf.ctx.visited_keys.begin(), tf.ctx.visited_keys.end()));
    EXPECT_GT(tf.ctx.visited_keys.size(), 40U);

    tf.ctx.visited_keys.clear();
    tf.ctx.stop_after_visits = 5;
    EXPECT_EQ(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx),
              H5_ITER_STOP);
    EXPECT_EQ(tf.ctx.visited_keys.size(), 5U);
    tf.ctx.stop_after_visits = -1;

    tf.ctx.visited_keys.clear();
    tf.ctx.error_after_visits = 3;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx),
                0);
    }
    tf.ctx.error_after_visits = -1;

    // 6. Query node count/size via H5B_get_info with op=nullptr and
    // op!=nullptr.
    H5B_info_t info{};
    ASSERT_GE(H5B_get_info(tf.f, &tf.cls, root_addr, &info, nullptr, &tf.ctx),
              0);
    EXPECT_GT(info.num_nodes, 10U);
    EXPECT_EQ(info.size, info.num_nodes * tf.raw_shared->sizeof_rnode);

    H5B_info_t info_with_op{};
    tf.ctx.visited_keys.clear();
    ASSERT_GE(H5B_get_info(tf.f, &tf.cls, root_addr, &info_with_op,
                           TestIterateOp, &tf.ctx),
              0);
    EXPECT_EQ(info_with_op.num_nodes, info.num_nodes);
    EXPECT_GT(tf.ctx.visited_keys.size(), 40U);

    tf.ctx.visited_keys.clear();
    tf.ctx.error_after_visits = 2;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5B_get_info(tf.f, &tf.cls, root_addr, &info_with_op,
                             TestIterateOp, &tf.ctx),
                0);
    }
    tf.ctx.error_after_visits = -1;

    // 7. Test H5B_valid (which calls get_shared(f, NULL)) on live/bad addrs.
    ValidSharedRegistry()[tf.f] = tf.shared.get();
    absl::Cleanup clear_valid_reg = [&] { ValidSharedRegistry().erase(tf.f); };
    EXPECT_GE(H5B_valid(tf.f, &tf.cls, root_addr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5B_valid(tf.f, &tf.cls, HADDR_UNDEF), 0);
      EXPECT_LT(H5B_valid(tf.f, &tf.cls, 0x10000000ULL), 0);
      ValidSharedRegistry().erase(tf.f);
      EXPECT_LT(H5B_valid(tf.f, &tf.cls, root_addr), 0);
    }

    // 8. Delete entire tree when remove == nullptr.
    EXPECT_GE(H5B_delete(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }
}

TEST(H5BTest, InsertLeafVariantsFollowMinMaxAndFullRootBoundaryUpdates) {
  // 1. Test follow_min=true and follow_max=true including H5B_INS_CHANGE,
  //    middle-node H5B_INS_LEFT/RIGHT insertions, and key-change propagation.
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2, H5B_LEFT, /*with_remove=*/false,
                        /*follow_min=*/true, /*follow_max=*/true));
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);

    // First node in empty tree invokes follow_min type->insert.
    tf.ctx.key = 100;
    tf.ctx.forced_insert_op = H5B_INS_NOOP;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    // Force follow_min callback error when inserting first node on another
    // tree.
    haddr_t err_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &err_addr), 0);
    {
      ScopedErrorSilence silence;
      tf.ctx.forced_insert_op = H5B_INS_ERROR;
      EXPECT_LT(H5B_insert(tf.f, &tf.cls, err_addr, &tf.ctx), 0);
    }

    // follow_min on non-empty node (< 100) updating left key via insert.
    tf.ctx.key = 50;
    tf.ctx.forced_insert_op = H5B_INS_NOOP;
    tf.ctx.change_lt_key = true;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.change_lt_key = false;

    // follow_max on non-empty node (>= right) returning H5B_INS_RIGHT split.
    tf.ctx.key = 200;
    tf.ctx.forced_insert_op = H5B_INS_RIGHT;
    tf.ctx.split_mid_value = 200;
    tf.ctx.change_rt_key = true;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.change_rt_key = false;

    // Insert H5B_INS_LEFT in middle (idx=0 when nchildren==2 -> memmove branch)
    // then in last slot (idx+1==nchildren -> fast-path branch).
    tf.ctx.key = 100;
    tf.ctx.forced_insert_op = H5B_INS_LEFT;
    tf.ctx.split_mid_value = 120;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    tf.ctx.key = 250;
    tf.ctx.forced_insert_op = H5B_INS_RIGHT;
    tf.ctx.split_mid_value = 250;
    tf.ctx.change_rt_key = true;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.change_rt_key = false;

    // Now root is full (nchildren==4). Split idx==3 via follow_max error first.
    {
      ScopedErrorSilence silence;
      tf.ctx.key = 900;
      tf.ctx.forced_insert_op = H5B_INS_ERROR;
      EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

      tf.ctx.key = 10;
      EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    // Exercise H5B_INS_CHANGE with idx > 0 (clamps *lt_key_changed=false).
    tf.ctx.key = 150;
    tf.ctx.forced_insert_op = H5B_INS_CHANGE;
    tf.ctx.change_lt_key = true;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.change_lt_key = false;

    // Split full root at idx==3 (idx >= nleft) via follow_max.
    tf.ctx.key = 260;
    tf.ctx.forced_insert_op = H5B_INS_RIGHT;
    tf.ctx.split_mid_value = 260;
    tf.ctx.change_rt_key = true;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.change_rt_key = false;
    tf.ctx.forced_insert_op = H5B_INS_FIRST;
    tf.ctx.split_mid_value = 0;

    // Exercise follow_min error and middle leaf error.
    {
      ScopedErrorSilence silence;
      tf.ctx.forced_insert_op = H5B_INS_ERROR;
      tf.ctx.key = 5;
      EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      tf.ctx.key = 150;
      EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      tf.ctx.key = 999;
      EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }
  }

  // 2. Test H5B_RIGHT critical_key with rt_key_changed on middle child and
  //    rightmost root split updating rt_key (H5B_insert line 619).
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2, H5B_RIGHT, /*with_remove=*/false,
                        /*follow_min=*/false, /*follow_max=*/false));
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    for (int64_t k : {10, 20, 30, 40}) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }
    // Root is full [10,20,30,40]. Inserting 50 takes !follow_max, sets
    // *rt_key_changed = true, and splits root (hits line 619 false branch).
    tf.ctx.key = 50;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    // Update middle child with rt_key_changed=true (hits line 1021-1025).
    tf.ctx.key = 15;
    tf.ctx.forced_insert_op = H5B_INS_CHANGE;
    tf.ctx.change_rt_key = true;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.change_rt_key = false;
  }

  // 3. Test root split with lt_key_changed == true (line 617) by filling root
  //    with 4 keys and inserting a new minimum key with !follow_min.
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2, H5B_LEFT));
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    for (int64_t k : {100, 200, 300, 400}) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }
    tf.ctx.key = 10;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }
}

TEST(H5BTest, RemoveHandlesAllCasesAndNeighborPatchingForLeftAndRightKeys) {
  // 1. critical_key == H5B_LEFT: build 2-level tree, exercise NOOP key changes,
  //    leftmost/middle/rightmost child removals, sibling key patching at
  //    level > 0, empty non-root node deletion (left/both/right siblings),
  //    and emptying root back to level=0.
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2, H5B_LEFT, /*with_remove=*/true));
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    // Insert 10 keys -> splits into 3+ level-0 leaves under level-1 root.
    for (int64_t k = 10; k <= 100; k += 10) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    // Iterate to inspect keys before removal.
    ASSERT_GE(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx), 0);
    const std::vector<int64_t> all_keys = tf.ctx.visited_keys;
    ASSERT_EQ(all_keys.size(), 10U);

    // NOOP removal updating lt_key for idx==0 and idx>0.
    tf.ctx.forced_remove_op = H5B_INS_NOOP;
    tf.ctx.remove_change_lt_key = true;
    tf.ctx.key = 10;
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.key = 50;
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.remove_change_lt_key = false;

    // Protect root to learn child leaf layout for targeted case coverage.
    H5B_cache_ud_t cache_ud{};
    cache_ud.f = tf.f;
    cache_ud.type = &tf.cls;
    cache_ud.rc_shared = tf.shared.get();
    cache_ud.exp_level = H5B_UNKNOWN_NODELEVEL;
    auto* root = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->level, 1U);
    ASSERT_GE(root->nchildren, 3U);

    // Gather first key of each leaf under root.
    std::vector<std::vector<int64_t>> leaf_keys(root->nchildren);
    for (unsigned i = 0; i < root->nchildren; ++i) {
      auto* leaf = static_cast<H5B_t*>(H5AC_protect(
          tf.f, H5AC_BT, root->child[i], &cache_ud, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(leaf, nullptr);
      for (unsigned j = 0; j < leaf->nchildren; ++j) {
        TestKey k{};
        std::memcpy(&k, H5B_NKEY(leaf, tf.raw_shared, j), sizeof(k));
        leaf_keys[i].push_back(k.value);
      }
      ASSERT_GE(H5AC_unprotect(tf.f, H5AC_BT, root->child[i], leaf,
                               H5AC__NO_FLAGS_SET),
                0);
    }
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);

    tf.ctx.forced_remove_op = H5B_INS_REMOVE;

    // Case: idx==0 on middle leaf (bt->left defined, level > 0) -> patches
    // left sibling's rightmost key!
    ASSERT_GE(leaf_keys[1].size(), 2U);
    tf.ctx.key = leaf_keys[1][0];
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    // If leaf 1 had >= 3 keys, remove a middle child too; then remove remaining
    // keys from middle leaf until nchildren==1 reaches 0 -> unlinks node with
    // both left and right siblings!
    for (size_t j = 1; j < leaf_keys[1].size(); ++j) {
      tf.ctx.key = leaf_keys[1][j];
      ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    // Remove remaining keys in rightmost leaf (right sibling is HADDR_UNDEF)
    // and leftmost leaf (left sibling is HADDR_UNDEF) until root resets to 0.
    for (size_t i = 2; i < leaf_keys.size(); ++i) {
      // Remove right-to-left to hit idx+1 == nchildren on multi-child leaves.
      for (auto it = leaf_keys[i].rbegin(); it != leaf_keys[i].rend(); ++it) {
        tf.ctx.key = *it;
        ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      }
    }
    if (leaf_keys[0].size() >= 3U) {
      // Remove middle key first.
      tf.ctx.key = leaf_keys[0][1];
      ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      tf.ctx.key = leaf_keys[0][0];
      ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      for (size_t j = 2; j < leaf_keys[0].size(); ++j) {
        tf.ctx.key = leaf_keys[0][j];
        ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      }
    } else {
      for (int64_t k : leaf_keys[0]) {
        tf.ctx.key = k;
        ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      }
    }

    // Root should now have nchildren == 0 and level == 0.
    tf.ctx.visited_keys.clear();
    EXPECT_EQ(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx),
              H5_ITER_CONT);
    EXPECT_TRUE(tf.ctx.visited_keys.empty());
  }

  // 2. critical_key == H5B_RIGHT: exercise H5B_RIGHT paths in all 4 removal
  //    cases and right-sibling patching, plus remove==nullptr removal.
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2, H5B_RIGHT, /*with_remove=*/true));
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    for (int64_t k = 10; k <= 100; k += 10) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    // NOOP updating rt_key at idx+1 < nchildren and idx+1 == nchildren.
    tf.ctx.forced_remove_op = H5B_INS_NOOP;
    tf.ctx.remove_change_rt_key = true;
    tf.ctx.key = 10;
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.key = 100;
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.remove_change_rt_key = false;

    H5B_cache_ud_t cache_ud{};
    cache_ud.f = tf.f;
    cache_ud.type = &tf.cls;
    cache_ud.rc_shared = tf.shared.get();
    cache_ud.exp_level = H5B_UNKNOWN_NODELEVEL;
    auto* root = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(root, nullptr);
    ASSERT_GE(root->nchildren, 3U);

    std::vector<std::vector<int64_t>> leaf_keys(root->nchildren);
    for (unsigned i = 0; i < root->nchildren; ++i) {
      auto* leaf = static_cast<H5B_t*>(H5AC_protect(
          tf.f, H5AC_BT, root->child[i], &cache_ud, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(leaf, nullptr);
      for (unsigned j = 0; j < leaf->nchildren; ++j) {
        TestKey k{};
        std::memcpy(&k, H5B_NKEY(leaf, tf.raw_shared, j), sizeof(k));
        leaf_keys[i].push_back(k.value);
      }
      ASSERT_GE(H5AC_unprotect(tf.f, H5AC_BT, root->child[i], leaf,
                               H5AC__NO_FLAGS_SET),
                0);
    }
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);

    // Switch type->remove to nullptr temporarily to cover no-callback removal.
    tf.cls.remove = nullptr;
    ASSERT_GE(leaf_keys[0].size(), 2U);
    tf.ctx.key = leaf_keys[0].front();  // idx == 0 under H5B_RIGHT.
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    tf.cls.remove = TestRemove;
    tf.ctx.forced_remove_op = H5B_INS_REMOVE;

    // Remove rightmost key of leaf 0 (bt->right defined, level > 0) -> patches
    // right sibling's leftmost key! Then finish clearing leaf 0.
    tf.ctx.key = leaf_keys[0].back();
    ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    for (size_t j = 1; j + 1 < leaf_keys[0].size(); ++j) {
      tf.ctx.key = leaf_keys[0][j];
      ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    // Clear middle leaf [1] (has bt->right defined; checks H5B_RIGHT in line
    // 1383) and remove middle child if >= 3 keys remain.
    for (size_t i = 1; i < leaf_keys.size(); ++i) {
      if (leaf_keys[i].size() >= 3U) {
        tf.ctx.key = leaf_keys[i][1];
        ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
        for (size_t j = 0; j < leaf_keys[i].size(); ++j) {
          if (j != 1) {
            tf.ctx.key = leaf_keys[i][j];
            ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
          }
        }
      } else {
        for (int64_t k : leaf_keys[i]) {
          tf.ctx.key = k;
          ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
        }
      }
    }
  }
}

TEST(H5BTest, ErrorPathsAndSelfCycleDetectionFailCleanly) {
  TreeFixture tf;
  ASSERT_TRUE(tf.Init(/*ik=*/2, H5B_LEFT, /*with_remove=*/true));
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  ScopedErrorSilence silence;

  haddr_t root_addr = HADDR_UNDEF;
  ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
  for (int64_t k = 10; k <= 100; k += 10) {
    tf.ctx.key = k;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // 1. get_shared == nullptr across H5B_find, H5B_insert, H5B_iterate,
  //    H5B_remove, H5B_delete, H5B_get_info.
  {
    bool found = false;
    H5B_info_t info{};
    tf.ctx.return_null_shared = true;
    EXPECT_LT(H5B_find(tf.f, &tf.cls, root_addr, &found, &tf.ctx), 0);
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    EXPECT_LT(H5B_iterate(tf.f, &tf.cls, root_addr, TestIterateOp, &tf.ctx), 0);
    EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    EXPECT_LT(H5B_delete(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    EXPECT_LT(H5B_get_info(tf.f, &tf.cls, root_addr, &info, nullptr, &tf.ctx),
              0);
    tf.ctx.return_null_shared = false;
  }

  // 2. Bad addresses fail H5AC_protect cleanly across all operations.
  {
    bool found = false;
    H5B_info_t info{};
    const haddr_t bad_addr = 0x10000000ULL;
    EXPECT_LT(H5B_find(tf.f, &tf.cls, bad_addr, &found, &tf.ctx), 0);
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, bad_addr, &tf.ctx), 0);
    EXPECT_LT(H5B_iterate(tf.f, &tf.cls, bad_addr, TestIterateOp, &tf.ctx), 0);
    EXPECT_LT(H5B_remove(tf.f, &tf.cls, bad_addr, &tf.ctx), 0);
    EXPECT_LT(H5B_delete(tf.f, &tf.cls, bad_addr, &tf.ctx), 0);
    EXPECT_LT(H5B_get_info(tf.f, &tf.cls, bad_addr, &info, nullptr, &tf.ctx),
              0);
  }

  // 3. H5B_find: type->found failure on leaf and propagation from level > 0.
  tf.ctx.key = 10;
  tf.ctx.fail_found = true;
  bool found = false;
  EXPECT_LT(H5B_find(tf.f, &tf.cls, root_addr, &found, &tf.ctx), 0);
  tf.ctx.fail_found = false;

  // 4. Mid-node gap check: cmp != 0 when key lies between child ranges triggers
  //    H5B__insert_helper "could not determine which branch" error.
  {
    haddr_t gap_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &gap_addr), 0);
    tf.ctx.key = 10;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, gap_addr, &tf.ctx), 0);
    tf.ctx.key = 30;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, gap_addr, &tf.ctx), 0);
    tf.ctx.gap_below_rt = true;
    tf.ctx.key = 20;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, gap_addr, &tf.ctx), 0);
    found = true;
    EXPECT_GE(H5B_find(tf.f, &tf.cls, gap_addr, &found, &tf.ctx), 0);
    EXPECT_FALSE(found);
    tf.ctx.gap_below_rt = false;
  }

  // 5. type->new_node failure on H5B_INS_FIRST, H5B_INS_LEFT, H5B_INS_RIGHT.
  {
    haddr_t empty_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &empty_addr), 0);
    tf.ctx.fail_new_node = true;
    tf.ctx.key = 50;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, empty_addr, &tf.ctx), 0);

    tf.ctx.fail_new_node = false;
    tf.ctx.key = 50;
    ASSERT_GE(H5B_insert(tf.f, &tf.cls, empty_addr, &tf.ctx), 0);

    tf.ctx.fail_new_node = true;
    tf.ctx.key = 10;  // new min -> H5B_INS_LEFT failure
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, empty_addr, &tf.ctx), 0);
    tf.ctx.key = 90;  // new max -> H5B_INS_RIGHT failure
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, empty_addr, &tf.ctx), 0);
    tf.ctx.fail_new_node = false;
  }

  // 6. H5B_remove: key not found & leaf remove error propagation.
  tf.ctx.key = 9999;
  EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  tf.ctx.key = 10;
  tf.ctx.forced_remove_op = H5B_INS_ERROR;
  EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  tf.ctx.forced_remove_op = H5B_INS_REMOVE;

  // 7. Corrupt root node temporarily to test self-cycle detection in H5B_find
  //    (bt->child[idx] == addr) and bad child/right links in get_info/find.
  H5B_cache_ud_t cache_ud{};
  cache_ud.f = tf.f;
  cache_ud.type = &tf.cls;
  cache_ud.rc_shared = tf.shared.get();
  cache_ud.exp_level = H5B_UNKNOWN_NODELEVEL;
  auto* root = static_cast<H5B_t*>(
      H5AC_protect(tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__NO_FLAGS_SET));
  ASSERT_NE(root, nullptr);
  ASSERT_GE(root->level, 1U);
  const haddr_t saved_child0 = root->child[0];
  root->child[0] = root_addr;
  ASSERT_GE(H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET),
            0);

  tf.ctx.key = 10;
  EXPECT_LT(H5B_find(tf.f, &tf.cls, root_addr, &found, &tf.ctx), 0);

  // Restore valid child[0], then corrupt child0->right for get_info failure.
  root = static_cast<H5B_t*>(
      H5AC_protect(tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__NO_FLAGS_SET));
  ASSERT_NE(root, nullptr);
  root->child[0] = saved_child0;
  ASSERT_GE(H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET),
            0);

  auto* child0 = static_cast<H5B_t*>(
      H5AC_protect(tf.f, H5AC_BT, saved_child0, &cache_ud, H5AC__NO_FLAGS_SET));
  ASSERT_NE(child0, nullptr);
  const haddr_t saved_right = child0->right;
  child0->right = 0x10000000ULL;
  ASSERT_GE(
      H5AC_unprotect(tf.f, H5AC_BT, saved_child0, child0, H5AC__NO_FLAGS_SET),
      0);

  H5B_info_t info{};
  EXPECT_LT(H5B_get_info(tf.f, &tf.cls, root_addr, &info, nullptr, &tf.ctx), 0);

  child0 = static_cast<H5B_t*>(
      H5AC_protect(tf.f, H5AC_BT, saved_child0, &cache_ud, H5AC__NO_FLAGS_SET));
  ASSERT_NE(child0, nullptr);
  child0->right = saved_right;
  ASSERT_GE(
      H5AC_unprotect(tf.f, H5AC_BT, saved_child0, child0, H5AC__NO_FLAGS_SET),
      0);

  // 8. H5B_delete: error callback on leaf propagates up cleanly, then delete
  //    the intact tree normally. Note that failed H5B_delete still unprotects
  //    the failed node with H5AC__DELETED_FLAG, so test failure on a separate
  //    small tree.
  {
    haddr_t partial_del_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &partial_del_addr), 0);
    for (int64_t k : {10, 50}) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, partial_del_addr, &tf.ctx), 0);
    }
    tf.ctx.fail_remove_on_key = true;
    tf.ctx.fail_remove_key = 50;
    EXPECT_LT(H5B_delete(tf.f, &tf.cls, partial_del_addr, &tf.ctx), 0);
    tf.ctx.fail_remove_on_key = false;
  }
  EXPECT_GE(H5B_delete(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
}

TEST(H5BTest, PublicSymbolTableAndChunkedDatasetExerciseV1Btrees) {
  ScopedFile file = CreateFileWithK(/*ik=*/2, /*sym_ik=*/2);
  ASSERT_TRUE(file.valid());

  // 1. Symbol-table v1 groups exercise H5B_SNODE_ID via public HDF5 APIs.
  constexpr int kNumGroups = 24;
  for (int i = 0; i < kNumGroups; ++i) {
    const std::string name = "grp_" + std::to_string(100 + i);
    ScopedGroup grp(H5Gcreate2(file.get(), name.c_str(), H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }
  EXPECT_GT(H5Lexists(file.get(), "grp_112", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(file.get(), "grp_missing", H5P_DEFAULT), 0);

  std::vector<std::string> iterated_names;
  auto visit_cb = [](hid_t /*grp*/, const char* name, const H5L_info2_t* /*i*/,
                     void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->push_back(name);
    return 0;
  };
  hsize_t idx = 0;
  ASSERT_GE(H5Literate2(file.get(), H5_INDEX_NAME, H5_ITER_INC, &idx, visit_cb,
                        &iterated_names),
            0);
  EXPECT_EQ(iterated_names.size(), static_cast<size_t>(kNumGroups));

  for (int i = 0; i < kNumGroups; i += 2) {
    const std::string name = "grp_" + std::to_string(100 + i);
    ASSERT_GE(H5Ldelete(file.get(), name.c_str(), H5P_DEFAULT), 0);
  }

  // 2. Extendible chunked dataset with earliest-format bounds uses v1 chunk
  //    B-tree (H5B_CHUNK_ID) across splits, overwrites, reads, and unlinks.
  const auto dims = std::to_array<hsize_t>({16, 16});
  const auto max_dims = std::to_array<hsize_t>({H5S_UNLIMITED, 16});
  const auto chunk_dims = std::to_array<hsize_t>({1, 16});
  ScopedSpace space(H5Screate_simple(2, dims.data(), max_dims.data()));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk_dims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "chunked", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::vector<int32_t> write_data(16 * 16);
  for (size_t i = 0; i < write_data.size(); ++i) {
    write_data[i] = static_cast<int32_t>(i);
  }
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, write_data.data()),
            0);

  // Overwrite one row to verify chunk lookup & update, then read back.
  std::vector<int32_t> read_data(16 * 16, -1);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_data.data()),
            0);
  EXPECT_EQ(read_data, write_data);

  dset.reset();
  ASSERT_GE(H5Ldelete(file.get(), "chunked", H5P_DEFAULT), 0);
}

TEST(H5BTest, MiddleSplitClampingAndNodeProtectFailurePaths) {
  auto make_cache_ud = [](TreeFixture& tf) {
    H5B_cache_ud_t cache_ud{};
    cache_ud.f = tf.f;
    cache_ud.type = &tf.cls;
    cache_ud.rc_shared = tf.shared.get();
    cache_ud.exp_level = H5B_UNKNOWN_NODELEVEL;
    return cache_ud;
  };

  auto read_node_keys = [](const H5B_t* node, const H5B_shared_t* raw_shared) {
    std::vector<int64_t> keys;
    keys.reserve(node->nchildren);
    for (unsigned j = 0; j < node->nchildren; ++j) {
      TestKey k{};
      std::memcpy(&k, H5B_NKEY(node, raw_shared, j), sizeof(k));
      keys.push_back(k.value);
    }
    return keys;
  };

  // 1. Middle-node split ratio with nleft == two_k clamped (--nleft, lines
  //    464 & 472), plus recursive get_shared failure (line 827) and corrupt
  //    child address checks during insert and delete (lines 884, 933, 987,
  //    1615).
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2));
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_btree_ratios(dxpl.get(), /*left=*/0.5, /*middle=*/1.0,
                                  /*right=*/0.5),
              0);
    ScopedApiContext api_ctx(dxpl.get());
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    for (int64_t k = 100; k <= 800; k += 100) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    H5B_cache_ud_t cache_ud = make_cache_ud(tf);
    auto* root = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->level, 1U);
    ASSERT_GE(root->nchildren, 3U);
    const haddr_t leaf1_addr = root->child[1];
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);

    auto* leaf1 = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, leaf1_addr, &cache_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(leaf1, nullptr);
    ASSERT_NE(leaf1->left, HADDR_UNDEF);
    ASSERT_NE(leaf1->right, HADDR_UNDEF);
    const std::vector<int64_t> leaf1_keys =
        read_node_keys(leaf1, tf.raw_shared);
    ASSERT_GE(leaf1_keys.size(), 2U);
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf1_addr, leaf1, H5AC__NO_FLAGS_SET),
        0);

    // Fill leaf1 (a middle node with left and right siblings) to two_k (4) and
    // split via an index strictly < nleft while middle split ratio == 1.0.
    const unsigned inserts_to_split =
        5U - static_cast<unsigned>(leaf1_keys.size());
    for (unsigned step = 1; step <= inserts_to_split; ++step) {
      tf.ctx.key = leaf1_keys[0] + static_cast<int64_t>(step);
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    ScopedErrorSilence silence;
    // Fail get_shared on the recursive child H5B__insert_helper call (line
    // 827).
    tf.ctx.get_shared_remaining = 2;
    tf.ctx.key = leaf1_keys[0] + 10;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    tf.ctx.get_shared_remaining = -1;

    // Corrupt root children temporarily to cover minimum, maximum, and middle
    // H5AC_protect failures in H5B__insert_helper, then H5B_delete (line 1615).
    root = static_cast<H5B_t*>(
        H5AC_protect(tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(root, nullptr);
    const haddr_t saved_child0 = root->child[0];
    root->child[0] = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);
    tf.ctx.key = 1;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    root = static_cast<H5B_t*>(
        H5AC_protect(tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(root, nullptr);
    root->child[0] = saved_child0;
    const unsigned last_idx = root->nchildren - 1U;
    const haddr_t saved_last = root->child[last_idx];
    root->child[last_idx] = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);
    tf.ctx.key = 9999;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    root = static_cast<H5B_t*>(
        H5AC_protect(tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(root, nullptr);
    root->child[last_idx] = saved_last;
    const haddr_t saved_mid = root->child[1];
    root->child[1] = 0x10000000ULL;
    TestKey mid_key{};
    std::memcpy(&mid_key, H5B_NKEY(root, tf.raw_shared, 1), sizeof(mid_key));
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);
    tf.ctx.key = mid_key.value + 1;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);

    root = static_cast<H5B_t*>(
        H5AC_protect(tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(root, nullptr);
    root->child[1] = saved_mid;
    root->child[0] = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);
    EXPECT_LT(H5B_delete(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // 2. H5B__split failure retrieving split ratios (non-DXPL plist, line 453).
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2));
    ScopedPlist non_dxpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(non_dxpl.valid());
    ScopedErrorSilence silence;

    haddr_t root_addr = HADDR_UNDEF;
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
      for (int64_t k : {10, 20, 30, 40}) {
        tf.ctx.key = k;
        ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
      }
    }
    ScopedApiContext bad_api_ctx(non_dxpl.get());
    ASSERT_TRUE(bad_api_ctx.valid());
    tf.ctx.key = 50;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // 3. H5B__split right-sibling protect error and cleanup (lines 519, 532-538,
  //    1052).
  {
    TreeFixture tf;
    ASSERT_TRUE(tf.Init(/*ik=*/2));
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t root_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, &root_addr), 0);
    for (int64_t k = 100; k <= 600; k += 100) {
      tf.ctx.key = k;
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    H5B_cache_ud_t cache_ud = make_cache_ud(tf);
    auto* root = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, root_addr, &cache_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(root, nullptr);
    const haddr_t leaf0_addr = root->child[0];
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET), 0);

    auto* leaf0 = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, leaf0_addr, &cache_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(leaf0, nullptr);
    const std::vector<int64_t> leaf0_keys =
        read_node_keys(leaf0, tf.raw_shared);
    ASSERT_GE(leaf0_keys.size(), 2U);
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf0_addr, leaf0, H5AC__NO_FLAGS_SET),
        0);

    const unsigned inserts_to_fill =
        4U - static_cast<unsigned>(leaf0_keys.size());
    for (unsigned step = 1; step <= inserts_to_fill; ++step) {
      tf.ctx.key = leaf0_keys[0] + static_cast<int64_t>(step);
      ASSERT_GE(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    leaf0 = static_cast<H5B_t*>(
        H5AC_protect(tf.f, H5AC_BT, leaf0_addr, &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(leaf0, nullptr);
    ASSERT_EQ(leaf0->nchildren, 4U);
    ASSERT_NE(leaf0->right, HADDR_UNDEF);
    leaf0->right = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf0_addr, leaf0, H5AC__NO_FLAGS_SET),
        0);

    ScopedErrorSilence silence;
    tf.ctx.key = leaf0_keys[0] + 10;
    EXPECT_LT(H5B_insert(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // 4. H5B__remove_helper sibling-protect errors (lines 1357, 1377, 1496,
  // 1512).
  auto build_three_leaf_tree =
      [&](H5B_dir_t critical_key, TreeFixture& tf, haddr_t* root_addr_out,
          std::vector<haddr_t>* leaf_addrs_out,
          std::vector<std::vector<int64_t>>* keys_out) {
        ASSERT_TRUE(tf.Init(/*ik=*/2, critical_key, /*with_remove=*/true));
        ASSERT_GE(H5B_create(tf.f, &tf.cls, &tf.ctx, root_addr_out), 0);
        for (int64_t k = 10; k <= 100; k += 10) {
          tf.ctx.key = k;
          ASSERT_GE(H5B_insert(tf.f, &tf.cls, *root_addr_out, &tf.ctx), 0);
        }
        H5B_cache_ud_t cache_ud = make_cache_ud(tf);
        auto* root = static_cast<H5B_t*>(H5AC_protect(
            tf.f, H5AC_BT, *root_addr_out, &cache_ud, H5AC__READ_ONLY_FLAG));
        ASSERT_NE(root, nullptr);
        ASSERT_GE(root->nchildren, 3U);
        leaf_addrs_out->clear();
        keys_out->clear();
        for (unsigned i = 0; i < root->nchildren; ++i) {
          leaf_addrs_out->push_back(root->child[i]);
          auto* leaf = static_cast<H5B_t*>(H5AC_protect(
              tf.f, H5AC_BT, root->child[i], &cache_ud, H5AC__READ_ONLY_FLAG));
          ASSERT_NE(leaf, nullptr);
          keys_out->push_back(read_node_keys(leaf, tf.raw_shared));
          ASSERT_GE(H5AC_unprotect(tf.f, H5AC_BT, root->child[i], leaf,
                                   H5AC__NO_FLAGS_SET),
                    0);
        }
        ASSERT_GE(H5AC_unprotect(tf.f, H5AC_BT, *root_addr_out, root,
                                 H5AC__NO_FLAGS_SET),
                  0);
      };

  // Line 1496: H5B_LEFT neighbor key patching with bad bt->left.
  {
    TreeFixture tf;
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    haddr_t root_addr = HADDR_UNDEF;
    std::vector<haddr_t> leaf_addrs;
    std::vector<std::vector<int64_t>> leaf_keys;
    build_three_leaf_tree(H5B_LEFT, tf, &root_addr, &leaf_addrs, &leaf_keys);
    ASSERT_GE(leaf_keys[1].size(), 2U);

    H5B_cache_ud_t cache_ud = make_cache_ud(tf);
    auto* leaf1 = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, leaf_addrs[1], &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(leaf1, nullptr);
    leaf1->left = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf_addrs[1], leaf1, H5AC__NO_FLAGS_SET),
        0);

    ScopedErrorSilence silence;
    tf.ctx.key = leaf_keys[1].front();
    EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // Line 1357: unlinking last entry of leaf1 with bad bt->left.
  {
    TreeFixture tf;
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    haddr_t root_addr = HADDR_UNDEF;
    std::vector<haddr_t> leaf_addrs;
    std::vector<std::vector<int64_t>> leaf_keys;
    build_three_leaf_tree(H5B_LEFT, tf, &root_addr, &leaf_addrs, &leaf_keys);
    for (size_t j = 0; j + 1 < leaf_keys[1].size(); ++j) {
      tf.ctx.key = leaf_keys[1][j];
      ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    H5B_cache_ud_t cache_ud = make_cache_ud(tf);
    auto* leaf1 = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, leaf_addrs[1], &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(leaf1, nullptr);
    leaf1->left = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf_addrs[1], leaf1, H5AC__NO_FLAGS_SET),
        0);

    ScopedErrorSilence silence;
    tf.ctx.key = leaf_keys[1].back();
    EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // Line 1377: unlinking last entry of leaf0 with bad bt->right.
  {
    TreeFixture tf;
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    haddr_t root_addr = HADDR_UNDEF;
    std::vector<haddr_t> leaf_addrs;
    std::vector<std::vector<int64_t>> leaf_keys;
    build_three_leaf_tree(H5B_LEFT, tf, &root_addr, &leaf_addrs, &leaf_keys);
    for (size_t j = 0; j + 1 < leaf_keys[0].size(); ++j) {
      tf.ctx.key = leaf_keys[0][j];
      ASSERT_GE(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
    }

    H5B_cache_ud_t cache_ud = make_cache_ud(tf);
    auto* leaf0 = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, leaf_addrs[0], &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(leaf0, nullptr);
    leaf0->right = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf_addrs[0], leaf0, H5AC__NO_FLAGS_SET),
        0);

    ScopedErrorSilence silence;
    tf.ctx.key = leaf_keys[0].back();
    EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }

  // Line 1512: H5B_RIGHT neighbor key patching with bad bt->right.
  {
    TreeFixture tf;
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    haddr_t root_addr = HADDR_UNDEF;
    std::vector<haddr_t> leaf_addrs;
    std::vector<std::vector<int64_t>> leaf_keys;
    build_three_leaf_tree(H5B_RIGHT, tf, &root_addr, &leaf_addrs, &leaf_keys);
    ASSERT_GE(leaf_keys[0].size(), 2U);

    H5B_cache_ud_t cache_ud = make_cache_ud(tf);
    auto* leaf0 = static_cast<H5B_t*>(H5AC_protect(
        tf.f, H5AC_BT, leaf_addrs[0], &cache_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(leaf0, nullptr);
    leaf0->right = 0x10000000ULL;
    ASSERT_GE(
        H5AC_unprotect(tf.f, H5AC_BT, leaf_addrs[0], leaf0, H5AC__NO_FLAGS_SET),
        0);

    ScopedErrorSilence silence;
    tf.ctx.key = leaf_keys[0].back();
    EXPECT_LT(H5B_remove(tf.f, &tf.cls, root_addr, &tf.ctx), 0);
  }
}

}  // namespace
}  // namespace hdf5
