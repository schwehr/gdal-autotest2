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
// Tests vector, array, hyperslab, chunk indexing, bit manipulation, and
// scattered sequence (opvv/memcpyvv) utilities (third_party/hdf5/src/H5VM.c and
// third_party/hdf5/src/H5VMprivate.h).
//
// Note: third_party/hdf5:hdf5 is compiled with -DNDEBUG and without
// NO_INLINED_CODE, so H5VM_hyper_copy uses the inlined stride switch rather
// than calling H5VM_hyper_stride.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VM.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VMprivate.h

// clang-format off
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5VMprivate.h"
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

// H5VM_HYPER_NDIMS in H5VM.c equals H5O_LAYOUT_NDIMS == H5S_MAX_RANK + 1.
constexpr unsigned kHyperMaxDims = H5S_MAX_RANK + 1;
constexpr uint8_t kSentinel = 0x5A;

struct OpvvSegment {
  hsize_t dst_off = 0;
  hsize_t src_off = 0;
  size_t len = 0;

  bool operator==(const OpvvSegment& other) const = default;
};

struct OpvvRecorder {
  std::vector<OpvvSegment> segments;
  size_t fail_on_call = std::numeric_limits<size_t>::max();
  size_t call_count = 0;
};

herr_t RecordOpvvSegment(hsize_t dst_off, hsize_t src_off, size_t len,
                         void* udata) {
  auto* rec = static_cast<OpvvRecorder*>(udata);
  if (rec->call_count++ == rec->fail_on_call) {
    return -1;
  }
  rec->segments.push_back({.dst_off = dst_off, .src_off = src_off, .len = len});
  return 0;
}

TEST(H5VmTest, SignedVectorComparisonsAndZeroPredicate) {
  constexpr auto a = std::to_array<hssize_t>({-5, 0, 10});
  constexpr auto b = std::to_array<hssize_t>({-5, 0, 10});
  constexpr auto c = std::to_array<hssize_t>({-5, -1, 99});
  constexpr auto d = std::to_array<hssize_t>({-5, 2, -10});
  constexpr auto zeros = std::to_array<hssize_t>({0, 0, 0});

  EXPECT_EQ(0, H5VM_vector_cmp_s(3, a.data(), a.data()));
  EXPECT_EQ(0, H5VM_vector_cmp_s(3, a.data(), b.data()));
  EXPECT_EQ(1, H5VM_vector_cmp_s(3, a.data(), c.data()));
  EXPECT_EQ(-1, H5VM_vector_cmp_s(3, a.data(), d.data()));
  EXPECT_EQ(-1, H5VM_vector_cmp_s(3, nullptr, a.data()));
  EXPECT_EQ(1, H5VM_vector_cmp_s(3, a.data(), nullptr));
  EXPECT_EQ(0, H5VM_vector_cmp_s(0, a.data(), c.data()));

  EXPECT_TRUE(H5VM_vector_eq_s(3, a.data(), b.data()));
  EXPECT_FALSE(H5VM_vector_eq_s(3, a.data(), c.data()));
  EXPECT_TRUE(H5VM_vector_lt_s(3, c.data(), a.data()));
  EXPECT_TRUE(H5VM_vector_gt_s(3, d.data(), a.data()));
  EXPECT_TRUE(H5VM_vector_le_s(3, a.data(), b.data()));
  EXPECT_TRUE(H5VM_vector_le_s(3, c.data(), a.data()));
  EXPECT_TRUE(H5VM_vector_ge_s(3, a.data(), b.data()));
  EXPECT_TRUE(H5VM_vector_ge_s(3, d.data(), a.data()));

  EXPECT_GT(H5VM_vector_zerop_s(3, zeros.data()), 0);
  EXPECT_GT(H5VM_vector_zerop_s(3, nullptr), 0);
  EXPECT_GT(H5VM_vector_zerop_s(0, a.data()), 0);
  EXPECT_EQ(0, H5VM_vector_zerop_s(3, a.data()));
}

TEST(H5VmTest, UnsignedVectorComparisonsProductIncAndCopy) {
  constexpr auto a = std::to_array<hsize_t>({2, 3, 4});
  constexpr auto b = std::to_array<hsize_t>({2, 3, 4});
  constexpr auto smaller = std::to_array<hsize_t>({2, 2, 9});
  constexpr auto larger = std::to_array<hsize_t>({2, 4, 0});
  constexpr auto zeros = std::to_array<hsize_t>({0, 0, 0});

  EXPECT_EQ(0, H5VM_vector_cmp_u(3, a.data(), a.data()));
  EXPECT_EQ(0, H5VM_vector_cmp_u(3, a.data(), b.data()));
  EXPECT_EQ(1, H5VM_vector_cmp_u(3, a.data(), smaller.data()));
  EXPECT_EQ(-1, H5VM_vector_cmp_u(3, a.data(), larger.data()));
  EXPECT_EQ(-1, H5VM_vector_cmp_u(3, nullptr, a.data()));
  EXPECT_EQ(1, H5VM_vector_cmp_u(3, a.data(), nullptr));
  EXPECT_EQ(0, H5VM_vector_cmp_u(0, a.data(), smaller.data()));

  EXPECT_TRUE(H5VM_vector_eq_u(3, a.data(), b.data()));
  EXPECT_TRUE(H5VM_vector_lt_u(3, smaller.data(), a.data()));
  EXPECT_TRUE(H5VM_vector_gt_u(3, larger.data(), a.data()));
  EXPECT_TRUE(H5VM_vector_le_u(3, a.data(), b.data()));
  EXPECT_TRUE(H5VM_vector_ge_u(3, a.data(), b.data()));

  EXPECT_GT(H5VM_vector_zerop_u(3, zeros.data()), 0);
  EXPECT_GT(H5VM_vector_zerop_u(3, nullptr), 0);
  EXPECT_GT(H5VM_vector_zerop_u(0, a.data()), 0);
  EXPECT_EQ(0, H5VM_vector_zerop_u(3, a.data()));

  EXPECT_EQ(1u, H5VM_vector_reduce_product(0, nullptr));
  EXPECT_EQ(0u, H5VM_vector_reduce_product(3, nullptr));
  EXPECT_EQ(24u, H5VM_vector_reduce_product(3, a.data()));
  EXPECT_EQ(0u, H5VM_vector_reduce_product(3, larger.data()));

  std::array<hsize_t, 3> dst{};
  H5VM_vector_cpy(3, dst.data(), a.data());
  EXPECT_THAT(dst, ElementsAre(2, 3, 4));
  H5VM_vector_inc(3, dst.data(), smaller.data());
  EXPECT_THAT(dst, ElementsAre(4, 5, 13));
  H5VM_vector_cpy(3, dst.data(), static_cast<const hsize_t*>(nullptr));
  EXPECT_THAT(dst, ElementsAre(0, 0, 0));

  dst = a;
  H5VM_vector_zero(3, dst.data());
  EXPECT_THAT(dst, ElementsAre(0, 0, 0));
}

TEST(H5VmTest, SwizzleAndUnswizzleCoordsRoundTrip) {
  std::array<hsize_t, 4> coords = {10, 20, 30, 40};
  // unlim_dim == 0 leaves coordinates unchanged.
  H5VM_swizzle_coords(hsize_t, coords.data(), 0);
  EXPECT_THAT(coords, ElementsAre(10, 20, 30, 40));
  H5VM_unswizzle_coords(hsize_t, coords.data(), 0);
  EXPECT_THAT(coords, ElementsAre(10, 20, 30, 40));

  H5VM_swizzle_coords(hsize_t, coords.data(), 2);
  EXPECT_THAT(coords, ElementsAre(30, 10, 20, 40));
  H5VM_unswizzle_coords(hsize_t, coords.data(), 2);
  EXPECT_THAT(coords, ElementsAre(10, 20, 30, 40));
}

TEST(H5VmTest, Log2Power2UpAndLimitEncSize) {
  struct Log2Case {
    uint64_t input;
    unsigned expected_log2;
    unsigned expected_enc_bytes;
  };
  constexpr auto kLog2Cases = std::to_array<Log2Case>({
      {.input = 0, .expected_log2 = 0, .expected_enc_bytes = 1},
      {.input = 1, .expected_log2 = 0, .expected_enc_bytes = 1},
      {.input = 2, .expected_log2 = 1, .expected_enc_bytes = 1},
      {.input = 255, .expected_log2 = 7, .expected_enc_bytes = 1},
      {.input = 256, .expected_log2 = 8, .expected_enc_bytes = 2},
      {.input = 0xFFFFULL, .expected_log2 = 15, .expected_enc_bytes = 2},
      {.input = 1ULL << 16, .expected_log2 = 16, .expected_enc_bytes = 3},
      {.input = 1ULL << 24, .expected_log2 = 24, .expected_enc_bytes = 4},
      {.input = 1ULL << 32, .expected_log2 = 32, .expected_enc_bytes = 5},
      {.input = 1ULL << 40, .expected_log2 = 40, .expected_enc_bytes = 6},
      {.input = 1ULL << 48, .expected_log2 = 48, .expected_enc_bytes = 7},
      {.input = 1ULL << 56, .expected_log2 = 56, .expected_enc_bytes = 8},
      {.input = std::numeric_limits<uint64_t>::max(),
       .expected_log2 = 63,
       .expected_enc_bytes = 8},
  });
  for (const auto& tc : kLog2Cases) {
    EXPECT_EQ(tc.expected_log2, H5VM_log2_gen(tc.input)) << tc.input;
    EXPECT_EQ(tc.expected_enc_bytes, H5VM_limit_enc_size(tc.input)) << tc.input;
  }

  for (unsigned bit = 0; bit < 32; ++bit) {
    EXPECT_EQ(bit, H5VM_log2_of2(1u << bit)) << bit;
  }

  struct Power2UpCase {
    hsize_t input;
    hsize_t expected;
  };
  constexpr auto kPower2UpCases = std::to_array<Power2UpCase>({
      {.input = 0, .expected = 1},
      {.input = 1, .expected = 1},
      {.input = 2, .expected = 2},
      {.input = 3, .expected = 4},
      {.input = 5, .expected = 8},
      {.input = 1ULL << 62, .expected = 1ULL << 62},
      {.input = (1ULL << 62) + 1, .expected = 1ULL << 63},
      {.input = 1ULL << 63, .expected = 0},
      {.input = std::numeric_limits<hsize_t>::max(), .expected = 0},
  });
  for (const auto& tc : kPower2UpCases) {
    EXPECT_EQ(tc.expected, H5VM_power2up(tc.input)) << tc.input;
  }
}

TEST(H5VmTest, BitGetAndSetMsbFirstOrdering) {
  std::array<unsigned char, 3> buf = {0x00, 0x00, 0x00};

  // Bits 0..7 map MSB->LSB within byte 0; bit 8 maps to MSB of byte 1.
  H5VM_bit_set(buf.data(), 0, true);
  H5VM_bit_set(buf.data(), 7, true);
  H5VM_bit_set(buf.data(), 8, true);
  H5VM_bit_set(buf.data(), 19, true);
  EXPECT_THAT(buf, ElementsAre(0x81, 0x80, 0x10));

  EXPECT_TRUE(H5VM_bit_get(buf.data(), 0));
  EXPECT_FALSE(H5VM_bit_get(buf.data(), 1));
  EXPECT_TRUE(H5VM_bit_get(buf.data(), 7));
  EXPECT_TRUE(H5VM_bit_get(buf.data(), 8));
  EXPECT_TRUE(H5VM_bit_get(buf.data(), 19));

  H5VM_bit_set(buf.data(), 0, false);
  H5VM_bit_set(buf.data(), 19, false);
  EXPECT_THAT(buf, ElementsAre(0x01, 0x80, 0x00));
  EXPECT_FALSE(H5VM_bit_get(buf.data(), 0));
  EXPECT_FALSE(H5VM_bit_get(buf.data(), 19));
}

TEST(H5VmTest, HyperStrideHandlesAllRankCasesAndNullOffset) {
  // 1-D (default branch with loop count 0)
  {
    constexpr auto size = std::to_array<hsize_t>({4});
    constexpr auto total = std::to_array<hsize_t>({10});
    constexpr auto offset = std::to_array<hsize_t>({3});
    std::array<hsize_t, 1> stride{};
    EXPECT_EQ(3u, H5VM_hyper_stride(1, size.data(), total.data(), offset.data(),
                                    stride.data()));
    EXPECT_THAT(stride, ElementsAre(1));
    EXPECT_EQ(0u, H5VM_hyper_stride(1, size.data(), total.data(), H5VM_ZERO,
                                    stride.data()));
    EXPECT_THAT(stride, ElementsAre(1));
  }

  // 2-D (case 2)
  {
    constexpr auto size = std::to_array<hsize_t>({2, 3});
    constexpr auto total = std::to_array<hsize_t>({4, 5});
    constexpr auto offset = std::to_array<hsize_t>({1, 2});
    std::array<hsize_t, 2> stride{};
    EXPECT_EQ(1 * 5 + 2, H5VM_hyper_stride(2, size.data(), total.data(),
                                           offset.data(), stride.data()));
    EXPECT_THAT(stride, ElementsAre(2, 1));
    EXPECT_EQ(0u, H5VM_hyper_stride(2, size.data(), total.data(), H5VM_ZERO,
                                    stride.data()));
    EXPECT_THAT(stride, ElementsAre(2, 1));
  }

  // 3-D (case 3)
  {
    constexpr auto size = std::to_array<hsize_t>({2, 2, 3});
    constexpr auto total = std::to_array<hsize_t>({3, 4, 5});
    constexpr auto offset = std::to_array<hsize_t>({1, 2, 1});
    std::array<hsize_t, 3> stride{};
    EXPECT_EQ(1 * 20 + 2 * 5 + 1,
              H5VM_hyper_stride(3, size.data(), total.data(), offset.data(),
                                stride.data()));
    EXPECT_THAT(stride, ElementsAre(5 * (4 - 2), 5 - 3, 1));
    EXPECT_EQ(0u, H5VM_hyper_stride(3, size.data(), total.data(), H5VM_ZERO,
                                    stride.data()));
  }

  // 4-D (case 4)
  {
    constexpr auto size = std::to_array<hsize_t>({1, 2, 2, 3});
    constexpr auto total = std::to_array<hsize_t>({2, 3, 4, 5});
    constexpr auto offset = std::to_array<hsize_t>({1, 1, 2, 1});
    std::array<hsize_t, 4> stride{};
    EXPECT_EQ(1 * 60 + 1 * 20 + 2 * 5 + 1,
              H5VM_hyper_stride(4, size.data(), total.data(), offset.data(),
                                stride.data()));
    EXPECT_THAT(stride, ElementsAre(20 * (3 - 2), 5 * (4 - 2), 5 - 3, 1));
    EXPECT_EQ(0u, H5VM_hyper_stride(4, size.data(), total.data(), H5VM_ZERO,
                                    stride.data()));
  }

  // 5-D (default branch with loop iterations)
  {
    constexpr auto size = std::to_array<hsize_t>({1, 2, 2, 2, 3});
    constexpr auto total = std::to_array<hsize_t>({2, 3, 3, 4, 5});
    constexpr auto offset = std::to_array<hsize_t>({1, 1, 1, 2, 1});
    std::array<hsize_t, 5> stride{};
    EXPECT_EQ(1 * 180 + 1 * 60 + 1 * 20 + 2 * 5 + 1,
              H5VM_hyper_stride(5, size.data(), total.data(), offset.data(),
                                stride.data()));
    EXPECT_THAT(stride,
                ElementsAre(60 * (3 - 2), 20 * (3 - 2), 5 * (4 - 2), 5 - 3, 1));
    EXPECT_EQ(0u, H5VM_hyper_stride(5, size.data(), total.data(), H5VM_ZERO,
                                    stride.data()));
  }
}

TEST(H5VmTest, HyperEqComparesOffsetsSizesAndZeroVolumes) {
  constexpr auto off1 = std::to_array<hsize_t>({1, 2, 3});
  constexpr auto off_zero = std::to_array<hsize_t>({0, 0, 0});
  constexpr auto off_diff = std::to_array<hsize_t>({1, 9, 3});
  constexpr auto sz1 = std::to_array<hsize_t>({4, 5, 6});
  constexpr auto sz_diff = std::to_array<hsize_t>({4, 5, 7});
  constexpr auto sz_with_zero = std::to_array<hsize_t>({4, 0, 6});

  EXPECT_GT(H5VM_hyper_eq(0, nullptr, nullptr, nullptr, nullptr), 0);
  EXPECT_GT(H5VM_hyper_eq(3, off1.data(), sz1.data(), off1.data(), sz1.data()),
            0);
  EXPECT_GT(H5VM_hyper_eq(3, nullptr, sz1.data(), off_zero.data(), sz1.data()),
            0);
  EXPECT_GT(H5VM_hyper_eq(3, off_zero.data(), sz1.data(), nullptr, sz1.data()),
            0);

  EXPECT_EQ(0, H5VM_hyper_eq(3, nullptr, sz1.data(), off1.data(), sz1.data()));
  EXPECT_EQ(0, H5VM_hyper_eq(3, off1.data(), sz1.data(), nullptr, sz1.data()));
  EXPECT_EQ(0, H5VM_hyper_eq(3, off1.data(), sz1.data(), off_diff.data(),
                             sz1.data()));
  EXPECT_EQ(0, H5VM_hyper_eq(3, off1.data(), sz1.data(), off1.data(),
                             sz_diff.data()));
  EXPECT_EQ(0, H5VM_hyper_eq(3, off1.data(), nullptr, off1.data(), sz1.data()));
  EXPECT_EQ(0, H5VM_hyper_eq(3, off1.data(), sz1.data(), off1.data(), nullptr));
  EXPECT_EQ(0, H5VM_hyper_eq(3, off1.data(), sz_with_zero.data(), off1.data(),
                             sz_with_zero.data()));
  EXPECT_EQ(0, H5VM_hyper_eq(3, nullptr, nullptr, off_zero.data(), nullptr));
}

TEST(H5VmTest, HyperFillContiguousAndStridedHyperslabs) {
  // 1-D partial fill.
  {
    std::array<uint8_t, 8> buf{};
    buf.fill(kSentinel);
    constexpr auto size = std::to_array<hsize_t>({4});
    constexpr auto total = std::to_array<hsize_t>({8});
    constexpr auto offset = std::to_array<hsize_t>({2});
    ASSERT_GE(H5VM_hyper_fill(1, size.data(), total.data(), offset.data(),
                              buf.data(), 0xA5),
              0);
    EXPECT_THAT(buf, ElementsAre(kSentinel, kSentinel, 0xA5, 0xA5, 0xA5, 0xA5,
                                 kSentinel, kSentinel));
  }

  // 3-D fully contiguous fill reduces dimensionality to 0 in
  // H5VM__stride_optimize1.
  {
    std::array<uint8_t, 24> buf{};
    buf.fill(kSentinel);
    constexpr auto dims = std::to_array<hsize_t>({2, 3, 4});
    ASSERT_GE(H5VM_hyper_fill(3, dims.data(), dims.data(), H5VM_ZERO,
                              buf.data(), 0x3C),
              0);
    EXPECT_TRUE(std::all_of(buf.begin(), buf.end(),
                            [](uint8_t b) { return b == 0x3C; }));
  }

  // 3-D non-contiguous interior slab (leaving trailing elements untouched so
  // stride advance never leaves the backing vector).
  {
    constexpr auto total = std::to_array<hsize_t>({3, 4, 5});
    constexpr auto size = std::to_array<hsize_t>({2, 2, 3});
    constexpr auto offset = std::to_array<hsize_t>({0, 1, 1});
    std::vector<uint8_t> buf(3 * 4 * 5, kSentinel);
    ASSERT_GE(H5VM_hyper_fill(3, size.data(), total.data(), offset.data(),
                              buf.data(), 0x7E),
              0);

    for (size_t z = 0; z < 3; ++z) {
      for (size_t y = 0; y < 4; ++y) {
        for (size_t x = 0; x < 5; ++x) {
          const bool in_slab =
              (z < 2) && (y >= 1 && y < 3) && (x >= 1 && x < 4);
          EXPECT_EQ(in_slab ? 0x7E : kSentinel, buf[z * 20 + y * 5 + x]);
        }
      }
    }
  }

  // Max rank (33 dimensions).
  {
    std::array<hsize_t, kHyperMaxDims> size{};
    std::array<hsize_t, kHyperMaxDims> total{};
    size.fill(1);
    total.fill(1);
    size[kHyperMaxDims - 2] = 2;
    size[kHyperMaxDims - 1] = 3;
    total[kHyperMaxDims - 2] = 2;
    total[kHyperMaxDims - 1] = 3;
    std::array<uint8_t, 6> buf{};
    buf.fill(kSentinel);
    ASSERT_GE(H5VM_hyper_fill(kHyperMaxDims, size.data(), total.data(),
                              H5VM_ZERO, buf.data(), 0xEE),
              0);
    EXPECT_THAT(buf, ElementsAre(0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE));
  }
}

// Helper to test H5VM_hyper_copy across contiguity patterns and null/non-null
// offsets.
void VerifyHyperCopy(std::span<const hsize_t> size,
                     std::span<const hsize_t> dst_total,
                     const hsize_t* dst_offset,
                     std::span<const hsize_t> src_total,
                     const hsize_t* src_offset) {
  const unsigned ndims = static_cast<unsigned>(size.size());
  const hsize_t dst_bytes = H5VM_vector_reduce_product(ndims, dst_total.data());
  const hsize_t src_bytes = H5VM_vector_reduce_product(ndims, src_total.data());

  // Add 1 trailing guard byte so stride advances after the final copied
  // element stay strictly within the allocation under UBSan pointer-overflow.
  std::vector<uint8_t> src(src_bytes + 1);
  for (size_t i = 0; i < src_bytes; ++i) {
    src[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  }
  src[src_bytes] = kSentinel;

  std::vector<uint8_t> dst(dst_bytes + 1, kSentinel);

  ASSERT_GE(
      H5VM_hyper_copy(ndims, size.data(), dst_total.data(), dst_offset,
                      dst.data(), src_total.data(), src_offset, src.data()),
      0);

  // Build expected destination independently via coordinates.
  std::vector<uint8_t> expected(dst_bytes + 1, kSentinel);
  const hsize_t nelmts = H5VM_vector_reduce_product(ndims, size.data());
  std::array<hsize_t, kHyperMaxDims> coords{};
  std::array<hsize_t, kHyperMaxDims> dst_coords{};
  std::array<hsize_t, kHyperMaxDims> src_coords{};
  for (hsize_t idx = 0; idx < nelmts; ++idx) {
    ASSERT_GE(H5VM_array_calc(idx, ndims, size.data(), coords.data()), 0);
    for (unsigned d = 0; d < ndims; ++d) {
      dst_coords[d] = coords[d] + (dst_offset ? dst_offset[d] : 0);
      src_coords[d] = coords[d] + (src_offset ? src_offset[d] : 0);
    }
    const hsize_t d_off =
        H5VM_array_offset(ndims, dst_total.data(), dst_coords.data());
    const hsize_t s_off =
        H5VM_array_offset(ndims, src_total.data(), src_coords.data());
    expected[d_off] = src[s_off];
  }

  EXPECT_EQ(expected, dst);
}

TEST(H5VmTest, HyperCopyCoversAllRankSwitchAndStrideOptimizationBranches) {
  // 1-D (default in H5VM_hyper_copy, case 1 in H5VM__stride_optimize2).
  {
    constexpr auto size = std::to_array<hsize_t>({5});
    constexpr auto dst_total = std::to_array<hsize_t>({8});
    constexpr auto src_total = std::to_array<hsize_t>({9});
    constexpr auto dst_off = std::to_array<hsize_t>({2});
    constexpr auto src_off = std::to_array<hsize_t>({3});
    VerifyHyperCopy(size, dst_total, dst_off.data(), src_total, src_off.data());
    VerifyHyperCopy(size, dst_total, H5VM_ZERO, src_total, H5VM_ZERO);
  }

  // 2-D (case 2): both contiguous (collapses to 0) and non-contiguous.
  {
    constexpr auto size = std::to_array<hsize_t>({2, 3});
    constexpr auto full = std::to_array<hsize_t>({2, 3});
    constexpr auto wider = std::to_array<hsize_t>({3, 5});
    constexpr auto off = std::to_array<hsize_t>({1, 1});
    VerifyHyperCopy(size, full, H5VM_ZERO, full, H5VM_ZERO);
    VerifyHyperCopy(size, wider, off.data(), full, H5VM_ZERO);
    VerifyHyperCopy(size, full, H5VM_ZERO, wider, off.data());
  }

  // 3-D (case 3): 0 dims contiguous, 1 dim contiguous, all dims contiguous.
  {
    constexpr auto size = std::to_array<hsize_t>({2, 2, 3});
    constexpr auto full = std::to_array<hsize_t>({2, 2, 3});
    constexpr auto break_dim2 = std::to_array<hsize_t>({2, 2, 4});
    constexpr auto break_dim1 = std::to_array<hsize_t>({3, 4, 3});
    constexpr auto off = std::to_array<hsize_t>({0, 1, 0});
    VerifyHyperCopy(size, full, H5VM_ZERO, full, H5VM_ZERO);
    VerifyHyperCopy(size, break_dim2, H5VM_ZERO, full, H5VM_ZERO);
    VerifyHyperCopy(size, break_dim1, off.data(), break_dim1, H5VM_ZERO);
    VerifyHyperCopy(size, full, H5VM_ZERO, break_dim1, off.data());
  }

  // 4-D (case 4): break at dim 3, break at dim 2, break at dim 1, and full.
  {
    constexpr auto size = std::to_array<hsize_t>({2, 2, 2, 3});
    constexpr auto full = std::to_array<hsize_t>({2, 2, 2, 3});
    constexpr auto break_dim3 = std::to_array<hsize_t>({2, 2, 2, 4});
    constexpr auto break_dim2 = std::to_array<hsize_t>({2, 2, 3, 3});
    constexpr auto break_dim1 = std::to_array<hsize_t>({3, 3, 2, 3});
    constexpr auto off = std::to_array<hsize_t>({1, 0, 0, 0});
    VerifyHyperCopy(size, full, H5VM_ZERO, full, H5VM_ZERO);
    VerifyHyperCopy(size, break_dim3, H5VM_ZERO, full, H5VM_ZERO);
    VerifyHyperCopy(size, break_dim2, H5VM_ZERO, break_dim2, H5VM_ZERO);
    VerifyHyperCopy(size, break_dim1, off.data(), break_dim1, off.data());
  }

  // 5-D (default loop): both fully contiguous and non-contiguous.
  {
    constexpr auto size = std::to_array<hsize_t>({2, 2, 2, 2, 3});
    constexpr auto full = std::to_array<hsize_t>({2, 2, 2, 2, 3});
    constexpr auto padded = std::to_array<hsize_t>({3, 2, 3, 2, 4});
    constexpr auto off = std::to_array<hsize_t>({1, 0, 1, 0, 1});
    VerifyHyperCopy(size, full, H5VM_ZERO, full, H5VM_ZERO);
    VerifyHyperCopy(size, padded, off.data(), padded, H5VM_ZERO);
    VerifyHyperCopy(size, padded, H5VM_ZERO, padded, off.data());
  }

  // Max rank (33 dimensions).
  {
    std::array<hsize_t, kHyperMaxDims> size{};
    std::array<hsize_t, kHyperMaxDims> total{};
    size.fill(1);
    total.fill(1);
    size[0] = 2;
    size[kHyperMaxDims - 1] = 3;
    total[0] = 2;
    total[kHyperMaxDims - 1] = 3;
    VerifyHyperCopy(size, total, H5VM_ZERO, total, H5VM_ZERO);
  }
}

TEST(H5VmTest, StrideFillAndCopyUnsignedAndSigned) {
  // H5VM_stride_fill with n == 0 and nelmts == 0.
  {
    std::array<uint8_t, 6> buf{};
    buf.fill(kSentinel);
    ASSERT_GE(H5VM_stride_fill(0, 4, nullptr, nullptr, buf.data(), 0x11), 0);
    EXPECT_THAT(buf, ElementsAre(0x11, 0x11, 0x11, 0x11, kSentinel, kSentinel));

    constexpr auto zero_size = std::to_array<hsize_t>({0});
    constexpr auto unit_stride = std::to_array<hsize_t>({1});
    ASSERT_GE(H5VM_stride_fill(1, 2, zero_size.data(), unit_stride.data(),
                               buf.data(), 0x99),
              0);
    EXPECT_THAT(buf, ElementsAre(0x11, 0x11, 0x11, 0x11, kSentinel, kSentinel));
  }

  // H5VM_stride_copy with n == 0, nelmts == 0, and 2-D multi-byte elements.
  {
    constexpr auto src = std::to_array<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8});
    std::array<uint8_t, 12> dst{};
    dst.fill(kSentinel);

    ASSERT_GE(H5VM_stride_copy(0, 5, nullptr, nullptr, dst.data(), nullptr,
                               src.data()),
              0);
    EXPECT_THAT(std::span<const uint8_t>(dst.data(), 6),
                ElementsAre(1, 2, 3, 4, 5, kSentinel));

    dst.fill(kSentinel);
    constexpr auto zero_size = std::to_array<hsize_t>({0});
    constexpr auto unit_stride = std::to_array<hsize_t>({1});
    ASSERT_GE(H5VM_stride_copy(1, 2, zero_size.data(), unit_stride.data(),
                               dst.data(), unit_stride.data(), src.data()),
              0);
    EXPECT_EQ(kSentinel, dst[0]);

    // 2x2 elements of 2 bytes with gap strides.
    constexpr auto size2d = std::to_array<hsize_t>({2, 2});
    constexpr auto dst_stride = std::to_array<hsize_t>({2, 2});
    constexpr auto src_stride = std::to_array<hsize_t>({0, 2});
    ASSERT_GE(H5VM_stride_copy(2, 2, size2d.data(), dst_stride.data(),
                               dst.data(), src_stride.data(), src.data()),
              0);
    EXPECT_THAT(dst, ElementsAre(1, 2, 3, 4, kSentinel, kSentinel, 5, 6, 7, 8,
                                 kSentinel, kSentinel));
  }

  // H5VM_stride_copy_s with n == 0, nelmts == 0, and negative reverse stride.
  {
    constexpr auto src = std::to_array<uint8_t>(
        {kSentinel, 10, 11, 20, 21, 30, 31, 40, 41, kSentinel});
    std::array<uint8_t, 10> dst{};
    dst.fill(kSentinel);

    ASSERT_GE(H5VM_stride_copy_s(0, 4, nullptr, nullptr, dst.data() + 1,
                                 nullptr, src.data() + 1),
              0);
    EXPECT_THAT(std::span<const uint8_t>(dst.data() + 1, 5),
                ElementsAre(10, 11, 20, 21, kSentinel));

    dst.fill(kSentinel);
    constexpr auto zero_size = std::to_array<hsize_t>({0});
    constexpr auto unit_stride = std::to_array<hssize_t>({1});
    ASSERT_GE(H5VM_stride_copy_s(1, 2, zero_size.data(), unit_stride.data(),
                                 dst.data(), unit_stride.data(), src.data()),
              0);
    EXPECT_EQ(kSentinel, dst[0]);

    // Reverse 4 elements of 2 bytes: start at &padded_src[8] ({40, 41}) and
    // step -2 per element. Two leading sentinel bytes keep the post-iteration
    // pointer decrement at &padded_src[0] (within the array bounds).
    constexpr std::array<uint8_t, 12> padded_src = {
        kSentinel, kSentinel, 10, 11, 20,        21,
        30,        31,        40, 41, kSentinel, kSentinel};
    std::array<uint8_t, 12> padded_dst{};
    padded_dst.fill(kSentinel);
    constexpr auto count = std::to_array<hsize_t>({4});
    constexpr auto fwd_stride = std::to_array<hssize_t>({2});
    constexpr auto rev_stride = std::to_array<hssize_t>({-2});
    ASSERT_GE(H5VM_stride_copy_s(1, 2, count.data(), fwd_stride.data(),
                                 padded_dst.data() + 2, rev_stride.data(),
                                 padded_src.data() + 8),
              0);
    EXPECT_THAT(padded_dst, ElementsAre(kSentinel, kSentinel, 40, 41, 30, 31,
                                        20, 21, 10, 11, kSentinel, kSentinel));

    // 2-D signed copy: reverse 2x2 rows (src row step -2 bytes: +1 within row,
    // then -4 on row carry) while spacing dst rows by +1 sentinel byte. After
    // element (1,1) at padded_src[5], j=1 adds +1 and j=0 adds -4 to index 2.
    constexpr auto dims2d = std::to_array<hsize_t>({2, 2});
    constexpr auto dst_s2d = std::to_array<hssize_t>({1, 1});
    constexpr auto src_s2d = std::to_array<hssize_t>({-4, 1});
    std::array<uint8_t, 10> dst2d{};
    dst2d.fill(kSentinel);
    ASSERT_GE(H5VM_stride_copy_s(2, 1, dims2d.data(), dst_s2d.data(),
                                 dst2d.data() + 1, src_s2d.data(),
                                 padded_src.data() + 6),
              0);
    EXPECT_THAT(dst2d, ElementsAre(kSentinel, 30, 31, kSentinel, 20, 21,
                                   kSentinel, kSentinel, kSentinel, kSentinel));
  }
}

TEST(H5VmTest, ArrayFillDoublingAndTailCases) {
  constexpr auto pattern = std::to_array<uint8_t>({0xDE, 0xAD, 0xBE});
  constexpr auto kCounts = std::to_array<size_t>({1, 2, 3, 4, 7, 15, 16, 37});
  for (const size_t count : kCounts) {
    std::vector<uint8_t> buf(count * pattern.size() + 2, kSentinel);
    ASSERT_GE(
        H5VM_array_fill(buf.data(), pattern.data(), pattern.size(), count), 0);
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(0xDE, buf[i * 3 + 0]) << "count=" << count << " i=" << i;
      EXPECT_EQ(0xAD, buf[i * 3 + 1]) << "count=" << count << " i=" << i;
      EXPECT_EQ(0xBE, buf[i * 3 + 2]) << "count=" << count << " i=" << i;
    }
    EXPECT_EQ(kSentinel, buf[count * 3]);
    EXPECT_EQ(kSentinel, buf[count * 3 + 1]);
  }
}

TEST(H5VmTest, ArrayDownOffsetAndCalcRoundTrip) {
  // n == 0 edge cases.
  {
    hsize_t dummy = 99;
    H5VM_array_down(0, &dummy, &dummy);
    EXPECT_EQ(99u, dummy);
    EXPECT_EQ(0u, H5VM_array_offset_pre(0, &dummy, &dummy));
    EXPECT_EQ(0u, H5VM_array_offset(0, &dummy, &dummy));
    EXPECT_GE(H5VM_array_calc_pre(0, 0, &dummy, &dummy), 0);
    EXPECT_GE(H5VM_array_calc(0, 0, &dummy, &dummy), 0);
  }

  // 3-D exhaustive round-trip.
  constexpr auto dims = std::to_array<hsize_t>({2, 3, 4});
  std::array<hsize_t, 3> down{};
  H5VM_array_down(3, dims.data(), down.data());
  EXPECT_THAT(down, ElementsAre(12, 4, 1));

  for (hsize_t linear = 0; linear < 24; ++linear) {
    std::array<hsize_t, 3> coords{};
    std::array<hsize_t, 3> coords_pre{};
    ASSERT_GE(H5VM_array_calc(linear, 3, dims.data(), coords.data()), 0);
    ASSERT_GE(H5VM_array_calc_pre(linear, 3, down.data(), coords_pre.data()),
              0);
    EXPECT_EQ(coords, coords_pre);
    EXPECT_EQ(linear, H5VM_array_offset(3, dims.data(), coords.data()));
    EXPECT_EQ(linear, H5VM_array_offset_pre(3, down.data(), coords.data()));
  }
}

TEST(H5VmTest, ChunkScaledAndChunkIndexComputations) {
  // ndims == 0.
  {
    hsize_t dummy = 1;
    H5VM_chunk_scaled(0, &dummy, &dummy, &dummy);
    EXPECT_EQ(0u, H5VM_chunk_index(0, &dummy, &dummy, &dummy));
    EXPECT_EQ(0u, H5VM_chunk_index_scaled(0, &dummy, &dummy, &dummy, &dummy));
  }

  // 3x6 grid of 3x5 chunks.
  constexpr auto chunk_dims = std::to_array<hsize_t>({3, 5});
  constexpr auto nchunks = std::to_array<hsize_t>({3, 6});
  std::array<hsize_t, 2> down_nchunks{};
  H5VM_array_down(2, nchunks.data(), down_nchunks.data());
  EXPECT_THAT(down_nchunks, ElementsAre(6, 1));

  constexpr auto coord = std::to_array<hsize_t>({7, 23});  // chunk (2, 4) -> 16
  std::array<hsize_t, 2> scaled_only{};
  std::array<hsize_t, 2> scaled_idx{};
  H5VM_chunk_scaled(2, coord.data(), chunk_dims.data(), scaled_only.data());
  EXPECT_THAT(scaled_only, ElementsAre(2, 4));
  EXPECT_EQ(16u, H5VM_chunk_index(2, coord.data(), chunk_dims.data(),
                                  down_nchunks.data()));
  EXPECT_EQ(16u,
            H5VM_chunk_index_scaled(2, coord.data(), chunk_dims.data(),
                                    down_nchunks.data(), scaled_idx.data()));
  EXPECT_THAT(scaled_idx, ElementsAre(2, 4));
}

TEST(H5VmTest, OpvvCoversAllStateMachineTransitionsAndContinuation) {
  ASSERT_GE(H5open(), 0);

  // 1. Start in src_smaller, loop multiple times, exhaust src mid-dst, then
  //    resume on the remainder.
  {
    auto dst_len = std::to_array<size_t>({10, 4});
    auto dst_off = std::to_array<hsize_t>({100, 200});
    auto src_len = std::to_array<size_t>({3, 4});
    auto src_off = std::to_array<hsize_t>({10, 20});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec;
    const ssize_t nbytes =
        H5VM_opvv(dst_len.size(), &dst_curr, dst_len.data(), dst_off.data(),
                  src_len.size(), &src_curr, src_len.data(), src_off.data(),
                  RecordOpvvSegment, &rec);
    EXPECT_EQ(7, nbytes);
    EXPECT_EQ(0u, dst_curr);
    EXPECT_EQ(2u, src_curr);
    EXPECT_EQ(3u, dst_len[0]);
    EXPECT_EQ(107u, dst_off[0]);
    EXPECT_THAT(rec.segments,
                ElementsAre(OpvvSegment{100, 10, 3}, OpvvSegment{103, 20, 4}));

    auto src2_len = std::to_array<size_t>({3, 4});
    auto src2_off = std::to_array<hsize_t>({30, 40});
    size_t src2_curr = 0;
    OpvvRecorder rec2;
    const ssize_t nbytes2 =
        H5VM_opvv(dst_len.size(), &dst_curr, dst_len.data(), dst_off.data(),
                  src2_len.size(), &src2_curr, src2_len.data(), src2_off.data(),
                  RecordOpvvSegment, &rec2);
    EXPECT_EQ(7, nbytes2);
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(2u, src2_curr);
    EXPECT_THAT(rec2.segments,
                ElementsAre(OpvvSegment{107, 30, 3}, OpvvSegment{200, 40, 4}));
  }

  // 2. Start in src_smaller -> dst_smaller -> equal -> src_smaller -> equal,
  //    plus non-zero starting indices.
  {
    auto dst_len = std::to_array<size_t>({99, 10, 4, 5, 3, 2});
    auto dst_off = std::to_array<hsize_t>({99, 100, 200, 300, 400, 500});
    auto src_len = std::to_array<size_t>({99, 99, 3, 9, 3, 5});
    auto src_off = std::to_array<hsize_t>({99, 99, 10, 20, 30, 40});
    size_t dst_curr = 1;
    size_t src_curr = 2;
    OpvvRecorder rec;
    const ssize_t nbytes =
        H5VM_opvv(dst_len.size(), &dst_curr, dst_len.data(), dst_off.data(),
                  src_len.size(), &src_curr, src_len.data(), src_off.data(),
                  RecordOpvvSegment, &rec);
    // Trace:
    //   src_smaller: dst(10@100) vs src(3@10) -> (100,10,3), dst rem 7@103
    //   dst_smaller: dst(7@103) vs src(9@20) -> (103,20,7), src rem 2@27
    //   src_smaller: dst(4@200) vs src(2@27) -> (200,27,2), dst rem 2@202
    //   dst_smaller: dst(2@202) vs src(3@30) -> (202,30,2), src rem 1@32
    //   src_smaller: dst(5@300) vs src(1@32) -> (300,32,1), dst rem 4@301
    //   dst_smaller: dst(4@301) vs src(5@40) -> (301,40,4), src rem 1@44
    //   src_smaller: dst(3@400) vs src(1@44) -> (400,44,1), src exhausted.
    EXPECT_EQ(20, nbytes);
    EXPECT_EQ(4u, dst_curr);
    EXPECT_EQ(6u, src_curr);
    EXPECT_EQ(2u, dst_len[4]);
    EXPECT_EQ(401u, dst_off[4]);
    EXPECT_THAT(rec.segments,
                ElementsAre(OpvvSegment{100, 10, 3}, OpvvSegment{103, 20, 7},
                            OpvvSegment{200, 27, 2}, OpvvSegment{202, 30, 2},
                            OpvvSegment{300, 32, 1}, OpvvSegment{301, 40, 4},
                            OpvvSegment{400, 44, 1}));
  }

  // 3. src_smaller -> equal -> dst_smaller, and dst_smaller -> equal ->
  //    dst_smaller (loop) -> exhaust dst.
  {
    auto dst_len = std::to_array<size_t>({5, 3, 2, 2});
    auto dst_off = std::to_array<hsize_t>({0, 10, 20, 30});
    auto src_len = std::to_array<size_t>({2, 3, 3, 10});
    auto src_off = std::to_array<hsize_t>({100, 110, 120, 130});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec;
    // src_smaller: (0,100,2)->dst rem 3; next src=3 -> equal!
    // equal: (2,110,3)->next dst=3,src=3 -> equal: (10,120,3)->next
    // dst=2,src=10
    //   -> dst_smaller!
    // dst_smaller: (20,130,2)->src rem 8; dst=2 -> dst_smaller: (30,132,2)->dst
    //   exhausted!
    const ssize_t nbytes =
        H5VM_opvv(dst_len.size(), &dst_curr, dst_len.data(), dst_off.data(),
                  src_len.size(), &src_curr, src_len.data(), src_off.data(),
                  RecordOpvvSegment, &rec);
    EXPECT_EQ(12, nbytes);
    EXPECT_EQ(4u, dst_curr);
    EXPECT_EQ(3u, src_curr);
    EXPECT_EQ(6u, src_len[3]);
    EXPECT_EQ(134u, src_off[3]);
    EXPECT_THAT(rec.segments,
                ElementsAre(OpvvSegment{0, 100, 2}, OpvvSegment{2, 110, 3},
                            OpvvSegment{10, 120, 3}, OpvvSegment{20, 130, 2},
                            OpvvSegment{30, 132, 2}));
  }

  // 4. dst_smaller -> src_smaller and equal -> src_smaller, equal ->
  //    exhaust src vs exhaust dst vs exhaust both.
  {
    auto dst_len = std::to_array<size_t>({3, 5, 4});
    auto dst_off = std::to_array<hsize_t>({0, 10, 20});
    auto src_len = std::to_array<size_t>({8, 2});
    auto src_off = std::to_array<hsize_t>({100, 200});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec;
    // dst_smaller: (0,100,3) src rem 5; next dst=5 -> equal!
    // equal: (10,103,5) next dst=4, src=2 -> src_smaller!
    // src_smaller: (20,200,2) src exhausted!
    EXPECT_EQ(
        10, H5VM_opvv(dst_len.size(), &dst_curr, dst_len.data(), dst_off.data(),
                      src_len.size(), &src_curr, src_len.data(), src_off.data(),
                      RecordOpvvSegment, &rec));
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(2u, src_curr);
    EXPECT_EQ(2u, dst_len[2]);
    EXPECT_EQ(22u, dst_off[2]);
    EXPECT_THAT(rec.segments,
                ElementsAre(OpvvSegment{0, 100, 3}, OpvvSegment{10, 103, 5},
                            OpvvSegment{20, 200, 2}));
  }

  // 5. equal loop terminating on src exhaustion (while dst has segments left),
  //    on dst exhaustion (while src has segments left), and on exact tie.
  {
    auto dst_len = std::to_array<size_t>({4, 4, 4});
    auto dst_off = std::to_array<hsize_t>({0, 10, 20});
    auto src_len = std::to_array<size_t>({4, 4});
    auto src_off = std::to_array<hsize_t>({100, 110});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec;
    EXPECT_EQ(
        8, H5VM_opvv(dst_len.size(), &dst_curr, dst_len.data(), dst_off.data(),
                     src_len.size(), &src_curr, src_len.data(), src_off.data(),
                     RecordOpvvSegment, &rec));
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(2u, src_curr);
    EXPECT_THAT(rec.segments,
                ElementsAre(OpvvSegment{0, 100, 4}, OpvvSegment{10, 110, 4}));

    dst_curr = 0;
    src_curr = 0;
    rec.segments.clear();
    EXPECT_EQ(
        8, H5VM_opvv(2, &dst_curr, dst_len.data(), dst_off.data(), 3, &src_curr,
                     dst_len.data(), dst_off.data(), RecordOpvvSegment, &rec));
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(2u, src_curr);
    EXPECT_THAT(rec.segments,
                ElementsAre(OpvvSegment{0, 0, 4}, OpvvSegment{10, 10, 4}));
  }
}

TEST(H5VmTest, OpvvPropagatesCallbackFailureInAllThreeStates) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  // Fail inside src_smaller.
  {
    auto dst_len = std::to_array<size_t>({5});
    auto dst_off = std::to_array<hsize_t>({0});
    auto src_len = std::to_array<size_t>({2});
    auto src_off = std::to_array<hsize_t>({0});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec{.fail_on_call = 0};
    EXPECT_LT(
        H5VM_opvv(1, &dst_curr, dst_len.data(), dst_off.data(), 1, &src_curr,
                  src_len.data(), src_off.data(), RecordOpvvSegment, &rec),
        0);
  }

  // Fail inside dst_smaller.
  {
    auto dst_len = std::to_array<size_t>({2});
    auto dst_off = std::to_array<hsize_t>({0});
    auto src_len = std::to_array<size_t>({5});
    auto src_off = std::to_array<hsize_t>({0});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec{.fail_on_call = 0};
    EXPECT_LT(
        H5VM_opvv(1, &dst_curr, dst_len.data(), dst_off.data(), 1, &src_curr,
                  src_len.data(), src_off.data(), RecordOpvvSegment, &rec),
        0);
  }

  // Fail inside equal.
  {
    auto dst_len = std::to_array<size_t>({3});
    auto dst_off = std::to_array<hsize_t>({0});
    auto src_len = std::to_array<size_t>({3});
    auto src_off = std::to_array<hsize_t>({0});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    OpvvRecorder rec{.fail_on_call = 0};
    EXPECT_LT(
        H5VM_opvv(1, &dst_curr, dst_len.data(), dst_off.data(), 1, &src_curr,
                  src_len.data(), src_off.data(), RecordOpvvSegment, &rec),
        0);
  }
}

TEST(H5VmTest, MemcpyvvCoversAllStatesTransitionsAndContinuation) {
  std::array<uint8_t, 64> src{};
  std::iota(src.begin(), src.end(), static_cast<uint8_t>(1));

  // 1. src_smaller multi-iteration -> exhaust src, then resume dst remainder.
  {
    std::array<uint8_t, 20> dst{};
    dst.fill(kSentinel);
    auto dst_len = std::to_array<size_t>({10, 3});
    auto dst_off = std::to_array<hsize_t>({2, 14});
    auto src_len = std::to_array<size_t>({3, 4});
    auto src_off = std::to_array<hsize_t>({5, 20});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    EXPECT_EQ(
        7, H5VM_memcpyvv(dst.data(), dst_len.size(), &dst_curr, dst_len.data(),
                         dst_off.data(), src.data(), src_len.size(), &src_curr,
                         src_len.data(), src_off.data()));
    EXPECT_EQ(0u, dst_curr);
    EXPECT_EQ(2u, src_curr);
    EXPECT_EQ(3u, dst_len[0]);
    EXPECT_EQ(9u, dst_off[0]);

    auto src2_len = std::to_array<size_t>({3, 3});
    auto src2_off = std::to_array<hsize_t>({30, 40});
    size_t src2_curr = 0;
    EXPECT_EQ(
        6, H5VM_memcpyvv(dst.data(), dst_len.size(), &dst_curr, dst_len.data(),
                         dst_off.data(), src.data(), src2_len.size(),
                         &src2_curr, src2_len.data(), src2_off.data()));
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(2u, src2_curr);
    EXPECT_THAT(dst, ElementsAre(kSentinel, kSentinel, 6, 7, 8, 21, 22, 23, 24,
                                 31, 32, 33, kSentinel, kSentinel, 41, 42, 43,
                                 kSentinel, kSentinel, kSentinel));
  }

  // 2. src_smaller -> dst_smaller -> src_smaller -> dst_smaller -> equal ->
  //    src_smaller -> equal (covers all cross-state transitions).
  {
    std::array<uint8_t, 32> dst{};
    dst.fill(kSentinel);
    auto dst_len = std::to_array<size_t>({6, 2, 5, 3, 2});
    auto dst_off = std::to_array<hsize_t>({0, 7, 10, 16, 20});
    auto src_len = std::to_array<size_t>({2, 5, 2, 4, 3, 2});
    auto src_off = std::to_array<hsize_t>({0, 10, 20, 30, 40, 50});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    // Trace:
    //   src_smaller: dst(6) vs src(2@0) -> copy 2, dst rem 4; next src(5@10) ->
    //     dst_smaller!
    //   dst_smaller: dst(4) vs src(5@10) -> copy 4, src rem 1; next dst(2@7) ->
    //     src_smaller!
    //   src_smaller: dst(2) vs src(1@14) -> copy 1, dst rem 1; next src(2@20)
    //   ->
    //     dst_smaller!
    //   dst_smaller: dst(1) vs src(2@20) -> copy 1, src rem 1; next dst(5@10)
    //   ->
    //     src_smaller!
    //   src_smaller: dst(5) vs src(1@21) -> copy 1, dst rem 4; next src(4@30)
    //   ->
    //     equal!
    //   equal: dst(4) vs src(4@30) -> copy 4; next dst(3@16), src(3@40) ->
    //     equal: copy 3; next dst(2@20), src(2@50) -> equal: copy 2!
    EXPECT_EQ(
        18, H5VM_memcpyvv(dst.data(), dst_len.size(), &dst_curr, dst_len.data(),
                          dst_off.data(), src.data(), src_len.size(), &src_curr,
                          src_len.data(), src_off.data()));
    EXPECT_EQ(5u, dst_curr);
    EXPECT_EQ(6u, src_curr);
    EXPECT_THAT(std::span<const uint8_t>(dst.data(), 23),
                ElementsAre(1, 2, 11, 12, 13, 14, kSentinel, 15, 21, kSentinel,
                            22, 31, 32, 33, 34, kSentinel, 41, 42, 43,
                            kSentinel, 51, 52, kSentinel));
  }

  // 3. src_smaller -> equal -> src_smaller, and dst_smaller multi-iteration
  //    ending in dst exhaustion, plus equal -> dst_smaller.
  {
    std::array<uint8_t, 20> dst{};
    dst.fill(kSentinel);
    auto dst_len = std::to_array<size_t>({5, 3, 6});
    auto dst_off = std::to_array<hsize_t>({0, 6, 10});
    auto src_len = std::to_array<size_t>({2, 3, 2, 4});
    auto src_off = std::to_array<hsize_t>({0, 10, 20, 30});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    // src_smaller: (0,0,2) dst rem 3 -> equal (3@10): (2,10,3) ->
    // equal(3@6, 2@20) -> src_smaller: (6,20,2) dst rem 1 -> dst_smaller:
    // (8,30,1) src rem 3 -> src_smaller(6@10, 3@31): (10,31,3) -> dst rem 3 ->
    // src exhausted!
    EXPECT_EQ(
        11, H5VM_memcpyvv(dst.data(), dst_len.size(), &dst_curr, dst_len.data(),
                          dst_off.data(), src.data(), src_len.size(), &src_curr,
                          src_len.data(), src_off.data()));
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(4u, src_curr);
    EXPECT_EQ(3u, dst_len[2]);
    EXPECT_EQ(13u, dst_off[2]);
    EXPECT_THAT(std::span<const uint8_t>(dst.data(), 14),
                ElementsAre(1, 2, 11, 12, 13, kSentinel, 21, 22, 31, kSentinel,
                            32, 33, 34, kSentinel));

    // dst_smaller multi-iteration exhausting dst mid-src.
    dst.fill(kSentinel);
    auto d2_len = std::to_array<size_t>({2, 3, 2});
    auto d2_off = std::to_array<hsize_t>({0, 3, 7});
    auto s2_len = std::to_array<size_t>({10});
    auto s2_off = std::to_array<hsize_t>({5});
    dst_curr = 0;
    src_curr = 0;
    EXPECT_EQ(7,
              H5VM_memcpyvv(dst.data(), d2_len.size(), &dst_curr, d2_len.data(),
                            d2_off.data(), src.data(), s2_len.size(), &src_curr,
                            s2_len.data(), s2_off.data()));
    EXPECT_EQ(3u, dst_curr);
    EXPECT_EQ(0u, src_curr);
    EXPECT_EQ(3u, s2_len[0]);
    EXPECT_EQ(12u, s2_off[0]);
    EXPECT_THAT(
        std::span<const uint8_t>(dst.data(), 10),
        ElementsAre(6, 7, kSentinel, 8, 9, 10, kSentinel, 11, 12, kSentinel));
  }

  // 4. dst_smaller -> equal -> dst_smaller, and equal -> dst_smaller.
  {
    std::array<uint8_t, 16> dst{};
    dst.fill(kSentinel);
    auto dst_len = std::to_array<size_t>({2, 3, 2});
    auto dst_off = std::to_array<hsize_t>({0, 3, 7});
    auto src_len = std::to_array<size_t>({5, 4});
    auto src_off = std::to_array<hsize_t>({0, 10});
    size_t dst_curr = 0;
    size_t src_curr = 0;
    // dst_smaller: (0,0,2) src rem 3 -> equal(3@3, 3@2): (3,2,3) ->
    // dst(2@7), src(4@10) -> dst_smaller: (7,10,2) dst exhausted!
    EXPECT_EQ(
        7, H5VM_memcpyvv(dst.data(), dst_len.size(), &dst_curr, dst_len.data(),
                         dst_off.data(), src.data(), src_len.size(), &src_curr,
                         src_len.data(), src_off.data()));
    EXPECT_EQ(3u, dst_curr);
    EXPECT_EQ(1u, src_curr);
    EXPECT_EQ(2u, src_len[1]);
    EXPECT_EQ(12u, src_off[1]);
    EXPECT_THAT(
        std::span<const uint8_t>(dst.data(), 10),
        ElementsAre(1, 2, kSentinel, 3, 4, 5, kSentinel, 11, 12, kSentinel));

    // equal -> dst_smaller (without entering dst_smaller first).
    dst.fill(kSentinel);
    auto d3_len = std::to_array<size_t>({3, 2});
    auto d3_off = std::to_array<hsize_t>({0, 4});
    auto s3_len = std::to_array<size_t>({3, 5});
    auto s3_off = std::to_array<hsize_t>({0, 10});
    dst_curr = 0;
    src_curr = 0;
    EXPECT_EQ(5,
              H5VM_memcpyvv(dst.data(), d3_len.size(), &dst_curr, d3_len.data(),
                            d3_off.data(), src.data(), s3_len.size(), &src_curr,
                            s3_len.data(), s3_off.data()));
    EXPECT_EQ(2u, dst_curr);
    EXPECT_EQ(1u, src_curr);
    EXPECT_EQ(3u, s3_len[1]);
    EXPECT_EQ(12u, s3_off[1]);
    EXPECT_THAT(std::span<const uint8_t>(dst.data(), 7),
                ElementsAre(1, 2, 3, kSentinel, 11, 12, kSentinel));
  }
}

}  // namespace
}  // namespace hdf5
