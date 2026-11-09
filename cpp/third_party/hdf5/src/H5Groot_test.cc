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
// Tests root group creation, opening across superblock versions, symbol table
// caching/validation, mounting, and teardown (third_party/hdf5/src/H5Groot.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Groot.c

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5GROOT_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5GROOT_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5GROOT_TEST_HAS_ASAN_OR_LSAN) &&                       \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5GROOT_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5GROOT_TEST_HAS_OTHER_SAN) &&                     \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5GROOT_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5C_cache_entry_t
// and H5O_t. Define NDEBUG around internal headers to match struct layouts.
#ifndef NDEBUG
#define H5G_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5G_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5G_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5G_TEST_RESTORE_UNDEF_NDEBUG
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

std::vector<uint8_t> ExtractFileImage(hid_t file_id) {
  ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  ssize_t got = H5Fget_file_image(file_id, image.data(), image.size());
  if (got != size) {
    return {};
  }
  return image;
}

ScopedFile OpenFileImage(const std::vector<uint8_t>& image, unsigned flags,
                         const char* name = "img.h5") {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_file_image(fapl, const_cast<uint8_t*>(image.data()),
                        image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen(name, flags, fapl));
}

TEST(H5GrootTest, CreateRootWithStabAndModernFormats) {
  // v0 superblock with cached stab on root group creation.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
        0);
    ScopedFile f_v0(H5Fcreate("v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f_v0.valid());

    auto* f = static_cast<H5F_t*>(H5VL_object_verify(f_v0, H5I_FILE));
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f->shared->sblock->root_ent, nullptr);
    EXPECT_EQ(f->shared->sblock->root_ent->type, H5G_CACHED_STAB);

    // Calling H5G_mkroot again returns immediately via root_grp early check.
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_GE(H5G_mkroot(f, /*create_root=*/false), 0);
  }

  // v0 superblock where root uses new group format (creation order tracked) ->
  // H5G_NOTHING_CACHED during create_root=true.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_link_creation_order(fcpl, H5P_CRT_ORDER_TRACKED), 0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
        0);
    ScopedFile f_v0_new(
        H5Fcreate("v0_new_root.h5", H5F_ACC_TRUNC, fcpl, fapl));
    ASSERT_TRUE(f_v0_new.valid());
    auto* f = static_cast<H5F_t*>(H5VL_object_verify(f_v0_new, H5I_FILE));
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f->shared->sblock->root_ent, nullptr);
    EXPECT_EQ(f->shared->sblock->root_ent->type, H5G_NOTHING_CACHED);
  }

  // v2 superblock with superblock extension (SOHM) -> hits super_vers >= 2 and
  // asserts ext_addr != HADDR_UNDEF with nopen_objs == 2.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl, 1), 0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
    ScopedFile f_v2(H5Fcreate("v2_sohm.h5", H5F_ACC_TRUNC, fcpl, fapl));
    ASSERT_TRUE(f_v2.valid());
    auto* f = static_cast<H5F_t*>(H5VL_object_verify(f_v2, H5I_FILE));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->shared->sblock->root_ent, nullptr);
    EXPECT_TRUE(H5_addr_defined(f->shared->sblock->ext_addr));
  }
}

TEST(H5GrootTest, ReopenV0SuperblockBranchesAndErrorCleanup) {
  // Produce a v0 file image containing an old-style root group with a child.
  std::vector<uint8_t> stab_image;
  H5O_stab_t root_stab{};
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
        0);
    ScopedFile f(H5Fcreate("base_v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f.valid());
    auto* f_int = static_cast<H5F_t*>(H5VL_object_verify(f, H5I_FILE));
    ASSERT_NE(f_int, nullptr);
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      ASSERT_NE(H5O_msg_read(H5G_oloc(f_int->shared->root_grp), H5O_STAB_ID,
                             &root_stab),
                nullptr);
    }
    ScopedGroup child(
        H5Gcreate2(f, "/child", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(child.valid());
    child.reset();
    ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
    stab_image = ExtractFileImage(f);
  }
  ASSERT_GT(stab_image.size(), 96u);
  // Sanity-check v0 superblock header fields before patching bytes below:
  // offset 8: super_vers == 0; offset 13: sizeof_addr == 8; offset 14: 8.
  ASSERT_EQ(stab_image[8], 0);
  ASSERT_EQ(stab_image[13], 8);
  ASSERT_EQ(stab_image[14], 8);

  // 1. Reopen unchanged RDONLY and RDWR (runs H5G__stab_valid on RDWR).
  {
    ScopedFile f_rd = OpenFileImage(stab_image, H5F_ACC_RDONLY, "rdonly.h5");
    ASSERT_TRUE(f_rd.valid());
    H5G_info_t info{};
    ASSERT_GE(H5Gget_info_by_name(f_rd, "/", &info, H5P_DEFAULT), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  }
  {
    ScopedFile f_rw = OpenFileImage(stab_image, H5F_ACC_RDWR, "rdwr.h5");
    ASSERT_TRUE(f_rw.valid());
    H5G_info_t info{};
    ASSERT_GE(H5Gget_info_by_name(f_rw, "/", &info, H5P_DEFAULT), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  }

  // 2. Patch root_ent->type (v0 offset 72..75) from H5G_CACHED_STAB (1) to
  // H5G_NOTHING_CACHED (0). Opening RDWR exercises H5G_mkroot's caching path
  // (reading H5O_STAB_ID from root and writing cached stab + marking dirty).
  {
    std::vector<uint8_t> uncached_image = stab_image;
    ASSERT_EQ(uncached_image[72], 1);
    uncached_image[72] = 0;
    ScopedFile f_cache =
        OpenFileImage(uncached_image, H5F_ACC_RDWR, "cache_stab.h5");
    ASSERT_TRUE(f_cache.valid());
    auto* f = static_cast<H5F_t*>(H5VL_object_verify(f_cache, H5I_FILE));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->shared->sblock->root_ent->type, H5G_CACHED_STAB);
  }

  // 3. Reopen v0 image where root uses new format (UTF-8 link converted root to
  // compact) while root_ent->type still claims H5G_CACHED_STAB -> exercises
  // H5G_mkroot clearing root_ent->type to H5G_NOTHING_CACHED.
  {
    std::vector<uint8_t> utf8_image;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
          0);
      ScopedFile f(H5Fcreate("utf8_v0.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
      ASSERT_TRUE(f.valid());
      ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
      ASSERT_TRUE(lcpl.valid());
      ASSERT_GE(H5Pset_char_encoding(lcpl, H5T_CSET_UTF8), 0);
      ScopedGroup child(
          H5Gcreate2(f, "/utf8_sub", lcpl, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(child.valid());
      child.reset();
      ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
      utf8_image = ExtractFileImage(f);
    }
    ASSERT_GT(utf8_image.size(), 96u);
    utf8_image[72] = 1;  // Force H5G_CACHED_STAB even though root has no STAB.
    ScopedFile f_reopen =
        OpenFileImage(utf8_image, H5F_ACC_RDONLY, "utf8_reopen.h5");
    ASSERT_TRUE(f_reopen.valid());
    auto* f = static_cast<H5F_t*>(H5VL_object_verify(f_reopen, H5I_FILE));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->shared->sblock->root_ent->type, H5G_NOTHING_CACHED);
  }

  // 4. Patch root_ent->header (bytes 64..71) to point at offset 0 (superblock
  // magic, invalid as an object header). Opening fails H5O_open/H5O_msg_exists
  // and executes H5G_mkroot's error cleanup cleanly without leaks.
  {
    std::vector<uint8_t> bad_hdr_image = stab_image;
    std::memset(&bad_hdr_image[64], 0, 8);
    ScopedErrorSilence silence;
    ScopedFile f_err =
        OpenFileImage(bad_hdr_image, H5F_ACC_RDONLY, "bad_hdr.h5");
    EXPECT_FALSE(f_err.valid());
  }

  // 5. Corrupt root B-tree signature ("TREE" -> "XXXX") while root_ent caches
  // STAB -> fails H5G__stab_valid on RDWR reopen.
  {
    ASSERT_LE(static_cast<size_t>(root_stab.btree_addr) + 4u,
              stab_image.size());
    std::vector<uint8_t> bad_btree_img = stab_image;
    ASSERT_EQ(std::memcmp(&bad_btree_img[root_stab.btree_addr], "TREE", 4), 0);
    std::memcpy(&bad_btree_img[root_stab.btree_addr], "XXXX", 4);
    ScopedErrorSilence silence;
    ScopedFile f_err =
        OpenFileImage(bad_btree_img, H5F_ACC_RDWR, "bad_root_bt.h5");
    EXPECT_FALSE(f_err.valid());
  }

  // 6. Uncached root_ent->type == 0 with invalid root_addr (offset 0) -> RDWR
  // reopen reaches H5O_msg_exists on uncached check and fails cleanly.
  {
    std::vector<uint8_t> bad_uncached_img = stab_image;
    bad_uncached_img[72] = 0;
    std::memset(&bad_uncached_img[64], 0, 8);
    ScopedErrorSilence silence;
    ScopedFile f_err =
        OpenFileImage(bad_uncached_img, H5F_ACC_RDWR, "bad_uncached.h5");
    EXPECT_FALSE(f_err.valid());
  }

  // 7. Truncate root's v1 H5O_STAB_ID payload size (16 -> 8 bytes, converting
  // the remaining 8 bytes to a H5O_NULL_ID message) and increment the v1
  // object header prefix nmesgs (root_addr + 2..3, 16-bit LE) so deserializing
  // the object header passes v1_pfx_nmesgs >= oh->nmesgs. With root_ent->type
  // == 0, H5O_msg_exists succeeds and H5O_msg_read fails H5O__stab_decode.
  {
    uint64_t root_addr_le = 0;
    std::memcpy(&root_addr_le, &stab_image[64], sizeof(root_addr_le));
    ASSERT_LE(static_cast<size_t>(root_addr_le) + 16u, stab_image.size());
    ASSERT_EQ(stab_image[static_cast<size_t>(root_addr_le)], 1);

    uint64_t btree_le = static_cast<uint64_t>(root_stab.btree_addr);
    uint64_t heap_le = static_cast<uint64_t>(root_stab.heap_addr);
    std::vector<uint8_t> truncated_stab_img = stab_image;
    truncated_stab_img[72] = 0;

    const size_t nmesgs_off = static_cast<size_t>(root_addr_le) + 2u;
    uint16_t nmesgs =
        static_cast<uint16_t>(truncated_stab_img[nmesgs_off]) |
        static_cast<uint16_t>(
            static_cast<uint16_t>(truncated_stab_img[nmesgs_off + 1u]) << 8);
    ++nmesgs;
    truncated_stab_img[nmesgs_off] = static_cast<uint8_t>(nmesgs & 0xffu);
    truncated_stab_img[nmesgs_off + 1u] =
        static_cast<uint8_t>((nmesgs >> 8) & 0xffu);

    bool patched = false;
    for (size_t off = static_cast<size_t>(root_addr_le);
         off + 24u <= truncated_stab_img.size(); ++off) {
      if (truncated_stab_img[off] == 0x11 && truncated_stab_img[off + 1] == 0 &&
          truncated_stab_img[off + 2] == 0x10 &&
          truncated_stab_img[off + 3] == 0 &&
          std::memcmp(&truncated_stab_img[off + 8], &btree_le, 8) == 0 &&
          std::memcmp(&truncated_stab_img[off + 16], &heap_le, 8) == 0) {
        truncated_stab_img[off + 2] = 0x08;
        std::memset(&truncated_stab_img[off + 16], 0, 8);
        patched = true;
        break;
      }
    }
    ASSERT_TRUE(patched);
    ScopedErrorSilence silence;
    ScopedFile f_err =
        OpenFileImage(truncated_stab_img, H5F_ACC_RDWR, "trunc_stab.h5");
    EXPECT_FALSE(f_err.valid());
  }
}

TEST(H5GrootTest, MkrootAndRootLocInternalErrorPaths) {
  // 1. H5G_mkroot(create_root=true) on a RDONLY v2 file fails H5G__obj_create.
  {
    std::vector<uint8_t> v2_img;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
      ScopedFile f(H5Fcreate("v2_ro_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
      ASSERT_TRUE(f.valid());
      ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
      v2_img = ExtractFileImage(f);
    }
    ASSERT_FALSE(v2_img.empty());
    ScopedFile ro_file = OpenFileImage(v2_img, H5F_ACC_RDONLY, "v2_ro.h5");
    ASSERT_TRUE(ro_file.valid());
    auto* ro_f = static_cast<H5F_t*>(H5VL_object_verify(ro_file, H5I_FILE));
    ASSERT_NE(ro_f, nullptr);
    ASSERT_EQ(ro_f->shared->sblock->root_ent, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5G_t* saved_root = ro_f->shared->root_grp;
    ASSERT_GE(H5G_node_close(ro_f), 0);
    ro_f->shared->grp_btree_shared = nullptr;
    ro_f->shared->root_grp = nullptr;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_mkroot(ro_f, /*create_root=*/true), 0);
    }
    EXPECT_EQ(ro_f->shared->root_grp, nullptr);
    ro_f->shared->root_grp = saved_root;
  }

  // 2. H5G_mkroot(create_root=false) fails H5AC_mark_entry_dirty(sblock) when
  // superblock is temporarily unpinned & root_ent is uncached.
  {
    std::vector<uint8_t> v0_img;
    {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
          0);
      ScopedFile f(H5Fcreate("v0_dirty_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl));
      ASSERT_TRUE(f.valid());
      ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
      v0_img = ExtractFileImage(f);
    }
    ASSERT_FALSE(v0_img.empty());
    ScopedFile rw_file = OpenFileImage(v0_img, H5F_ACC_RDWR, "v0_dirty.h5");
    ASSERT_TRUE(rw_file.valid());
    auto* f = static_cast<H5F_t*>(H5VL_object_verify(rw_file, H5I_FILE));
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f->shared->sblock->root_ent, nullptr);
    ASSERT_TRUE(f->shared->sblock->cache_info.is_pinned);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5G_t* saved_root = f->shared->root_grp;
    H5G_cache_type_t saved_type = f->shared->sblock->root_ent->type;
    bool saved_pinned = f->shared->sblock->cache_info.is_pinned;

    ASSERT_GE(H5G_node_close(f), 0);
    f->shared->grp_btree_shared = nullptr;
    f->shared->root_grp = nullptr;
    f->shared->sblock->root_ent->type = H5G_NOTHING_CACHED;
    f->shared->sblock->cache_info.is_pinned = false;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5G_mkroot(f, /*create_root=*/false), 0);
    }
    f->shared->sblock->cache_info.is_pinned = saved_pinned;
    f->shared->sblock->root_ent->type = saved_type;
    if (f->shared->root_grp != nullptr) {
      ASSERT_GE(H5G_root_free(f->shared->root_grp), 0);
    }
    f->shared->root_grp = saved_root;
  }
}

TEST(H5GrootTest, RootOfAndRootLocAcrossMountAndReopen) {
  ScopedFile parent = CreateInMemoryFile("parent.h5");
  ScopedFile child = CreateInMemoryFile("child.h5");
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child.valid());

  ScopedGroup mnt(
      H5Gcreate2(parent, "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mnt.valid());
  ASSERT_GE(H5Fmount(parent, "/mnt", child, H5P_DEFAULT), 0);
  absl::Cleanup unmount_cleanup = [&] {
    H5Funmount(parent, "/mnt");
  };

  ScopedFile reopened(H5Freopen(parent));
  ASSERT_TRUE(reopened.valid());

  auto* f_parent = static_cast<H5F_t*>(H5VL_object_verify(parent, H5I_FILE));
  auto* f_child = static_cast<H5F_t*>(H5VL_object_verify(child, H5I_FILE));
  auto* f_reopen = static_cast<H5F_t*>(H5VL_object_verify(reopened, H5I_FILE));
  ASSERT_NE(f_parent, nullptr);
  ASSERT_NE(f_child, nullptr);
  ASSERT_NE(f_reopen, nullptr);

  // H5G_rootof walks child->parent and updates oloc.file when called via
  // reopened file handle sharing f->shared.
  H5G_t* root_via_parent = H5G_rootof(f_parent);
  H5G_t* root_via_child = H5G_rootof(f_child);
  H5G_t* root_via_reopen = H5G_rootof(f_reopen);
  ASSERT_NE(root_via_parent, nullptr);
  EXPECT_EQ(root_via_child, root_via_parent);
  EXPECT_EQ(root_via_reopen, root_via_parent);
  EXPECT_EQ(root_via_reopen->oloc.file, f_reopen);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5G_loc_t loc{};
  EXPECT_GE(H5G_root_loc(f_parent, &loc), 0);
  EXPECT_EQ(loc.oloc->file, f_parent);
  EXPECT_FALSE(loc.oloc->holding_file);

  H5G_loc_t child_loc{};
  EXPECT_GE(H5G_root_loc(f_child, &child_loc), 0);
  EXPECT_NE(child_loc.oloc, nullptr);
}

#if !defined(H5GROOT_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5GROOT_TEST_HAS_OTHER_SAN)
TEST(H5GrootTest, MkrootHandlesRootGroupAllocationFailures) {
  // Sanitizer allocators abort on SIZE_MAX malloc instead of returning NULL;
  // guard this OOM branch check under non-sanitizer builds.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  ScopedFile v2_file(H5Fcreate("v2_oom.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(v2_file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(v2_file, H5I_FILE));
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(f->shared->sblock->root_ent, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5G_t* saved_root = f->shared->root_grp;
  ASSERT_NE(saved_root, nullptr);
  f->shared->root_grp = nullptr;
  absl::Cleanup restore_root = [&] {
    if (f->shared->root_grp == nullptr) {
      f->shared->root_grp = saved_root;
    }
  };

  ScopedErrorSilence silence;

  // 1. Fail H5FL_CALLOC(H5G_t) by draining free lists and setting size to max.
  ASSERT_GE(H5G_node_close(f), 0);
  f->shared->grp_btree_shared = nullptr;
  ASSERT_GE(H5FL_garbage_coll(), 0);
  {
    const size_t orig_g_size = H5FL_REG_NAME(H5G_t).size;
    absl::Cleanup restore_g_size = [&] {
      H5FL_REG_NAME(H5G_t).size = orig_g_size;
    };
    H5FL_REG_NAME(H5G_t).size = std::numeric_limits<size_t>::max();
    EXPECT_LT(H5G_mkroot(f, /*create_root=*/false), 0);
    EXPECT_EQ(f->shared->root_grp, nullptr);
  }

  // 2. Allow H5G_t allocation and fail H5FL_CALLOC(H5G_shared_t), exercising
  // H5FL_FREE cleanup of f->shared->root_grp before returning failure.
  ASSERT_GE(H5G_node_close(f), 0);
  f->shared->grp_btree_shared = nullptr;
  ASSERT_GE(H5FL_garbage_coll(), 0);
  {
    const size_t orig_shared_size = H5FL_REG_NAME(H5G_shared_t).size;
    absl::Cleanup restore_shared_size = [&] {
      H5FL_REG_NAME(H5G_shared_t).size = orig_shared_size;
    };
    H5FL_REG_NAME(H5G_shared_t).size = std::numeric_limits<size_t>::max();
    EXPECT_LT(H5G_mkroot(f, /*create_root=*/false), 0);
    EXPECT_EQ(f->shared->root_grp, nullptr);
  }
  ASSERT_GE(H5FL_garbage_coll(), 0);
}
#endif

}  // namespace
}  // namespace hdf5
