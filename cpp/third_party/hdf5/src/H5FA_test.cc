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
// Tests fixed array lifecycle, unpaged and paged element get/set, iteration,
// pending/immediate deletion, SWMR flush dependencies, file handle patching,
// custom class error injection, and public fixed-dimension chunked datasets
// (third_party/hdf5/src/H5FA.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FA.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5FA_hdr_t, H5FA_dblock_t, and H5FA_dblk_page_t). Define
// NDEBUG around internal headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FA_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5FA_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FA_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

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

  ScopedFA(ScopedFA&& other) noexcept : fa_(std::exchange(other.fa_, nullptr)) {}
  ScopedFA& operator=(ScopedFA&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.fa_, nullptr));
    }
    return *this;
  }

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

H5FA_create_t MakeCparam(hsize_t nelmts, uint8_t page_bits = 3,
                         const H5FA_class_t* cls = H5FA_CLS_TEST) {
  H5FA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_dblk_page_nelmts_bits = page_bits;
  cparam.nelmts = nelmts;
  return cparam;
}

struct IterCollect {
  std::vector<std::pair<hsize_t, uint64_t>> items;
  int stop_at = -1;
  int error_at = -1;
};

int CollectElementsOp(hsize_t idx, const void* raw_elmt, void* raw_udata) {
  auto* state = static_cast<IterCollect*>(raw_udata);
  const int count = static_cast<int>(state->items.size());
  if (state->error_at == count) {
    return H5_ITER_ERROR;
  }
  state->items.emplace_back(idx, *static_cast<const uint64_t*>(raw_elmt));
  if (state->stop_at == count) {
    return H5_ITER_STOP;
  }
  return H5_ITER_CONT;
}

// Class fill callback has no ctx parameter; guarded via absl::Cleanup in tests.
bool g_fail_fill = false;

struct FaultCtrl {
  bool fail_crt_context = false;
  bool fail_encode = false;
  bool fail_decode = false;
};

void* FaultCrtContext(void* udata) {
  auto* ctrl = static_cast<FaultCtrl*>(udata);
  if (ctrl != nullptr && ctrl->fail_crt_context) {
    return nullptr;
  }
  return ctrl;
}

herr_t FaultDstContext(void* /*ctx*/) { return 0; }

herr_t FaultFill(void* nat_blk, size_t nelmts) {
  if (g_fail_fill) {
    return -1;
  }
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = H5FA_TEST_FILL;
  }
  return 0;
}

herr_t FaultEncode(void* raw, const void* elmt, size_t nelmts, void* ctx) {
  auto* ctrl = static_cast<FaultCtrl*>(ctx);
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

herr_t FaultDecode(const void* raw, void* elmt, size_t nelmts, void* ctx) {
  auto* ctrl = static_cast<FaultCtrl*>(ctx);
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

H5FA_class_t MakeFaultClass() {
  H5FA_class_t cls{};
  cls.id = H5FA_CLS_TEST_ID;
  cls.name = "H5FA_fault_test";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = FaultCrtContext;
  cls.dst_context = FaultDstContext;
  cls.fill = FaultFill;
  cls.encode = FaultEncode;
  cls.decode = FaultDecode;
  return cls;
}

TEST(H5FATest, UnpagedLifecycleGetSetIterateAndReopen) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // max_dblk_page_nelmts_bits=4 (16 elmts/page), nelmts=8 -> unpaged dblock.
  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/4);
  haddr_t fa_addr = HADDR_UNDEF;
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);
    EXPECT_TRUE(H5_addr_defined(fa_addr));

    hsize_t nelmts = 0;
    ASSERT_GE(H5FA_get_nelmts(fa.get(), &nelmts), 0);
    EXPECT_EQ(nelmts, 8u);

    // 1. Read before data block exists returns class fill value.
    uint64_t val = 0;
    ASSERT_GE(H5FA_get(fa.get(), 2, &val), 0);
    EXPECT_EQ(val, H5FA_TEST_FILL);

    // 2. Write elements -> lazily allocates unpaged dblock and marks hdr dirty.
    const std::array<std::pair<hsize_t, uint64_t>, 3> kWrites = {
        {{0, 100}, {3, 303}, {7, 707}}};
    for (const auto& [idx, v] : kWrites) {
      ASSERT_GE(H5FA_set(fa.get(), idx, &v), 0);
    }

    // 3. Verify written and untouched elements via H5FA_get.
    for (const auto& [idx, expected] : kWrites) {
      ASSERT_GE(H5FA_get(fa.get(), idx, &val), 0);
      EXPECT_EQ(val, expected);
    }
    ASSERT_GE(H5FA_get(fa.get(), 5, &val), 0);
    EXPECT_EQ(val, H5FA_TEST_FILL);

    // 4. Verify H5FA_iterate visits all elements, stops on H5_ITER_STOP, and
    //    propagates H5_ITER_ERROR.
    IterCollect all;
    EXPECT_EQ(H5FA_iterate(fa.get(), CollectElementsOp, &all), H5_ITER_CONT);
    ASSERT_EQ(all.items.size(), 8u);
    EXPECT_EQ(all.items[0].second, 100u);
    EXPECT_EQ(all.items[3].second, 303u);
    EXPECT_EQ(all.items[5].second, H5FA_TEST_FILL);
    EXPECT_EQ(all.items[7].second, 707u);

    IterCollect stop_early;
    stop_early.stop_at = 3;
    EXPECT_EQ(H5FA_iterate(fa.get(), CollectElementsOp, &stop_early),
              H5_ITER_STOP);
    EXPECT_EQ(stop_early.items.size(), 4u);

    {
      ScopedErrorSilence silence;
      IterCollect err_early;
      err_early.error_at = 2;
      EXPECT_EQ(H5FA_iterate(fa.get(), CollectElementsOp, &err_early),
                H5_ITER_ERROR);
    }
  }

  // 5. Flush, evict cache, reopen via H5FA_open, and verify persisted data.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedFA reopened(H5FA_open(f, fa_addr, nullptr));
  ASSERT_NE(reopened.get(), nullptr);

  uint64_t val = 0;
  ASSERT_GE(H5FA_get(reopened.get(), 0, &val), 0);
  EXPECT_EQ(val, 100u);
  ASSERT_GE(H5FA_get(reopened.get(), 3, &val), 0);
  EXPECT_EQ(val, 303u);
  ASSERT_GE(H5FA_get(reopened.get(), 7, &val), 0);
  EXPECT_EQ(val, 707u);
  ASSERT_GE(H5FA_get(reopened.get(), 4, &val), 0);
  EXPECT_EQ(val, H5FA_TEST_FILL);
}

TEST(H5FATest, PagedLifecycleSetGetPartialLastPageAndReopen) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // max_dblk_page_nelmts_bits=3 (8 elmts/page), nelmts=20 -> 3 pages (8, 8, 4)
  // so both full non-last pages and the partial last page are exercised.
  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
  haddr_t fa_addr = HADDR_UNDEF;
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);

    // 1. Write index 0 (creates dblock + page 0) then index 1 (existing page).
    const uint64_t v0 = 1000;
    const uint64_t v1 = 1001;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v0), 0);
    ASSERT_GE(H5FA_set(fa.get(), 1, &v1), 0);

    // 2. Read from uninitialized page 1 and uninitialized last page 2 -> fill.
    uint64_t val = 0;
    ASSERT_GE(H5FA_get(fa.get(), 10, &val), 0);
    EXPECT_EQ(val, H5FA_TEST_FILL);
    ASSERT_GE(H5FA_get(fa.get(), 18, &val), 0);
    EXPECT_EQ(val, H5FA_TEST_FILL);

    // 3. Write to partial last page (page 2, idx 19) and middle page (page 1).
    const uint64_t v19 = 1019;
    const uint64_t v9 = 1009;
    ASSERT_GE(H5FA_set(fa.get(), 19, &v19), 0);
    ASSERT_GE(H5FA_set(fa.get(), 9, &v9), 0);

    // 4. Iterate across all 20 elements spanning all 3 pages.
    IterCollect all;
    EXPECT_EQ(H5FA_iterate(fa.get(), CollectElementsOp, &all), H5_ITER_CONT);
    ASSERT_EQ(all.items.size(), 20u);
    EXPECT_EQ(all.items[0].second, 1000u);
    EXPECT_EQ(all.items[1].second, 1001u);
    EXPECT_EQ(all.items[9].second, 1009u);
    EXPECT_EQ(all.items[10].second, H5FA_TEST_FILL);
    EXPECT_EQ(all.items[19].second, 1019u);
  }

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedFA reopened(H5FA_open(f, fa_addr, nullptr));
  ASSERT_NE(reopened.get(), nullptr);

  uint64_t val = 0;
  ASSERT_GE(H5FA_get(reopened.get(), 1, &val), 0);
  EXPECT_EQ(val, 1001u);
  ASSERT_GE(H5FA_get(reopened.get(), 9, &val), 0);
  EXPECT_EQ(val, 1009u);
  ASSERT_GE(H5FA_get(reopened.get(), 12, &val), 0);
  EXPECT_EQ(val, H5FA_TEST_FILL);
  ASSERT_GE(H5FA_get(reopened.get(), 19, &val), 0);
  EXPECT_EQ(val, 1019u);
}

TEST(H5FATest, PendingDeleteBlocksReopenAndDeletesOnLastClose) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
  ScopedFA fa1(H5FA_create(f, &cparam, nullptr));
  ASSERT_NE(fa1.get(), nullptr);
  haddr_t fa_addr = HADDR_UNDEF;
  ASSERT_GE(H5FA_get_addr(fa1.get(), &fa_addr), 0);

  const uint64_t v = 77;
  ASSERT_GE(H5FA_set(fa1.get(), 0, &v), 0);
  ASSERT_GE(H5FA_set(fa1.get(), 19, &v), 0);

  // Open second handle (file_rc becomes 2), then close first (file_rc == 1).
  ScopedFA fa2(H5FA_open(f, fa_addr, nullptr));
  ASSERT_NE(fa2.get(), nullptr);
  fa1.reset();

  // Delete while fa2 is open marks pending_delete = true.
  ASSERT_GE(H5FA_delete(f, fa_addr, nullptr), 0);

  // Opening while pending_delete is true fails cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA_open(f, fa_addr, nullptr), nullptr);
  }

  // Closing the last handle deletes the array on disk and from cache.
  fa2.reset();
}

TEST(H5FATest, ImmediateDeleteAndInvalidAddressErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t cparam = MakeCparam(/*nelmts=*/8, /*page_bits=*/4);
  haddr_t fa_addr = HADDR_UNDEF;
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr), 0);
    const uint64_t v = 42;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v), 0);
  }
  ASSERT_GE(H5FA_delete(f, fa_addr, nullptr), 0);

  // Address 0 is the HDF5 superblock, not a fixed array header.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FA_open(f, 0, nullptr), nullptr);
    EXPECT_LT(H5FA_delete(f, 0, nullptr), 0);
  }

  // Protected child data block causes H5FA__hdr_delete failure in H5FA_delete
  // (after unprotecting hdr), exercising both error paths cleanly before retry.
  haddr_t fa_addr2 = HADDR_UNDEF;
  haddr_t dblk_addr = HADDR_UNDEF;
  {
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);
    ASSERT_GE(H5FA_get_addr(fa.get(), &fa_addr2), 0);
    const uint64_t v = 99;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v), 0);
    dblk_addr = fa.get()->hdr->dblk_addr;
    ASSERT_TRUE(H5_addr_defined(dblk_addr));
  }

  H5FA_hdr_t* hdr =
      H5FA__hdr_protect(f, fa_addr2, nullptr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(hdr, nullptr);
  H5FA_dblock_t* dblock =
      H5FA__dblock_protect(hdr, dblk_addr, H5AC__READ_ONLY_FLAG);
  ASSERT_NE(dblock, nullptr);
  EXPECT_GE(H5FA__hdr_unprotect(hdr, H5AC__NO_FLAGS_SET), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FA_delete(f, fa_addr2, nullptr), 0);
  }
  EXPECT_GE(H5FA__dblock_unprotect(dblock, H5AC__NO_FLAGS_SET), 0);
  EXPECT_GE(H5FA_delete(f, fa_addr2, nullptr), 0);
}

TEST(H5FATest, DependWithSwmrProxyAndPatchFile) {
  // SWMR mode requires a POSIX/sec2 file rather than Core VFD.
  TempFile tmp_file("h5fa_depend_patch.h5");

  H5AC_proxy_entry_t* parent_proxy = nullptr;
  auto destroy_proxy = absl::MakeCleanup([&parent_proxy] {
    if (parent_proxy != nullptr) {
      H5AC_proxy_entry_dest(parent_proxy);
      parent_proxy = nullptr;
    }
  });

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

    ScopedFile file2(H5Freopen(file.get()));
    ASSERT_TRUE(file2.valid());

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    H5F_t* f2 = GetH5File(file2.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f2, nullptr);

    parent_proxy = H5AC_proxy_entry_create();
    ASSERT_NE(parent_proxy, nullptr);

    const H5FA_create_t cparam = MakeCparam(/*nelmts=*/20, /*page_bits=*/3);
    ScopedFA fa(H5FA_create(f, &cparam, nullptr));
    ASSERT_NE(fa.get(), nullptr);

    // Collide proxy address with cached header so H5FA_depend fails cleanly
    // before attaching, then reset and attach properly.
    {
      parent_proxy->addr = fa.get()->hdr->addr;
      absl::Cleanup restore_addr([parent_proxy] {
        parent_proxy->addr = HADDR_UNDEF;
      });
      ScopedErrorSilence silence;
      EXPECT_LT(H5FA_depend(fa.get(), parent_proxy), 0);
      EXPECT_EQ(fa.get()->hdr->parent, nullptr);
    }

    // Call H5FA_depend twice to cover initial attachment and idempotency.
    ASSERT_GE(H5FA_depend(fa.get(), parent_proxy), 0);
    ASSERT_GE(H5FA_depend(fa.get(), parent_proxy), 0);

    // Patch file handle to f2, call again when already equal, and restore f.
    ASSERT_GE(H5FA_patch_file(fa.get(), f2), 0);
    EXPECT_EQ(fa.get()->f, f2);
    ASSERT_GE(H5FA_patch_file(fa.get(), f2), 0);
    ASSERT_GE(H5FA_patch_file(fa.get(), f), 0);

    const uint64_t v = 88;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v), 0);
    ASSERT_GE(H5FA_set(fa.get(), 19, &v), 0);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    fa.reset();
    ASSERT_GE(H5AC_evict(f), 0);
  }
  std::move(destroy_proxy).Invoke();
}

TEST(H5FATest, CustomClassFaultInjectionCoversErrorPaths) {
  // FaultCtrl and FaultClass must outlive `file` because cached H5FA headers
  // remain referenced until ~ScopedFile.
  g_fail_fill = false;
  auto reset_fill = absl::MakeCleanup([] { g_fail_fill = false; });
  FaultCtrl fault_ctrl{};
  const H5FA_class_t cls = MakeFaultClass();

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5FA_create_t unpaged_cparam = MakeCparam(8, 4, &cls);
  const H5FA_create_t paged_cparam = MakeCparam(20, 3, &cls);

  // 1. Fail header creation when crt_context returns nullptr.
  {
    ScopedErrorSilence silence;
    fault_ctrl.fail_crt_context = true;
    EXPECT_EQ(H5FA_create(f, &unpaged_cparam, &fault_ctrl), nullptr);
    fault_ctrl.fail_crt_context = false;
  }

  // 2. Unpaged array error paths: fill failure and reload decode failure.
  {
    ScopedFA fa(H5FA_create(f, &unpaged_cparam, &fault_ctrl));
    ASSERT_NE(fa.get(), nullptr);

    {
      ScopedErrorSilence silence;
      g_fail_fill = true;
      uint64_t out = 0;
      EXPECT_LT(H5FA_get(fa.get(), 0, &out), 0);
      IterCollect iter;
      EXPECT_EQ(H5FA_iterate(fa.get(), CollectElementsOp, &iter),
                H5_ITER_ERROR);
      const uint64_t v = 1;
      EXPECT_LT(H5FA_set(fa.get(), 0, &v), 0);
      g_fail_fill = false;
    }

    // Create unpaged dblock, then flush + evict dblock while fa keeps hdr
    // pinned with custom cls; decode failure triggers H5FA_get/H5FA_set errors.
    const uint64_t v = 55;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    {
      ScopedErrorSilence silence;
      fault_ctrl.fail_decode = true;
      uint64_t out = 0;
      EXPECT_LT(H5FA_get(fa.get(), 0, &out), 0);
      EXPECT_LT(H5FA_set(fa.get(), 1, &v), 0);
      fault_ctrl.fail_decode = false;
    }
  }

  // 3. Paged array error paths: fill on uninit page, create page, and decode
  //    failure on page reload.
  {
    ScopedFA fa(H5FA_create(f, &paged_cparam, &fault_ctrl));
    ASSERT_NE(fa.get(), nullptr);

    const uint64_t v = 99;
    ASSERT_GE(H5FA_set(fa.get(), 0, &v), 0);

    {
      ScopedErrorSilence silence;
      g_fail_fill = true;
      uint64_t out = 0;
      EXPECT_LT(H5FA_get(fa.get(), 10, &out), 0);
      EXPECT_LT(H5FA_set(fa.get(), 10, &v), 0);
      g_fail_fill = false;
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    {
      ScopedErrorSilence silence;
      fault_ctrl.fail_decode = true;
      uint64_t out = 0;
      EXPECT_LT(H5FA_get(fa.get(), 0, &out), 0);
      EXPECT_LT(H5FA_set(fa.get(), 1, &v), 0);
      fault_ctrl.fail_decode = false;
    }
  }
}

TEST(H5FATest, PublicFixedDimChunkedDatasetsUseFixedArray) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ScopedFile file(
      H5Fcreate("fa_public_dset.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  for (bool use_deflate : {false, true}) {
    const char* name = use_deflate ? "dset_filt" : "dset_raw";
    const hsize_t dims[1] = {40};
    const hsize_t chunk[1] = {4};
    ScopedSpace space(H5Screate_simple(1, dims, dims));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk), 0);
    if (use_deflate) {
      ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    }

    ScopedDataset dset(H5Dcreate2(file.get(), name, H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::vector<int32_t> write_vals(40);
    for (int32_t i = 0; i < 40; ++i) {
      write_vals[i] = i * 5 + 2;
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_vals.data()),
              0);
  }

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  for (const char* name : {"dset_raw", "dset_filt"}) {
    ScopedDataset dset(H5Dopen2(file.get(), name, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int32_t> out(40, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    const std::array<int32_t, 3> sample{out[0], out[19], out[39]};
    EXPECT_THAT(sample, ElementsAre(2, 97, 197));
  }
}

}  // namespace
}  // namespace hdf5
