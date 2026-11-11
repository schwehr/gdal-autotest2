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

// Unit tests for //third_party/netcdf/libsrc/lookup3.c.
//
// Exercises hash_fast() and underlying hashlittle() across zero-length inputs,
// all modulo-4 pointer alignments (4-byte, 2-byte, and 1-byte paths), all
// switch tail lengths 0..12, multi-block loops (>12 bytes), and bit
// sensitivity.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"

extern "C" {
uint32_t hash_fast(const void* key, size_t length);
}  // extern "C"

namespace netcdf {
namespace {

constexpr uint32_t kZeroLengthDigest = 0xdeadbeefU + 992099683U;

std::vector<uint8_t> MakeTestPattern(size_t length) {
  std::vector<uint8_t> out(length);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>((i * 37U + 13U) ^ (i >> 2));
  }
  return out;
}

TEST(Lookup3Test, ZeroLengthAcrossAllAlignmentsAndNullPointer) {
  alignas(4) std::array<uint8_t, 8> buf{};
  EXPECT_EQ(kZeroLengthDigest, hash_fast(nullptr, 0));
  for (size_t offset = 0; offset < 4; ++offset) {
    EXPECT_EQ(kZeroLengthDigest, hash_fast(buf.data() + offset, 0))
        << "offset=" << offset;
  }
}

TEST(Lookup3Test, MatchesAcrossAllFourAlignmentsAndTailCases) {
  // Lengths 1..40 hit every switch tail case 0..12 inside 0, 1, 2, and 3
  // 12-byte mix rounds on the 4-byte, 2-byte, and 1-byte aligned code paths.
  alignas(4) std::array<uint8_t, 64> aligned_buf{};
  const std::vector<uint8_t> pattern = MakeTestPattern(48);

  for (size_t len = 1; len <= 40; ++len) {
    SCOPED_TRACE(len);

    // Exact-sized heap buffer catches any 1-byte overread under ASan/MSan.
    std::vector<uint8_t> exact_heap(pattern.begin(), pattern.begin() + len);
    const uint32_t baseline = hash_fast(exact_heap.data(), len);

    for (size_t offset = 0; offset < 4; ++offset) {
      std::memset(aligned_buf.data(), 0xA5, aligned_buf.size());
      uint8_t* ptr = aligned_buf.data() + offset;
      ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) & 3U, offset);
      std::memcpy(ptr, pattern.data(), len);

      EXPECT_EQ(baseline, hash_fast(ptr, len)) << "offset=" << offset;
    }

    // Also verify exact-tail heap slices at unaligned offsets (+1, +2, +3).
    for (size_t offset = 1; offset < 4; ++offset) {
      std::vector<uint8_t> slice(offset + len, 0);
      std::memcpy(slice.data() + offset, pattern.data(), len);
      EXPECT_EQ(baseline, hash_fast(slice.data() + offset, len))
          << "heap offset=" << offset;
    }
  }
}

TEST(Lookup3Test, SingleBitFlipAndLengthChangesAffectDigest) {
  constexpr size_t kLengths[] = {1, 2, 4, 7, 8, 11, 12, 13, 24, 25, 36, 37};
  for (size_t len : kLengths) {
    SCOPED_TRACE(len);
    std::vector<uint8_t> data = MakeTestPattern(len);
    const uint32_t original = hash_fast(data.data(), data.size());

    for (size_t byte_idx = 0; byte_idx < len; ++byte_idx) {
      std::vector<uint8_t> mutated = data;
      mutated[byte_idx] ^= 0x01U;
      EXPECT_NE(original, hash_fast(mutated.data(), mutated.size()))
          << "byte_idx=" << byte_idx;
    }

    std::vector<uint8_t> nul_padded = data;
    nul_padded.push_back(0);
    EXPECT_NE(original, hash_fast(nul_padded.data(), nul_padded.size()));
  }
}

TEST(Lookup3Test, RepeatableAndDistinctAcrossIdentifierStrings) {
  const std::vector<std::string> keys = {
      "x",
      "y",
      "z",
      "time",
      "lat",
      "lon",
      "temp",
      "pres",
      "_Fill",
      "var_01",
      "var_02",
      "long_dimension_name",
      "another_very_long_variable_identifier_exceeding_24_bytes",
  };
  std::vector<uint32_t> digests;
  digests.reserve(keys.size());
  for (const std::string& key : keys) {
    const uint32_t d1 = hash_fast(key.data(), key.size());
    const uint32_t d2 = hash_fast(key.data(), key.size());
    EXPECT_EQ(d1, d2) << "key=" << key;
    for (size_t j = 0; j < digests.size(); ++j) {
      EXPECT_NE(digests[j], d1) << keys[j] << " collided with " << key;
    }
    digests.push_back(d1);
  }
}

}  // namespace
}  // namespace netcdf
