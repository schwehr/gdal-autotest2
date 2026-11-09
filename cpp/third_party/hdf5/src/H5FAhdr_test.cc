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
// Tests fixed array header allocation, initialization, creation, reference
// counting, cache protection/unprotection, SWMR proxy setup, deletion, and
// resource cleanup (third_party/hdf5/src/H5FAhdr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FAhdr.c

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
#define H5FAHDR_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5FAHDR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FAHDR_TEST_RESTORE_UNDEF_NDEBUG
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

struct CustomCtxCtrl {
  bool fail_crt_context = false;
  bool fail_dst_context = false;
  int crt_calls = 0;
  int dst_calls = 0;
};

struct CustomCtx {
  CustomCtxCtrl* ctrl = nullptr;
};

void* CustomCrtContext(void* udata) {
  auto* ctrl = static_cast<CustomCtxCtrl*>(udata);
  if (ctrl != nullptr) {
    ++ctrl->crt_calls;
    if (ctrl->fail_crt_context) {
      return nullptr;
    }
  }
  return new CustomCtx{ctrl};
}

herr_t CustomDstContext(void* raw_ctx) {
  auto* ctx = static_cast<CustomCtx*>(raw_ctx);
  if (ctx->ctrl != nullptr) {
    ++ctx->ctrl->dst_calls;
    if (ctx->ctrl->fail_dst_context) {
      return -1;
    }
  }
  delete ctx;
  return 0;
}

herr_t CustomFill(void* nat_blk, size_t nelmts) {
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = H5FA_TEST_FILL;
  }
  return 0;
}

herr_t CustomEncode(void* raw, const void* elmt, size_t nelmts, void* /*ctx*/) {
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

herr_t CustomDecode(const void* raw, void* elmt, size_t nelmts, void* /*ctx*/) {
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

H5FA_class_t MakeCustomClass(bool with_context) {
  H5FA_class_t cls{};
  cls.id = H5FA_CLS_TEST_ID;
  cls.name = with_context ? "H5FAhdr_with_ctx" : "H5FAhdr_no_ctx";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = with_context ? CustomCrtContext : nullptr;
  cls.dst_context = with_context ? CustomDstContext : nullptr;
  cls.fill = CustomFill;
  cls.encode = CustomEncode;
  cls.decode = CustomDecode;
  return cls;
}

H5FA_create_t MakeCparam(hsize_t nelmts = 8,
                         const H5FA_class_t* cls = H5FA_CLS_TEST) {
  H5FA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_dblk_page_nelmts_bits = 3;
  cparam.nelmts = nelmts;
  return cparam;
}

TEST(H5FAhdrTest, AllocInitCreateAndDestWithAndWithoutCallbackContext) {
  CustomCtxCtrl ctx_ctrl{};
  const H5FA_class_t ctx_cls = MakeCustomClass(/*with_context=*/true);
  const H5FA_class_t no_ctx_cls = MakeCustomClass(/*with_context=*/false);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Alloc, init, and dest with crt_context == nullptr.
  {
    H5FA_hdr_t* hdr = H5FA__hdr_alloc(f);
    ASSERT_NE(hdr, nullptr);
    EXPECT_FALSE(H5_addr_defined(hdr->addr));
    EXPECT_EQ(hdr->f, f);
    EXPECT_FALSE(hdr->swmr_write);
    hdr->cparam = MakeCparam(10, &no_ctx_cls);
    ASSERT_GE(H5FA__hdr_init(hdr, nullptr), 0);
    EXPECT_EQ(hdr->cb_ctx, nullptr);
    EXPECT_EQ(hdr->size, H5FA_HEADER_SIZE_HDR(hdr));
    EXPECT_EQ(hdr->stats.hdr_size, hdr->size);
    EXPECT_EQ(hdr->stats.nelmts, 10u);
    EXPECT_GE(H5FA__hdr_dest(hdr), 0);
  }

  // 2. Direct H5FA__hdr_create and empty H5FA__hdr_delete (no dblk_addr).
  {
    const H5FA_create_t cparam = MakeCparam(8, &ctx_cls);
    const haddr_t hdr_addr = H5FA__hdr_create(f, &cparam, &ctx_ctrl);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));
    EXPECT_EQ(ctx_ctrl.crt_calls, 1);

    H5FA_hdr_t* hdr =
        H5FA__hdr_protect(f, hdr_addr, &ctx_ctrl, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_FALSE(H5_addr_defined(hdr->dblk_addr));
    EXPECT_GE(H5FA__hdr_delete(hdr), 0);
    EXPECT_EQ(ctx_ctrl.dst_calls, 1);
  }

  // 3. Non-empty array delete covers H5FA__hdr_delete deleting its data block.
  {
    const H5FA_create_t cparam = MakeCparam(8);
    haddr_t hdr_addr = HADDR_UNDEF;
    {
      ScopedFA fa(H5FA_create(f, &cparam, nullptr));
      ASSERT_NE(fa.get(), nullptr);
      ASSERT_GE(H5FA_get_addr(fa.get(), &hdr_addr), 0);
      const uint64_t val = 1234;
      ASSERT_GE(H5FA_set(fa.get(), 0, &val), 0);
    }

    H5FA_hdr_t* hdr =
        H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_TRUE(H5_addr_defined(hdr->dblk_addr));
    EXPECT_GE(H5FA__hdr_delete(hdr), 0);
  }
}

TEST(H5FAhdrTest, RefCountingPinningFuseAndModified) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(8);
  haddr_t hdr_addr = HADDR_UNDEF;
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(fa.get(), &hdr_addr), 0);

    H5FA_hdr_t* hdr = fa.get()->hdr;
    EXPECT_EQ(hdr->rc, 1u);
    EXPECT_EQ(hdr->file_rc, 1u);

    // Extra rc incr/decr while already pinned.
    ASSERT_GE(H5FA__hdr_incr(hdr), 0);
    EXPECT_EQ(hdr->rc, 2u);
    ASSERT_GE(H5FA__hdr_decr(hdr), 0);
    EXPECT_EQ(hdr->rc, 1u);

    // Extra file_rc fuse incr/decr.
    ASSERT_GE(H5FA__hdr_fuse_incr(hdr), 0);
    EXPECT_EQ(hdr->file_rc, 2u);
    EXPECT_EQ(H5FA__hdr_fuse_decr(hdr), 1u);

    // Marking header dirty succeeds when pinned.
    ASSERT_GE(H5FA__hdr_modified(hdr), 0);
    EXPECT_TRUE(hdr->cache_info.is_dirty);
  }

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // Unprotected clean cached hdr (rc == 0) fails incr and modified.
  H5FA_hdr_t* unprot_hdr =
      H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(unprot_hdr, nullptr);
  ASSERT_EQ(unprot_hdr->rc, 0u);
  ASSERT_GE(H5FA__hdr_unprotect(unprot_hdr, H5AC__NO_FLAGS_SET), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__hdr_incr(unprot_hdr), 0);
    EXPECT_LT(H5FA__hdr_modified(unprot_hdr), 0);
  }

  // H5FA__hdr_decr failure when header was unpinned while rc == 1; re-pin and
  // decrement cleanly so nothing leaks.
  H5FA_hdr_t* hdr =
      H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(hdr, nullptr);
  ASSERT_GE(H5FA__hdr_incr(hdr), 0);
  ASSERT_EQ(hdr->rc, 1u);
  ASSERT_GE(H5FA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  ASSERT_GE(H5AC_unpin_entry(hdr), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__hdr_decr(hdr), 0);
  }
  ASSERT_EQ(hdr->rc, 0u);
  hdr = H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(hdr, nullptr);
  ASSERT_GE(H5FA__hdr_incr(hdr), 0);
  ASSERT_GE(H5FA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5FA__hdr_decr(hdr), 0);
}

TEST(H5FAhdrTest, ProtectUnprotectDeleteAndSwmrTopProxy) {
  TempFile temp_file("h5fa_hdr_swmr.h5");
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

  // 1. Create header directly in SWMR mode (creates top_proxy and links hdr).
  const H5FA_create_t cparam = MakeCparam(8);
  const haddr_t hdr_addr = H5FA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));

  {
    H5FA_hdr_t* hdr =
        H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_TRUE(hdr->swmr_write);
    EXPECT_NE(hdr->top_proxy, nullptr);

    bool hdr_dirty = false;
    const haddr_t dblk_addr = H5FA__dblock_create(hdr, &hdr_dirty);
    ASSERT_TRUE(H5_addr_defined(dblk_addr));
    hdr->dblk_addr = dblk_addr;
    ASSERT_GE(H5FA__hdr_unprotect(hdr, H5AC__DIRTIED_FLAG), 0);
  }

  // 2. Flush & evict so reloaded SWMR header starts with top_proxy == nullptr
  //    and H5FA__hdr_protect creates top_proxy and links hdr.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  H5FA_hdr_t* hdr =
      H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(hdr, nullptr);
  EXPECT_NE(hdr->top_proxy, nullptr);

  // Dirty unprotect on read-only protected header fails cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__hdr_unprotect(hdr, H5AC__DIRTIED_FLAG), 0);
  }
  EXPECT_GE(H5FA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  // 3. Protect dblock read-only so H5FA__hdr_delete fails on dblock deletion
  //    (note H5FA__hdr_delete still unprotects hdr on error), then unprotect
  //    dblock and delete cleanly.
  hdr = H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  H5FA_dblock_t* dblock =
      H5FA__dblock_protect(hdr, hdr->dblk_addr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblock, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__hdr_delete(hdr), 0);
  }
  EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);

  hdr = H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  EXPECT_GE(H5FA__hdr_delete(hdr), 0);

  // 4. Invalid address (superblock at addr 0) fails H5FA__hdr_protect.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA__hdr_protect(f, 0, nullptr, H5AC__READ_ONLY_FLAG), nullptr);
  }
}

TEST(H5FAhdrTest, ContextCallbackFailuresAndDestCleanup) {
  CustomCtxCtrl ctx_ctrl{};
  const H5FA_class_t ctx_cls = MakeCustomClass(/*with_context=*/true);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. crt_context failure in H5FA__hdr_init and H5FA__hdr_create.
  ctx_ctrl.fail_crt_context = true;
  {
    ScopedErrorSilence silence;

    H5FA_hdr_t* hdr = H5FA__hdr_alloc(f);
    ASSERT_NE(hdr, nullptr);
    hdr->cparam = MakeCparam(8, &ctx_cls);
    EXPECT_LT(H5FA__hdr_init(hdr, &ctx_ctrl), 0);
    EXPECT_GE(H5FA__hdr_dest(hdr), 0);

    const H5FA_create_t cparam = MakeCparam(8, &ctx_cls);
    EXPECT_EQ(H5FA__hdr_create(f, &cparam, &ctx_ctrl), HADDR_UNDEF);
  }
  ctx_ctrl.fail_crt_context = false;

  // 2. dst_context failure in H5FA__hdr_dest leaves hdr intact, then succeeds
  //    once restored so no memory leaks.
  H5FA_hdr_t* hdr = H5FA__hdr_alloc(f);
  ASSERT_NE(hdr, nullptr);
  hdr->cparam = MakeCparam(8, &ctx_cls);
  ASSERT_GE(H5FA__hdr_init(hdr, &ctx_ctrl), 0);
  ASSERT_NE(hdr->cb_ctx, nullptr);

  ctx_ctrl.fail_dst_context = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__hdr_dest(hdr), 0);
  }
  ctx_ctrl.fail_dst_context = false;
  EXPECT_GE(H5FA__hdr_dest(hdr), 0);
}

TEST(H5FAhdrTest, FileAllocCacheInsertAndReadOnlyDeleteErrorPaths) {
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
  ScopedFile file(H5Fcreate("h5fa_hdr_err.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(8);

  // 1. Lower VFD maxaddr below EOA + hdr->size so H5MF_alloc fails cleanly.
  {
    H5FD_t* lf = nullptr;
    ASSERT_GE(H5F_shared_get_file_driver(H5F_get_shared(f), &lf), 0);
    ASSERT_NE(lf, nullptr);
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_FARRAY_HDR);
    ASSERT_TRUE(H5_addr_defined(eoa));
    const haddr_t saved_maxaddr = lf->maxaddr;
    lf->maxaddr = eoa + 1;
    absl::Cleanup restore_maxaddr([lf, saved_maxaddr] {
      lf->maxaddr = saved_maxaddr;
    });

    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA__hdr_create(f, &cparam, nullptr), HADDR_UNDEF);
  }

  // 2. Pre-populate cache at EOA so H5AC_insert_entry fails cleanly and frees
  //    disk space via H5MF_xfree.
  {
    const haddr_t next_eoa = H5F_get_eoa(f, H5FD_MEM_FARRAY_HDR);
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

    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5FA__hdr_create(f, &cparam, nullptr), HADDR_UNDEF);
    }
  }

  // 3. Read-only protected header fails H5AC_unprotect inside H5FA__hdr_delete,
  //    then unprotects and deletes cleanly on RW retry.
  const haddr_t hdr_addr = H5FA__hdr_create(f, &cparam, nullptr);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));
  H5FA_hdr_t* ro_hdr =
      H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(ro_hdr, nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA__hdr_delete(ro_hdr), 0);
  }
  EXPECT_GE(H5FA__hdr_unprotect(ro_hdr, H5AC__NO_FLAGS_SET), 0);

  H5FA_hdr_t* rw_hdr =
      H5FA__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(rw_hdr, nullptr);
  EXPECT_GE(H5FA__hdr_delete(rw_hdr), 0);
}

}  // namespace
}  // namespace hdf5
