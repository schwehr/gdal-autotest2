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
// Tests local heap metadata cache callbacks (prefix and data block) and
// corrupted header/free-list deserialization checks
// (third_party/hdf5/src/H5HLcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HLcache.c

#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5HL_prfx_t and H5HL_dblk_t). Match those struct layouts
// across both the prelude and internal headers.
#ifndef NDEBUG
#define H5HLCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5HL_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HLpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5HLCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HLCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::Eq;

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

void EncodeUint64LE(uint8_t* dst, uint64_t val, size_t nbytes) {
  for (size_t i = 0; i < nbytes; ++i) {
    dst[i] = static_cast<uint8_t>((val >> (i * 8)) & 0xffU);
  }
}

std::vector<uint8_t> BuildPrefixImage(size_t sizeof_size, size_t sizeof_addr,
                                      size_t prfx_size, haddr_t dblk_addr,
                                      size_t dblk_size, uint64_t free_block,
                                      size_t total_len) {
  std::vector<uint8_t> buf(total_len, 0);
  uint8_t* p = buf.data();
  std::memcpy(p, H5HL_MAGIC, H5_SIZEOF_MAGIC);
  p += H5_SIZEOF_MAGIC;
  *p++ = 0;  // H5HL_VERSION
  *p++ = 0;  // reserved
  *p++ = 0;
  *p++ = 0;
  EncodeUint64LE(p, dblk_size, sizeof_size);
  p += sizeof_size;
  EncodeUint64LE(p, free_block, sizeof_size);
  p += sizeof_size;
  EncodeUint64LE(p, dblk_addr, sizeof_addr);
  (void)prfx_size;
  return buf;
}

TEST(H5HLcacheTest, ClassDescriptorsHaveExpectedFields) {
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].id, H5AC_LHEAP_PRFX_ID);
  EXPECT_STREQ(H5AC_LHEAP_PRFX[0].name, "local heap prefix");
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].mem_type, H5FD_MEM_LHEAP);
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].flags, H5AC__CLASS_SPECULATIVE_LOAD_FLAG);
  EXPECT_NE(H5AC_LHEAP_PRFX[0].get_initial_load_size, nullptr);
  EXPECT_NE(H5AC_LHEAP_PRFX[0].get_final_load_size, nullptr);
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].verify_chksum, nullptr);
  EXPECT_NE(H5AC_LHEAP_PRFX[0].deserialize, nullptr);
  EXPECT_NE(H5AC_LHEAP_PRFX[0].image_len, nullptr);
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].pre_serialize, nullptr);
  EXPECT_NE(H5AC_LHEAP_PRFX[0].serialize, nullptr);
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].notify, nullptr);
  EXPECT_NE(H5AC_LHEAP_PRFX[0].free_icr, nullptr);
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].fsf_size, nullptr);

  EXPECT_EQ(H5AC_LHEAP_DBLK[0].id, H5AC_LHEAP_DBLK_ID);
  EXPECT_STREQ(H5AC_LHEAP_DBLK[0].name, "local heap datablock");
  EXPECT_EQ(H5AC_LHEAP_DBLK[0].mem_type, H5FD_MEM_LHEAP);
  EXPECT_EQ(H5AC_LHEAP_DBLK[0].flags, H5AC__CLASS_NO_FLAGS_SET);
  EXPECT_NE(H5AC_LHEAP_DBLK[0].get_initial_load_size, nullptr);
  EXPECT_EQ(H5AC_LHEAP_DBLK[0].get_final_load_size, nullptr);
  EXPECT_EQ(H5AC_LHEAP_DBLK[0].verify_chksum, nullptr);
  EXPECT_NE(H5AC_LHEAP_DBLK[0].deserialize, nullptr);
  EXPECT_NE(H5AC_LHEAP_DBLK[0].image_len, nullptr);
  EXPECT_EQ(H5AC_LHEAP_DBLK[0].pre_serialize, nullptr);
  EXPECT_NE(H5AC_LHEAP_DBLK[0].serialize, nullptr);
  EXPECT_NE(H5AC_LHEAP_DBLK[0].notify, nullptr);
  EXPECT_NE(H5AC_LHEAP_DBLK[0].free_icr, nullptr);
  EXPECT_EQ(H5AC_LHEAP_DBLK[0].fsf_size, nullptr);
}

TEST(H5HLcacheTest, FlushReopenAndEvictExercisePrefixAndDatablockCallbacks) {
  // Use sizeof_addr=4, sizeof_size=8 so the raw header (28 bytes) requires
  // 4-byte alignment padding to reach prfx_size (32 bytes), covering the gap
  // zeroing path in H5HL__cache_prefix_serialize.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), /*sizeof_addr=*/4, /*sizeof_size=*/8), 0);

  ScopedFile file =
      CreateInMemoryFile("h5hlcache_rt.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  haddr_t empty_addr = HADDR_UNDEF;
  haddr_t single_addr = HADDR_UNDEF;
  haddr_t full_single_addr = HADDR_UNDEF;
  haddr_t split_addr = HADDR_UNDEF;
  size_t off_s0 = 0;
  size_t off_s2 = 0;
  size_t off_split_0 = 0;
  size_t off_split_2 = 0;

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // 1. Contiguous heap (`single_cache_obj == true`) with a 2-node freelist
    // (`fl0 -> fl1 -> H5HL_FREE_NULL`).
    ASSERT_GE(H5HL_create(f, /*size_hint=*/128, &single_addr), 0);
    H5HL_t* h_single = H5HL_protect(f, single_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h_single, nullptr);
    size_t off_s1 = 0;
    constexpr char kS0[] = "alpha_item_0123";
    constexpr char kS1[] = "beta_item_01234";
    constexpr char kS2[] = "gamma_item_0123";
    ASSERT_GE(H5HL_insert(f, h_single, sizeof(kS0), kS0, &off_s0), 0);
    ASSERT_GE(H5HL_insert(f, h_single, sizeof(kS1), kS1, &off_s1), 0);
    ASSERT_GE(H5HL_insert(f, h_single, sizeof(kS2), kS2, &off_s2), 0);
    ASSERT_GE(H5HL_remove(f, h_single, off_s1, sizeof(kS1)), 0);
    ASSERT_NE(h_single->freelist, nullptr);
    ASSERT_NE(h_single->freelist->next, nullptr);
    ASSERT_GE(H5HL_unprotect(h_single), 0);

    // 2. Contiguous heap filled completely (`freelist == nullptr` ->
    // H5HL_FREE_NULL).
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &full_single_addr), 0);
    H5HL_t* h_full = H5HL_protect(f, full_single_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h_full, nullptr);
    size_t off_full = 0;
    uint8_t full_buf[32]{};
    std::memset(full_buf, 0x55, sizeof(full_buf));
    ASSERT_GE(H5HL_insert(f, h_full, sizeof(full_buf), full_buf, &off_full), 0);
    EXPECT_EQ(h_full->freelist, nullptr);
    ASSERT_GE(H5HL_unprotect(h_full), 0);

    // 3. Split heap (`single_cache_obj == false`) with a multi-node freelist:
    // create split_addr, block EOA with another heap, then grow split_addr.
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &split_addr), 0);
    haddr_t blocker_addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/32, &blocker_addr), 0);

    H5HL_t* h_split = H5HL_protect(f, split_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h_split, nullptr);
    uint8_t large_block[256]{};
    std::memset(large_block, 0x33, sizeof(large_block));
    size_t off_large = 0;
    ASSERT_GE(H5HL_insert(f, h_split, sizeof(large_block), large_block,
                          &off_large),
              0);
    ASSERT_FALSE(h_split->single_cache_obj);
    size_t off_split_1 = 0;
    ASSERT_GE(H5HL_insert(f, h_split, sizeof(kS0), kS0, &off_split_0), 0);
    ASSERT_GE(H5HL_insert(f, h_split, sizeof(kS1), kS1, &off_split_1), 0);
    ASSERT_GE(H5HL_insert(f, h_split, sizeof(kS2), kS2, &off_split_2), 0);
    ASSERT_GE(H5HL_remove(f, h_split, off_split_1, sizeof(kS1)), 0);
    {
      // Exercise error path in H5HL__cache_datablock_notify:
      // AFTER_LOAD fails when prfx is not protected.
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5AC_LHEAP_DBLK[0].notify(H5AC_NOTIFY_ACTION_AFTER_LOAD, h_split->dblk),
          0);
    }
    ASSERT_GE(H5HL_unprotect(h_split), 0);

    // 4. Empty heap (`dblk_size == 0`, `freelist == nullptr`).
    ASSERT_GE(H5HL_create(f, /*size_hint=*/0, &empty_addr), 0);
  }

  // Flush triggers prefix & datablock serialize + image_len + AFTER_FLUSH /
  // ENTRY_CLEANED notifications.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("h5hlcache_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = GetH5File(reopened.get());
  ASSERT_NE(rf, nullptr);

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // Cold-load contiguous heap, full contiguous heap, split heap, and empty
    // heap (via H5HL_get_size, which deserializes the zero-sized prefix
    // without touching H5AC_LHEAP_DBLK).
    H5HL_t* h_single = H5HL_protect(rf, single_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(h_single, nullptr);
    EXPECT_TRUE(h_single->single_cache_obj);
    EXPECT_STREQ(static_cast<const char*>(H5HL_offset_into(h_single, off_s0)),
                 "alpha_item_0123");
    EXPECT_STREQ(static_cast<const char*>(H5HL_offset_into(h_single, off_s2)),
                 "gamma_item_0123");
    ASSERT_GE(H5HL_unprotect(h_single), 0);

    H5HL_t* h_full = H5HL_protect(rf, full_single_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(h_full, nullptr);
    EXPECT_EQ(h_full->freelist, nullptr);
    EXPECT_EQ(h_full->free_block, H5HL_FREE_NULL);
    ASSERT_GE(H5HL_unprotect(h_full), 0);

    H5HL_t* h_split = H5HL_protect(rf, split_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(h_split, nullptr);
    EXPECT_FALSE(h_split->single_cache_obj);
    EXPECT_STREQ(
        static_cast<const char*>(H5HL_offset_into(h_split, off_split_0)),
        "alpha_item_0123");
    EXPECT_STREQ(
        static_cast<const char*>(H5HL_offset_into(h_split, off_split_2)),
        "gamma_item_0123");
    ASSERT_GE(H5HL_unprotect(h_split), 0);

    size_t empty_dblk_sz = 999;
    ASSERT_GE(H5HL_get_size(rf, empty_addr, &empty_dblk_sz), 0);
    EXPECT_EQ(empty_dblk_sz, 0u);

    // Evict all entries -> BEFORE_EVICT + free_icr on prefix and datablock.
    ASSERT_GE(H5AC_evict(rf), 0);
  }
}

TEST(H5HLcacheTest, DirectCallbacksRoundTripGapPaddingAndRetainedImage) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), /*sizeof_addr=*/4, /*sizeof_size=*/8), 0);

  ScopedFile file =
      CreateInMemoryFile("h5hlcache_direct.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const size_t sizeof_size = H5F_SIZEOF_SIZE(f);
  const size_t sizeof_addr = H5F_SIZEOF_ADDR(f);
  const size_t prfx_size = H5HL_SIZEOF_HDR(f);
  ASSERT_EQ(sizeof_size, 8u);
  ASSERT_EQ(sizeof_addr, 4u);
  ASSERT_EQ(prfx_size, 32u);

  // 1. get_initial_load_size returns H5HL_SPEC_READ_SIZE (512).
  H5HL_cache_prfx_ud_t udata{};
  udata.sizeof_size = sizeof_size;
  udata.sizeof_addr = sizeof_addr;
  udata.prfx_addr = 0x1000;
  udata.sizeof_prfx = prfx_size;

  size_t init_len = 0;
  ASSERT_GE(H5AC_LHEAP_PRFX[0].get_initial_load_size(&udata, &init_len), 0);
  EXPECT_EQ(init_len, 512u);

  // 2. Construct an in-memory contiguous heap with 2 free blocks, serialize it
  // into a 0xAA-padded buffer, verify gap zeroing and trailing zeroing.
  constexpr size_t kDblkSize = 64;
  H5HL_t* heap = H5HL__new(sizeof_size, sizeof_addr, prfx_size);
  ASSERT_NE(heap, nullptr);
  heap->single_cache_obj = true;
  heap->prfx_addr = 0x1000;
  heap->dblk_addr = 0x1000 + prfx_size;
  heap->dblk_size = kDblkSize;
  heap->dblk_image = H5FL_BLK_CALLOC(lheap_chunk, kDblkSize);
  ASSERT_NE(heap->dblk_image, nullptr);
  std::memcpy(heap->dblk_image, "hello", 6);

  auto* fl0 = H5FL_MALLOC(H5HL_free_t);
  auto* fl1 = H5FL_MALLOC(H5HL_free_t);
  ASSERT_NE(fl0, nullptr);
  ASSERT_NE(fl1, nullptr);
  fl0->offset = 16;
  fl0->size = 16;
  fl0->prev = nullptr;
  fl0->next = fl1;
  fl1->offset = 40;
  fl1->size = 24;
  fl1->prev = fl0;
  fl1->next = nullptr;
  heap->freelist = fl0;

  H5HL_prfx_t* prfx = H5HL__prfx_new(heap);
  ASSERT_NE(prfx, nullptr);
  prfx->cache_info.type = H5AC_LHEAP_PRFX;
  prfx->cache_info.addr = heap->prfx_addr;
  absl::Cleanup free_prfx = [&] {
    if (prfx != nullptr) {
      H5AC_LHEAP_PRFX[0].free_icr(prfx);
    }
  };

  size_t img_len = 0;
  ASSERT_GE(H5AC_LHEAP_PRFX[0].image_len(prfx, &img_len), 0);
  EXPECT_EQ(img_len, prfx_size + kDblkSize);

  std::vector<uint8_t> buf(img_len, 0xAA);
  ASSERT_GE(H5AC_LHEAP_PRFX[0].serialize(f, buf.data(), buf.size(), prfx), 0);
  // Bytes [28, 32) are the 4-byte alignment gap between header and dblk.
  EXPECT_THAT(std::vector<uint8_t>(buf.begin() + 28, buf.begin() + 32),
              Each(Eq(0)));

  // Check get_final_load_size & deserialize round-trip on the serialized buffer.
  size_t final_len = buf.size();
  ASSERT_GE(H5AC_LHEAP_PRFX[0].get_final_load_size(buf.data(), buf.size(),
                                                   &udata, &final_len),
            0);
  EXPECT_EQ(final_len, prfx_size + kDblkSize);

  bool dirty = false;
  auto* decoded_prfx = static_cast<H5HL_prfx_t*>(
      H5AC_LHEAP_PRFX[0].deserialize(buf.data(), buf.size(), &udata, &dirty));
  ASSERT_NE(decoded_prfx, nullptr);
  decoded_prfx->cache_info.type = H5AC_LHEAP_PRFX;
  decoded_prfx->cache_info.addr = udata.prfx_addr;
  absl::Cleanup free_decoded_prfx = [&] {
    if (decoded_prfx != nullptr) {
      H5AC_LHEAP_PRFX[0].free_icr(decoded_prfx);
    }
  };
  EXPECT_TRUE(decoded_prfx->heap->single_cache_obj);
  EXPECT_EQ(decoded_prfx->heap->dblk_size, kDblkSize);
  ASSERT_NE(decoded_prfx->heap->freelist, nullptr);
  EXPECT_EQ(decoded_prfx->heap->freelist->offset, 16u);
  ASSERT_NE(decoded_prfx->heap->freelist->next, nullptr);
  EXPECT_EQ(decoded_prfx->heap->freelist->next->offset, 40u);

  // 3. Also test H5AC_LHEAP_DBLK callbacks directly, including:
  // - serialize with empty freelist (`H5HL_FREE_NULL`),
  // - deserialize when `heap->dblk_image != nullptr` (retained image),
  // - notify for all handled actions plus invalid action error.
  H5HL_t* split_heap = H5HL__new(sizeof_size, sizeof_addr, prfx_size);
  ASSERT_NE(split_heap, nullptr);
  split_heap->single_cache_obj = false;
  split_heap->prfx_addr = 0x2000;
  split_heap->dblk_addr = 0x5000;
  split_heap->dblk_size = 64;
  split_heap->dblk_image = H5FL_BLK_CALLOC(lheap_chunk, 64);
  ASSERT_NE(split_heap->dblk_image, nullptr);
  std::memcpy(split_heap->dblk_image, "retained", 9);

  H5HL_prfx_t* split_prfx = H5HL__prfx_new(split_heap);
  ASSERT_NE(split_prfx, nullptr);
  split_prfx->cache_info.type = H5AC_LHEAP_PRFX;
  split_prfx->cache_info.addr = split_heap->prfx_addr;
  absl::Cleanup free_split_prfx = [&] {
    if (split_prfx != nullptr) {
      H5AC_LHEAP_PRFX[0].free_icr(split_prfx);
    }
  };

  // Serialize split prefix into padded buffer to check trailing zero-clear.
  size_t split_prfx_len = 0;
  ASSERT_GE(H5AC_LHEAP_PRFX[0].image_len(split_prfx, &split_prfx_len), 0);
  EXPECT_EQ(split_prfx_len, prfx_size);
  std::vector<uint8_t> split_prfx_buf(split_prfx_len, 0xBB);
  ASSERT_GE(H5AC_LHEAP_PRFX[0].serialize(f, split_prfx_buf.data(),
                                         split_prfx_buf.size(), split_prfx),
            0);
  EXPECT_THAT(std::vector<uint8_t>(split_prfx_buf.begin() + 28,
                                   split_prfx_buf.end()),
              Each(Eq(0)));

  size_t dblk_init_len = 0;
  ASSERT_GE(H5AC_LHEAP_DBLK[0].get_initial_load_size(split_heap,
                                                     &dblk_init_len),
            0);
  EXPECT_EQ(dblk_init_len, 64u);

  // Deserialize dblk when `split_heap->dblk_image != nullptr`: skips image
  // allocation/copy and preserves the retained in-memory buffer.
  std::vector<uint8_t> dummy_disk(64, 0xCC);
  auto* dblk = static_cast<H5HL_dblk_t*>(H5AC_LHEAP_DBLK[0].deserialize(
      dummy_disk.data(), dummy_disk.size(), split_heap, &dirty));
  ASSERT_NE(dblk, nullptr);
  dblk->cache_info.type = H5AC_LHEAP_DBLK;
  dblk->cache_info.addr = split_heap->dblk_addr;
  absl::Cleanup free_dblk = [&] {
    if (dblk != nullptr) {
      H5AC_LHEAP_DBLK[0].free_icr(dblk);
    }
  };
  EXPECT_EQ(std::memcmp(split_heap->dblk_image, "retained", 9), 0);

  size_t dblk_img_len = 0;
  ASSERT_GE(H5AC_LHEAP_DBLK[0].image_len(dblk, &dblk_img_len), 0);
  EXPECT_EQ(dblk_img_len, 64u);

  std::vector<uint8_t> dblk_out(64, 0);
  ASSERT_GE(H5AC_LHEAP_DBLK[0].serialize(f, dblk_out.data(), dblk_out.size(),
                                         dblk),
            0);
  EXPECT_EQ(std::memcmp(dblk_out.data(), "retained", 9), 0);

  for (H5C_notify_action_t action : {
           H5AC_NOTIFY_ACTION_AFTER_INSERT,
           H5AC_NOTIFY_ACTION_AFTER_FLUSH,
           H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
           H5AC_NOTIFY_ACTION_ENTRY_CLEANED,
           H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
           H5AC_NOTIFY_ACTION_CHILD_CLEANED,
           H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
           H5AC_NOTIFY_ACTION_CHILD_SERIALIZED,
       }) {
    EXPECT_GE(H5AC_LHEAP_DBLK[0].notify(action, dblk), 0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5AC_LHEAP_DBLK[0].notify(static_cast<H5C_notify_action_t>(999), dblk),
        0);
  }
}

TEST(H5HLcacheTest, DeserializeRejectsTruncatedAndCorruptBuffers) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kSizeofSize = 8;
  constexpr size_t kSizeofAddr = 8;
  constexpr size_t kPrfxSize = 32;
  constexpr size_t kDblkSize = 64;
  constexpr haddr_t kPrfxAddr = 0x1000;
  constexpr haddr_t kDblkAddr = kPrfxAddr + kPrfxSize;

  H5HL_cache_prfx_ud_t udata{};
  udata.sizeof_size = kSizeofSize;
  udata.sizeof_addr = kSizeofAddr;
  udata.prfx_addr = kPrfxAddr;
  udata.sizeof_prfx = kPrfxSize;

  const std::vector<uint8_t> valid =
      BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, kDblkAddr,
                       kDblkSize, H5HL_FREE_NULL, kPrfxSize + kDblkSize);

  ScopedErrorSilence silence;
  bool dirty = false;

  auto expect_header_rejected = [&](const std::vector<uint8_t>& buf) {
    size_t actual_len = buf.size();
    EXPECT_LT(H5AC_LHEAP_PRFX[0].get_final_load_size(buf.data(), buf.size(),
                                                     &udata, &actual_len),
              0);
    EXPECT_EQ(
        H5AC_LHEAP_PRFX[0].deserialize(buf.data(), buf.size(), &udata, &dirty),
        nullptr);
  };

  // 1. Truncated magic (len < 4, >= 1 to avoid UB in image + len - 1).
  expect_header_rejected(std::vector<uint8_t>(valid.begin(), valid.begin() + 3));

  // 2. Bad signature.
  {
    std::vector<uint8_t> bad = valid;
    bad[0] = 'X';
    expect_header_rejected(bad);
  }

  // 3. Truncated version.
  expect_header_rejected(std::vector<uint8_t>(valid.begin(), valid.begin() + 4));

  // 4. Unsupported version (!= 0).
  {
    std::vector<uint8_t> bad = valid;
    bad[4] = 2;
    expect_header_rejected(bad);
  }

  // 5. Truncated reserved bytes.
  expect_header_rejected(std::vector<uint8_t>(valid.begin(), valid.begin() + 7));

  // 6. Truncated dblk_size.
  expect_header_rejected(
      std::vector<uint8_t>(valid.begin(), valid.begin() + 8 + kSizeofSize - 1));

  // 7. Truncated free_block.
  expect_header_rejected(std::vector<uint8_t>(
      valid.begin(), valid.begin() + 8 + 2 * kSizeofSize - 1));

  // 8. Out-of-range free_block (>= dblk_size and != H5HL_FREE_NULL).
  {
    std::vector<uint8_t> bad =
        BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, kDblkAddr,
                         kDblkSize, kDblkSize, kPrfxSize + kDblkSize);
    expect_header_rejected(bad);
  }

  // 9. Truncated dblk_addr.
  expect_header_rejected(std::vector<uint8_t>(
      valid.begin(), valid.begin() + 8 + 2 * kSizeofSize + kSizeofAddr - 1));

  // 10. Undefined dblk_addr (HADDR_UNDEF).
  {
    std::vector<uint8_t> bad =
        BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, HADDR_UNDEF,
                         kDblkSize, H5HL_FREE_NULL, kPrfxSize + kDblkSize);
    expect_header_rejected(bad);
  }

  // 11. Truncated contiguous data block (after header succeeds and prfx is
  // allocated).
  EXPECT_EQ(H5AC_LHEAP_PRFX[0].deserialize(valid.data(),
                                           kPrfxSize + kDblkSize - 1, &udata,
                                           &dirty),
            nullptr);

  // 12. Contiguous data block with corrupt free list entries:
  // (a) next free block == 0 ("free block size is zero?").
  {
    std::vector<uint8_t> bad =
        BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, kDblkAddr,
                         kDblkSize, /*free_block=*/0, kPrfxSize + kDblkSize);
    EncodeUint64LE(bad.data() + kPrfxSize, 0, kSizeofSize);
    EncodeUint64LE(bad.data() + kPrfxSize + kSizeofSize, 16, kSizeofSize);
    EXPECT_EQ(
        H5AC_LHEAP_PRFX[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
        nullptr);
  }

  // (b) free block size exceeds dblk_size (`fl->offset + fl->size >
  // dblk_size`).
  {
    std::vector<uint8_t> bad =
        BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, kDblkAddr,
                         kDblkSize, /*free_block=*/0, kPrfxSize + kDblkSize);
    EncodeUint64LE(bad.data() + kPrfxSize, H5HL_FREE_NULL, kSizeofSize);
    EncodeUint64LE(bad.data() + kPrfxSize + kSizeofSize, kDblkSize + 16,
                   kSizeofSize);
    EXPECT_EQ(
        H5AC_LHEAP_PRFX[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
        nullptr);
  }

  // (c) second free block pointer overflows `UINT64_MAX - 2 * sizeof_size`.
  {
    std::vector<uint8_t> bad =
        BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, kDblkAddr,
                         kDblkSize, /*free_block=*/0, kPrfxSize + kDblkSize);
    EncodeUint64LE(bad.data() + kPrfxSize, UINT64_MAX, kSizeofSize);
    EncodeUint64LE(bad.data() + kPrfxSize + kSizeofSize, 16, kSizeofSize);
    EXPECT_EQ(
        H5AC_LHEAP_PRFX[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
        nullptr);
  }

  // (d) second free block offset + 2*sizeof_size exceeds dblk_size.
  {
    std::vector<uint8_t> bad =
        BuildPrefixImage(kSizeofSize, kSizeofAddr, kPrfxSize, kDblkAddr,
                         kDblkSize, /*free_block=*/0, kPrfxSize + kDblkSize);
    EncodeUint64LE(bad.data() + kPrfxSize, kDblkSize - 4, kSizeofSize);
    EncodeUint64LE(bad.data() + kPrfxSize + kSizeofSize, 16, kSizeofSize);
    EXPECT_EQ(
        H5AC_LHEAP_PRFX[0].deserialize(bad.data(), bad.size(), &udata, &dirty),
        nullptr);
  }

  // 13. H5AC_LHEAP_DBLK deserialize with corrupt free list cleans up dblk.
  {
    H5HL_t* heap = H5HL__new(kSizeofSize, kSizeofAddr, kPrfxSize);
    ASSERT_NE(heap, nullptr);
    heap->single_cache_obj = false;
    heap->dblk_size = kDblkSize;
    heap->free_block = 0;
    ASSERT_GE(H5HL__inc_rc(heap), 0);
    absl::Cleanup dec_heap = [&] { H5HL__dec_rc(heap); };

    std::vector<uint8_t> bad_dblk(kDblkSize, 0);
    EncodeUint64LE(bad_dblk.data(), 0, kSizeofSize);
    EXPECT_EQ(H5AC_LHEAP_DBLK[0].deserialize(bad_dblk.data(), bad_dblk.size(),
                                             heap, &dirty),
              nullptr);
    EXPECT_EQ(heap->dblk, nullptr);
  }
}

}  // namespace
}  // namespace hdf5
