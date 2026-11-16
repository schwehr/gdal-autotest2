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
// Tests internal OpenJPEG multi-component transforms (MCT) and norm routines
// (src/lib/openjp2/mct.c).
//
// Caveats:
// - opj_mct_encode, opj_mct_decode, opj_mct_encode_real, and
//   opj_mct_decode_real use SSE/SSE2 16-byte aligned loads/stores on x86 and
//   fall back to scalar loops on non-x86 targets (--cpu=arm).
// - opj_mct_getnorm and opj_mct_getnorm_real index directly into static 3-entry
//   arrays without bounds checks; compno >= 3 is undefined behavior and is
//   intentionally not exercised.
// - Cross-module dependency: opj_mct_decode_custom round-trip test uses
//   opj_matrix_inversion_f (src/lib/openjp2/invert.c).
// - All upstream opj_mct_* functions are covered; only OOM branches in
//   opj_mct_encode_custom / opj_mct_decode_custom remain uncovered.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/mct.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/mct.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::FloatEq;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);

constexpr std::size_t kGuardElements = 4;
constexpr OPJ_INT32 kIntSentinel = -999999;
constexpr OPJ_FLOAT32 kFloatSentinel = -999.0f;
constexpr OPJ_FLOAT64 kDoubleSentinel = -999.0;
constexpr OPJ_FLOAT32 kDirectFloatTolerance = 1e-4f;
// 1e-4f covers SSE vs scalar (--cpu=arm / aarch64 FMA contraction) float
// differences and 4-5 digit tabulated ICT constants on inputs up to +-128.75.
constexpr OPJ_FLOAT32 kRoundTripFloatTolerance = 0.01f;
// Empirical round-trip tolerance across SSE/scalar float ICT forward+inverse
// and 4-digit upstream ICT coefficients on inputs up to +-128.75.

// 32-byte aligned buffer with trailing sentinel guard elements to satisfy SSE/
// SSE2 16-byte alignment invariants and detect out-of-bounds writes.
template <typename T>
class AlignedBuffer {
 public:
  AlignedBuffer(std::size_t count, T sentinel)
      : count_(count),
        storage_(static_cast<T*>(
            opj_aligned_32_malloc((count + kGuardElements) * sizeof(T)))) {
    if (storage_ != nullptr) {
      std::fill_n(storage_.get(), count_ + kGuardElements, sentinel);
    }
  }

  AlignedBuffer(std::span<const T> values, T sentinel)
      : AlignedBuffer(values.size(), sentinel) {
    if (storage_ != nullptr) {
      std::copy(values.begin(), values.end(), storage_.get());
    }
  }

  T* data() { return storage_.get(); }
  const T* data() const { return storage_.get(); }
  std::size_t size() const { return count_; }

  std::span<const T> values() const { return {storage_.get(), count_}; }
  std::span<const T> guard() const {
    return {storage_.get() + count_, kGuardElements};
  }

 private:
  std::size_t count_;
  OpjAlignedPtr<T> storage_;
};

struct RctVectorCase {
  std::string_view name;
  OPJ_INT32 r;
  OPJ_INT32 g;
  OPJ_INT32 b;
  OPJ_INT32 expected_y;
  OPJ_INT32 expected_u;
  OPJ_INT32 expected_v;
};

constexpr auto kRctVectorCases = std::to_array<RctVectorCase>({
    {.name = "zero",
     .r = 0,
     .g = 0,
     .b = 0,
     .expected_y = 0,
     .expected_u = 0,
     .expected_v = 0},
    {.name = "white_255",
     .r = 255,
     .g = 255,
     .b = 255,
     .expected_y = 255,
     .expected_u = 0,
     .expected_v = 0},
    {.name = "unit_gray",
     .r = 1,
     .g = 1,
     .b = 1,
     .expected_y = 1,
     .expected_u = 0,
     .expected_v = 0},
    {.name = "positive_remainder_truncates_down",
     .r = 3,
     .g = 0,
     .b = 0,
     .expected_y = 0,
     .expected_u = 0,
     .expected_v = 3},
    {.name = "red_primary",
     .r = 100,
     .g = 0,
     .b = 0,
     .expected_y = 25,
     .expected_u = 0,
     .expected_v = 100},
    {.name = "green_primary",
     .r = 0,
     .g = 100,
     .b = 0,
     .expected_y = 50,
     .expected_u = -100,
     .expected_v = -100},
    {.name = "blue_primary",
     .r = 0,
     .g = 0,
     .b = 100,
     .expected_y = 25,
     .expected_u = 100,
     .expected_v = 0},
    {.name = "negative_floor_div",
     .r = -3,
     .g = 1,
     .b = -2,
     .expected_y = -1,
     .expected_u = -3,
     .expected_v = -4},
    {.name = "negative_chroma",
     .r = -128,
     .g = 64,
     .b = -1,
     .expected_y = -1,
     .expected_u = -65,
     .expected_v = -192},
    {.name = "large_24bit_signed",
     .r = 8388607,
     .g = -8388608,
     .b = 4194303,
     .expected_y = -1048577,
     .expected_u = 12582911,
     .expected_v = 16777215},
});

struct IctVectorCase {
  std::string_view name;
  OPJ_FLOAT32 c0;
  OPJ_FLOAT32 c1;
  OPJ_FLOAT32 c2;
  OPJ_FLOAT32 expected_0;
  OPJ_FLOAT32 expected_1;
  OPJ_FLOAT32 expected_2;
};

constexpr auto kIctForwardCases = std::to_array<IctVectorCase>({
    {.name = "zero",
     .c0 = 0.0f,
     .c1 = 0.0f,
     .c2 = 0.0f,
     .expected_0 = 0.0f,
     .expected_1 = 0.0f,
     .expected_2 = 0.0f},
    {.name = "red_basis",
     .c0 = 1.0f,
     .c1 = 0.0f,
     .c2 = 0.0f,
     .expected_0 = 0.299f,
     .expected_1 = -0.16875f,
     .expected_2 = 0.5f},
    {.name = "green_basis",
     .c0 = 0.0f,
     .c1 = 1.0f,
     .c2 = 0.0f,
     .expected_0 = 0.587f,
     .expected_1 = -0.331260f,
     .expected_2 = -0.41869f},
    {.name = "blue_basis",
     .c0 = 0.0f,
     .c1 = 0.0f,
     .c2 = 1.0f,
     .expected_0 = 0.114f,
     .expected_1 = 0.5f,
     .expected_2 = -0.08131f},
    {.name = "gray_100",
     .c0 = 100.0f,
     .c1 = 100.0f,
     .c2 = 100.0f,
     .expected_0 = 100.0f,
     .expected_1 = -0.001f,
     .expected_2 = 0.0f},
    {.name = "signed_mixed",
     .c0 = -50.0f,
     .c1 = 120.0f,
     .c2 = -30.0f,
     .expected_0 = 52.07f,
     .expected_1 = -46.3137f,
     .expected_2 = -72.8035f},
});

constexpr auto kIctInverseCases = std::to_array<IctVectorCase>({
    {.name = "zero",
     .c0 = 0.0f,
     .c1 = 0.0f,
     .c2 = 0.0f,
     .expected_0 = 0.0f,
     .expected_1 = 0.0f,
     .expected_2 = 0.0f},
    {.name = "y_basis",
     .c0 = 1.0f,
     .c1 = 0.0f,
     .c2 = 0.0f,
     .expected_0 = 1.0f,
     .expected_1 = 1.0f,
     .expected_2 = 1.0f},
    {.name = "cb_basis",
     .c0 = 0.0f,
     .c1 = 1.0f,
     .c2 = 0.0f,
     .expected_0 = 0.0f,
     .expected_1 = -0.34413f,
     .expected_2 = 1.772f},
    {.name = "cr_basis",
     .c0 = 0.0f,
     .c1 = 0.0f,
     .c2 = 1.0f,
     .expected_0 = 1.402f,
     .expected_1 = -0.71414f,
     .expected_2 = 0.0f},
    {.name = "signed_mixed",
     .c0 = -50.0f,
     .c1 = 25.0f,
     .c2 = -10.0f,
     .expected_0 = -64.02f,
     .expected_1 = -51.46185f,
     .expected_2 = -5.7f},
});

// Sizes spanning < 4 (scalar only), exact 4/8 multiples (SIMD only), and
// multi-iteration SIMD + non-empty scalar tails up to 69.
constexpr auto kSimdAndTailSizes = std::to_array<OPJ_SIZE_T>(
    {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 31, 32, 33, 35, 39, 69});

// JPEG 2000 Part 1 forward ICT matrix (non-dyadic signed coefficients).
constexpr auto kIct3x3 = std::to_array<OPJ_FLOAT32>({
    0.299f,
    0.587f,
    0.114f,
    -0.16875f,
    -0.331260f,
    0.5f,
    0.5f,
    -0.41869f,
    -0.08131f,
});

// Asymmetric invertible matrices whose entries are exact dyadic multiples of
// 1/8 so Q13 quantization in opj_mct_encode_custom introduces zero rounding
// error on the matrix coefficients themselves.
constexpr auto kAsymmetric3x3 = std::to_array<OPJ_FLOAT32>({
    1.25f,
    -0.50f,
    0.25f,
    0.125f,
    1.50f,
    -0.25f,
    -0.375f,
    0.25f,
    1.75f,
});

constexpr auto kAsymmetric4x4 = std::to_array<OPJ_FLOAT32>({
    1.50f,
    0.25f,
    -0.125f,
    0.00f,
    0.00f,
    1.25f,
    0.375f,
    -0.25f,
    -0.25f,
    0.00f,
    1.75f,
    0.125f,
    0.125f,
    -0.375f,
    0.00f,
    1.50f,
});

constexpr auto kDyadicPerm4x4 = std::to_array<OPJ_FLOAT32>({
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    -0.5f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    2.0f,
    -1.0f,
    0.0f,
    0.0f,
    0.0f,
});

constexpr auto kDyadicPerm4x4Inv = std::to_array<OPJ_FLOAT32>({
    0.0f,
    0.0f,
    0.0f,
    -1.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    -2.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.5f,
    0.0f,
});

OPJ_INT32 MakeIntSample(std::size_t index, OPJ_UINT32 comp) {
  const std::int64_t centered =
      static_cast<std::int64_t>((index * 137ULL + comp * 59ULL) % 20001ULL) -
      10000;
  const std::int64_t sign = ((index + comp) % 3 == 0) ? -1 : 1;
  return static_cast<OPJ_INT32>(centered * sign * ((comp == 1) ? 3 : 1));
}

OPJ_FLOAT32 MakeFloatSample(std::size_t index, OPJ_UINT32 comp) {
  const int centered =
      static_cast<int>((index * 43ULL + comp * 29ULL) % 256ULL) - 128;
  const float frac = static_cast<float>((index + comp * 3U) % 5U) * 0.25f;
  return static_cast<OPJ_FLOAT32>(centered) + ((centered < 0) ? -frac : frac);
}

std::vector<std::vector<OPJ_INT32>> MakeIntComponentInputs(
    OPJ_UINT32 num_comps, OPJ_SIZE_T sample_count) {
  std::vector<std::vector<OPJ_INT32>> inputs(num_comps);
  for (OPJ_UINT32 c = 0; c < num_comps; ++c) {
    inputs[c].resize(sample_count);
    for (OPJ_SIZE_T i = 0; i < sample_count; ++i) {
      inputs[c][i] = MakeIntSample(i, c);
    }
  }
  return inputs;
}

std::vector<std::vector<OPJ_FLOAT32>> MakeFloatComponentInputs(
    OPJ_UINT32 num_comps, OPJ_SIZE_T sample_count) {
  std::vector<std::vector<OPJ_FLOAT32>> inputs(num_comps);
  for (OPJ_UINT32 c = 0; c < num_comps; ++c) {
    inputs[c].resize(sample_count);
    for (OPJ_SIZE_T i = 0; i < sample_count; ++i) {
      inputs[c][i] = MakeFloatSample(i, c);
    }
  }
  return inputs;
}

template <typename T>
void PrepareCustomBuffers(std::span<const std::vector<T>> inputs,
                          OPJ_SIZE_T sample_count, T sentinel,
                          std::vector<AlignedBuffer<T>>* buffers,
                          std::vector<OPJ_BYTE*>* ptrs) {
  buffers->clear();
  buffers->reserve(inputs.size());
  ptrs->assign(inputs.size(), nullptr);
  for (std::size_t c = 0; c < inputs.size(); ++c) {
    ASSERT_EQ(sample_count, inputs[c].size());
    buffers->emplace_back(inputs[c], sentinel);
    ASSERT_NE(nullptr, buffers->back().data());
    (*ptrs)[c] = reinterpret_cast<OPJ_BYTE*>(buffers->back().data());
  }
}

void VerifyCustomEncode(std::span<const OPJ_FLOAT32> matrix,
                        OPJ_UINT32 num_comps, OPJ_SIZE_T sample_count,
                        OPJ_UINT32 is_signed,
                        std::span<const std::vector<OPJ_INT32>> inputs) {
  ASSERT_EQ(static_cast<std::size_t>(num_comps), inputs.size());
  ASSERT_EQ(static_cast<std::size_t>(num_comps) * num_comps, matrix.size());

  std::vector<OPJ_FLOAT32> mutable_matrix(matrix.begin(), matrix.end());
  std::vector<AlignedBuffer<OPJ_INT32>> comps;
  std::vector<OPJ_BYTE*> data_ptrs;
  ASSERT_NO_FATAL_FAILURE(PrepareCustomBuffers<OPJ_INT32>(
      inputs, sample_count, kIntSentinel, &comps, &data_ptrs));

  ASSERT_EQ(OPJ_TRUE,
            opj_mct_encode_custom(
                reinterpret_cast<OPJ_BYTE*>(mutable_matrix.data()),
                sample_count, data_ptrs.data(), num_comps, is_signed));

  for (OPJ_UINT32 c = 0; c < num_comps; ++c) {
    EXPECT_EQ(reinterpret_cast<OPJ_BYTE*>(comps[c].data() + sample_count),
              data_ptrs[c])
        << "comp=" << c;
    EXPECT_THAT(comps[c].guard(), Each(Eq(kIntSentinel))) << "comp=" << c;
  }

  // Pins upstream Q13 coefficient truncation (not rounding) and
  // opj_int_fix_mul ((prod + 4096) >> 13).
  std::vector<OPJ_INT32> q13_matrix(matrix.size());
  for (std::size_t i = 0; i < matrix.size(); ++i) {
    q13_matrix[i] =
        static_cast<OPJ_INT32>(matrix[i] * static_cast<OPJ_FLOAT32>(1 << 13));
  }
  for (OPJ_SIZE_T s = 0; s < sample_count; ++s) {
    for (OPJ_UINT32 j = 0; j < num_comps; ++j) {
      OPJ_INT32 expected = 0;
      for (OPJ_UINT32 k = 0; k < num_comps; ++k) {
        const OPJ_INT64 prod =
            static_cast<OPJ_INT64>(q13_matrix[j * num_comps + k]) *
            static_cast<OPJ_INT64>(inputs[k][s]);
        expected += static_cast<OPJ_INT32>((prod + 4096) >> 13);
      }
      EXPECT_EQ(expected, comps[j].values()[s])
          << "sample=" << s << ", comp=" << j;
    }
  }
}

void VerifyCustomDecode(std::span<const OPJ_FLOAT32> matrix,
                        OPJ_UINT32 num_comps, OPJ_SIZE_T sample_count,
                        OPJ_UINT32 is_signed,
                        std::span<const std::vector<OPJ_FLOAT32>> inputs) {
  ASSERT_EQ(static_cast<std::size_t>(num_comps), inputs.size());
  ASSERT_EQ(static_cast<std::size_t>(num_comps) * num_comps, matrix.size());

  std::vector<OPJ_FLOAT32> mutable_matrix(matrix.begin(), matrix.end());
  std::vector<AlignedBuffer<OPJ_FLOAT32>> comps;
  std::vector<OPJ_BYTE*> data_ptrs;
  ASSERT_NO_FATAL_FAILURE(PrepareCustomBuffers<OPJ_FLOAT32>(
      inputs, sample_count, kFloatSentinel, &comps, &data_ptrs));

  ASSERT_EQ(OPJ_TRUE,
            opj_mct_decode_custom(
                reinterpret_cast<OPJ_BYTE*>(mutable_matrix.data()),
                sample_count, data_ptrs.data(), num_comps, is_signed));

  for (OPJ_UINT32 c = 0; c < num_comps; ++c) {
    EXPECT_EQ(reinterpret_cast<OPJ_BYTE*>(comps[c].data() + sample_count),
              data_ptrs[c])
        << "comp=" << c;
    EXPECT_THAT(comps[c].guard(), Each(FloatEq(kFloatSentinel)))
        << "comp=" << c;
  }

  for (OPJ_SIZE_T s = 0; s < sample_count; ++s) {
    for (OPJ_UINT32 j = 0; j < num_comps; ++j) {
      OPJ_FLOAT32 expected = 0.0f;
      for (OPJ_UINT32 k = 0; k < num_comps; ++k) {
        expected += matrix[j * num_comps + k] * inputs[k][s];
      }
      EXPECT_NEAR(expected, comps[j].values()[s], kDirectFloatTolerance)
          << "sample=" << s << ", comp=" << j;
    }
  }
}

TEST(MctTest, ReversibleNormsAndGetNormMatchConstants) {
  const OPJ_FLOAT64* norms = opj_mct_get_mct_norms();
  ASSERT_NE(nullptr, norms);
  // Returns a pointer to the static table, not a heap copy.
  EXPECT_EQ(norms, opj_mct_get_mct_norms());

  constexpr std::array<OPJ_FLOAT64, 3> kExpectedNorms = {1.732, 0.8292, 0.8292};
  for (OPJ_UINT32 i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(kExpectedNorms[i], norms[i]) << "compno=" << i;
    EXPECT_DOUBLE_EQ(kExpectedNorms[i], opj_mct_getnorm(i)) << "compno=" << i;
  }
}

TEST(MctTest, IrreversibleNormsAndGetNormRealMatchConstants) {
  const OPJ_FLOAT64* norms_real = opj_mct_get_mct_norms_real();
  ASSERT_NE(nullptr, norms_real);
  EXPECT_EQ(norms_real, opj_mct_get_mct_norms_real());

  constexpr std::array<OPJ_FLOAT64, 3> kExpectedNorms = {1.732, 1.805, 1.573};
  for (OPJ_UINT32 i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(kExpectedNorms[i], norms_real[i]) << "compno=" << i;
    EXPECT_DOUBLE_EQ(kExpectedNorms[i], opj_mct_getnorm_real(i))
        << "compno=" << i;
  }

  // Verify the JPEG 2000 irreversible ICT synthesis matrix column L2 norms
  // agree with the tabulated 4-digit constants in opj_mct_norms_real.
  std::array<OPJ_FLOAT32, 9> ict_decoding_matrix = {
      1.0f, 0.0f, 1.402f, 1.0f, -0.34413f, -0.71414f, 1.0f, 1.772f, 0.0f,
  };
  std::array<OPJ_FLOAT64, 3> calculated{};
  opj_calculate_norms(calculated.data(), 3, ict_decoding_matrix.data());
  for (OPJ_UINT32 i = 0; i < 3; ++i) {
    EXPECT_NEAR(norms_real[i], calculated[i], 1e-3) << "compno=" << i;
  }
}

TEST(MctTest, ReversibleZeroLengthIsNoOp) {
  opj_mct_encode(nullptr, nullptr, nullptr, 0);
  opj_mct_decode(nullptr, nullptr, nullptr, 0);

  AlignedBuffer<OPJ_INT32> c0(0, kIntSentinel);
  AlignedBuffer<OPJ_INT32> c1(0, kIntSentinel);
  AlignedBuffer<OPJ_INT32> c2(0, kIntSentinel);
  ASSERT_NE(nullptr, c0.data());
  ASSERT_NE(nullptr, c1.data());
  ASSERT_NE(nullptr, c2.data());

  opj_mct_encode(c0.data(), c1.data(), c2.data(), 0);
  EXPECT_THAT(c0.guard(), Each(Eq(kIntSentinel)));
  EXPECT_THAT(c1.guard(), Each(Eq(kIntSentinel)));
  EXPECT_THAT(c2.guard(), Each(Eq(kIntSentinel)));

  opj_mct_decode(c0.data(), c1.data(), c2.data(), 0);
  EXPECT_THAT(c0.guard(), Each(Eq(kIntSentinel)));
  EXPECT_THAT(c1.guard(), Each(Eq(kIntSentinel)));
  EXPECT_THAT(c2.guard(), Each(Eq(kIntSentinel)));
}

TEST(MctTest, ReversibleKnownVectorsTableDriven) {
  // 1. Verify each vector individually with n = 1 (pure scalar tail path).
  for (const RctVectorCase& tc : kRctVectorCases) {
    SCOPED_TRACE(tc.name);
    AlignedBuffer<OPJ_INT32> c0(std::span<const OPJ_INT32>(&tc.r, 1),
                                kIntSentinel);
    AlignedBuffer<OPJ_INT32> c1(std::span<const OPJ_INT32>(&tc.g, 1),
                                kIntSentinel);
    AlignedBuffer<OPJ_INT32> c2(std::span<const OPJ_INT32>(&tc.b, 1),
                                kIntSentinel);
    ASSERT_NE(nullptr, c0.data());
    ASSERT_NE(nullptr, c1.data());
    ASSERT_NE(nullptr, c2.data());

    opj_mct_encode(c0.data(), c1.data(), c2.data(), 1);
    EXPECT_THAT(c0.values(), ElementsAre(tc.expected_y));
    EXPECT_THAT(c1.values(), ElementsAre(tc.expected_u));
    EXPECT_THAT(c2.values(), ElementsAre(tc.expected_v));

    opj_mct_decode(c0.data(), c1.data(), c2.data(), 1);
    EXPECT_THAT(c0.values(), ElementsAre(tc.r));
    EXPECT_THAT(c1.values(), ElementsAre(tc.g));
    EXPECT_THAT(c2.values(), ElementsAre(tc.b));
    EXPECT_THAT(c0.guard(), Each(Eq(kIntSentinel)));
    EXPECT_THAT(c1.guard(), Each(Eq(kIntSentinel)));
    EXPECT_THAT(c2.guard(), Each(Eq(kIntSentinel)));
  }

  // 2. Pack all 10 vectors together to exercise SSE2 (2x4) + scalar tail (2).
  constexpr std::size_t n = kRctVectorCases.size();
  AlignedBuffer<OPJ_INT32> c0(n, kIntSentinel);
  AlignedBuffer<OPJ_INT32> c1(n, kIntSentinel);
  AlignedBuffer<OPJ_INT32> c2(n, kIntSentinel);
  ASSERT_NE(nullptr, c0.data());
  ASSERT_NE(nullptr, c1.data());
  ASSERT_NE(nullptr, c2.data());
  for (std::size_t i = 0; i < n; ++i) {
    c0.data()[i] = kRctVectorCases[i].r;
    c1.data()[i] = kRctVectorCases[i].g;
    c2.data()[i] = kRctVectorCases[i].b;
  }

  opj_mct_encode(c0.data(), c1.data(), c2.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(kRctVectorCases[i].expected_y, c0.values()[i]) << "i=" << i;
    EXPECT_EQ(kRctVectorCases[i].expected_u, c1.values()[i]) << "i=" << i;
    EXPECT_EQ(kRctVectorCases[i].expected_v, c2.values()[i]) << "i=" << i;
  }

  opj_mct_decode(c0.data(), c1.data(), c2.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(kRctVectorCases[i].r, c0.values()[i]) << "i=" << i;
    EXPECT_EQ(kRctVectorCases[i].g, c1.values()[i]) << "i=" << i;
    EXPECT_EQ(kRctVectorCases[i].b, c2.values()[i]) << "i=" << i;
  }
  EXPECT_THAT(c0.guard(), Each(Eq(kIntSentinel)));
  EXPECT_THAT(c1.guard(), Each(Eq(kIntSentinel)));
  EXPECT_THAT(c2.guard(), Each(Eq(kIntSentinel)));
}

TEST(MctTest, ReversibleEncodeAndDecodeCoverSimdAndTailSizes) {
  for (const OPJ_SIZE_T n : kSimdAndTailSizes) {
    SCOPED_TRACE(::testing::Message() << "n=" << n);
    const std::vector<std::vector<OPJ_INT32>> orig =
        MakeIntComponentInputs(3, n);
    AlignedBuffer<OPJ_INT32> c0(orig[0], kIntSentinel);
    AlignedBuffer<OPJ_INT32> c1(orig[1], kIntSentinel);
    AlignedBuffer<OPJ_INT32> c2(orig[2], kIntSentinel);
    ASSERT_NE(nullptr, c0.data());
    ASSERT_NE(nullptr, c1.data());
    ASSERT_NE(nullptr, c2.data());

    opj_mct_encode(c0.data(), c1.data(), c2.data(), n);
    for (OPJ_SIZE_T i = 0; i < n; ++i) {
      const OPJ_INT32 r = orig[0][i];
      const OPJ_INT32 g = orig[1][i];
      const OPJ_INT32 b = orig[2][i];
      const OPJ_INT32 expected_y = (r + (g * 2) + b) >> 2;
      EXPECT_EQ(expected_y, c0.values()[i]) << "i=" << i;
      EXPECT_EQ(b - g, c1.values()[i]) << "i=" << i;
      EXPECT_EQ(r - g, c2.values()[i]) << "i=" << i;
    }
    EXPECT_THAT(c0.guard(), Each(Eq(kIntSentinel)));
    EXPECT_THAT(c1.guard(), Each(Eq(kIntSentinel)));
    EXPECT_THAT(c2.guard(), Each(Eq(kIntSentinel)));

    opj_mct_decode(c0.data(), c1.data(), c2.data(), n);
    for (OPJ_SIZE_T i = 0; i < n; ++i) {
      EXPECT_EQ(orig[0][i], c0.values()[i]) << "i=" << i;
      EXPECT_EQ(orig[1][i], c1.values()[i]) << "i=" << i;
      EXPECT_EQ(orig[2][i], c2.values()[i]) << "i=" << i;
    }
    EXPECT_THAT(c0.guard(), Each(Eq(kIntSentinel)));
    EXPECT_THAT(c1.guard(), Each(Eq(kIntSentinel)));
    EXPECT_THAT(c2.guard(), Each(Eq(kIntSentinel)));
  }
}

TEST(MctTest, IrreversibleZeroLengthIsNoOp) {
  opj_mct_encode_real(nullptr, nullptr, nullptr, 0);
  opj_mct_decode_real(nullptr, nullptr, nullptr, 0);

  AlignedBuffer<OPJ_FLOAT32> c0(0, kFloatSentinel);
  AlignedBuffer<OPJ_FLOAT32> c1(0, kFloatSentinel);
  AlignedBuffer<OPJ_FLOAT32> c2(0, kFloatSentinel);
  ASSERT_NE(nullptr, c0.data());
  ASSERT_NE(nullptr, c1.data());
  ASSERT_NE(nullptr, c2.data());

  opj_mct_encode_real(c0.data(), c1.data(), c2.data(), 0);
  EXPECT_THAT(c0.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(c1.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(c2.guard(), Each(FloatEq(kFloatSentinel)));

  opj_mct_decode_real(c0.data(), c1.data(), c2.data(), 0);
  EXPECT_THAT(c0.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(c1.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(c2.guard(), Each(FloatEq(kFloatSentinel)));
}

TEST(MctTest, IrreversibleKnownBasisAndMixedVectors) {
  // 1. Forward ICT basis and mixed vectors.
  constexpr std::size_t n_fwd = kIctForwardCases.size();
  AlignedBuffer<OPJ_FLOAT32> f0(n_fwd, kFloatSentinel);
  AlignedBuffer<OPJ_FLOAT32> f1(n_fwd, kFloatSentinel);
  AlignedBuffer<OPJ_FLOAT32> f2(n_fwd, kFloatSentinel);
  ASSERT_NE(nullptr, f0.data());
  ASSERT_NE(nullptr, f1.data());
  ASSERT_NE(nullptr, f2.data());
  for (std::size_t i = 0; i < n_fwd; ++i) {
    f0.data()[i] = kIctForwardCases[i].c0;
    f1.data()[i] = kIctForwardCases[i].c1;
    f2.data()[i] = kIctForwardCases[i].c2;
  }

  opj_mct_encode_real(f0.data(), f1.data(), f2.data(), n_fwd);
  for (std::size_t i = 0; i < n_fwd; ++i) {
    SCOPED_TRACE(kIctForwardCases[i].name);
    EXPECT_NEAR(kIctForwardCases[i].expected_0, f0.values()[i],
                kDirectFloatTolerance);
    EXPECT_NEAR(kIctForwardCases[i].expected_1, f1.values()[i],
                kDirectFloatTolerance);
    EXPECT_NEAR(kIctForwardCases[i].expected_2, f2.values()[i],
                kDirectFloatTolerance);
  }
  EXPECT_THAT(f0.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(f1.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(f2.guard(), Each(FloatEq(kFloatSentinel)));

  // 2. Inverse ICT basis and mixed vectors.
  constexpr std::size_t n_inv = kIctInverseCases.size();
  AlignedBuffer<OPJ_FLOAT32> d0(n_inv, kFloatSentinel);
  AlignedBuffer<OPJ_FLOAT32> d1(n_inv, kFloatSentinel);
  AlignedBuffer<OPJ_FLOAT32> d2(n_inv, kFloatSentinel);
  ASSERT_NE(nullptr, d0.data());
  ASSERT_NE(nullptr, d1.data());
  ASSERT_NE(nullptr, d2.data());
  for (std::size_t i = 0; i < n_inv; ++i) {
    d0.data()[i] = kIctInverseCases[i].c0;
    d1.data()[i] = kIctInverseCases[i].c1;
    d2.data()[i] = kIctInverseCases[i].c2;
  }

  opj_mct_decode_real(d0.data(), d1.data(), d2.data(), n_inv);
  for (std::size_t i = 0; i < n_inv; ++i) {
    SCOPED_TRACE(kIctInverseCases[i].name);
    EXPECT_NEAR(kIctInverseCases[i].expected_0, d0.values()[i],
                kDirectFloatTolerance);
    EXPECT_NEAR(kIctInverseCases[i].expected_1, d1.values()[i],
                kDirectFloatTolerance);
    EXPECT_NEAR(kIctInverseCases[i].expected_2, d2.values()[i],
                kDirectFloatTolerance);
  }
  EXPECT_THAT(d0.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(d1.guard(), Each(FloatEq(kFloatSentinel)));
  EXPECT_THAT(d2.guard(), Each(FloatEq(kFloatSentinel)));
}

TEST(MctTest, IrreversibleEncodeAndDecodeCoverSimdAndTailSizes) {
  for (const OPJ_SIZE_T n : kSimdAndTailSizes) {
    SCOPED_TRACE(::testing::Message() << "n=" << n);
    const std::vector<std::vector<OPJ_FLOAT32>> orig =
        MakeFloatComponentInputs(3, n);
    AlignedBuffer<OPJ_FLOAT32> c0(orig[0], kFloatSentinel);
    AlignedBuffer<OPJ_FLOAT32> c1(orig[1], kFloatSentinel);
    AlignedBuffer<OPJ_FLOAT32> c2(orig[2], kFloatSentinel);
    ASSERT_NE(nullptr, c0.data());
    ASSERT_NE(nullptr, c1.data());
    ASSERT_NE(nullptr, c2.data());

    opj_mct_encode_real(c0.data(), c1.data(), c2.data(), n);
    for (OPJ_SIZE_T i = 0; i < n; ++i) {
      const OPJ_FLOAT32 r = orig[0][i];
      const OPJ_FLOAT32 g = orig[1][i];
      const OPJ_FLOAT32 b = orig[2][i];
      EXPECT_NEAR(0.299f * r + 0.587f * g + 0.114f * b, c0.values()[i],
                  kDirectFloatTolerance)
          << "i=" << i;
      EXPECT_NEAR(-0.16875f * r - 0.331260f * g + 0.5f * b, c1.values()[i],
                  kDirectFloatTolerance)
          << "i=" << i;
      EXPECT_NEAR(0.5f * r - 0.41869f * g - 0.08131f * b, c2.values()[i],
                  kDirectFloatTolerance)
          << "i=" << i;
    }
    EXPECT_THAT(c0.guard(), Each(FloatEq(kFloatSentinel)));
    EXPECT_THAT(c1.guard(), Each(FloatEq(kFloatSentinel)));
    EXPECT_THAT(c2.guard(), Each(FloatEq(kFloatSentinel)));

    opj_mct_decode_real(c0.data(), c1.data(), c2.data(), n);
    for (OPJ_SIZE_T i = 0; i < n; ++i) {
      EXPECT_NEAR(orig[0][i], c0.values()[i], kRoundTripFloatTolerance)
          << "i=" << i;
      EXPECT_NEAR(orig[1][i], c1.values()[i], kRoundTripFloatTolerance)
          << "i=" << i;
      EXPECT_NEAR(orig[2][i], c2.values()[i], kRoundTripFloatTolerance)
          << "i=" << i;
    }
    EXPECT_THAT(c0.guard(), Each(FloatEq(kFloatSentinel)));
    EXPECT_THAT(c1.guard(), Each(FloatEq(kFloatSentinel)));
    EXPECT_THAT(c2.guard(), Each(FloatEq(kFloatSentinel)));
  }
}

TEST(MctTest, CustomZeroComponentsReturnsFalse) {
  // With pNbComp == 0, opj_malloc(0) returns nullptr (see opj_malloc.c and
  // alloc_fault_injector.cc), so both custom functions fail safely before
  // dereferencing pCodingdata/pDecodingData or pData.
  EXPECT_EQ(OPJ_FALSE, opj_mct_encode_custom(nullptr, 0, nullptr, 0, 0));
  EXPECT_EQ(OPJ_FALSE, opj_mct_decode_custom(nullptr, 0, nullptr, 0, 0));

  std::array<OPJ_FLOAT32, 1> matrix = {1.0f};
  std::array<OPJ_INT32, 4> int_buf = {10, 20, 30, 40};
  std::array<OPJ_FLOAT32, 4> float_buf = {1.0f, 2.0f, 3.0f, 4.0f};
  std::array<OPJ_BYTE*, 1> int_ptrs = {
      reinterpret_cast<OPJ_BYTE*>(int_buf.data())};
  std::array<OPJ_BYTE*, 1> float_ptrs = {
      reinterpret_cast<OPJ_BYTE*>(float_buf.data())};

  EXPECT_EQ(OPJ_FALSE,
            opj_mct_encode_custom(reinterpret_cast<OPJ_BYTE*>(matrix.data()), 4,
                                  int_ptrs.data(), 0, 1));
  EXPECT_EQ(reinterpret_cast<OPJ_BYTE*>(int_buf.data()), int_ptrs[0]);
  EXPECT_THAT(int_buf, ElementsAre(10, 20, 30, 40));

  EXPECT_EQ(OPJ_FALSE,
            opj_mct_decode_custom(reinterpret_cast<OPJ_BYTE*>(matrix.data()), 4,
                                  float_ptrs.data(), 0, 1));
  EXPECT_EQ(reinterpret_cast<OPJ_BYTE*>(float_buf.data()), float_ptrs[0]);
  EXPECT_THAT(float_buf, ElementsAre(1.0f, 2.0f, 3.0f, 4.0f));
}

TEST(MctTest, CustomZeroSamplesIsNoOpAndPreservesPointers) {
  const std::vector<std::vector<OPJ_INT32>> empty_int(3);
  const std::vector<std::vector<OPJ_FLOAT32>> empty_float(3);
  for (const OPJ_UINT32 is_signed : {0U, 1U}) {
    SCOPED_TRACE(::testing::Message() << "is_signed=" << is_signed);
    ASSERT_NO_FATAL_FAILURE(
        VerifyCustomEncode(kAsymmetric3x3, 3, 0, is_signed, empty_int));
    ASSERT_NO_FATAL_FAILURE(
        VerifyCustomDecode(kAsymmetric3x3, 3, 0, is_signed, empty_float));
  }
}

TEST(MctTest, CustomEncodeMatchesQ13FixedPointAcrossComponentsAndSignFlags) {
  constexpr std::array<OPJ_FLOAT32, 1> kScale1x1 = {-2.5f};

  for (const OPJ_UINT32 is_signed : {0U, 1U}) {
    SCOPED_TRACE(::testing::Message() << "is_signed=" << is_signed);

    // 1 component.
    const std::vector<std::vector<OPJ_INT32>> inputs_1c = {
        {0, 10, -4, 123, -456}};
    ASSERT_NO_FATAL_FAILURE(
        VerifyCustomEncode(kScale1x1, 1, 5, is_signed, inputs_1c));

    // 3 and 4 components across single, small, and multi-iteration sample
    // sizes.
    for (const OPJ_SIZE_T n : {OPJ_SIZE_T{1}, OPJ_SIZE_T{5}, OPJ_SIZE_T{35}}) {
      SCOPED_TRACE(::testing::Message() << "n=" << n);
      const std::vector<std::vector<OPJ_INT32>> inputs_3c =
          MakeIntComponentInputs(3, n);
      ASSERT_NO_FATAL_FAILURE(
          VerifyCustomEncode(kAsymmetric3x3, 3, n, is_signed, inputs_3c));
      ASSERT_NO_FATAL_FAILURE(
          VerifyCustomEncode(kIct3x3, 3, n, is_signed, inputs_3c));

      const std::vector<std::vector<OPJ_INT32>> inputs_4c =
          MakeIntComponentInputs(4, n);
      ASSERT_NO_FATAL_FAILURE(
          VerifyCustomEncode(kAsymmetric4x4, 4, n, is_signed, inputs_4c));
    }
  }
}

TEST(MctTest, CustomDecodeMatchesFloatMultiplyAcrossComponentsAndSignFlags) {
  constexpr std::array<OPJ_FLOAT32, 1> kScale1x1 = {-2.5f};

  for (const OPJ_UINT32 is_signed : {0U, 1U}) {
    SCOPED_TRACE(::testing::Message() << "is_signed=" << is_signed);

    // 1 component.
    const std::vector<std::vector<OPJ_FLOAT32>> inputs_1c = {
        {0.0f, 10.0f, -4.0f, 12.25f, -45.5f}};
    ASSERT_NO_FATAL_FAILURE(
        VerifyCustomDecode(kScale1x1, 1, 5, is_signed, inputs_1c));

    // 3 and 4 components across single, small, and multi-iteration sample
    // sizes.
    for (const OPJ_SIZE_T n : {OPJ_SIZE_T{1}, OPJ_SIZE_T{5}, OPJ_SIZE_T{35}}) {
      SCOPED_TRACE(::testing::Message() << "n=" << n);
      const std::vector<std::vector<OPJ_FLOAT32>> inputs_3c =
          MakeFloatComponentInputs(3, n);
      ASSERT_NO_FATAL_FAILURE(
          VerifyCustomDecode(kAsymmetric3x3, 3, n, is_signed, inputs_3c));
      ASSERT_NO_FATAL_FAILURE(
          VerifyCustomDecode(kIct3x3, 3, n, is_signed, inputs_3c));

      const std::vector<std::vector<OPJ_FLOAT32>> inputs_4c =
          MakeFloatComponentInputs(4, n);
      ASSERT_NO_FATAL_FAILURE(
          VerifyCustomDecode(kAsymmetric4x4, 4, n, is_signed, inputs_4c));
    }
  }
}

TEST(MctTest, CustomInvertibleTransformRoundTripsThreeAndFourComponents) {
  constexpr OPJ_SIZE_T kSamples = 35;

  // 1. Float forward + inverse via opj_matrix_inversion_f for 3x3 and 4x4.
  auto verify_float_round_trip = [&](std::span<const OPJ_FLOAT32> forward,
                                     OPJ_UINT32 num_comps) {
    std::vector<OPJ_FLOAT32> src_copy(forward.begin(), forward.end());
    std::vector<OPJ_FLOAT32> inverse(forward.size(), 0.0f);
    ASSERT_EQ(OPJ_TRUE, opj_matrix_inversion_f(src_copy.data(), inverse.data(),
                                               num_comps));

    const std::vector<std::vector<OPJ_FLOAT32>> orig =
        MakeFloatComponentInputs(num_comps, kSamples);
    std::vector<AlignedBuffer<OPJ_FLOAT32>> work;
    std::vector<OPJ_BYTE*> fwd_ptrs;
    ASSERT_NO_FATAL_FAILURE(PrepareCustomBuffers<OPJ_FLOAT32>(
        orig, kSamples, kFloatSentinel, &work, &fwd_ptrs));
    std::vector<OPJ_BYTE*> inv_ptrs = fwd_ptrs;

    std::vector<OPJ_FLOAT32> fwd_mat(forward.begin(), forward.end());
    ASSERT_EQ(OPJ_TRUE,
              opj_mct_decode_custom(reinterpret_cast<OPJ_BYTE*>(fwd_mat.data()),
                                    kSamples, fwd_ptrs.data(), num_comps, 1));
    ASSERT_EQ(OPJ_TRUE,
              opj_mct_decode_custom(reinterpret_cast<OPJ_BYTE*>(inverse.data()),
                                    kSamples, inv_ptrs.data(), num_comps, 1));

    for (OPJ_UINT32 c = 0; c < num_comps; ++c) {
      for (OPJ_SIZE_T i = 0; i < kSamples; ++i) {
        EXPECT_NEAR(orig[c][i], work[c].values()[i], kDirectFloatTolerance)
            << "comp=" << c << ", i=" << i;
      }
      EXPECT_THAT(work[c].guard(), Each(FloatEq(kFloatSentinel)));
    }
  };

  ASSERT_NO_FATAL_FAILURE(verify_float_round_trip(kAsymmetric3x3, 3));
  ASSERT_NO_FATAL_FAILURE(verify_float_round_trip(kAsymmetric4x4, 4));

  // 2. Exact dyadic 4x4 encode_custom (int32) -> float -> decode_custom.
  std::vector<std::vector<OPJ_INT32>> dyadic_orig =
      MakeIntComponentInputs(4, kSamples);
  for (OPJ_SIZE_T i = 0; i < kSamples; ++i) {
    // Keep component 2 even so scaling by -0.5f is exact in integer Q13.
    dyadic_orig[2][i] &= ~1;
  }
  std::vector<AlignedBuffer<OPJ_INT32>> int_work;
  std::vector<OPJ_BYTE*> enc_ptrs;
  ASSERT_NO_FATAL_FAILURE(PrepareCustomBuffers<OPJ_INT32>(
      dyadic_orig, kSamples, kIntSentinel, &int_work, &enc_ptrs));

  std::vector<OPJ_FLOAT32> perm_fwd(kDyadicPerm4x4.begin(),
                                    kDyadicPerm4x4.end());
  ASSERT_EQ(OPJ_TRUE,
            opj_mct_encode_custom(reinterpret_cast<OPJ_BYTE*>(perm_fwd.data()),
                                  kSamples, enc_ptrs.data(), 4, 1));

  std::vector<std::vector<OPJ_FLOAT32>> converted(
      4, std::vector<OPJ_FLOAT32>(kSamples));
  for (OPJ_UINT32 c = 0; c < 4; ++c) {
    for (OPJ_SIZE_T i = 0; i < kSamples; ++i) {
      converted[c][i] = static_cast<OPJ_FLOAT32>(int_work[c].values()[i]);
    }
  }
  std::vector<AlignedBuffer<OPJ_FLOAT32>> float_work;
  std::vector<OPJ_BYTE*> dec_ptrs;
  ASSERT_NO_FATAL_FAILURE(PrepareCustomBuffers<OPJ_FLOAT32>(
      converted, kSamples, kFloatSentinel, &float_work, &dec_ptrs));

  std::vector<OPJ_FLOAT32> perm_inv(kDyadicPerm4x4Inv.begin(),
                                    kDyadicPerm4x4Inv.end());
  ASSERT_EQ(OPJ_TRUE,
            opj_mct_decode_custom(reinterpret_cast<OPJ_BYTE*>(perm_inv.data()),
                                  kSamples, dec_ptrs.data(), 4, 1));

  for (OPJ_UINT32 c = 0; c < 4; ++c) {
    for (OPJ_SIZE_T i = 0; i < kSamples; ++i) {
      EXPECT_FLOAT_EQ(static_cast<OPJ_FLOAT32>(dyadic_orig[c][i]),
                      float_work[c].values()[i])
          << "comp=" << c << ", i=" << i;
    }
    EXPECT_THAT(float_work[c].guard(), Each(FloatEq(kFloatSentinel)));
  }
}

TEST(MctTest, CalculateNormsHandlesZeroOneThreeAndFourComponents) {
  // 1. Zero components is a safe no-op.
  opj_calculate_norms(nullptr, 0, nullptr);
  std::array<OPJ_FLOAT64, 2> norms_zero = {kDoubleSentinel, kDoubleSentinel};
  std::array<OPJ_FLOAT32, 1> dummy_matrix = {42.0f};
  opj_calculate_norms(norms_zero.data(), 0, dummy_matrix.data());
  EXPECT_THAT(norms_zero, ElementsAre(kDoubleSentinel, kDoubleSentinel));

  // 2. Single component (1x1) norms equal |matrix[0]|.
  std::array<OPJ_FLOAT64, 2> norms_1 = {kDoubleSentinel, kDoubleSentinel};
  std::array<OPJ_FLOAT32, 1> mat_1_zero = {0.0f};
  opj_calculate_norms(norms_1.data(), 1, mat_1_zero.data());
  EXPECT_DOUBLE_EQ(0.0, norms_1[0]);
  EXPECT_DOUBLE_EQ(kDoubleSentinel, norms_1[1]);

  std::array<OPJ_FLOAT32, 1> mat_1_neg = {-3.5f};
  opj_calculate_norms(norms_1.data(), 1, mat_1_neg.data());
  EXPECT_DOUBLE_EQ(3.5, norms_1[0]);
  EXPECT_DOUBLE_EQ(kDoubleSentinel, norms_1[1]);

  // 3. Asymmetric 3x3 matrix with Pythagorean columns distinguishes column
  //    norms (5, 13, 3) from row norms.
  std::array<OPJ_FLOAT32, 9> mat_3 = {
      3.0f, 0.0f, 1.0f, 4.0f, -5.0f, 2.0f, 0.0f, 12.0f, 2.0f,
  };
  std::array<OPJ_FLOAT64, 4> norms_3 = {kDoubleSentinel, kDoubleSentinel,
                                        kDoubleSentinel, kDoubleSentinel};
  opj_calculate_norms(norms_3.data(), 3, mat_3.data());
  EXPECT_DOUBLE_EQ(5.0, norms_3[0]);
  EXPECT_DOUBLE_EQ(13.0, norms_3[1]);
  EXPECT_DOUBLE_EQ(3.0, norms_3[2]);
  EXPECT_DOUBLE_EQ(kDoubleSentinel, norms_3[3]);

  // 4. Asymmetric 4x4 matrix with Pythagorean columns (3, 5, 7, 9).
  std::array<OPJ_FLOAT32, 16> mat_4 = {
      1.0f, 0.0f, 2.0f, 1.0f, 2.0f, -3.0f, 0.0f, -4.0f,
      2.0f, 0.0f, 3.0f, 8.0f, 0.0f, 4.0f,  6.0f, 0.0f,
  };
  std::array<OPJ_FLOAT64, 5> norms_4 = {kDoubleSentinel, kDoubleSentinel,
                                        kDoubleSentinel, kDoubleSentinel,
                                        kDoubleSentinel};
  opj_calculate_norms(norms_4.data(), 4, mat_4.data());
  EXPECT_DOUBLE_EQ(3.0, norms_4[0]);
  EXPECT_DOUBLE_EQ(5.0, norms_4[1]);
  EXPECT_DOUBLE_EQ(7.0, norms_4[2]);
  EXPECT_DOUBLE_EQ(9.0, norms_4[3]);
  EXPECT_DOUBLE_EQ(kDoubleSentinel, norms_4[4]);
}

}  // namespace
}  // namespace openjpeg2
