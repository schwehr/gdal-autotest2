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
// Tests symbol table node metadata cache callbacks
// (third_party/hdf5/src/H5Gcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gcache.c

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t), the first member of H5G_node_t. Match those
// struct layouts across both the prelude and internal headers.
#ifndef NDEBUG
#define H5GCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

// H5B2private.h (pulled via H5Gpkg.h) uses `try` as a C parameter name, which
// collides with the C++ keyword and Google3's `-fno-exceptions` `try` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5GCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::Eq;

constexpr uint8_t kNodeVersion = 1;

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

ScopedFile CreateOldFormatInMemoryFile(unsigned ik, unsigned lk) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid() || H5Pset_sym_k(fcpl.get(), ik, lk) < 0) {
    return ScopedFile();
  }
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid() ||
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) <
          0) {
    return ScopedFile();
  }
  return ScopedFile(
      H5Fcreate("h5gcache_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

TEST(H5GcacheTest, ClassDescriptorMatchesExpectedDefinitions) {
  EXPECT_EQ(H5AC_SNODE[0].id, H5AC_SNODE_ID);
  EXPECT_STREQ(H5AC_SNODE[0].name, "Symbol table node");
  EXPECT_EQ(H5AC_SNODE[0].mem_type, H5FD_MEM_BTREE);
  EXPECT_EQ(H5AC_SNODE[0].flags, H5AC__CLASS_NO_FLAGS_SET);
  EXPECT_NE(H5AC_SNODE[0].get_initial_load_size, nullptr);
  EXPECT_EQ(H5AC_SNODE[0].get_final_load_size, nullptr);
  EXPECT_EQ(H5AC_SNODE[0].verify_chksum, nullptr);
  EXPECT_NE(H5AC_SNODE[0].deserialize, nullptr);
  EXPECT_NE(H5AC_SNODE[0].image_len, nullptr);
  EXPECT_EQ(H5AC_SNODE[0].pre_serialize, nullptr);
  EXPECT_NE(H5AC_SNODE[0].serialize, nullptr);
  EXPECT_EQ(H5AC_SNODE[0].notify, nullptr);
  EXPECT_NE(H5AC_SNODE[0].free_icr, nullptr);
  EXPECT_EQ(H5AC_SNODE[0].fsf_size, nullptr);
}

TEST(H5GcacheTest, SymbolTableGroupFlushAndReopenExercisesCacheCallbacks) {
  ScopedFile file = CreateOldFormatInMemoryFile(/*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());

  // Populate the old-format symbol table root group with enough links (nested
  // groups + soft link) to create and split multiple SNODE leaves.
  {
    ScopedGroup sub(
        H5Gcreate2(file.get(), "sub_group", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
    for (int i = 0; i < 6; ++i) {
      ScopedGroup child(H5Gcreate2(sub.get(), absl::StrFormat("c_%02d", i).c_str(),
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(child.valid());
    }
  }
  ASSERT_GE(H5Lcreate_soft("/sub_group", file.get(), "soft_alias", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  for (int i = 0; i < 12; ++i) {
    ScopedGroup g(H5Gcreate2(file.get(), absl::StrFormat("node_%02d", i).c_str(),
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info(file.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  EXPECT_EQ(ginfo.nlinks, 14u);

  // Flush triggers SNODE serialize + image_len; file image reopen triggers
  // get_initial_load_size + deserialize + free_icr on close.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl(/*backing_store=*/false);
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("h5gcache_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(reopened.valid());

  H5G_info_t reopened_info{};
  ASSERT_GE(H5Gget_info(reopened.get(), &reopened_info), 0);
  EXPECT_EQ(reopened_info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  EXPECT_EQ(reopened_info.nlinks, 14u);

  ScopedGroup sub_reopened(H5Gopen2(reopened.get(), "sub_group", H5P_DEFAULT));
  ASSERT_TRUE(sub_reopened.valid());
  H5G_info_t sub_info{};
  ASSERT_GE(H5Gget_info(sub_reopened.get(), &sub_info), 0);
  EXPECT_EQ(sub_info.nlinks, 6u);
}

TEST(H5GcacheTest, DirectCallbacksRoundTripAndZeroPadBuffer) {
  ScopedFile file = CreateOldFormatInMemoryFile(/*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  size_t expected_len = 0;
  ASSERT_GE(H5AC_SNODE[0].get_initial_load_size(f, &expected_len), 0);
  ASSERT_EQ(expected_len, static_cast<size_t>(H5G_NODE_SIZE(f)));

  // Prepare 3 symbol table entries (fewer than 2*lk = 4 slots so trailing bytes
  // in serialize must be zeroed).
  H5G_entry_t entries[3]{};
  entries[0].type = H5G_NOTHING_CACHED;
  entries[0].name_off = 16;
  entries[0].header = 0x1000;

  entries[1].type = H5G_CACHED_STAB;
  entries[1].name_off = 32;
  entries[1].header = 0x2000;
  entries[1].cache.stab.btree_addr = 0x3000;
  entries[1].cache.stab.heap_addr = 0x4000;

  entries[2].type = H5G_CACHED_SLINK;
  entries[2].name_off = 48;
  entries[2].header = HADDR_UNDEF;
  entries[2].cache.slink.lval_offset = 64;

  std::vector<uint8_t> raw(expected_len, 0xCC);
  std::memcpy(raw.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
  raw[4] = kNodeVersion;
  raw[5] = 0;
  raw[6] = 3;  // nsyms (uint16 little-endian)
  raw[7] = 0;
  uint8_t* p = raw.data() + 8;
  ASSERT_GE(H5G__ent_encode_vec(f, &p, entries, 3), 0);
  const size_t used_bytes = static_cast<size_t>(p - raw.data());
  ASSERT_LT(used_bytes, expected_len);

  bool dirty = false;
  auto* sym = static_cast<H5G_node_t*>(
      H5AC_SNODE[0].deserialize(raw.data(), raw.size(), f, &dirty));
  ASSERT_NE(sym, nullptr);
  sym->cache_info.type = H5AC_SNODE;
  auto free_sym = absl::MakeCleanup([&]() {
    if (sym != nullptr) {
      sym->cache_info.type = H5AC_SNODE;
      EXPECT_GE(H5AC_SNODE[0].free_icr(sym), 0);
    }
  });

  EXPECT_FALSE(dirty);
  EXPECT_EQ(sym->nsyms, 3u);
  EXPECT_EQ(sym->node_size, expected_len);
  EXPECT_EQ(sym->entry[0].type, H5G_NOTHING_CACHED);
  EXPECT_EQ(sym->entry[0].header, 0x1000u);
  EXPECT_EQ(sym->entry[1].type, H5G_CACHED_STAB);
  EXPECT_EQ(sym->entry[1].cache.stab.btree_addr, 0x3000u);
  EXPECT_EQ(sym->entry[1].cache.stab.heap_addr, 0x4000u);
  EXPECT_EQ(sym->entry[2].type, H5G_CACHED_SLINK);
  EXPECT_EQ(sym->entry[2].cache.slink.lval_offset, 64u);

  size_t reported_len = 0;
  ASSERT_GE(H5AC_SNODE[0].image_len(sym, &reported_len), 0);
  EXPECT_EQ(reported_len, expected_len);

  // Serialize into a non-zero-filled buffer and verify trailing zero-padding.
  std::vector<uint8_t> out(expected_len, 0xAA);
  ASSERT_GE(H5AC_SNODE[0].serialize(f, out.data(), out.size(), sym), 0);
  EXPECT_EQ(std::memcmp(out.data(), raw.data(), used_bytes), 0);
  EXPECT_THAT(
      std::vector<uint8_t>(out.begin() + static_cast<ptrdiff_t>(used_bytes),
                           out.end()),
      Each(Eq(0)));

  // Trigger serialize failure via H5G__ent_encode_vec with H5G_CACHED_ERROR.
  {
    ScopedErrorSilence silence;
    sym->entry[0].type = H5G_CACHED_ERROR;
    EXPECT_LT(H5AC_SNODE[0].serialize(f, out.data(), out.size(), sym), 0);
    sym->entry[0].type = H5G_NOTHING_CACHED;
  }

  ASSERT_GE(H5AC_SNODE[0].free_icr(sym), 0);
  sym = nullptr;
}

TEST(H5GcacheTest, DeserializeRejectsTruncatedAndCorruptBuffers) {
  ScopedFile file = CreateOldFormatInMemoryFile(/*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  size_t node_len = 0;
  ASSERT_GE(H5AC_SNODE[0].get_initial_load_size(f, &node_len), 0);
  ASSERT_GT(node_len, 16u);

  ScopedErrorSilence silence;
  bool dirty = false;

  // 1. Truncated before magic (len < H5_SIZEOF_MAGIC = 4).
  {
    std::vector<uint8_t> buf(3, 0);
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 2. Invalid magic bytes.
  {
    std::vector<uint8_t> buf(node_len, 0);
    std::memcpy(buf.data(), "BAD!", H5_SIZEOF_MAGIC);
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 3. Truncated before version byte (len == 4).
  {
    std::vector<uint8_t> buf(4, 0);
    std::memcpy(buf.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 4. Unsupported version byte (!= 1).
  {
    std::vector<uint8_t> buf(node_len, 0);
    std::memcpy(buf.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
    buf[4] = 99;
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 5. Truncated before reserved byte (len == 5).
  {
    std::vector<uint8_t> buf(5, 0);
    std::memcpy(buf.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
    buf[4] = kNodeVersion;
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 6. Truncated before 2-byte nsyms field (len == 7).
  {
    std::vector<uint8_t> buf(7, 0);
    std::memcpy(buf.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
    buf[4] = kNodeVersion;
    buf[5] = 0;
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 7. nsyms = 1 with buffer truncated immediately after header (len == 8).
  {
    std::vector<uint8_t> buf(8, 0);
    std::memcpy(buf.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
    buf[4] = kNodeVersion;
    buf[5] = 0;
    buf[6] = 1;
    buf[7] = 0;
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }

  // 8. Full-sized buffer with invalid symbol table entry cache type.
  {
    std::vector<uint8_t> buf(node_len, 0);
    std::memcpy(buf.data(), H5G_NODE_MAGIC, H5_SIZEOF_MAGIC);
    buf[4] = kNodeVersion;
    buf[5] = 0;
    buf[6] = 1;
    buf[7] = 0;
    const size_t type_offset =
        8 + static_cast<size_t>(H5F_SIZEOF_SIZE(f)) + H5F_SIZEOF_ADDR(f);
    ASSERT_LT(type_offset, buf.size());
    buf[type_offset] = 99;  // Unknown H5G_cache_type_t
    EXPECT_EQ(H5AC_SNODE[0].deserialize(buf.data(), buf.size(), f, &dirty),
              nullptr);
  }
}

}  // namespace
}  // namespace hdf5
