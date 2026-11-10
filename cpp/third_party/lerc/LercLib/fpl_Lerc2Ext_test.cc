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
// Unit tests for LercNS::LosslessFPCompression and fpl_Lerc2Ext helpers
// (third_party/lerc/LercLib/fpl_Lerc2Ext.cpp).
//
// Note: Built with -fexceptions and -use_header_modules because lerc_assert()
// throws `const char*`.
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_Lerc2Ext.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_Lerc2Ext.cpp

#include "third_party/lerc/LercLib/fpl_Lerc2Ext.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/fpl_Predictor.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

// Global-namespace helpers defined in fpl_Lerc2Ext.cpp.
void lerc_assert(bool v);
unsigned char* restoreSequence(unsigned char* data, std::size_t size, int level,
                               bool make_copy);
int getBestLevel(const unsigned char* ptr, std::size_t size,
                 int max_delta_order);

namespace lerc {
namespace {

using ::LercNS::LosslessFPCompression;
using ::LercNS::Predictor;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

template <typename T>
std::vector<unsigned char> EncodeSurface(const std::vector<T>& data,
                                         bool is_double, int cols, int rows,
                                         int depth) {
  LosslessFPCompression codec;
  if (!codec.ComputeHuffmanCodesFlt(data.data(), is_double, cols, rows,
                                    depth)) {
    ADD_FAILURE() << "ComputeHuffmanCodesFlt failed";
    return {};
  }
  const int expected_len = codec.compressedLength();
  if (expected_len <= 1) {
    ADD_FAILURE() << "Unexpected compressedLength=" << expected_len;
    return {};
  }

  std::vector<unsigned char> blob(static_cast<std::size_t>(expected_len), 0);
  unsigned char* dst = blob.data();
  if (!codec.EncodeHuffmanFlt(&dst)) {
    ADD_FAILURE() << "EncodeHuffmanFlt failed";
    return {};
  }
  EXPECT_EQ(expected_len, static_cast<int>(dst - blob.data()));
  return blob;
}

template <typename T>
void ExpectBitExactRoundTrip(const std::vector<T>& input, bool is_double,
                             int cols, int rows, int depth,
                             unsigned char* out_pred_code = nullptr) {
  const std::vector<unsigned char> blob =
      EncodeSurface(input, is_double, cols, rows, depth);
  ASSERT_FALSE(blob.empty());
  if (out_pred_code != nullptr) {
    *out_pred_code = blob[0];
  }

  std::vector<T> decoded(input.size());
  const unsigned char* src = blob.data();
  std::size_t remaining = blob.size();
  ASSERT_TRUE(LosslessFPCompression::DecodeHuffmanFlt(
      &src, remaining, decoded.data(), is_double, cols, rows, depth));
  EXPECT_EQ(0U, remaining);
  EXPECT_EQ(blob.data() + blob.size(), src);

  using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
  std::vector<Bits> expected_bits(input.size());
  std::vector<Bits> decoded_bits(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    expected_bits[i] = std::bit_cast<Bits>(input[i]);
    decoded_bits[i] = std::bit_cast<Bits>(decoded[i]);
  }
  EXPECT_THAT(decoded_bits, ElementsAreArray(expected_bits));
}

TEST(FplLerc2ExtTest, FloatAndDoubleRoundTripAcrossPredictorsAndMultiBlock) {
  // Use 128x200 (> 8192 elements) to exercise multi-block TestBlock sampling
  // and multi-snippet getBestLevel2 across all three predictor families.
  // MSan safety on HUFFMAN_NORMAL planes here depends on upstream
  // EncodeHuffman's entropy heuristic and bit-stream termination on this data.
  constexpr int kCols = 128;
  constexpr int kRows = 200;
  constexpr int kCount = kCols * kRows;

  // 1. Pseudo-random discrete values favor PREDICTOR_NONE (raw stats smallest).
  std::vector<float> noisy_f(kCount);
  std::vector<double> noisy_d(kCount);
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kCols; ++c) {
      const double v = static_cast<double>((r * 131 + c * 977) % 17) * 0.25;
      noisy_f[r * kCols + c] = static_cast<float>(v);
      noisy_d[r * kCols + c] = v;
    }
  }
  unsigned char pred_none_f = 255;
  unsigned char pred_none_d = 255;
  {
    SCOPED_TRACE("PREDICTOR_NONE float");
    ExpectBitExactRoundTrip(noisy_f, false, kCols, kRows, 1, &pred_none_f);
  }
  {
    SCOPED_TRACE("PREDICTOR_NONE double");
    ExpectBitExactRoundTrip(noisy_d, true, kCols, kRows, 1, &pred_none_d);
  }
  EXPECT_EQ(Predictor::getCode(LercNS::PREDICTOR_NONE), pred_none_f);
  EXPECT_EQ(Predictor::getCode(LercNS::PREDICTOR_NONE), pred_none_d);

  // 2. Row-jumped horizontal ramp favors PREDICTOR_DELTA1.
  std::vector<float> delta1_f(kCount);
  std::vector<double> delta1_d(kCount);
  for (int r = 0; r < kRows; ++r) {
    const double row_base = static_cast<double>((r * 97) % 131) * 1.5;
    for (int c = 0; c < kCols; ++c) {
      const double v = row_base + static_cast<double>(c) * 1.0;
      delta1_f[r * kCols + c] = static_cast<float>(v);
      delta1_d[r * kCols + c] = v;
    }
  }
  unsigned char pred_d1_f = 255;
  unsigned char pred_d1_d = 255;
  {
    SCOPED_TRACE("PREDICTOR_DELTA1 float");
    ExpectBitExactRoundTrip(delta1_f, false, kCols, kRows, 1, &pred_d1_f);
  }
  {
    SCOPED_TRACE("PREDICTOR_DELTA1 double");
    ExpectBitExactRoundTrip(delta1_d, true, kCols, kRows, 1, &pred_d1_d);
  }
  EXPECT_EQ(Predictor::getCode(LercNS::PREDICTOR_DELTA1), pred_d1_f);
  EXPECT_EQ(Predictor::getCode(LercNS::PREDICTOR_DELTA1), pred_d1_d);

  // 3. Column-varying profile constant across rows favors PREDICTOR_ROWS_COLS
  // because horizontal differencing leaves column entropy while the vertical
  // pass cancels rows 1..N-1 to zero.
  std::vector<float> cross_f(kCount);
  std::vector<double> cross_d(kCount);
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kCols; ++c) {
      const double v = static_cast<double>((c * 131) % 29) * 0.25 + 100.0;
      cross_f[r * kCols + c] = static_cast<float>(v);
      cross_d[r * kCols + c] = v;
    }
  }
  unsigned char pred_rc_f = 255;
  unsigned char pred_rc_d = 255;
  {
    SCOPED_TRACE("PREDICTOR_ROWS_COLS float");
    ExpectBitExactRoundTrip(cross_f, false, kCols, kRows, 1, &pred_rc_f);
  }
  {
    SCOPED_TRACE("PREDICTOR_ROWS_COLS double");
    ExpectBitExactRoundTrip(cross_d, true, kCols, kRows, 1, &pred_rc_d);
  }
  EXPECT_EQ(Predictor::getCode(LercNS::PREDICTOR_ROWS_COLS), pred_rc_f);
  EXPECT_EQ(Predictor::getCode(LercNS::PREDICTOR_ROWS_COLS), pred_rc_d);
}

TEST(FplLerc2ExtTest, MultiDepthSpecialFloatBitsAndRecomputeCleanupRoundTrip) {
  // Special IEEE float patterns (-0.0, subnormals, NaN, inf) round-trip
  // bit-for-bit across iDepth == 3.
  const std::vector<float> depth_f = {
      0.0f,
      -0.0f,
      1.0f,
      std::numeric_limits<float>::quiet_NaN(),
      -42.5f,
      1e-42f,
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      3.125f,
      -0.25f,
      1024.0f,
      -0.125f,
  };
  ExpectBitExactRoundTrip(depth_f, false, 2, 2, 3);

  const std::vector<double> depth_d = {
      1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.5, 8.5, 9.5, 10.5, 11.5, 12.5,
  };
  ExpectBitExactRoundTrip(depth_d, true, 2, 2, 3);

  // Calling ComputeHuffmanCodesFlt twice for iDepth=1 on the same codec object
  // cleans up stale m_buffers cleanly without leaking.
  const std::vector<float> first = {1.0f, 2.0f, 3.0f, 4.0f};
  const std::vector<float> second = {10.0f, 20.0f, 30.0f, 40.0f};
  LosslessFPCompression codec;
  ASSERT_TRUE(codec.ComputeHuffmanCodesFlt(first.data(), false, 2, 2, 1));
  ASSERT_TRUE(codec.ComputeHuffmanCodesFlt(second.data(), false, 2, 2, 1));

  std::vector<unsigned char> blob(codec.compressedLength());
  unsigned char* dst = blob.data();
  ASSERT_TRUE(codec.EncodeHuffmanFlt(&dst));

  std::vector<float> out(second.size());
  const unsigned char* src = blob.data();
  std::size_t rem = blob.size();
  ASSERT_TRUE(LosslessFPCompression::DecodeHuffmanFlt(&src, rem, out.data(),
                                                      false, 2, 2, 1));
  EXPECT_THAT(out, ElementsAreArray(second));
}

TEST(FplLerc2ExtTest, DecodeRejectsMalformedPlaneZeroHeadersAndCoversHelpers) {
  const std::vector<float> sample = {1.0f, 2.0f, 3.0f, 4.0f};
  const std::vector<unsigned char> valid =
      EncodeSurface(sample, false, 2, 2, 1);

  std::vector<float> out(4, 0.0f);

  // Malformed headers all fail at byte plane 0 before any output_buffers push,
  // avoiding upstream multi-plane leak.
  // TODO(schwehr): Report upstream: DecodeHuffmanFltSlice leaks earlier
  // output_buffers if a byte > 0 header check fails.
  struct MalformedHeaderCase {
    std::string_view label;
    std::size_t rem_limit;
    int offset;
    std::uint32_t value;
    std::size_t width;
  };
  constexpr MalformedHeaderCase kMalformedCases[] = {
      {.label = "pred_code > 2",
       .rem_limit = 0,
       .offset = 0,
       .value = 3,
       .width = 1},
      {.label = "truncated before 6B plane-0 header",
       .rem_limit = 5,
       .offset = -1,
       .value = 0,
       .width = 0},
      {.label = "plane-0 byte_index >= 4",
       .rem_limit = 0,
       .offset = 1,
       .value = 4,
       .width = 1},
      {.label = "plane-0 best_level > MAX_DELTA",
       .rem_limit = 0,
       .offset = 2,
       .value = 6,
       .width = 1},
      {.label = "plane-0 compressed_size > remaining",
       .rem_limit = 0,
       .offset = 3,
       .value = 0x100000U,
       .width = 4},
  };
  for (const auto& tc : kMalformedCases) {
    SCOPED_TRACE(tc.label);
    auto bad = valid;
    if (tc.offset >= 0) {
      if (tc.width == 1) {
        bad[static_cast<std::size_t>(tc.offset)] =
            static_cast<unsigned char>(tc.value);
      } else {
        WritePodAt(bad, static_cast<std::size_t>(tc.offset), tc.value);
      }
    }
    const unsigned char* src = bad.data();
    std::size_t rem = tc.rem_limit > 0 ? tc.rem_limit : bad.size();
    EXPECT_FALSE(LosslessFPCompression::DecodeHuffmanFlt(&src, rem, out.data(),
                                                         false, 2, 2, 1));
  }

  // Global helper coverage: lerc_assert, restoreSequence(make_copy=true),
  // getBestLevel(max_delta_order=0).
  lerc_assert(true);
  EXPECT_THROW(lerc_assert(false), const char*);

  unsigned char raw_seq[] = {5, 1, 2, 1};
  OwnedMallocBytes level0(restoreSequence(raw_seq, sizeof(raw_seq), 0, true));
  ASSERT_NE(nullptr, level0.get());
  EXPECT_THAT(std::vector<unsigned char>(level0.get(), level0.get() + 4),
              ElementsAre(5, 1, 2, 1));

  OwnedMallocBytes level2(restoreSequence(raw_seq, sizeof(raw_seq), 2, true));
  ASSERT_NE(nullptr, level2.get());
  // Original {5, 6, 9, 13}: 1st diff {5, 1, 3, 4}, 2nd diff {5, 1, 2, 1}.
  EXPECT_THAT(std::vector<unsigned char>(level2.get(), level2.get() + 4),
              ElementsAre(5, 6, 9, 13));

  EXPECT_EQ(0, getBestLevel(raw_seq, sizeof(raw_seq), 0));
}

}  // namespace
}  // namespace lerc
