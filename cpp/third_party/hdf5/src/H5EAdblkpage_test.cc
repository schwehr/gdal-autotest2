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
// Tests extensible array data block page allocation, creation, protection,
// unprotection, destruction, error cleanup paths, and SWMR top_proxy
// attachment (third_party/hdf5/src/H5EAdblkpage.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAdblkpage.c

#include <cstdint>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5EA_hdr_t, H5EA_sblock_t, H5EA_dblock_t, and
// H5EA_dblk_page_t). Define NDEBUG around internal headers so C++ struct
// layouts match `:hdf5`.
#ifndef NDEBUG
#define H5EADBLKPAGE_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5EADBLKPAGE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EADBLKPAGE_TEST_RESTORE_UNDEF_NDEBUG
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
  cls.name = "H5EAdblkpage_custom";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = CodecCrtContext;
  cls.dst_context = CodecDstContext;
  cls.fill = fail_fill ? FailingFill : CodecFill;
  cls.encode = CodecEncode;
  cls.decode = CodecDecode;
  return cls;
}

H5EA_create_t MakePagedCparam(const H5EA_class_t* cls = H5EA_CLS_TEST) {
  // With idx_blk_elmts=4, data_blk_min_elmts=4, sup_blk_min_data_ptrs=2, and
  // max_dblk_page_nelmts_bits=3 (8 elmts/page):
  //   sblk 3 covers indices [32..63] with 2 paged dblocks of 16 elmts (2 pages
  //   each, 8 elmts/page).
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

// Helper to protect the super block containing index 32 (sblk 3) and the
// first paged data block (dblk 0 in sblk 3), computing page 0 and page 1 file
// addresses.
struct PagedDblkLocations {
  haddr_t dblk_addr = HADDR_UNDEF;
  haddr_t page0_addr = HADDR_UNDEF;
  haddr_t page1_addr = HADDR_UNDEF;
};

PagedDblkLocations ComputePagedDblkLocations(H5EA_hdr_t* hdr,
                                             H5EA_sblock_t* sblk) {
  PagedDblkLocations loc{};
  loc.dblk_addr = sblk->dblk_addrs[0];
  H5EA_dblock_t* dblock = H5EA__dblock_protect(
      hdr, sblk, loc.dblk_addr, sblk->dblk_nelmts, H5AC__READ_ONLY_FLAG);
  EXPECT_NE(dblock, nullptr);
  if (dblock != nullptr) {
    loc.page0_addr = dblock->addr + H5EA_DBLOCK_PREFIX_SIZE(dblock);
    loc.page1_addr = loc.page0_addr + sblk->dblk_page_size;
    EXPECT_GE(H5EA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);
  }
  return loc;
}

TEST(H5EAdblkpageTest, AllocAndDestLifecycle) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakePagedCparam();
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Trigger creation of superblock 3 so hdr->elmt_fac and sblock exist.
  const uint64_t val = 101;
  ASSERT_GE(H5EA_set(ea.get(), 32, &val), 0);

  H5EA_hdr_t* hdr = ea.get()->hdr;
  ASSERT_NE(hdr, nullptr);
  const size_t rc_before = hdr->rc;

  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);
  H5EA_sblock_t* sblock = H5EA__sblock_protect(
      hdr, iblock, iblock->sblk_addrs[1], 3, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(sblock, nullptr);

  H5EA_dblk_page_t* page = H5EA__dblk_page_alloc(hdr, sblock);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->hdr, hdr);
  EXPECT_EQ(page->parent, sblock);
  EXPECT_NE(page->elmts, nullptr);
  EXPECT_EQ(hdr->rc, rc_before + 1);

  EXPECT_GE(H5EA__dblk_page_dest(page), 0);
  EXPECT_EQ(hdr->rc, rc_before);

  EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5EAdblkpageTest, CreateProtectModifyFlushEvictAndReload) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakePagedCparam();
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Setting element 32 creates paged dblock 0 and page 0, leaving page 1
  // uncreated on disk.
  const uint64_t val0 = 0x1111222233334444ULL;
  ASSERT_GE(H5EA_set(ea.get(), 32, &val0), 0);

  H5EA_hdr_t* hdr = ea.get()->hdr;
  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);
  H5EA_sblock_t* sblock = H5EA__sblock_protect(
      hdr, iblock, iblock->sblk_addrs[1], 3, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(sblock, nullptr);

  const PagedDblkLocations loc = ComputePagedDblkLocations(hdr, sblock);
  ASSERT_TRUE(H5_addr_defined(loc.page0_addr));
  ASSERT_TRUE(H5_addr_defined(loc.page1_addr));

  // Directly create page 1, protect it read/write, populate elements, unprotect
  // dirtied, flush, evict cache, and verify persisted data on reload.
  ASSERT_GE(H5EA__dblk_page_create(hdr, sblock, loc.page1_addr), 0);

  H5EA_dblk_page_t* page1 = H5EA__dblk_page_protect(
      hdr, sblock, loc.page1_addr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(page1, nullptr);
  auto* elmts = static_cast<uint64_t*>(page1->elmts);
  for (size_t i = 0; i < hdr->dblk_page_nelmts; ++i) {
    EXPECT_EQ(elmts[i], H5EA_TEST_FILL);
    elmts[i] = 5000 + i;
  }
  EXPECT_GE(H5EA__dblk_page_unprotect(page1, H5AC__DIRTIED_FLAG), 0);

  // Unprotect parents before flush & eviction.
  EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  // Re-protect parents and reload page 1 with READ_ONLY_FLAG.
  iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);
  sblock = H5EA__sblock_protect(hdr, iblock, iblock->sblk_addrs[1], 3,
                                H5AC__READ_ONLY_FLAG);
  ASSERT_NE(sblock, nullptr);

  page1 = H5EA__dblk_page_protect(hdr, sblock, loc.page1_addr,
                                  H5AC__READ_ONLY_FLAG);
  ASSERT_NE(page1, nullptr);
  const auto* read_elmts = static_cast<const uint64_t*>(page1->elmts);
  for (size_t i = 0; i < hdr->dblk_page_nelmts; ++i) {
    EXPECT_EQ(read_elmts[i], 5000 + i);
  }

  // Attempting to dirty a read-only protected page causes unprotect to fail.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__dblk_page_unprotect(page1, H5AC__DIRTIED_FLAG), 0);
  }
  // Cleanly release the read-only protection.
  EXPECT_GE(H5EA__dblk_page_unprotect(page1, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5EAdblkpageTest, ErrorPathsUnprotectedHdrFillDecodeAndDuplicateInsert) {
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

  // 1. H5EA__hdr_incr failure on unprotected clean cached hdr with rc == 0.
  {
    const H5EA_create_t cparam = MakePagedCparam();
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

    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__dblk_page_alloc(unprot_hdr, nullptr), nullptr);
    EXPECT_LT(H5EA__dblk_page_create(unprot_hdr, nullptr, 0x2000), 0);
  }

  // 2. Failing fill callback in H5EA__dblk_page_create.
  {
    const H5EA_create_t cparam = MakePagedCparam(&fail_fill_cls);
    ScopedEA ea(H5EA_create(f, &cparam, &fail_fill_ctrl));
    ASSERT_NE(ea.get(), nullptr);

    H5EA_hdr_t* hdr = ea.get()->hdr;
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__dblk_page_create(hdr, nullptr, 0x3000), 0);
  }

  // 3. Duplicate insert in H5EA__dblk_page_create & decode failure in protect.
  {
    const H5EA_create_t cparam = MakePagedCparam(&codec_cls);
    ScopedEA ea(H5EA_create(f, &cparam, &codec_ctrl));
    ASSERT_NE(ea.get(), nullptr);

    const uint64_t val = 77;
    ASSERT_GE(H5EA_set(ea.get(), 32, &val), 0);

    H5EA_hdr_t* hdr = ea.get()->hdr;
    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);
    H5EA_sblock_t* sblock = H5EA__sblock_protect(
        hdr, iblock, iblock->sblk_addrs[1], 3, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(sblock, nullptr);

    const PagedDblkLocations loc = ComputePagedDblkLocations(hdr, sblock);
    ASSERT_TRUE(H5_addr_defined(loc.page0_addr));

    // Duplicate cache insertion at page0_addr fails cleanly and destroys page.
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__dblk_page_create(hdr, sblock, loc.page0_addr), 0);
    }

    EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

    // Flush & evict while keeping `ea` pinned so custom `cls` stays active.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);
    sblock = H5EA__sblock_protect(hdr, iblock, iblock->sblk_addrs[1], 3,
                                  H5AC__READ_ONLY_FLAG);
    ASSERT_NE(sblock, nullptr);

    codec_ctrl.fail_decode = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5EA__dblk_page_protect(hdr, sblock, loc.page0_addr,
                                        H5AC__READ_ONLY_FLAG),
                nullptr);
    }
    codec_ctrl.fail_decode = false;

    // 4. H5AC_proxy_entry_add_child failure in H5EA__dblk_page_protect triggers
    //    error handling and H5AC_unprotect cleanup on protected dblk_page.
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
      EXPECT_EQ(H5EA__dblk_page_protect(hdr, sblock, loc.page0_addr,
                                        H5AC__READ_ONLY_FLAG),
                nullptr);
    }

    EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
  }

  // 5. H5EA__hdr_decr failure inside H5EA__dblk_page_dest when header was
  //    unexpectedly unpinned while rc == 1; re-pin and finish destroying cleanly.
  {
    const H5EA_create_t cparam = MakePagedCparam();
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
    H5EA_dblk_page_t* page = H5EA__dblk_page_alloc(hdr, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_EQ(hdr->rc, 1u);
    ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

    // Unpin header so H5EA__hdr_decr inside dest returns failure.
    ASSERT_GE(H5AC_unpin_entry(hdr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__dblk_page_dest(page), 0);
    }

    // Re-pin header and retry dest so no memory is leaked.
    hdr = H5EA__hdr_protect(f, empty_addr, nullptr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    ASSERT_GE(H5EA__hdr_incr(hdr), 0);
    ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5EA__dblk_page_dest(page), 0);
  }
}

TEST(H5EAdblkpageTest, SwmrTopProxyAttachedOnCreateAndProtect) {
  TempFile temp_file("h5ea_dblkpage_swmr");
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

  const H5EA_create_t cparam = MakePagedCparam();
  haddr_t ea_addr = HADDR_UNDEF;
  PagedDblkLocations loc{};

  {
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    ASSERT_NE(ea.get()->hdr->top_proxy, nullptr);

    // Setting element 32 creates page 0 in SWMR mode.
    const uint64_t val0 = 9001;
    ASSERT_GE(H5EA_set(ea.get(), 32, &val0), 0);

    H5EA_hdr_t* hdr = ea.get()->hdr;
    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);
    H5EA_sblock_t* sblock = H5EA__sblock_protect(
        hdr, iblock, iblock->sblk_addrs[1], 3, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(sblock, nullptr);

    loc = ComputePagedDblkLocations(hdr, sblock);
    ASSERT_TRUE(H5_addr_defined(loc.page0_addr));
    ASSERT_TRUE(H5_addr_defined(loc.page1_addr));

    // Directly create page 1 with hdr->top_proxy active.
    ASSERT_GE(H5EA__dblk_page_create(hdr, sblock, loc.page1_addr), 0);
    H5EA_dblk_page_t* page1 = H5EA__dblk_page_protect(
        hdr, sblock, loc.page1_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(page1, nullptr);
    EXPECT_EQ(page1->top_proxy, hdr->top_proxy);
    EXPECT_GE(H5EA__dblk_page_unprotect(page1, H5AC__NO_FLAGS_SET), 0);

    EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
  }

  // Flush and evict everything so reloaded page has top_proxy == nullptr and
  // H5EA__dblk_page_protect attaches it to hdr->top_proxy.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedEA reopened(H5EA_open(f, ea_addr, nullptr));
  ASSERT_NE(reopened.get(), nullptr);
  H5EA_hdr_t* hdr = reopened.get()->hdr;
  ASSERT_NE(hdr->top_proxy, nullptr);

  H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(iblock, nullptr);
  H5EA_sblock_t* sblock = H5EA__sblock_protect(
      hdr, iblock, iblock->sblk_addrs[1], 3, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(sblock, nullptr);

  H5EA_dblk_page_t* page0 = H5EA__dblk_page_protect(
      hdr, sblock, loc.page0_addr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(page0, nullptr);
  EXPECT_EQ(page0->top_proxy, hdr->top_proxy);
  EXPECT_EQ(static_cast<const uint64_t*>(page0->elmts)[0], 9001u);

  EXPECT_GE(H5EA__dblk_page_unprotect(page0, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__sblock_unprotect(sblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);
}

}  // namespace
}  // namespace hdf5
