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
// Tests extensible array metadata cache callbacks (header, index block, super
// block, data block, and data block page) including size calculation, checksum
// verification, serialization/deserialization, corruption error paths,
// eviction/reload, SWMR flush-dependency notifications, fsf_size, and ICR free
// (third_party/hdf5/src/H5EAcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAcache.c

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5EA_hdr_t, H5EA_iblock_t, H5EA_sblock_t, H5EA_dblock_t,
// and H5EA_dblk_page_t). Define NDEBUG around internal headers so C++ struct
// layouts match `:hdf5`.
#ifndef NDEBUG
#define H5EACACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5EA_FRIEND
#define H5EA_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5EACACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EACACHE_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedEA {
 public:
  ScopedEA() = default;
  explicit ScopedEA(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEA() { reset(); }

  ScopedEA(const ScopedEA&) = delete;
  ScopedEA& operator=(const ScopedEA&) = delete;

  void reset(H5EA_t* next = nullptr) {
    if (ea_ != nullptr) {
      H5EA_close(ea_);
    }
    ea_ = next;
  }

  H5EA_t* get() const { return ea_; }

 private:
  H5EA_t* ea_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

struct CodecControl {
  bool fail_encode = false;
  bool fail_decode = false;
};

void* CodecCrtContext(void* udata) { return udata; }
herr_t CodecDstContext(void* /*ctx*/) { return 0; }

herr_t CodecFill(void* nat_blk, size_t nelmts) {
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = H5EA_TEST_FILL;
  }
  return 0;
}

herr_t CodecEncode(void* raw, const void* elmt, size_t nelmts, void* ctx) {
  auto* ctrl = static_cast<CodecControl*>(ctx);
  if (ctrl != nullptr && ctrl->fail_encode) {
    return -1;
  }
  auto* dst = static_cast<uint8_t*>(raw);
  const auto* src = static_cast<const uint64_t*>(elmt);
  for (size_t i = 0; i < nelmts; ++i) {
    const uint64_t v = src[i];
    for (int b = 0; b < 8; ++b) {
      dst[i * 8 + b] = static_cast<uint8_t>((v >> (b * 8)) & 0xffU);
    }
  }
  return 0;
}

herr_t CodecDecode(const void* raw, void* elmt, size_t nelmts, void* ctx) {
  auto* ctrl = static_cast<CodecControl*>(ctx);
  if (ctrl != nullptr && ctrl->fail_decode) {
    return -1;
  }
  const auto* src = static_cast<const uint8_t*>(raw);
  auto* dst = static_cast<uint64_t*>(elmt);
  for (size_t i = 0; i < nelmts; ++i) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) {
      v |= static_cast<uint64_t>(src[i * 8 + b]) << (b * 8);
    }
    dst[i] = v;
  }
  return 0;
}

H5EA_class_t MakeCodecClass(H5EA_cls_id_t id = H5EA_CLS_TEST_ID) {
  H5EA_class_t cls{};
  cls.id = id;
  cls.name = "H5EAcache_custom";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = CodecCrtContext;
  cls.dst_context = CodecDstContext;
  cls.fill = CodecFill;
  cls.encode = CodecEncode;
  cls.decode = CodecDecode;
  return cls;
}

H5EA_create_t MakeTieredCparam(const H5EA_class_t* cls = H5EA_CLS_TEST,
                               uint8_t max_nelmts_bits = 12,
                               uint8_t idx_blk_elmts = 4) {
  H5EA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = max_nelmts_bits;
  cparam.idx_blk_elmts = idx_blk_elmts;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 2;
  cparam.max_dblk_page_nelmts_bits = 3;
  return cparam;
}

TEST(H5EAcacheTest, EndToEndFlushEvictAndReloadAllMetadataTypes) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Case 1: Empty array (hdr with idx_blk_addr == HADDR_UNDEF) flush/evict/open.
  {
    const H5EA_create_t cparam = MakeTieredCparam();
    haddr_t empty_addr = HADDR_UNDEF;
    {
      ScopedEA ea(H5EA_create(f, &cparam, nullptr));
      ASSERT_NE(ea.get(), nullptr);
      ASSERT_GE(H5EA_get_addr(ea.get(), &empty_addr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    ScopedEA reopened(H5EA_open(f, empty_addr, nullptr));
    ASSERT_NE(reopened.get(), nullptr);
    EXPECT_EQ(reopened.get()->hdr->stats.computed.nindex_blks, 0u);
    EXPECT_EQ(reopened.get()->hdr->stats.computed.index_blk_size, 0u);
  }

  // Case 2: Populated array with idx_blk, iblock->dblock, unpaged sblock,
  // paged sblock, and dblk_page flushed, evicted, and reloaded from disk.
  {
    const H5EA_create_t cparam = MakeTieredCparam();
    haddr_t ea_addr = HADDR_UNDEF;
    constexpr auto kIndices =
        std::to_array<hsize_t>({0, 2, 4, 16, 24, 32, 40, 50});
    {
      ScopedEA ea(H5EA_create(f, &cparam, nullptr));
      ASSERT_NE(ea.get(), nullptr);
      ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
      for (hsize_t idx : kIndices) {
        const uint64_t val = idx * 17 + 3;
        ASSERT_GE(H5EA_set(ea.get(), idx, &val), 0);
      }
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    ScopedEA reopened(H5EA_open(f, ea_addr, nullptr));
    ASSERT_NE(reopened.get(), nullptr);
    EXPECT_EQ(reopened.get()->hdr->stats.computed.nindex_blks, 1u);
    EXPECT_GT(reopened.get()->hdr->stats.computed.index_blk_size, 0u);

    for (hsize_t idx : kIndices) {
      uint64_t val = 0;
      ASSERT_GE(H5EA_get(reopened.get(), idx, &val), 0);
      EXPECT_EQ(val, idx * 17 + 3);
    }
  }
}

TEST(H5EAcacheTest, SwmrFlushDependencyLifecycleEvictionAndDelete) {
  TempFile tmp_file("h5eacache_swmr.h5");

  H5AC_proxy_entry_t* parent_proxy = nullptr;
  auto destroy_proxy = absl::MakeCleanup([&parent_proxy] {
    if (parent_proxy != nullptr) {
      H5AC_proxy_entry_dest(parent_proxy);
      parent_proxy = nullptr;
    }
  });

  // Phase 1: SWMR file with proxy parent attached; flush clears
  // has_hdr_depend on sblock/dblock/dblk_page, and evict invokes BEFORE_EVICT
  // across hdr, iblock, sblock, dblock, and dblk_page.
  haddr_t ea_addr = HADDR_UNDEF;
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(
        H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    parent_proxy = H5AC_proxy_entry_create();
    ASSERT_NE(parent_proxy, nullptr);

    const H5EA_create_t cparam = MakeTieredCparam();
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    ASSERT_TRUE(ea.get()->hdr->swmr_write);
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    ASSERT_GE(H5EA_depend(ea.get(), parent_proxy), 0);

    for (hsize_t idx :
         {hsize_t{0}, hsize_t{4}, hsize_t{16}, hsize_t{32}, hsize_t{40}}) {
      const uint64_t val = idx + 1000;
      ASSERT_GE(H5EA_set(ea.get(), idx, &val), 0);
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ea.reset();
    ASSERT_GE(H5AC_evict(f), 0);
  }
  std::move(destroy_proxy).Invoke();

  // Phase 2: Reopen in SWMR-write mode to cover AFTER_LOAD across all five
  // entry types, then extend and delete while entries remain dirty/dependent to
  // cover BEFORE_EVICT with active has_hdr_depend and fsf_size on delete.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fopen(tmp_file.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                            fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedEA ea(H5EA_open(f, ea_addr, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    for (hsize_t idx :
         {hsize_t{0}, hsize_t{4}, hsize_t{16}, hsize_t{32}, hsize_t{40}}) {
      uint64_t val = 0;
      ASSERT_GE(H5EA_get(ea.get(), idx, &val), 0);
      EXPECT_EQ(val, idx + 1000);
    }

    // Extend into a new dblock in sblk 3 so sblock/dblock/dblk_page have
    // active has_hdr_depend == true when deleted without flushing first.
    const uint64_t tail_val = 2048;
    ASSERT_GE(H5EA_set(ea.get(), 48, &tail_val), 0);
    ea.reset();
    ASSERT_GE(H5EA_delete(f, ea_addr, nullptr), 0);
  }
}

TEST(H5EAcacheTest, HeaderCacheCallbacksSerializeVerifyDeserializeErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();

  H5EA_hdr_cache_ud_t udata{};
  udata.f = f;
  udata.addr = 0x2000;
  udata.ctx_udata = nullptr;

  size_t init_len = 0;
  ASSERT_GE(H5AC_EARRAY_HDR[0].get_initial_load_size(&udata, &init_len), 0);
  EXPECT_EQ(init_len, H5EA_HEADER_SIZE_FILE(f));

  H5EA_hdr_t* src_hdr = H5EA__hdr_alloc(f);
  ASSERT_NE(src_hdr, nullptr);
  auto free_src_hdr = absl::MakeCleanup([&src_hdr] {
    if (src_hdr != nullptr) {
      H5EA__hdr_dest(src_hdr);
    }
  });
  src_hdr->cparam = cparam;
  ASSERT_GE(H5EA__hdr_init(src_hdr, nullptr), 0);
  src_hdr->addr = 0x2000;
  src_hdr->idx_blk_addr = 0x3000;
  src_hdr->stats.stored.max_idx_set = 10;
  src_hdr->stats.stored.nelmts = 16;

  size_t img_len = 0;
  ASSERT_GE(H5AC_EARRAY_HDR[0].image_len(src_hdr, &img_len), 0);
  EXPECT_EQ(img_len, src_hdr->size);

  std::vector<uint8_t> image(img_len, 0);
  ASSERT_GE(H5AC_EARRAY_HDR[0].serialize(f, image.data(), img_len, src_hdr), 0);

  EXPECT_EQ(H5AC_EARRAY_HDR[0].verify_chksum(image.data(), img_len, &udata), 1);
  {
    std::vector<uint8_t> flipped = image;
    flipped[0] ^= 0x01;
    EXPECT_EQ(H5AC_EARRAY_HDR[0].verify_chksum(flipped.data(), img_len, &udata),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_EARRAY_HDR[0].verify_chksum(image.data(), 2, &udata), 0);
  }

  bool dirty = false;
  void* deser_raw =
      H5AC_EARRAY_HDR[0].deserialize(image.data(), img_len, &udata, &dirty);
  ASSERT_NE(deser_raw, nullptr);
  auto* deser_hdr = static_cast<H5EA_hdr_t*>(deser_raw);
  EXPECT_EQ(deser_hdr->idx_blk_addr, 0x3000u);
  EXPECT_EQ(deser_hdr->stats.stored.max_idx_set, 10u);
  EXPECT_EQ(deser_hdr->stats.computed.nindex_blks, 1u);
  EXPECT_GE(H5AC_EARRAY_HDR[0].free_icr(deser_hdr), 0);

  // Also test deserializing header with idx_blk_addr == HADDR_UNDEF.
  src_hdr->idx_blk_addr = HADDR_UNDEF;
  std::vector<uint8_t> empty_image(img_len, 0);
  ASSERT_GE(
      H5AC_EARRAY_HDR[0].serialize(f, empty_image.data(), img_len, src_hdr), 0);
  void* empty_raw = H5AC_EARRAY_HDR[0].deserialize(empty_image.data(), img_len,
                                                   &udata, &dirty);
  ASSERT_NE(empty_raw, nullptr);
  auto* empty_hdr = static_cast<H5EA_hdr_t*>(empty_raw);
  EXPECT_EQ(empty_hdr->stats.computed.nindex_blks, 0u);
  EXPECT_EQ(empty_hdr->stats.computed.index_blk_size, 0u);
  EXPECT_GE(H5AC_EARRAY_HDR[0].free_icr(empty_hdr), 0);

  {
    ScopedErrorSilence silence;

    std::vector<uint8_t> bad_magic = image;
    bad_magic[0] = 'Z';
    EXPECT_EQ(H5AC_EARRAY_HDR[0].deserialize(bad_magic.data(), img_len, &udata,
                                             &dirty),
              nullptr);

    std::vector<uint8_t> bad_ver = image;
    bad_ver[H5_SIZEOF_MAGIC] = 99;
    EXPECT_EQ(
        H5AC_EARRAY_HDR[0].deserialize(bad_ver.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_id = image;
    bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(H5EA_NUM_CLS_ID);
    EXPECT_EQ(
        H5AC_EARRAY_HDR[0].deserialize(bad_id.data(), img_len, &udata, &dirty),
        nullptr);
  }

  // Header notify callback paths: non-SWMR and SWMR (no proxy).
  src_hdr->swmr_write = false;
  src_hdr->parent = nullptr;
  EXPECT_GE(H5AC_EARRAY_HDR[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, src_hdr),
            0);

  src_hdr->swmr_write = true;
  src_hdr->top_proxy = nullptr;
  for (H5AC_notify_action_t action :
       {H5AC_NOTIFY_ACTION_AFTER_INSERT, H5AC_NOTIFY_ACTION_AFTER_LOAD,
        H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
        H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
        H5AC_NOTIFY_ACTION_CHILD_CLEANED, H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
        H5AC_NOTIFY_ACTION_CHILD_SERIALIZED,
        H5AC_NOTIFY_ACTION_BEFORE_EVICT}) {
    EXPECT_GE(H5AC_EARRAY_HDR[0].notify(action, src_hdr), 0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_EARRAY_HDR[0].notify(static_cast<H5AC_notify_action_t>(999),
                                        src_hdr),
              0);
  }
}

TEST(H5EAcacheTest, IndexBlockCallbacksAllVariantsAndErrors) {
  CodecControl ctrl;
  H5EA_class_t cls = MakeCodecClass();

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Variant A: idx_blk_elmts = 4, max_nelmts_bits = 12 (nsblk_addrs > 0).
  {
    const H5EA_create_t cparam = MakeTieredCparam(&cls, 12, 4);
    ScopedEA ea(H5EA_create(f, &cparam, &ctrl));
    ASSERT_NE(ea.get(), nullptr);
    H5EA_hdr_t* hdr = ea.get()->hdr;
    hdr->idx_blk_addr = 0x4000;

    const size_t nsblks = H5EA_SBLK_FIRST_IDX(cparam.sup_blk_min_data_ptrs);
    const size_t ndblk_addrs =
        2 * (static_cast<size_t>(cparam.sup_blk_min_data_ptrs) - 1);
    const size_t nsblk_addrs = hdr->nsblks - nsblks;
    ASSERT_GT(nsblk_addrs, 0u);

    std::vector<uint64_t> elmts = {11, 22, 33, 44};
    std::vector<haddr_t> dblk_addrs(ndblk_addrs, 0x5000);
    std::vector<haddr_t> sblk_addrs(nsblk_addrs, 0x6000);

    H5EA_iblock_t src_ib{};
    src_ib.hdr = hdr;
    src_ib.addr = hdr->idx_blk_addr;
    src_ib.nsblks = nsblks;
    src_ib.ndblk_addrs = ndblk_addrs;
    src_ib.nsblk_addrs = nsblk_addrs;
    src_ib.elmts = elmts.data();
    src_ib.dblk_addrs = dblk_addrs.data();
    src_ib.sblk_addrs = sblk_addrs.data();
    src_ib.size = H5EA_IBLOCK_SIZE(&src_ib);

    size_t init_len = 0;
    ASSERT_GE(H5AC_EARRAY_IBLOCK[0].get_initial_load_size(hdr, &init_len), 0);
    EXPECT_EQ(init_len, src_ib.size);

    size_t img_len = 0;
    ASSERT_GE(H5AC_EARRAY_IBLOCK[0].image_len(&src_ib, &img_len), 0);
    EXPECT_EQ(img_len, src_ib.size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_EARRAY_IBLOCK[0].serialize(f, image.data(), img_len, &src_ib),
              0);

    EXPECT_EQ(H5AC_EARRAY_IBLOCK[0].verify_chksum(image.data(), img_len, hdr),
              1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x02;
      EXPECT_EQ(
          H5AC_EARRAY_IBLOCK[0].verify_chksum(flipped.data(), img_len, hdr), 0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_EARRAY_IBLOCK[0].verify_chksum(image.data(), 2, hdr), 0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_EARRAY_IBLOCK[0].deserialize(image.data(), img_len, hdr, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_ib = static_cast<H5EA_iblock_t*>(deser_raw);
    EXPECT_EQ(static_cast<uint64_t*>(deser_ib->elmts)[2], 33u);
    EXPECT_EQ(deser_ib->dblk_addrs[0], 0x5000u);
    EXPECT_EQ(deser_ib->sblk_addrs[0], 0x6000u);

    {
      ScopedErrorSilence silence;

      std::vector<uint8_t> bad_magic = image;
      bad_magic[0] = 'X';
      EXPECT_EQ(H5AC_EARRAY_IBLOCK[0].deserialize(bad_magic.data(), img_len,
                                                  hdr, &dirty),
                nullptr);

      std::vector<uint8_t> bad_ver = image;
      bad_ver[H5_SIZEOF_MAGIC] = 42;
      EXPECT_EQ(H5AC_EARRAY_IBLOCK[0].deserialize(bad_ver.data(), img_len, hdr,
                                                  &dirty),
                nullptr);

      std::vector<uint8_t> bad_id = image;
      bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(cls.id) ^ 1U;
      EXPECT_EQ(H5AC_EARRAY_IBLOCK[0].deserialize(bad_id.data(), img_len, hdr,
                                                  &dirty),
                nullptr);

      std::vector<uint8_t> bad_addr = image;
      bad_addr[H5_SIZEOF_MAGIC + 2] ^= 0xffU;
      EXPECT_EQ(H5AC_EARRAY_IBLOCK[0].deserialize(bad_addr.data(), img_len, hdr,
                                                  &dirty),
                nullptr);

      ctrl.fail_decode = true;
      EXPECT_EQ(
          H5AC_EARRAY_IBLOCK[0].deserialize(image.data(), img_len, hdr, &dirty),
          nullptr);
      ctrl.fail_decode = false;

      ctrl.fail_encode = true;
      std::vector<uint8_t> tmp(img_len, 0);
      EXPECT_LT(
          H5AC_EARRAY_IBLOCK[0].serialize(f, tmp.data(), img_len, &src_ib), 0);
      ctrl.fail_encode = false;
    }

    for (H5AC_notify_action_t action :
         {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
          H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
          H5AC_NOTIFY_ACTION_CHILD_CLEANED,
          H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
          H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
      EXPECT_GE(H5AC_EARRAY_IBLOCK[0].notify(action, deser_ib), 0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_EARRAY_IBLOCK[0].notify(
                    static_cast<H5AC_notify_action_t>(999), deser_ib),
                0);
    }

    EXPECT_GE(H5AC_EARRAY_IBLOCK[0].free_icr(deser_ib), 0);
  }

  // Variant B: idx_blk_elmts = 0 and max_nelmts_bits = 3 (nsblk_addrs == 0).
  {
    const H5EA_create_t cparam = MakeTieredCparam(&cls, 3, 0);
    ScopedEA ea(H5EA_create(f, &cparam, &ctrl));
    ASSERT_NE(ea.get(), nullptr);
    H5EA_hdr_t* hdr = ea.get()->hdr;
    hdr->idx_blk_addr = 0x4800;

    const size_t nsblks = H5EA_SBLK_FIRST_IDX(cparam.sup_blk_min_data_ptrs);
    const size_t ndblk_addrs =
        2 * (static_cast<size_t>(cparam.sup_blk_min_data_ptrs) - 1);
    ASSERT_EQ(hdr->nsblks - nsblks, 0u);

    std::vector<haddr_t> dblk_addrs(ndblk_addrs, 0x5800);
    H5EA_iblock_t src_ib{};
    src_ib.hdr = hdr;
    src_ib.addr = hdr->idx_blk_addr;
    src_ib.nsblks = nsblks;
    src_ib.ndblk_addrs = ndblk_addrs;
    src_ib.nsblk_addrs = 0;
    src_ib.dblk_addrs = dblk_addrs.data();
    src_ib.size = H5EA_IBLOCK_SIZE(&src_ib);

    size_t img_len = 0;
    ASSERT_GE(H5AC_EARRAY_IBLOCK[0].image_len(&src_ib, &img_len), 0);
    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_EARRAY_IBLOCK[0].serialize(f, image.data(), img_len, &src_ib),
              0);

    bool dirty = false;
    void* deser_raw =
        H5AC_EARRAY_IBLOCK[0].deserialize(image.data(), img_len, hdr, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_ib = static_cast<H5EA_iblock_t*>(deser_raw);
    EXPECT_EQ(deser_ib->elmts, nullptr);
    EXPECT_EQ(deser_ib->nsblk_addrs, 0u);
    EXPECT_GE(H5AC_EARRAY_IBLOCK[0].free_icr(deser_ib), 0);
  }
}

TEST(H5EAcacheTest, SuperBlockCallbacksUnpagedPagedAndErrors) {
  CodecControl ctrl;
  H5EA_class_t cls = MakeCodecClass();

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam(&cls);
  ScopedEA ea(H5EA_create(f, &cparam, &ctrl));
  ASSERT_NE(ea.get(), nullptr);
  H5EA_hdr_t* hdr = ea.get()->hdr;

  H5EA_iblock_t dummy_parent{};
  dummy_parent.hdr = hdr;

  // sblk_idx 2 is unpaged (dblk_nelmts == 8); sblk_idx 3 is paged (16).
  for (unsigned sblk_idx : {2u, 3u}) {
    const size_t ndblks = hdr->sblk_info[sblk_idx].ndblks;
    const size_t dblk_nelmts = hdr->sblk_info[sblk_idx].dblk_nelmts;
    const size_t npages =
        (dblk_nelmts > hdr->dblk_page_nelmts)
            ? (dblk_nelmts / hdr->dblk_page_nelmts)
            : 0u;
    const size_t page_init_size = npages > 0 ? ((npages + 7) / 8) : 0u;

    std::vector<haddr_t> dblk_addrs(ndblks, 0x7000 + sblk_idx * 0x100);
    std::vector<uint8_t> page_init(ndblks * page_init_size, 0x01);

    H5EA_sblock_t src_sb{};
    src_sb.hdr = hdr;
    src_sb.parent = &dummy_parent;
    src_sb.addr = 0x6000 + sblk_idx * 0x100;
    src_sb.idx = sblk_idx;
    src_sb.block_off = hdr->sblk_info[sblk_idx].start_idx;
    src_sb.ndblks = ndblks;
    src_sb.dblk_nelmts = dblk_nelmts;
    src_sb.dblk_npages = npages;
    src_sb.dblk_page_init_size = page_init_size;
    src_sb.dblk_addrs = dblk_addrs.data();
    src_sb.page_init = npages > 0 ? page_init.data() : nullptr;
    src_sb.size = H5EA_SBLOCK_SIZE(&src_sb);

    H5EA_sblock_cache_ud_t udata{};
    udata.hdr = hdr;
    udata.parent = &dummy_parent;
    udata.sblk_idx = sblk_idx;
    udata.sblk_addr = src_sb.addr;

    size_t init_len = 0;
    ASSERT_GE(
        H5AC_EARRAY_SBLOCK[0].get_initial_load_size(&udata, &init_len), 0);
    EXPECT_EQ(init_len, src_sb.size);

    size_t img_len = 0;
    ASSERT_GE(H5AC_EARRAY_SBLOCK[0].image_len(&src_sb, &img_len), 0);
    EXPECT_EQ(img_len, src_sb.size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(
        H5AC_EARRAY_SBLOCK[0].serialize(f, image.data(), img_len, &src_sb), 0);

    EXPECT_EQ(H5AC_EARRAY_SBLOCK[0].verify_chksum(image.data(), img_len, &udata),
              1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x04;
      EXPECT_EQ(
          H5AC_EARRAY_SBLOCK[0].verify_chksum(flipped.data(), img_len, &udata),
          0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_EARRAY_SBLOCK[0].verify_chksum(image.data(), 2, &udata),
                0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_EARRAY_SBLOCK[0].deserialize(image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_sb = static_cast<H5EA_sblock_t*>(deser_raw);
    EXPECT_EQ(deser_sb->block_off, src_sb.block_off);
    EXPECT_EQ(deser_sb->dblk_addrs[0], dblk_addrs[0]);

    if (sblk_idx == 2u) {
      {
        ScopedErrorSilence silence;

        std::vector<uint8_t> bad_magic = image;
        bad_magic[0] = 'Q';
        EXPECT_EQ(H5AC_EARRAY_SBLOCK[0].deserialize(bad_magic.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_ver = image;
        bad_ver[H5_SIZEOF_MAGIC] = 55;
        EXPECT_EQ(H5AC_EARRAY_SBLOCK[0].deserialize(bad_ver.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_id = image;
        bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(cls.id) ^ 1U;
        EXPECT_EQ(H5AC_EARRAY_SBLOCK[0].deserialize(bad_id.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_addr = image;
        bad_addr[H5_SIZEOF_MAGIC + 2] ^= 0xffU;
        EXPECT_EQ(H5AC_EARRAY_SBLOCK[0].deserialize(bad_addr.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);
      }

      deser_sb->has_hdr_depend = false;
      for (H5AC_notify_action_t action :
           {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
            H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
            H5AC_NOTIFY_ACTION_CHILD_CLEANED,
            H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
            H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
        EXPECT_GE(H5AC_EARRAY_SBLOCK[0].notify(action, deser_sb), 0);
      }
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5AC_EARRAY_SBLOCK[0].notify(
                      static_cast<H5AC_notify_action_t>(999), deser_sb),
                  0);
      }
    }

    EXPECT_GE(H5AC_EARRAY_SBLOCK[0].free_icr(deser_sb), 0);
  }
}

TEST(H5EAcacheTest, DataBlockAndPageCallbacksUnpagedPagedFsfAndErrors) {
  CodecControl ctrl;
  H5EA_class_t cls = MakeCodecClass();

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam(&cls);
  ScopedEA ea(H5EA_create(f, &cparam, &ctrl));
  ASSERT_NE(ea.get(), nullptr);
  H5EA_hdr_t* hdr = ea.get()->hdr;

  H5EA_sblock_t dummy_parent{};
  dummy_parent.hdr = hdr;

  // 1. Data block: both unpaged (nelmts = 8) and paged (nelmts = 16).
  for (size_t nelmts : {size_t{8}, size_t{16}}) {
    const size_t npages =
        (nelmts > hdr->dblk_page_nelmts) ? (nelmts / hdr->dblk_page_nelmts) : 0u;
    std::vector<uint64_t> elmts = {1, 2, 3, 4, 5, 6, 7, 8};

    H5EA_dblock_t src_db{};
    src_db.cache_info.type = H5AC_EARRAY_DBLOCK;
    src_db.hdr = hdr;
    src_db.parent = &dummy_parent;
    src_db.addr = 0x8000 + nelmts * 0x10;
    src_db.block_off = 16;
    src_db.nelmts = nelmts;
    src_db.npages = npages;
    src_db.elmts = npages == 0 ? elmts.data() : nullptr;
    src_db.size = H5EA_DBLOCK_SIZE(&src_db);

    H5EA_dblock_cache_ud_t udata{};
    udata.hdr = hdr;
    udata.parent = &dummy_parent;
    udata.nelmts = nelmts;
    udata.dblk_addr = src_db.addr;

    size_t init_len = 0;
    ASSERT_GE(
        H5AC_EARRAY_DBLOCK[0].get_initial_load_size(&udata, &init_len), 0);
    EXPECT_EQ(init_len, npages == 0 ? src_db.size
                                    : H5EA_DBLOCK_PREFIX_SIZE(&src_db));

    size_t img_len = 0;
    ASSERT_GE(H5AC_EARRAY_DBLOCK[0].image_len(&src_db, &img_len), 0);
    EXPECT_EQ(img_len, init_len);

    hsize_t fsf_sz = 0;
    ASSERT_GE(H5AC_EARRAY_DBLOCK[0].fsf_size(&src_db, &fsf_sz), 0);
    EXPECT_EQ(fsf_sz, src_db.size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(
        H5AC_EARRAY_DBLOCK[0].serialize(f, image.data(), img_len, &src_db), 0);

    EXPECT_EQ(H5AC_EARRAY_DBLOCK[0].verify_chksum(image.data(), img_len, &udata),
              1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x08;
      EXPECT_EQ(
          H5AC_EARRAY_DBLOCK[0].verify_chksum(flipped.data(), img_len, &udata),
          0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_EARRAY_DBLOCK[0].verify_chksum(image.data(), 2, &udata),
                0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_EARRAY_DBLOCK[0].deserialize(image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_db = static_cast<H5EA_dblock_t*>(deser_raw);
    EXPECT_EQ(deser_db->block_off, 16u);
    EXPECT_EQ(deser_db->size, src_db.size);

    if (npages == 0) {
      EXPECT_EQ(static_cast<uint64_t*>(deser_db->elmts)[7], 8u);

      {
        ScopedErrorSilence silence;

        std::vector<uint8_t> bad_magic = image;
        bad_magic[0] = 'M';
        EXPECT_EQ(H5AC_EARRAY_DBLOCK[0].deserialize(bad_magic.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_ver = image;
        bad_ver[H5_SIZEOF_MAGIC] = 7;
        EXPECT_EQ(H5AC_EARRAY_DBLOCK[0].deserialize(bad_ver.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_id = image;
        bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(cls.id) ^ 1U;
        EXPECT_EQ(H5AC_EARRAY_DBLOCK[0].deserialize(bad_id.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_addr = image;
        bad_addr[H5_SIZEOF_MAGIC + 2] ^= 0xffU;
        EXPECT_EQ(H5AC_EARRAY_DBLOCK[0].deserialize(bad_addr.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        ctrl.fail_decode = true;
        EXPECT_EQ(H5AC_EARRAY_DBLOCK[0].deserialize(image.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);
        ctrl.fail_decode = false;

        ctrl.fail_encode = true;
        std::vector<uint8_t> tmp(img_len, 0);
        EXPECT_LT(
            H5AC_EARRAY_DBLOCK[0].serialize(f, tmp.data(), img_len, &src_db),
            0);
        ctrl.fail_encode = false;
      }

      deser_db->has_hdr_depend = false;
      for (H5AC_notify_action_t action :
           {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
            H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
            H5AC_NOTIFY_ACTION_CHILD_CLEANED,
            H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
            H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
        EXPECT_GE(H5AC_EARRAY_DBLOCK[0].notify(action, deser_db), 0);
      }
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5AC_EARRAY_DBLOCK[0].notify(
                      static_cast<H5AC_notify_action_t>(999), deser_db),
                  0);
      }
    }

    EXPECT_GE(H5AC_EARRAY_DBLOCK[0].free_icr(deser_db), 0);
  }

  // 2. Data block page callbacks.
  {
    std::vector<uint64_t> page_elmts = {10, 20, 30, 40, 50, 60, 70, 80};
    H5EA_dblk_page_t src_page{};
    src_page.hdr = hdr;
    src_page.parent = &dummy_parent;
    src_page.addr = 0x9000;
    src_page.elmts = page_elmts.data();
    src_page.size = H5EA_DBLK_PAGE_SIZE(hdr);

    H5EA_dblk_page_cache_ud_t udata{};
    udata.hdr = hdr;
    udata.parent = &dummy_parent;
    udata.dblk_page_addr = src_page.addr;

    size_t init_len = 0;
    ASSERT_GE(H5AC_EARRAY_DBLK_PAGE[0].get_initial_load_size(&udata, &init_len),
              0);
    EXPECT_EQ(init_len, src_page.size);

    size_t img_len = 0;
    ASSERT_GE(H5AC_EARRAY_DBLK_PAGE[0].image_len(&src_page, &img_len), 0);
    EXPECT_EQ(img_len, src_page.size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_EARRAY_DBLK_PAGE[0].serialize(f, image.data(), img_len,
                                                 &src_page),
              0);

    EXPECT_EQ(
        H5AC_EARRAY_DBLK_PAGE[0].verify_chksum(image.data(), img_len, &udata),
        1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x10;
      EXPECT_EQ(H5AC_EARRAY_DBLK_PAGE[0].verify_chksum(flipped.data(), img_len,
                                                       &udata),
                0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5AC_EARRAY_DBLK_PAGE[0].verify_chksum(image.data(), 2, &udata), 0);
    }

    bool dirty = false;
    void* deser_raw = H5AC_EARRAY_DBLK_PAGE[0].deserialize(
        image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_page = static_cast<H5EA_dblk_page_t*>(deser_raw);
    EXPECT_EQ(static_cast<uint64_t*>(deser_page->elmts)[5], 60u);

    {
      ScopedErrorSilence silence;

      ctrl.fail_decode = true;
      EXPECT_EQ(H5AC_EARRAY_DBLK_PAGE[0].deserialize(image.data(), img_len,
                                                     &udata, &dirty),
                nullptr);
      ctrl.fail_decode = false;

      ctrl.fail_encode = true;
      std::vector<uint8_t> tmp(img_len, 0);
      EXPECT_LT(H5AC_EARRAY_DBLK_PAGE[0].serialize(f, tmp.data(), img_len,
                                                   &src_page),
                0);
      ctrl.fail_encode = false;
    }

    deser_page->has_hdr_depend = false;
    for (H5AC_notify_action_t action :
         {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
          H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
          H5AC_NOTIFY_ACTION_CHILD_CLEANED,
          H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
          H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
      EXPECT_GE(H5AC_EARRAY_DBLK_PAGE[0].notify(action, deser_page), 0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_EARRAY_DBLK_PAGE[0].notify(
                    static_cast<H5AC_notify_action_t>(999), deser_page),
                0);
    }

    EXPECT_GE(H5AC_EARRAY_DBLK_PAGE[0].free_icr(deser_page), 0);
  }
}

}  // namespace
}  // namespace hdf5
