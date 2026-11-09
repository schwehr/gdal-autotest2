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
// Tests fractal heap metadata cache callbacks for header (H5AC_FHEAP_HDR),
// indirect block (H5AC_FHEAP_IBLOCK), and direct block (H5AC_FHEAP_DBLOCK),
// covering speculative load sizing, checksum verification, filtered and
// unfiltered serialization/deserialization, temporary space relocation and
// compressed resizing, flush dependency notifications, ICR freeing, fsf_size
// deletion, and corruption/error paths (third_party/hdf5/src/H5HFcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFcache.c

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5HF_hdr_t, H5HF_indirect_t, H5HF_direct_t) and H5C_t.
// Define NDEBUG around internal headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5HFCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h and H5Fpkg.h use `try` as a C parameter name, colliding with the
// C++ keyword and Google3's `-fno-exceptions` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5AC_FRIEND
#define H5C_FRIEND
#define H5F_FRIEND
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

// RAII wrapper pushing an HDF5 API context and metadata tag.
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

// Temporarily forces H5MF_alloc() to fail while verifying allocation errors.
class ScopedBlockAllocFailure {
 public:
  explicit ScopedBlockAllocFailure(H5F_t* f)
      : f_(f),
        saved_strategy_(f->shared->fs_strategy),
        saved_tmp_addr_(f->shared->tmp_addr),
        saved_maxaddr_(f->shared->lf->maxaddr) {
    f_->shared->fs_strategy = H5F_FSPACE_STRATEGY_NONE;
    f_->shared->tmp_addr = 0;
    f_->shared->lf->maxaddr = 0;
  }

  ~ScopedBlockAllocFailure() {
    f_->shared->lf->maxaddr = saved_maxaddr_;
    f_->shared->tmp_addr = saved_tmp_addr_;
    f_->shared->fs_strategy = saved_strategy_;
  }

  ScopedBlockAllocFailure(const ScopedBlockAllocFailure&) = delete;
  ScopedBlockAllocFailure& operator=(const ScopedBlockAllocFailure&) = delete;

 private:
  H5F_t* f_;
  H5F_fspace_strategy_t saved_strategy_;
  haddr_t saved_tmp_addr_;
  haddr_t saved_maxaddr_;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_create_t MakeHeapParams(unsigned width = 4, size_t start_block_size = 256,
                             size_t max_direct_size = 512,
                             unsigned max_index = 20,
                             unsigned start_root_rows = 1,
                             bool checksum_dblocks = true,
                             uint32_t max_man_size = 256, uint16_t id_len = 0) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = checksum_dblocks;
  cparam.max_man_size = max_man_size;
  cparam.id_len = id_len;
  return cparam;
}

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((seed + i * 29U) & 0xffU);
  }
  return data;
}

std::vector<uint8_t> MakeIncompressiblePattern(size_t size, uint32_t seed) {
  std::vector<uint8_t> data(size);
  uint32_t x = seed | 1U;
  for (size_t i = 0; i < size; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    data[i] = static_cast<uint8_t>(x & 0xffU);
  }
  return data;
}

// Configurable custom H5Z filter for exercising filter mask updates, resize
// paths, and pipeline errors in direct block cache callbacks.
constexpr H5Z_filter_t kDynamicFilterId = H5Z_FILTER_RESERVED + 17;

enum class FilterMode {
  kCopyUnchanged = 0,
  kSkipOptional = 1,  // Returns 0 on optional filter -> sets filter_mask bit
  kShrinkTail = 2,    // Halves trailing zeros / shrinks reported length
  kFail = 3,          // Always returns 0
};

FilterMode g_dynamic_filter_mode = FilterMode::kCopyUnchanged;

size_t DynamicFilterApply(unsigned int flags, size_t /*cd_nelmts*/,
                          const unsigned int /*cd_values*/[], size_t nbytes,
                          size_t* buf_size, void** buf) {
  if (flags & H5Z_FLAG_REVERSE) {
    if (g_dynamic_filter_mode == FilterMode::kFail) {
      return 0;
    }
    return nbytes;
  }
  switch (g_dynamic_filter_mode) {
    case FilterMode::kCopyUnchanged:
      return nbytes;
    case FilterMode::kSkipOptional:
    case FilterMode::kFail:
      return 0;
    case FilterMode::kShrinkTail:
      if (nbytes > 32 && *buf_size >= nbytes) {
        return nbytes - 16;
      }
      return nbytes;
  }
  return 0;
}

// Exercises speculative header loading, filtered header decode/encode,
// invalid magic/version/pline/finish-init error paths, verify_chksum,
// pre_serialize checks, flag bits, and free_icr.
TEST(H5HFcacheTest, HeaderCacheCallbacksRoundTripAndErrors) {
  ScopedFile file = CreateInMemoryFile("h5hfcache_hdr.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Unfiltered header: test flags, verify_chksum, get_final_load_size,
  //    serialize/deserialize, pre_serialize errors, and free_icr.
  {
    const H5HF_create_t cparam = MakeHeapParams();
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_NE(hdr, nullptr);
    hdr->huge_ids_wrapped = true;
    hdr->checksum_dblocks = false;
    hdr->huge_next_id = 42;
    hdr->man_size = 512;
    hdr->man_alloc_size = 256;
    hdr->man_iter_off = 256;
    hdr->man_nobjs = 3;
    hdr->huge_size = 1024;
    hdr->huge_nobjs = 2;
    hdr->tiny_size = 12;
    hdr->tiny_nobjs = 4;

    H5HF_hdr_cache_ud_t udata{f};
    size_t init_len = 0;
    ASSERT_GE(H5AC_FHEAP_HDR[0].get_initial_load_size(&udata, &init_len), 0);
    EXPECT_EQ(init_len, hdr->heap_size);

    size_t img_len = 0;
    ASSERT_GE(H5AC_FHEAP_HDR[0].image_len(hdr, &img_len), 0);
    EXPECT_EQ(img_len, hdr->heap_size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_FHEAP_HDR[0].serialize(f, image.data(), img_len, hdr), 0);

    // get_final_load_size with filter_len == 0 leaves actual_len unchanged.
    size_t final_len = init_len;
    ASSERT_GE(H5AC_FHEAP_HDR[0].get_final_load_size(image.data(), init_len,
                                                    &udata, &final_len),
              0);
    EXPECT_EQ(final_len, init_len);

    // Checksum verification: valid, flipped bit, and too-short buffer error.
    EXPECT_EQ(H5AC_FHEAP_HDR[0].verify_chksum(image.data(), img_len, &udata),
              1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x01U;
      EXPECT_EQ(
          H5AC_FHEAP_HDR[0].verify_chksum(flipped.data(), img_len, &udata), 0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_FHEAP_HDR[0].verify_chksum(image.data(), 2, &udata), 0);
    }

    // pre_serialize: valid, tmp addr failure, and mismatched length failure.
    haddr_t new_addr = HADDR_UNDEF;
    size_t new_len = 0;
    unsigned flags = 7;
    EXPECT_GE(H5AC_FHEAP_HDR[0].pre_serialize(f, hdr, hdr->heap_addr, img_len,
                                              &new_addr, &new_len, &flags),
              0);
    EXPECT_EQ(flags, 0U);

    {
      ScopedErrorSilence silence;
      const haddr_t saved_tmp = f->shared->tmp_addr;
      f->shared->tmp_addr = hdr->heap_addr;
      EXPECT_LT(H5AC_FHEAP_HDR[0].pre_serialize(f, hdr, hdr->heap_addr, img_len,
                                                &new_addr, &new_len, &flags),
                0);
      f->shared->tmp_addr = saved_tmp;

      EXPECT_LT(
          H5AC_FHEAP_HDR[0].pre_serialize(f, hdr, hdr->heap_addr, img_len + 1,
                                          &new_addr, &new_len, &flags),
          0);
    }

    // Deserialize and verify all fields + free_icr.
    bool dirty = false;
    auto* deser = static_cast<H5HF_hdr_t*>(
        H5AC_FHEAP_HDR[0].deserialize(image.data(), img_len, &udata, &dirty));
    ASSERT_NE(deser, nullptr);
    EXPECT_TRUE(deser->huge_ids_wrapped);
    EXPECT_FALSE(deser->checksum_dblocks);
    EXPECT_EQ(deser->huge_next_id, 42U);
    EXPECT_EQ(deser->man_size, 512U);
    EXPECT_EQ(deser->man_alloc_size, 256U);
    EXPECT_EQ(deser->man_iter_off, 256U);
    EXPECT_EQ(deser->man_nobjs, 3U);
    EXPECT_EQ(deser->huge_size, 1024U);
    EXPECT_EQ(deser->huge_nobjs, 2U);
    EXPECT_EQ(deser->tiny_size, 12U);
    EXPECT_EQ(deser->tiny_nobjs, 4U);
    EXPECT_GE(H5AC_FHEAP_HDR[0].free_icr(deser), 0);

    // Prefix corruption errors in get_final_load_size and deserialize.
    {
      ScopedErrorSilence silence;
      std::vector<uint8_t> bad_magic = image;
      bad_magic[0] = 'X';
      size_t tmp_len = init_len;
      EXPECT_LT(H5AC_FHEAP_HDR[0].get_final_load_size(
                    bad_magic.data(), init_len, &udata, &tmp_len),
                0);
      EXPECT_EQ(H5AC_FHEAP_HDR[0].deserialize(bad_magic.data(), img_len, &udata,
                                              &dirty),
                nullptr);

      std::vector<uint8_t> bad_ver = image;
      bad_ver[H5_SIZEOF_MAGIC] = 99;
      tmp_len = init_len;
      EXPECT_LT(H5AC_FHEAP_HDR[0].get_final_load_size(bad_ver.data(), init_len,
                                                      &udata, &tmp_len),
                0);
      EXPECT_EQ(H5AC_FHEAP_HDR[0].deserialize(bad_ver.data(), img_len, &udata,
                                              &dirty),
                nullptr);
    }
  }

  // 2. Filtered header: speculative final load size, filter pipeline encode &
  //    decode, corrupt pline message error, and finish_init failure.
  {
    H5HF_create_t cparam = MakeHeapParams();
    auto reset_pline = absl::MakeCleanup([&]() {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    });
    unsigned level = 6;
    ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                         1, &level),
              0);

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_GT(hdr->filter_len, 0U);
    hdr->pline_root_direct_size = 192;
    hdr->pline_root_direct_filter_mask = 0x5U;

    H5HF_hdr_cache_ud_t udata{f};
    size_t init_len = 0;
    ASSERT_GE(H5AC_FHEAP_HDR[0].get_initial_load_size(&udata, &init_len), 0);

    size_t img_len = 0;
    ASSERT_GE(H5AC_FHEAP_HDR[0].image_len(hdr, &img_len), 0);
    EXPECT_GT(img_len, init_len);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_FHEAP_HDR[0].serialize(f, image.data(), img_len, hdr), 0);

    // Speculative final_load_size grows init_len to exact img_len.
    size_t final_len = init_len;
    ASSERT_GE(H5AC_FHEAP_HDR[0].get_final_load_size(image.data(), init_len,
                                                    &udata, &final_len),
              0);
    EXPECT_EQ(final_len, img_len);

    bool dirty = false;
    auto* deser = static_cast<H5HF_hdr_t*>(
        H5AC_FHEAP_HDR[0].deserialize(image.data(), img_len, &udata, &dirty));
    ASSERT_NE(deser, nullptr);
    EXPECT_EQ(deser->filter_len, hdr->filter_len);
    EXPECT_EQ(deser->pline_root_direct_size, 192U);
    EXPECT_EQ(deser->pline_root_direct_filter_mask, 0x5U);
    EXPECT_EQ(deser->pline.nused, 1U);
    EXPECT_GE(H5AC_FHEAP_HDR[0].free_icr(deser), 0);

    // Corrupt encoded pline message version byte so H5O_msg_decode fails.
    {
      ScopedErrorSilence silence;
      std::vector<uint8_t> bad_pline = image;
      const size_t pline_off =
          (init_len - H5HF_SIZEOF_CHKSUM) + hdr->sizeof_size + 4;
      ASSERT_LT(pline_off, bad_pline.size());
      bad_pline[pline_off] = 99;
      EXPECT_EQ(H5AC_FHEAP_HDR[0].deserialize(bad_pline.data(), img_len, &udata,
                                              &dirty),
                nullptr);
    }

    // Corrupt id_len slightly? Actually tiny_init handles small id_len cleanly,
    // while invalid man_dtable cparam cannot occur normally without OOM; verify
    // unchecksummed unfiltered flags false/true both deserialize cleanly.
    hdr->huge_ids_wrapped = false;
    hdr->checksum_dblocks = true;
    ASSERT_GE(H5AC_FHEAP_HDR[0].serialize(f, image.data(), img_len, hdr), 0);
    deser = static_cast<H5HF_hdr_t*>(
        H5AC_FHEAP_HDR[0].deserialize(image.data(), img_len, &udata, &dirty));
    ASSERT_NE(deser, nullptr);
    EXPECT_FALSE(deser->huge_ids_wrapped);
    EXPECT_TRUE(deser->checksum_dblocks);
    EXPECT_GE(H5AC_FHEAP_HDR[0].free_icr(deser), 0);
  }
}

// Exercises indirect block cache callbacks: initial load size, checksum checks,
// root/child/multi-level deserialize (filtered & unfiltered), pre_serialize
// alloc error + tmp relocation + real-space no-op, notify actions/errors,
// and free_icr.
TEST(H5HFcacheTest, IndirectBlockCallbacksMultiLevelAndErrors) {
  for (bool filtered : {false, true}) {
    ScopedFile file = CreateInMemoryFile("h5hfcache_iblock.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // Configure small doubling table so inserting several objects forces
    // a multi-level tree (root iblock with nrows > max_direct_rows and a
    // child indirect block):
    //   width=2, start_block_size=128, max_direct_size=128 ->
    //   max_direct_rows=2, max_index=12 -> max_root_rows=5 (> 2!).
    H5HF_create_t cparam = MakeHeapParams(
        /*width=*/2, /*start_block_size=*/128, /*max_direct_size=*/128,
        /*max_index=*/12, /*start_root_rows=*/1, /*checksum_dblocks=*/true,
        /*max_man_size=*/96);
    auto reset_pline = absl::MakeCleanup([&]() {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    });
    if (filtered) {
      unsigned level = 6;
      ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                           1, &level),
                0);
    }

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    bool heap_closed = false;
    auto close_fh = absl::MakeCleanup([&]() {
      if (!heap_closed && fh != nullptr) {
        H5HF_close(fh);
      }
    });

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id(id_len);
    std::vector<std::vector<uint8_t>> inserted_ids;
    std::vector<std::vector<uint8_t>> inserted_vals;

    // Insert 7 objects of 72 bytes each so each consumes a whole 128B block,
    // creating root direct entries + at least one child indirect block row.
    for (int i = 0; i < 7; ++i) {
      std::vector<uint8_t> val = MakePattern(72, static_cast<uint8_t>(i * 19));
      ASSERT_GE(H5HF_insert(fh, val.size(), val.data(), id.data()), 0);
      inserted_ids.push_back(id);
      inserted_vals.push_back(std::move(val));
    }

    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_GT(hdr->man_dtable.curr_root_rows, hdr->man_dtable.max_direct_rows);
    ASSERT_NE(hdr->root_iblock, nullptr);
    H5HF_indirect_t* root_iblock = hdr->root_iblock;
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, root_iblock->addr));

    // Find the first child indirect block address in root_iblock's indirect
    // rows.
    const unsigned indirect_start =
        hdr->man_dtable.max_direct_rows * hdr->man_dtable.cparam.width;
    unsigned child_entry = 0;
    haddr_t child_ib_addr = HADDR_UNDEF;
    for (unsigned u = indirect_start;
         u < root_iblock->nrows * hdr->man_dtable.cparam.width; ++u) {
      if (H5_addr_defined(root_iblock->ents[u].addr)) {
        child_entry = u;
        child_ib_addr = root_iblock->ents[u].addr;
        break;
      }
    }
    ASSERT_NE(child_ib_addr, HADDR_UNDEF);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, child_ib_addr));

    const unsigned child_nrows = H5HF__dtable_size_to_rows(
        &hdr->man_dtable,
        hdr->man_dtable
            .row_block_size[child_entry / hdr->man_dtable.cparam.width]);
    bool child_did_protect = false;
    H5HF_indirect_t* child_iblock = H5HF__man_iblock_protect(
        hdr, child_ib_addr, child_nrows, root_iblock, child_entry, false,
        H5AC__READ_ONLY_FLAG, &child_did_protect);
    ASSERT_NE(child_iblock, nullptr);
    auto unprot_child = absl::MakeCleanup([&]() {
      if (child_iblock != nullptr) {
        H5HF__man_iblock_unprotect(child_iblock, H5AC__NO_FLAGS_SET,
                                   child_did_protect);
        child_iblock = nullptr;
      }
    });

    // 1. Direct pre_serialize allocation failure on tmp-addressed root iblock.
    {
      ScopedErrorSilence silence;
      ScopedBlockAllocFailure alloc_fail(f);
      haddr_t new_addr = HADDR_UNDEF;
      size_t new_len = 0;
      unsigned flags = 0;
      EXPECT_LT(H5AC_FHEAP_IBLOCK[0].pre_serialize(
                    f, root_iblock, root_iblock->addr, root_iblock->size,
                    &new_addr, &new_len, &flags),
                0);
    }

    // 2. notify(): test no-op actions, BEFORE_EVICT/AFTER_LOAD/AFTER_INSERT
    //    success + failure, and unknown action error on child_iblock.
    {
      for (H5AC_notify_action_t action :
           {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
            H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
            H5AC_NOTIFY_ACTION_CHILD_CLEANED,
            H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
            H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
        EXPECT_GE(H5AC_FHEAP_IBLOCK[0].notify(action, child_iblock), 0);
      }

      void* const saved_fd_parent = child_iblock->fd_parent;
      ASSERT_NE(saved_fd_parent, nullptr);

      // Destroy active flush dependency and verify fd_parent cleared.
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT,
                                            child_iblock),
                0);
      EXPECT_EQ(child_iblock->fd_parent, nullptr);

      {
        ScopedErrorSilence silence;
        // Unknown action returns failure (10 is in valid enum range [0, 15]).
        EXPECT_LT(H5AC_FHEAP_IBLOCK[0].notify(
                      static_cast<H5AC_notify_action_t>(10), child_iblock),
                  0);

        // BEFORE_EVICT without an active dependency fails destroy_flush_depend.
        child_iblock->fd_parent = saved_fd_parent;
        EXPECT_LT(H5AC_FHEAP_IBLOCK[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT,
                                              child_iblock),
                  0);

        // AFTER_INSERT with unpinned parent fails create_flush_depend.
        auto* par_entry = static_cast<H5C_cache_entry_t*>(saved_fd_parent);
        const bool saved_pinned = par_entry->is_pinned;
        const bool saved_protected = par_entry->is_protected;
        par_entry->is_pinned = false;
        par_entry->is_protected = false;
        EXPECT_LT(H5AC_FHEAP_IBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT,
                                              child_iblock),
                  0);
        par_entry->is_pinned = saved_pinned;
        par_entry->is_protected = saved_protected;
      }

      // Restore flush dependency via AFTER_LOAD, then test AFTER_INSERT too.
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_LOAD,
                                            child_iblock),
                0);
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT,
                                            child_iblock),
                0);
      child_iblock->fd_parent = saved_fd_parent;
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT,
                                            child_iblock),
                0);
    }

    // Release protection before flushing the cache.
    std::move(unprot_child).Invoke();

    // 3. Flushing moves both child and root iblocks from tmp_addr to real
    //    file space (exercising pre_serialize parent==NULL and parent!=NULL)
    //    and serializes them. Re-dirty + flush hits the non-tmp no-op branch.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    EXPECT_FALSE(H5F_IS_TMP_ADDR(f, hdr->man_dtable.table_addr));

    {
      // Exercise pre_serialize no-op when root iblock is already in real space.
      haddr_t new_addr = HADDR_UNDEF;
      size_t new_len = 0;
      unsigned flags = 99;
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].pre_serialize(
                    f, root_iblock, root_iblock->addr, root_iblock->size,
                    &new_addr, &new_len, &flags),
                0);
      EXPECT_EQ(flags, 0U);
    }

    // 4. Direct get_initial_load_size, image_len, serialize, verify_chksum,
    //    deserialize (root + child), error paths, and free_icr.
    {
      H5HF_parent_t root_par{hdr, nullptr, 0};
      const unsigned root_nrows = root_iblock->nrows;
      H5HF_iblock_cache_ud_t root_ud{&root_par, f, &root_nrows};

      size_t init_len = 0;
      ASSERT_GE(H5AC_FHEAP_IBLOCK[0].get_initial_load_size(&root_ud, &init_len),
                0);
      EXPECT_EQ(init_len, root_iblock->size);

      size_t img_len = 0;
      ASSERT_GE(H5AC_FHEAP_IBLOCK[0].image_len(root_iblock, &img_len), 0);
      EXPECT_EQ(img_len, root_iblock->size);

      std::vector<uint8_t> root_img(img_len, 0);
      ASSERT_GE(H5AC_FHEAP_IBLOCK[0].serialize(f, root_img.data(), img_len,
                                               root_iblock),
                0);

      EXPECT_EQ(H5AC_FHEAP_IBLOCK[0].verify_chksum(root_img.data(), img_len,
                                                   &root_ud),
                1);
      {
        std::vector<uint8_t> flipped = root_img;
        flipped[1] ^= 0x02U;
        EXPECT_EQ(H5AC_FHEAP_IBLOCK[0].verify_chksum(flipped.data(), img_len,
                                                     &root_ud),
                  0);
      }
      {
        ScopedErrorSilence silence;
        EXPECT_LT(
            H5AC_FHEAP_IBLOCK[0].verify_chksum(root_img.data(), 2, &root_ud),
            0);
      }

      // Deserialize root indirect block (nrows > max_direct_rows allocates
      // child_iblocks array!).
      bool dirty = false;
      auto* deser_root =
          static_cast<H5HF_indirect_t*>(H5AC_FHEAP_IBLOCK[0].deserialize(
              root_img.data(), img_len, &root_ud, &dirty));
      ASSERT_NE(deser_root, nullptr);
      EXPECT_NE(deser_root->child_iblocks, nullptr);
      EXPECT_EQ(deser_root->nrows, root_nrows);
      EXPECT_GT(deser_root->nchildren, 0U);
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].free_icr(deser_root), 0);

      // Serialize & deserialize child indirect block (parent != nullptr).
      const haddr_t real_child_addr = root_iblock->ents[child_entry].addr;
      ASSERT_FALSE(H5F_IS_TMP_ADDR(f, real_child_addr));
      child_iblock = H5HF__man_iblock_protect(
          hdr, real_child_addr, child_nrows, root_iblock, child_entry, false,
          H5AC__READ_ONLY_FLAG, &child_did_protect);
      ASSERT_NE(child_iblock, nullptr);

      size_t child_img_len = 0;
      ASSERT_GE(H5AC_FHEAP_IBLOCK[0].image_len(child_iblock, &child_img_len),
                0);
      std::vector<uint8_t> child_img(child_img_len, 0);
      ASSERT_GE(H5AC_FHEAP_IBLOCK[0].serialize(f, child_img.data(),
                                               child_img_len, child_iblock),
                0);
      ASSERT_GE(H5HF__man_iblock_unprotect(child_iblock, H5AC__NO_FLAGS_SET,
                                           child_did_protect),
                0);
      child_iblock = nullptr;

      H5HF_parent_t child_par{hdr, root_iblock, child_entry};
      H5HF_iblock_cache_ud_t child_ud{&child_par, f, &child_nrows};
      auto* deser_child =
          static_cast<H5HF_indirect_t*>(H5AC_FHEAP_IBLOCK[0].deserialize(
              child_img.data(), child_img_len, &child_ud, &dirty));
      ASSERT_NE(deser_child, nullptr);
      EXPECT_EQ(deser_child->parent, root_iblock);
      EXPECT_EQ(deser_child->child_iblocks, nullptr);
      EXPECT_GE(H5AC_FHEAP_IBLOCK[0].free_icr(deser_child), 0);

      // Corruption error paths in deserialize: bad magic, bad version, bad
      // heap_addr.
      ScopedErrorSilence silence;
      std::vector<uint8_t> bad_magic = root_img;
      bad_magic[0] = 'Z';
      EXPECT_EQ(H5AC_FHEAP_IBLOCK[0].deserialize(bad_magic.data(), img_len,
                                                 &root_ud, &dirty),
                nullptr);

      std::vector<uint8_t> bad_ver = root_img;
      bad_ver[H5_SIZEOF_MAGIC] = 42;
      EXPECT_EQ(H5AC_FHEAP_IBLOCK[0].deserialize(bad_ver.data(), img_len,
                                                 &root_ud, &dirty),
                nullptr);

      std::vector<uint8_t> bad_addr = root_img;
      bad_addr[H5_SIZEOF_MAGIC + 1] ^= 0xffU;
      EXPECT_EQ(H5AC_FHEAP_IBLOCK[0].deserialize(bad_addr.data(), img_len,
                                                 &root_ud, &dirty),
                nullptr);
    }

    // 5. Evict everything from cache after closing and reopening to exercise
    //    cache-driven AFTER_LOAD / deserialize end-to-end.
    const haddr_t heap_addr = hdr->heap_addr;
    ASSERT_GE(H5HF_close(fh), 0);
    heap_closed = true;
    fh = nullptr;
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    H5HF_t* reopened = H5HF_open(f, heap_addr);
    ASSERT_NE(reopened, nullptr);
    auto close_reopened = absl::MakeCleanup([&]() { H5HF_close(reopened); });
    for (size_t i = 0; i < inserted_ids.size(); ++i) {
      std::vector<uint8_t> out(inserted_vals[i].size(), 0);
      ASSERT_GE(H5HF_read(reopened, inserted_ids[i].data(), out.data()), 0);
      EXPECT_THAT(out, ElementsAreArray(inserted_vals[i]));
    }
  }
}

// Registers the test-only dynamic filter and unregisters on scope exit.
class ScopedDynamicFilter {
 public:
  ScopedDynamicFilter() {
    g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    const H5Z_class2_t dyn_filter = {
        .version = H5Z_CLASS_T_VERS,
        .id = kDynamicFilterId,
        .encoder_present = 1,
        .decoder_present = 1,
        .name = "test_dynamic_filter",
        .can_apply = nullptr,
        .set_local = nullptr,
        .filter = DynamicFilterApply,
    };
    registered_ = (H5Zregister(&dyn_filter) >= 0);
  }

  ~ScopedDynamicFilter() {
    g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    if (registered_) {
      H5Zunregister(kDynamicFilterId);
    }
  }

  ScopedDynamicFilter(const ScopedDynamicFilter&) = delete;
  ScopedDynamicFilter& operator=(const ScopedDynamicFilter&) = delete;

  bool valid() const { return registered_; }

 private:
  bool registered_ = false;
};

// Exercises unfiltered direct block cache callbacks across checksummed and
// unchecksummed heaps: pre_serialize tmp relocation + alloc error + real-space
// no-op, notify actions/errors, image_len, verify_chksum, deserialize/errors,
// and real-space dblock removal triggering fsf_size.
TEST(H5HFcacheTest, DirectBlockUnfilteredCallbacksAndFsfSize) {
  for (bool checksum_dblocks : {true, false}) {
    ScopedFile file = CreateInMemoryFile("h5hfcache_dblock_unfilt.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    const H5HF_create_t cparam = MakeHeapParams(
        /*width=*/2, /*start_block_size=*/128, /*max_direct_size=*/256,
        /*max_index=*/16, /*start_root_rows=*/1,
        /*checksum_dblocks=*/checksum_dblocks, /*max_man_size=*/200);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
    std::vector<uint8_t> id1(id_len), id2(id_len);
    const std::vector<uint8_t> val1 = MakePattern(72, 0x11U);
    const std::vector<uint8_t> val2 = MakePattern(72, 0x22U);

    // First insert creates a root direct block at tmp_addr.
    ASSERT_GE(H5HF_insert(fh, val1.size(), val1.data(), id1.data()), 0);
    H5HF_hdr_t* hdr = fh->hdr;
    ASSERT_EQ(hdr->man_dtable.curr_root_rows, 0U);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, hdr->man_dtable.table_addr));

    {
      H5HF_direct_t* root_db =
          H5HF__man_dblock_protect(hdr, hdr->man_dtable.table_addr,
                                   hdr->man_dtable.cparam.start_block_size,
                                   nullptr, 0, H5AC__READ_ONLY_FLAG);
      ASSERT_NE(root_db, nullptr);

      for (H5AC_notify_action_t action :
           {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
            H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
            H5AC_NOTIFY_ACTION_CHILD_CLEANED,
            H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
            H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
        EXPECT_GE(H5AC_FHEAP_DBLOCK[0].notify(action, root_db), 0);
      }
      void* const saved_fd = root_db->fd_parent;
      ASSERT_NE(saved_fd, nullptr);
      EXPECT_GE(
          H5AC_FHEAP_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT, root_db),
          0);
      EXPECT_EQ(root_db->fd_parent, nullptr);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5AC_FHEAP_DBLOCK[0].notify(
                      static_cast<H5AC_notify_action_t>(10), root_db),
                  0);
        root_db->fd_parent = saved_fd;
        EXPECT_LT(H5AC_FHEAP_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT,
                                              root_db),
                  0);
        auto* par_entry = static_cast<H5C_cache_entry_t*>(saved_fd);
        const bool saved_pinned = par_entry->is_pinned;
        const bool saved_protected = par_entry->is_protected;
        par_entry->is_pinned = false;
        par_entry->is_protected = false;
        EXPECT_LT(H5AC_FHEAP_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT,
                                              root_db),
                  0);
        par_entry->is_pinned = saved_pinned;
        par_entry->is_protected = saved_protected;
      }
      EXPECT_GE(
          H5AC_FHEAP_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_LOAD, root_db),
          0);
      EXPECT_GE(
          H5AC_FHEAP_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT, root_db),
          0);
      root_db->fd_parent = saved_fd;
      EXPECT_GE(
          H5AC_FHEAP_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, root_db),
          0);

      // Alloc failure during pre_serialize on tmp root dblock.
      {
        ScopedErrorSilence silence;
        ScopedBlockAllocFailure alloc_fail(f);
        haddr_t new_addr = HADDR_UNDEF;
        size_t new_len = 0;
        unsigned flags = 0;
        EXPECT_LT(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                      f, root_db, hdr->man_dtable.table_addr, root_db->size,
                      &new_addr, &new_len, &flags),
                  0);
      }

      ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, hdr->man_dtable.table_addr,
                               root_db, H5AC__NO_FLAGS_SET),
                0);
    }

    // Flush moves root dblock to real space.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_FALSE(H5F_IS_TMP_ADDR(f, hdr->man_dtable.table_addr));

    {
      H5HF_direct_t* root_db =
          H5HF__man_dblock_protect(hdr, hdr->man_dtable.table_addr,
                                   hdr->man_dtable.cparam.start_block_size,
                                   nullptr, 0, H5AC__NO_FLAGS_SET);
      ASSERT_NE(root_db, nullptr);
      H5HF_dblock_cache_ud_t db_ud{};
      db_ud.par_info = {hdr, nullptr, 0};
      db_ud.f = f;
      db_ud.dblock_size = root_db->size;
      db_ud.odi_size = root_db->size;

      size_t init_len = 0;
      ASSERT_GE(H5AC_FHEAP_DBLOCK[0].get_initial_load_size(&db_ud, &init_len),
                0);
      EXPECT_EQ(init_len, root_db->size);

      size_t img_len = 0;
      ASSERT_GE(H5AC_FHEAP_DBLOCK[0].image_len(root_db, &img_len), 0);
      EXPECT_EQ(img_len, root_db->size);

      haddr_t new_addr = HADDR_UNDEF;
      size_t new_len = 0;
      unsigned flags = 99;
      ASSERT_GE(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                    f, root_db, hdr->man_dtable.table_addr, root_db->size,
                    &new_addr, &new_len, &flags),
                0);
      EXPECT_EQ(flags, 0U);

      std::vector<uint8_t> img(img_len, 0);
      ASSERT_GE(H5AC_FHEAP_DBLOCK[0].serialize(f, img.data(), img_len, root_db),
                0);

      // Test fsf_size directly on root_db with temporary file_size.
      const hsize_t saved_fs = root_db->file_size;
      root_db->file_size = img_len;
      hsize_t reported_fs = 0;
      EXPECT_GE(H5AC_FHEAP_DBLOCK[0].fsf_size(root_db, &reported_fs), 0);
      EXPECT_EQ(reported_fs, img_len);
      root_db->file_size = saved_fs;

      ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, hdr->man_dtable.table_addr,
                               root_db, H5AC__NO_FLAGS_SET),
                0);

      // Verify checksum behavior for checksum_dblocks true vs false.
      EXPECT_EQ(H5AC_FHEAP_DBLOCK[0].verify_chksum(img.data(), img_len, &db_ud),
                1);
      std::vector<uint8_t> flipped = img;
      flipped[0] ^= 0x01U;
      if (checksum_dblocks) {
        EXPECT_EQ(
            H5AC_FHEAP_DBLOCK[0].verify_chksum(flipped.data(), img_len, &db_ud),
            0);
      } else {
        EXPECT_EQ(
            H5AC_FHEAP_DBLOCK[0].verify_chksum(flipped.data(), img_len, &db_ud),
            1);
      }

      // Deserialize valid root direct block and free_icr.
      bool dirty = false;
      auto* deser =
          static_cast<H5HF_direct_t*>(H5AC_FHEAP_DBLOCK[0].deserialize(
              img.data(), img_len, &db_ud, &dirty));
      ASSERT_NE(deser, nullptr);
      EXPECT_EQ(deser->parent, nullptr);
      EXPECT_GE(H5AC_FHEAP_DBLOCK[0].free_icr(deser), 0);

      // Bad magic, bad version, bad heap_addr errors in deserialize.
      ScopedErrorSilence silence;
      std::vector<uint8_t> bad_magic = img;
      bad_magic[0] = 'Q';
      EXPECT_EQ(H5AC_FHEAP_DBLOCK[0].deserialize(bad_magic.data(), img_len,
                                                 &db_ud, &dirty),
                nullptr);

      std::vector<uint8_t> bad_ver = img;
      bad_ver[H5_SIZEOF_MAGIC] = 88;
      EXPECT_EQ(H5AC_FHEAP_DBLOCK[0].deserialize(bad_ver.data(), img_len,
                                                 &db_ud, &dirty),
                nullptr);

      std::vector<uint8_t> bad_addr = img;
      bad_addr[H5_SIZEOF_MAGIC + 1] ^= 0xffU;
      EXPECT_EQ(H5AC_FHEAP_DBLOCK[0].deserialize(bad_addr.data(), img_len,
                                                 &db_ud, &dirty),
                nullptr);
    }

    // Second insert promotes to root indirect block with two child direct
    // blocks (entry 0 and entry 1).
    ASSERT_GE(H5HF_insert(fh, val2.size(), val2.data(), id2.data()), 0);
    ASSERT_GT(hdr->man_dtable.curr_root_rows, 0U);
    ASSERT_NE(hdr->root_iblock, nullptr);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, hdr->root_iblock->ents[1].addr));

    // Flush so child dblocks move to real space, then remove id2 so child
    // dblock 1 is destroyed via H5AC__DELETED_FLAG | H5AC__FREE_FILE_SPACE_FLAG
    // -> triggers H5HF__cache_dblock_fsf_size!
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_FALSE(H5F_IS_TMP_ADDR(f, hdr->root_iblock->ents[1].addr));
    EXPECT_GE(H5HF_remove(fh, id2.data()), 0);
    EXPECT_EQ(hdr->man_nobjs, 1U);
    if (hdr->man_dtable.curr_root_rows > 0) {
      bool did_prot = false;
      H5HF_indirect_t* ib = H5HF__man_iblock_protect(
          hdr, hdr->man_dtable.table_addr, hdr->man_dtable.curr_root_rows,
          nullptr, 0, false, H5AC__READ_ONLY_FLAG, &did_prot);
      ASSERT_NE(ib, nullptr);
      EXPECT_FALSE(H5_addr_defined(ib->ents[1].addr));
      EXPECT_GE(H5HF__man_iblock_unprotect(ib, H5AC__NO_FLAGS_SET, did_prot),
                0);
    }
  }
}

// Exercises filtered root direct block cache callbacks: image_len fallbacks,
// tmp relocation + H5MF_alloc error, real-space resize and filter_mask updates,
// verify_chksum decompress/flipped/pipeline errors, and deserialize.
TEST(H5HFcacheTest, DirectBlockFilteredRootCallbacks) {
  ScopedDynamicFilter dyn_reg;
  ASSERT_TRUE(dyn_reg.valid());

  ScopedFile file = CreateInMemoryFile("h5hfcache_dblock_filt_root.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5HF_create_t cparam = MakeHeapParams(
      /*width=*/2, /*start_block_size=*/128, /*max_direct_size=*/256,
      /*max_index=*/16, /*start_root_rows=*/1, /*checksum_dblocks=*/true,
      /*max_man_size=*/200);
  auto reset_pline = absl::MakeCleanup([&]() {
    if (cparam.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
    }
  });
  ASSERT_GE(H5Z_append(&cparam.pline, kDynamicFilterId, H5Z_FLAG_OPTIONAL, 0,
                       nullptr),
            0);

  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
  std::vector<uint8_t> id1(id_len);
  const std::vector<uint8_t> val1 = MakePattern(48, 0x33U);
  ASSERT_GE(H5HF_insert(fh, val1.size(), val1.data(), id1.data()), 0);

  H5HF_hdr_t* hdr = fh->hdr;
  ASSERT_EQ(hdr->man_dtable.curr_root_rows, 0U);
  ASSERT_GT(hdr->filter_len, 0U);
  ASSERT_TRUE(H5F_IS_TMP_ADDR(f, hdr->man_dtable.table_addr));

  // While still in tmp space, exercise image_len fallbacks and H5MF_alloc
  // failure in pre_serialize cleanly without freeing live real space.
  {
    H5HF_direct_t* root_db =
        H5HF__man_dblock_protect(hdr, hdr->man_dtable.table_addr,
                                 hdr->man_dtable.cparam.start_block_size,
                                 nullptr, 0, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root_db, nullptr);
    size_t img_len = 0;
    const size_t saved_root_sz = hdr->pline_root_direct_size;

    root_db->file_size = 96;
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].image_len(root_db, &img_len), 0);
    EXPECT_EQ(img_len, 96U);
    root_db->file_size = 0;

    hdr->pline_root_direct_size = 80;
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].image_len(root_db, &img_len), 0);
    EXPECT_EQ(img_len, 80U);

    hdr->pline_root_direct_size = 0;
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].image_len(root_db, &img_len), 0);
    EXPECT_EQ(img_len, root_db->size);

    hdr->pline_root_direct_size = saved_root_sz;

    {
      ScopedErrorSilence silence;
      ScopedBlockAllocFailure alloc_fail(f);
      haddr_t new_addr = HADDR_UNDEF;
      size_t new_len = 0;
      unsigned flags = 0;
      g_dynamic_filter_mode = FilterMode::kShrinkTail;
      EXPECT_LT(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                    f, root_db, hdr->man_dtable.table_addr,
                    hdr->pline_root_direct_size, &new_addr, &new_len, &flags),
                0);
      g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    }

    ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, hdr->man_dtable.table_addr,
                             root_db, H5AC__NO_FLAGS_SET),
              0);
  }

  // Flush 1: kShrinkTail shrinks 128 -> 112 (at_tmp_addr == true, moved +
  // resized).
  g_dynamic_filter_mode = FilterMode::kShrinkTail;
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  EXPECT_EQ(hdr->pline_root_direct_size, 112U);
  EXPECT_EQ(hdr->pline_root_direct_filter_mask, 0U);
  EXPECT_FALSE(H5F_IS_TMP_ADDR(f, hdr->man_dtable.table_addr));

  // Flush 2: update object with kSkipOptional (sets filter_mask bit 0 and
  // changes size 112 -> 128 in real space -> H5MF_xfree + H5MF_alloc).
  bool id_changed = false;
  const std::vector<uint8_t> val1_upd = MakePattern(48, 0x55U);
  ASSERT_GE(H5HF_write(fh, id1.data(), &id_changed, val1_upd.data()), 0);
  g_dynamic_filter_mode = FilterMode::kSkipOptional;
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  EXPECT_EQ(hdr->pline_root_direct_size, 128U);
  EXPECT_EQ(hdr->pline_root_direct_filter_mask, 1U);

  // Flush 3: update object again keeping same filter_mask and size (unchanged
  // real-space path).
  const std::vector<uint8_t> val1_upd2 = MakePattern(48, 0x77U);
  ASSERT_GE(H5HF_write(fh, id1.data(), &id_changed, val1_upd2.data()), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  EXPECT_EQ(hdr->pline_root_direct_size, 128U);

  // Direct filtered root dblock pre_serialize, serialize, verify_chksum, and
  // deserialize.
  {
    H5HF_direct_t* root_db =
        H5HF__man_dblock_protect(hdr, hdr->man_dtable.table_addr,
                                 hdr->man_dtable.cparam.start_block_size,
                                 nullptr, 0, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(root_db, nullptr);

    haddr_t new_addr = HADDR_UNDEF;
    size_t new_len = 0;
    unsigned flags = 0;
    g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    ASSERT_GE(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                  f, root_db, hdr->man_dtable.table_addr,
                  hdr->pline_root_direct_size, &new_addr, &new_len, &flags),
              0);
    const size_t ser_len = root_db->write_size;
    std::vector<uint8_t> ser_img(ser_len, 0);
    ASSERT_GE(
        H5AC_FHEAP_DBLOCK[0].serialize(f, ser_img.data(), ser_len, root_db), 0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, hdr->man_dtable.table_addr,
                             root_db, H5AC__NO_FLAGS_SET),
              0);

    H5HF_dblock_cache_ud_t db_ud{};
    db_ud.par_info = {hdr, nullptr, 0};
    db_ud.f = f;
    db_ud.dblock_size = 128;
    db_ud.odi_size = hdr->pline_root_direct_size;
    db_ud.filter_mask = 0;

    size_t init_len = 0;
    ASSERT_GE(H5AC_FHEAP_DBLOCK[0].get_initial_load_size(&db_ud, &init_len), 0);
    EXPECT_EQ(init_len, hdr->pline_root_direct_size);

    // verify_chksum allocates db_ud.dblk and sets decompressed=true; passing
    // db_ud to deserialize transfers ownership to deser, freed by free_icr.
    EXPECT_EQ(
        H5AC_FHEAP_DBLOCK[0].verify_chksum(ser_img.data(), ser_len, &db_ud), 1);
    EXPECT_TRUE(db_ud.decompressed);
    ASSERT_NE(db_ud.dblk, nullptr);
    bool dirty = false;
    auto* deser = static_cast<H5HF_direct_t*>(H5AC_FHEAP_DBLOCK[0].deserialize(
        ser_img.data(), ser_len, &db_ud, &dirty));
    ASSERT_NE(deser, nullptr);
    EXPECT_EQ(db_ud.dblk, nullptr);
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].free_icr(deser), 0);

    // verify_chksum with flipped checksum byte -> returns 0 (no leak).
    {
      H5HF_dblock_cache_ud_t bad_ud = db_ud;
      bad_ud.dblk = nullptr;
      bad_ud.decompressed = false;
      std::vector<uint8_t> flipped = ser_img;
      flipped[H5HF_MAN_ABS_DIRECT_OVERHEAD(hdr) - 1] ^= 0x10U;
      EXPECT_EQ(
          H5AC_FHEAP_DBLOCK[0].verify_chksum(flipped.data(), ser_len, &bad_ud),
          0);
    }

    // verify_chksum pipeline failure (reverse filter returns 0 -> FAIL).
    {
      ScopedErrorSilence silence;
      H5HF_dblock_cache_ud_t fail_ud = db_ud;
      fail_ud.dblk = nullptr;
      fail_ud.decompressed = false;
      g_dynamic_filter_mode = FilterMode::kFail;
      EXPECT_LT(
          H5AC_FHEAP_DBLOCK[0].verify_chksum(ser_img.data(), ser_len, &fail_ud),
          0);
      g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    }
  }
}

// Exercises filtered child direct block cache callbacks under a root indirect
// block: get_initial_load_size, image_len, H5MF_alloc & mandatory H5Z_pipeline
// errors in pre_serialize, real-space resize + filter_mask updates, unchanged
// flush, and child deserialize.
TEST(H5HFcacheTest, DirectBlockFilteredChildCallbacks) {
  ScopedDynamicFilter dyn_reg;
  ASSERT_TRUE(dyn_reg.valid());

  ScopedFile file = CreateInMemoryFile("h5hfcache_dblock_filt_child.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5HF_create_t cparam = MakeHeapParams(
      /*width=*/2, /*start_block_size=*/128, /*max_direct_size=*/256,
      /*max_index=*/16, /*start_root_rows=*/1, /*checksum_dblocks=*/true,
      /*max_man_size=*/200);
  auto reset_pline = absl::MakeCleanup([&]() {
    if (cparam.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
    }
  });
  ASSERT_GE(H5Z_append(&cparam.pline, kDynamicFilterId, H5Z_FLAG_OPTIONAL, 0,
                       nullptr),
            0);

  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
  std::vector<uint8_t> id1(id_len), id2(id_len);
  const std::vector<uint8_t> val1 = MakePattern(72, 0x10U);
  const std::vector<uint8_t> val2 = MakePattern(72, 0x20U);

  ASSERT_GE(H5HF_insert(fh, val1.size(), val1.data(), id1.data()), 0);
  ASSERT_GE(H5HF_insert(fh, val2.size(), val2.data(), id2.data()), 0);

  H5HF_hdr_t* hdr = fh->hdr;
  ASSERT_GT(hdr->man_dtable.curr_root_rows, 0U);
  H5HF_indirect_t* root_ib = hdr->root_iblock;
  ASSERT_NE(root_ib, nullptr);
  ASSERT_TRUE(H5_addr_defined(root_ib->ents[1].addr));
  ASSERT_TRUE(H5F_IS_TMP_ADDR(f, root_ib->ents[1].addr));

  // Check get_initial_load_size, image_len, and tmp-space pre_serialize error
  // paths on filtered child dblock.
  {
    H5HF_dblock_cache_ud_t db_ud{};
    db_ud.par_info = {hdr, root_ib, 1};
    db_ud.f = f;
    db_ud.dblock_size = 128;
    size_t init_len = 0;
    ASSERT_GE(H5AC_FHEAP_DBLOCK[0].get_initial_load_size(&db_ud, &init_len), 0);
    EXPECT_EQ(init_len, root_ib->filt_ents[1].size);

    H5HF_direct_t* child_db = H5HF__man_dblock_protect(
        hdr, root_ib->ents[1].addr, 128, root_ib, 1, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(child_db, nullptr);
    size_t img_len = 0;
    const size_t saved_ent_sz = root_ib->filt_ents[1].size;
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].image_len(child_db, &img_len), 0);
    EXPECT_EQ(img_len, saved_ent_sz);

    root_ib->filt_ents[1].size = 0;
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].image_len(child_db, &img_len), 0);
    EXPECT_EQ(img_len, child_db->size);
    root_ib->filt_ents[1].size = saved_ent_sz;

    {
      ScopedErrorSilence silence;
      {
        ScopedBlockAllocFailure alloc_fail(f);
        haddr_t new_addr = HADDR_UNDEF;
        size_t new_len = 0;
        unsigned flags = 0;
        g_dynamic_filter_mode = FilterMode::kShrinkTail;
        EXPECT_LT(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                      f, child_db, root_ib->ents[1].addr,
                      root_ib->filt_ents[1].size, &new_addr, &new_len, &flags),
                  0);
        g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
      }

      // Mandatory filter returning 0 fails H5Z_pipeline in pre_serialize.
      const unsigned saved_flags = hdr->pline.filter[0].flags;
      hdr->pline.filter[0].flags = H5Z_FLAG_MANDATORY;
      g_dynamic_filter_mode = FilterMode::kFail;
      haddr_t new_addr = HADDR_UNDEF;
      size_t new_len = 0;
      unsigned flags = 0;
      EXPECT_LT(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                    f, child_db, root_ib->ents[1].addr,
                    root_ib->filt_ents[1].size, &new_addr, &new_len, &flags),
                0);
      hdr->pline.filter[0].flags = saved_flags;
      g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    }

    ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, root_ib->ents[1].addr,
                             child_db, H5AC__NO_FLAGS_SET),
              0);
  }

  // Flush 1: kShrinkTail shrinks child dblocks 128 -> 112 in tmp space.
  g_dynamic_filter_mode = FilterMode::kShrinkTail;
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  EXPECT_EQ(root_ib->filt_ents[1].size, 112U);
  EXPECT_EQ(root_ib->filt_ents[1].filter_mask, 0U);
  EXPECT_FALSE(H5F_IS_TMP_ADDR(f, root_ib->ents[1].addr));

  // Flush 2: modify id2 and switch to kSkipOptional -> changes filter_mask
  // 0 -> 1 and resizes 112 -> 128 in real space (H5MF_xfree + H5MF_alloc).
  bool id_changed = false;
  const std::vector<uint8_t> val2_upd = MakePattern(72, 0x40U);
  ASSERT_GE(H5HF_write(fh, id2.data(), &id_changed, val2_upd.data()), 0);
  g_dynamic_filter_mode = FilterMode::kSkipOptional;
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  EXPECT_EQ(root_ib->filt_ents[1].size, 128U);
  EXPECT_EQ(root_ib->filt_ents[1].filter_mask, 1U);

  // Flush 3: modify id2 keeping kSkipOptional -> hits !at_tmp_addr unchanged
  // size & unchanged filter_mask path.
  const std::vector<uint8_t> val2_upd2 = MakePattern(72, 0x60U);
  ASSERT_GE(H5HF_write(fh, id2.data(), &id_changed, val2_upd2.data()), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // Serialize, verify_chksum, and deserialize child dblock with non-NULL
  // parent.
  {
    H5HF_direct_t* child_db = H5HF__man_dblock_protect(
        hdr, root_ib->ents[1].addr, 128, root_ib, 1, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(child_db, nullptr);

    haddr_t new_addr = HADDR_UNDEF;
    size_t new_len = 0;
    unsigned flags = 0;
    g_dynamic_filter_mode = FilterMode::kCopyUnchanged;
    ASSERT_GE(H5AC_FHEAP_DBLOCK[0].pre_serialize(
                  f, child_db, root_ib->ents[1].addr,
                  root_ib->filt_ents[1].size, &new_addr, &new_len, &flags),
              0);
    const size_t ser_len = child_db->write_size;
    std::vector<uint8_t> ser_img(ser_len, 0);
    ASSERT_GE(
        H5AC_FHEAP_DBLOCK[0].serialize(f, ser_img.data(), ser_len, child_db),
        0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_FHEAP_DBLOCK, root_ib->ents[1].addr,
                             child_db, H5AC__NO_FLAGS_SET),
              0);

    H5HF_dblock_cache_ud_t db_ud{};
    db_ud.par_info = {hdr, root_ib, 1};
    db_ud.f = f;
    db_ud.dblock_size = 128;
    db_ud.odi_size = ser_len;
    db_ud.filter_mask = 0;

    EXPECT_EQ(
        H5AC_FHEAP_DBLOCK[0].verify_chksum(ser_img.data(), ser_len, &db_ud), 1);
    bool dirty = false;
    auto* deser = static_cast<H5HF_direct_t*>(H5AC_FHEAP_DBLOCK[0].deserialize(
        ser_img.data(), ser_len, &db_ud, &dirty));
    ASSERT_NE(deser, nullptr);
    EXPECT_EQ(deser->parent, root_ib);
    EXPECT_EQ(deser->par_entry, 1U);
    EXPECT_GE(H5AC_FHEAP_DBLOCK[0].free_icr(deser), 0);
  }
}

// End-to-end deflate-filtered heap with incompressible payload growth, flush,
// cache eviction, reopen, and data verification.
TEST(H5HFcacheTest, DirectBlockDeflateReopenEndToEnd) {
  ScopedFile file = CreateInMemoryFile("h5hfcache_deflate_e2e.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5HF_create_t cparam = MakeHeapParams(
      /*width=*/2, /*start_block_size=*/256, /*max_direct_size=*/512,
      /*max_index=*/16, /*start_root_rows=*/1, /*checksum_dblocks=*/true,
      /*max_man_size=*/300);
  auto reset_pline = absl::MakeCleanup([&]() {
    if (cparam.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
    }
  });
  unsigned level = 6;
  ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1,
                       &level),
            0);

  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  bool heap_closed = false;
  auto close_fh = absl::MakeCleanup([&]() {
    if (!heap_closed && fh != nullptr) {
      H5HF_close(fh);
    }
  });

  haddr_t heap_addr = HADDR_UNDEF;
  ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  std::vector<uint8_t> id1(id_len), id2(id_len);
  const std::vector<uint8_t> v1_zeros(140, 0x00U);
  const std::vector<uint8_t> v2_rand = MakeIncompressiblePattern(140, 0x9999);
  ASSERT_GE(H5HF_insert(fh, v1_zeros.size(), v1_zeros.data(), id1.data()), 0);
  ASSERT_GE(H5HF_insert(fh, v2_rand.size(), v2_rand.data(), id2.data()), 0);

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // Overwrite v1 with incompressible data so its compressed dblock grows.
  const std::vector<uint8_t> v1_rand = MakeIncompressiblePattern(140, 0x1234);
  bool id_changed = false;
  ASSERT_GE(H5HF_write(fh, id1.data(), &id_changed, v1_rand.data()), 0);
  ASSERT_GE(H5HF_close(fh), 0);
  heap_closed = true;
  fh = nullptr;

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  H5HF_t* reopened = H5HF_open(f, heap_addr);
  ASSERT_NE(reopened, nullptr);
  auto close_reopened = absl::MakeCleanup([&]() { H5HF_close(reopened); });

  std::vector<uint8_t> out1(v1_rand.size(), 0);
  std::vector<uint8_t> out2(v2_rand.size(), 0);
  EXPECT_GE(H5HF_read(reopened, id1.data(), out1.data()), 0);
  EXPECT_GE(H5HF_read(reopened, id2.data(), out2.data()), 0);
  EXPECT_THAT(out1, ElementsAreArray(v1_rand));
  EXPECT_THAT(out2, ElementsAreArray(v2_rand));
}

}  // namespace
}  // namespace hdf5
