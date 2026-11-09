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
// Tests extensible array data block allocation, creation, superblock index
// mapping, protection, unprotection, unpaged and paged deletion, error paths,
// and SWMR top_proxy attachment (third_party/hdf5/src/H5EAdblock.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAdblock.c

#include <cstdint>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5EA_hdr_t, H5EA_iblock_t, H5EA_sblock_t, and
// H5EA_dblock_t). Define NDEBUG around internal headers so C++ struct layouts
// match `:hdf5`.
#ifndef NDEBUG
#define H5EADBLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5EADBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EADBLOCK_TEST_RESTORE_UNDEF_NDEBUG
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
  bool fail_decode = false;
};

void* CodecCrtContext(void* udata) { return udata; }
herr_t CodecDstContext(void* /*ctx*/) { return 0; }

herr_t FailingFill(void* /*nat_blk*/, size_t /*nelmts*/) { return -1; }

herr_t CodecFill(void* nat_blk, size_t nelmts) {
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = H5EA_TEST_FILL;
  }
  return 0;
}

herr_t CodecEncode(void* raw, const void* elmt, size_t nelmts, void* /*ctx*/) {
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

H5EA_class_t MakeCustomClass(bool fail_fill) {
  H5EA_class_t cls{};
  cls.id = H5EA_CLS_TEST_ID;
  cls.name = "H5EAdblock_custom";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = CodecCrtContext;
  cls.dst_context = CodecDstContext;
  cls.fill = fail_fill ? FailingFill : CodecFill;
  cls.encode = CodecEncode;
  cls.decode = CodecDecode;
  return cls;
}

H5EA_create_t MakeTieredCparam(const H5EA_class_t* cls = H5EA_CLS_TEST) {
  // With idx_blk_elmts=4, data_blk_min_elmts=4, sup_blk_min_data_ptrs=2, and
  // max_dblk_page_nelmts_bits=3 (dblk_page_nelmts=8):
  //   iblock inline elmts: [0..3]
  //   sblk 0: [4..7]   (1 x 4 elmts, in iblock, unpaged)
  //   sblk 1: [8..15]  (1 x 8 elmts, in iblock, unpaged)
  //   sblk 2: [16..31] (2 x 8 elmts, in sblock, unpaged)
  //   sblk 3: [32..63] (2 x 16 elmts, in sblock, paged: 2 pages x 8 elmts)
  H5EA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 12;
  cparam.idx_blk_elmts = 4;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 2;
  cparam.max_dblk_page_nelmts_bits = 3;
  return cparam;
}

TEST(H5EAdblockTest, SblkIdxAndAllocDestUnpagedAndPaged) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Trigger index block creation so hdr and iblock are available as parent.
  const uint64_t val = 1;
  ASSERT_GE(H5EA_set(ea.get(), 0, &val), 0);

  H5EA_hdr_t* hdr = ea.get()->hdr;
  ASSERT_NE(hdr, nullptr);

  // Verify superblock index boundaries.
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 4), 0u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 7), 0u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 8), 1u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 15), 1u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 16), 2u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 31), 2u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 32), 3u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 63), 3u);
  EXPECT_EQ(H5EA__dblock_sblk_idx(hdr, 64), 4u);

  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);

  const size_t rc_before = hdr->rc;

  // Unpaged dblock alloc + dest (nelmts <= dblk_page_nelmts).
  H5EA_dblock_t* unpaged = H5EA__dblock_alloc(hdr, iblock, 8);
  ASSERT_NE(unpaged, nullptr);
  EXPECT_EQ(unpaged->hdr, hdr);
  EXPECT_EQ(unpaged->parent, iblock);
  EXPECT_EQ(unpaged->nelmts, 8u);
  EXPECT_EQ(unpaged->npages, 0u);
  EXPECT_NE(unpaged->elmts, nullptr);
  EXPECT_EQ(hdr->rc, rc_before + 1);
  EXPECT_GE(H5EA__dblock_dest(unpaged), 0);
  EXPECT_EQ(hdr->rc, rc_before);

  // Paged dblock alloc + dest (nelmts > dblk_page_nelmts).
  H5EA_dblock_t* paged = H5EA__dblock_alloc(hdr, iblock, 16);
  ASSERT_NE(paged, nullptr);
  EXPECT_EQ(paged->nelmts, 16u);
  EXPECT_EQ(paged->npages, 2u);
  EXPECT_EQ(paged->elmts, nullptr);
  EXPECT_EQ(hdr->rc, rc_before + 1);
  EXPECT_GE(H5EA__dblock_dest(paged), 0);
  EXPECT_EQ(hdr->rc, rc_before);

  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5EAdblockTest, UnpagedCreateProtectFlushEvictReloadAndDelete) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  const uint64_t inline_val = 10;
  ASSERT_GE(H5EA_set(ea.get(), 0, &inline_val), 0);

  H5EA_hdr_t* hdr = ea.get()->hdr;
  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(iblock, nullptr);

  bool stats_changed = false;
  const haddr_t dblk_addr =
      H5EA__dblock_create(hdr, iblock, &stats_changed, /*dblk_off=*/8,
                          /*nelmts=*/8);
  ASSERT_TRUE(H5_addr_defined(dblk_addr));
  EXPECT_TRUE(stats_changed);

  // Protect read/write, check fill values, modify elements, unprotect dirtied.
  H5EA_dblock_t* dblock = H5EA__dblock_protect(
      hdr, iblock, dblk_addr, /*dblk_nelmts=*/8, H5AC__NO_FLAGS_SET);
  ASSERT_NE(dblock, nullptr);
  auto* elmts = static_cast<uint64_t*>(dblock->elmts);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(elmts[i], H5EA_TEST_FILL);
    elmts[i] = 2000 + i;
  }
  EXPECT_GE(H5EA__dblock_unprotect(dblock, H5AC__DIRTIED_FLAG), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

  // Flush and evict cache, then reload read-only and verify contents.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);
  dblock = H5EA__dblock_protect(
      hdr, iblock, dblk_addr, /*dblk_nelmts=*/8, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblock, nullptr);
  const auto* read_elmts = static_cast<const uint64_t*>(dblock->elmts);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(read_elmts[i], 2000 + i);
  }

  // Dirty unprotect on a read-only protected data block must fail cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__dblock_unprotect(dblock, H5AC__DIRTIED_FLAG), 0);
  }
  EXPECT_GE(H5EA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);

  // Delete the unpaged data block directly.
  EXPECT_GE(H5EA__dblock_delete(hdr, iblock, dblk_addr, /*dblk_nelmts=*/8), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5EAdblockTest, PagedCreateAndDeleteExpungesCachedAndUncachedPages) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Set element 32 (sblk 3, dblk 0, page 0), leaving page 1 uncached/uninit.
  const uint64_t val = 3232;
  ASSERT_GE(H5EA_set(ea.get(), 32, &val), 0);

  H5EA_hdr_t* hdr = ea.get()->hdr;
  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);
  H5EA_sblock_t* sblock = H5EA__sblock_protect(
      hdr, iblock, iblock->sblk_addrs[1], 3, H5AC__NO_FLAGS_SET);
  ASSERT_NE(sblock, nullptr);

  // Delete dblk 0 (16 elmts = 2 pages): expunges cached page 0 and safely skips
  // missing page 1, then deletes dblock 0 and frees disk space.
  const haddr_t dblk0_addr = sblock->dblk_addrs[0];
  ASSERT_TRUE(H5_addr_defined(dblk0_addr));
  EXPECT_GE(H5EA__dblock_delete(hdr, sblock, dblk0_addr, sblock->dblk_nelmts),
            0);
  sblock->dblk_addrs[0] = HADDR_UNDEF;

  // Directly create another paged data block (16 elmts), verify paged state,
  // and delete it cleanly as well.
  bool stats_changed = false;
  const haddr_t dblk1_addr = H5EA__dblock_create(
      hdr, sblock, &stats_changed, /*dblk_off=*/48, /*nelmts=*/16);
  ASSERT_TRUE(H5_addr_defined(dblk1_addr));
  EXPECT_TRUE(stats_changed);

  H5EA_dblock_t* dblk1 = H5EA__dblock_protect(
      hdr, sblock, dblk1_addr, /*dblk_nelmts=*/16, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblk1, nullptr);
  EXPECT_EQ(dblk1->npages, 2u);
  EXPECT_EQ(dblk1->elmts, nullptr);
  const haddr_t dblk1_page0_addr =
      dblk1->addr + H5EA_DBLOCK_PREFIX_SIZE(dblk1);
  EXPECT_GE(H5EA__dblock_unprotect(dblk1, H5AC__NO_FLAGS_SET), 0);

  // Create and protect page 0 of dblk1 so H5EA__dblock_delete's page expunge
  // fails cleanly while deleting dblk1; then release and expunge page 0.
  ASSERT_GE(H5EA__dblk_page_create(hdr, sblock, dblk1_page0_addr), 0);
  H5EA_dblk_page_t* dblk1_page0 = H5EA__dblk_page_protect(
      hdr, sblock, dblk1_page0_addr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblk1_page0, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5EA__dblock_delete(hdr, sblock, dblk1_addr, /*dblk_nelmts=*/16), 0);
  }
  EXPECT_GE(H5EA__dblk_page_unprotect(dblk1_page0, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5AC_expunge_entry(f, H5AC_EARRAY_DBLK_PAGE, dblk1_page0_addr,
                               H5AC__NO_FLAGS_SET),
            0);

  EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__DIRTIED_FLAG), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5EAdblockTest, ErrorPathsUnprotectedHdrFillFailDecodeFailAndDeleteFail) {
  // Custom classes and callback contexts must outlive `file` because cached
  // H5EA headers remain referenced until H5Fflush / ~ScopedFile.
  const H5EA_class_t fail_fill_cls = MakeCustomClass(/*fail_fill=*/true);
  const H5EA_class_t codec_cls = MakeCustomClass(/*fail_fill=*/false);
  CodecControl fail_fill_ctrl{};
  CodecControl codec_ctrl{};

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Unprotected clean cached hdr (rc == 0) triggers H5EA__hdr_incr failure
  //    and H5EA__dblock_dest cleanup with dblock->hdr == nullptr.
  {
    const H5EA_create_t cparam = MakeTieredCparam();
    haddr_t empty_addr = HADDR_UNDEF;
    {
      ScopedEA ea(H5EA_create(f, &cparam, nullptr));
      ASSERT_NE(ea.get(), nullptr);
      ASSERT_GE(H5EA_get_addr(ea.get(), &empty_addr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5EA_hdr_t* unprot_hdr = H5EA__hdr_protect(
        f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(unprot_hdr, nullptr);
    ASSERT_EQ(unprot_hdr->rc, 0u);
    ASSERT_GE(H5EA__hdr_unprotect(unprot_hdr, H5AC__NO_FLAGS_SET), 0);

    bool stats_changed = false;
    int dummy_parent = 0;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__dblock_alloc(unprot_hdr, &dummy_parent, 8), nullptr);
    EXPECT_EQ(H5EA__dblock_create(unprot_hdr, &dummy_parent, &stats_changed, 8,
                                  8),
              HADDR_UNDEF);
    EXPECT_FALSE(stats_changed);
  }

  // 2. Failing fill callback in H5EA__dblock_create releases disk space via
  //    H5MF_xfree and destroys the unpaged dblock.
  {
    const H5EA_create_t cparam = MakeTieredCparam(&fail_fill_cls);
    ScopedEA ea(H5EA_create(f, &cparam, &fail_fill_ctrl));
    ASSERT_NE(ea.get(), nullptr);

    H5EA_hdr_t* hdr = ea.get()->hdr;
    bool stats_changed = false;
    int dummy_parent = 0;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__dblock_create(hdr, &dummy_parent, &stats_changed, 8, 8),
              HADDR_UNDEF);
    EXPECT_FALSE(stats_changed);
  }

  // 3. Failing decode callback on reload causes H5EA__dblock_protect and
  //    H5EA__dblock_delete to fail cleanly.
  {
    const H5EA_create_t cparam = MakeTieredCparam(&codec_cls);
    ScopedEA ea(H5EA_create(f, &cparam, &codec_ctrl));
    ASSERT_NE(ea.get(), nullptr);

    // Create an unpaged dblock in sblk 0 via H5EA_set(4).
    const uint64_t val = 44;
    ASSERT_GE(H5EA_set(ea.get(), 4, &val), 0);

    H5EA_hdr_t* hdr = ea.get()->hdr;
    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);
    const haddr_t dblk_addr = iblock->dblk_addrs[0];
    ASSERT_TRUE(H5_addr_defined(dblk_addr));
    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

    // Flush & evict while keeping `ea` pinned so custom `cls` stays active.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);

    codec_ctrl.fail_decode = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5EA__dblock_protect(hdr, iblock, dblk_addr, /*dblk_nelmts=*/4,
                                     H5AC__READ_ONLY_FLAG),
                nullptr);
      EXPECT_LT(
          H5EA__dblock_delete(hdr, iblock, dblk_addr, /*dblk_nelmts=*/4), 0);
    }
    codec_ctrl.fail_decode = false;

    // 4. H5AC_proxy_entry_add_child failure in H5EA__dblock_protect triggers
    //    error handling and H5AC_unprotect cleanup on protected dblock.
    {
      H5AC_proxy_entry_t* fake_proxy = H5AC_proxy_entry_create();
      ASSERT_NE(fake_proxy, nullptr);
      fake_proxy->addr = hdr->addr;  // Collides with cached header address.
      hdr->top_proxy = fake_proxy;
      absl::Cleanup restore_proxy([&]() {
        hdr->top_proxy = nullptr;
        EXPECT_GE(H5AC_proxy_entry_dest(fake_proxy), 0);
      });

      ScopedErrorSilence silence;
      EXPECT_EQ(H5EA__dblock_protect(hdr, iblock, dblk_addr, /*dblk_nelmts=*/4,
                                     H5AC__READ_ONLY_FLAG),
                nullptr);
    }

    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
  }

  // 5. H5EA__hdr_decr failure inside H5EA__dblock_dest when header is
  //    unpinned while rc == 1; re-pin and retry dest so no memory leaks.
  {
    const H5EA_create_t cparam = MakeTieredCparam();
    haddr_t empty_addr = HADDR_UNDEF;
    {
      ScopedEA ea(H5EA_create(f, &cparam, nullptr));
      ASSERT_NE(ea.get(), nullptr);
      ASSERT_GE(H5EA_get_addr(ea.get(), &empty_addr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5EA_hdr_t* hdr = H5EA__hdr_protect(
        f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    ASSERT_EQ(hdr->rc, 0u);
    int dummy_parent = 0;
    H5EA_dblock_t* dblock = H5EA__dblock_alloc(hdr, &dummy_parent, 8);
    ASSERT_NE(dblock, nullptr);
    ASSERT_EQ(hdr->rc, 1u);
    ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

    ASSERT_GE(H5AC_unpin_entry(hdr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__dblock_dest(dblock), 0);
    }

    hdr = H5EA__hdr_protect(f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    ASSERT_GE(H5EA__hdr_incr(hdr), 0);
    ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5EA__dblock_dest(dblock), 0);
  }
}

TEST(H5EAdblockTest, SwmrTopProxyAttachedOnCreateAndProtect) {
  TempFile temp_file("h5ea_dblock_swmr");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                 H5F_LIBVER_LATEST),
            0);

  ScopedFile file(H5Fcreate(temp_file.path().c_str(), H5F_ACC_TRUNC,
                            H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  haddr_t ea_addr = HADDR_UNDEF;
  haddr_t dblk_addr = HADDR_UNDEF;

  {
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    ASSERT_NE(ea.get()->hdr->top_proxy, nullptr);

    // Trigger creation of unpaged dblock in sblk 0 (index 4).
    const uint64_t val = 555;
    ASSERT_GE(H5EA_set(ea.get(), 4, &val), 0);

    H5EA_hdr_t* hdr = ea.get()->hdr;
    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);
    dblk_addr = iblock->dblk_addrs[0];
    ASSERT_TRUE(H5_addr_defined(dblk_addr));

    H5EA_dblock_t* dblock = H5EA__dblock_protect(
        hdr, iblock, dblk_addr, /*dblk_nelmts=*/4, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(dblock, nullptr);
    EXPECT_EQ(dblock->top_proxy, hdr->top_proxy);
    EXPECT_GE(H5EA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);

    // Also test direct H5EA__dblock_create and delete while top_proxy is set.
    bool stats_changed = false;
    const haddr_t extra_addr =
        H5EA__dblock_create(hdr, iblock, &stats_changed, /*dblk_off=*/8,
                            /*nelmts=*/8);
    ASSERT_TRUE(H5_addr_defined(extra_addr));
    EXPECT_GE(H5EA__dblock_delete(hdr, iblock, extra_addr, /*dblk_nelmts=*/8),
              0);

    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
  }

  // Flush and evict everything so reloaded dblock starts with top_proxy == null
  // and H5EA__dblock_protect attaches it to hdr->top_proxy.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedEA reopened(H5EA_open(f, ea_addr, nullptr));
  ASSERT_NE(reopened.get(), nullptr);
  H5EA_hdr_t* hdr = reopened.get()->hdr;
  ASSERT_NE(hdr->top_proxy, nullptr);

  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);

  H5EA_dblock_t* dblock = H5EA__dblock_protect(
      hdr, iblock, dblk_addr, /*dblk_nelmts=*/4, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblock, nullptr);
  EXPECT_EQ(dblock->top_proxy, hdr->top_proxy);
  EXPECT_EQ(static_cast<const uint64_t*>(dblock->elmts)[0], 555u);

  EXPECT_GE(H5EA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

}  // namespace
}  // namespace hdf5
