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
// Tests extensible array testing class callbacks (context lifecycle, fill,
// encode, decode, debug, debug context) and creation parameter inspection/
// comparison helpers (third_party/hdf5/src/H5EAtest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5EAtest.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`). Match `NDEBUG` around the internal
// prelude and headers so internal struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5EATEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5EA_FRIEND
#define H5EA_TESTING
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5EApkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5EATEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EATEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

// RAII wrapper for pushing/popping an HDF5 API context and restoring tag state.
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

// RAII wrapper around an internal H5EA_t handle.
class ScopedEarray {
 public:
  ScopedEarray() = default;
  explicit ScopedEarray(H5EA_t* ea) : ea_(ea) {}
  ~ScopedEarray() { reset(); }

  ScopedEarray(const ScopedEarray&) = delete;
  ScopedEarray& operator=(const ScopedEarray&) = delete;

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

H5EA_create_t MakeTestCreateParams() {
  H5EA_create_t cparam{};
  cparam.cls = H5EA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_nelmts_bits = 16;
  cparam.idx_blk_elmts = 4;
  cparam.data_blk_min_elmts = 4;
  cparam.sup_blk_min_data_ptrs = 4;
  cparam.max_dblk_page_nelmts_bits = 6;  // 64 elements per page
  return cparam;
}

struct EncodeCbState {
  int calls = 0;
  size_t total_elmts = 0;
  uint64_t last_first_elmt = 0;
  bool should_fail = false;
};

herr_t TestEncodeCb(const void* elmt, size_t nelmts, void* udata) {
  auto* state = static_cast<EncodeCbState*>(udata);
  ++state->calls;
  state->total_elmts += nelmts;
  if (nelmts > 0 && elmt != nullptr) {
    std::memcpy(&state->last_first_elmt, elmt, sizeof(state->last_first_elmt));
  }
  return state->should_fail ? -1 : 0;
}

std::vector<uint8_t> SnapshotFileImage(hid_t file_id) {
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  const ssize_t copied =
      H5Fget_file_image(file_id, image.data(), image.size());
  if (copied != size) {
    return {};
  }
  return image;
}

TEST(H5EAtestTest, TestClassMetadataFields) {
  ASSERT_GE(H5open(), 0);
  const H5EA_class_t& cls = H5EA_CLS_TEST[0];
  EXPECT_EQ(H5EA_CLS_TEST_ID, cls.id);
  EXPECT_STREQ("Testing", cls.name);
  EXPECT_EQ(sizeof(uint64_t), cls.nat_elmt_size);
  EXPECT_NE(nullptr, cls.crt_context);
  EXPECT_NE(nullptr, cls.dst_context);
  EXPECT_NE(nullptr, cls.fill);
  EXPECT_NE(nullptr, cls.encode);
  EXPECT_NE(nullptr, cls.decode);
  EXPECT_NE(nullptr, cls.debug);
  EXPECT_NE(nullptr, cls.crt_dbg_ctx);
  EXPECT_NE(nullptr, cls.dst_dbg_ctx);
  EXPECT_EQ(UINT64_MAX, H5EA_TEST_FILL);
}

TEST(H5EAtestTest, DirectCallbacksContextEncodeDecodeFillAndError) {
  ASSERT_GE(H5open(), 0);
  const H5EA_class_t& cls = H5EA_CLS_TEST[0];

  // 1. Fill callback populates every slot with H5EA_TEST_FILL.
  std::array<uint64_t, 5> filled{1, 2, 3, 4, 5};
  ASSERT_GE(cls.fill(filled.data(), filled.size()), 0);
  EXPECT_THAT(filled, Each(H5EA_TEST_FILL));

  // 2. Encode and decode without a user callback struct (ctx->cb == nullptr).
  void* plain_ctx = cls.crt_context(nullptr);
  ASSERT_NE(nullptr, plain_ctx);
  absl::Cleanup free_plain_ctx = [&cls, plain_ctx] {
    EXPECT_GE(cls.dst_context(plain_ctx), 0);
  };

  constexpr std::array<uint64_t, 3> kPlainInputs{
      0x0102030405060708ULL,
      0xfedcba9876543210ULL,
      0ULL,
  };
  std::array<uint8_t, 3 * sizeof(uint64_t)> raw{};
  ASSERT_GE(cls.encode(raw.data(), kPlainInputs.data(), kPlainInputs.size(),
                       plain_ctx),
            0);
  // HDF5 UINT64ENCODE writes little-endian bytes.
  EXPECT_EQ(0x08, raw[0]);
  EXPECT_EQ(0x01, raw[7]);

  std::array<uint64_t, 3> decoded{};
  ASSERT_GE(
      cls.decode(raw.data(), decoded.data(), decoded.size(), plain_ctx), 0);
  EXPECT_THAT(decoded, ElementsAre(kPlainInputs[0], kPlainInputs[1],
                                   kPlainInputs[2]));

  // 3. Encode with an active H5EA__ctx_cb_t callback (success and failure).
  EncodeCbState state{};
  H5EA__ctx_cb_t cb{TestEncodeCb, &state};
  void* cb_ctx = cls.crt_context(&cb);
  ASSERT_NE(nullptr, cb_ctx);
  absl::Cleanup free_cb_ctx = [&cls, cb_ctx] {
    EXPECT_GE(cls.dst_context(cb_ctx), 0);
  };

  constexpr std::array<uint64_t, 2> kCbInputs{42ULL, 9999999999ULL};
  std::array<uint8_t, 2 * sizeof(uint64_t)> cb_raw{};
  ASSERT_GE(
      cls.encode(cb_raw.data(), kCbInputs.data(), kCbInputs.size(), cb_ctx), 0);
  EXPECT_EQ(1, state.calls);
  EXPECT_EQ(2U, state.total_elmts);
  EXPECT_EQ(42ULL, state.last_first_elmt);

  std::array<uint64_t, 2> cb_decoded{};
  ASSERT_GE(
      cls.decode(cb_raw.data(), cb_decoded.data(), cb_decoded.size(), cb_ctx),
      0);
  EXPECT_THAT(cb_decoded, ElementsAre(42ULL, 9999999999ULL));

  state.should_fail = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        cls.encode(cb_raw.data(), kCbInputs.data(), kCbInputs.size(), cb_ctx),
        0);
  }
  EXPECT_EQ(2, state.calls);
}

TEST(H5EAtestTest, DebugAndDebugContextCallbacks) {
  ScopedFile file = CreateInMemoryFile("h5eatest_dbg.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  const H5EA_class_t& cls = H5EA_CLS_TEST[0];
  void* dbg_ctx = cls.crt_dbg_ctx(f, 0x2000);
  ASSERT_NE(nullptr, dbg_ctx);
  EXPECT_GE(cls.dst_dbg_ctx(dbg_ctx), 0);

  char* buf = nullptr;
  size_t len = 0;
  absl::Cleanup free_buf = [&buf] { std::free(buf); };
  FILE* stream = open_memstream(&buf, &len);
  ASSERT_NE(nullptr, stream);
  absl::Cleanup close_stream = [&stream] {
    if (stream != nullptr) {
      std::fclose(stream);
    }
  };

  const uint64_t value = 9876543210ULL;
  EXPECT_GE(cls.debug(stream, /*indent=*/3, /*fwidth=*/16, /*idx=*/42, &value),
            0);
  EXPECT_EQ(0, std::fclose(stream));
  stream = nullptr;

  ASSERT_NE(nullptr, buf);
  const std::string output(buf, len);
  EXPECT_THAT(output, HasSubstr("Element #42:"));
  EXPECT_THAT(output, HasSubstr("9876543210"));
  EXPECT_EQ("   Element #42:     9876543210\n", output);
}

TEST(H5EAtestTest, GetAndCmpCreationParameters) {
  ScopedFile file = CreateInMemoryFile("h5eatest_cparam.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5EA_create_t expected = MakeTestCreateParams();
  ScopedEarray ea(H5EA_create(f, &expected, nullptr));
  ASSERT_NE(nullptr, ea.get());

  H5EA_create_t actual{};
  ASSERT_GE(H5EA__get_cparam_test(ea.get(), &actual), 0);
  EXPECT_EQ(expected.raw_elmt_size, actual.raw_elmt_size);
  EXPECT_EQ(expected.max_nelmts_bits, actual.max_nelmts_bits);
  EXPECT_EQ(expected.idx_blk_elmts, actual.idx_blk_elmts);
  EXPECT_EQ(expected.sup_blk_min_data_ptrs, actual.sup_blk_min_data_ptrs);
  EXPECT_EQ(expected.data_blk_min_elmts, actual.data_blk_min_elmts);
  EXPECT_EQ(expected.max_dblk_page_nelmts_bits,
            actual.max_dblk_page_nelmts_bits);
  EXPECT_EQ(0, H5EA__cmp_cparam_test(&expected, &actual));

  struct CmpCase {
    std::string_view label;
    H5EA_create_t lhs;
    H5EA_create_t rhs;
    int expected_cmp;
  };

  constexpr H5EA_create_t kRef{
      .cls = nullptr,
      .raw_elmt_size = 8,
      .max_nelmts_bits = 16,
      .idx_blk_elmts = 4,
      .data_blk_min_elmts = 4,
      .sup_blk_min_data_ptrs = 4,
      .max_dblk_page_nelmts_bits = 6,
  };

  constexpr auto kCases = std::to_array<CmpCase>({
      {
          .label = "equal",
          .lhs = kRef,
          .rhs = kRef,
          .expected_cmp = 0,
      },
      {
          .label = "raw_elmt_size_lt",
          .lhs = {.raw_elmt_size = 4,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "raw_elmt_size_gt",
          .lhs = {.raw_elmt_size = 16,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = 1,
      },
      {
          .label = "max_nelmts_bits_lt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 12,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "max_nelmts_bits_gt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 20,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = 1,
      },
      {
          .label = "idx_blk_elmts_lt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 2,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "idx_blk_elmts_gt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 8,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = 1,
      },
      {
          .label = "sup_blk_min_data_ptrs_lt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 2,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "sup_blk_min_data_ptrs_gt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 8,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = 1,
      },
      {
          .label = "data_blk_min_elmts_lt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 2,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "data_blk_min_elmts_gt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 8,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 6},
          .rhs = kRef,
          .expected_cmp = 1,
      },
      {
          .label = "max_dblk_page_nelmts_bits_lt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 5},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "max_dblk_page_nelmts_bits_gt",
          .lhs = {.raw_elmt_size = 8,
                  .max_nelmts_bits = 16,
                  .idx_blk_elmts = 4,
                  .data_blk_min_elmts = 4,
                  .sup_blk_min_data_ptrs = 4,
                  .max_dblk_page_nelmts_bits = 7},
          .rhs = kRef,
          .expected_cmp = 1,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(tc.expected_cmp, H5EA__cmp_cparam_test(&tc.lhs, &tc.rhs));
  }
}

TEST(H5EAtestTest, ExtensibleArrayRoundTripAndDeleteViaInMemoryImage) {
  struct Sample {
    hsize_t idx;
    uint64_t val;
  };
  // Indices span all four H5EA tiers under MakeTestCreateParams():
  // - 0, 3: index block
  // - 5: direct data block off index block
  // - 70: unpaged data block under super block
  // - 2100, 2165: two pages of a paged data block under super block
  constexpr auto kSamples = std::to_array<Sample>({
      {.idx = 0, .val = 1000},
      {.idx = 3, .val = 1003},
      {.idx = 5, .val = 1005},
      {.idx = 70, .val = 1070},
      {.idx = 2100, .val = 3100},
      {.idx = 2165, .val = 3165},
  });
  constexpr auto kMissingIndices =
      std::to_array<hsize_t>({1, 6, 71, 2101, 5000});

  const H5EA_create_t cparam = MakeTestCreateParams();
  EncodeCbState cb_state{};
  H5EA__ctx_cb_t cb{TestEncodeCb, &cb_state};
  haddr_t ea_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  // 1. Create array in Core VFD, write elements across all tiers, and flush.
  {
    ScopedFile file = CreateInMemoryFile("h5eatest_rt.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedEarray ea(H5EA_create(f, &cparam, &cb));
    ASSERT_NE(nullptr, ea.get());
    ASSERT_GE(H5EA_get_addr(ea.get(), &ea_addr), 0);
    ASSERT_NE(HADDR_UNDEF, ea_addr);

    for (const auto& s : kSamples) {
      ASSERT_GE(H5EA_set(ea.get(), s.idx, &s.val), 0);
    }

    for (const hsize_t idx : kMissingIndices) {
      uint64_t val = 0;
      ASSERT_GE(H5EA_get(ea.get(), idx, &val), 0);
      EXPECT_EQ(H5EA_TEST_FILL, val);
    }

    ea.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    EXPECT_GT(cb_state.calls, 0);
    EXPECT_GT(cb_state.total_elmts, 0U);

    image = SnapshotFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // 2. Reopen the serialized image in a fresh Core VFD file to trigger
  //    H5EA__test_crt_context, H5EA__test_decode, and H5EA_delete cleanup.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile file(H5Fopen("h5eatest_reopen.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    {
      ScopedEarray ea(H5EA_open(f, ea_addr, nullptr));
      ASSERT_NE(nullptr, ea.get());

      H5EA_create_t actual{};
      ASSERT_GE(H5EA__get_cparam_test(ea.get(), &actual), 0);
      EXPECT_EQ(0, H5EA__cmp_cparam_test(&cparam, &actual));

      hsize_t nelmts = 0;
      ASSERT_GE(H5EA_get_nelmts(ea.get(), &nelmts), 0);
      EXPECT_EQ(2166ULL, nelmts);

      for (const auto& s : kSamples) {
        uint64_t val = 0;
        ASSERT_GE(H5EA_get(ea.get(), s.idx, &val), 0);
        EXPECT_EQ(s.val, val);
      }

      for (const hsize_t idx : kMissingIndices) {
        uint64_t val = 0;
        ASSERT_GE(H5EA_get(ea.get(), idx, &val), 0);
        EXPECT_EQ(H5EA_TEST_FILL, val);
      }
    }

    ASSERT_GE(H5EA_delete(f, ea_addr, nullptr), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }
}

}  // namespace
}  // namespace hdf5
