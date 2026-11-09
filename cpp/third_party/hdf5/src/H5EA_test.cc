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
// Tests extensible array lifecycle, tiered element lookup/set/get across index
// block, data blocks, super blocks, and paged data block pages, flush
// dependencies, iteration, pending/immediate deletion, file patching, custom
// class error injection, and public 1D unlimited chunked datasets
// (third_party/hdf5/src/H5EA.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EA.c

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
#define H5EA_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5EA_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EA_TEST_RESTORE_UNDEF_NDEBUG
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

class ScopedEA {
 public:
  ScopedEA() = default;
  explicit ScopedEA(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEA() { reset(); }

  ScopedEA(const ScopedEA&) = delete;
  ScopedEA& operator=(const ScopedEA&) = delete;

  ScopedEA(ScopedEA&& other) noexcept : ea_(std::exchange(other.ea_, nullptr)) {}
  ScopedEA& operator=(ScopedEA&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.ea_, nullptr));
    }
    return *this;
  }

  void reset(H5EA_t* next = nullptr) {
    if (ea_ != nullptr) {
      H5EA_close(ea_);
    }
    ea_ = next;
  }

  H5EA_t* release() { return std::exchange(ea_, nullptr); }
  H5EA_t* get() const { return ea_; }

 private:
  H5EA_t* ea_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// Creation parameters exercising every H5EA storage tier with small indices:
// - idx_blk_elmts = 4: indices [0, 3] reside in the index block.
// - sup_blk_min_data_ptrs = 2, data_blk_min_elmts = 4:
//   - sblk 0 (iblock->dblk_addrs[0]): 1 dblk * 4 elmts -> indices [4, 7]
//   - sblk 1 (iblock->dblk_addrs[1]): 1 dblk * 8 elmts -> indices [8, 15]
//   - sblk 2 (iblock->sblk_addrs[0]): 2 dblks * 8 elmts -> indices [16, 31]
//     (dblk_nelmts == 8 <= dblk_page_nelmts == 8, so unpaged)
//   - sblk 3 (iblock->sblk_addrs[1]): 2 dblks * 16 elmts -> indices [32, 63]
//     (dblk_nelmts == 16 > dblk_page_nelmts == 8, so 2 pages/dblk)
H5EA_create_t MakeTieredCparam(const H5EA_class_t* cls = H5EA_CLS_TEST,
                               uint8_t idx_blk_elmts = 4) {
  H5EA_create_t cparam{};
  cparam.cls = cls;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 12;
  cparam.idx_blk_elmts = idx_blk_elmts;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 2;
  cparam.max_dblk_page_nelmts_bits = 3;
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

// Fault toggles for custom H5EA_class_t error-injection tests. Always cleared
// via absl::Cleanup before returning from each test.
struct FaultCtrl {
  bool fail_crt_context = false;
  bool fail_fill = false;
  bool fail_encode = false;
  bool fail_decode = false;
};

FaultCtrl& GlobalFaults() {
  static FaultCtrl* ctrl = new FaultCtrl();
  return *ctrl;
}

void* FaultCrtContext(void* udata) {
  auto* ctrl = udata != nullptr ? static_cast<FaultCtrl*>(udata)
                                : &GlobalFaults();
  if (ctrl->fail_crt_context) {
    return nullptr;
  }
  return ctrl;
}

herr_t FaultDstContext(void* /*ctx*/) { return 0; }

herr_t FaultFill(void* nat_blk, size_t nelmts) {
  if (GlobalFaults().fail_fill) {
    return -1;
  }
  auto* dst = static_cast<uint64_t*>(nat_blk);
  for (size_t i = 0; i < nelmts; ++i) {
    dst[i] = H5EA_TEST_FILL;
  }
  return 0;
}

herr_t FaultEncode(void* raw, const void* elmt, size_t nelmts, void* ctx) {
  auto* ctrl = ctx != nullptr ? static_cast<FaultCtrl*>(ctx) : &GlobalFaults();
  if (ctrl->fail_encode) {
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
  auto* ctrl = ctx != nullptr ? static_cast<FaultCtrl*>(ctx) : &GlobalFaults();
  if (ctrl->fail_decode) {
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

H5EA_class_t MakeFaultClass() {
  H5EA_class_t cls{};
  cls.id = H5EA_CLS_TEST_ID;
  cls.name = "H5EA_fault_test";
  cls.nat_elmt_size = sizeof(uint64_t);
  cls.crt_context = FaultCrtContext;
  cls.dst_context = FaultDstContext;
  cls.fill = FaultFill;
  cls.encode = FaultEncode;
  cls.decode = FaultDecode;
  cls.debug = nullptr;
  cls.crt_dbg_ctx = nullptr;
  cls.dst_dbg_ctx = nullptr;
  return cls;
}

TEST(H5EATest, SparseReadsAndAllTierWritesIterateAndReopen) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  haddr_t ea_addr = HADDR_UNDEF;
  {
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    EXPECT_TRUE(H5_addr_defined(ea_addr));

    hsize_t nelmts = 999;
    ASSERT_GE(H5EA_get_nelmts(ea.get(), &nelmts), 0);
    EXPECT_EQ(nelmts, 0u);

    // 1. Read beyond max_idx_set returns fill value.
    uint64_t val = 0;
    ASSERT_GE(H5EA_get(ea.get(), 0, &val), 0);
    EXPECT_EQ(val, H5EA_TEST_FILL);

    // 2. Write a single high element in sblk 3, dblk 1, page 0 (idx 50) first
    //    so max_idx_set = 51 while lower blocks and sibling pages remain
    //    unallocated on disk.
    const uint64_t v50 = 5050;
    ASSERT_GE(H5EA_set(ea.get(), 50, &v50), 0);
    ASSERT_GE(H5EA_get_nelmts(ea.get(), &nelmts), 0);
    EXPECT_EQ(nelmts, 51u);

    // 3. Sparse reads across each unallocated tier (< max_idx_set) return fill:
    //    idx 1 (iblock), idx 5 (uncreated iblock dblk), idx 17 (uncreated sblk),
    //    idx 33 (created sblk 3, uncreated dblk 0), idx 58 (created dblk 1,
    //    uninitialized page 1).
    for (hsize_t sparse_idx :
         {hsize_t{1}, hsize_t{5}, hsize_t{17}, hsize_t{33}, hsize_t{58}}) {
      val = 0;
      ASSERT_GE(H5EA_get(ea.get(), sparse_idx, &val), 0);
      EXPECT_EQ(val, H5EA_TEST_FILL) << "sparse_idx=" << sparse_idx;
    }

    // 4. Populate elements across every tier: index block, direct data blocks,
    //    unpaged super block data blocks, and paged data block pages.
    constexpr auto kIndices = std::to_array<hsize_t>(
        {0, 3, 4, 7, 8, 15, 16, 24, 32, 40, 48, 58, 63});
    for (hsize_t idx : kIndices) {
      const uint64_t encoded = idx * 100 + 7;
      ASSERT_GE(H5EA_set(ea.get(), idx, &encoded), 0);
    }
    // Overwrite an existing element (will_extend == false).
    const uint64_t updated_50 = 5007;
    ASSERT_GE(H5EA_set(ea.get(), 50, &updated_50), 0);

    ASSERT_GE(H5EA_get_nelmts(ea.get(), &nelmts), 0);
    EXPECT_EQ(nelmts, 64u);

    // 5. Verify H5EA_iterate visits all [0, 64) elements, supports H5_ITER_STOP,
    //    and propagates H5_ITER_ERROR.
    IterCollect all;
    EXPECT_EQ(H5EA_iterate(ea.get(), CollectElementsOp, &all), H5_ITER_CONT);
    ASSERT_EQ(all.items.size(), 64u);
    for (hsize_t idx : kIndices) {
      EXPECT_EQ(all.items[idx].second, idx * 100 + 7);
    }
    EXPECT_EQ(all.items[50].second, updated_50);
    EXPECT_EQ(all.items[1].second, H5EA_TEST_FILL);

    IterCollect stop_early;
    stop_early.stop_at = 4;
    EXPECT_EQ(H5EA_iterate(ea.get(), CollectElementsOp, &stop_early),
              H5_ITER_STOP);
    EXPECT_EQ(stop_early.items.size(), 5u);

    {
      ScopedErrorSilence silence;
      IterCollect err_early;
      err_early.error_at = 2;
      EXPECT_EQ(H5EA_iterate(ea.get(), CollectElementsOp, &err_early),
                H5_ITER_ERROR);
    }
  }

  // 6. Flush, evict entire cache, reopen via H5EA_open, and verify persisted
  //    data across all tiers.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedEA reopened(H5EA_open(f, ea_addr, nullptr));
  ASSERT_NE(reopened.get(), nullptr);

  hsize_t nelmts = 0;
  ASSERT_GE(H5EA_get_nelmts(reopened.get(), &nelmts), 0);
  EXPECT_EQ(nelmts, 64u);

  for (hsize_t idx :
       {hsize_t{0}, hsize_t{4}, hsize_t{16}, hsize_t{32}, hsize_t{40},
        hsize_t{58}, hsize_t{63}}) {
    uint64_t val = 0;
    ASSERT_GE(H5EA_get(reopened.get(), idx, &val), 0);
    EXPECT_EQ(val, idx * 100 + 7);
  }
  uint64_t val50 = 0;
  ASSERT_GE(H5EA_get(reopened.get(), 50, &val50), 0);
  EXPECT_EQ(val50, 5007u);
}

TEST(H5EATest, FlushBetweenExtensionsExercisesFlushDependencies) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Flush clears has_hdr_depend on existing cached blocks, so subsequent
  // extending writes re-create flush dependencies on existing dblocks, sblocks,
  // and dblk_pages.
  const uint64_t v = 123;
  ASSERT_GE(H5EA_set(ea.get(), 4, &v), 0);   // sblk 0, dblk 0
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5EA_set(ea.get(), 5, &v), 0);   // extend existing iblock dblk
  ASSERT_GE(H5EA_set(ea.get(), 16, &v), 0);  // sblk 2, dblk 0 (unpaged)
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5EA_set(ea.get(), 17, &v), 0);  // extend existing sblk dblk
  ASSERT_GE(H5EA_set(ea.get(), 24, &v), 0);  // sblk 2, dblk 1 (existing sblk)
  ASSERT_GE(H5EA_set(ea.get(), 32, &v), 0);  // sblk 3, dblk 0, page 0
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5EA_set(ea.get(), 33, &v), 0);  // extend existing dblk_page
  ASSERT_GE(H5EA_set(ea.get(), 40, &v), 0);  // sblk 3, dblk 0, page 1

  uint64_t out = 0;
  ASSERT_GE(H5EA_get(ea.get(), 40, &out), 0);
  EXPECT_EQ(out, v);
}

TEST(H5EATest, ZeroIdxBlkElmtsAndReadOnlyMissingIblock) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam =
      MakeTieredCparam(H5EA_CLS_TEST, /*idx_blk_elmts=*/0);
  ScopedEA ea(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea.get(), nullptr);

  // Temporarily bump max_idx_set before any iblock exists so H5EA_get invokes
  // H5EA__lookup_elmt read-only on an undefined hdr->idx_blk_addr.
  ea.get()->hdr->stats.stored.max_idx_set = 1;
  uint64_t val = 0;
  ASSERT_GE(H5EA_get(ea.get(), 0, &val), 0);
  EXPECT_EQ(val, H5EA_TEST_FILL);
  ea.get()->hdr->stats.stored.max_idx_set = 0;

  const uint64_t v0 = 900;
  const uint64_t v32 = 932;
  ASSERT_GE(H5EA_set(ea.get(), 0, &v0), 0);
  ASSERT_GE(H5EA_set(ea.get(), 32, &v32), 0);
  ASSERT_GE(H5EA_get(ea.get(), 0, &val), 0);
  EXPECT_EQ(val, v0);
  ASSERT_GE(H5EA_get(ea.get(), 32, &val), 0);
  EXPECT_EQ(val, v32);
}

TEST(H5EATest, PendingDeleteBlocksReopenAndDeletesOnLastClose) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  const H5EA_create_t cparam = MakeTieredCparam();
  ScopedEA ea1(H5EA_create(f, &cparam, nullptr));
  ASSERT_NE(ea1.get(), nullptr);
  haddr_t ea_addr = HADDR_UNDEF;
  ASSERT_GE(H5EA_get_addr(ea1.get(), &ea_addr), 0);

  const uint64_t v = 77;
  ASSERT_GE(H5EA_set(ea1.get(), 0, &v), 0);
  ASSERT_GE(H5EA_set(ea1.get(), 32, &v), 0);

  // Open a second handle (file_rc becomes 2), then close the first (file_rc=1).
  ScopedEA ea2(H5EA_open(f, ea_addr, nullptr));
  ASSERT_NE(ea2.get(), nullptr);
  ea1.reset();

  // Deleting while ea2 is open marks pending_delete = true.
  ASSERT_GE(H5EA_delete(f, ea_addr, nullptr), 0);

  // Opening while pending_delete is true must fail cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5EA_open(f, ea_addr, nullptr), nullptr);
  }

  // Closing the last handle deletes the array on disk and from cache.
  ea2.reset();
}

TEST(H5EATest, ImmediateDeleteAndInvalidAddressErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // Delete after closing with dirty unflushed blocks across all tiers.
  const H5EA_create_t cparam = MakeTieredCparam();
  haddr_t ea_addr = HADDR_UNDEF;
  {
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    const uint64_t v = 42;
    for (hsize_t idx : {hsize_t{0}, hsize_t{4}, hsize_t{16}, hsize_t{32}}) {
      ASSERT_GE(H5EA_set(ea.get(), idx, &v), 0);
    }
  }
  ASSERT_GE(H5EA_delete(f, ea_addr, nullptr), 0);

  // Opening or deleting at address 0 (superblock, not an EA header) fails.
  ScopedErrorSilence silence;
  EXPECT_EQ(H5EA_open(f, 0, nullptr), nullptr);
  EXPECT_LT(H5EA_delete(f, 0, nullptr), 0);
}

TEST(H5EATest, DependWithSwmrProxyAndPatchFile) {
  TempFile tmp_file("h5ea_depend_patch.h5");

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

    const H5EA_create_t cparam = MakeTieredCparam();
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);

    // Call H5EA_depend twice to cover both initial attachment and idempotency.
    ASSERT_GE(H5EA_depend(ea.get(), parent_proxy), 0);
    ASSERT_GE(H5EA_depend(ea.get(), parent_proxy), 0);

    // Exercise H5EA_patch_file with a distinct file handle and again when equal.
    ASSERT_GE(H5EA_patch_file(ea.get(), f2), 0);
    EXPECT_EQ(ea.get()->f, f2);
    ASSERT_GE(H5EA_patch_file(ea.get(), f2), 0);
    ASSERT_GE(H5EA_patch_file(ea.get(), f), 0);

    const uint64_t v = 88;
    for (hsize_t idx : {hsize_t{0}, hsize_t{4}, hsize_t{16}, hsize_t{32}}) {
      ASSERT_GE(H5EA_set(ea.get(), idx, &v), 0);
    }
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ea.reset();
    ASSERT_GE(H5AC_evict(f), 0);
  }
  std::move(destroy_proxy).Invoke();
}

TEST(H5EATest, CustomClassFaultInjectionCoversErrorPaths) {
  auto reset_faults =
      absl::MakeCleanup([] { GlobalFaults() = FaultCtrl{}; });
  H5EA_class_t cls = MakeFaultClass();
  const H5EA_create_t cparam = MakeTieredCparam(&cls);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Fail header creation when crt_context returns nullptr.
  {
    ScopedErrorSilence silence;
    GlobalFaults().fail_crt_context = true;
    EXPECT_EQ(H5EA_create(f, &cparam, nullptr), nullptr);
    GlobalFaults().fail_crt_context = false;
  }

  // 2. Fail fill during H5EA_get (beyond max_idx_set) and H5EA_set (iblock).
  {
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);

    ScopedErrorSilence silence;
    GlobalFaults().fail_fill = true;
    uint64_t out = 0;
    EXPECT_LT(H5EA_get(ea.get(), 0, &out), 0);
    const uint64_t v = 1;
    EXPECT_LT(H5EA_set(ea.get(), 0, &v), 0);
    GlobalFaults().fail_fill = false;

    // Create iblock, then fail fill on iblock dblk, sblk dblk, and dblk page.
    ASSERT_GE(H5EA_set(ea.get(), 0, &v), 0);
    GlobalFaults().fail_fill = true;
    EXPECT_LT(H5EA_set(ea.get(), 4, &v), 0);   // iblock dblk create fails
    EXPECT_LT(H5EA_set(ea.get(), 16, &v), 0);  // sblk dblk create fails
    GlobalFaults().fail_fill = false;

    // Create paged dblk (page 0), then fail fill on sibling page 1.
    ASSERT_GE(H5EA_set(ea.get(), 32, &v), 0);
    GlobalFaults().fail_fill = true;
    EXPECT_LT(H5EA_set(ea.get(), 40, &v), 0);  // dblk page create fails
    // Also fail fill on sparse H5EA_get (thing == NULL) and H5EA_iterate.
    EXPECT_LT(H5EA_get(ea.get(), 5, &out), 0);
    IterCollect iter;
    EXPECT_EQ(H5EA_iterate(ea.get(), CollectElementsOp, &iter), H5_ITER_ERROR);
    GlobalFaults().fail_fill = false;
  }

  // 3. Keep ea open so hdr stays pinned with custom cls; flush + evict child
  //    blocks, then inject decode failures on iblock, dblk, and dblk_page.
  {
    ScopedEA ea(H5EA_create(f, &cparam, nullptr));
    ASSERT_NE(ea.get(), nullptr);
    const uint64_t v = 55;
    for (hsize_t idx : {hsize_t{0}, hsize_t{4}, hsize_t{16}, hsize_t{32}}) {
      ASSERT_GE(H5EA_set(ea.get(), idx, &v), 0);
    }

    // Inject decode failure when reloading iblock from disk.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);
    {
      ScopedErrorSilence silence;
      GlobalFaults().fail_decode = true;
      uint64_t out = 0;
      EXPECT_LT(H5EA_get(ea.get(), 0, &out), 0);
      GlobalFaults().fail_decode = false;
    }

    // Bring iblock into cache cleanly, then fail decode when reloading child
    // dblk (iblock->dblk), sblk unpaged dblk, and paged dblk_page.
    uint64_t out = 0;
    ASSERT_GE(H5EA_get(ea.get(), 0, &out), 0);
    EXPECT_EQ(out, v);
    {
      ScopedErrorSilence silence;
      GlobalFaults().fail_decode = true;
      EXPECT_LT(H5EA_get(ea.get(), 4, &out), 0);
      EXPECT_LT(H5EA_get(ea.get(), 16, &out), 0);
      EXPECT_LT(H5EA_get(ea.get(), 32, &out), 0);
      GlobalFaults().fail_decode = false;
    }
  }
}

TEST(H5EATest, PublicOneDimUnlimitedChunkedDatasetsUseExtensibleArray) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ScopedFile file(
      H5Fcreate("ea_public_dset.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  for (bool use_deflate : {false, true}) {
    const char* name = use_deflate ? "dset_filt" : "dset_raw";
    hsize_t dims[1] = {10};
    hsize_t maxdims[1] = {H5S_UNLIMITED};
    hsize_t chunk[1] = {2};
    ScopedSpace space(H5Screate_simple(1, dims, maxdims));
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

    std::vector<int32_t> write_vals(10);
    for (int32_t i = 0; i < 10; ++i) {
      write_vals[i] = i * 3 + 1;
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_vals.data()),
              0);

    // Extend and write at a sparse offset to exercise EA growth.
    const hsize_t new_dims[1] = {64};
    ASSERT_GE(H5Dset_extent(dset.get(), new_dims), 0);
    ScopedSpace fspace(H5Dget_space(dset.get()));
    ASSERT_TRUE(fspace.valid());
    const hsize_t start[1] = {60};
    const hsize_t count[1] = {4};
    ASSERT_GE(
        H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, start, nullptr, count,
                            nullptr),
        0);
    ScopedSpace mspace(H5Screate_simple(1, count, nullptr));
    ASSERT_TRUE(mspace.valid());
    const std::array<int32_t, 4> tail = {600, 601, 602, 603};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace.get(), fspace.get(),
                       H5P_DEFAULT, tail.data()),
              0);
  }

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  for (const char* name : {"dset_raw", "dset_filt"}) {
    ScopedDataset dset(H5Dopen2(file.get(), name, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int32_t> out(64, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[9], 28);
    const std::array<int32_t, 4> tail_read{out[60], out[61], out[62], out[63]};
    EXPECT_THAT(tail_read, ElementsAre(600, 601, 602, 603));
  }
}

}  // namespace
}  // namespace hdf5
