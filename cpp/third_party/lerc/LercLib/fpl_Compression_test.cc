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
// Unit tests for LercNS::fpl_Compression
// (third_party/lerc/LercLib/fpl_Compression.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_Compression.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/fpl_Compression.cpp

#include "third_party/lerc/LercLib/fpl_Compression.h"

#include <cstddef>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

namespace lerc {
namespace {

using ::LercNS::fpl_Compression;
using ::testing::ElementsAreArray;

TEST(FplCompressionTest, FastEstimateWithoutOutputComputesStridedEntropy) {
  // Constant input has zero sampled entropy.
  const std::vector<char> constant(128, 0x1B);
  EXPECT_EQ(0U,
            fpl_Compression::compress_buffer(constant.data(), constant.size(),
                                             /*output=*/nullptr,
                                             /*fast=*/true));

  // Periodic pattern with all 256 byte values spaced across stride
  // PRIME_MULT=7. getEntropySize sums sampled bits (~total_count bytes for
  // uniform samples).
  std::vector<char> uniform(256 * 7);
  for (std::size_t i = 0; i < uniform.size(); ++i) {
    uniform[i] = static_cast<char>((i / 7) & 0xFF);
  }
  const std::size_t est_uniform = fpl_Compression::compress_buffer(
      uniform.data(), uniform.size(), /*output=*/nullptr, /*fast=*/true);
  EXPECT_EQ(256U, est_uniform);

  // Skewed pattern yields non-zero sampled entropy below sample count (100B).
  std::vector<char> skewed(700);
  for (std::size_t i = 0; i < skewed.size(); ++i) {
    skewed[i] = static_cast<char>((i % 7 == 0) ? (i & 3) : 0);
  }
  const std::size_t est_skewed = fpl_Compression::compress_buffer(
      skewed.data(), skewed.size(), /*output=*/nullptr, /*fast=*/true);
  EXPECT_EQ(25U, est_skewed);
}

TEST(FplCompressionTest, CompressAndExtractRoundTripAcrossModes) {
  // 1. Constant (HUFFMAN_RLE)
  const std::vector<char> constant(64, 0x55);

  // 2. Repeating runs across all 256 values (HUFFMAN_PACKBITS)
  std::vector<char> runs;
  runs.reserve(256 * 16);
  for (int v = 0; v < 256; ++v) {
    runs.insert(runs.end(), 16, static_cast<char>(v));
  }

  // 3. Pseudorandom permutation across 256 values (HUFFMAN_NO_ENCODING)
  std::vector<char> incompressible(512);
  for (std::size_t i = 0; i < incompressible.size(); ++i) {
    incompressible[i] = static_cast<char>((i * 131 + 17) & 0xFF);
  }

  // 4. Skewed non-repeating pattern (HUFFMAN_NORMAL)
  std::vector<char> skewed(512);
  for (std::size_t i = 0; i < skewed.size(); ++i) {
    skewed[i] = static_cast<char>((i % 9 == 0) ? 1 : ((i % 20 == 0) ? 2 : 0));
  }

  const std::vector<std::vector<char>> inputs = {constant, runs, incompressible,
                                                 skewed};
  for (const auto& input : inputs) {
    for (bool fast : {false, true}) {
      char* raw_compressed = nullptr;
      const std::size_t comp_size = fpl_Compression::compress_buffer(
          input.data(), input.size(), &raw_compressed, fast);
      OwnedMallocChars compressed(raw_compressed);
      ASSERT_GT(comp_size, 0U);
      ASSERT_NE(nullptr, compressed.get());

      char* raw_extracted = nullptr;
      const std::size_t out_size = fpl_Compression::extract_buffer(
          compressed.get(), comp_size, input.size(), &raw_extracted);
      OwnedMallocChars extracted(raw_extracted);
      ASSERT_EQ(input.size(), out_size);
      ASSERT_NE(nullptr, extracted.get());
      EXPECT_THAT(
          std::vector<char>(extracted.get(), extracted.get() + out_size),
          ElementsAreArray(input));
    }
  }
}

}  // namespace
}  // namespace lerc
