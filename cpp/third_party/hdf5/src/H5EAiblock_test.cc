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
// Tests extensible array index block allocation, creation, protection,
// unprotection, SWMR proxy attachment, recursive deletion, cleanup, and error
// handling (third_party/hdf5/src/H5EAiblock.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAiblock.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5C_cache_entry_t
// (first member of H5EA_hdr_t / H5EA_iblock_t). Define NDEBUG around internal
// headers so C++ struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5EAIBLOCK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5EA_FRIEND
#define H5EA_TESTING
#define H5F_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5EAIBLOCK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EAIBLOCK_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedEa {
 public:
  ScopedEa() = default;
  explicit ScopedEa(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEa() { reset(); }

  ScopedEa(const ScopedEa&) = delete;
  ScopedEa& operator=(const ScopedEa&) = delete;

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

struct FillControl {
  bool fail_fill = false;
};

FillControl g_fill_control;

herr_t ControlledFill(void* nat_blk, size_t nelmts) {
  if (g_fill_control.fail_fill) {
    return -1;
  }
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = UINT64_MAX;
  }
  return 0;
}

herr_t RawEncode(void* raw, const void* elmt, size_t nelmts, void* /*ctx*/) {
  auto* out = static_cast<uint8_t*>(raw);
  const auto* in = static_cast<const uint64_t*>(elmt);
  for (size_t i = 0; i < nelmts; ++i) {
    for (int b = 0; b < 8; ++b) {
      out[i * 8 + b] = static_cast<uint8_t>((in[i] >> (b * 8)) & 0xffU);
    }
  }
  return 0;
}

herr_t RawDecode(const void* raw, void* elmt, size_t nelmts, void* /*ctx*/) {
  const auto* in = static_cast<const uint8_t*>(raw);
  auto* out = static_cast<uint64_t*>(elmt);
  for (size_t i = 0; i < nelmts; ++i) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) {
      v |= static_cast<uint64_t>(in[i * 8 + b]) << (b * 8);
    }
    out[i] = v;
  }
  return 0;
}

H5EA_class_t MakeControlledClass() {
  H5EA_class_t cls{};
  cls.id = H5EA_CLS_TEST_ID;
  cls.name = "H5EAiblock_ctrl";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = nullptr;
  cls.dst_context = nullptr;
  cls.fill = ControlledFill;
  cls.encode = RawEncode;
  cls.decode = RawDecode;
  return cls;
}

H5EA_create_t MakeParams(const H5EA_class_t* cls, uint8_t max_nelmts_bits,
                         uint8_t idx_blk_elmts) {
  H5EA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = max_nelmts_bits;
  cparam.idx_blk_elmts = idx_blk_elmts;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 4;
  cparam.max_dblk_page_nelmts_bits = 4;
  return cparam;
}

ScopedFile CreateSwmrTempFile(const TempFile& tmp_file) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST) <
      0) {
    return ScopedFile();
  }
  ScopedFile file(
      H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  if (!file.valid()) {
    return ScopedFile();
  }
  if (H5Fstart_swmr_write(file.get()) < 0) {
    return ScopedFile();
  }
  return file;
}

TEST(H5EAiblockTest, AllocCreateProtectUnprotectVariants) {
  ScopedFile file = CreateInMemoryFile("h5eaiblock_variants.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // Variant A: idx_blk_elmts == 0 AND nsblk_addrs == 0 (max_nelmts_bits = 5 =>
  // hdr->nsblks == 4 == iblock->nsblks).
  {
    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 5, 0);
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));

    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    bool stats_changed = false;
    hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));
    EXPECT_TRUE(stats_changed);

    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(iblock, nullptr);
    EXPECT_EQ(iblock->elmts, nullptr);
    EXPECT_NE(iblock->dblk_addrs, nullptr);
    EXPECT_EQ(iblock->nsblk_addrs, 0u);
    EXPECT_EQ(iblock->sblk_addrs, nullptr);
    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }

  // Variant B: idx_blk_elmts > 0 AND nsblk_addrs > 0 (max_nelmts_bits = 10).
  {
    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));

    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    bool stats_changed = false;
    hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));

    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(iblock, nullptr);
    EXPECT_NE(iblock->elmts, nullptr);
    EXPECT_NE(iblock->dblk_addrs, nullptr);
    EXPECT_GT(iblock->nsblk_addrs, 0u);
    EXPECT_NE(iblock->sblk_addrs, nullptr);
    EXPECT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

    // Direct H5EA__iblock_alloc + H5EA__iblock_dest round-trip.
    H5EA_iblock_t* scratch_ib = H5EA__iblock_alloc(hdr);
    ASSERT_NE(scratch_ib, nullptr);
    EXPECT_GE(H5EA__iblock_dest(scratch_ib), 0);

    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }
}

TEST(H5EAiblockTest, DeleteRemovesEmptyAndPopulatedChildrenAcrossNonSwmrAndSwmr) {
  // 1. Delete populated non-SWMR array with elements in iblock, direct dblocks
  // (spanning multiple sblk_info entries), and external superblocks.
  {
    ScopedFile file = CreateInMemoryFile("h5eaiblock_del.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);
    ScopedEa ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);

    // Populate index 1 (iblock elmts), 4 (sblk 0 dblk), 18 (sblk 2 dblk), 64
    // (sblk 4 external sblk), and 128 (sblk 5 paged external sblk), leaving some
    // slots undefined.
    for (hsize_t idx :
         {hsize_t{1}, hsize_t{4}, hsize_t{18}, hsize_t{64}, hsize_t{128}}) {
      uint64_t val = idx + 100;
      ASSERT_GE(H5EA_set(ea.get(), idx, &val), 0);
    }

    haddr_t ea_addr = HADDR_UNDEF;
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    ea.reset();

    EXPECT_GE(H5EA_delete(f, ea_addr, nullptr), 0);
  }

  // 2. SWMR create, non-SWMR write -> SWMR reopen reload (hits
  // H5EA__iblock_protect top_proxy attachment), and SWMR delete.
  TempFile tmp_file("h5eaiblock_swmr.h5");
  haddr_t saved_addr = HADDR_UNDEF;
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);
    ScopedEa ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    uint64_t val = 55;
    ASSERT_GE(H5EA_set(ea.get(), 4, &val), 0);
    ASSERT_GE(H5EA_get_addr(ea.get(), &saved_addr), 0);
  }

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fopen(tmp_file.c_str(),
                            H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedEa ea(H5EA_open(f, saved_addr, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    uint64_t out = 0;
    ASSERT_GE(H5EA_get(ea.get(), 4, &out), 0);
    EXPECT_EQ(out, 55u);
    ea.reset();

    // Create a second EA directly under SWMR_WRITE, exercise iblock_create +
    // iblock_delete while top_proxy is active.
    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);
    ScopedEa ea2(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea2.get(), nullptr);
    uint64_t v2 = 99;
    ASSERT_GE(H5EA_set(ea2.get(), 2, &v2), 0);
    haddr_t ea2_addr = HADDR_UNDEF;
    ASSERT_GE(H5EA_get_addr(ea2.get(), &ea2_addr), 0);
    ea2.reset();

    EXPECT_GE(H5EA_delete(f, ea2_addr, nullptr), 0);
  }

  // 3. Verify direct H5EA__iblock_create / H5EA__iblock_delete in a file
  // transitioned via H5Fstart_swmr_write.
  {
    TempFile swmr_start_file("h5eaiblock_swmr_start.h5");
    ScopedFile file = CreateSwmrTempFile(swmr_start_file);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));
    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    bool stats_changed = false;
    hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));
    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }

  // 4. Verify H5EA__iblock_protect fails cleanly when top_proxy child
  // attachment fails on SWMR reopen/reload.
  {
    TempFile swmr_proxy_file("h5eaiblock_swmr_proxy_err.h5");
    ScopedFile file = CreateSwmrTempFile(swmr_proxy_file);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));
    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    ASSERT_NE(hdr->top_proxy, nullptr);

    bool stats_changed = false;
    hdr->idx_blk_addr = H5EA__iblock_create(hdr, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(hdr->idx_blk_addr));

    H5EA_iblock_t* iblock = H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(iblock, nullptr);
    ASSERT_NE(iblock->top_proxy, nullptr);
    ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, iblock), 0);
    iblock->top_proxy = nullptr;
    ASSERT_GE(H5EA__iblock_unprotect(iblock, H5AC__NO_FLAGS_SET), 0);

    // Detach hdr so proxy nchildren reaches 0 and reset addr to HADDR_UNDEF.
    ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, hdr), 0);
    hdr->top_proxy->addr = HADDR_UNDEF;

    {
      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_TRUE(H5_addr_defined(eoa));
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->tmp_addr = orig_tmp; });
      f->shared->tmp_addr = eoa + 1;

      ScopedErrorSilence silence;
      EXPECT_EQ(H5EA__iblock_protect(hdr, H5AC__NO_FLAGS_SET), nullptr);
    }

    ASSERT_GE(H5AC_proxy_entry_dest(hdr->top_proxy), 0);
    hdr->top_proxy = nullptr;
    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }
}

TEST(H5EAiblockTest, ErrorAndCleanupPathsCoverAllocCreateProtectAndDelete) {
  ScopedFile file = CreateInMemoryFile("h5eaiblock_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5EA_class_t ctrl_cls = MakeControlledClass();
  absl::Cleanup reset_ctrl = [&] { g_fill_control.fail_fill = false; };
  const H5EA_create_t ctrl_cparam = MakeParams(&ctrl_cls, 10, 4);

  haddr_t hdr_addr = H5EA__hdr_create(f, &ctrl_cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));

  // 1. Failing H5EA__hdr_incr inside H5EA__iblock_alloc and H5EA__iblock_create
  // when hdr is unprotected/unpinned.
  H5EA_hdr_t* hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
  {
    bool sc = false;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__iblock_alloc(hdr), nullptr);
    EXPECT_EQ(H5EA__iblock_create(hdr, &sc), HADDR_UNDEF);
  }

  // Re-protect header for H5EA__iblock_create failure checks.
  hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  bool stats_changed = false;

  // 2. Failing H5MF_alloc inside H5EA__iblock_create.
  {
    const hsize_t orig_aggr = f->shared->meta_aggr.size;
    const haddr_t orig_tmp = f->shared->tmp_addr;
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
    ASSERT_TRUE(H5_addr_defined(eoa));
    auto restore_space = absl::MakeCleanup([&] {
      f->shared->meta_aggr.size = orig_aggr;
      f->shared->tmp_addr = orig_tmp;
    });
    f->shared->meta_aggr.size = 0;
    f->shared->tmp_addr = eoa;

    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__iblock_create(hdr, &stats_changed), HADDR_UNDEF);
  }

  // 3. Failing fill callback inside H5EA__iblock_create cleans up allocated
  // disk space, element buffer, address arrays, and header ref count.
  {
    g_fill_control.fail_fill = true;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__iblock_create(hdr, &stats_changed), HADDR_UNDEF);
    g_fill_control.fail_fill = false;
  }

  // 4. Failing H5AC_insert_entry inside H5EA__iblock_create on read-only file.
  {
    const unsigned orig_flags = f->shared->flags;
    auto restore_flags =
        absl::MakeCleanup([&] { f->shared->flags = orig_flags; });
    f->shared->flags &= ~static_cast<unsigned>(H5F_ACC_RDWR);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__iblock_create(hdr, &stats_changed), HADDR_UNDEF);
  }

  // 5. Failing H5EA__iblock_protect / H5EA__iblock_delete when idx_blk_addr is
  // zeroed/invalid.
  haddr_t bad_addr = H5MF_alloc(f, H5FD_MEM_EARRAY_IBLOCK, 64);
  ASSERT_TRUE(H5_addr_defined(bad_addr));
  hdr->idx_blk_addr = bad_addr;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__iblock_protect(hdr, H5AC__READ_ONLY_FLAG), nullptr);
    EXPECT_LT(H5EA__iblock_delete(hdr), 0);
  }
  hdr->idx_blk_addr = HADDR_UNDEF;
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);

  // 6. Failing H5EA__iblock_unprotect on read-only entry dirtied, plus failing
  // H5EA__dblock_delete and H5EA__sblock_delete inside H5EA__iblock_delete.
  const H5EA_create_t cparam = MakeParams(H5EA_CLS_TEST, 10, 4);

  // 6a. Corrupt dblk_addrs[0] during iblock_delete.
  {
    haddr_t case_hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(case_hdr_addr));
    H5EA_hdr_t* h =
        H5EA__hdr_protect(f, case_hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h, nullptr);
    h->idx_blk_addr = H5EA__iblock_create(h, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(h->idx_blk_addr));

    H5EA_iblock_t* ib = H5EA__iblock_protect(h, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(ib, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__iblock_unprotect(ib, H5AC__DIRTIED_FLAG), 0);
    }
    ASSERT_GE(H5EA__iblock_unprotect(ib, H5AC__NO_FLAGS_SET), 0);

    ib = H5EA__iblock_protect(h, H5AC__NO_FLAGS_SET);
    ASSERT_NE(ib, nullptr);
    ib->dblk_addrs[0] = bad_addr;
    ASSERT_GE(H5EA__iblock_unprotect(ib, H5AC__NO_FLAGS_SET), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__iblock_delete(h), 0);
    }
    h->idx_blk_addr = HADDR_UNDEF;
    EXPECT_GE(H5EA__hdr_delete(h), 0);
  }

  // 6b. Corrupt sblk_addrs[0] during iblock_delete.
  {
    haddr_t case_hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(case_hdr_addr));
    H5EA_hdr_t* h =
        H5EA__hdr_protect(f, case_hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(h, nullptr);
    h->idx_blk_addr = H5EA__iblock_create(h, &stats_changed);
    ASSERT_TRUE(H5_addr_defined(h->idx_blk_addr));

    H5EA_iblock_t* ib = H5EA__iblock_protect(h, H5AC__NO_FLAGS_SET);
    ASSERT_NE(ib, nullptr);
    ib->sblk_addrs[0] = bad_addr;
    ASSERT_GE(H5EA__iblock_unprotect(ib, H5AC__NO_FLAGS_SET), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__iblock_delete(h), 0);
    }
    h->idx_blk_addr = HADDR_UNDEF;
    EXPECT_GE(H5EA__hdr_delete(h), 0);
  }
}

}  // namespace
}  // namespace hdf5
