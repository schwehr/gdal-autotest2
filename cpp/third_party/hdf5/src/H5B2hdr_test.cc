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
// Tests v2 B-tree header allocation, initialization, creation, reference
// counting, cache protection/unprotection, SWMR proxy setup, deletion, and
// resource cleanup (third_party/hdf5/src/H5B2hdr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2hdr.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5B2_hdr_t). Define NDEBUG around internal headers so C++
// test translation units match `:hdf5` struct layout.
#ifndef NDEBUG
#define H5B2HDR_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#include "third_party/hdf5/src/H5B2pkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5B2HDR_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2HDR_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedB2 {
 public:
  ScopedB2() = default;
  explicit ScopedB2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedB2() { reset(); }

  ScopedB2(const ScopedB2&) = delete;
  ScopedB2& operator=(const ScopedB2&) = delete;

  void reset(H5B2_t* next = nullptr) {
    if (bt2_ != nullptr) {
      H5B2_close(bt2_);
    }
    bt2_ = next;
  }

  H5B2_t* get() const { return bt2_; }

 private:
  H5B2_t* bt2_ = nullptr;
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
  auto* ctx = new CustomCtx{state};
  return ctx;
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

herr_t CustomStore(void* nrecord, const void* udata) {
  *static_cast<hsize_t*>(nrecord) = *static_cast<const hsize_t*>(udata);
  return 0;
}

herr_t CustomCompare(const void* rec1, const void* rec2, int* result) {
  const hsize_t a = *static_cast<const hsize_t*>(rec1);
  const hsize_t b = *static_cast<const hsize_t*>(rec2);
  *result = (a < b) ? -1 : ((a > b) ? 1 : 0);
  return 0;
}

herr_t CustomEncode(uint8_t* raw, const void* nrecord, void* /*ctx*/) {
  const uint64_t v = static_cast<uint64_t>(*static_cast<const hsize_t*>(nrecord));
  for (int i = 0; i < 8; ++i) {
    raw[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffU);
  }
  return 0;
}

herr_t CustomDecode(const uint8_t* raw, void* nrecord, void* /*ctx*/) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(raw[i]) << (i * 8);
  }
  *static_cast<hsize_t*>(nrecord) = static_cast<hsize_t>(v);
  return 0;
}

H5B2_class_t MakeCustomClass(bool with_context = true,
                             H5B2_subid_t id = H5B2_TEST_ID) {
  H5B2_class_t cls{};
  cls.id = id;
  cls.name = "H5B2hdr_custom";
  cls.nrec_size = sizeof(hsize_t);
  cls.crt_context = with_context ? CustomCrtContext : nullptr;
  cls.dst_context = with_context ? CustomDstContext : nullptr;
  cls.store = CustomStore;
  cls.compare = CustomCompare;
  cls.encode = CustomEncode;
  cls.decode = CustomDecode;
  cls.debug = nullptr;
  return cls;
}

struct RemoveState {
  int count = 0;
  bool fail = false;
};

herr_t CountingRemoveOp(const void* /*record*/, void* op_data) {
  auto* state = static_cast<RemoveState*>(op_data);
  if (state->fail) {
    return -1;
  }
  ++state->count;
  return 0;
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

TEST(H5B2hdrTest, AllocInitAndFreeDepthZeroAndMultiDepth) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Depth 0 without client context callbacks.
  H5B2_class_t no_ctx_cls = MakeCustomClass(/*with_context=*/false);
  H5B2_create_t cparam{};
  cparam.cls = &no_ctx_cls;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  H5B2_hdr_t* hdr0 = H5B2__hdr_alloc(f);
  ASSERT_NE(hdr0, nullptr);
  EXPECT_EQ(hdr0->f, f);
  EXPECT_EQ(hdr0->hdr_size, H5B2_HEADER_SIZE_HDR(hdr0));
  EXPECT_FALSE(H5_addr_defined(hdr0->root.addr));

  ASSERT_GE(H5B2__hdr_init(hdr0, &cparam, nullptr, 0), 0);
  EXPECT_EQ(hdr0->cb_ctx, nullptr);
  EXPECT_EQ(hdr0->depth, 0);
  EXPECT_NE(hdr0->page, nullptr);
  EXPECT_NE(hdr0->nat_off, nullptr);
  EXPECT_NE(hdr0->node_info[0].nat_rec_fac, nullptr);
  EXPECT_EQ(hdr0->node_info[0].node_ptr_fac, nullptr);
  EXPECT_FALSE(hdr0->swmr_write);
  EXPECT_GE(H5B2__hdr_free(hdr0), 0);

  // Depth 2 with client context callbacks, min/max native records, and proxy.
  CustomCtxData ctx_state;
  H5B2_class_t ctx_cls = MakeCustomClass(/*with_context=*/true);
  cparam.cls = &ctx_cls;

  H5B2_hdr_t* hdr2 = H5B2__hdr_alloc(f);
  ASSERT_NE(hdr2, nullptr);
  ASSERT_GE(H5B2__hdr_init(hdr2, &cparam, &ctx_state, 2), 0);
  EXPECT_NE(hdr2->cb_ctx, nullptr);
  EXPECT_EQ(ctx_state.crt_calls, 1);
  EXPECT_EQ(hdr2->depth, 2);
  EXPECT_NE(hdr2->node_info[1].nat_rec_fac, nullptr);
  EXPECT_NE(hdr2->node_info[1].node_ptr_fac, nullptr);
  EXPECT_NE(hdr2->node_info[2].nat_rec_fac, nullptr);
  EXPECT_NE(hdr2->node_info[2].node_ptr_fac, nullptr);

  hdr2->min_native_rec = H5MM_malloc(ctx_cls.nrec_size);
  hdr2->max_native_rec = H5MM_malloc(ctx_cls.nrec_size);
  hdr2->top_proxy = H5AC_proxy_entry_create();
  ASSERT_NE(hdr2->min_native_rec, nullptr);
  ASSERT_NE(hdr2->max_native_rec, nullptr);
  ASSERT_NE(hdr2->top_proxy, nullptr);

  EXPECT_GE(H5B2__hdr_free(hdr2), 0);
  EXPECT_EQ(ctx_state.dst_calls, 1);
}

TEST(H5B2hdrTest, InitFailureAndDstContextFailureInFree) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  CustomCtxData ctx_state;
  H5B2_class_t cls = MakeCustomClass(/*with_context=*/true);
  H5B2_create_t cparam{};
  cparam.cls = &cls;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  // Failing crt_context inside H5B2__hdr_init frees hdr internally.
  {
    ScopedErrorSilence silence;
    ctx_state.fail_crt_context = true;
    H5B2_hdr_t* hdr = H5B2__hdr_alloc(f);
    ASSERT_NE(hdr, nullptr);
    EXPECT_LT(H5B2__hdr_init(hdr, &cparam, &ctx_state, 0), 0);
  }

  // Failing dst_context inside H5B2__hdr_free returns FAIL, then succeeds once
  // the callback is allowed to succeed so nothing leaks.
  ctx_state.fail_crt_context = false;
  H5B2_hdr_t* hdr = H5B2__hdr_alloc(f);
  ASSERT_NE(hdr, nullptr);
  ASSERT_GE(H5B2__hdr_init(hdr, &cparam, &ctx_state, 1), 0);
  ctx_state.fail_dst_context = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2__hdr_free(hdr), 0);
  }
  ctx_state.fail_dst_context = false;
  EXPECT_GE(H5B2__hdr_free(hdr), 0);
}

TEST(H5B2hdrTest, CreateRefCountsDirtyProtectUnprotectAndDeleteEmpty) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  haddr_t hdr_addr = H5B2__hdr_create(f, &cparam, f);
  ASSERT_TRUE(H5_addr_defined(hdr_addr));

  H5B2_hdr_t* hdr =
      H5B2__hdr_protect(f, hdr_addr, f, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);

  // Pin / unpin transitions on rc == 0 and rc > 0.
  EXPECT_GE(H5B2__hdr_incr(hdr), 0);
  EXPECT_EQ(hdr->rc, 1u);
  EXPECT_GE(H5B2__hdr_incr(hdr), 0);
  EXPECT_EQ(hdr->rc, 2u);
  EXPECT_GE(H5B2__hdr_decr(hdr), 0);
  EXPECT_EQ(hdr->rc, 1u);
  EXPECT_GE(H5B2__hdr_decr(hdr), 0);
  EXPECT_EQ(hdr->rc, 0u);

  // File reference counts.
  EXPECT_GE(H5B2__hdr_fuse_incr(hdr), 0);
  EXPECT_EQ(hdr->file_rc, 1u);
  EXPECT_GE(H5B2__hdr_fuse_incr(hdr), 0);
  EXPECT_EQ(hdr->file_rc, 2u);
  EXPECT_EQ(H5B2__hdr_fuse_decr(hdr), 1u);
  EXPECT_EQ(H5B2__hdr_fuse_decr(hdr), 0u);

  // Dirty marking.
  EXPECT_GE(H5B2__hdr_dirty(hdr), 0);
  unsigned status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, hdr_addr, &status), 0);
  EXPECT_TRUE((status & H5AC_ES__IS_DIRTY) != 0);

  EXPECT_GE(H5B2__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  // Read-only protect and delete empty header (root.addr == HADDR_UNDEF).
  hdr = H5B2__hdr_protect(f, hdr_addr, f, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(hdr, nullptr);
  EXPECT_GE(H5B2__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  hdr = H5B2__hdr_protect(f, hdr_addr, f, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  EXPECT_GE(H5B2__hdr_delete(hdr), 0);

  // Protecting a non-header (zeroed) address must fail cleanly.
  haddr_t bad_addr = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  ASSERT_TRUE(H5_addr_defined(bad_addr));
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5B2__hdr_protect(f, bad_addr, f, H5AC__READ_ONLY_FLAG), nullptr);
  }
}

TEST(H5B2hdrTest, DeletePopulatedTreeAndErrorCleanup) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  haddr_t hdr_addr = HADDR_UNDEF;
  {
    ScopedB2 bt2(H5B2_create(f, &cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &hdr_addr), 0);
    for (hsize_t i = 0; i < 140; ++i) {
      hsize_t rec = (i * 37) % 140;
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
  }

  // Delete populated multi-level tree cleanly, verifying all records visited.
  RemoveState rm_state;
  H5B2_hdr_t* hdr = H5B2__hdr_protect(f, hdr_addr, f, H5AC__NO_FLAGS_SET);
  ASSERT_NE(hdr, nullptr);
  hdr->remove_op = CountingRemoveOp;
  hdr->remove_op_data = &rm_state;
  EXPECT_GE(H5B2__hdr_delete(hdr), 0);
  EXPECT_EQ(rm_state.count, 140);

  // Separate tree to exercise failing remove_op inside H5B2__hdr_delete
  // without reusing a header whose descendants were already deleted.
  haddr_t err_hdr_addr = HADDR_UNDEF;
  {
    ScopedB2 bt2(H5B2_create(f, &cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &err_hdr_addr), 0);
    hsize_t rec = 1;
    ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
  }

  rm_state.fail = true;
  H5B2_hdr_t* err_hdr =
      H5B2__hdr_protect(f, err_hdr_addr, f, H5AC__NO_FLAGS_SET);
  ASSERT_NE(err_hdr, nullptr);
  err_hdr->remove_op = CountingRemoveOp;
  err_hdr->remove_op_data = &rm_state;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5B2__hdr_delete(err_hdr), 0);
  }
}

TEST(H5B2hdrTest, SwmrCreateAndProtectReloadCreatesTopProxy) {
  TempFile tmp_file("h5b2hdr_swmr.h5");

  // Phase 1: Create a v3 superblock SWMR-enabled file and allocate a SWMR
  // header directly via H5B2__hdr_create (creates top_proxy and adds hdr as
  // child of top_proxy). Declare custom state before ScopedFile so teardown
  // flush/eviction finds valid class pointers.
  CustomCtxData ctx_state;
  H5B2_class_t swmr_cls = MakeCustomClass(/*with_context=*/true, H5B2_CDSET_ID);
  {
    ScopedFile file = CreateSwmrTempFile(tmp_file);
    ASSERT_TRUE(file.valid());
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_create_t cparam{};
    cparam.cls = &swmr_cls;
    cparam.node_size = 128;
    cparam.rrec_size = 8;
    cparam.split_percent = 100;
    cparam.merge_percent = 40;

    haddr_t hdr_addr = H5B2__hdr_create(f, &cparam, &ctx_state);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));

    H5B2_hdr_t* hdr =
        H5B2__hdr_protect(f, hdr_addr, &ctx_state, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_TRUE(hdr->swmr_write);
    EXPECT_NE(hdr->top_proxy, nullptr);
    EXPECT_GE(H5B2__hdr_delete(hdr), 0);
  }

  // Phase 2: Create a real chunked dataset with >1 unlimited dimension (uses
  // H5B2_CDSET_ID) on a v1.10+ file, close it, then reopen with
  // H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE so H5B2__hdr_protect reloads the header
  // from disk and lazily creates hdr->top_proxy.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    hsize_t dims[2] = {4, 4};
    hsize_t maxdims[2] = {H5S_UNLIMITED, H5S_UNLIMITED};
    hsize_t chunk[2] = {2, 2};
    ScopedSpace space(H5Screate_simple(2, dims, maxdims));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int> data(16, 7);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
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

    ScopedDataset dset(H5Dopen2(file.get(), "dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int> out(16, 0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[15], 7);
  }
}

TEST(H5B2hdrTest, ErrorPathsAndHeaderResourceFailures) {
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_class_t no_ctx_cls = MakeCustomClass(/*with_context=*/false);
    H5B2_create_t cparam{};
    cparam.cls = &no_ctx_cls;
    cparam.node_size = 128;
    cparam.rrec_size = 8;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;

    // Force H5MF_alloc failure in H5B2__hdr_create via exhausted meta aggregator
    // colliding with lowered tmp_addr.
    {
      const hsize_t orig_aggr_size = f->shared->meta_aggr.size;
      const haddr_t orig_tmp_addr = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_TRUE(H5_addr_defined(eoa));
      auto restore_space = absl::MakeCleanup([&] {
        f->shared->meta_aggr.size = orig_aggr_size;
        f->shared->tmp_addr = orig_tmp_addr;
      });
      f->shared->meta_aggr.size = 0;
      f->shared->tmp_addr = eoa;

      ScopedErrorSilence silence;
      EXPECT_EQ(H5B2__hdr_create(f, &cparam, nullptr), HADDR_UNDEF);
    }

    // Force H5AC_insert_entry failure in H5B2__hdr_create on read-only intent
    // (allocates header space, then releases it via H5MF_xfree and hdr_free).
    {
      ASSERT_GE(f->shared->meta_aggr.size, H5B2_HEADER_SIZE_FILE(f));
      const unsigned orig_flags = f->shared->flags;
      auto restore_flags =
          absl::MakeCleanup([&] { f->shared->flags = orig_flags; });
      f->shared->flags &= ~static_cast<unsigned>(H5F_ACC_RDWR);

      ScopedErrorSilence silence;
      EXPECT_EQ(H5B2__hdr_create(f, &cparam, nullptr), HADDR_UNDEF);
    }

    // Cache-state errors on a resident unprotected, unpinned empty header.
    haddr_t hdr_addr = H5B2__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));
    H5B2_hdr_t* hdr =
        H5B2__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    ASSERT_GE(H5B2__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5B2__hdr_incr(hdr), 0);
      EXPECT_EQ(hdr->rc, 0u);

      hdr->rc = 1;
      EXPECT_LT(H5B2__hdr_decr(hdr), 0);
      EXPECT_EQ(hdr->rc, 0u);

      EXPECT_LT(H5B2__hdr_dirty(hdr), 0);
      EXPECT_LT(H5B2__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);
      EXPECT_LT(H5B2__hdr_delete(hdr), 0);
    }

    hdr = H5B2__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    EXPECT_GE(H5B2__hdr_delete(hdr), 0);

    // Block factory termination errors inside H5B2__hdr_free (nat_rec_fac and
    // node_ptr_fac with outstanding allocations).
    {
      H5B2_hdr_t* hdr_a = H5B2__hdr_alloc(f);
      ASSERT_NE(hdr_a, nullptr);
      ASSERT_GE(H5B2__hdr_init(hdr_a, &cparam, nullptr, 0), 0);
      void* blk0 = H5FL_FAC_MALLOC(hdr_a->node_info[0].nat_rec_fac);
      ASSERT_NE(blk0, nullptr);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5B2__hdr_free(hdr_a), 0);
      }
      H5FL_FAC_FREE(hdr_a->node_info[0].nat_rec_fac, blk0);
      EXPECT_GE(H5B2__hdr_free(hdr_a), 0);
    }

    {
      H5B2_hdr_t* hdr_b = H5B2__hdr_alloc(f);
      ASSERT_NE(hdr_b, nullptr);
      ASSERT_GE(H5B2__hdr_init(hdr_b, &cparam, nullptr, 1), 0);
      void* blk1 = H5FL_FAC_MALLOC(hdr_b->node_info[1].node_ptr_fac);
      ASSERT_NE(blk1, nullptr);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5B2__hdr_free(hdr_b), 0);
      }
      // Clear factories already destroyed on the earlier iterations so retry
      // cleanup terminates node_info[1].node_ptr_fac cleanly.
      hdr_b->node_info[0].nat_rec_fac = nullptr;
      hdr_b->node_info[1].nat_rec_fac = nullptr;
      H5FL_FAC_FREE(hdr_b->node_info[1].node_ptr_fac, blk1);
      EXPECT_GE(H5B2__hdr_free(hdr_b), 0);
    }
  }

  // SWMR H5B2__hdr_protect failure when H5AC_proxy_entry_add_child fails.
  {
    TempFile tmp_file("h5b2hdr_swmr_err.h5");
    H5B2_class_t swmr_cls = MakeCustomClass(/*with_context=*/false, H5B2_CDSET_ID);
    ScopedFile file = CreateSwmrTempFile(tmp_file);
    ASSERT_TRUE(file.valid());
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_create_t cparam{};
    cparam.cls = &swmr_cls;
    cparam.node_size = 128;
    cparam.rrec_size = 8;
    cparam.split_percent = 100;
    cparam.merge_percent = 30;

    haddr_t hdr_addr = H5B2__hdr_create(f, &cparam, nullptr);
    ASSERT_TRUE(H5_addr_defined(hdr_addr));

    // Detach initial top_proxy so H5B2__hdr_protect attempts to recreate it.
    H5B2_hdr_t* hdr =
        H5B2__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    ASSERT_NE(hdr->top_proxy, nullptr);
    ASSERT_GE(H5AC_proxy_entry_remove_child(hdr->top_proxy, hdr), 0);
    ASSERT_GE(H5AC_proxy_entry_dest(hdr->top_proxy), 0);
    hdr->top_proxy = nullptr;
    ASSERT_GE(H5B2__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

    {
      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_TRUE(H5_addr_defined(eoa));
      auto restore_tmp =
          absl::MakeCleanup([&] { f->shared->tmp_addr = orig_tmp; });
      f->shared->tmp_addr = eoa + 1;

      ScopedErrorSilence silence;
      EXPECT_EQ(H5B2__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET),
                nullptr);
    }

    // Re-protect, clean up half-initialized top_proxy, and delete header.
    hdr = H5B2__hdr_protect(f, hdr_addr, nullptr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    if (hdr->top_proxy != nullptr) {
      ASSERT_GE(H5AC_proxy_entry_dest(hdr->top_proxy), 0);
      hdr->top_proxy = nullptr;
    }
    EXPECT_GE(H5B2__hdr_delete(hdr), 0);
  }
}

}  // namespace
}  // namespace hdf5
