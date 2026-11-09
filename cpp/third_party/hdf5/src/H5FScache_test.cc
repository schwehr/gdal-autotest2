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
// Tests free-space metadata cache callbacks for header (H5AC_FSPACE_HDR) and
// serialized section info (H5AC_FSPACE_SINFO), including initial load size,
// image length, checksum verification, pre-serialization (tmp relocation and
// self-referential FSM growth), serialization/deserialization, corruption and
// callback error paths, cache notifications, and ICR freeing
// (third_party/hdf5/src/H5FScache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FScache.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FS_t and H5FS_sinfo_t) and H5C_t. Define NDEBUG around
// internal headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FSCACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#define H5FS_FRIEND
#define H5MF_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5FSpkg.h"
#pragma push_macro("try")
#undef try
#define try hdf5_c_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FSCACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSCACHE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

struct CacheClassControl {
  bool fail_init = false;
  bool fail_term = false;
  bool fail_serialize = false;
  bool fail_deserialize = false;
  bool fail_add = false;
  bool set_no_add_flag = false;
};

struct TestSection {
  H5FS_section_info_t base{};
  uint32_t payload = 0;
  bool fail_on_add = false;
};

TestSection g_dummy_no_add_section{};

herr_t InitCacheTestClass(H5FS_section_class_t* /*cls*/, void* udata) {
  auto* ctrl = static_cast<CacheClassControl*>(udata);
  if (ctrl != nullptr && ctrl->fail_init) {
    return -1;
  }
  return 0;
}

herr_t TermCacheTestClass(H5FS_section_class_t* cls) {
  auto* ctrl = static_cast<CacheClassControl*>(cls->cls_private);
  if (ctrl != nullptr && ctrl->fail_term) {
    return -1;
  }
  return 0;
}

herr_t AddCacheTestSection(H5FS_section_info_t** sect, unsigned* /*flags*/,
                           void* /*op_data*/) {
  auto* ts = reinterpret_cast<TestSection*>(*sect);
  if (ts->fail_on_add) {
    delete ts;
    *sect = nullptr;
    return -1;
  }
  return 0;
}

herr_t SerializeCacheTestSection(const H5FS_section_class_t* cls,
                                 const H5FS_section_info_t* sect,
                                 uint8_t* buf) {
  auto* ctrl = static_cast<CacheClassControl*>(cls->cls_private);
  if (ctrl != nullptr && ctrl->fail_serialize) {
    return -1;
  }
  const auto* ts = reinterpret_cast<const TestSection*>(sect);
  for (int b = 0; b < 4; ++b) {
    buf[b] = static_cast<uint8_t>((ts->payload >> (b * 8)) & 0xffU);
  }
  return 0;
}

H5FS_section_info_t* DeserializeCacheTestSection(
    const H5FS_section_class_t* cls, const uint8_t* buf, haddr_t sect_addr,
    hsize_t sect_size, unsigned* des_flags) {
  auto* ctrl = static_cast<CacheClassControl*>(cls->cls_private);
  if (ctrl != nullptr && ctrl->fail_deserialize) {
    return nullptr;
  }
  if (ctrl != nullptr && ctrl->set_no_add_flag) {
    *des_flags |= H5FS_DESERIALIZE_NO_ADD;
    return &g_dummy_no_add_section.base;
  }
  auto* ts = new TestSection();
  ts->base.addr = sect_addr;
  ts->base.size = sect_size;
  ts->base.type = cls->type;
  ts->base.state = H5FS_SECT_SERIALIZED;
  ts->fail_on_add = (ctrl != nullptr && ctrl->fail_add);
  if (cls->serial_size == 4) {
    uint32_t v = 0;
    for (int b = 0; b < 4; ++b) {
      v |= static_cast<uint32_t>(buf[b]) << (b * 8);
    }
    ts->payload = v;
  }
  return &ts->base;
}

herr_t FreeCacheTestSection(H5FS_section_info_t* sect) {
  delete reinterpret_cast<TestSection*>(sect);
  return 0;
}

H5FS_section_class_t MakeSectionClass(unsigned type, size_t serial_size,
                                      unsigned flags, CacheClassControl* ctrl,
                                      bool use_null_serialize = false) {
  return H5FS_section_class_t{
      /*type=*/type,
      /*serial_size=*/serial_size,
      /*flags=*/flags,
      /*cls_private=*/ctrl,
      /*init_cls=*/InitCacheTestClass,
      /*term_cls=*/TermCacheTestClass,
      /*add=*/AddCacheTestSection,
      /*serialize=*/
      (!use_null_serialize && serial_size > 0) ? SerializeCacheTestSection
                                               : nullptr,
      /*deserialize=*/DeserializeCacheTestSection,
      /*can_merge=*/nullptr,
      /*merge=*/nullptr,
      /*can_shrink=*/nullptr,
      /*shrink=*/nullptr,
      /*free=*/FreeCacheTestSection,
      /*valid=*/nullptr,
      /*split=*/nullptr,
      /*debug=*/nullptr,
  };
}

TestSection* MakeSection(haddr_t addr, hsize_t size, unsigned type = 0,
                         uint32_t payload = 0) {
  auto* ts = new TestSection();
  ts->base.addr = addr;
  ts->base.size = size;
  ts->base.type = type;
  ts->base.state = H5FS_SECT_LIVE;
  ts->payload = payload;
  return ts;
}

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__FREESPACE_TAG) {
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

H5FS_create_t MakeFsCreateParams(
    H5FS_client_t client = H5FS_CLIENT_FHEAP_ID) {
  H5FS_create_t params{};
  params.client = client;
  params.shrink_percent = 75;
  params.expand_percent = 125;
  params.max_sect_addr = 32;
  params.max_sect_size = 0x10000;
  return params;
}

TEST(H5FScacheTest, HeaderCacheCallbacksRoundTripNotifyErrorsAndFreeIcr) {
  CacheClassControl ctrl;
  ScopedFile file = CreateInMemoryFile("h5fscache_hdr.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/4, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};
  const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FHEAP_ID);

  haddr_t fs_addr = HADDR_UNDEF;
  H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
  ASSERT_NE(fs, nullptr);
  absl::Cleanup close_fs = [&] {
    if (fs != nullptr) {
      H5FS_close(f, fs);
    }
  };
  fs->tot_space = 1024;
  fs->tot_sect_count = 3;
  fs->serial_sect_count = 2;
  fs->ghost_sect_count = 1;
  fs->sect_addr = 0x2400;
  fs->sect_size = 64;
  fs->alloc_sect_size = 64;

  // 1. Initial load size and image_len callbacks.
  H5FS_hdr_cache_ud_t udata{};
  udata.f = f;
  udata.nclasses = 1;
  udata.classes = classes;
  udata.cls_init_udata = &ctrl;
  udata.addr = fs_addr;

  size_t init_len = 0;
  ASSERT_GE(H5AC_FSPACE_HDR[0].get_initial_load_size(&udata, &init_len), 0);
  EXPECT_EQ(init_len, H5FS_HEADER_SIZE(f));

  size_t img_len = 0;
  ASSERT_GE(H5AC_FSPACE_HDR[0].image_len(fs, &img_len), 0);
  EXPECT_EQ(img_len, fs->hdr_size);

  // 2. Serialize and verify checksum (valid, bit-flipped, truncated).
  std::vector<uint8_t> image(img_len, 0);
  ASSERT_GE(H5AC_FSPACE_HDR[0].serialize(f, image.data(), img_len, fs), 0);
  EXPECT_EQ(H5AC_FSPACE_HDR[0].verify_chksum(image.data(), img_len, &udata), 1);

  {
    std::vector<uint8_t> flipped = image;
    flipped[0] ^= 0x01U;
    EXPECT_EQ(H5AC_FSPACE_HDR[0].verify_chksum(flipped.data(), img_len, &udata),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_FSPACE_HDR[0].verify_chksum(image.data(), 2, &udata), 0);
  }

  // 3. Deserialize valid image and verify all decoded fields.
  bool dirty = false;
  void* raw_hdr =
      H5AC_FSPACE_HDR[0].deserialize(image.data(), img_len, &udata, &dirty);
  ASSERT_NE(raw_hdr, nullptr);
  auto* deser_hdr = static_cast<H5FS_t*>(raw_hdr);
  EXPECT_EQ(deser_hdr->client, H5FS_CLIENT_FHEAP_ID);
  EXPECT_EQ(deser_hdr->tot_space, 1024u);
  EXPECT_EQ(deser_hdr->tot_sect_count, 3u);
  EXPECT_EQ(deser_hdr->serial_sect_count, 2u);
  EXPECT_EQ(deser_hdr->ghost_sect_count, 1u);
  EXPECT_EQ(deser_hdr->shrink_percent, 75u);
  EXPECT_EQ(deser_hdr->expand_percent, 125u);
  EXPECT_EQ(deser_hdr->max_sect_addr, 32u);
  EXPECT_EQ(deser_hdr->max_sect_size, 0x10000u);
  EXPECT_EQ(deser_hdr->sect_addr, 0x2400u);
  EXPECT_EQ(deser_hdr->sect_size, 64u);
  EXPECT_EQ(deser_hdr->alloc_sect_size, 64u);

  // Also test free_icr error propagation (fail_term = true, then retry clean).
  {
    ScopedErrorSilence silence;
    ctrl.fail_term = true;
    EXPECT_LT(H5AC_FSPACE_HDR[0].free_icr(deser_hdr), 0);
    ctrl.fail_term = false;
  }
  EXPECT_GE(H5AC_FSPACE_HDR[0].free_icr(deser_hdr), 0);

  // Also deserialize with nclasses == 0 in udata (delete-style load).
  {
    H5FS_hdr_cache_ud_t zero_udata = udata;
    zero_udata.nclasses = 0;
    zero_udata.classes = nullptr;
    void* zero_raw = H5AC_FSPACE_HDR[0].deserialize(image.data(), img_len,
                                                    &zero_udata, &dirty);
    ASSERT_NE(zero_raw, nullptr);
    EXPECT_GE(H5AC_FSPACE_HDR[0].free_icr(zero_raw), 0);
  }

  // 4. Corruption & init_cls failure paths in header deserialize.
  {
    ScopedErrorSilence silence;

    ctrl.fail_init = true;
    EXPECT_EQ(
        H5AC_FSPACE_HDR[0].deserialize(image.data(), img_len, &udata, &dirty),
        nullptr);
    ctrl.fail_init = false;

    std::vector<uint8_t> bad_magic = image;
    bad_magic[0] = 'X';
    EXPECT_EQ(H5AC_FSPACE_HDR[0].deserialize(bad_magic.data(), img_len, &udata,
                                             &dirty),
              nullptr);

    std::vector<uint8_t> bad_ver = image;
    bad_ver[H5_SIZEOF_MAGIC] = 9;
    EXPECT_EQ(
        H5AC_FSPACE_HDR[0].deserialize(bad_ver.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_client = image;
    bad_client[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(H5FS_NUM_CLIENT_ID);
    EXPECT_EQ(H5AC_FSPACE_HDR[0].deserialize(bad_client.data(), img_len, &udata,
                                             &dirty),
              nullptr);

    // Header nclasses field sits after magic(4) + ver(1) + client(1) +
    // 4*sizeof_size(32) = byte offset 38 (for default 8-byte size). Encode 5.
    std::vector<uint8_t> bad_nclasses = image;
    const size_t ncls_off = H5_SIZEOF_MAGIC + 2 + 4 * H5F_SIZEOF_SIZE(f);
    bad_nclasses[ncls_off] = 5;
    bad_nclasses[ncls_off + 1] = 0;
    EXPECT_EQ(H5AC_FSPACE_HDR[0].deserialize(bad_nclasses.data(), img_len,
                                             &udata, &dirty),
              nullptr);
  }

  // 5. Reset synthetic counts before closing so H5FS_close sanity stays valid.
  fs->tot_space = 0;
  fs->tot_sect_count = 0;
  fs->serial_sect_count = 0;
  fs->ghost_sect_count = 0;
  fs->sect_addr = HADDR_UNDEF;
  fs->sect_size = 0;
  fs->alloc_sect_size = 0;

  // 6. Exercise header notify actions across all enum cases + error paths.
  for (H5AC_notify_action_t action :
       {H5AC_NOTIFY_ACTION_AFTER_INSERT, H5AC_NOTIFY_ACTION_AFTER_LOAD,
        H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_CLEANED,
        H5AC_NOTIFY_ACTION_CHILD_DIRTIED, H5AC_NOTIFY_ACTION_CHILD_CLEANED,
        H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
        H5AC_NOTIFY_ACTION_CHILD_SERIALIZED, H5AC_NOTIFY_ACTION_BEFORE_EVICT}) {
    EXPECT_GE(H5AC_FSPACE_HDR[0].notify(action, fs), 0);
  }
  EXPECT_GE(H5AC_FSPACE_HDR[0].notify(H5AC_NOTIFY_ACTION_ENTRY_DIRTIED, fs), 0);

  // Trigger H5AC_unsettle_entry_ring error via settled RDFSM ring during
  // simulated close warning, restoring all cache flags afterward.
  {
    const H5AC_ring_t saved_ring = fs->cache_info.ring;
    const bool saved_settled = f->shared->cache->rdfsm_settled;
    const bool saved_close_warn = f->shared->cache->close_warning_received;
    auto restore_cache = absl::MakeCleanup([&] {
      fs->cache_info.ring = saved_ring;
      f->shared->cache->rdfsm_settled = saved_settled;
      f->shared->cache->close_warning_received = saved_close_warn;
    });
    fs->cache_info.ring = H5C_RING_RDFSM;
    f->shared->cache->rdfsm_settled = true;
    f->shared->cache->close_warning_received = true;
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_FSPACE_HDR[0].notify(H5AC_NOTIFY_ACTION_ENTRY_DIRTIED, fs),
              0);
  }

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_FSPACE_HDR[0].notify(static_cast<H5AC_notify_action_t>(999),
                                        fs),
              0);
  }

  ASSERT_GE(H5FS_close(f, fs), 0);
  fs = nullptr;
  EXPECT_GE(H5FS_delete(f, fs_addr), 0);
}

TEST(H5FScacheTest, HeaderPreSerializeCasesAndErrorPaths) {
  ScopedFile file = CreateInMemoryFile("h5fscache_hdr_preser.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  CacheClassControl ctrl;
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/4, /*flags=*/0, &ctrl);
  const H5FS_section_class_t cls1_ghost = MakeSectionClass(
      /*type=*/1, /*serial_size=*/0, H5FS_CLS_GHOST_OBJ, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0, &cls1_ghost};
  const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FHEAP_ID);

  haddr_t new_addr = HADDR_UNDEF;
  size_t new_len = 0;
  unsigned flags = 0;

  // 1. sinfo == nullptr && !defined(sect_addr).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);
    EXPECT_EQ(flags, 0u);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 2. sinfo != nullptr && serial_sect_count == 0 (ghost only, line 572 else).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x100, 64, 1)->base, 0, nullptr,
                            nullptr),
              0);
    EXPECT_NE(fs->sinfo, nullptr);
    EXPECT_EQ(fs->serial_sect_count, 0u);
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);
    EXPECT_EQ(flags, 0u);
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 3. Case 1 with sinfo != nullptr, serial_sect_count > 0, !defined(sect_addr):
  // error paths (get_entry_ring fail + H5MF_alloc fail) and normal alloc+insert.
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x200, 64, 0, 1)->base, 0,
                            nullptr, nullptr),
              0);

    {
      ScopedErrorSilence silence;
      // Bad header address fails H5AC_get_entry_ring.
      EXPECT_LT(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, /*addr=*/0xdeadbeefULL,
                                                 fs->hdr_size, &new_addr,
                                                 &new_len, &flags),
                0);

      // Allocation failure in Case 1.
      ScopedBlockAllocFailure block_alloc(f);
      EXPECT_LT(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                                 &new_addr, &new_len, &flags),
                0);
    }

    // Success path: allocates real sect_addr, inserts sinfo, clears fs->sinfo.
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);
    EXPECT_EQ(fs->sinfo, nullptr);
    EXPECT_TRUE(H5_addr_defined(fs->sect_addr));
    EXPECT_FALSE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    // Re-running pre_serialize hits sinfo == nullptr && real sect_addr branch.
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);

    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 4. Case 2 and Case 3 with sinfo != nullptr (locked/protected in cache) AND
  // sinfo == nullptr with tmp sect_addr (plus H5MF_alloc error in both).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x300, 64, 0, 2)->base, 0,
                            nullptr, nullptr),
              0);
    // H5FS_close inserts sinfo at tmp_addr (use_tmp_space == true).
    ASSERT_GE(H5FS_close(f, fs), 0);

    fs = H5FS_open(f, fs_addr, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    // Error when sinfo == nullptr and tmp sect_addr fails H5MF_alloc.
    {
      ScopedErrorSilence silence;
      ScopedBlockAllocFailure block_alloc(f);
      EXPECT_LT(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                                 &new_addr, &new_len, &flags),
                0);
    }

    // Success when sinfo == nullptr and tmp sect_addr moves to real addr.
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);
    EXPECT_FALSE(H5F_IS_TMP_ADDR(f, fs->sect_addr));
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 5. Case 2 (sinfo != nullptr, tmp sect_addr) & Case 3 (sinfo != nullptr, real
  // sect_addr) while sinfo is protected in cache.
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x400, 64, 0, 3)->base, 0,
                            nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_close(f, fs), 0);

    fs = H5FS_open(f, fs_addr, 2, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    H5FS_sinfo_cache_ud_t sinfo_ud{f, fs};
    auto* sinfo = static_cast<H5FS_sinfo_t*>(H5AC_protect(
        f, H5AC_FSPACE_SINFO, fs->sect_addr, &sinfo_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(sinfo, nullptr);
    fs->sinfo = sinfo;
    fs->sinfo_protected = true;
    absl::Cleanup unprot_sinfo = [&] {
      if (fs->sinfo != nullptr) {
        H5AC_unprotect(f, H5AC_FSPACE_SINFO, fs->sect_addr, fs->sinfo,
                       H5AC__NO_FLAGS_SET);
        fs->sinfo = nullptr;
        fs->sinfo_protected = false;
      }
    };

    {
      ScopedErrorSilence silence;
      ScopedBlockAllocFailure block_alloc(f);
      EXPECT_LT(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                                 &new_addr, &new_len, &flags),
                0);
    }

    // Case 2 relocates protected sinfo from tmp_addr to real addr.
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);
    EXPECT_FALSE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    // Case 3 with sinfo != nullptr already at real addr.
    EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fs, fs->addr, fs->hdr_size,
                                               &new_addr, &new_len, &flags),
              0);

    std::move(unprot_sinfo).Invoke();
    EXPECT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }
}

TEST(H5FScacheTest, HeaderPreSerializeSelfReferentialSectSizeGrowth) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                 /*persist=*/true, /*threshold=*/1),
      0);

  ScopedFile file =
      CreateInMemoryFile("h5fscache_self_ref.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx(H5AC__FREESPACE_TAG);
  ASSERT_TRUE(api_ctx.valid());

  H5F_mem_page_t fs_type = H5F_MEM_PAGE_GENERIC;
  H5MF__alloc_to_fs_type(f->shared, H5FD_MEM_FSPACE_SINFO, 256, &fs_type);

  // Start the self-referential MDFSM and allocate its header in H5AC_RING_MDFSM
  // BEFORE creating the test free blocks so H5FS_alloc_hdr does not perturb the
  // two identical-size free sections below.
  {
    H5AC_ring_t orig_ring = H5AC_RING_INV;
    H5AC_set_ring(H5AC_RING_MDFSM, &orig_ring);
    auto restore_ring =
        absl::MakeCleanup([&] { H5AC_set_ring(orig_ring, nullptr); });
    ASSERT_GE(H5MF__start_fstype(f, fs_type), 0);
    ASSERT_NE(f->shared->fs_man[fs_type], nullptr);
    ASSERT_GE(H5FS_alloc_hdr(f, f->shared->fs_man[fs_type],
                             &f->shared->fs_addr[fs_type]),
              0);
  }

  // Allocate 5 equal metadata blocks so freeing two non-adjacent blocks creates
  // 2 same-sized sections in the self-referential H5FD_MEM_FSPACE_SINFO FSM.
  constexpr hsize_t kBlkSize = 256;
  const haddr_t b0 = H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, kBlkSize);
  const haddr_t b1 = H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, kBlkSize);
  const haddr_t b2 = H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, kBlkSize);
  const haddr_t b3 = H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, kBlkSize);
  const haddr_t b4 = H5MF_alloc(f, H5FD_MEM_FSPACE_SINFO, kBlkSize);
  ASSERT_TRUE(H5_addr_defined(b0));
  ASSERT_TRUE(H5_addr_defined(b4));
  absl::Cleanup free_blocks = [&] {
    H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, b1, kBlkSize);
    H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, b3, kBlkSize);
    H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, b4, kBlkSize);
  };

  ASSERT_GE(H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, b0, kBlkSize), 0);
  ASSERT_GE(H5MF_xfree(f, H5FD_MEM_FSPACE_SINFO, b2, kBlkSize), 0);

  H5FS_t* fsm = f->shared->fs_man[fs_type];
  ASSERT_NE(fsm, nullptr);
  ASSERT_NE(fsm->sinfo, nullptr);
  ASSERT_EQ(fsm->serial_sect_count, 2u);
  ASSERT_EQ(fsm->sinfo->serial_size_count, 1u);
  const hsize_t saved_sect_size = fsm->sect_size;

  // Allocating section info from `fsm` itself splits `b0`, adding a new
  // section size (serial_size_count: 1 -> 2), growing fsm->sect_size and
  // hitting H5FS__cache_hdr_pre_serialize's resize-and-reallocate branch.
  haddr_t new_addr = HADDR_UNDEF;
  size_t new_len = 0;
  unsigned flags = 0;
  EXPECT_GE(H5AC_FSPACE_HDR[0].pre_serialize(f, fsm, fsm->addr, fsm->hdr_size,
                                             &new_addr, &new_len, &flags),
            0);
  EXPECT_GT(fsm->sect_size, saved_sect_size);
  EXPECT_EQ(fsm->sinfo, nullptr);
  EXPECT_TRUE(H5_addr_defined(fsm->sect_addr));
}

TEST(H5FScacheTest, SinfoCallbacksSerializationGapNoAddPreSerializeAndErrors) {
  ScopedFile file = CreateInMemoryFile("h5fscache_sinfo.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  CacheClassControl ctrl;
  // cls0: serializable, 4-byte custom payload.
  // cls1: serializable, serial_size == 0 (null serialize callback branch).
  // cls2: ghost object (not serialized; shares a size node and has a ghost-only
  //       size node).
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/4, /*flags=*/0, &ctrl);
  const H5FS_section_class_t cls1 = MakeSectionClass(
      /*type=*/1, /*serial_size=*/0, /*flags=*/0, &ctrl,
      /*use_null_serialize=*/true);
  const H5FS_section_class_t cls2_ghost = MakeSectionClass(
      /*type=*/2, /*serial_size=*/0, H5FS_CLS_GHOST_OBJ, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0, &cls1, &cls2_ghost};
  const H5FS_create_t cparam = MakeFsCreateParams(H5FS_CLIENT_FHEAP_ID);

  // 1. Round-trip with mixed serializable, null-serialize, shared-size ghost,
  // ghost-only size, same-size multiples, plus trailing gap bytes.
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 3, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    absl::Cleanup close_fs = [&] {
      if (fs != nullptr) {
        H5FS_close(f, fs);
      }
    };

    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x100, 64, 0, 10)->base, 0,
                            nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x300, 64, 1, 0)->base, 0,
                            nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x500, 64, 2, 0)->base, 0,
                            nullptr, nullptr),
              0);  // Ghost in same 64-byte size node as cls0/cls1.
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x700, 128, 2, 0)->base, 0,
                            nullptr, nullptr),
              0);  // Ghost-only 128-byte size node (serial_count == 0).
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x900, 256, 0, 20)->base, 0,
                            nullptr, nullptr),
              0);

    // Pad sect_size by 16 bytes to exercise gap zeroing in serialize and
    // chksum_image skip in deserialize.
    constexpr size_t kGapBytes = 16;
    const size_t exact_len = fs->sect_size;
    const size_t padded_len = exact_len + kGapBytes;
    fs->sect_size = padded_len;
    fs->alloc_sect_size = padded_len;
    fs->sinfo->cache_info.type = H5AC_FSPACE_SINFO;

    H5FS_sinfo_cache_ud_t sinfo_ud{f, fs};
    size_t init_len = 0;
    ASSERT_GE(
        H5AC_FSPACE_SINFO[0].get_initial_load_size(&sinfo_ud, &init_len), 0);
    EXPECT_EQ(init_len, padded_len);

    size_t img_len = 0;
    ASSERT_GE(H5AC_FSPACE_SINFO[0].image_len(fs->sinfo, &img_len), 0);
    EXPECT_EQ(img_len, padded_len);

    std::vector<uint8_t> image(padded_len, 0xAA);
    ASSERT_GE(H5AC_FSPACE_SINFO[0].serialize(f, image.data(), padded_len,
                                             fs->sinfo),
              0);
    // Verify gap bytes before trailing 4-byte checksum were zero-initialized.
    for (size_t i = exact_len - H5FS_SIZEOF_CHKSUM;
         i < padded_len - H5FS_SIZEOF_CHKSUM; ++i) {
      EXPECT_EQ(image[i], 0u);
    }

    EXPECT_EQ(
        H5AC_FSPACE_SINFO[0].verify_chksum(image.data(), padded_len, &sinfo_ud),
        1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x04U;
      EXPECT_EQ(H5AC_FSPACE_SINFO[0].verify_chksum(flipped.data(), padded_len,
                                                   &sinfo_ud),
                0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_FSPACE_SINFO[0].verify_chksum(image.data(), 2, &sinfo_ud),
                0);
    }

    // Serialize failure propagates from cls->serialize through node & sinfo.
    {
      ScopedErrorSilence silence;
      ctrl.fail_serialize = true;
      std::vector<uint8_t> tmp(padded_len, 0);
      EXPECT_LT(
          H5AC_FSPACE_SINFO[0].serialize(f, tmp.data(), padded_len, fs->sinfo),
          0);
      ctrl.fail_serialize = false;
    }

    // Create a clean matching header with sinfo == nullptr to receive the
    // deserialized section info image.
    H5FS_t* target = H5FS_create(f, nullptr, &cparam, 3, classes, &ctrl, 0, 0);
    ASSERT_NE(target, nullptr);
    target->addr = fs_addr;
    target->serial_sect_count = fs->serial_sect_count;
    target->tot_sect_count = fs->serial_sect_count;
    target->tot_space = fs->tot_space;
    target->sect_size = padded_len;
    target->alloc_sect_size = padded_len;

    H5FS_sinfo_cache_ud_t deser_ud{f, target};
    bool dirty = false;
    void* raw_sinfo = H5AC_FSPACE_SINFO[0].deserialize(image.data(), padded_len,
                                                       &deser_ud, &dirty);
    ASSERT_NE(raw_sinfo, nullptr);
    auto* deser_sinfo = static_cast<H5FS_sinfo_t*>(raw_sinfo);
    EXPECT_EQ(target->serial_sect_count, 3u);
    EXPECT_EQ(target->tot_space, 64u + 64u + 256u);
    EXPECT_GE(H5AC_FSPACE_SINFO[0].free_icr(deser_sinfo), 0);

    // Deserialize error paths (must reset target counts & sinfo after failures).
    auto reset_target = [&] {
      target->sinfo = nullptr;
      target->serial_sect_count = fs->serial_sect_count;
      target->tot_sect_count = fs->serial_sect_count;
      target->tot_space = fs->tot_space;
    };
    {
      ScopedErrorSilence silence;

      std::vector<uint8_t> bad_magic = image;
      bad_magic[0] = 'Z';
      EXPECT_EQ(H5AC_FSPACE_SINFO[0].deserialize(bad_magic.data(), padded_len,
                                                 &deser_ud, &dirty),
                nullptr);
      reset_target();

      std::vector<uint8_t> bad_ver = image;
      bad_ver[H5_SIZEOF_MAGIC] = 3;
      EXPECT_EQ(H5AC_FSPACE_SINFO[0].deserialize(bad_ver.data(), padded_len,
                                                 &deser_ud, &dirty),
                nullptr);
      reset_target();

      std::vector<uint8_t> bad_addr = image;
      bad_addr[H5_SIZEOF_MAGIC + 1] ^= 0xffU;
      EXPECT_EQ(H5AC_FSPACE_SINFO[0].deserialize(bad_addr.data(), padded_len,
                                                 &deser_ud, &dirty),
                nullptr);
      reset_target();

      ctrl.fail_deserialize = true;
      EXPECT_EQ(H5AC_FSPACE_SINFO[0].deserialize(image.data(), padded_len,
                                                 &deser_ud, &dirty),
                nullptr);
      ctrl.fail_deserialize = false;
      reset_target();

      ctrl.fail_add = true;
      EXPECT_EQ(H5AC_FSPACE_SINFO[0].deserialize(image.data(), padded_len,
                                                 &deser_ud, &dirty),
                nullptr);
      ctrl.fail_add = false;
      reset_target();
    }

    // Exercise H5FS_DESERIALIZE_NO_ADD flag (skips H5FS_sect_add and hits the
    // image-end loop condition on exact-sized image).
    {
      fs->sect_size = exact_len;
      fs->alloc_sect_size = exact_len;
      std::vector<uint8_t> exact_img(exact_len, 0);
      ASSERT_GE(H5AC_FSPACE_SINFO[0].serialize(f, exact_img.data(), exact_len,
                                               fs->sinfo),
                0);
      target->serial_sect_count = 3;
      target->tot_sect_count = 3;
      target->sect_size = exact_len;
      target->alloc_sect_size = exact_len;
      ctrl.set_no_add_flag = true;
      void* no_add_raw = H5AC_FSPACE_SINFO[0].deserialize(
          exact_img.data(), exact_len, &deser_ud, &dirty);
      ctrl.set_no_add_flag = false;
      ASSERT_NE(no_add_raw, nullptr);
      EXPECT_EQ(target->tot_sect_count, 0u);
      EXPECT_GE(H5AC_FSPACE_SINFO[0].free_icr(no_add_raw), 0);
    }

    target->addr = HADDR_UNDEF;
    target->sect_addr = HADDR_UNDEF;
    target->sect_size = 0;
    target->alloc_sect_size = 0;
    EXPECT_GE(H5FS_close(f, target), 0);

    // Close source fs without serializing padded size (revert to exact).
    fs->sect_size = exact_len;
    fs->alloc_sect_size = 0;
    ASSERT_GE(H5FS_close(f, fs), 0);
    fs = nullptr;
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 2. Empty serializable section deserialize (serial_sect_count == 0).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 3, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    H5FS_sinfo_t* empty_sinfo = H5FS__sinfo_new(f, fs);
    ASSERT_NE(empty_sinfo, nullptr);
    empty_sinfo->cache_info.type = H5AC_FSPACE_SINFO;
    const size_t empty_len = empty_sinfo->sect_prefix_size;
    fs->sect_size = empty_len;
    fs->alloc_sect_size = empty_len;

    std::vector<uint8_t> image(empty_len, 0);
    ASSERT_GE(
        H5AC_FSPACE_SINFO[0].serialize(f, image.data(), empty_len, empty_sinfo),
        0);

    fs->sinfo = nullptr;
    H5FS_sinfo_cache_ud_t ud{f, fs};
    bool dirty = false;
    void* deser =
        H5AC_FSPACE_SINFO[0].deserialize(image.data(), empty_len, &ud, &dirty);
    ASSERT_NE(deser, nullptr);
    EXPECT_GE(H5AC_FSPACE_SINFO[0].free_icr(deser), 0);

    // Clean up empty_sinfo allocated earlier.
    fs->sinfo = empty_sinfo;
    fs->sect_size = 0;
    fs->alloc_sect_size = 0;
    ASSERT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }

  // 3. sinfo pre_serialize (tmp addr relocate, real addr no-op, alloc error).
  {
    haddr_t fs_addr = HADDR_UNDEF;
    H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 3, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x200, 64, 0, 7)->base, 0,
                            nullptr, nullptr),
              0);
    ASSERT_GE(H5FS_close(f, fs), 0);

    fs = H5FS_open(f, fs_addr, 3, classes, &ctrl, 0, 0);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(H5F_IS_TMP_ADDR(f, fs->sect_addr));

    H5FS_sinfo_cache_ud_t sinfo_ud{f, fs};
    auto* sinfo = static_cast<H5FS_sinfo_t*>(H5AC_protect(
        f, H5AC_FSPACE_SINFO, fs->sect_addr, &sinfo_ud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(sinfo, nullptr);
    absl::Cleanup unprot_sinfo = [&] {
      if (sinfo != nullptr) {
        H5AC_unprotect(f, H5AC_FSPACE_SINFO, fs->sect_addr, sinfo,
                       H5AC__NO_FLAGS_SET);
      }
    };

    haddr_t new_addr = HADDR_UNDEF;
    size_t new_len = 0;
    unsigned flags = 0;

    {
      ScopedErrorSilence silence;
      ScopedBlockAllocFailure block_alloc(f);
      EXPECT_LT(H5AC_FSPACE_SINFO[0].pre_serialize(f, sinfo, fs->sect_addr,
                                                   fs->sect_size, &new_addr,
                                                   &new_len, &flags),
                0);
    }

    const haddr_t old_tmp = fs->sect_addr;
    EXPECT_GE(H5AC_FSPACE_SINFO[0].pre_serialize(
                  f, sinfo, fs->sect_addr, fs->sect_size, &new_addr, &new_len,
                  &flags),
              0);
    EXPECT_NE(new_addr, old_tmp);
    EXPECT_EQ(flags, H5C__SERIALIZE_MOVED_FLAG);

    flags = 999;
    EXPECT_GE(H5AC_FSPACE_SINFO[0].pre_serialize(
                  f, sinfo, fs->sect_addr, fs->sect_size, &new_addr, &new_len,
                  &flags),
              0);
    EXPECT_EQ(flags, 0u);

    std::move(unprot_sinfo).Invoke();
    sinfo = nullptr;
    ASSERT_GE(H5FS_close(f, fs), 0);
    EXPECT_GE(H5FS_delete(f, fs_addr), 0);
  }
}

TEST(H5FScacheTest, SinfoNotifySwmrNoOpAndErrorPropagation) {
  ScopedFile file = CreateInMemoryFile("h5fscache_sinfo_notify.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  CacheClassControl ctrl;
  const H5FS_section_class_t cls0 =
      MakeSectionClass(/*type=*/0, /*serial_size=*/0, /*flags=*/0, &ctrl);
  const H5FS_section_class_t* classes[] = {&cls0};
  const H5FS_create_t cparam = MakeFsCreateParams();

  haddr_t fs_addr = HADDR_UNDEF;
  H5FS_t* fs = H5FS_create(f, &fs_addr, &cparam, 1, classes, &ctrl, 0, 0);
  ASSERT_NE(fs, nullptr);
  ASSERT_GE(H5FS_sect_add(f, fs, &MakeSection(0x100, 64)->base, 0, nullptr,
                          nullptr),
            0);
  ASSERT_GE(H5FS_close(f, fs), 0);

  fs = H5FS_open(f, fs_addr, 1, classes, &ctrl, 0, 0);
  ASSERT_NE(fs, nullptr);
  H5FS_sinfo_cache_ud_t sinfo_ud{f, fs};
  auto* sinfo = static_cast<H5FS_sinfo_t*>(H5AC_protect(
      f, H5AC_FSPACE_SINFO, fs->sect_addr, &sinfo_ud, H5AC__READ_ONLY_FLAG));
  ASSERT_NE(sinfo, nullptr);

  // 1. Non-SWMR notify does nothing and returns >= 0.
  EXPECT_FALSE(fs->swmr_write);
  EXPECT_GE(H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, sinfo),
            0);

  // 2. SWMR mode enabled: test no-op actions, unknown action error, and flush
  // dependency create/destroy success and error propagation.
  fs->swmr_write = true;
  auto restore_swmr = absl::MakeCleanup([&] { fs->swmr_write = false; });

  for (H5AC_notify_action_t action :
       {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
        H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
        H5AC_NOTIFY_ACTION_CHILD_CLEANED, H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
        H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
    EXPECT_GE(H5AC_FSPACE_SINFO[0].notify(action, sinfo), 0);
  }

  // AFTER_INSERT creates flush dep (fs is pinned), BEFORE_EVICT destroys it.
  EXPECT_GE(H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, sinfo),
            0);
  EXPECT_GE(H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT, sinfo),
            0);

  // AFTER_LOAD also creates flush dep; verify and clean up via BEFORE_EVICT.
  EXPECT_GE(H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_AFTER_LOAD, sinfo),
            0);
  EXPECT_GE(H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT, sinfo),
            0);

  {
    ScopedErrorSilence silence;
    // Unknown action hits default error branch.
    EXPECT_LT(H5AC_FSPACE_SINFO[0].notify(
                  static_cast<H5AC_notify_action_t>(999), sinfo),
              0);

    // BEFORE_EVICT without an active dependency fails destroy_flush_depend.
    EXPECT_LT(
        H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT, sinfo), 0);

    // AFTER_INSERT with unpinned/unprotected parent fails create_flush_depend.
    const bool saved_pinned = fs->cache_info.is_pinned;
    fs->cache_info.is_pinned = false;
    EXPECT_LT(
        H5AC_FSPACE_SINFO[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, sinfo), 0);
    fs->cache_info.is_pinned = saved_pinned;
  }

  std::move(restore_swmr).Invoke();
  ASSERT_GE(
      H5AC_unprotect(f, H5AC_FSPACE_SINFO, fs->sect_addr, sinfo,
                     H5AC__NO_FLAGS_SET),
      0);
  ASSERT_GE(H5FS_close(f, fs), 0);
  EXPECT_GE(H5FS_delete(f, fs_addr), 0);
}

TEST(H5FScacheTest, PublicPersistentFreeSpaceFlushReopenAndEvict) {
  TempFile tmp_file("h5fscache_e2e.h5");
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_FSM_AGGR,
                                       /*persist=*/true, /*threshold=*/1),
            0);

  {
    ScopedFile file(
        H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, fcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedSpace space(H5Screate_simple(1, std::array<hsize_t, 1>{64}.data(),
                                       nullptr));
    ASSERT_TRUE(space.valid());

    for (const char* name : {"d1", "d2", "d3"}) {
      ScopedDataset dset(H5Dcreate2(file.get(), name, H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
    }
    // Unlink middle dataset to populate persistent file free-space sections.
    ASSERT_GE(H5Ldelete(file.get(), "d2", H5P_DEFAULT), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  ScopedFile reopened(
      H5Fopen(tmp_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  const ssize_t nsects =
      H5Fget_free_sections(reopened.get(), H5FD_MEM_DEFAULT, 0, nullptr);
  EXPECT_GE(nsects, 0);
}

}  // namespace
}  // namespace hdf5
