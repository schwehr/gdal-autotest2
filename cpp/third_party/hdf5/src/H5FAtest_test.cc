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
// Tests fixed array testing class callbacks (context lifecycle, fill, encode,
// decode, debug, debug context) and creation parameter inspection/comparison
// helpers (third_party/hdf5/src/H5FAtest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FAtest.c

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
#define H5FATEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FA_FRIEND
#define H5FA_TESTING
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FApkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FATEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FATEST_TEST_RESTORE_UNDEF_NDEBUG
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

// RAII wrapper around an internal H5FA_t handle.
class ScopedFarray {
 public:
  ScopedFarray() = default;
  explicit ScopedFarray(H5FA_t* fa) : fa_(fa) {}
  ~ScopedFarray() { reset(); }

  ScopedFarray(const ScopedFarray&) = delete;
  ScopedFarray& operator=(const ScopedFarray&) = delete;

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

H5FA_create_t MakeTestCreateParams(hsize_t nelmts = 40) {
  H5FA_create_t cparam{};
  cparam.cls = H5FA_CLS_TEST;
  cparam.raw_elmt_size = sizeof(uint64_t);
  cparam.max_dblk_page_nelmts_bits = 4;  // 16 elements per page.
  cparam.nelmts = nelmts;
  return cparam;
}

std::vector<uint8_t> SnapshotFileImage(hid_t file_id) {
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  const ssize_t copied = H5Fget_file_image(file_id, image.data(), image.size());
  if (copied != size) {
    return {};
  }
  return image;
}

TEST(H5FAtestTest, TestClassMetadataFields) {
  ASSERT_GE(H5open(), 0);
  const H5FA_class_t& cls = H5FA_CLS_TEST[0];
  EXPECT_EQ(H5FA_CLS_TEST_ID, cls.id);
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
  EXPECT_EQ(UINT64_MAX, H5FA_TEST_FILL);
}

TEST(H5FAtestTest, DirectCallbacksContextFillEncodeAndDecode) {
  ASSERT_GE(H5open(), 0);
  const H5FA_class_t& cls = H5FA_CLS_TEST[0];

  // 1. Fill callback sets every slot to H5FA_TEST_FILL.
  std::array<uint64_t, 5> filled{1, 2, 3, 4, 5};
  ASSERT_GE(cls.fill(filled.data(), filled.size()), 0);
  EXPECT_THAT(filled, Each(H5FA_TEST_FILL));

  uint64_t single_filled = 77;
  ASSERT_GE(cls.fill(&single_filled, 1), 0);
  EXPECT_EQ(H5FA_TEST_FILL, single_filled);

  // 2. Context creation/destruction accepts both null and non-null udata.
  int dummy_udata = 123;
  void* udata_ctx = cls.crt_context(&dummy_udata);
  ASSERT_NE(nullptr, udata_ctx);
  EXPECT_GE(cls.dst_context(udata_ctx), 0);

  void* ctx = cls.crt_context(nullptr);
  ASSERT_NE(nullptr, ctx);
  absl::Cleanup free_ctx = [&cls, ctx] { EXPECT_GE(cls.dst_context(ctx), 0); };

  // 3. Encode and decode multiple elements in little-endian wire order.
  constexpr std::array<uint64_t, 4> kInputs{
      0x0102030405060708ULL,
      0xfedcba9876543210ULL,
      0ULL,
      UINT64_MAX,
  };
  std::array<uint8_t, 4 * sizeof(uint64_t)> raw{};
  ASSERT_GE(cls.encode(raw.data(), kInputs.data(), kInputs.size(), ctx), 0);
  EXPECT_EQ(0x08, raw[0]);
  EXPECT_EQ(0x01, raw[7]);

  std::array<uint64_t, 4> decoded{};
  ASSERT_GE(cls.decode(raw.data(), decoded.data(), decoded.size(), ctx), 0);
  EXPECT_THAT(decoded,
              ElementsAre(kInputs[0], kInputs[1], kInputs[2], kInputs[3]));

  // 4. Single-element encode/decode boundary check.
  constexpr uint64_t kSingle = 0x1122334455667788ULL;
  std::array<uint8_t, sizeof(uint64_t)> single_raw{};
  uint64_t single_out = 0;
  ASSERT_GE(cls.encode(single_raw.data(), &kSingle, 1, ctx), 0);
  ASSERT_GE(cls.decode(single_raw.data(), &single_out, 1, ctx), 0);
  EXPECT_EQ(kSingle, single_out);
}

TEST(H5FAtestTest, DebugAndDebugContextCallbacks) {
  ScopedFile file = CreateInMemoryFile("h5fatest_dbg.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  const H5FA_class_t& cls = H5FA_CLS_TEST[0];
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

TEST(H5FAtestTest, GetAndCmpCreationParameters) {
  ScopedFile file = CreateInMemoryFile("h5fatest_cparam.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(nullptr, f);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  const H5FA_create_t expected = MakeTestCreateParams(/*nelmts=*/24);
  ScopedFarray fa(H5FA_create(f, &expected, nullptr));
  ASSERT_NE(nullptr, fa.get());

  // Pre-fill output with sentinel values before H5FA__get_cparam_test().
  H5FA_create_t actual{};
  actual.raw_elmt_size = 0xff;
  actual.nelmts = 9999;
  ASSERT_GE(H5FA__get_cparam_test(fa.get(), &actual), 0);
  EXPECT_EQ(expected.raw_elmt_size, actual.raw_elmt_size);
  EXPECT_EQ(expected.nelmts, actual.nelmts);
  EXPECT_EQ(0, H5FA__cmp_cparam_test(&expected, &actual));

  struct CmpCase {
    std::string_view label;
    H5FA_create_t lhs;
    H5FA_create_t rhs;
    int expected_cmp;
  };

  constexpr H5FA_create_t kRef{
      .cls = nullptr,
      .raw_elmt_size = 8,
      .max_dblk_page_nelmts_bits = 4,
      .nelmts = 24,
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
                  .max_dblk_page_nelmts_bits = 4,
                  .nelmts = 24},
          .rhs = kRef,
          .expected_cmp = -1,
      },
      {
          .label = "raw_elmt_size_gt",
          .lhs = {.raw_elmt_size = 16,
                  .max_dblk_page_nelmts_bits = 4,
                  .nelmts = 24},
          .rhs = kRef,
          .expected_cmp = 1,
      },
      {
          .label = "different_nelmts_ignored_when_raw_elmt_size_matches",
          .lhs = {.raw_elmt_size = 8,
                  .max_dblk_page_nelmts_bits = 6,
                  .nelmts = 100},
          .rhs = kRef,
          .expected_cmp = 0,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(tc.expected_cmp, H5FA__cmp_cparam_test(&tc.lhs, &tc.rhs));
  }
}

TEST(H5FAtestTest, FixedArrayRoundTripAndDeleteViaInMemoryImage) {
  struct Sample {
    hsize_t idx;
    uint64_t val;
  };
  // Indices span all 3 pages of a 40-element paged array (16 elements/page):
  // page 0 (0..15), page 1 (16..31), and partial page 2 (32..39).
  constexpr auto kSamples = std::to_array<Sample>({
      {.idx = 0, .val = 1000},
      {.idx = 15, .val = 1015},
      {.idx = 18, .val = 1018},
      {.idx = 33, .val = 1033},
      {.idx = 39, .val = 1039},
  });
  constexpr auto kMissingIndices = std::to_array<hsize_t>({1, 16, 31, 32, 38});

  const H5FA_create_t paged_cparam = MakeTestCreateParams(/*nelmts=*/40);
  const H5FA_create_t unpaged_cparam = MakeTestCreateParams(/*nelmts=*/6);
  haddr_t paged_addr = HADDR_UNDEF;
  haddr_t unpaged_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  // 1. Create both paged and unpaged fixed arrays in Core VFD and flush.
  {
    ScopedFile file = CreateInMemoryFile("h5fatest_rt.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedFarray paged_fa(H5FA_create(f, &paged_cparam, nullptr));
    ASSERT_NE(nullptr, paged_fa.get());
    ASSERT_GE(H5FA_get_addr(paged_fa.get(), &paged_addr), 0);
    ASSERT_NE(HADDR_UNDEF, paged_addr);

    for (const auto& s : kSamples) {
      ASSERT_GE(H5FA_set(paged_fa.get(), s.idx, &s.val), 0);
    }
    for (const hsize_t idx : kMissingIndices) {
      uint64_t val = 0;
      ASSERT_GE(H5FA_get(paged_fa.get(), idx, &val), 0);
      EXPECT_EQ(H5FA_TEST_FILL, val);
    }

    ScopedFarray unpaged_fa(H5FA_create(f, &unpaged_cparam, nullptr));
    ASSERT_NE(nullptr, unpaged_fa.get());
    ASSERT_GE(H5FA_get_addr(unpaged_fa.get(), &unpaged_addr), 0);
    ASSERT_NE(HADDR_UNDEF, unpaged_addr);

    constexpr uint64_t kUnpagedVal = 7777;
    ASSERT_GE(H5FA_set(unpaged_fa.get(), 2, &kUnpagedVal), 0);

    paged_fa.reset();
    unpaged_fa.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    image = SnapshotFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // 2. Reopen the serialized image in Core VFD to exercise H5FA__test_decode,
  // H5FA__test_crt_context, H5FA__test_dst_context, and H5FA_delete.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
    ScopedFile file(H5Fopen("h5fatest_reopen.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(nullptr, f);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    {
      ScopedFarray paged_fa(H5FA_open(f, paged_addr, nullptr));
      ASSERT_NE(nullptr, paged_fa.get());

      H5FA_create_t actual{};
      ASSERT_GE(H5FA__get_cparam_test(paged_fa.get(), &actual), 0);
      EXPECT_EQ(0, H5FA__cmp_cparam_test(&paged_cparam, &actual));
      EXPECT_EQ(40ULL, actual.nelmts);

      hsize_t nelmts = 0;
      ASSERT_GE(H5FA_get_nelmts(paged_fa.get(), &nelmts), 0);
      EXPECT_EQ(40ULL, nelmts);

      for (const auto& s : kSamples) {
        uint64_t val = 0;
        ASSERT_GE(H5FA_get(paged_fa.get(), s.idx, &val), 0);
        EXPECT_EQ(s.val, val);
      }
      for (const hsize_t idx : kMissingIndices) {
        uint64_t val = 0;
        ASSERT_GE(H5FA_get(paged_fa.get(), idx, &val), 0);
        EXPECT_EQ(H5FA_TEST_FILL, val);
      }

      struct IterState {
        size_t count = 0;
        uint64_t last_val = 0;
      } iter_state;
      auto iter_cb = [](hsize_t /*idx*/, const void* elmt, void* udata) -> int {
        auto* st = static_cast<IterState*>(udata);
        ++st->count;
        std::memcpy(&st->last_val, elmt, sizeof(st->last_val));
        return H5_ITER_CONT;
      };
      EXPECT_GE(H5FA_iterate(paged_fa.get(), iter_cb, &iter_state), 0);
      EXPECT_EQ(40u, iter_state.count);
      EXPECT_EQ(1039ULL, iter_state.last_val);

      ScopedFarray unpaged_fa(H5FA_open(f, unpaged_addr, nullptr));
      ASSERT_NE(nullptr, unpaged_fa.get());
      uint64_t unpaged_val = 0;
      ASSERT_GE(H5FA_get(unpaged_fa.get(), 2, &unpaged_val), 0);
      EXPECT_EQ(7777ULL, unpaged_val);
      ASSERT_GE(H5FA_get(unpaged_fa.get(), 0, &unpaged_val), 0);
      EXPECT_EQ(H5FA_TEST_FILL, unpaged_val);
    }

    ASSERT_GE(H5FA_delete(f, paged_addr, nullptr), 0);
    ASSERT_GE(H5FA_delete(f, unpaged_addr, nullptr), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }
}

}  // namespace
}  // namespace hdf5
