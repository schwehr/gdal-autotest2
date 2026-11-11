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

// Unit tests for //third_party/netcdf/libdispatch/dcrc32.c.
//
// Exercises NC_crc32() (and static crc32_z()) across null pointers, zero-length
// buffers, standard IEEE 802.3 check vectors, unaligned offsets, and both
// 8-byte unrolled and single-byte remainder loop boundaries. (BYFOUR and
// DYNAMIC_CRC_TABLE are disabled in third_party/netcdf/config.h.)

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/nccrc.h"
}  // extern "C"

namespace netcdf {
namespace {

// Direct bit-by-bit IEEE 802.3 CRC-32 reference (polynomial 0xedb88320U).
uint32_t ReferenceCrc32(uint32_t crc, const uint8_t* data, size_t len) {
  constexpr uint32_t kPoly = 0xedb88320U;
  crc ^= 0xffffffffU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
    }
  }
  return crc ^ 0xffffffffU;
}

std::vector<uint8_t> MakePatternBytes(size_t count) {
  std::vector<uint8_t> bytes(count);
  for (size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 131U + 17U) & 0xffU);
  }
  return bytes;
}

TEST(Dcrc32Test, NullBufferReturnsZeroRegardlessOfSeedAndLength) {
  EXPECT_EQ(0U, NC_crc32(0U, nullptr, 0U));
  EXPECT_EQ(0U, NC_crc32(0x12345678U, nullptr, 0U));
  EXPECT_EQ(0U, NC_crc32(0xffffffffU, nullptr, 32U));
}

TEST(Dcrc32Test, ZeroLengthNonNullBufferPreservesInitialCrc) {
  constexpr std::array<uint8_t, 1> kDummy{0x42U};
  EXPECT_EQ(0U, NC_crc32(0U, kDummy.data(), 0U));
  EXPECT_EQ(0xdeadbeefU, NC_crc32(0xdeadbeefU, kDummy.data(), 0U));
  EXPECT_EQ(0xffffffffU, NC_crc32(0xffffffffU, kDummy.data(), 0U));
}

TEST(Dcrc32Test, StandardCheckVectorMatchesIsoHdlc3309) {
  constexpr std::string_view kCheckInput = "123456789";
  constexpr uint32_t kExpectedCrc = 0xcbf43926U;
  EXPECT_EQ(kExpectedCrc,
            NC_crc32(0U, kCheckInput.data(),
                     static_cast<unsigned int>(kCheckInput.size())));
  EXPECT_EQ(
      kExpectedCrc,
      ReferenceCrc32(0U, reinterpret_cast<const uint8_t*>(kCheckInput.data()),
                     kCheckInput.size()));
}

TEST(Dcrc32Test, LoopBoundariesAndAlignmentOffsetsMatchBitwiseReference) {
  const std::vector<uint8_t> storage = MakePatternBytes(300);
  constexpr size_t kLengths[] = {1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 64, 257};
  constexpr uint32_t kSeeds[] = {0U, 0x5a5a5a5aU, 0xffffffffU};

  for (const uint32_t seed : kSeeds) {
    for (size_t offset = 0; offset < 4; ++offset) {
      for (const size_t len : kLengths) {
        SCOPED_TRACE(
            absl::StrFormat("seed=%u offset=%zu len=%zu", seed, offset, len));
        const uint8_t* ptr = storage.data() + offset;
        const uint32_t expected = ReferenceCrc32(seed, ptr, len);
        EXPECT_EQ(expected,
                  NC_crc32(seed, ptr, static_cast<unsigned int>(len)));
      }
    }
  }
}

TEST(Dcrc32Test, IncrementalUpdatesMatchSinglePassAcrossBlockBoundaries) {
  const std::vector<uint8_t> data = MakePatternBytes(65);
  const uint32_t full_crc =
      NC_crc32(0U, data.data(), static_cast<unsigned int>(data.size()));
  EXPECT_EQ(ReferenceCrc32(0U, data.data(), data.size()), full_crc);

  constexpr size_t kSplits[] = {0, 1, 7, 8, 9, 15, 16, 32, 64, 65};
  for (const size_t split : kSplits) {
    SCOPED_TRACE(split);
    const unsigned int head_len = static_cast<unsigned int>(split);
    const unsigned int tail_len =
        static_cast<unsigned int>(data.size() - split);
    const uint32_t head_crc = NC_crc32(0U, data.data(), head_len);
    const uint32_t combined = NC_crc32(head_crc, data.data() + split, tail_len);
    EXPECT_EQ(full_crc, combined);
  }
}

}  // namespace
}  // namespace netcdf
