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

// Unit tests for //third_party/netcdf/libdispatch/dcrc64.c.
//
// Exercises NC_crc64() (ECMA-182 polynomial 0xc96c5795d7870f42ULL) and
// crc64_combine(), covering table initialization, unaligned byte prefixes,
// 8-byte word loops, trailing bytes, zero-length inputs, and GF(2) combine
// bit-pattern branches. Tests stay single-threaded because dcrc64.c uses a
// non-atomic ONCE() macro when <pthread.h> is disabled, and the big-endian
// crc64_big()/rev8() path is runtime-unreachable on little-endian hosts.

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

// Defined with external linkage in libdispatch/dcrc64.c, not declared in
// nccrc.h.
unsigned long long crc64_combine(unsigned long long crc1,
                                 unsigned long long crc2, uintmax_t len2);
}  // extern "C"

namespace netcdf {
namespace {

// Direct bit-by-bit ECMA-182 CRC-64 reference matching Mark Adler's crc64.c.
uint64_t ReferenceCrc64(uint64_t crc, const uint8_t* data, size_t len) {
  constexpr uint64_t kPoly = 0xc96c5795d7870f42ULL;
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1ULL) ? (kPoly ^ (crc >> 1)) : (crc >> 1);
    }
  }
  return ~crc;
}

std::vector<uint8_t> ToMutableBytes(std::string_view sv) {
  return std::vector<uint8_t>(sv.begin(), sv.end());
}

TEST(Dcrc64Test, ZeroLengthPreservesInitialCrc) {
  std::array<uint8_t, 1> dummy{0x11U};
  EXPECT_EQ(0ULL, NC_crc64(0ULL, nullptr, 0U));
  EXPECT_EQ(0xfeedfacecafebeefULL,
            NC_crc64(0xfeedfacecafebeefULL, nullptr, 0U));
  EXPECT_EQ(0ULL, NC_crc64(0ULL, dummy.data(), 0U));
  EXPECT_EQ(0xfeedfacecafebeefULL,
            NC_crc64(0xfeedfacecafebeefULL, dummy.data(), 0U));
}

TEST(Dcrc64Test, StandardCheckVectorsMatchReference) {
  std::vector<uint8_t> check1 = ToMutableBytes("123456789");
  constexpr uint64_t kExpectedCheck1 = 0x995dc9bbdf1939faULL;
  EXPECT_EQ(
      kExpectedCheck1,
      NC_crc64(0ULL, check1.data(), static_cast<unsigned int>(check1.size())));
  EXPECT_EQ(kExpectedCheck1,
            ReferenceCrc64(0ULL, check1.data(), check1.size()));

  std::vector<uint8_t> check2 =
      ToMutableBytes("This is a test of the emergency broadcast system.");
  constexpr uint64_t kExpectedCheck2 = 0x27db187fc15bbc72ULL;
  EXPECT_EQ(
      kExpectedCheck2,
      NC_crc64(0ULL, check2.data(), static_cast<unsigned int>(check2.size())));
  EXPECT_EQ(kExpectedCheck2,
            ReferenceCrc64(0ULL, check2.data(), check2.size()));
}

TEST(Dcrc64Test, UnalignedPrefixAlignedBodyAndTrailingBytesMatchReference) {
  alignas(8) std::array<uint8_t, 160> buffer{};
  for (size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = static_cast<uint8_t>((i * 73U + 29U) & 0xffU);
  }

  constexpr size_t kLengths[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 24, 31, 64, 127};
  constexpr uint64_t kSeeds[] = {0ULL, 0x0123456789abcdefULL};

  for (const uint64_t seed : kSeeds) {
    for (size_t offset = 0; offset < 8; ++offset) {
      for (const size_t len : kLengths) {
        SCOPED_TRACE(absl::StrFormat("seed=%#llx offset=%zu len=%zu",
                                     static_cast<unsigned long long>(seed),
                                     offset, len));
        uint8_t* ptr = buffer.data() + offset;
        const uint64_t expected = ReferenceCrc64(seed, ptr, len);
        EXPECT_EQ(expected,
                  NC_crc64(seed, ptr, static_cast<unsigned int>(len)));
      }
    }
  }
}

TEST(Dcrc64Test, CombineZeroLengthReturnsFirstCrc) {
  std::vector<uint8_t> payload = ToMutableBytes("netcdf-crc64-zero-len");
  const uint64_t crc1 =
      NC_crc64(0ULL, payload.data(), static_cast<unsigned int>(payload.size()));
  EXPECT_EQ(crc1, crc64_combine(crc1, 0ULL, 0));
  EXPECT_EQ(crc1, crc64_combine(crc1, 0xabcdef0123456789ULL, 0));
}

TEST(Dcrc64Test, CombineSplitsMatchFullCrcAcrossBitPatterns) {
  alignas(8) std::array<uint8_t, 128> data{};
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 41U + 5U) & 0xffU);
  }
  const uint64_t full_crc =
      NC_crc64(0ULL, data.data(), static_cast<unsigned int>(data.size()));
  EXPECT_EQ(ReferenceCrc64(0ULL, data.data(), data.size()), full_crc);

  // Selected tail lengths exercise bit 0 = 0/1, bit 1 = 0/1, early break on
  // len2 == 0 after first shift, loop exit via while (len2 != 0), and multi-
  // iteration matrix squaring in crc64_combine().
  constexpr size_t kTailLengths[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,
                                     12, 16, 17, 24, 31, 49, 64, 65, 96};
  for (const size_t tail_len : kTailLengths) {
    SCOPED_TRACE(tail_len);
    const size_t head_len = data.size() - tail_len;
    const uint64_t crc1 =
        NC_crc64(0ULL, data.data(), static_cast<unsigned int>(head_len));
    const uint64_t crc2 = NC_crc64(0ULL, data.data() + head_len,
                                   static_cast<unsigned int>(tail_len));
    EXPECT_EQ(full_crc, crc64_combine(crc1, crc2, tail_len));
  }
}

}  // namespace
}  // namespace netcdf
