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
// Tests v1 B-tree debugging and structural verification routines
// (third_party/hdf5/src/H5Bdbg.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Bdbg.c

#include <stdio.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

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
#define H5BDBG_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5BDBG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5BDBG_TEST_RESTORE_UNDEF_NDEBUG
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5BDBG_HAS_ASAN 1
#endif
#endif
#if defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__)
#define H5BDBG_HAS_ASAN 1
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

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

struct DbgKey {
  int64_t value = 0;
};

struct DbgTreeContext {
  H5UC_t* rc_shared = nullptr;
  int64_t key = 0;
  haddr_t next_leaf_addr = 0x3000;
  bool return_null_shared = false;
};

H5UC_t* DbgGetShared(const H5F_t* /*f*/, const void* udata) {
  const auto* ctx = static_cast<const DbgTreeContext*>(udata);
  if (ctx == nullptr || ctx->return_null_shared) {
    return nullptr;
  }
  return ctx->rc_shared;
}

herr_t DbgNewNode(H5F_t* /*f*/, H5B_ins_t op, void* lt_key, void* udata,
                  void* rt_key, haddr_t* addr_p) {
  auto* ctx = static_cast<DbgTreeContext*>(udata);
  auto* lt = static_cast<DbgKey*>(lt_key);
  auto* rt = static_cast<DbgKey*>(rt_key);
  lt->value = ctx->key;
  if (op != H5B_INS_LEFT) {
    rt->value = ctx->key + 1;
  }
  *addr_p = ctx->next_leaf_addr;
  ctx->next_leaf_addr += 16;
  return 0;
}

int DbgCmp2(void* lt_key, void* /*udata*/, void* rt_key) {
  const auto* lt = static_cast<const DbgKey*>(lt_key);
  const auto* rt = static_cast<const DbgKey*>(rt_key);
  if (lt->value < rt->value) return -1;
  if (lt->value > rt->value) return 1;
  return 0;
}

int DbgCmp3(void* lt_key, void* udata, void* rt_key) {
  const auto* lt = static_cast<const DbgKey*>(lt_key);
  const auto* rt = static_cast<const DbgKey*>(rt_key);
  const auto* ctx = static_cast<const DbgTreeContext*>(udata);
  if (ctx->key < lt->value) return -1;
  if (ctx->key >= rt->value) return 1;
  return 0;
}

htri_t DbgFound(H5F_t* /*f*/, haddr_t /*addr*/, const void* lt_key, bool* found,
                void* udata) {
  const auto* lt = static_cast<const DbgKey*>(lt_key);
  const auto* ctx = static_cast<const DbgTreeContext*>(udata);
  *found = (lt->value == ctx->key);
  return 0;
}

H5B_ins_t DbgInsert(H5F_t* /*f*/, haddr_t /*addr*/, void* /*lt_key*/,
                    bool* /*lt_key_changed*/, void* /*md_key*/, void* /*udata*/,
                    void* /*rt_key*/, bool* /*rt_key_changed*/,
                    haddr_t* /*new_node*/) {
  return H5B_INS_NOOP;
}

herr_t DbgDecodeKey(const H5B_shared_t* /*shared*/, const uint8_t* raw,
                    void* key_out) {
  uint64_t encoded = 0;
  for (int i = 0; i < 8; ++i) {
    encoded |= static_cast<uint64_t>(raw[i]) << (i * 8);
  }
  static_cast<DbgKey*>(key_out)->value = static_cast<int64_t>(encoded);
  return 0;
}

herr_t DbgEncodeKey(const H5B_shared_t* /*shared*/, uint8_t* raw,
                    const void* key_in) {
  const uint64_t val =
      static_cast<uint64_t>(static_cast<const DbgKey*>(key_in)->value);
  for (int i = 0; i < 8; ++i) {
    raw[i] = static_cast<uint8_t>((val >> (i * 8)) & 0xffU);
  }
  return 0;
}

herr_t DbgPrintKey(FILE* stream, int indent, int fwidth, const void* key,
                   const void* /*udata*/) {
  const auto* k = static_cast<const DbgKey*>(key);
  fprintf(stream, "%*s%-*s %lld\n", indent, "", fwidth,
          "Key:", static_cast<long long>(k->value));
  return 0;
}

H5B_class_t MakeDbgClass(H5B_subid_t id, bool with_debug_key) {
  H5B_class_t cls{};
  cls.id = id;
  cls.sizeof_nkey = sizeof(DbgKey);
  cls.get_shared = DbgGetShared;
  cls.new_node = DbgNewNode;
  cls.cmp2 = DbgCmp2;
  cls.cmp3 = DbgCmp3;
  cls.found = DbgFound;
  cls.insert = DbgInsert;
  cls.follow_min = false;
  cls.follow_max = false;
  cls.critical_key = H5B_LEFT;
  cls.remove = nullptr;
  cls.decode = DbgDecodeKey;
  cls.encode = DbgEncodeKey;
  cls.debug_key = with_debug_key ? DbgPrintKey : nullptr;
  return cls;
}

ScopedFile CreateFileWithK(unsigned ik) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedFile();
  if (H5Pset_istore_k(fcpl.get(), ik) < 0 ||
      H5Pset_sym_k(fcpl.get(), ik, ik) < 0) {
    return ScopedFile();
  }
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  return ScopedFile(
      H5Fcreate("h5bdbg_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string CaptureBtreeDebug(H5F_t* f, haddr_t addr, const H5B_class_t* type,
                              void* udata, herr_t* rc_out) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    *rc_out = -1;
    return "";
  }
  absl::Cleanup free_buf = [&buf] { std::free(buf); };
  *rc_out =
      H5B_debug(f, addr, stream, /*indent=*/2, /*fwidth=*/24, type, udata);
  fclose(stream);
  return buf != nullptr ? std::string(buf, len) : std::string();
}

TEST(H5BdbgTest, DebugPrintsNodeMetadataForAllTreeTypesAndDirtyStates) {
  H5B_class_t chunk_cls = MakeDbgClass(H5B_CHUNK_ID, /*with_debug_key=*/true);
  H5B_class_t chunk_nodbg_cls =
      MakeDbgClass(H5B_CHUNK_ID, /*with_debug_key=*/false);
  DbgTreeContext chunk_ctx;
  H5B_class_t snode_cls = MakeDbgClass(H5B_SNODE_ID, /*with_debug_key=*/false);
  DbgTreeContext snode_ctx;
  H5B_class_t base_cls = MakeDbgClass(H5B_CHUNK_ID, /*with_debug_key=*/false);
  H5B_class_t unknown_cls =
      MakeDbgClass(H5B_NUM_BTREE_ID, /*with_debug_key=*/false);
  DbgTreeContext unknown_ctx;
  ScopedShared chunk_shared;
  ScopedShared snode_shared;
  ScopedShared unknown_shared;
  ScopedFile file = CreateFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Test H5B_CHUNK_ID with debug_key present and absent, dirty and clean.
  H5B_shared_t* raw_chunk = H5B_shared_new(f, &chunk_cls, sizeof(DbgKey));
  ASSERT_NE(raw_chunk, nullptr);
  chunk_shared.reset(H5UC_create(raw_chunk, H5B_shared_free));
  ASSERT_NE(chunk_shared.get(), nullptr);
  chunk_ctx.rc_shared = chunk_shared.get();

  haddr_t chunk_addr = HADDR_UNDEF;
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    ASSERT_GE(H5B_create(f, &chunk_cls, &chunk_ctx, &chunk_addr), 0);
    for (int64_t k = 10; k <= 30; k += 10) {
      chunk_ctx.key = k;
      ASSERT_GE(H5B_insert(f, &chunk_cls, chunk_addr, &chunk_ctx), 0);
    }

    herr_t rc = -1;
    std::string out =
        CaptureBtreeDebug(f, chunk_addr, &chunk_cls, &chunk_ctx, &rc);
    ASSERT_GE(rc, 0);
    EXPECT_THAT(out, HasSubstr("H5B_CHUNK_ID"));
    EXPECT_THAT(out, HasSubstr("Dirty flag:"));
    EXPECT_THAT(out, HasSubstr("True"));
    EXPECT_THAT(out, HasSubstr("Child 0..."));
    EXPECT_THAT(out, HasSubstr("Left Key:"));
    EXPECT_THAT(out, HasSubstr("Right Key:"));

    std::string out_nodbg =
        CaptureBtreeDebug(f, chunk_addr, &chunk_nodbg_cls, &chunk_ctx, &rc);
    ASSERT_GE(rc, 0);
    EXPECT_THAT(out_nodbg, HasSubstr("Child 0..."));
    EXPECT_THAT(out_nodbg, Not(HasSubstr("Left Key:")));
  }

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    herr_t rc = -1;
    std::string clean_out =
        CaptureBtreeDebug(f, chunk_addr, &chunk_cls, &chunk_ctx, &rc);
    ASSERT_GE(rc, 0);
    EXPECT_THAT(clean_out, HasSubstr("False"));
  }

  // 2. Test H5B_SNODE_ID label branch.
  H5B_shared_t* raw_snode = H5B_shared_new(f, &snode_cls, sizeof(DbgKey));
  ASSERT_NE(raw_snode, nullptr);
  snode_shared.reset(H5UC_create(raw_snode, H5B_shared_free));
  ASSERT_NE(snode_shared.get(), nullptr);
  snode_ctx.rc_shared = snode_shared.get();
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    haddr_t snode_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(f, &snode_cls, &snode_ctx, &snode_addr), 0);
    herr_t rc = -1;
    std::string snode_out =
        CaptureBtreeDebug(f, snode_addr, &snode_cls, &snode_ctx, &rc);
    ASSERT_GE(rc, 0);
    EXPECT_THAT(snode_out, HasSubstr("H5B_SNODE_ID"));
  }

  // 3. Test "Unknown!" label branch using H5B_NUM_BTREE_ID after
  // H5B_shared_new.
  H5B_shared_t* raw_unknown = H5B_shared_new(f, &base_cls, sizeof(DbgKey));
  ASSERT_NE(raw_unknown, nullptr);
  raw_unknown->type = &unknown_cls;
  unknown_shared.reset(H5UC_create(raw_unknown, H5B_shared_free));
  ASSERT_NE(unknown_shared.get(), nullptr);
  unknown_ctx.rc_shared = unknown_shared.get();
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    haddr_t unknown_addr = HADDR_UNDEF;
    ASSERT_GE(H5B_create(f, &unknown_cls, &unknown_ctx, &unknown_addr), 0);
    herr_t rc = -1;
    std::string unknown_out =
        CaptureBtreeDebug(f, unknown_addr, &unknown_cls, &unknown_ctx, &rc);
    ASSERT_GE(rc, 0);
    EXPECT_THAT(unknown_out, HasSubstr("Unknown!"));
  }

  file.reset();
}

TEST(H5BdbgTest, DebugHandlesGetSharedFailureAndInvalidAddress) {
  H5B_class_t cls = MakeDbgClass(H5B_CHUNK_ID, /*with_debug_key=*/false);
  DbgTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(DbgKey));
  ASSERT_NE(raw_shared, nullptr);
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  ASSERT_NE(shared.get(), nullptr);
  ctx.rc_shared = shared.get();

  ScopedErrorSilence silence;
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. get_shared returning nullptr triggers H5E_CANTGET.
  ctx.return_null_shared = true;
  herr_t rc = 0;
  (void)CaptureBtreeDebug(f, 0x1000, &cls, &ctx, &rc);
  EXPECT_LT(rc, 0);

  // 2. Unallocated address beyond EOA fails H5AC_protect before deserialize.
  ctx.return_null_shared = false;
  (void)CaptureBtreeDebug(f, 0x10000000ULL, &cls, &ctx, &rc);
  EXPECT_LT(rc, 0);

  file.reset();
}

TEST(H5BdbgTest, VerifyStructureSucceedsOnMultiLevelTreeAndRejectsBadInputs) {
  H5B_class_t cls = MakeDbgClass(H5B_CHUNK_ID, /*with_debug_key=*/false);
  DbgTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(DbgKey));
  ASSERT_NE(raw_shared, nullptr);
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  ASSERT_NE(shared.get(), nullptr);
  ctx.rc_shared = shared.get();

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  haddr_t root_addr = HADDR_UNDEF;
  ASSERT_GE(H5B_create(f, &cls, &ctx, &root_addr), 0);
  // With K=2 (two_k=4), inserting 22 sequential keys yields a 3-level tree
  // (level 2 root, multiple level 1 internal nodes, multiple level 0 leaves).
  for (int64_t k = 10; k <= 220; k += 10) {
    ctx.key = k;
    ASSERT_GE(H5B_insert(f, &cls, root_addr, &ctx), 0);
  }

  // 1. Valid multi-level tree passes BFS structural verification.
  EXPECT_GE(H5B__verify_structure(f, root_addr, &cls, &ctx), 0);

  // 2. Error paths before entering the BFS queue.
  {
    ScopedErrorSilence silence;
    ctx.return_null_shared = true;
    EXPECT_LT(H5B__verify_structure(f, root_addr, &cls, &ctx), 0);
    ctx.return_null_shared = false;

    EXPECT_LT(H5B__verify_structure(f, 0x10000000ULL, &cls, &ctx), 0);
  }

  file.reset();
}

// Upstream H5B__verify_structure frees its BFS `child_t` queue before `done:`,
// so any error inside `while (cur)` intentionally leaks queued `child_t` nodes.
// Guard under ASan/LSan per AGENTS.md §9.3, and run in a threadsafe death-test
// subprocess so `--gunit_internal_run_death_test` turns off Google's at-exit
// HeapLeakChecker in the child while still flushing child gcov/LLVM coverage.
#if !defined(H5BDBG_HAS_ASAN)
bool RunCorruptStructureChecksInSubprocess() {
  H5B_class_t cls = MakeDbgClass(H5B_CHUNK_ID, /*with_debug_key=*/false);
  DbgTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateFileWithK(/*ik=*/2);
  if (!file.valid()) return false;
  H5F_t* f = GetH5File(file.get());
  if (f == nullptr) return false;

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(DbgKey));
  if (raw_shared == nullptr) return false;
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  if (shared.get() == nullptr) return false;
  ctx.rc_shared = shared.get();

  ScopedApiContext api_ctx;
  if (!api_ctx.valid()) return false;

  haddr_t root_addr = HADDR_UNDEF;
  if (H5B_create(f, &cls, &ctx, &root_addr) < 0) return false;
  for (int64_t k = 10; k <= 220; k += 10) {
    ctx.key = k;
    if (H5B_insert(f, &cls, root_addr, &ctx) < 0) return false;
  }

  H5B_cache_ud_t cache_udata{};
  cache_udata.f = f;
  cache_udata.type = &cls;
  cache_udata.rc_shared = shared.get();
  cache_udata.exp_level = H5B_UNKNOWN_NODELEVEL;

  auto* root = static_cast<H5B_t*>(
      H5AC_protect(f, H5AC_BT, root_addr, &cache_udata, H5AC__NO_FLAGS_SET));
  if (root == nullptr || root->level < 1U || root->nchildren < 2U) return false;
  const haddr_t child0_addr = root->child[0];
  const haddr_t child1_addr = root->child[1];
  if (H5AC_unprotect(f, H5AC_BT, root_addr, root, H5AC__NO_FLAGS_SET) < 0) {
    return false;
  }

  ScopedErrorSilence silence;
  bool all_failed_as_expected = true;

  auto verify_corrupt_node = [&](haddr_t target_addr, auto mutator,
                                 auto restorer) -> bool {
    auto* bt = static_cast<H5B_t*>(H5AC_protect(
        f, H5AC_BT, target_addr, &cache_udata, H5AC__NO_FLAGS_SET));
    if (bt == nullptr) return false;
    mutator(bt);
    if (H5AC_unprotect(f, H5AC_BT, target_addr, bt, H5AC__NO_FLAGS_SET) < 0) {
      return false;
    }

    if (H5B__verify_structure(f, root_addr, &cls, &ctx) >= 0) {
      all_failed_as_expected = false;
    } else {
      // H5B__verify_structure leaves `bt` protected on mid-loop failure.
      if (H5AC_unprotect(f, H5AC_BT, target_addr, bt, H5AC__NO_FLAGS_SET) < 0) {
        return false;
      }
    }

    bt = static_cast<H5B_t*>(H5AC_protect(f, H5AC_BT, target_addr, &cache_udata,
                                          H5AC__NO_FLAGS_SET));
    if (bt == nullptr) return false;
    restorer(bt);
    return H5AC_unprotect(f, H5AC_BT, target_addr, bt, H5AC__NO_FLAGS_SET) >= 0;
  };

  haddr_t saved_addr = HADDR_UNDEF;
  // 1. Corrupted left pointer on leftmost node (should be HADDR_UNDEF).
  if (!verify_corrupt_node(
          root_addr,
          [&](H5B_t* bt) {
            saved_addr = bt->left;
            bt->left = 0xdead0;
          },
          [&](H5B_t* bt) { bt->left = saved_addr; })) {
    return false;
  }

  // 2. Corrupted right pointer on rightmost node (should be HADDR_UNDEF).
  if (!verify_corrupt_node(
          root_addr,
          [&](H5B_t* bt) {
            saved_addr = bt->right;
            bt->right = 0xdead0;
          },
          [&](H5B_t* bt) { bt->right = saved_addr; })) {
    return false;
  }

  // 3. Cycle in children (child[1] == child[0]).
  if (!verify_corrupt_node(
          root_addr,
          [&](H5B_t* bt) {
            saved_addr = bt->child[1];
            bt->child[1] = bt->child[0];
          },
          [&](H5B_t* bt) { bt->child[1] = saved_addr; })) {
    return false;
  }

  // 4. Non-monotonically increasing keys (key[0] == key[1]).
  DbgKey saved_key{};
  if (!verify_corrupt_node(
          root_addr,
          [&](H5B_t* bt) {
            std::memcpy(&saved_key, H5B_NKEY(bt, raw_shared, 0),
                        sizeof(saved_key));
            std::memcpy(H5B_NKEY(bt, raw_shared, 0),
                        H5B_NKEY(bt, raw_shared, 1), sizeof(saved_key));
          },
          [&](H5B_t* bt) {
            std::memcpy(H5B_NKEY(bt, raw_shared, 0), &saved_key,
                        sizeof(saved_key));
          })) {
    return false;
  }

  // 5. Mismatched child level.
  unsigned saved_level = 0;
  if (!verify_corrupt_node(
          child0_addr,
          [&](H5B_t* bt) {
            saved_level = bt->level;
            bt->level = 99;
          },
          [&](H5B_t* bt) { bt->level = saved_level; })) {
    return false;
  }

  // 6. Broken right-sibling link between child[0] and child[1].
  if (!verify_corrupt_node(
          child0_addr,
          [&](H5B_t* bt) {
            saved_addr = bt->right;
            bt->right = 0xdead0;
          },
          [&](H5B_t* bt) { bt->right = saved_addr; })) {
    return false;
  }

  // 7. Broken left-sibling link between child[1] and child[0].
  if (!verify_corrupt_node(
          child1_addr,
          [&](H5B_t* bt) {
            saved_addr = bt->left;
            bt->left = 0xdead0;
          },
          [&](H5B_t* bt) { bt->left = saved_addr; })) {
    return false;
  }

  file.reset();
  return all_failed_as_expected;
}

TEST(H5BdbgTest, VerifyStructureDetectsCorruptedTreeNodes) {
  EXPECT_EXIT(
      {
        if (RunCorruptStructureChecksInSubprocess()) {
          std::fprintf(stderr, "CORRUPT_CHECKS_OK\n");
          std::fflush(stderr);
          std::exit(0);
        }
        std::exit(1);
      },
      ::testing::ExitedWithCode(0), "CORRUPT_CHECKS_OK");
}
#endif  // !defined(H5BDBG_HAS_ASAN)

}  // namespace
}  // namespace hdf5
