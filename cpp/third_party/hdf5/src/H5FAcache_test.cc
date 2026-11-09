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
// Tests fixed array metadata cache callbacks (header, data block, and data
// block page) including size calculation, checksum verification, serialization
// and deserialization, corruption error paths, eviction/reload, SWMR flush
// dependencies, fsf_size, and ICR free (third_party/hdf5/src/H5FAcache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FAcache.c

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FA_hdr_t, H5FA_dblock_t, and H5FA_dblk_page_t). Define
// NDEBUG around internal headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FACACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FA_FRIEND
#define H5FA_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FApkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FACACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FACACHE_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedFA {
 public:
  ScopedFA() = default;
  explicit ScopedFA(H5FA_t* fa) : fa_(fa) {}
  ~ScopedFA() { reset(); }

  ScopedFA(const ScopedFA&) = delete;
  ScopedFA& operator=(const ScopedFA&) = delete;

  void reset(H5FA_t* next = nullptr) {
    if (fa_ != nullptr) {
      H5FA_close(fa_);
    }
    fa_ = next;
  }

  H5FA_t* get() const { return fa_; }

 private:
  H5FA_t* fa_ = nullptr;
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
    dst[i] = H5FA_TEST_FILL;
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

H5FA_class_t MakeCodecClass(H5FA_cls_id_t id = H5FA_CLS_TEST_ID) {
  H5FA_class_t cls{};
  cls.id = id;
  cls.name = "H5FAcache_custom";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = CodecCrtContext;
  cls.dst_context = CodecDstContext;
  cls.fill = CodecFill;
  cls.encode = CodecEncode;
  cls.decode = CodecDecode;
  return cls;
}

H5FA_create_t MakeCparam(hsize_t nelmts, uint8_t page_bits = 3,
                         const H5FA_class_t* cls = H5FA_CLS_TEST) {
  H5FA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_dblk_page_nelmts_bits = page_bits;
  cparam.nelmts = nelmts;
  return cparam;
}

TEST(H5FAcacheTest, EndToEndFlushEvictAndReloadEmptyUnpagedAndPaged) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Case 1: Empty array (hdr with dblk_addr == HADDR_UNDEF).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/4);
    haddr_t empty_addr = HADDR_UNDEF;
    {
      ScopedFA fa(H5FA_create(f, &cparam, nullptr));
      ASSERT_NE(fa.get(), nullptr);
      ASSERT_GE(H5FA_get_addr(fa.get(), &empty_addr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    ScopedFA reopened(H5FA_open(f, empty_addr, nullptr));
    ASSERT_NE(reopened.get(), nullptr);
    EXPECT_EQ(reopened.get()->hdr->stats.dblk_size, 0u);
    EXPECT_FALSE(H5_addr_defined(reopened.get()->hdr->dblk_addr));
  }

  // Case 2: Unpaged populated array (nelmts=6 <= dblk_page_nelmts=8).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/6, /*page_bits=*/3);
    haddr_t fa_addr = HADDR_UNDEF;
    {
      ScopedFA fa(H5FA_create(f, &cparam, nullptr));
      ASSERT_NE(fa.get(), nullptr);
      ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);
      const uint64_t v = 4242;
      ASSERT_GE(H5FA_set(fa.get(), 4, &v), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    ScopedFA reopened(H5FA_open(f, fa_addr, nullptr));
    ASSERT_NE(reopened.get(), nullptr);
    EXPECT_GT(reopened.get()->hdr->stats.dblk_size, 0u);
    uint64_t val = 0;
    ASSERT_GE(H5FA_get(reopened.get(), 4, &val), 0);
    EXPECT_EQ(val, 4242u);
  }

  // Case 3: Paged populated array (nelmts=20 > dblk_page_nelmts=8).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
    haddr_t fa_addr = HADDR_UNDEF;
    {
      ScopedFA fa(H5FA_create(f, &cparam, nullptr));
      ASSERT_NE(fa.get(), nullptr);
      ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);
      const uint64_t v0 = 111;
      const uint64_t v19 = 1999;
      ASSERT_GE(H5FA_set(fa.get(), 0, &v0), 0);
      ASSERT_GE(H5FA_set(fa.get(), 19, &v19), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    ScopedFA reopened(H5FA_open(f, fa_addr, nullptr));
    ASSERT_NE(reopened.get(), nullptr);
    EXPECT_GT(reopened.get()->hdr->stats.dblk_size, 0u);
    uint64_t val = 0;
    ASSERT_GE(H5FA_get(reopened.get(), 0, &val), 0);
    EXPECT_EQ(val, 111u);
    ASSERT_GE(H5FA_get(reopened.get(), 19, &val), 0);
    EXPECT_EQ(val, 1999u);
  }
}

TEST(H5FAcacheTest, SwmrFlushDependencyLifecycleEvictionAndDelete) {
  TempFile tmp_file("h5facache_swmr.h5");

  H5AC_proxy_entry_t* parent_proxy = nullptr;
  auto destroy_proxy = absl::MakeCleanup([&parent_proxy] {
    if (parent_proxy != nullptr) {
      H5AC_proxy_entry_dest(parent_proxy);
      parent_proxy = nullptr;
    }
  });

  // Phase 1: SWMR file with proxy parent attached; flush + evict invokes
  // BEFORE_EVICT across hdr (with parent != nullptr), dblock, and dblk_page.
  haddr_t fa_addr = HADDR_UNDEF;
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

    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_TRUE(fa.get()->hdr->swmr_write);
    ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);
    ASSERT_GE(H5FA_depend(fa.get(), parent_proxy), 0);

    const uint64_t v0 = 1000;
    const uint64_t v18 = 1018;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v0), 0);
    ASSERT_GE(H5FA_set(fa.get(), 18, &v18), 0);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    fa.reset();
    ASSERT_GE(H5AC_evict(f), 0);
  }
  std::move(destroy_proxy).Invoke();

  // Phase 2: Reopen in SWMR-write mode to cover AFTER_LOAD across hdr, dblock,
  // and dblk_page, then dirty page 1 and delete immediately to exercise
  // fsf_size and BEFORE_EVICT during deletion.
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

    ScopedFA fa(H5FA_open(f, fa_addr, nullptr));
    ASSERT_NE(fa.get(), nullptr);

    uint64_t val = 0;
    ASSERT_GE(H5FA_get(fa.get(), 0, &val), 0);
    EXPECT_EQ(val, 1000u);
    ASSERT_GE(H5FA_get(fa.get(), 18, &val), 0);
    EXPECT_EQ(val, 1018u);

    const uint64_t v9 = 1009;
    ASSERT_GE(H5FA_set(fa.get(), 9, &v9), 0);
    fa.reset();
    ASSERT_GE(H5FA_delete(f, fa_addr, nullptr), 0);
  }
}

TEST(H5FAcacheTest, HeaderCacheCallbacksSerializeVerifyDeserializeErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5FA_hdr_cache_ud_t udata{};
  udata.f = f;
  udata.addr = 0x2000;
  udata.ctx_udata = nullptr;

  size_t init_len = 0;
  ASSERT_GE(H5AC_FARRAY_HDR[0].get_initial_load_size(&udata, &init_len), 0);
  EXPECT_EQ(init_len, H5FA_HEADER_SIZE_FILE(f));

  H5FA_hdr_t* src_hdr = H5FA__hdr_alloc(f);
  ASSERT_NE(src_hdr, nullptr);
  auto free_src_hdr = absl::MakeCleanup([&src_hdr] {
    if (src_hdr != nullptr) {
      H5FA__hdr_dest(src_hdr);
    }
  });
  src_hdr->cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
  ASSERT_GE(H5FA__hdr_init(src_hdr, nullptr), 0);
  src_hdr->addr = 0x2000;
  src_hdr->dblk_addr = 0x3000;

  size_t img_len = 0;
  ASSERT_GE(H5AC_FARRAY_HDR[0].image_len(src_hdr, &img_len), 0);
  EXPECT_EQ(img_len, src_hdr->size);

  // Variant A: Paged header (nelmts=20 > dblk_page_nelmts=8) with dblk_addr.
  std::vector<uint8_t> image(img_len, 0);
  ASSERT_GE(H5AC_FARRAY_HDR[0].serialize(f, image.data(), img_len, src_hdr), 0);

  EXPECT_EQ(H5AC_FARRAY_HDR[0].verify_chksum(image.data(), img_len, &udata), 1);
  {
    std::vector<uint8_t> flipped = image;
    flipped[0] ^= 0x01;
    EXPECT_EQ(H5AC_FARRAY_HDR[0].verify_chksum(flipped.data(), img_len, &udata),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_FARRAY_HDR[0].verify_chksum(image.data(), 2, &udata), 0);
  }

  bool dirty = false;
  void* deser_raw =
      H5AC_FARRAY_HDR[0].deserialize(image.data(), img_len, &udata, &dirty);
  ASSERT_NE(deser_raw, nullptr);
  auto* deser_hdr = static_cast<H5FA_hdr_t*>(deser_raw);
  EXPECT_EQ(deser_hdr->dblk_addr, 0x3000u);
  EXPECT_EQ(deser_hdr->stats.nelmts, 20u);
  EXPECT_GT(deser_hdr->stats.dblk_size, 0u);
  EXPECT_GE(H5AC_FARRAY_HDR[0].free_icr(deser_hdr), 0);

  // Variant B: Unpaged header (nelmts=6 <= dblk_page_nelmts=8) with dblk_addr.
  src_hdr->cparam.nelmts = 6;
  src_hdr->stats.nelmts = 6;
  std::vector<uint8_t> unpaged_image(img_len, 0);
  ASSERT_GE(
      H5AC_FARRAY_HDR[0].serialize(f, unpaged_image.data(), img_len, src_hdr),
      0);
  void* unpaged_raw = H5AC_FARRAY_HDR[0].deserialize(unpaged_image.data(),
                                                     img_len, &udata, &dirty);
  ASSERT_NE(unpaged_raw, nullptr);
  auto* unpaged_hdr = static_cast<H5FA_hdr_t*>(unpaged_raw);
  EXPECT_GT(unpaged_hdr->stats.dblk_size, 0u);
  EXPECT_GE(H5AC_FARRAY_HDR[0].free_icr(unpaged_hdr), 0);

  // Variant C: Empty header (dblk_addr == HADDR_UNDEF).
  src_hdr->dblk_addr = HADDR_UNDEF;
  std::vector<uint8_t> empty_image(img_len, 0);
  ASSERT_GE(
      H5AC_FARRAY_HDR[0].serialize(f, empty_image.data(), img_len, src_hdr), 0);
  void* empty_raw = H5AC_FARRAY_HDR[0].deserialize(empty_image.data(), img_len,
                                                   &udata, &dirty);
  ASSERT_NE(empty_raw, nullptr);
  auto* empty_hdr = static_cast<H5FA_hdr_t*>(empty_raw);
  EXPECT_EQ(empty_hdr->stats.dblk_size, 0u);
  EXPECT_GE(H5AC_FARRAY_HDR[0].free_icr(empty_hdr), 0);

  // Corrupt magic, version, and class ID deserialization failures.
  {
    ScopedErrorSilence silence;

    std::vector<uint8_t> bad_magic = image;
    bad_magic[0] = 'Z';
    EXPECT_EQ(H5AC_FARRAY_HDR[0].deserialize(bad_magic.data(), img_len, &udata,
                                             &dirty),
              nullptr);

    std::vector<uint8_t> bad_ver = image;
    bad_ver[H5_SIZEOF_MAGIC] = 99;
    EXPECT_EQ(
        H5AC_FARRAY_HDR[0].deserialize(bad_ver.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_id = image;
    bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(H5FA_NUM_CLS_ID);
    EXPECT_EQ(
        H5AC_FARRAY_HDR[0].deserialize(bad_id.data(), img_len, &udata, &dirty),
        nullptr);
  }

  // Header notify callback paths: non-SWMR and SWMR (no proxy).
  src_hdr->swmr_write = false;
  src_hdr->parent = nullptr;
  EXPECT_GE(H5AC_FARRAY_HDR[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, src_hdr),
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
    EXPECT_GE(H5AC_FARRAY_HDR[0].notify(action, src_hdr), 0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_FARRAY_HDR[0].notify(static_cast<H5AC_notify_action_t>(999),
                                        src_hdr),
              0);
  }
}

TEST(H5FAcacheTest, DataBlockAndPageCallbacksUnpagedPagedFsfAndErrors) {
  CodecControl ctrl;
  const H5FA_class_t cls = MakeCodecClass();

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Data block callbacks: unpaged (nelmts=8) and paged (nelmts=20).
  for (hsize_t nelmts : {hsize_t{8}, hsize_t{20}}) {
    const H5FA_create_t cparam = MakeCparam(nelmts, /*page_bits=*/3, &cls);
    ScopedFA fa(H5FA_create(f, &cparam, &ctrl));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;

    const size_t dblk_page_nelmts = 1u << cparam.max_dblk_page_nelmts_bits;
    const size_t npages =
        (nelmts > dblk_page_nelmts)
            ? static_cast<size_t>((nelmts + dblk_page_nelmts - 1) /
                                  dblk_page_nelmts)
            : 0u;
    const size_t page_init_size = npages > 0 ? ((npages + 7) / 8) : 0u;

    std::vector<uint64_t> elmts = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> page_init(page_init_size, 0x05);

    H5FA_dblock_t src_db{};
    src_db.cache_info.type = H5AC_FARRAY_DBLOCK;
    src_db.hdr = hdr;
    src_db.addr = 0x4000 + nelmts * 0x10;
    src_db.npages = npages;
    src_db.dblk_page_nelmts = dblk_page_nelmts;
    src_db.dblk_page_init_size = page_init_size;
    src_db.dblk_page_init = npages > 0 ? page_init.data() : nullptr;
    src_db.elmts = npages == 0 ? elmts.data() : nullptr;
    src_db.size = H5FA_DBLOCK_SIZE(&src_db);

    H5FA_dblock_cache_ud_t udata{};
    udata.hdr = hdr;
    udata.dblk_addr = src_db.addr;

    size_t init_len = 0;
    ASSERT_GE(
        H5AC_FARRAY_DBLOCK[0].get_initial_load_size(&udata, &init_len), 0);
    EXPECT_EQ(init_len, npages == 0 ? src_db.size
                                    : H5FA_DBLOCK_PREFIX_SIZE(&src_db));

    size_t img_len = 0;
    ASSERT_GE(H5AC_FARRAY_DBLOCK[0].image_len(&src_db, &img_len), 0);
    EXPECT_EQ(img_len, init_len);

    hsize_t fsf_sz = 0;
    ASSERT_GE(H5AC_FARRAY_DBLOCK[0].fsf_size(&src_db, &fsf_sz), 0);
    EXPECT_EQ(fsf_sz, src_db.size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(
        H5AC_FARRAY_DBLOCK[0].serialize(f, image.data(), img_len, &src_db), 0);

    EXPECT_EQ(H5AC_FARRAY_DBLOCK[0].verify_chksum(image.data(), img_len, &udata),
              1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x08;
      EXPECT_EQ(
          H5AC_FARRAY_DBLOCK[0].verify_chksum(flipped.data(), img_len, &udata),
          0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_FARRAY_DBLOCK[0].verify_chksum(image.data(), 2, &udata),
                0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_FARRAY_DBLOCK[0].deserialize(image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_db = static_cast<H5FA_dblock_t*>(deser_raw);
    EXPECT_EQ(deser_db->size, src_db.size);

    if (npages == 0) {
      EXPECT_EQ(static_cast<uint64_t*>(deser_db->elmts)[7], 8u);

      {
        ScopedErrorSilence silence;

        std::vector<uint8_t> bad_magic = image;
        bad_magic[0] = 'M';
        EXPECT_EQ(H5AC_FARRAY_DBLOCK[0].deserialize(bad_magic.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_ver = image;
        bad_ver[H5_SIZEOF_MAGIC] = 7;
        EXPECT_EQ(H5AC_FARRAY_DBLOCK[0].deserialize(bad_ver.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_id = image;
        bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(cls.id) ^ 1U;
        EXPECT_EQ(H5AC_FARRAY_DBLOCK[0].deserialize(bad_id.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        std::vector<uint8_t> bad_addr = image;
        bad_addr[H5_SIZEOF_MAGIC + 2] ^= 0xffU;
        EXPECT_EQ(H5AC_FARRAY_DBLOCK[0].deserialize(bad_addr.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);

        ctrl.fail_decode = true;
        EXPECT_EQ(H5AC_FARRAY_DBLOCK[0].deserialize(image.data(), img_len,
                                                    &udata, &dirty),
                  nullptr);
        ctrl.fail_decode = false;

        ctrl.fail_encode = true;
        std::vector<uint8_t> tmp(img_len, 0);
        EXPECT_LT(
            H5AC_FARRAY_DBLOCK[0].serialize(f, tmp.data(), img_len, &src_db),
            0);
        ctrl.fail_encode = false;
      }

      // Exercise non-SWMR notify and SWMR no-op/unknown notify actions.
      EXPECT_GE(
          H5AC_FARRAY_DBLOCK[0].notify(H5AC_NOTIFY_ACTION_AFTER_FLUSH, deser_db),
          0);
      hdr->swmr_write = true;
      auto restore_swmr =
          absl::MakeCleanup([hdr] { hdr->swmr_write = false; });
      for (H5AC_notify_action_t action :
           {H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_ENTRY_DIRTIED,
            H5AC_NOTIFY_ACTION_ENTRY_CLEANED, H5AC_NOTIFY_ACTION_CHILD_DIRTIED,
            H5AC_NOTIFY_ACTION_CHILD_CLEANED,
            H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
            H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
        EXPECT_GE(H5AC_FARRAY_DBLOCK[0].notify(action, deser_db), 0);
      }
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5AC_FARRAY_DBLOCK[0].notify(
                      static_cast<H5AC_notify_action_t>(999), deser_db),
                  0);
      }
      std::move(restore_swmr).Invoke();
    }

    EXPECT_GE(H5AC_FARRAY_DBLOCK[0].free_icr(deser_db), 0);
  }

  // 2. Data block page callbacks (including partial last page of 4 elements).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3,
                                            &cls);
    ScopedFA fa(H5FA_create(f, &cparam, &ctrl));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;

    std::vector<uint64_t> page_elmts = {10, 20, 30, 40};
    H5FA_dblk_page_t src_page{};
    src_page.hdr = hdr;
    src_page.addr = 0x9000;
    src_page.nelmts = page_elmts.size();
    src_page.elmts = page_elmts.data();
    src_page.size = H5FA_DBLK_PAGE_SIZE(hdr, src_page.nelmts);

    H5FA_dblk_page_cache_ud_t udata{};
    udata.hdr = hdr;
    udata.nelmts = page_elmts.size();
    udata.dblk_page_addr = src_page.addr;

    size_t init_len = 0;
    ASSERT_GE(
        H5AC_FARRAY_DBLK_PAGE[0].get_initial_load_size(&udata, &init_len), 0);
    EXPECT_EQ(init_len, src_page.size);

    size_t img_len = 0;
    ASSERT_GE(H5AC_FARRAY_DBLK_PAGE[0].image_len(&src_page, &img_len), 0);
    EXPECT_EQ(img_len, src_page.size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_FARRAY_DBLK_PAGE[0].serialize(f, image.data(), img_len,
                                                 &src_page),
              0);

    EXPECT_EQ(
        H5AC_FARRAY_DBLK_PAGE[0].verify_chksum(image.data(), img_len, &udata),
        1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x10;
      EXPECT_EQ(H5AC_FARRAY_DBLK_PAGE[0].verify_chksum(flipped.data(), img_len,
                                                       &udata),
                0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5AC_FARRAY_DBLK_PAGE[0].verify_chksum(image.data(), 2, &udata), 0);
    }

    bool dirty = false;
    void* deser_raw = H5AC_FARRAY_DBLK_PAGE[0].deserialize(
        image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_page = static_cast<H5FA_dblk_page_t*>(deser_raw);
    EXPECT_EQ(static_cast<uint64_t*>(deser_page->elmts)[3], 40u);

    {
      ScopedErrorSilence silence;

      ctrl.fail_decode = true;
      EXPECT_EQ(H5AC_FARRAY_DBLK_PAGE[0].deserialize(image.data(), img_len,
                                                     &udata, &dirty),
                nullptr);
      ctrl.fail_decode = false;

      ctrl.fail_encode = true;
      std::vector<uint8_t> tmp(img_len, 0);
      EXPECT_LT(H5AC_FARRAY_DBLK_PAGE[0].serialize(f, tmp.data(), img_len,
                                                   &src_page),
                0);
      ctrl.fail_encode = false;
    }

    for (H5AC_notify_action_t action :
         {H5AC_NOTIFY_ACTION_AFTER_INSERT, H5AC_NOTIFY_ACTION_AFTER_LOAD,
          H5AC_NOTIFY_ACTION_AFTER_FLUSH, H5AC_NOTIFY_ACTION_BEFORE_EVICT,
          H5AC_NOTIFY_ACTION_ENTRY_DIRTIED, H5AC_NOTIFY_ACTION_ENTRY_CLEANED,
          H5AC_NOTIFY_ACTION_CHILD_DIRTIED, H5AC_NOTIFY_ACTION_CHILD_CLEANED,
          H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
          H5AC_NOTIFY_ACTION_CHILD_SERIALIZED}) {
      EXPECT_GE(H5AC_FARRAY_DBLK_PAGE[0].notify(action, deser_page), 0);
    }
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5AC_FARRAY_DBLK_PAGE[0].notify(
                    static_cast<H5AC_notify_action_t>(999), deser_page),
                0);
    }

    EXPECT_GE(H5AC_FARRAY_DBLK_PAGE[0].free_icr(deser_page), 0);
  }
}

}  // namespace
}  // namespace hdf5
