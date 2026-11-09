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
// Tests extensible array header allocation, initialization, element buffer
// factories, creation, reference counting, cache protection/unprotection, SWMR
// top-proxy creation/reload, deletion, and cleanup
// (third_party/hdf5/src/H5EAhdr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAhdr.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5C_cache_entry_t
// (first member of H5EA_hdr_t). Define NDEBUG around internal headers so C++
// struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5EAHDR_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5EAHDR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EAHDR_TEST_RESTORE_UNDEF_NDEBUG
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

struct CustomCtxData {
  bool fail_crt_context = false;
  bool fail_dst_context = false;
  int crt_calls = 0;
  int dst_calls = 0;
};

struct CustomCtx {
  CustomCtxData* state = nullptr;
};

void* CustomCrtContext(void* udata) {
  auto* state = static_cast<CustomCtxData*>(udata);
  if (state != nullptr) {
    ++state->crt_calls;
    if (state->fail_crt_context) {
      return nullptr;
    }
  }
  return new CustomCtx{state};
}

herr_t CustomDstContext(void* raw_ctx) {
  auto* ctx = static_cast<CustomCtx*>(raw_ctx);
  if (ctx->state != nullptr) {
    ++ctx->state->dst_calls;
    if (ctx->state->fail_dst_context) {
      return -1;
    }
  }
  delete ctx;
  return 0;
}

herr_t CustomFill(void* nat_blk, size_t nelmts) {
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = UINT64_MAX;
  }
  return 0;
}

herr_t CustomEncode(void* raw, const void* elmt, size_t nelmts, void* /*ctx*/) {
  auto* out = static_cast<uint8_t*>(raw);
  const auto* in = static_cast<const uint64_t*>(elmt);
  for (size_t i = 0; i < nelmts; ++i) {
    for (int b = 0; b < 8; ++b) {
      out[i * 8 + b] = static_cast<uint8_t>((in[i] >> (b * 8)) & 0xffU);
    }
  }
  return 0;
}

herr_t CustomDecode(const void* raw, void* elmt, size_t nelmts, void* /*ctx*/) {
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

H5EA_class_t MakeCustomClass(bool with_context = true) {
  H5EA_class_t cls{};
  cls.id = H5EA_CLS_TEST_ID;
  cls.name = "H5EAhdr_custom";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = with_context ? CustomCrtContext : nullptr;
  cls.dst_context = with_context ? CustomDstContext : nullptr;
  cls.fill = CustomFill;
  cls.encode = CustomEncode;
  cls.decode = CustomDecode;
  cls.debug = nullptr;
  cls.crt_dbg_ctx = nullptr;
  cls.dst_dbg_ctx = nullptr;
  return cls;
}

H5EA_create_t MakeDefaultParams(const H5EA_class_t* cls) {
  H5EA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 10;
  cparam.idx_blk_elmts = 4;
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

TEST(H5EAhdrTest, AllocInitElementFactoriesAndDest) {
  ScopedFile file = CreateInMemoryFile("h5eahdr_alloc.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. Header without crt_context / dst_context callbacks, plus top_proxy cleanup.
  H5EA_class_t no_ctx_cls = MakeCustomClass(/*with_context=*/false);
  H5EA_create_t cparam = MakeDefaultParams(&no_ctx_cls);

  H5EA_hdr_t* hdr0 = H5EA__hdr_alloc(f);
  ASSERT_NE(hdr0, nullptr);
  EXPECT_EQ(hdr0->f, f);
  EXPECT_FALSE(H5_addr_defined(hdr0->addr));
  EXPECT_FALSE(hdr0->swmr_write);

  hdr0->cparam = cparam;
  ASSERT_GE(H5EA__hdr_init(hdr0, nullptr), 0);
  EXPECT_EQ(hdr0->cb_ctx, nullptr);
  EXPECT_GT(hdr0->nsblks, 0u);
  EXPECT_NE(hdr0->sblk_info, nullptr);
  EXPECT_EQ(hdr0->dblk_page_nelmts, 16u);
  EXPECT_EQ(hdr0->size, H5EA_HEADER_SIZE_HDR(hdr0));

  // Exercise factory allocation, reuse, sparse expansion (leaving gap NULLs),
  // and freeing.
  void* blk_min = H5EA__hdr_alloc_elmts(hdr0, 4);
  ASSERT_NE(blk_min, nullptr);
  void* blk_min_again = H5EA__hdr_alloc_elmts(hdr0, 4);
  ASSERT_NE(blk_min_again, nullptr);
  void* blk_wide = H5EA__hdr_alloc_elmts(hdr0, 32);
  ASSERT_NE(blk_wide, nullptr);
  EXPECT_GE(hdr0->elmt_fac.nalloc, 4u);
  EXPECT_EQ(hdr0->elmt_fac.fac[1], nullptr);

  EXPECT_GE(H5EA__hdr_free_elmts(hdr0, 4, blk_min), 0);
  EXPECT_GE(H5EA__hdr_free_elmts(hdr0, 4, blk_min_again), 0);
  EXPECT_GE(H5EA__hdr_free_elmts(hdr0, 32, blk_wide), 0);

  hdr0->top_proxy = H5AC_proxy_entry_create();
  ASSERT_NE(hdr0->top_proxy, nullptr);
  EXPECT_GE(H5EA__hdr_dest(hdr0), 0);

  // 2. Header with crt_context / dst_context and failure paths in init & dest.
  CustomCtxData ctx_state;
  H5EA_class_t ctx_cls = MakeCustomClass(/*with_context=*/true);
  cparam.cls = &ctx_cls;

  // Failing crt_context inside H5EA__hdr_init leaves sblk_info allocated;
  // H5EA__hdr_dest cleans up.
  {
    ScopedErrorSilence silence;
    ctx_state.fail_crt_context = true;
    H5EA_hdr_t* hdr_init_fail = H5EA__hdr_alloc(f);
    ASSERT_NE(hdr_init_fail, nullptr);
    hdr_init_fail->cparam = cparam;
    EXPECT_LT(H5EA__hdr_init(hdr_init_fail, &ctx_state), 0);
    EXPECT_GE(H5EA__hdr_dest(hdr_init_fail), 0);
  }

  // Failing dst_context and failing H5FL_fac_term inside H5EA__hdr_dest,
  // retrying after restoring/freeing so zero memory leaks under ASan.
  ctx_state.fail_crt_context = false;
  H5EA_hdr_t* hdr_ctx = H5EA__hdr_alloc(f);
  ASSERT_NE(hdr_ctx, nullptr);
  hdr_ctx->cparam = cparam;
  ASSERT_GE(H5EA__hdr_init(hdr_ctx, &ctx_state), 0);
  EXPECT_NE(hdr_ctx->cb_ctx, nullptr);
  EXPECT_EQ(ctx_state.crt_calls, 2);

  void* pinned_blk = H5EA__hdr_alloc_elmts(hdr_ctx, 4);
  ASSERT_NE(pinned_blk, nullptr);

  ctx_state.fail_dst_context = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__hdr_dest(hdr_ctx), 0);
  }
  ctx_state.fail_dst_context = false;
  {
    ScopedErrorSilence silence;
    // Now dst_context succeeds, then H5FL_fac_term fails on unfreed pinned_blk.
    EXPECT_LT(H5EA__hdr_dest(hdr_ctx), 0);
  }
  EXPECT_EQ(ctx_state.dst_calls, 2);
  EXPECT_GE(H5EA__hdr_free_elmts(hdr_ctx, 4, pinned_blk), 0);
  EXPECT_GE(H5EA__hdr_dest(hdr_ctx), 0);
}

TEST(H5EAhdrTest, CreateRefCountsModifiedProtectUnprotectAndDelete) {
  ScopedFile file = CreateInMemoryFile("h5eahdr_lifecycle.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeDefaultParams(H5EA_CLS_TEST);
  haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));

  H5EA_hdr_t* hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);

  // Pin/unpin reference transitions (`rc` 0 -> 1 -> 2 -> 1 -> 0).
  EXPECT_GE(H5EA__hdr_incr(hdr), 0);
  EXPECT_EQ(hdr->rc, 1u);
  EXPECT_GE(H5EA__hdr_incr(hdr), 0);
  EXPECT_EQ(hdr->rc, 2u);
  EXPECT_GE(H5EA__hdr_decr(hdr), 0);
  EXPECT_EQ(hdr->rc, 1u);
  EXPECT_GE(H5EA__hdr_decr(hdr), 0);
  EXPECT_EQ(hdr->rc, 0u);

  // File reference counts (`file_rc` 0 -> 1 -> 2 -> 1 -> 0).
  EXPECT_GE(H5EA__hdr_fuse_incr(hdr), 0);
  EXPECT_EQ(hdr->file_rc, 1u);
  EXPECT_GE(H5EA__hdr_fuse_incr(hdr), 0);
  EXPECT_EQ(hdr->file_rc, 2u);
  EXPECT_EQ(H5EA__hdr_fuse_decr(hdr), 1u);
  EXPECT_EQ(H5EA__hdr_fuse_decr(hdr), 0u);

  // Dirty marking.
  EXPECT_GE(H5EA__hdr_modified(hdr), 0);
  unsigned status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, hdr_addr, &status), 0);
  EXPECT_TRUE((status & H5AC_ES__IS_DIRTY) != 0);
  EXPECT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  // Read-only protection + invalid dirty unprotect check + clean unprotect.
  hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(hdr, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__hdr_unprotect(hdr, H5AC__DIRTIED_FLAG), 0);
  }
  EXPECT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  // Delete empty header (`idx_blk_addr == HADDR_UNDEF`).
  hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);

  // Protecting a zeroed/corrupt address fails cleanly.
  haddr_t bad_addr = H5MF_alloc(f, H5FD_MEM_EARRAY_HDR, 64);
  ASSERT_TRUE(H5_addr_defined(bad_addr));
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__hdr_protect(f, bad_addr, nullptr, H5AC__READ_ONLY_FLAG),
              nullptr);
  }

  // Delete a populated array (`idx_blk_addr` defined -> invokes iblock_delete).
  ScopedEa ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);
  uint64_t val = 77;
  ASSERT_GE(H5EA_set(ea.get(), 0, &val), 0);
  ASSERT_GE(H5EA_set(ea.get(), 64, &val), 0);
  haddr_t pop_addr = HADDR_UNDEF;
  ASSERT_GE(H5EA_get_addr(ea.get(), &pop_addr), 0);
  ea.reset();

  hdr = H5EA__hdr_protect(f, pop_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);

  // Delete failure when `hdr->idx_blk_addr` points to invalid file space.
  haddr_t broken_hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(broken_hdr_addr));
  hdr = H5EA__hdr_protect(f, broken_hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  hdr->idx_blk_addr = bad_addr;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5EA__hdr_delete(hdr), 0);
  }
  hdr = H5EA__hdr_protect(f, broken_hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  hdr->idx_blk_addr = HADDR_UNDEF;
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);
}

TEST(H5EAhdrTest, SwmrCreateAndReopenReloadLazilyCreatesTopProxy) {
  TempFile tmp_file("h5eahdr_swmr.h5");

  // 1. Direct SWMR creation creates hdr->top_proxy immediately.
  {
    ScopedFile file = CreateSwmrTempFile(tmp_file);
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    const H5EA_create_t cparam = MakeDefaultParams(H5EA_CLS_TEST);
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));

    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_TRUE(hdr->swmr_write);
    EXPECT_NE(hdr->top_proxy, nullptr);
    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }

  // 2. Write a v1.10+ file without SWMR, then reopen with H5F_ACC_SWMR_WRITE
  // so H5EA__hdr_protect deserializes the header and lazily creates top_proxy.
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

    const H5EA_create_t cparam = MakeDefaultParams(H5EA_CLS_TEST);
    ScopedEa ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    uint64_t val = 12345;
    ASSERT_GE(H5EA_set(ea.get(), 64, &val), 0);
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
    EXPECT_TRUE(ea.get()->hdr->swmr_write);
    EXPECT_NE(ea.get()->hdr->top_proxy, nullptr);

    uint64_t out = 0;
    ASSERT_GE(H5EA_get(ea.get(), 64, &out), 0);
    EXPECT_EQ(out, 12345u);
  }
}

TEST(H5EAhdrTest, CreateProtectAndCacheErrorCleanupPaths) {
  {
    ScopedFile file = CreateInMemoryFile("h5eahdr_err.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // 1. Failing H5EA__hdr_init inside H5EA__hdr_create cleans up hdr.
    CustomCtxData ctx_state;
    ctx_state.fail_crt_context = true;
    H5EA_class_t ctx_cls = MakeCustomClass(/*with_context=*/true);
    H5EA_create_t ctx_cparam = MakeDefaultParams(&ctx_cls);
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5EA__hdr_create(f, &ctx_cparam, &ctx_state), HADDR_UNDEF);
    }

    const H5EA_create_t cparam = MakeDefaultParams(H5EA_CLS_TEST);

    // 2. Failing H5MF_alloc inside H5EA__hdr_create via exhausted meta aggr.
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
      EXPECT_EQ(H5EA__hdr_create(f, &cparam, nullptr), HADDR_UNDEF);
    }

    // 3. Failing H5AC_insert_entry inside H5EA__hdr_create via read-only flags.
    {
      const unsigned orig_flags = f->shared->flags;
      auto restore_flags =
          absl::MakeCleanup([&] { f->shared->flags = orig_flags; });
      f->shared->flags &= ~static_cast<unsigned>(H5F_ACC_RDWR);

      ScopedErrorSilence silence;
      EXPECT_EQ(H5EA__hdr_create(f, &cparam, nullptr), HADDR_UNDEF);
    }

    // 4. Cache-state error checks on a resident unprotected, unpinned header.
    // Note: H5EA__hdr_delete() is intentionally not called when unprotected
    // because H5EAhdr.c uses HGOTO_ERROR (which jumps to `done:`) inside its
    // `done:` handler on H5EA__hdr_unprotect failure.
    haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));
    H5EA_hdr_t* hdr =
        H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5EA__hdr_incr(hdr), 0);
      EXPECT_EQ(hdr->rc, 0u);

      hdr->rc = 1;
      EXPECT_LT(H5EA__hdr_decr(hdr), 0);
      EXPECT_EQ(hdr->rc, 0u);

      EXPECT_LT(H5EA__hdr_modified(hdr), 0);
      EXPECT_LT(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
    }

    hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_GE(H5EA__hdr_delete(hdr), 0);
  }

  // 5. SWMR H5EA__hdr_protect proxy add_child failure path. Note: on failure
  // here, H5EA__hdr_protect leaves `hdr` protected in cache with an orphaned
  // `top_proxy`, so we clean both via the already-resident pointer `hdr`.
  TempFile tmp_file("h5eahdr_swmr_err.h5");
  ScopedFile file = CreateSwmrTempFile(tmp_file);
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t cparam = MakeDefaultParams(H5EA_CLS_TEST);
  haddr_t hdr_addr = H5EA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));

  H5EA_hdr_t* hdr = H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  ASSERT_NE(hdr->top_proxy, nullptr);
  ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, hdr), 0);
  ASSERT_GE(H5AC_proxy_entry_dest(hdr->top_proxy), 0);
  hdr->top_proxy = nullptr;
  ASSERT_GE(H5EA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  {
    const haddr_t orig_tmp = f->shared->tmp_addr;
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
    ASSERT_TRUE(H5_addr_defined(eoa));
    auto restore_tmp = absl::MakeCleanup([&] { f->shared->tmp_addr = orig_tmp; });
    f->shared->tmp_addr = eoa + 1;

    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET),
              nullptr);
  }

  // `H5EA__hdr_protect` protected `hdr` before `H5AC_proxy_entry_add_child`
  // failed, so `hdr` is currently protected and holds a newly created proxy.
  if (hdr->top_proxy != nullptr) {
    ASSERT_GE(H5AC_proxy_entry_dest(hdr->top_proxy), 0);
    hdr->top_proxy = nullptr;
  }
  EXPECT_GE(H5EA__hdr_delete(hdr), 0);
}

}  // namespace
}  // namespace hdf5
