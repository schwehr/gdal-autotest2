// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Clean-room unit tests for libtiff byte-swapping and bit-reversal routines
// (tif_swab.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_swab.c

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr uint16_t ByteSwap16(uint16_t v) {
  return static_cast<uint16_t>(((v & 0x00ffU) << 8) | ((v & 0xff00U) >> 8));
}

constexpr uint32_t ByteSwap32(uint32_t v) {
  return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
         ((v & 0x00ff0000U) >> 8) | ((v & 0xff000000U) >> 24);
}

constexpr uint64_t ByteSwap64(uint64_t v) {
  return ((v & 0x00000000000000ffULL) << 56) |
         ((v & 0x000000000000ff00ULL) << 40) |
         ((v & 0x0000000000ff0000ULL) << 24) |
         ((v & 0x00000000ff000000ULL) << 8) |
         ((v & 0x000000ff00000000ULL) >> 8) |
         ((v & 0x0000ff0000000000ULL) >> 24) |
         ((v & 0x00ff000000000000ULL) >> 40) |
         ((v & 0xff00000000000000ULL) >> 56);
}

constexpr uint8_t ReverseBits8(uint8_t b) {
  uint8_t out = 0;
  for (int bit = 0; bit < 8; ++bit) {
    out = static_cast<uint8_t>((out << 1) | ((b >> bit) & 0x01U));
  }
  return out;
}

constexpr std::array<tmsize_t, 19> kArrayTestLengths = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 129,
};

constexpr std::array<tmsize_t, 3> kNonPositiveCounts = {0, -1, -17};

TEST(TifSwabTest, SwabShortSingleAndRoundTrip) {
  struct Case {
    uint16_t input;
    uint16_t expected;
  };
  constexpr auto kCases = std::to_array<Case>({
      {.input = 0x0000U, .expected = 0x0000U},
      {.input = 0xffffU, .expected = 0xffffU},
      {.input = 0x00ffU, .expected = 0xff00U},
      {.input = 0x1234U, .expected = 0x3412U},
      {.input = 0xabcdU, .expected = 0xcdabU},
      {.input = 0x8001U, .expected = 0x0180U},
  });

  for (const Case& c : kCases) {
    SCOPED_TRACE(c.input);
    uint16_t val = c.input;
    TIFFSwabShort(&val);
    EXPECT_EQ(val, c.expected);
    TIFFSwabShort(&val);
    EXPECT_EQ(val, c.input);
  }
}

TEST(TifSwabTest, SwabLongSingleAndRoundTrip) {
  struct Case {
    uint32_t input;
    uint32_t expected;
  };
  constexpr auto kCases = std::to_array<Case>({
      {.input = 0x00000000U, .expected = 0x00000000U},
      {.input = 0xffffffffU, .expected = 0xffffffffU},
      {.input = 0x12345678U, .expected = 0x78563412U},
      {.input = 0x00ff00ffU, .expected = 0xff00ff00U},
      {.input = 0xaabbccddU, .expected = 0xddccbbaaU},
      {.input = 0x80000001U, .expected = 0x01000080U},
  });

  for (const Case& c : kCases) {
    SCOPED_TRACE(c.input);
    uint32_t val = c.input;
    TIFFSwabLong(&val);
    EXPECT_EQ(val, c.expected);
    TIFFSwabLong(&val);
    EXPECT_EQ(val, c.input);
  }
}

TEST(TifSwabTest, SwabLong8SingleAndRoundTrip) {
  struct Case {
    uint64_t input;
    uint64_t expected;
  };
  constexpr auto kCases = std::to_array<Case>({
      {.input = 0x0000000000000000ULL, .expected = 0x0000000000000000ULL},
      {.input = 0xffffffffffffffffULL, .expected = 0xffffffffffffffffULL},
      {.input = 0x0123456789abcdefULL, .expected = 0xefcdab8967452301ULL},
      {.input = 0x0011223344556677ULL, .expected = 0x7766554433221100ULL},
      {.input = 0x8000000000000001ULL, .expected = 0x0100000000000080ULL},
  });

  for (const Case& c : kCases) {
    SCOPED_TRACE(c.input);
    uint64_t val = c.input;
    TIFFSwabLong8(&val);
    EXPECT_EQ(val, c.expected);
    TIFFSwabLong8(&val);
    EXPECT_EQ(val, c.input);
  }
}

TEST(TifSwabTest, SwabFloatSingleAndRoundTrip) {
  // Choose finite IEEE-754 bit patterns whose byte-swapped patterns are also
  // non-NaN bit patterns so floating-point checks remain well-defined.
  constexpr auto kBitPatterns = std::to_array<uint32_t>({
      0x00000000U,
      0x80000000U,
      0x3f800000U,
      0x12345678U,
      0xc0490fdbU,
  });

  for (const uint32_t bits : kBitPatterns) {
    SCOPED_TRACE(bits);
    float val = std::bit_cast<float>(bits);
    TIFFSwabFloat(&val);
    EXPECT_EQ(std::bit_cast<uint32_t>(val), ByteSwap32(bits));
    TIFFSwabFloat(&val);
    EXPECT_EQ(std::bit_cast<uint32_t>(val), bits);
    EXPECT_FLOAT_EQ(val, std::bit_cast<float>(bits));
  }
}

TEST(TifSwabTest, SwabDoubleSingleAndRoundTrip) {
  // Choose finite IEEE-754 bit patterns whose byte-swapped patterns are also
  // non-NaN bit patterns.
  constexpr auto kBitPatterns = std::to_array<uint64_t>({
      0x0000000000000000ULL,
      0x8000000000000000ULL,
      0x3ff0000000000000ULL,
      0x0123456789abcdefULL,
      0x400921fb54442d18ULL,
  });

  for (const uint64_t bits : kBitPatterns) {
    SCOPED_TRACE(bits);
    double val = std::bit_cast<double>(bits);
    TIFFSwabDouble(&val);
    EXPECT_EQ(std::bit_cast<uint64_t>(val), ByteSwap64(bits));
    TIFFSwabDouble(&val);
    EXPECT_EQ(std::bit_cast<uint64_t>(val), bits);
    EXPECT_DOUBLE_EQ(val, std::bit_cast<double>(bits));
  }
}

TEST(TifSwabTest, SwabArrayOfShortLengthsAndNoOps) {
  constexpr uint16_t kSentinel = 0x5a5aU;
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFSwabArrayOfShort(nullptr, n);
    std::array<uint16_t, 4> buf = {kSentinel, 0x1234U, 0xabcdU, kSentinel};
    TIFFSwabArrayOfShort(buf.data() + 1, n);
    EXPECT_EQ(buf[0], kSentinel);
    EXPECT_EQ(buf[1], 0x1234U);
    EXPECT_EQ(buf[2], 0xabcdU);
    EXPECT_EQ(buf[3], kSentinel);
  }

  for (const tmsize_t n : kArrayTestLengths) {
    SCOPED_TRACE(n);
    std::vector<uint16_t> buf(static_cast<size_t>(n) + 2U, kSentinel);
    std::vector<uint16_t> orig(static_cast<size_t>(n));
    for (tmsize_t i = 0; i < n; ++i) {
      const uint16_t v = static_cast<uint16_t>(0x0102U + i * 0x0305U);
      orig[static_cast<size_t>(i)] = v;
      buf[static_cast<size_t>(i) + 1U] = v;
    }

    TIFFSwabArrayOfShort(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U],
                ByteSwap16(orig[static_cast<size_t>(i)]))
          << "index=" << i;
    }

    TIFFSwabArrayOfShort(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U], orig[static_cast<size_t>(i)])
          << "index=" << i;
    }
  }
}

TEST(TifSwabTest, SwabArrayOfTriplesLengthsAndNoOps) {
  constexpr uint8_t kSentinel = 0xa5U;
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFSwabArrayOfTriples(nullptr, n);
    std::array<uint8_t, 8> buf = {kSentinel, 0x11U, 0x22U, 0x33U,
                                  0x44U,     0x55U, 0x66U, kSentinel};
    TIFFSwabArrayOfTriples(buf.data() + 1, n);
    EXPECT_EQ(buf[0], kSentinel);
    EXPECT_EQ(buf[1], 0x11U);
    EXPECT_EQ(buf[2], 0x22U);
    EXPECT_EQ(buf[3], 0x33U);
    EXPECT_EQ(buf[4], 0x44U);
    EXPECT_EQ(buf[5], 0x55U);
    EXPECT_EQ(buf[6], 0x66U);
    EXPECT_EQ(buf[7], kSentinel);
  }

  for (const tmsize_t n : kArrayTestLengths) {
    SCOPED_TRACE(n);
    const size_t payload_bytes = static_cast<size_t>(n) * 3U;
    std::vector<uint8_t> buf(payload_bytes + 2U, kSentinel);
    std::vector<uint8_t> orig(payload_bytes);
    for (tmsize_t i = 0; i < n; ++i) {
      const size_t off = static_cast<size_t>(i) * 3U;
      orig[off + 0] = static_cast<uint8_t>((i * 7 + 1) & 0xffU);
      orig[off + 1] = static_cast<uint8_t>((i * 7 + 2) & 0xffU);
      orig[off + 2] = static_cast<uint8_t>((i * 7 + 3) & 0xffU);
      buf[off + 1] = orig[off + 0];
      buf[off + 2] = orig[off + 1];
      buf[off + 3] = orig[off + 2];
    }

    TIFFSwabArrayOfTriples(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      const size_t off = static_cast<size_t>(i) * 3U;
      EXPECT_EQ(buf[off + 1], orig[off + 2]) << "triple=" << i;
      EXPECT_EQ(buf[off + 2], orig[off + 1]) << "triple=" << i;
      EXPECT_EQ(buf[off + 3], orig[off + 0]) << "triple=" << i;
    }

    TIFFSwabArrayOfTriples(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (size_t i = 0; i < payload_bytes; ++i) {
      EXPECT_EQ(buf[i + 1], orig[i]) << "byte=" << i;
    }
  }
}

TEST(TifSwabTest, SwabArrayOfLongLengthsAndNoOps) {
  constexpr uint32_t kSentinel = 0x5a5a5a5aU;
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFSwabArrayOfLong(nullptr, n);
    std::array<uint32_t, 4> buf = {kSentinel, 0x12345678U, 0x9abcdef0U,
                                   kSentinel};
    TIFFSwabArrayOfLong(buf.data() + 1, n);
    EXPECT_EQ(buf[0], kSentinel);
    EXPECT_EQ(buf[1], 0x12345678U);
    EXPECT_EQ(buf[2], 0x9abcdef0U);
    EXPECT_EQ(buf[3], kSentinel);
  }

  for (const tmsize_t n : kArrayTestLengths) {
    SCOPED_TRACE(n);
    std::vector<uint32_t> buf(static_cast<size_t>(n) + 2U, kSentinel);
    std::vector<uint32_t> orig(static_cast<size_t>(n));
    for (tmsize_t i = 0; i < n; ++i) {
      const uint32_t v = 0x01020304U + static_cast<uint32_t>(i) * 0x0507090bU;
      orig[static_cast<size_t>(i)] = v;
      buf[static_cast<size_t>(i) + 1U] = v;
    }

    TIFFSwabArrayOfLong(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U],
                ByteSwap32(orig[static_cast<size_t>(i)]))
          << "index=" << i;
    }

    TIFFSwabArrayOfLong(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U], orig[static_cast<size_t>(i)])
          << "index=" << i;
    }
  }
}

TEST(TifSwabTest, SwabArrayOfLong8LengthsAndNoOps) {
  constexpr uint64_t kSentinel = 0x5a5a5a5a5a5a5a5aULL;
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFSwabArrayOfLong8(nullptr, n);
    std::array<uint64_t, 4> buf = {kSentinel, 0x0123456789abcdefULL,
                                   0x1122334455667788ULL, kSentinel};
    TIFFSwabArrayOfLong8(buf.data() + 1, n);
    EXPECT_EQ(buf[0], kSentinel);
    EXPECT_EQ(buf[1], 0x0123456789abcdefULL);
    EXPECT_EQ(buf[2], 0x1122334455667788ULL);
    EXPECT_EQ(buf[3], kSentinel);
  }

  for (const tmsize_t n : kArrayTestLengths) {
    SCOPED_TRACE(n);
    std::vector<uint64_t> buf(static_cast<size_t>(n) + 2U, kSentinel);
    std::vector<uint64_t> orig(static_cast<size_t>(n));
    for (tmsize_t i = 0; i < n; ++i) {
      const uint64_t v = 0x0102030405060708ULL +
                         static_cast<uint64_t>(i) * 0x030507090b0d0f11ULL;
      orig[static_cast<size_t>(i)] = v;
      buf[static_cast<size_t>(i) + 1U] = v;
    }

    TIFFSwabArrayOfLong8(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U],
                ByteSwap64(orig[static_cast<size_t>(i)]))
          << "index=" << i;
    }

    TIFFSwabArrayOfLong8(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U], orig[static_cast<size_t>(i)])
          << "index=" << i;
    }
  }
}

TEST(TifSwabTest, SwabArrayOfFloatLengthsAndNoOps) {
  constexpr uint32_t kSentinelBits = 0x5a5a5a5aU;
  const float kSentinel = std::bit_cast<float>(kSentinelBits);
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFSwabArrayOfFloat(nullptr, n);
    std::array<float, 4> buf = {kSentinel, std::bit_cast<float>(0x12345678U),
                                std::bit_cast<float>(0x3f800000U), kSentinel};
    TIFFSwabArrayOfFloat(buf.data() + 1, n);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf[0]), kSentinelBits);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf[1]), 0x12345678U);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf[2]), 0x3f800000U);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf[3]), kSentinelBits);
  }

  for (const tmsize_t n : kArrayTestLengths) {
    SCOPED_TRACE(n);
    std::vector<float> buf(static_cast<size_t>(n) + 2U, kSentinel);
    std::vector<uint32_t> orig_bits(static_cast<size_t>(n));
    for (tmsize_t i = 0; i < n; ++i) {
      // Keep top byte away from 0x7f/0xff and bottom byte away from 0x7f/0xff
      // so neither original nor swapped bit pattern forms an IEEE-754 NaN.
      const uint32_t b0 = 0x10U + static_cast<uint32_t>(i & 0x3fU);
      const uint32_t b1 = 0x20U + static_cast<uint32_t>((i * 3) & 0x7fU);
      const uint32_t b2 = 0x30U + static_cast<uint32_t>((i * 5) & 0x7fU);
      const uint32_t b3 = 0x40U + static_cast<uint32_t>((i * 7) & 0x3fU);
      const uint32_t bits = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
      orig_bits[static_cast<size_t>(i)] = bits;
      buf[static_cast<size_t>(i) + 1U] = std::bit_cast<float>(bits);
    }

    TIFFSwabArrayOfFloat(buf.data() + 1, n);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf.front()), kSentinelBits);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf.back()), kSentinelBits);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(std::bit_cast<uint32_t>(buf[static_cast<size_t>(i) + 1U]),
                ByteSwap32(orig_bits[static_cast<size_t>(i)]))
          << "index=" << i;
    }

    TIFFSwabArrayOfFloat(buf.data() + 1, n);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf.front()), kSentinelBits);
    EXPECT_EQ(std::bit_cast<uint32_t>(buf.back()), kSentinelBits);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(std::bit_cast<uint32_t>(buf[static_cast<size_t>(i) + 1U]),
                orig_bits[static_cast<size_t>(i)])
          << "index=" << i;
    }
  }
}

TEST(TifSwabTest, SwabArrayOfDoubleLengthsAndNoOps) {
  constexpr uint64_t kSentinelBits = 0x5a5a5a5a5a5a5a5aULL;
  const double kSentinel = std::bit_cast<double>(kSentinelBits);
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFSwabArrayOfDouble(nullptr, n);
    std::array<double, 4> buf = {
        kSentinel, std::bit_cast<double>(0x0123456789abcdefULL),
        std::bit_cast<double>(0x3ff0000000000000ULL), kSentinel};
    TIFFSwabArrayOfDouble(buf.data() + 1, n);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf[0]), kSentinelBits);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf[1]), 0x0123456789abcdefULL);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf[2]), 0x3ff0000000000000ULL);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf[3]), kSentinelBits);
  }

  for (const tmsize_t n : kArrayTestLengths) {
    SCOPED_TRACE(n);
    std::vector<double> buf(static_cast<size_t>(n) + 2U, kSentinel);
    std::vector<uint64_t> orig_bits(static_cast<size_t>(n));
    for (tmsize_t i = 0; i < n; ++i) {
      const uint64_t b0 = 0x10U + static_cast<uint64_t>(i & 0x3fU);
      const uint64_t b7 = 0x40U + static_cast<uint64_t>((i * 7) & 0x3fU);
      const uint64_t mid = 0x010203040506ULL * static_cast<uint64_t>(i + 1);
      const uint64_t bits =
          (b7 << 56) | ((mid & 0x0000ffffffffffffULL) << 8) | b0;
      orig_bits[static_cast<size_t>(i)] = bits;
      buf[static_cast<size_t>(i) + 1U] = std::bit_cast<double>(bits);
    }

    TIFFSwabArrayOfDouble(buf.data() + 1, n);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf.front()), kSentinelBits);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf.back()), kSentinelBits);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(std::bit_cast<uint64_t>(buf[static_cast<size_t>(i) + 1U]),
                ByteSwap64(orig_bits[static_cast<size_t>(i)]))
          << "index=" << i;
    }

    TIFFSwabArrayOfDouble(buf.data() + 1, n);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf.front()), kSentinelBits);
    EXPECT_EQ(std::bit_cast<uint64_t>(buf.back()), kSentinelBits);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(std::bit_cast<uint64_t>(buf[static_cast<size_t>(i) + 1U]),
                orig_bits[static_cast<size_t>(i)])
          << "index=" << i;
    }
  }
}

TEST(TifSwabTest, GetBitRevTableValuesAndInvariants) {
  const unsigned char* no_rev = TIFFGetBitRevTable(0);
  const unsigned char* rev = TIFFGetBitRevTable(1);
  ASSERT_NE(no_rev, nullptr);
  ASSERT_NE(rev, nullptr);
  EXPECT_NE(no_rev, rev);

  EXPECT_EQ(TIFFGetBitRevTable(-1), rev);
  EXPECT_EQ(TIFFGetBitRevTable(2), rev);
  EXPECT_EQ(TIFFGetBitRevTable(255), rev);

  for (int i = 0; i < 256; ++i) {
    SCOPED_TRACE(i);
    const uint8_t b = static_cast<uint8_t>(i);
    EXPECT_EQ(no_rev[i], b);
    EXPECT_EQ(rev[i], ReverseBits8(b));
    EXPECT_EQ(rev[rev[i]], b);
  }

  EXPECT_EQ(rev[0x00], 0x00U);
  EXPECT_EQ(rev[0x01], 0x80U);
  EXPECT_EQ(rev[0x80], 0x01U);
  EXPECT_EQ(rev[0x0f], 0xf0U);
  EXPECT_EQ(rev[0xaa], 0x55U);
  EXPECT_EQ(rev[0xff], 0xffU);
}

TEST(TifSwabTest, ReverseBitsBoundaryAndLargeBuffers) {
  constexpr uint8_t kSentinel = 0xa5U;
  for (const tmsize_t n : kNonPositiveCounts) {
    SCOPED_TRACE(n);
    TIFFReverseBits(nullptr, n);
    std::array<uint8_t, 4> buf = {kSentinel, 0x01U, 0x83U, kSentinel};
    TIFFReverseBits(buf.data() + 1, n);
    EXPECT_EQ(buf[0], kSentinel);
    EXPECT_EQ(buf[1], 0x01U);
    EXPECT_EQ(buf[2], 0x83U);
    EXPECT_EQ(buf[3], kSentinel);
  }

  constexpr auto kLengths = std::to_array<tmsize_t>({
      1,  2,  3,  4,  5,  6,  7,   8,   9,   10,  15,   16,
      17, 24, 25, 63, 64, 65, 255, 256, 257, 513, 1024,
  });

  for (const tmsize_t n : kLengths) {
    SCOPED_TRACE(n);
    std::vector<uint8_t> buf(static_cast<size_t>(n) + 2U, kSentinel);
    std::vector<uint8_t> orig(static_cast<size_t>(n));
    for (tmsize_t i = 0; i < n; ++i) {
      const uint8_t v = static_cast<uint8_t>((i * 37 + 13) & 0xffU);
      orig[static_cast<size_t>(i)] = v;
      buf[static_cast<size_t>(i) + 1U] = v;
    }

    TIFFReverseBits(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U],
                ReverseBits8(orig[static_cast<size_t>(i)]))
          << "index=" << i;
    }

    TIFFReverseBits(buf.data() + 1, n);
    EXPECT_EQ(buf.front(), kSentinel);
    EXPECT_EQ(buf.back(), kSentinel);
    for (tmsize_t i = 0; i < n; ++i) {
      EXPECT_EQ(buf[static_cast<size_t>(i) + 1U], orig[static_cast<size_t>(i)])
          << "index=" << i;
    }
  }
}

TEST(TifSwabTest, BigEndianAndFillOrderIntegrationRoundTrip) {
  // 1) Big-endian 16-bit grayscale round-trip exercises byte-swapping on
  //    little-endian hosts via TIFFWriteScanline / TIFFReadScanline.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 8;
    constexpr uint32_t kHeight = 2;
    const std::array<uint16_t, kWidth> row0_ref = {
        0x0102U, 0x1122U, 0x3344U, 0x5566U, 0x7788U, 0x99aaU, 0xbbccU, 0xddeeU};
    const std::array<uint16_t, kWidth> row1_ref = {
        0xf00dU, 0xcafeU, 0xbeefU, 0x1234U, 0x5678U, 0x9abcU, 0xdef0U, 0x0001U};

    {
      UniqueTiff writer = stream.Open("wb");
      ASSERT_NE(writer, nullptr);
      EXPECT_EQ(TIFFIsBigEndian(writer.get()), 1);
      const int expect_swab =
          (std::endian::native == std::endian::little) ? 1 : 0;
      EXPECT_EQ(TIFFIsByteSwapped(writer.get()), expect_swab);

      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE,
                             static_cast<uint16_t>(16)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL,
                             static_cast<uint16_t>(1)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             static_cast<uint16_t>(PHOTOMETRIC_MINISBLACK)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             static_cast<uint16_t>(COMPRESSION_NONE)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             static_cast<uint16_t>(PLANARCONFIG_CONTIG)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kHeight), 1);

      // TIFFWriteScanline swaps caller buffers in place, so pass mutable
      // copies.
      auto row0 = row0_ref;
      auto row1 = row1_ref;
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(reader.get()), 1);
    std::array<uint16_t, kWidth> out0{};
    std::array<uint16_t, kWidth> out1{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), out0.data(), 0, 0), 1);
    ASSERT_EQ(TIFFReadScanline(reader.get(), out1.data(), 1, 0), 1);
    EXPECT_EQ(out0, row0_ref);
    EXPECT_EQ(out1, row1_ref);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }

  // 2) FILLORDER_LSB2MSB 1-bit strip round-trip exercises TIFFReverseBits.
  {
    MemoryTiffStream stream;
    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 2;
    const std::array<uint8_t, 8> payload_ref = {0x01U, 0x23U, 0x45U, 0x67U,
                                                0x89U, 0xabU, 0xcdU, 0xefU};

    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE,
                             static_cast<uint16_t>(1)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL,
                             static_cast<uint16_t>(1)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             static_cast<uint16_t>(PHOTOMETRIC_MINISBLACK)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             static_cast<uint16_t>(COMPRESSION_NONE)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER,
                             static_cast<uint16_t>(FILLORDER_LSB2MSB)),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kHeight), 1);

      auto mutable_payload = payload_ref;
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), 0, mutable_payload.data(),
                                static_cast<tmsize_t>(mutable_payload.size())),
          static_cast<tmsize_t>(mutable_payload.size()));
    }

    // Default read mode uses FILLORDER_MSB2LSB so libtiff reverses bits back.
    UniqueTiff reader_msb = stream.Open("r");
    ASSERT_NE(reader_msb, nullptr);
    uint16_t fill_order = 0;
    ASSERT_EQ(TIFFGetField(reader_msb.get(), TIFFTAG_FILLORDER, &fill_order),
              1);
    EXPECT_EQ(fill_order, FILLORDER_LSB2MSB);
    std::array<uint8_t, 8> out_msb{};
    ASSERT_EQ(TIFFReadEncodedStrip(reader_msb.get(), 0, out_msb.data(),
                                   static_cast<tmsize_t>(out_msb.size())),
              static_cast<tmsize_t>(out_msb.size()));
    EXPECT_EQ(out_msb, payload_ref);
    reader_msb.reset();

    // Reading with 'L' returns raw LSB2MSB bytes without reversing.
    UniqueTiff reader_lsb = stream.Open("rL");
    ASSERT_NE(reader_lsb, nullptr);
    std::array<uint8_t, 8> out_lsb{};
    ASSERT_EQ(TIFFReadEncodedStrip(reader_lsb.get(), 0, out_lsb.data(),
                                   static_cast<tmsize_t>(out_lsb.size())),
              static_cast<tmsize_t>(out_lsb.size()));
    for (size_t i = 0; i < payload_ref.size(); ++i) {
      EXPECT_EQ(out_lsb[i], ReverseBits8(payload_ref[i])) << "byte=" << i;
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    EXPECT_EQ(stream.diagnostics().warning_count(), 0U);
  }
}

}  // namespace
}  // namespace libtiff
