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
// Tests fixed array data block allocation, creation, protection, unprotection,
// unpaged and paged deletion, error paths, and SWMR top_proxy attachment
// (third_party/hdf5/src/H5FAdblock.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FAdblock.c

#include <cstddef>
#include <cstdint>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/H5FDdevelop.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FA_hdr_t and H5FA_dblock_t). Define NDEBUG around internal
// headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FADBLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5FADBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FADBLOCK_TEST_RESTORE_UNDEF_NDEBUG
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
  bool fail_decode = false;
};

void* CodecCrtContext(void* udata) { return udata; }
herr_t CodecDstContext(void* /*ctx*/) { return 0; }

herr_t FailingFill(void* /*nat_blk*/, size_t /*nelmts*/) { return -1; }

herr_t CodecFill(void* nat_blk, size_t nelmts) {
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = H5FA_TEST_FILL;
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

H5FA_class_t MakeCustomClass(bool fail_fill) {
  H5FA_class_t cls{};
  cls.id = H5FA_CLS_TEST_ID;
  cls.name = "H5FAdblock_custom";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = CodecCrtContext;
  cls.dst_context = CodecDstContext;
  cls.fill = fail_fill ? FailingFill : CodecFill;
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

TEST(H5FAdblockTest, AllocAndDestUnpagedAndPagedExactAndPartial) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Unpaged (nelmts=6 <= dblk_page_nelmts=8).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/6, /*page_bits=*/3);
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;
    const size_t rc_before = hdr->rc;

    H5FA_dblock_t* unpaged = H5FA__dblock_alloc(hdr);
    ASSERT_NE(unpaged, nullptr);
    EXPECT_EQ(unpaged->hdr, hdr);
    EXPECT_EQ(unpaged->npages, 0u);
    EXPECT_NE(unpaged->elmts, nullptr);
    EXPECT_EQ(unpaged->dblk_page_init, nullptr);
    EXPECT_EQ(hdr->rc, rc_before + 1);
    EXPECT_GE(H5FA__dblock_dest(unpaged), 0);
    EXPECT_EQ(hdr->rc, rc_before);
  }

  // 2. Paged exact multiple (nelmts=16, 2 pages, last_page_nelmts=8).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/16, /*page_bits=*/3);
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;
    const size_t rc_before = hdr->rc;

    H5FA_dblock_t* paged_exact = H5FA__dblock_alloc(hdr);
    ASSERT_NE(paged_exact, nullptr);
    EXPECT_EQ(paged_exact->npages, 2u);
    EXPECT_EQ(paged_exact->last_page_nelmts, 8u);
    EXPECT_EQ(paged_exact->elmts, nullptr);
    EXPECT_NE(paged_exact->dblk_page_init, nullptr);
    EXPECT_EQ(hdr->rc, rc_before + 1);
    EXPECT_GE(H5FA__dblock_dest(paged_exact), 0);
    EXPECT_EQ(hdr->rc, rc_before);
  }

  // 3. Paged partial last page (nelmts=20, 3 pages, last_page_nelmts=4).
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;

    H5FA_dblock_t* paged_partial = H5FA__dblock_alloc(hdr);
    ASSERT_NE(paged_partial, nullptr);
    EXPECT_EQ(paged_partial->npages, 3u);
    EXPECT_EQ(paged_partial->last_page_nelmts, 4u);
    EXPECT_EQ(paged_partial->elmts, nullptr);
    EXPECT_NE(paged_partial->dblk_page_init, nullptr);
    EXPECT_GE(H5FA__dblock_dest(paged_partial), 0);
  }
}

TEST(H5FAdblockTest, UnpagedCreateProtectFlushEvictReloadAndDelete) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/3);
  ScopedFA fa(H5FA_create(f, &cparam, nullptr));
  ASSERT_NE(fa.get(), nullptr);
  H5FA_hdr_t* hdr = fa.get()->hdr;

  bool hdr_dirty = false;
  const haddr_t dblk_addr = H5FA__dblock_create(hdr, &hdr_dirty);
  ASSERT_TRUE(H5_addr_defined(dblk_addr));
  EXPECT_TRUE(hdr_dirty);
  hdr->dblk_addr = dblk_addr;
  ASSERT_GE(H5FA__hdr_modified(hdr), 0);

  // Protect RW, verify fill values, populate elements, unprotect dirtied.
  H5FA_dblock_t* dblock =
      H5FA__dblock_protect(hdr, dblk_addr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(dblock, nullptr);
  auto* elmts = static_cast<uint64_t*>(dblock->elmts);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(elmts[i], H5FA_TEST_FILL);
    elmts[i] = 2000 + i;
  }
  EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__DIRTIED_FLAG), 0);

  // Flush and evict cache, then reload read-only and verify contents.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  dblock = H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblock, nullptr);
  const auto* read_elmts = static_cast<const uint64_t*>(dblock->elmts);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(read_elmts[i], 2000 + i);
  }

  // Dirty unprotect on read-only protected data block fails cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__dblock_unprotect(dblock, H5AC__DIRTIED_FLAG), 0);
  }
  EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);

  // Delete unpaged data block directly and clear hdr->dblk_addr.
  EXPECT_GE(H5FA__dblock_delete(hdr, dblk_addr), 0);
  hdr->dblk_addr = HADDR_UNDEF;
  EXPECT_GE(H5FA__hdr_modified(hdr), 0);
}

TEST(H5FAdblockTest, PagedCreateAndDeleteExpungesCachedAndUncachedPages) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;

    // Set idx 0 (creates dblock + page 0, leaving pages 1 and 2 uncached).
    const uint64_t val = 3232;
    ASSERT_GE(H5FA_set(fa.get(), 0, &val), 0);

    // Delete expunges cached page 0 and safely skips missing pages 1 and 2.
    const haddr_t dblk_addr = hdr->dblk_addr;
    ASSERT_TRUE(H5_addr_defined(dblk_addr));
    EXPECT_GE(H5FA__dblock_delete(hdr, dblk_addr), 0);
    hdr->dblk_addr = HADDR_UNDEF;
    EXPECT_GE(H5FA__hdr_modified(hdr), 0);
  }

  // Directly create another paged data block, protect its page 0 so
  // H5FA__dblock_delete fails while expunging, then clean up cleanly.
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    H5FA_hdr_t* hdr = fa.get()->hdr;

    bool hdr_dirty = false;
    const haddr_t dblk_addr = H5FA__dblock_create(hdr, &hdr_dirty);
    ASSERT_TRUE(H5_addr_defined(dblk_addr));
    EXPECT_TRUE(hdr_dirty);

    H5FA_dblock_t* dblock =
        H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(dblock, nullptr);
    EXPECT_EQ(dblock->npages, 3u);
    EXPECT_EQ(dblock->elmts, nullptr);
    const haddr_t page0_addr = dblock->addr + H5FA_DBLOCK_PREFIX_SIZE(dblock);
    EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);

    ASSERT_GE(H5FA__dblk_page_create(hdr, page0_addr, 8), 0);
    H5FA_dblk_page_t* page0 =
        H5FA__dblk_page_protect(hdr, page0_addr, 8, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(page0, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5FA__dblock_delete(hdr, dblk_addr), 0);
    }
    EXPECT_GE(H5FA__dblk_page_unprotect(page0, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5AC_expunge_entry(f, H5AC_FARRAY_DBLK_PAGE, page0_addr,
                                 H5AC__NO_FLAGS_SET),
              0);
  }
}

TEST(H5FAdblockTest, ErrorPathsUnprotectedHdrFillDecodeProxyAndDest) {
  const H5FA_class_t fail_fill_cls = MakeCustomClass(/*fail_fill=*/true);
  const H5FA_class_t codec_cls = MakeCustomClass(/*fail_fill=*/false);
  CodecControl fail_fill_ctrl{};
  CodecControl codec_ctrl{};

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Unprotected clean cached hdr (rc == 0) fails H5FA__hdr_incr.
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/3);
    haddr_t empty_addr = HADDR_UNDEF;
    {
      ScopedFA fa(H5FA_create(f, &cparam, nullptr));
      ASSERT_NE(fa.get(), nullptr);
      ASSERT_GE(H5FA_get_addr(fa.get(), &empty_addr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5FA_hdr_t* unprot_hdr =
        H5FA__hdr_protect(f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(unprot_hdr, nullptr);
    ASSERT_EQ(unprot_hdr->rc, 0u);
    ASSERT_GE(H5FA__hdr_unprotect(unprot_hdr, H5AC__NO_FLAGS_SET), 0);

    bool hdr_dirty = false;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA__dblock_alloc(unprot_hdr), nullptr);
    EXPECT_EQ(H5FA__dblock_create(unprot_hdr, &hdr_dirty), HADDR_UNDEF);
    EXPECT_FALSE(hdr_dirty);
  }

  // 2. Failing fill callback in H5FA__dblock_create frees disk space via
  //    H5MF_xfree and destroys the unpaged data block.
  {
    const H5FA_create_t cparam = MakeCparam(8, 3, &fail_fill_cls);
    ScopedFA fa(H5FA_create(f, &cparam, &fail_fill_ctrl));
    ASSERT_NE(fa.get(), nullptr);

    H5FA_hdr_t* hdr = fa.get()->hdr;
    bool hdr_dirty = false;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA__dblock_create(hdr, &hdr_dirty), HADDR_UNDEF);
    EXPECT_FALSE(hdr_dirty);
  }

  // 3. Failing decode callback on reload fails H5FA__dblock_protect and
  //    H5FA__dblock_delete, and proxy error fails H5FA__dblock_protect.
  {
    const H5FA_create_t cparam = MakeCparam(8, 3, &codec_cls);
    ScopedFA fa(H5FA_create(f, &cparam, &codec_ctrl));
    ASSERT_NE(fa.get(), nullptr);

    const uint64_t val = 44;
    ASSERT_GE(H5FA_set(fa.get(), 0, &val), 0);
    H5FA_hdr_t* hdr = fa.get()->hdr;
    const haddr_t dblk_addr = hdr->dblk_addr;
    ASSERT_TRUE(H5_addr_defined(dblk_addr));

    // Flush & evict while keeping `fa` pinned so custom `cls` stays active.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    codec_ctrl.fail_decode = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG),
                nullptr);
      EXPECT_LT(H5FA__dblock_delete(hdr, dblk_addr), 0);
    }
    codec_ctrl.fail_decode = false;

    // 4. H5AC_proxy_entry_add_child failure in H5FA__dblock_protect cleans up
    //    protected dblock via H5AC_unprotect.
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
      EXPECT_EQ(H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG),
                nullptr);
    }
  }

  // 5. H5FA__hdr_decr failure inside H5FA__dblock_dest when header is unpinned
  //    while rc == 1; re-pin and retry dest so no memory leaks.
  {
    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/3);
    haddr_t empty_addr = HADDR_UNDEF;
    {
      ScopedFA fa(H5FA_create(f, &cparam, nullptr));
      ASSERT_NE(fa.get(), nullptr);
      ASSERT_GE(H5FA_get_addr(fa.get(), &empty_addr), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5FA_hdr_t* hdr =
        H5FA__hdr_protect(f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    ASSERT_EQ(hdr->rc, 0u);
    H5FA_dblock_t* dblock = H5FA__dblock_alloc(hdr);
    ASSERT_NE(dblock, nullptr);
    ASSERT_EQ(hdr->rc, 1u);
    ASSERT_GE(H5FA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

    ASSERT_GE(H5AC_unpin_entry(hdr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5FA__dblock_dest(dblock), 0);
    }

    hdr = H5FA__hdr_protect(f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    ASSERT_GE(H5FA__hdr_incr(hdr), 0);
    ASSERT_GE(H5FA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5FA__dblock_dest(dblock), 0);
  }
}

TEST(H5FAdblockTest, SwmrTopProxyAttachedOnCreateAndProtect) {
  TempFile temp_file("h5fa_dblock_swmr.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/3);
  haddr_t fa_addr = HADDR_UNDEF;
  haddr_t dblk_addr = HADDR_UNDEF;

  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);
    ASSERT_NE(fa.get()->hdr->top_proxy, nullptr);

    const uint64_t val = 555;
    ASSERT_GE(H5FA_set(fa.get(), 0, &val), 0);

    H5FA_hdr_t* hdr = fa.get()->hdr;
    dblk_addr = hdr->dblk_addr;
    ASSERT_TRUE(H5_addr_defined(dblk_addr));

    H5FA_dblock_t* dblock =
        H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(dblock, nullptr);
    EXPECT_EQ(dblock->top_proxy, hdr->top_proxy);
    EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);
  }

  // Flush and evict so reloaded dblock starts with top_proxy == nullptr and
  // H5FA__dblock_protect attaches it to hdr->top_proxy.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedFA reopened(H5FA_open(f, fa_addr, nullptr));
  ASSERT_NE(reopened.get(), nullptr);
  H5FA_hdr_t* hdr = reopened.get()->hdr;
  ASSERT_NE(hdr->top_proxy, nullptr);

  H5FA_dblock_t* dblock =
      H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblock, nullptr);
  EXPECT_EQ(dblock->top_proxy, hdr->top_proxy);
  EXPECT_EQ(static_cast<const uint64_t*>(dblock->elmts)[0], 555u);

  EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5FAdblockTest, FileAllocAndDuplicateCacheInsertErrorsInCreate) {
  // Use H5F_FSPACE_STRATEGY_NONE so H5MF_alloc invokes H5FD__extend directly
  // without aggregator padding and allocates right at EOA.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                 0),
      0);
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(H5Fcreate("h5fa_dblock_err.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/3);
  ScopedFA fa(H5FA_create(f, &cparam, nullptr));
  ASSERT_NE(fa.get(), nullptr);
  H5FA_hdr_t* hdr = fa.get()->hdr;

  // 1. Lower VFD maxaddr below EOA + dblock->size so H5MF_alloc fails cleanly.
  {
    H5FD_t* lf = nullptr;
    ASSERT_GE(H5F_shared_get_file_driver(H5F_get_shared(f), &lf), 0);
    ASSERT_NE(lf, nullptr);
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_FARRAY_DBLOCK);
    ASSERT_TRUE(H5_addr_defined(eoa));
    const haddr_t saved_maxaddr = lf->maxaddr;
    lf->maxaddr = eoa + 1;
    absl::Cleanup restore_maxaddr([lf, saved_maxaddr] {
      lf->maxaddr = saved_maxaddr;
    });

    bool hdr_dirty = false;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA__dblock_create(hdr, &hdr_dirty), HADDR_UNDEF);
    EXPECT_FALSE(hdr_dirty);
  }

  // 2. Pre-populate cache at EOA so H5AC_insert_entry fails cleanly and frees
  //    disk space via H5MF_xfree.
  {
    const haddr_t next_eoa = H5F_get_eoa(f, H5FD_MEM_FARRAY_DBLOCK);
    ASSERT_TRUE(H5_addr_defined(next_eoa));

    H5AC_proxy_entry_t* dummy = H5AC_proxy_entry_create();
    ASSERT_NE(dummy, nullptr);
    bool dummy_in_cache = false;
    bool dummy_pinned = false;
    absl::Cleanup cleanup_dummy([&] {
      if (dummy_pinned) {
        EXPECT_GE(H5AC_unpin_entry(dummy), 0);
      }
      if (dummy_in_cache) {
        EXPECT_GE(H5AC_remove_entry(dummy), 0);
      }
      EXPECT_GE(H5AC_proxy_entry_dest(dummy), 0);
    });

    ASSERT_GE(H5AC_insert_entry(f, H5AC_PROXY_ENTRY, next_eoa, dummy,
                                H5AC__PIN_ENTRY_FLAG),
              0);
    dummy_in_cache = true;
    dummy_pinned = true;
    ASSERT_GE(H5AC_mark_entry_clean(dummy), 0);
    ASSERT_GE(H5AC_mark_entry_serialized(dummy), 0);

    bool hdr_dirty = false;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA__dblock_create(hdr, &hdr_dirty), HADDR_UNDEF);
    EXPECT_FALSE(hdr_dirty);
  }
}

}  // namespace
}  // namespace hdf5
