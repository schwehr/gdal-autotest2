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
// Tests v1 B-tree metadata cache callbacks (third_party/hdf5/src/H5Bcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Bcache.c

#include <cstdint>
#include <cstring>
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
#define H5BCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5BCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5BCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::Eq;

// RAII wrapper for pushing/popping an HDF5 API context and restoring tag state.
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

// RAII wrapper around a ref-counted H5B_shared_t (H5UC_t*).
class ScopedShared {
 public:
  ScopedShared() = default;
  explicit ScopedShared(H5UC_t* rc) : rc_(rc) {}
  ~ScopedShared() { reset(); }

  ScopedShared(const ScopedShared&) = delete;
  ScopedShared& operator=(const ScopedShared&) = delete;
  ScopedShared(ScopedShared&& other) noexcept
      : rc_(std::exchange(other.rc_, nullptr)) {}
  ScopedShared& operator=(ScopedShared&& other) noexcept {
    if (this != &other) {
      reset();
      rc_ = std::exchange(other.rc_, nullptr);
    }
    return *this;
  }

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

struct CacheKey {
  int64_t value = 0;
};

struct CacheTreeContext {
  H5UC_t* rc_shared = nullptr;
  int64_t key = 0;
  haddr_t next_leaf_addr = 0x2000;
  int decode_calls = 0;
  int fail_decode_on_call = 0;
  int encode_calls = 0;
  int fail_encode_on_call = 0;
};

H5UC_t* TestGetShared(const H5F_t* /*f*/, const void* udata) {
  const auto* ctx = static_cast<const CacheTreeContext*>(udata);
  return ctx != nullptr ? ctx->rc_shared : nullptr;
}

herr_t TestNewNode(H5F_t* /*f*/, H5B_ins_t op, void* lt_key, void* udata,
                   void* rt_key, haddr_t* addr_p) {
  auto* ctx = static_cast<CacheTreeContext*>(udata);
  auto* lt = static_cast<CacheKey*>(lt_key);
  auto* rt = static_cast<CacheKey*>(rt_key);
  lt->value = ctx->key;
  if (op != H5B_INS_LEFT) {
    rt->value = ctx->key + 1;
  }
  *addr_p = ctx->next_leaf_addr;
  ctx->next_leaf_addr += 16;
  return 0;
}

int TestCmp2(void* lt_key, void* /*udata*/, void* rt_key) {
  const auto* lt = static_cast<const CacheKey*>(lt_key);
  const auto* rt = static_cast<const CacheKey*>(rt_key);
  if (lt->value < rt->value) return -1;
  if (lt->value > rt->value) return 1;
  return 0;
}

int TestCmp3(void* lt_key, void* udata, void* rt_key) {
  const auto* lt = static_cast<const CacheKey*>(lt_key);
  const auto* rt = static_cast<const CacheKey*>(rt_key);
  const auto* ctx = static_cast<const CacheTreeContext*>(udata);
  if (ctx->key < lt->value) return -1;
  if (ctx->key >= rt->value) return 1;
  return 0;
}

htri_t TestFound(H5F_t* /*f*/, haddr_t /*addr*/, const void* lt_key,
                 bool* found, void* udata) {
  const auto* lt = static_cast<const CacheKey*>(lt_key);
  const auto* ctx = static_cast<const CacheTreeContext*>(udata);
  *found = (lt->value == ctx->key);
  return 0;
}

H5B_ins_t TestInsert(H5F_t* /*f*/, haddr_t /*addr*/, void* /*lt_key*/,
                     bool* /*lt_key_changed*/, void* /*md_key*/,
                     void* /*udata*/, void* /*rt_key*/,
                     bool* /*rt_key_changed*/, haddr_t* /*new_node*/) {
  return H5B_INS_NOOP;
}

herr_t TestDecodeKey(const H5B_shared_t* shared, const uint8_t* raw,
                     void* key_out) {
  auto* ctx = static_cast<CacheTreeContext*>(shared->udata);
  if (ctx != nullptr) {
    ++ctx->decode_calls;
    if (ctx->fail_decode_on_call == ctx->decode_calls) {
      return -1;
    }
  }
  uint64_t encoded = 0;
  for (int i = 0; i < 8; ++i) {
    encoded |= static_cast<uint64_t>(raw[i]) << (i * 8);
  }
  static_cast<CacheKey*>(key_out)->value = static_cast<int64_t>(encoded);
  return 0;
}

herr_t TestEncodeKey(const H5B_shared_t* shared, uint8_t* raw,
                     const void* key_in) {
  auto* ctx = static_cast<CacheTreeContext*>(shared->udata);
  if (ctx != nullptr) {
    ++ctx->encode_calls;
    if (ctx->fail_encode_on_call == ctx->encode_calls) {
      return -1;
    }
  }
  const uint64_t val =
      static_cast<uint64_t>(static_cast<const CacheKey*>(key_in)->value);
  for (int i = 0; i < 8; ++i) {
    raw[i] = static_cast<uint8_t>((val >> (i * 8)) & 0xffU);
  }
  return 0;
}

H5B_class_t MakeTestBtreeClass() {
  H5B_class_t cls{};
  cls.id = H5B_CHUNK_ID;
  cls.sizeof_nkey = sizeof(CacheKey);
  cls.get_shared = TestGetShared;
  cls.new_node = TestNewNode;
  cls.cmp2 = TestCmp2;
  cls.cmp3 = TestCmp3;
  cls.found = TestFound;
  cls.insert = TestInsert;
  cls.follow_min = false;
  cls.follow_max = false;
  cls.critical_key = H5B_LEFT;
  cls.remove = nullptr;
  cls.decode = TestDecodeKey;
  cls.encode = TestEncodeKey;
  cls.debug_key = nullptr;
  return cls;
}

ScopedFile CreateInMemoryFileWithK(unsigned ik) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedFile();
  if (H5Pset_istore_k(fcpl.get(), ik) < 0) return ScopedFile();
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  return ScopedFile(
      H5Fcreate("h5bcache_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void EncodeAddr(const H5F_t* f, uint8_t** p, haddr_t addr) {
  const size_t addr_size = H5F_sizeof_addr(f);
  for (size_t i = 0; i < addr_size; ++i) {
    (*p)[i] = static_cast<uint8_t>((addr >> (i * 8)) & 0xffU);
  }
  *p += addr_size;
}

TEST(H5BcacheTest, ClassDescriptorHasExpectedCallbacks) {
  EXPECT_EQ(H5AC_BT[0].id, H5AC_BT_ID);
  EXPECT_STREQ(H5AC_BT[0].name, "v1 B-tree");
  EXPECT_EQ(H5AC_BT[0].mem_type, H5FD_MEM_BTREE);
  EXPECT_EQ(H5AC_BT[0].flags, H5AC__CLASS_NO_FLAGS_SET);
  EXPECT_NE(H5AC_BT[0].get_initial_load_size, nullptr);
  EXPECT_EQ(H5AC_BT[0].get_final_load_size, nullptr);
  EXPECT_EQ(H5AC_BT[0].verify_chksum, nullptr);
  EXPECT_NE(H5AC_BT[0].deserialize, nullptr);
  EXPECT_NE(H5AC_BT[0].image_len, nullptr);
  EXPECT_EQ(H5AC_BT[0].pre_serialize, nullptr);
  EXPECT_NE(H5AC_BT[0].serialize, nullptr);
  EXPECT_EQ(H5AC_BT[0].notify, nullptr);
  EXPECT_NE(H5AC_BT[0].free_icr, nullptr);
  EXPECT_EQ(H5AC_BT[0].fsf_size, nullptr);
}

TEST(H5BcacheTest, FlushAndReopenViaFileImageExercisesCacheCallbacks) {
  H5B_class_t cls = MakeTestBtreeClass();
  CacheTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateInMemoryFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(CacheKey));
  ASSERT_NE(raw_shared, nullptr);
  raw_shared->udata = &ctx;
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  ASSERT_NE(shared.get(), nullptr);
  ctx.rc_shared = shared.get();

  haddr_t root_addr = HADDR_UNDEF;
  haddr_t empty_addr = HADDR_UNDEF;
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // 1. Create a non-empty multi-node B-tree and an empty B-tree.
    ASSERT_GE(H5B_create(f, &cls, &ctx, &root_addr), 0);
    ASSERT_GE(H5B_create(f, &cls, &ctx, &empty_addr), 0);
    for (int64_t k = 10; k <= 60; k += 10) {
      ctx.key = k;
      ASSERT_GE(H5B_insert(f, &cls, root_addr, &ctx), 0);
    }
  }

  // 2. Flush to trigger H5B__cache_serialize and copy out the file image.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);

  // Close the writer before re-opening from the serialized image.
  file.reset();

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopened_image.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = GetH5File(reopened.get());
  ASSERT_NE(rf, nullptr);

  // 3. Re-read from cold cache to trigger get_initial_load_size & deserialize.
  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    bool found = false;
    ctx.key = 40;
    ASSERT_GE(H5B_find(rf, &cls, root_addr, &found, &ctx), 0);
    EXPECT_TRUE(found);

    found = true;
    ctx.key = 999;
    ASSERT_GE(H5B_find(rf, &cls, empty_addr, &found, &ctx), 0);
    EXPECT_FALSE(found);
  }

  // Closing the reopened file invokes H5B__cache_free_icr on cached nodes.
  reopened.reset();
  shared.reset();
}

TEST(H5BcacheTest, DirectSerializeDeserializeAndFreeIcrRoundTrip) {
  H5B_class_t cls = MakeTestBtreeClass();
  CacheTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateInMemoryFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(CacheKey));
  ASSERT_NE(raw_shared, nullptr);
  raw_shared->udata = &ctx;
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  ASSERT_NE(shared.get(), nullptr);
  ctx.rc_shared = shared.get();

  H5B_cache_ud_t udata{};
  udata.f = f;
  udata.type = &cls;
  udata.exp_level = H5B_UNKNOWN_NODELEVEL;
  udata.rc_shared = shared.get();

  size_t load_len = 0;
  ASSERT_GE(H5AC_BT[0].get_initial_load_size(&udata, &load_len), 0);
  EXPECT_EQ(load_len, raw_shared->sizeof_rnode);

  // Build an in-memory H5B_t node directly to test
  // serialize/image_len/free_icr.
  H5B_t* node = static_cast<H5B_t*>(H5FL_CALLOC(H5B_t));
  ASSERT_NE(node, nullptr);
  node->rc_shared = shared.get();
  H5UC_INC(node->rc_shared);
  node->native = H5FL_BLK_MALLOC(native_block, raw_shared->sizeof_keys);
  node->child = H5FL_SEQ_MALLOC(haddr_t, raw_shared->two_k);
  ASSERT_NE(node->native, nullptr);
  ASSERT_NE(node->child, nullptr);
  std::memset(node->native, 0, raw_shared->sizeof_keys);

  node->level = 1;
  node->nchildren = 2;
  node->left = 0x1100;
  node->right = 0x2200;
  node->child[0] = 0x3300;
  node->child[1] = 0x4400;
  std::vector<int64_t> expected_keys = {100, 200, 300};
  for (size_t i = 0; i < expected_keys.size(); ++i) {
    CacheKey k{expected_keys[i]};
    std::memcpy(H5B_NKEY(node, raw_shared, i), &k, sizeof(k));
  }

  size_t img_len = 0;
  ASSERT_GE(H5AC_BT[0].image_len(node, &img_len), 0);
  EXPECT_EQ(img_len, raw_shared->sizeof_rnode);

  // Serialize into a padded buffer pre-filled with 0xAA to verify zero-padding.
  std::vector<uint8_t> buf(img_len + 16, 0xAA);
  ASSERT_GE(H5AC_BT[0].serialize(f, buf.data(), buf.size(), node), 0);
  EXPECT_THAT(std::vector<uint8_t>(buf.begin() + img_len, buf.end()),
              Each(Eq(0)));
  EXPECT_EQ(std::memcmp(buf.data(), H5B_MAGIC, H5_SIZEOF_MAGIC), 0);
  EXPECT_EQ(buf[4], static_cast<uint8_t>(H5B_CHUNK_ID));
  EXPECT_EQ(buf[5], 1);

  ASSERT_GE(H5AC_BT[0].free_icr(node), 0);

  // Deserialize with exact expected level and verify all fields.
  udata.exp_level = 1;
  bool dirty = false;
  auto* decoded = static_cast<H5B_t*>(
      H5AC_BT[0].deserialize(buf.data(), img_len, &udata, &dirty));
  ASSERT_NE(decoded, nullptr);
  absl::Cleanup free_decoded = [decoded] { H5AC_BT[0].free_icr(decoded); };

  EXPECT_FALSE(dirty);
  EXPECT_EQ(decoded->level, 1U);
  EXPECT_EQ(decoded->nchildren, 2U);
  EXPECT_EQ(decoded->left, 0x1100U);
  EXPECT_EQ(decoded->right, 0x2200U);
  EXPECT_EQ(decoded->child[0], 0x3300U);
  EXPECT_EQ(decoded->child[1], 0x4400U);
  for (size_t i = 0; i < expected_keys.size(); ++i) {
    CacheKey k{};
    std::memcpy(&k, H5B_NKEY(decoded, raw_shared, i), sizeof(k));
    EXPECT_EQ(k.value, expected_keys[i]);
  }
}

TEST(H5BcacheTest, DeserializeRejectsTruncatedAndCorruptedImages) {
  H5B_class_t cls = MakeTestBtreeClass();
  CacheTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateInMemoryFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(CacheKey));
  ASSERT_NE(raw_shared, nullptr);
  raw_shared->udata = &ctx;
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  ASSERT_NE(shared.get(), nullptr);
  ctx.rc_shared = shared.get();

  H5B_cache_ud_t udata{};
  udata.f = f;
  udata.type = &cls;
  udata.exp_level = H5B_UNKNOWN_NODELEVEL;
  udata.rc_shared = shared.get();

  const size_t addr_size = H5F_sizeof_addr(f);
  const size_t full_len = raw_shared->sizeof_rnode;
  std::vector<uint8_t> valid_buf(full_len, 0);
  {
    uint8_t* p = valid_buf.data();
    std::memcpy(p, H5B_MAGIC, H5_SIZEOF_MAGIC);
    p += H5_SIZEOF_MAGIC;
    *p++ = static_cast<uint8_t>(H5B_CHUNK_ID);
    *p++ = 0;  // level
    *p++ = 1;  // nchildren = 1 (LE uint16)
    *p++ = 0;
    EncodeAddr(f, &p, HADDR_UNDEF);  // left
    EncodeAddr(f, &p, HADDR_UNDEF);  // right
    CacheKey k0{10};
    CacheKey k1{20};
    ASSERT_GE(TestEncodeKey(raw_shared, p, &k0), 0);
    p += raw_shared->sizeof_rkey;
    EncodeAddr(f, &p, 0x5000);
    ASSERT_GE(TestEncodeKey(raw_shared, p, &k1), 0);
  }

  ScopedErrorSilence silence;
  bool dirty = false;

  // 1. Truncated magic (len < 4, >= 1 to avoid UB in image + len - 1).
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), 3, &udata, &dirty),
            nullptr);

  // 2. Wrong magic signature.
  {
    std::vector<uint8_t> bad = valid_buf;
    bad[0] = 'X';
    EXPECT_EQ(H5AC_BT[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
              nullptr);
  }

  // 3. Truncated type & level.
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), 5, &udata, &dirty),
            nullptr);

  // 4. Wrong node type byte.
  {
    std::vector<uint8_t> bad = valid_buf;
    bad[4] = static_cast<uint8_t>(H5B_SNODE_ID);
    EXPECT_EQ(H5AC_BT[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
              nullptr);
  }

  // 5. Unexpected node level mismatch.
  {
    H5B_cache_ud_t level_udata = udata;
    level_udata.exp_level = 3;
    EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), valid_buf.size(),
                                     &level_udata, &dirty),
              nullptr);
  }

  // 6. Truncated nchildren uint16.
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), 7, &udata, &dirty),
            nullptr);

  // 7. nchildren exceeds two_k.
  {
    std::vector<uint8_t> bad = valid_buf;
    const uint16_t too_many = static_cast<uint16_t>(raw_shared->two_k + 1);
    bad[6] = static_cast<uint8_t>(too_many & 0xffU);
    bad[7] = static_cast<uint8_t>((too_many >> 8) & 0xffU);
    EXPECT_EQ(H5AC_BT[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
              nullptr);
  }

  // 8. Truncated left sibling address.
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), 8 + addr_size - 1, &udata,
                                   &dirty),
            nullptr);

  // 9. Truncated right sibling address.
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), 8 + 2 * addr_size - 1,
                                   &udata, &dirty),
            nullptr);

  // 10. Truncated child key.
  EXPECT_EQ(
      H5AC_BT[0].deserialize(valid_buf.data(),
                             8 + 2 * addr_size + raw_shared->sizeof_rkey - 1,
                             &udata, &dirty),
      nullptr);

  // 11. Truncated child address.
  EXPECT_EQ(H5AC_BT[0].deserialize(
                valid_buf.data(),
                8 + 2 * addr_size + raw_shared->sizeof_rkey + addr_size - 1,
                &udata, &dirty),
            nullptr);

  // 12. Key decode callback fails on first key.
  ctx.decode_calls = 0;
  ctx.fail_decode_on_call = 1;
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), valid_buf.size(), &udata,
                                   &dirty),
            nullptr);

  // 13. Key decode callback fails on trailing final key.
  ctx.decode_calls = 0;
  ctx.fail_decode_on_call = 2;
  EXPECT_EQ(H5AC_BT[0].deserialize(valid_buf.data(), valid_buf.size(), &udata,
                                   &dirty),
            nullptr);
}

TEST(H5BcacheTest, SerializeRejectsOverflowLevelAndEncodeFailures) {
  H5B_class_t cls = MakeTestBtreeClass();
  CacheTreeContext ctx;
  ScopedShared shared;
  ScopedFile file = CreateInMemoryFileWithK(/*ik=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B_shared_t* raw_shared = H5B_shared_new(f, &cls, sizeof(CacheKey));
  ASSERT_NE(raw_shared, nullptr);
  raw_shared->udata = &ctx;
  shared.reset(H5UC_create(raw_shared, H5B_shared_free));
  ASSERT_NE(shared.get(), nullptr);
  ctx.rc_shared = shared.get();

  H5B_t* node = static_cast<H5B_t*>(H5FL_CALLOC(H5B_t));
  ASSERT_NE(node, nullptr);
  node->rc_shared = shared.get();
  H5UC_INC(node->rc_shared);
  node->native = H5FL_BLK_MALLOC(native_block, raw_shared->sizeof_keys);
  node->child = H5FL_SEQ_MALLOC(haddr_t, raw_shared->two_k);
  ASSERT_NE(node->native, nullptr);
  ASSERT_NE(node->child, nullptr);
  std::memset(node->native, 0, raw_shared->sizeof_keys);
  std::memset(node->child, 0, sizeof(haddr_t) * raw_shared->two_k);
  absl::Cleanup free_node = [node] { H5AC_BT[0].free_icr(node); };

  node->nchildren = 1;
  node->left = HADDR_UNDEF;
  node->right = HADDR_UNDEF;
  node->child[0] = 0x6000;
  std::vector<uint8_t> out(raw_shared->sizeof_rnode, 0);

  ScopedErrorSilence silence;

  // 1. Level >= 2^8 cannot fit in 1-byte disk header field.
  node->level = 1U << LEVEL_BITS;
  EXPECT_LT(H5AC_BT[0].serialize(f, out.data(), out.size(), node), 0);

  // 2. Encode failure on child key.
  node->level = 0;
  ctx.encode_calls = 0;
  ctx.fail_encode_on_call = 1;
  EXPECT_LT(H5AC_BT[0].serialize(f, out.data(), out.size(), node), 0);

  // 3. Encode failure on final key.
  ctx.encode_calls = 0;
  ctx.fail_encode_on_call = 2;
  EXPECT_LT(H5AC_BT[0].serialize(f, out.data(), out.size(), node), 0);
}

}  // namespace
}  // namespace hdf5
