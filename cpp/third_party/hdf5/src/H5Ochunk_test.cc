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
// Tests object header chunk management routines: adding continuation chunks
// (cont_chunkno == 0 and > 0), protecting/unprotecting chunk proxies, resizing
// chunks at EOF, updating cached chunk indices after condensation, deleting
// chunks (normal and SWMR write modes), and destroying chunk proxies
// (third_party/hdf5/src/H5Ochunk.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ochunk.c

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout for internal headers.
#ifndef NDEBUG
#define H5O_CHUNK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5A_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
// H5Apkg.h -> H5B2private.h declares a `bool try` parameter, which collides
// with the C++ `try` keyword.
#pragma push_macro("try")
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#undef try
#define try h5apkg_try_param
#include "third_party/hdf5/src/H5Apkg.h"
#undef try
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5O_CHUNK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_CHUNK_TEST_RESTORE_UNDEF_NDEBUG
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

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void WriteCompactByteAttr(hid_t obj_id, const std::string& name, size_t nbytes,
                          uint8_t fill) {
  const hsize_t dim = static_cast<hsize_t>(nbytes);
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(H5Acreate2(obj_id, name.c_str(), H5T_NATIVE_UCHAR,
                             space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  std::vector<uint8_t> data(nbytes, fill);
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_UCHAR, data.data()), 0);
}

TEST(H5OchunkTest, MultiChunkGrowthCondenseAndUpdateIdxThroughPublicApi) {
  ScopedFile file = CreateInMemoryFile("chunk_public.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  // Keep attributes compact so chunk 0..3+ are allocated cleanly.
  ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);

  ScopedGroup grp(H5Gcreate2(file.get(), "/multi", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Add a 200B padding attribute in chunk 0, fence chunk 0 at EOF, grow into
  // chunk 1, fence chunk 1, then delete pad0 so chunk 0 has free space large
  // enough for a 16B continuation message (when chunk 1 overflows into chunk 2)
  // while too small for a 256B attribute to migrate forward from chunk 1.
  WriteCompactByteAttr(grp.get(), "pad0", 200, 0x11);
  unsigned prev_nchunks = 1;
  int blocker_id = 0;
  auto fence_chunk = [&] {
    ScopedGroup blocker(H5Gcreate2(
        file.get(), absl::StrCat("/blocker_", blocker_id++).c_str(),
        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blocker.valid());
  };
  fence_chunk();

  bool deleted_pad0 = false;
  int chunk2_extras = 0;
  for (int i = 0; i < 80; ++i) {
    std::string name = absl::StrCat("a_", i);
    WriteCompactByteAttr(grp.get(), name, 256, static_cast<uint8_t>(i));
    H5O_native_info_t cur{};
    ASSERT_GE(H5Oget_native_info(grp.get(), &cur, H5O_NATIVE_INFO_HDR), 0);
    if (cur.hdr.nchunks > prev_nchunks) {
      prev_nchunks = cur.hdr.nchunks;
      fence_chunk();
      if (!deleted_pad0 && prev_nchunks >= 2U) {
        ASSERT_GE(H5Adelete(grp.get(), "pad0"), 0);
        deleted_pad0 = true;
      }
    }
    if (prev_nchunks >= 3U) {
      ++chunk2_extras;
      if (chunk2_extras >= 4) break;
    }
  }

  H5O_native_info_t info{};
  ASSERT_GE(H5Oget_native_info(grp.get(), &info, H5O_NATIVE_INFO_HDR), 0);
  ASSERT_GE(info.hdr.nchunks, 3U);

  // Pin / inspect chunk layout directly to identify exact attributes in chunk 1
  // and verify deleting all messages in chunk 1 condenses chunk 1 out while
  // chunk 2 remains cached (triggering H5O__chunk_delete + H5O__chunk_update_idx).
  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  std::vector<std::string> chunk1_attr_names;
  {
    ScopedApiContext api_ctx(oloc->addr);
    ASSERT_TRUE(api_ctx.valid());
    H5O_t* oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    absl::Cleanup unprotect_oh = [&] {
      H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET);
    };
    ASSERT_GE(oh->nchunks, 3U);

    // Ensure chunk 2 is resident in cache so update_idx sees H5AC_ES__IN_CACHE.
    H5O_chunk_proxy_t* chk2 = H5O__chunk_protect(f, oh, 2);
    ASSERT_NE(chk2, nullptr);
    ASSERT_GE(H5O__chunk_unprotect(f, chk2, /*dirtied=*/false), 0);

    // Check which messages live in chunk 1.
    for (unsigned m = 0; m < oh->nmesgs; ++m) {
      if (oh->mesg[m].chunkno == 1 && oh->mesg[m].type == H5O_MSG_ATTR) {
        const H5A_t* attr_ptr = static_cast<const H5A_t*>(oh->mesg[m].native);
        H5A_t* decoded = nullptr;
        if (attr_ptr == nullptr) {
          decoded = static_cast<H5A_t*>(H5O_msg_decode(
              f, oh, H5O_ATTR_ID, oh->mesg[m].raw_size, oh->mesg[m].raw));
          attr_ptr = decoded;
        }
        if (attr_ptr != nullptr && attr_ptr->shared->name != nullptr) {
          chunk1_attr_names.emplace_back(attr_ptr->shared->name);
        }
        if (decoded != nullptr) {
          H5O_msg_free(H5O_ATTR_ID, decoded);
        }
      }
    }
  }
  ASSERT_FALSE(chunk1_attr_names.empty());

  const unsigned chunks_before = info.hdr.nchunks;
  for (const std::string& name : chunk1_attr_names) {
    ASSERT_GE(H5Adelete(grp.get(), name.c_str()), 0);
  }
  ASSERT_GE(H5Oget_native_info(grp.get(), &info, H5O_NATIVE_INFO_HDR), 0);
  EXPECT_LT(info.hdr.nchunks, chunks_before);
}

TEST(H5OchunkTest, SwmrWriteDeletesChunkWithoutFreeingFileSpace) {
  TempFile temp_file("chunk_swmr.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
            0);

  ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);

  ScopedGroup grp(H5Gcreate2(file.get(), "/swmr_grp", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedGroup blocker(H5Gcreate2(file.get(), "/swmr_blocker", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(blocker.valid());

  // Add enough attributes so the object header spans 2+ chunks, flush clean
  // before entering SWMR write mode, then reopen /swmr_grp cleanly.
  for (int i = 0; i < 16; ++i) {
    WriteCompactByteAttr(grp.get(), absl::StrCat("s_", i), 128,
                         static_cast<uint8_t>(i));
  }
  blocker.reset();
  grp.reset();

  ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);
  grp.reset(H5Gopen2(file.get(), "/swmr_grp", H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Calling H5O__chunk_delete on an open SWMR-write oh takes the !oh->swmr_write
  // false branch cleanly without calling H5AC__FREE_FILE_SPACE_FLAG. Because
  // oh stays clean, closing the file frees oh->chunk[*].image cleanly.
  ScopedApiContext api_ctx(oloc->addr);
  ASSERT_TRUE(api_ctx.valid());
  H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
  ASSERT_NE(oh, nullptr);
  absl::Cleanup unprotect_oh = [&] {
    H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET);
  };
  ASSERT_GE(oh->nchunks, 2U);
  EXPECT_TRUE(oh->swmr_write);

  const unsigned last_idx = static_cast<unsigned>(oh->nchunks - 1);
  EXPECT_GE(H5O__chunk_delete(f, oh, last_idx), 0);
  std::move(unprotect_oh).Invoke();
  grp.reset();
}

TEST(H5OchunkTest, DirectChunkProtectUnprotectResizeUpdateAndErrors) {
  ScopedFile file = CreateInMemoryFile("chunk_direct.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);

  ScopedGroup grp(H5Gcreate2(file.get(), "/direct", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  unsigned prev_nchunks = 1;
  int blocker_id = 0;
  auto fence_chunk = [&] {
    ScopedGroup blocker(H5Gcreate2(
        file.get(), absl::StrCat("/dir_blocker_", blocker_id++).c_str(),
        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blocker.valid());
  };
  fence_chunk();
  for (int i = 0; i < 60 && prev_nchunks < 3U; ++i) {
    WriteCompactByteAttr(grp.get(), absl::StrCat("d_", i), 256, 0x33);
    H5O_native_info_t cur{};
    ASSERT_GE(H5Oget_native_info(grp.get(), &cur, H5O_NATIVE_INFO_HDR), 0);
    if (cur.hdr.nchunks > prev_nchunks) {
      prev_nchunks = cur.hdr.nchunks;
      fence_chunk();
    }
  }

  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext api_ctx(oloc->addr);
  ASSERT_TRUE(api_ctx.valid());

  H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
  ASSERT_NE(oh, nullptr);
  absl::Cleanup unprotect_oh = [&] {
    H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET);
  };
  ASSERT_GE(oh->nchunks, 3U);

  // Protect, resize, and unprotect chunk 0 (dirtied=true and dirtied=false),
  // plus error paths on resize (new_size == 0) and unprotect (dirtied on
  // unpinned/unprotected oh).
  {
    H5O_chunk_proxy_t* chk0_dirty = H5O__chunk_protect(f, oh, 0);
    ASSERT_NE(chk0_dirty, nullptr);
    const size_t saved_size0 = oh->chunk[0].size;
    oh->chunk[0].size = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__chunk_resize(oh, chk0_dirty), 0);
    }
    oh->chunk[0].size = saved_size0;
    EXPECT_GE(H5O__chunk_resize(oh, chk0_dirty), 0);
    EXPECT_GE(H5O__chunk_unprotect(f, chk0_dirty, /*dirtied=*/true), 0);

    H5O_chunk_proxy_t* chk0_clean = H5O__chunk_protect(f, oh, 0);
    ASSERT_NE(chk0_clean, nullptr);
    const bool saved_prot = oh->cache_info.is_protected;
    const bool saved_pin = oh->cache_info.is_pinned;
    oh->cache_info.is_protected = false;
    oh->cache_info.is_pinned = false;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__chunk_unprotect(f, chk0_clean, /*dirtied=*/true), 0);
    }
    oh->cache_info.is_protected = saved_prot;
    oh->cache_info.is_pinned = saved_pin;
    EXPECT_GE(H5O__chunk_unprotect(f, chk0_clean, /*dirtied=*/false), 0);
  }

  // Protect, resize, unprotect, and update_idx on continuation chunk > 0,
  // plus error paths for resize (new_size == 0) and unprotect (size mismatch).
  {
    H5O_chunk_proxy_t* chk1 = H5O__chunk_protect(f, oh, 1);
    ASSERT_NE(chk1, nullptr);
    const size_t saved_size1 = oh->chunk[1].size;
    oh->chunk[1].size = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__chunk_resize(oh, chk1), 0);
    }
    oh->chunk[1].size = saved_size1 + 8;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__chunk_unprotect(f, chk1, /*dirtied=*/true), 0);
    }
    oh->chunk[1].size = saved_size1;
    EXPECT_GE(H5O__chunk_resize(oh, chk1), 0);
    EXPECT_GE(H5O__chunk_unprotect(f, chk1, /*dirtied=*/true), 0);
    EXPECT_GE(H5O__chunk_update_idx(f, oh, 1), 0);
  }

  // Exercise cache-failure error branches on chunk_add / protect / update_idx /
  // delete by using an invalid chunk address (restored immediately afterward).
  {
    ScopedErrorSilence silence;
    const haddr_t saved_addr1 = oh->chunk[1].addr;
    const haddr_t saved_addr2 = oh->chunk[2].addr;

    // Duplicate cache address in H5O__chunk_add triggers H5AC_insert_entry fail
    // -> cleanup calls H5O__chunk_dest(chk_proxy). With cont_chunkno == 1 it
    // also unprotects cont_chk_proxy on failure!
    const haddr_t dup_addr = oh->chunk[0].addr;
    oh->chunk[1].addr = dup_addr;
    EXPECT_LT(H5O__chunk_add(f, oh, 1, /*cont_chunkno=*/0), 0);
    oh->chunk[1].addr = saved_addr1;

    oh->chunk[2].addr = dup_addr;
    EXPECT_LT(H5O__chunk_add(f, oh, 2, /*cont_chunkno=*/1), 0);
    oh->chunk[2].addr = saved_addr2;

    // Invalid address on H5O__chunk_protect, H5O__chunk_update_idx, and
    // H5O__chunk_delete (note chunk[1] stays unmodified in cache).
    oh->chunk[1].addr = HADDR_UNDEF;
    EXPECT_EQ(H5O__chunk_protect(f, oh, 1), nullptr);
    EXPECT_LT(H5O__chunk_update_idx(f, oh, 1), 0);
    EXPECT_LT(H5O__chunk_delete(f, oh, 1), 0);

    // Invalid cont_chunkno failure in H5O__chunk_add (cont_chunkno=1 with
    // unresolvable chunk[1].addr).
    EXPECT_LT(H5O__chunk_add(f, oh, 2, /*cont_chunkno=*/1), 0);

    oh->chunk[1].addr = saved_addr1;
  }

  // Destroy an orphan chunk proxy directly via H5O__chunk_dest.
  {
    H5O_chunk_proxy_t* orphan = H5O__chunk_protect(f, oh, 0);
    ASSERT_NE(orphan, nullptr);
    EXPECT_GE(H5O__chunk_dest(orphan), 0);
  }
}

}  // namespace
}  // namespace hdf5
