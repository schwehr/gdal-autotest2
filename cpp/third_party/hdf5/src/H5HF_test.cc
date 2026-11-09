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
// Tests fractal heap lifecycle, tiny/managed/huge object storage, direct and
// indirect huge IDs, filtered objects, concurrent open/reopen/pending delete,
// and error paths for third_party/hdf5/src/H5HF.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HF.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h pulls in H5B2private.h, which uses `try` as a C parameter name,
// colliding with both the C++ keyword and Google3's `-fno-exceptions` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

// RAII wrapper for pushing/popping an HDF5 API context and metadata tag.
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

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_create_t MakeHeapCreateParams(
    uint16_t id_len = 0, uint32_t max_man_size = 512, unsigned width = 4,
    size_t start_block_size = 512, size_t max_direct_size = 4096,
    unsigned max_index = 20, unsigned start_root_rows = 1,
    bool checksum_dblocks = true) {
  H5HF_create_t cparam{};
  cparam.managed.width = width;
  cparam.managed.start_block_size = start_block_size;
  cparam.managed.max_direct_size = max_direct_size;
  cparam.managed.max_index = max_index;
  cparam.managed.start_root_rows = start_root_rows;
  cparam.checksum_dblocks = checksum_dblocks;
  cparam.max_man_size = max_man_size;
  cparam.id_len = id_len;
  return cparam;
}

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((seed + i * 17) & 0xffU);
  }
  return data;
}

// Custom H5Z filter state used to exercise filter rejection error paths.
constexpr H5Z_filter_t kToggleFilterId = H5Z_FILTER_RESERVED + 11;
bool g_toggle_filter_allow = true;

htri_t ToggleFilterCanApply(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                            hid_t /*space_id*/) {
  return g_toggle_filter_allow ? 1 : -1;
}

size_t ToggleFilterApply(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                         const unsigned int /*cd_values*/[], size_t nbytes,
                         size_t* /*buf_size*/, void** /*buf*/) {
  return nbytes;
}

TEST(H5HFTest, CreateMetadataAccessorsAndCreateFailure) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // 1. Create a heap with id_len=0 (auto minimal managed ID) and inspect state.
  {
    const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/0);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });

    size_t id_len = 0;
    EXPECT_GE(H5HF_get_id_len(fh, &id_len), 0);
    EXPECT_GT(id_len, 1U);

    haddr_t heap_addr = HADDR_UNDEF;
    EXPECT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);
    EXPECT_NE(heap_addr, HADDR_UNDEF);

    H5HF_stat_t stats{};
    EXPECT_GE(H5HF_stat_info(fh, &stats), 0);
    EXPECT_EQ(stats.man_nobjs, 0U);
    EXPECT_EQ(stats.huge_nobjs, 0U);
    EXPECT_EQ(stats.tiny_nobjs, 0U);

    hsize_t heap_size = 0;
    EXPECT_GE(H5HF_size(fh, &heap_size), 0);
    EXPECT_GT(heap_size, 0U);
  }

  // 2. Create heaps with id_len=1 (direct huge ID) and explicit id_len=32.
  {
    const H5HF_create_t direct_param = MakeHeapCreateParams(/*id_len=*/1);
    H5HF_t* fh = H5HF_create(f, &direct_param);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    size_t id_len = 0;
    EXPECT_GE(H5HF_get_id_len(fh, &id_len), 0);
    EXPECT_EQ(id_len, 17U);
  }
  {
    const H5HF_create_t custom_param = MakeHeapCreateParams(/*id_len=*/32);
    H5HF_t* fh = H5HF_create(f, &custom_param);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    size_t id_len = 0;
    EXPECT_GE(H5HF_get_id_len(fh, &id_len), 0);
    EXPECT_EQ(id_len, 32U);
  }

  // 3. Invalid creation parameters fail H5HF__hdr_create -> H5HF_create returns
  //    nullptr.
  ScopedErrorSilence silence;
  {
    const H5HF_create_t too_small_id = MakeHeapCreateParams(/*id_len=*/2);
    EXPECT_EQ(H5HF_create(f, &too_small_id), nullptr);

    const H5HF_create_t too_large_id =
        MakeHeapCreateParams(/*id_len=*/H5HF_MAX_ID_LEN + 1);
    EXPECT_EQ(H5HF_create(f, &too_large_id), nullptr);

    const H5HF_create_t too_large_man =
        MakeHeapCreateParams(/*id_len=*/0, /*max_man_size=*/4096);
    EXPECT_EQ(H5HF_create(f, &too_large_man), nullptr);
  }
}

TEST(H5HFTest, TinyObjectsShortAndExtendedLifecycle) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  struct OpState {
    std::vector<uint8_t> observed;
    bool should_fail = false;
  };
  auto op_cb = [](const void* obj, size_t obj_len, void* op_data) -> herr_t {
    auto* st = static_cast<OpState*>(op_data);
    if (st->should_fail) {
      return -1;
    }
    const auto* bytes = static_cast<const uint8_t*>(obj);
    st->observed.assign(bytes, bytes + obj_len);
    return 0;
  };

  // 1. Short tiny format (id_len=12 => tiny_max_len=11,
  // tiny_len_extended=false).
  {
    const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/12);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    for (size_t sz : {size_t{1}, size_t{5}, size_t{11}}) {
      std::vector<uint8_t> payload = MakePattern(sz, static_cast<uint8_t>(sz));
      std::array<uint8_t, 12> id{};
      ASSERT_GE(H5HF_insert(fh, payload.size(), payload.data(), id.data()), 0);

      size_t got_len = 0;
      EXPECT_GE(H5HF_get_obj_len(fh, id.data(), &got_len), 0);
      EXPECT_EQ(got_len, sz);

      hsize_t got_off = 999;
      EXPECT_GE(H5HF_get_obj_off(fh, id.data(), &got_off), 0);
      EXPECT_EQ(got_off, 0U);

      std::vector<uint8_t> read_buf(sz, 0xaaU);
      EXPECT_GE(H5HF_read(fh, id.data(), read_buf.data()), 0);
      EXPECT_THAT(read_buf, ElementsAreArray(payload));

      OpState st;
      EXPECT_GE(H5HF_op(fh, id.data(), op_cb, &st), 0);
      EXPECT_THAT(st.observed, ElementsAreArray(payload));

      {
        ScopedErrorSilence silence;
        st.should_fail = true;
        EXPECT_LT(H5HF_op(fh, id.data(), op_cb, &st), 0);

        bool id_changed = false;
        EXPECT_LT(H5HF_write(fh, id.data(), &id_changed, payload.data()), 0);
      }

      EXPECT_GE(H5HF_remove(fh, id.data()), 0);
    }

    H5HF_stat_t stats{};
    EXPECT_GE(H5HF_stat_info(fh, &stats), 0);
    EXPECT_EQ(stats.tiny_nobjs, 0U);
    EXPECT_EQ(stats.tiny_size, 0U);
  }

  // 2. Extended tiny format (id_len=24 => tiny_max_len=22,
  //    tiny_len_extended=true).
  {
    const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/24);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    const std::vector<uint8_t> payload = MakePattern(20, 0x33U);
    std::array<uint8_t, 24> id{};
    ASSERT_GE(H5HF_insert(fh, payload.size(), payload.data(), id.data()), 0);

    size_t got_len = 0;
    EXPECT_GE(H5HF_get_obj_len(fh, id.data(), &got_len), 0);
    EXPECT_EQ(got_len, 20U);

    std::vector<uint8_t> read_buf(20, 0x55U);
    EXPECT_GE(H5HF_read(fh, id.data(), read_buf.data()), 0);
    EXPECT_THAT(read_buf, ElementsAreArray(payload));

    EXPECT_GE(H5HF_remove(fh, id.data()), 0);
  }
}

TEST(H5HFTest, ManagedObjectsMultiBlockLifecycleAndWriteOps) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  // Small blocks force growth from root direct block to root indirect block,
  // initializing hdr->next_block iterator and free space manager before close.
  const H5HF_create_t cparam = MakeHeapCreateParams(
      /*id_len=*/0, /*max_man_size=*/200, /*width=*/2,
      /*start_block_size=*/64, /*max_direct_size=*/512, /*max_index=*/12,
      /*start_root_rows=*/1, /*checksum_dblocks=*/true);
  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() {
    if (fh != nullptr) {
      H5HF_close(fh);
    }
  });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

  constexpr int kCount = 12;
  std::vector<std::vector<uint8_t>> ids(kCount, std::vector<uint8_t>(id_len));
  std::vector<std::vector<uint8_t>> payloads;
  for (int i = 0; i < kCount; ++i) {
    payloads.push_back(MakePattern(36, static_cast<uint8_t>(i * 7 + 1)));
    ASSERT_GE(H5HF_insert(fh, payloads.back().size(), payloads.back().data(),
                          ids[i].data()),
              0);
  }

  for (int i = 0; i < kCount; ++i) {
    size_t got_len = 0;
    EXPECT_GE(H5HF_get_obj_len(fh, ids[i].data(), &got_len), 0);
    EXPECT_EQ(got_len, payloads[i].size());

    hsize_t got_off = 0;
    EXPECT_GE(H5HF_get_obj_off(fh, ids[i].data(), &got_off), 0);
    EXPECT_GT(got_off, 0U);

    std::vector<uint8_t> read_buf(got_len, 0xffU);
    EXPECT_GE(H5HF_read(fh, ids[i].data(), read_buf.data()), 0);
    EXPECT_THAT(read_buf, ElementsAreArray(payloads[i]));
  }

  // Overwrite object #5 in place via H5HF_write (exercises H5HF__op_write).
  const std::vector<uint8_t> replacement = MakePattern(36, 0xa5U);
  bool id_changed = false;
  ASSERT_GE(H5HF_write(fh, ids[5].data(), &id_changed, replacement.data()), 0);
  std::vector<uint8_t> verify_buf(36, 0x00U);
  ASSERT_GE(H5HF_read(fh, ids[5].data(), verify_buf.data()), 0);
  EXPECT_THAT(verify_buf, ElementsAreArray(replacement));

  // Direct H5HF_op on managed object (success and failing user callback).
  std::vector<uint8_t> op_copy;
  auto read_op = [](const void* obj, size_t len, void* op_data) -> herr_t {
    const auto* p = static_cast<const uint8_t*>(obj);
    static_cast<std::vector<uint8_t>*>(op_data)->assign(p, p + len);
    return 0;
  };
  EXPECT_GE(H5HF_op(fh, ids[5].data(), read_op, &op_copy), 0);
  EXPECT_THAT(op_copy, ElementsAreArray(replacement));

  {
    ScopedErrorSilence silence;
    auto fail_op = [](const void*, size_t, void*) -> herr_t { return -1; };
    EXPECT_LT(H5HF_op(fh, ids[5].data(), fail_op, nullptr), 0);
  }

  // Remove two middle objects to activate H5FS free-space tracking, then
  // re-insert to verify reuse, and close explicitly to exercise
  // H5HF__space_close and H5HF__man_iter_reset.
  ASSERT_GE(H5HF_remove(fh, ids[2].data()), 0);
  ASSERT_GE(H5HF_remove(fh, ids[6].data()), 0);

  const std::vector<uint8_t> reused = MakePattern(36, 0x7eU);
  std::vector<uint8_t> reused_id(id_len);
  ASSERT_GE(H5HF_insert(fh, reused.size(), reused.data(), reused_id.data()), 0);
  std::vector<uint8_t> reused_read(36, 0);
  ASSERT_GE(H5HF_read(fh, reused_id.data(), reused_read.data()), 0);
  EXPECT_THAT(reused_read, ElementsAreArray(reused));

  EXPECT_GE(H5HF_close(fh), 0);
  fh = nullptr;
}

TEST(H5HFTest, HugeObjectsDirectIndirectAndFilteredLifecycle) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  struct Case {
    uint16_t id_len;
    bool filtered;
  };
  constexpr std::array<Case, 4> kCases = {{
      {.id_len = 0, .filtered = false},  // indirect unfiltered
      {.id_len = 1, .filtered = false},  // direct unfiltered
      {.id_len = 0, .filtered = true},   // indirect filtered
      {.id_len = 1, .filtered = true},   // direct filtered
  }};

  for (const Case& tc : kCases) {
    H5HF_create_t cparam = MakeHeapCreateParams(tc.id_len, /*max_man_size=*/64);
    auto reset_pline = absl::MakeCleanup([&]() {
      if (cparam.pline.nused > 0) {
        H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
      }
    });
    if (tc.filtered) {
      unsigned level = 6;
      ASSERT_GE(H5Z_append(&cparam.pline, H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL,
                           1, &level),
                0);
    }

    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    size_t id_len = 0;
    ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);

    const std::vector<uint8_t> obj1 = MakePattern(128, 0x11U);
    const std::vector<uint8_t> obj2 = MakePattern(160, 0x22U);
    std::vector<uint8_t> id1(id_len);
    std::vector<uint8_t> id2(id_len);

    ASSERT_GE(H5HF_insert(fh, obj1.size(), obj1.data(), id1.data()), 0);
    ASSERT_GE(H5HF_insert(fh, obj2.size(), obj2.data(), id2.data()), 0);

    size_t len1 = 0;
    EXPECT_GE(H5HF_get_obj_len(fh, id1.data(), &len1), 0);
    EXPECT_EQ(len1, obj1.size());

    hsize_t off1 = 0;
    EXPECT_GE(H5HF_get_obj_off(fh, id1.data(), &off1), 0);
    EXPECT_NE(off1, HADDR_UNDEF);
    EXPECT_GT(off1, 0U);

    std::vector<uint8_t> out1(obj1.size(), 0xaaU);
    EXPECT_GE(H5HF_read(fh, id1.data(), out1.data()), 0);
    EXPECT_THAT(out1, ElementsAreArray(obj1));

    // H5HF_write succeeds on unfiltered huge objects and rejects filtered ones.
    const std::vector<uint8_t> updated1 = MakePattern(128, 0x99U);
    bool id_changed = false;
    if (!tc.filtered) {
      EXPECT_GE(H5HF_write(fh, id1.data(), &id_changed, updated1.data()), 0);
      std::fill(out1.begin(), out1.end(), 0);
      EXPECT_GE(H5HF_read(fh, id1.data(), out1.data()), 0);
      EXPECT_THAT(out1, ElementsAreArray(updated1));
    } else {
      ScopedErrorSilence silence;
      EXPECT_LT(H5HF_write(fh, id1.data(), &id_changed, updated1.data()), 0);
    }

    // H5HF_op on huge object: success and callback error.
    std::vector<uint8_t> op_out;
    auto op_ok = [](const void* obj, size_t len, void* op_data) -> herr_t {
      const auto* p = static_cast<const uint8_t*>(obj);
      static_cast<std::vector<uint8_t>*>(op_data)->assign(p, p + len);
      return 0;
    };
    EXPECT_GE(H5HF_op(fh, id2.data(), op_ok, &op_out), 0);
    EXPECT_THAT(op_out, ElementsAreArray(obj2));

    {
      ScopedErrorSilence silence;
      auto op_fail = [](const void*, size_t, void*) -> herr_t { return -1; };
      EXPECT_LT(H5HF_op(fh, id2.data(), op_fail, nullptr), 0);
    }

    // Remove id1 while keeping id2 alive; probing missing indirect IDs hits
    // all NOTFOUND error paths in H5HF_get_obj_len/off/read/write/op/remove.
    EXPECT_GE(H5HF_remove(fh, id1.data()), 0);
    if (!tc.id_len) {
      ScopedErrorSilence silence;
      size_t missing_len = 0;
      hsize_t missing_off = 0;
      std::vector<uint8_t> tmp(128);
      EXPECT_LT(H5HF_get_obj_len(fh, id1.data(), &missing_len), 0);
      EXPECT_LT(H5HF_get_obj_off(fh, id1.data(), &missing_off), 0);
      EXPECT_LT(H5HF_read(fh, id1.data(), tmp.data()), 0);
      EXPECT_LT(H5HF_write(fh, id1.data(), &id_changed, tmp.data()), 0);
      EXPECT_LT(H5HF_op(fh, id1.data(), op_ok, &op_out), 0);
      EXPECT_LT(H5HF_remove(fh, id1.data()), 0);
    }
    EXPECT_GE(H5HF_remove(fh, id2.data()), 0);
  }
}

TEST(H5HFTest, OpenReopenPendingDeleteAndImmediateDelete) {
  haddr_t heap1_addr = HADDR_UNDEF;
  haddr_t heap2_addr = HADDR_UNDEF;
  std::vector<uint8_t> h2_man_id;
  std::vector<uint8_t> h2_tiny_id;
  std::vector<uint8_t> h2_huge_id;
  const std::vector<uint8_t> man_val = MakePattern(48, 0x10U);
  const std::vector<uint8_t> tiny_val = MakePattern(6, 0x20U);
  const std::vector<uint8_t> huge_val = MakePattern(600, 0x30U);

  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    // 1. Concurrent open + pending delete + close teardown deletion.
    {
      const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/16);
      H5HF_t* fh1 = H5HF_create(f, &cparam);
      ASSERT_NE(fh1, nullptr);
      auto close_fh1 = absl::MakeCleanup([&]() {
        if (fh1 != nullptr) {
          H5HF_close(fh1);
        }
      });
      ASSERT_GE(H5HF_get_heap_addr(fh1, &heap1_addr), 0);

      std::array<uint8_t, 16> man_id{};
      ASSERT_GE(H5HF_insert(fh1, man_val.size(), man_val.data(), man_id.data()),
                0);

      // Concurrent H5HF_open bumps file_rc to 2.
      H5HF_t* fh1_second = H5HF_open(f, heap1_addr);
      ASSERT_NE(fh1_second, nullptr);
      auto close_fh1_second = absl::MakeCleanup([&]() {
        if (fh1_second != nullptr) {
          H5HF_close(fh1_second);
        }
      });

      std::vector<uint8_t> read_back(man_val.size());
      EXPECT_GE(H5HF_read(fh1_second, man_id.data(), read_back.data()), 0);
      EXPECT_THAT(read_back, ElementsAreArray(man_val));

      // First H5HF_close decrements file_rc (2 -> 1), skipping teardown.
      EXPECT_GE(H5HF_close(fh1_second), 0);
      fh1_second = nullptr;

      // Calling H5HF_delete while file_rc > 0 marks pending_delete = true.
      ASSERT_GE(H5HF_delete(f, heap1_addr), 0);

      // Opening a heap marked pending_delete fails.
      {
        ScopedErrorSilence silence;
        EXPECT_EQ(H5HF_open(f, heap1_addr), nullptr);
      }

      // Closing the last open handle executes pending_delete cleanup.
      EXPECT_GE(H5HF_close(fh1), 0);
      fh1 = nullptr;
    }

    // 2. Create heap2, insert all 3 object classes, flush, and extract image.
    {
      const H5HF_create_t cparam = MakeHeapCreateParams(/*id_len=*/16);
      H5HF_t* fh2 = H5HF_create(f, &cparam);
      ASSERT_NE(fh2, nullptr);
      auto close_fh2 = absl::MakeCleanup([&]() {
        if (fh2 != nullptr) {
          H5HF_close(fh2);
        }
      });
      ASSERT_GE(H5HF_get_heap_addr(fh2, &heap2_addr), 0);

      size_t id_len = 0;
      ASSERT_GE(H5HF_get_id_len(fh2, &id_len), 0);
      h2_man_id.resize(id_len);
      h2_tiny_id.resize(id_len);
      h2_huge_id.resize(id_len);

      ASSERT_GE(
          H5HF_insert(fh2, man_val.size(), man_val.data(), h2_man_id.data()),
          0);
      ASSERT_GE(
          H5HF_insert(fh2, tiny_val.size(), tiny_val.data(), h2_tiny_id.data()),
          0);
      ASSERT_GE(
          H5HF_insert(fh2, huge_val.size(), huge_val.data(), h2_huge_id.data()),
          0);

      EXPECT_GE(H5HF_close(fh2), 0);
      fh2 = nullptr;
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // 3. Reopen the file image, open heap2, read objects, close, then delete
  //    immediately (file_rc == 0 path in H5HF_delete).
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_hf.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* reopened_fh = H5HF_open(f2, heap2_addr);
  ASSERT_NE(reopened_fh, nullptr);
  auto close_reopened_fh = absl::MakeCleanup([&]() {
    if (reopened_fh != nullptr) {
      H5HF_close(reopened_fh);
    }
  });

  std::vector<uint8_t> got_man(man_val.size());
  std::vector<uint8_t> got_tiny(tiny_val.size());
  std::vector<uint8_t> got_huge(huge_val.size());
  EXPECT_GE(H5HF_read(reopened_fh, h2_man_id.data(), got_man.data()), 0);
  EXPECT_GE(H5HF_read(reopened_fh, h2_tiny_id.data(), got_tiny.data()), 0);
  EXPECT_GE(H5HF_read(reopened_fh, h2_huge_id.data(), got_huge.data()), 0);
  EXPECT_THAT(got_man, ElementsAreArray(man_val));
  EXPECT_THAT(got_tiny, ElementsAreArray(tiny_val));
  EXPECT_THAT(got_huge, ElementsAreArray(huge_val));

  EXPECT_GE(H5HF_close(reopened_fh), 0);
  reopened_fh = nullptr;

  // Immediate H5HF_delete when file_rc == 0.
  EXPECT_GE(H5HF_delete(f2, heap2_addr), 0);

  // Invalid heap address fails H5HF__hdr_protect in both H5HF_open and
  // H5HF_delete.
  {
    ScopedErrorSilence silence;
    constexpr haddr_t kBadAddr = 0x1000000;
    EXPECT_EQ(H5HF_open(f2, kBadAddr), nullptr);
    EXPECT_LT(H5HF_delete(f2, kBadAddr), 0);
  }
}

TEST(H5HFTest, InsertErrorsInvalidVersionReservedAndBadIds) {
  const H5Z_class2_t toggle_filter = {
      .version = H5Z_CLASS_T_VERS,
      .id = kToggleFilterId,
      .encoder_present = 1,
      .decoder_present = 1,
      .name = "test_toggle_filter",
      .can_apply = ToggleFilterCanApply,
      .set_local = nullptr,
      .filter = ToggleFilterApply,
  };
  g_toggle_filter_allow = true;
  ASSERT_GE(H5Zregister(&toggle_filter), 0);
  auto unregister_filter = absl::MakeCleanup([&]() {
    g_toggle_filter_allow = true;
    H5Zunregister(kToggleFilterId);
  });

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  H5HF_create_t cparam =
      MakeHeapCreateParams(/*id_len=*/0, /*max_man_size=*/64);
  auto reset_pline = absl::MakeCleanup([&]() {
    if (cparam.pline.nused > 0) {
      H5O_msg_reset(H5O_PLINE_ID, &cparam.pline);
    }
  });
  ASSERT_GE(H5Z_append(&cparam.pline, kToggleFilterId, H5Z_FLAG_MANDATORY, 0,
                       nullptr),
            0);

  // 1. Failing filter can_apply triggers H5HF_create failure.
  {
    ScopedErrorSilence silence;
    g_toggle_filter_allow = false;
    EXPECT_EQ(H5HF_create(f, &cparam), nullptr);
    g_toggle_filter_allow = true;
  }

  H5HF_t* fh = H5HF_create(f, &cparam);
  ASSERT_NE(fh, nullptr);
  auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

  size_t id_len = 0;
  ASSERT_GE(H5HF_get_id_len(fh, &id_len), 0);
  std::vector<uint8_t> id(id_len, 0);
  std::vector<uint8_t> payload = MakePattern(32, 0x44U);
  std::vector<uint8_t> huge_payload = MakePattern(96, 0x88U);

  ScopedErrorSilence silence;

  // 2. Zero-size insert fails.
  EXPECT_LT(H5HF_insert(fh, 0, payload.data(), id.data()), 0);

  // 3. write_once mode rejects managed insert.
  fh->hdr->write_once = true;
  EXPECT_LT(H5HF_insert(fh, payload.size(), payload.data(), id.data()), 0);
  fh->hdr->write_once = false;

  // 4. Re-checking rejected filter fails H5HF__man_insert inside H5HF_insert.
  g_toggle_filter_allow = false;
  fh->hdr->checked_filters = false;
  EXPECT_LT(H5HF_insert(fh, payload.size(), payload.data(), id.data()), 0);
  g_toggle_filter_allow = true;
  fh->hdr->checked_filters = true;

  // 5. Wrapped huge IDs fail H5HF__huge_insert on unfiltered indirect heap
  //    (no pipeline buffer allocated, so failure path is leak-free).
  {
    const H5HF_create_t plain_param =
        MakeHeapCreateParams(/*id_len=*/0, /*max_man_size=*/64);
    H5HF_t* plain_fh = H5HF_create(f, &plain_param);
    ASSERT_NE(plain_fh, nullptr);
    auto close_plain = absl::MakeCleanup([&]() { H5HF_close(plain_fh); });

    size_t plain_id_len = 0;
    ASSERT_GE(H5HF_get_id_len(plain_fh, &plain_id_len), 0);
    std::vector<uint8_t> plain_id(plain_id_len, 0);

    plain_fh->hdr->huge_ids_wrapped = true;
    EXPECT_LT(H5HF_insert(plain_fh, huge_payload.size(), huge_payload.data(),
                          plain_id.data()),
              0);
    plain_fh->hdr->huge_ids_wrapped = false;
  }

  // 6. Version mismatch, reserved ID type, and invalid managed offset=0 across
  //    get_obj_len, get_obj_off, read, write, op, and remove.
  size_t out_len = 0;
  hsize_t out_off = 0;
  bool id_changed = false;
  auto noop_cb = [](const void*, size_t, void*) -> herr_t { return 0; };

  std::vector<uint8_t> bad_vers_id(id_len, 0);
  bad_vers_id[0] = 0x40U;
  EXPECT_LT(H5HF_get_obj_len(fh, bad_vers_id.data(), &out_len), 0);
  EXPECT_LT(H5HF_get_obj_off(fh, bad_vers_id.data(), &out_off), 0);
  EXPECT_LT(H5HF_read(fh, bad_vers_id.data(), payload.data()), 0);
  EXPECT_LT(H5HF_write(fh, bad_vers_id.data(), &id_changed, payload.data()), 0);
  EXPECT_LT(H5HF_op(fh, bad_vers_id.data(), noop_cb, nullptr), 0);
  EXPECT_LT(H5HF_remove(fh, bad_vers_id.data()), 0);

  std::vector<uint8_t> reserved_id(id_len, 0);
  reserved_id[0] = H5HF_ID_VERS_CURR | H5HF_ID_TYPE_RESERVED;
  EXPECT_LT(H5HF_get_obj_len(fh, reserved_id.data(), &out_len), 0);
  EXPECT_LT(H5HF_get_obj_off(fh, reserved_id.data(), &out_off), 0);
  EXPECT_LT(H5HF_read(fh, reserved_id.data(), payload.data()), 0);
  EXPECT_LT(H5HF_write(fh, reserved_id.data(), &id_changed, payload.data()), 0);
  EXPECT_LT(H5HF_op(fh, reserved_id.data(), noop_cb, nullptr), 0);
  EXPECT_LT(H5HF_remove(fh, reserved_id.data()), 0);

  std::vector<uint8_t> bad_man_id(id_len, 0);
  bad_man_id[0] = H5HF_ID_VERS_CURR | H5HF_ID_TYPE_MAN;
  EXPECT_LT(H5HF_read(fh, bad_man_id.data(), payload.data()), 0);
  EXPECT_LT(H5HF_write(fh, bad_man_id.data(), &id_changed, payload.data()), 0);
  EXPECT_LT(H5HF_op(fh, bad_man_id.data(), noop_cb, nullptr), 0);
  EXPECT_LT(H5HF_remove(fh, bad_man_id.data()), 0);

  // 7. Pending-delete close failure and immediate delete failure when the huge
  //    B-tree header cannot be decoded, followed by clean restoration/deletion.
  {
    const H5HF_create_t err_param =
        MakeHeapCreateParams(/*id_len=*/0, /*max_man_size=*/64);
    H5HF_t* err_fh = H5HF_create(f, &err_param);
    ASSERT_NE(err_fh, nullptr);

    haddr_t err_addr = HADDR_UNDEF;
    ASSERT_GE(H5HF_get_heap_addr(err_fh, &err_addr), 0);
    size_t err_id_len = 0;
    ASSERT_GE(H5HF_get_id_len(err_fh, &err_id_len), 0);
    std::vector<uint8_t> err_id(err_id_len, 0);
    ASSERT_GE(H5HF_insert(err_fh, huge_payload.size(), huge_payload.data(),
                          err_id.data()),
              0);

    // Ensure the heap header bytes exist in file storage before reading err+1.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    ASSERT_GE(H5HF_delete(f, err_addr), 0);
    const haddr_t orig_bt2_addr = err_fh->hdr->huge_bt2_addr;
    err_fh->hdr->huge_bt2_addr = err_addr + 1;

    EXPECT_LT(H5HF_close(err_fh), 0);
    EXPECT_LT(H5HF_delete(f, err_addr), 0);

    H5HF_hdr_t* hdr = H5HF__hdr_protect(f, err_addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(hdr, nullptr);
    hdr->huge_bt2_addr = orig_bt2_addr;
    ASSERT_GE(
        H5AC_unprotect(f, H5AC_FHEAP_HDR, err_addr, hdr, H5AC__NO_FLAGS_SET),
        0);
    EXPECT_GE(H5HF_delete(f, err_addr), 0);
  }
}

}  // namespace
}  // namespace hdf5
