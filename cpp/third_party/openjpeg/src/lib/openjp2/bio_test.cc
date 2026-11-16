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
// Tests internal OpenJPEG bit input-output (BIO) stream APIs
// (src/lib/openjp2/bio.c).
//
// Caveats:
// - opj_bio_write requires 1 <= n <= 32 and opj_bio_read requires n > 0
//   (and n <= 32 under OPJ_UBSAN_BUILD) via C assert().
// - Non-null buffer pointers are passed even when len == 0 to avoid C pointer
//   arithmetic on NULL under UBSan pointer-overflow checks.
// - Tests inspect internal opj_bio_t fields whose layout may change on upgrade.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/bio.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/bio.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"
#include "third_party/openjpeg2/google/util/test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;

OpjBioPtr CreateBio() { return OpjBioPtr(opj_bio_create()); }

constexpr uint32_t MaskForBits(uint32_t n) {
  return (n == 32) ? 0xFFFFFFFFU : ((uint32_t{1} << n) - 1U);
}

struct BitOp {
  uint32_t value;
  uint32_t bits;
};

struct StuffingTestCase {
  std::string_view name;
  std::array<BitOp, 4> ops;
  std::size_t num_ops;
  std::array<OPJ_BYTE, 4> expected_bytes;
  std::size_t expected_len;
};

constexpr StuffingTestCase kStuffingCases[] = {
    {
        .name = "Single0xFFTrailingZeroPad",
        .ops = {{{.value = 0xFFU, .bits = 8}}},
        .num_ops = 1,
        .expected_bytes = {0xFF, 0x00},
        .expected_len = 2,
    },
    {
        .name = "SevenOnesNotStuffed",
        .ops = {{{.value = 0x7FU, .bits = 7}}},
        .num_ops = 1,
        .expected_bytes = {0xFE},
        .expected_len = 1,
    },
    {
        .name = "0xFFFollowedBySevenOnes",
        .ops = {{{.value = 0xFFU, .bits = 8}, {.value = 0x7FU, .bits = 7}}},
        .num_ops = 2,
        .expected_bytes = {0xFF, 0x7F},
        .expected_len = 2,
    },
    {
        .name = "0xFFFollowedBy7And4Bits",
        .ops = {{{.value = 0xFFU, .bits = 8},
                 {.value = 0x55U, .bits = 7},
                 {.value = 0xAU, .bits = 4}}},
        .num_ops = 3,
        .expected_bytes = {0xFF, 0x55, 0xA0},
        .expected_len = 3,
    },
    {
        .name = "BackToBack0xFFViaSevenBitGap",
        .ops = {{{.value = 0xFFU, .bits = 8},
                 {.value = 0x7FU, .bits = 7},
                 {.value = 0xFFU, .bits = 8}}},
        .num_ops = 3,
        .expected_bytes = {0xFF, 0x7F, 0xFF, 0x00},
        .expected_len = 4,
    },
    {
        .name = "Non0xFFFollowedBy0xFF",
        .ops = {{{.value = 0xABU, .bits = 8}, {.value = 0xFFU, .bits = 8}}},
        .num_ops = 2,
        .expected_bytes = {0xAB, 0xFF, 0x00},
        .expected_len = 3,
    },
    {
        .name = "CrossWriteBoundaryForms0xFF",
        .ops = {{{.value = 0x0FU, .bits = 4},
                 {.value = 0xFFU, .bits = 8},
                 {.value = 0x01U, .bits = 1}}},
        .num_ops = 3,
        .expected_bytes = {0xFF, 0x7C},
        .expected_len = 2,
    },
};

TEST(BioTest, CreateAndDestroyLifecycleAndNullDestroyIsSafe) {
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());
  bio.reset();

  opj_bio_destroy(nullptr);
}

TEST(BioTest, EmptyFlushEmitsSingleZeroByteAndInalignConsumesZeroBytes) {
  constexpr OPJ_BYTE kSentinel = 0xAA;
  std::array<OPJ_BYTE, 4> buf{};
  buf.fill(kSentinel);

  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  opj_bio_init_enc(bio.get(), buf.data(), static_cast<OPJ_UINT32>(buf.size()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf, ElementsAre(0x00, kSentinel, kSentinel, kSentinel));

  // Inalign with zero bits read leaves the position at 0 bytes.
  opj_bio_init_dec(bio.get(), buf.data(), 1);
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));

  // Reading the emitted zero byte advances numbytes to 1.
  opj_bio_init_dec(bio.get(), buf.data(), 1);
  EXPECT_EQ(0U, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
}

TEST(BioTest, PutbitAndWriteMultiBitRoundTrip) {
  constexpr OPJ_BYTE kSentinel = 0x55;
  std::array<OPJ_BYTE, 12> buf{};
  buf.fill(kSentinel);

  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  opj_bio_init_enc(bio.get(), buf.data(), static_cast<OPJ_UINT32>(buf.size()));
  opj_bio_putbit(bio.get(), 1);
  opj_bio_putbit(bio.get(), 0);
  opj_bio_putbit(bio.get(), 1);
  // Only the lowest 3 bits of 0xFF are written.
  opj_bio_write(bio.get(), 0xFFU, 3);
  opj_bio_write(bio.get(), 0x2DU, 6);
  opj_bio_write(bio.get(), 0xA55AU, 16);
  opj_bio_write(bio.get(), 0x80000001U, 32);

  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  ASSERT_EQ(8, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf, ElementsAre(0xBE, 0xDA, 0x55, 0xA8, 0x00, 0x00, 0x00, 0x10,
                               kSentinel, kSentinel, kSentinel, kSentinel));

  opj_bio_init_dec(bio.get(), buf.data(), 8);
  EXPECT_EQ(1U, opj_bio_read(bio.get(), 1));
  EXPECT_EQ(0U, opj_bio_read(bio.get(), 1));
  EXPECT_EQ(1U, opj_bio_read(bio.get(), 1));
  EXPECT_EQ(0x7U, opj_bio_read(bio.get(), 3));
  EXPECT_EQ(0x2DU, opj_bio_read(bio.get(), 6));
  EXPECT_EQ(0xA55AU, opj_bio_read(bio.get(), 16));
  EXPECT_EQ(0x80000001U, opj_bio_read(bio.get(), 32));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(8, opj_bio_numbytes(bio.get()));
}

TEST(BioTest, AllBitWidthsFrom1To32RoundTrip) {
  constexpr uint32_t kPatterns[] = {0xA55A3C69U, 0xFFFFFFFFU};
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  for (const uint32_t pattern : kPatterns) {
    SCOPED_TRACE(pattern);
    std::vector<OPJ_BYTE> buf(128, 0);
    opj_bio_init_enc(bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(buf.size()));

    for (uint32_t n = 1; n <= 32; ++n) {
      opj_bio_write(bio.get(), pattern & MaskForBits(n), n);
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
    const std::ptrdiff_t encoded_bytes = opj_bio_numbytes(bio.get());
    ASSERT_GT(encoded_bytes, 0);
    ASSERT_LE(encoded_bytes, static_cast<std::ptrdiff_t>(buf.size()));

    opj_bio_init_dec(bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(encoded_bytes));
    for (uint32_t n = 1; n <= 32; ++n) {
      const uint32_t expected = pattern & MaskForBits(n);
      EXPECT_EQ(expected, opj_bio_read(bio.get(), n)) << "bits=" << n;
    }
    EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
    EXPECT_EQ(encoded_bytes, opj_bio_numbytes(bio.get()));
  }
}

TEST(BioTest, ByteStuffingAfter0xFFTableDriven) {
  constexpr OPJ_BYTE kSentinel = 0xAA;
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  for (const StuffingTestCase& tc : kStuffingCases) {
    SCOPED_TRACE(tc.name);
    std::array<OPJ_BYTE, 6> buf{};
    buf.fill(kSentinel);

    opj_bio_init_enc(bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(buf.size()));
    for (std::size_t i = 0; i < tc.num_ops; ++i) {
      opj_bio_write(bio.get(), tc.ops[i].value, tc.ops[i].bits);
    }
    ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
    ASSERT_EQ(static_cast<std::ptrdiff_t>(tc.expected_len),
              opj_bio_numbytes(bio.get()));

    for (std::size_t i = 0; i < tc.expected_len; ++i) {
      EXPECT_EQ(tc.expected_bytes[i], buf[i]) << "byte " << i;
    }
    for (std::size_t i = tc.expected_len; i < buf.size(); ++i) {
      EXPECT_EQ(kSentinel, buf[i]) << "sentinel byte " << i;
    }

    opj_bio_init_dec(bio.get(), buf.data(),
                     static_cast<OPJ_UINT32>(tc.expected_len));
    for (std::size_t i = 0; i < tc.num_ops; ++i) {
      EXPECT_EQ(tc.ops[i].value, opj_bio_read(bio.get(), tc.ops[i].bits))
          << "op " << i;
    }
    EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
    EXPECT_EQ(static_cast<std::ptrdiff_t>(tc.expected_len),
              opj_bio_numbytes(bio.get()));
  }
}

TEST(BioTest, InalignAndByteinStuffingEdgeCases) {
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  // 1. Reading only 4 bits of a 0xFF byte still triggers inalign to consume the
  // trailing stuffed 0x00 byte.
  std::array<OPJ_BYTE, 2> stuffed_zero = {0xFF, 0x00};
  opj_bio_init_dec(bio.get(), stuffed_zero.data(),
                   static_cast<OPJ_UINT32>(stuffed_zero.size()));
  EXPECT_EQ(0xFU, opj_bio_read(bio.get(), 4));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(2, opj_bio_numbytes(bio.get()));

  // 2. If 0xFF has not been read yet, inalign leaves buf untouched at 0 bytes.
  opj_bio_init_dec(bio.get(), stuffed_zero.data(),
                   static_cast<OPJ_UINT32>(stuffed_zero.size()));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));

  // 3. After reading 0xFF, opj_bio_bytein sets ct = 7 and skips bit 7 of the
  // following byte (0xAA = 1010_1010 -> low 7 bits 0x2A = 010_1010).
  std::array<OPJ_BYTE, 2> ff_then_aa = {0xFF, 0xAA};
  opj_bio_init_dec(bio.get(), ff_then_aa.data(),
                   static_cast<OPJ_UINT32>(ff_then_aa.size()));
  EXPECT_EQ(0xFFU, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(0x2AU, opj_bio_read(bio.get(), 7));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(2, opj_bio_numbytes(bio.get()));

  // 4. Consecutive 0xFF bytes in input decode 0xFF then 7-bit 0x7F, leaving
  // (buf & 0xff) == 0xFF so inalign consumes the third byte.
  std::array<OPJ_BYTE, 3> ff_ff_zero = {0xFF, 0xFF, 0x00};
  opj_bio_init_dec(bio.get(), ff_ff_zero.data(),
                   static_cast<OPJ_UINT32>(ff_ff_zero.size()));
  EXPECT_EQ(0xFFU, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(0x7FU, opj_bio_read(bio.get(), 7));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(3, opj_bio_numbytes(bio.get()));
}

TEST(BioTest, FlushFailsOnBufferExhaustion) {
  constexpr OPJ_BYTE kSentinel = 0x5A;
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  // 1. Zero-length buffer fails immediately on empty flush.
  std::array<OPJ_BYTE, 2> buf{};
  buf.fill(kSentinel);
  opj_bio_init_enc(bio.get(), buf.data(), 0);
  EXPECT_EQ(OPJ_FALSE, opj_bio_flush(bio.get()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf, ElementsAre(kSentinel, kSentinel));

  // 2. Zero-length buffer with >8 bits exercises opj_bio_byteout failure inside
  // opj_bio_putbit without out-of-bounds writes.
  opj_bio_init_enc(bio.get(), buf.data(), 0);
  opj_bio_write(bio.get(), 0xABCDU, 16);
  EXPECT_EQ(OPJ_FALSE, opj_bio_flush(bio.get()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf, ElementsAre(kSentinel, kSentinel));

  // 3. One-byte buffer fails in flush when 9 bits require a second byte.
  buf.fill(kSentinel);
  opj_bio_init_enc(bio.get(), buf.data(), 1);
  opj_bio_write(bio.get(), 0x155U, 9);
  EXPECT_EQ(OPJ_FALSE, opj_bio_flush(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf, ElementsAre(0xAA, kSentinel));

  // 4. One-byte buffer emits 0xFF on the first byteout in flush, setting
  // ct == 7, then fails on the second byteout for the stuffed 0x00 byte.
  buf.fill(kSentinel);
  opj_bio_init_enc(bio.get(), buf.data(), 1);
  opj_bio_write(bio.get(), 0xFFU, 8);
  EXPECT_EQ(OPJ_FALSE, opj_bio_flush(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf, ElementsAre(0xFF, kSentinel));

  // 5. Two-byte buffer ending in 0xFF similarly fails on the stuffed byte.
  std::array<OPJ_BYTE, 3> buf3{};
  buf3.fill(kSentinel);
  opj_bio_init_enc(bio.get(), buf3.data(), 2);
  opj_bio_write(bio.get(), 0x55FFU, 16);
  EXPECT_EQ(OPJ_FALSE, opj_bio_flush(bio.get()));
  EXPECT_EQ(2, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf3, ElementsAre(0x55, 0xFF, kSentinel));
}

TEST(BioTest, ReadAndInalignHandleBufferExhaustion) {
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  // 1. Zero-length input produces zero bits on read without OOB access.
  std::array<OPJ_BYTE, 2> buf = {0xFF, 0xFF};
  opj_bio_init_dec(bio.get(), buf.data(), 0);
  EXPECT_EQ(0U, opj_bio_read(bio.get(), 16));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));

  // 2. Truncated 1-byte {0xFF} input fails inalign after reading 8 bits.
  opj_bio_init_dec(bio.get(), buf.data(), 1);
  EXPECT_EQ(0xFFU, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
  EXPECT_EQ(OPJ_FALSE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));

  // 3. Truncated 1-byte {0xFF} input also fails inalign after reading 4 bits.
  opj_bio_init_dec(bio.get(), buf.data(), 1);
  EXPECT_EQ(0xFU, opj_bio_read(bio.get(), 4));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
  EXPECT_EQ(OPJ_FALSE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));

  // 4. Reading past end after 0xFF exercises bytein ct == 7 with bp >= end.
  opj_bio_init_dec(bio.get(), buf.data(), 1);
  EXPECT_EQ(0xFFU, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(0U, opj_bio_read(bio.get(), 7));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));

  // 5. Two-byte {0xFF, 0xFF} fails inalign after reading 8 + 7 bits.
  opj_bio_init_dec(bio.get(), buf.data(), 2);
  EXPECT_EQ(0xFFU, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(0x7FU, opj_bio_read(bio.get(), 7));
  EXPECT_EQ(OPJ_FALSE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(2, opj_bio_numbytes(bio.get()));
}

TEST(BioTest, ReusingHandleResetsAllInternalState) {
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  // 1. Leave partial 0xFF + extra bits in encoder state without flushing.
  std::array<OPJ_BYTE, 4> buf1{};
  opj_bio_init_enc(bio.get(), buf1.data(),
                   static_cast<OPJ_UINT32>(buf1.size()));
  opj_bio_write(bio.get(), 0xFFU, 8);
  opj_bio_write(bio.get(), 0x3U, 2);
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));

  // 2. Re-initializing encoder resets buf, ct, and buffer bounds cleanly.
  std::array<OPJ_BYTE, 4> buf2 = {0x99, 0x99, 0x99, 0x99};
  opj_bio_init_enc(bio.get(), buf2.data(),
                   static_cast<OPJ_UINT32>(buf2.size()));
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  opj_bio_write(bio.get(), 0xA5U, 8);
  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
  EXPECT_THAT(buf2, ElementsAre(0xA5, 0x99, 0x99, 0x99));

  // 3. Leave decoder mid-byte with (buf & 0xff) == 0xff, then re-initialize
  // decoder on an unrelated buffer.
  std::array<OPJ_BYTE, 1> ff_buf = {0xFF};
  opj_bio_init_dec(bio.get(), ff_buf.data(), 1);
  EXPECT_EQ(0xFU, opj_bio_read(bio.get(), 4));

  std::array<OPJ_BYTE, 1> clean_buf = {0x42};
  opj_bio_init_dec(bio.get(), clean_buf.data(), 1);
  EXPECT_EQ(0, opj_bio_numbytes(bio.get()));
  EXPECT_EQ(0x42U, opj_bio_read(bio.get(), 8));
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(1, opj_bio_numbytes(bio.get()));
}

TEST(BioTest, MixedLongSequenceWithMultiple0xFFRoundTrips) {
  std::vector<BitOp> ops;
  ops.reserve(120);
  uint32_t rng = 0x13579BDFU;
  for (int i = 0; i < 120; ++i) {
    rng = rng * 1664525U + 1013904223U;
    uint32_t bits = (rng % 32U) + 1U;
    if (i % 7 == 0) {
      bits = 8U;
    } else if (i % 11 == 0) {
      bits = 1U;
    }
    // Inject periodic all-ones bytes to exercise repeated 0xFF stuffing.
    const uint32_t value =
        (i % 7 == 0) ? 0xFFU : ((rng >> 5) & MaskForBits(bits));
    ops.push_back(BitOp{.value = value, .bits = bits});
  }

  std::vector<OPJ_BYTE> encoded(1024, 0);
  OpjBioPtr bio = CreateBio();
  ASSERT_NE(nullptr, bio.get());

  opj_bio_init_enc(bio.get(), encoded.data(),
                   static_cast<OPJ_UINT32>(encoded.size()));
  for (const BitOp& op : ops) {
    if (op.bits == 1) {
      opj_bio_putbit(bio.get(), op.value);
    } else {
      opj_bio_write(bio.get(), op.value, op.bits);
    }
  }
  ASSERT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  const std::ptrdiff_t encoded_bytes = opj_bio_numbytes(bio.get());
  ASSERT_GT(encoded_bytes, 0);
  ASSERT_LE(encoded_bytes, static_cast<std::ptrdiff_t>(encoded.size()));

  opj_bio_init_dec(bio.get(), encoded.data(),
                   static_cast<OPJ_UINT32>(encoded_bytes));
  for (std::size_t i = 0; i < ops.size(); ++i) {
    EXPECT_EQ(ops[i].value, opj_bio_read(bio.get(), ops[i].bits)) << "op " << i;
  }
  EXPECT_EQ(OPJ_TRUE, opj_bio_inalign(bio.get()));
  EXPECT_EQ(encoded_bytes, opj_bio_numbytes(bio.get()));
}

}  // namespace
}  // namespace openjpeg2
