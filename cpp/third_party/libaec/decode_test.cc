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
// Tests libaec adaptive entropy decoder, seeking, and range decoding
// (third_party/libaec/src/decode.c).
// Note: ENABLE_RSI_PADDING is disabled in google3, so encoder outputs omit RSI
// byte padding; AEC_PAD_RSI on decode is tested with synthesized CCSDS bytes.
// Never include src/encode.h here (colliding internal_state struct).
//
// See also:
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/decode.c
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/decode.h

#include <algorithm>
#include <array>
#include <barrier>  // NOLINT
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>    // NOLINT
#include <thread>  // NOLINT
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/libaec/include/libaec.h"

// clang-format off
extern "C" {
#include "third_party/libaec/src/decode.h"
}  // extern "C"
// clang-format on

namespace libaec {
namespace {

using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(AEC_OK == 0);

// MSB-first bit writer for synthesizing exact CCSDS 121.0-B-3 test bitstreams.
class BitWriter {
 public:
  void WriteBits(uint32_t value, int bit_count) {
    for (int i = bit_count - 1; i >= 0; --i) {
      current_ = static_cast<uint8_t>((current_ << 1) | ((value >> i) & 1u));
      if (++bits_in_current_ == 8) {
        bytes_.push_back(current_);
        current_ = 0;
        bits_in_current_ = 0;
      }
    }
  }

  void WriteZeroes(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      WriteBits(0, 1);
    }
  }

  void PadToByte() {
    if (bits_in_current_ > 0) {
      current_ = static_cast<uint8_t>(current_ << (8 - bits_in_current_));
      bytes_.push_back(current_);
      current_ = 0;
      bits_in_current_ = 0;
    }
  }

  std::vector<uint8_t> Finish() {
    PadToByte();
    return bytes_;
  }

 private:
  std::vector<uint8_t> bytes_;
  uint8_t current_ = 0;
  int bits_in_current_ = 0;
};

std::vector<uint8_t> EncodeBuffer(unsigned int bits_per_sample,
                                  unsigned int block_size, unsigned int rsi,
                                  unsigned int flags,
                                  std::span<const uint8_t> input) {
  std::vector<uint8_t> encoded(input.size() * 2 + 128, 0xA5);
  aec_stream enc{};
  enc.bits_per_sample = bits_per_sample;
  enc.block_size = block_size;
  enc.rsi = rsi;
  enc.flags = flags;
  enc.next_in = input.data();
  enc.avail_in = input.size();
  enc.next_out = encoded.data();
  enc.avail_out = encoded.size();
  EXPECT_EQ(AEC_OK, aec_buffer_encode(&enc));
  encoded.resize(enc.total_out);
  return encoded;
}

void VerifyBufferDecode(unsigned int bits_per_sample, unsigned int block_size,
                        unsigned int rsi, unsigned int flags,
                        std::span<const uint8_t> encoded,
                        std::span<const uint8_t> expected) {
  constexpr size_t kTrail = 16;
  std::vector<uint8_t> decoded(expected.size() + kTrail, 0xA5);
  aec_stream dec{};
  dec.bits_per_sample = bits_per_sample;
  dec.block_size = block_size;
  dec.rsi = rsi;
  dec.flags = flags;
  dec.next_in = encoded.data();
  dec.avail_in = encoded.size();
  dec.next_out = decoded.data();
  dec.avail_out = expected.size();
  ASSERT_EQ(AEC_OK, aec_buffer_decode(&dec));
  EXPECT_EQ(expected.size(), dec.total_out);
  EXPECT_THAT(std::span<const uint8_t>(decoded).first(expected.size()),
              ElementsAreArray(expected));
  EXPECT_THAT(std::span<const uint8_t>(decoded).subspan(expected.size()),
              Each(0xA5));
}

void VerifySlowDecode(unsigned int bits_per_sample, unsigned int block_size,
                      unsigned int rsi, unsigned int flags,
                      size_t bytes_per_sample, std::span<const uint8_t> encoded,
                      std::span<const uint8_t> expected) {
  aec_stream dec{};
  dec.bits_per_sample = bits_per_sample;
  dec.block_size = block_size;
  dec.rsi = rsi;
  dec.flags = flags;
  ASSERT_EQ(AEC_OK, aec_decode_init(&dec));
  absl::Cleanup end_stream = [&dec] {
    EXPECT_EQ(AEC_OK, aec_decode_end(&dec));
  };

  std::vector<uint8_t> decoded(expected.size(), 0xA5);
  size_t in_pos = 0;
  size_t out_pos = 0;
  int idle_steps = 0;

  while (out_pos < expected.size() && idle_steps < 16) {
    const size_t avail_in_chunk = (in_pos < encoded.size()) ? 1 : 0;
    const size_t avail_out_chunk =
        std::min(bytes_per_sample, expected.size() - out_pos);
    dec.next_in = encoded.data() + in_pos;
    dec.avail_in = avail_in_chunk;
    dec.next_out = decoded.data() + out_pos;
    dec.avail_out = avail_out_chunk;
    ASSERT_EQ(AEC_OK, aec_decode(&dec, AEC_NO_FLUSH));

    const size_t in_used = avail_in_chunk - dec.avail_in;
    const size_t out_written = avail_out_chunk - dec.avail_out;
    in_pos += in_used;
    out_pos += out_written;
    idle_steps = (in_used == 0 && out_written == 0) ? (idle_steps + 1) : 0;
  }

  EXPECT_EQ(expected.size(), out_pos);
  EXPECT_THAT(decoded, ElementsAreArray(expected));
}

TEST(DecodeTest, InitValidatesConfigurationParameters) {
  struct InitCase {
    unsigned int bits_per_sample;
    unsigned int block_size;
    unsigned int rsi;
    unsigned int flags;
    int expected_status;
    int expected_id_len;
  };
  constexpr auto kCases = std::to_array<InitCase>({
      // Invalid bits_per_sample, rsi, and block_size bounds.
      {.bits_per_sample = 0,
       .block_size = 8,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 33,
       .block_size = 8,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 8,
       .rsi = 0,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 8,
       .rsi = 4097,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 0,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 3,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 258,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      // AEC_RESTRICTED allows 1..4 bits and rejects 5..8 bits.
      {.bits_per_sample = 2,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_OK,
       .expected_id_len = 1},
      {.bits_per_sample = 4,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_OK,
       .expected_id_len = 2},
      {.bits_per_sample = 6,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      {.bits_per_sample = 8,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_RESTRICTED,
       .expected_status = AEC_CONF_ERROR,
       .expected_id_len = 0},
      // Standard & wide widths.
      {.bits_per_sample = 8,
       .block_size = 256,
       .rsi = 1,
       .flags = 0,
       .expected_status = AEC_OK,
       .expected_id_len = 3},
      {.bits_per_sample = 16,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_DATA_MSB,
       .expected_status = AEC_OK,
       .expected_id_len = 4},
      {.bits_per_sample = 24,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_DATA_3BYTE | AEC_DATA_MSB,
       .expected_status = AEC_OK,
       .expected_id_len = 5},
      {.bits_per_sample = 32,
       .block_size = 8,
       .rsi = 1,
       .flags = AEC_DATA_SIGNED,
       .expected_status = AEC_OK,
       .expected_id_len = 5},
  });

  for (size_t i = 0; i < kCases.size(); ++i) {
    const InitCase& tc = kCases[i];
    SCOPED_TRACE(i);
    aec_stream dec{};
    dec.bits_per_sample = tc.bits_per_sample;
    dec.block_size = tc.block_size;
    dec.rsi = tc.rsi;
    dec.flags = tc.flags;
    const int status = aec_decode_init(&dec);
    EXPECT_EQ(tc.expected_status, status);
    if (status == AEC_OK) {
      // Failed init already frees state without nulling dec.state; only call
      // aec_decode_end after AEC_OK.
      absl::Cleanup end_stream = [&dec] {
        EXPECT_EQ(AEC_OK, aec_decode_end(&dec));
      };
      EXPECT_EQ(tc.expected_id_len, dec.state->id_len);
    } else {
      EXPECT_EQ(tc.expected_status, aec_buffer_decode(&dec));
    }
  }
}

TEST(DecodeTest, FlushCoversAllOutputFormattersAndPostprocessing) {
  // 1. Unsigned 8-bit values crossing all four flush post-processing branches.
  {
    constexpr auto kUnsigned = std::to_array<uint8_t>({
        100,
        120,
        10,
        200,
        100,
        50,
        240,
        10,
        10,
        10,
        20,
        5,
        250,
        2,
        128,
        129,
    });
    const auto enc = EncodeBuffer(8, 8, 2, AEC_DATA_PREPROCESS, kUnsigned);
    VerifyBufferDecode(8, 8, 2, AEC_DATA_PREPROCESS, enc, kUnsigned);
  }

  // 2. Signed 8-bit values crossing negative ref sign extension and signed
  // wrapping branches.
  {
    constexpr auto kSignedVals = std::to_array<int8_t>({
        -100,
        -120,
        10,
        50,
        -120,
        100,
        120,
        -50,
        -10,
        20,
        5,
        -5,
        -127,
        126,
        0,
        -1,
    });
    constexpr auto kBytes =
        std::bit_cast<std::array<uint8_t, kSignedVals.size()>>(kSignedVals);
    const auto enc =
        EncodeBuffer(8, 8, 2, AEC_DATA_SIGNED | AEC_DATA_PREPROCESS, kBytes);
    VerifyBufferDecode(8, 8, 2, AEC_DATA_SIGNED | AEC_DATA_PREPROCESS, enc,
                       kBytes);
  }

  // 3. 16-bit LSB/MSB, 24-bit 3-byte LSB/MSB, and 32-bit LSB/MSB formatters.
  constexpr size_t kSamples = 16;
  auto test_width = [kSamples](unsigned int bits, size_t bps,
                               unsigned int flags) {
    std::vector<uint8_t> data(kSamples * bps);
    const bool msb = (flags & AEC_DATA_MSB) != 0;
    for (size_t i = 0; i < kSamples; ++i) {
      const uint32_t v =
          (bits == 32 ? 0x80000100u : 0x001200u) + static_cast<uint32_t>(i * 7);
      for (size_t b = 0; b < bps; ++b) {
        const size_t shift = (msb ? (bps - 1 - b) : b) * 8;
        data[i * bps + b] = static_cast<uint8_t>((v >> shift) & 0xFFu);
      }
    }
    const auto enc = EncodeBuffer(bits, 8, 2, flags, data);
    VerifyBufferDecode(bits, 8, 2, flags, enc, data);
  };
  test_width(16, 2, AEC_DATA_PREPROCESS);
  test_width(16, 2, AEC_DATA_MSB | AEC_DATA_PREPROCESS);
  test_width(24, 3, AEC_DATA_3BYTE | AEC_DATA_PREPROCESS);
  test_width(24, 3, AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS);
  test_width(32, 4, AEC_DATA_PREPROCESS | AEC_DATA_SIGNED);
  test_width(32, 4, AEC_DATA_MSB);
}

TEST(DecodeTest, FastPathCoversSplittingZeroSeAndUncompModes) {
  // Build a large multi-RSI 16-bit stream so earlier blocks strictly satisfy
  // BUFFERSPACE (avail_in >= in_blklen && avail_out >= out_blklen) and exercise
  // m_split (k == 0 and k > 0), m_zero_block (<5, >=5, ROS), m_se (ref=0 and
  // ref=1), m_uncomp, and direct_get_fs (>56 zero bits) on the fast path.
  constexpr unsigned int kBlockSize = 8;
  constexpr unsigned int kRsi = 16;
  constexpr size_t kTotalBlocks = kRsi * 3;
  std::vector<uint8_t> input;
  auto push16 = [&input](uint16_t v) {
    input.push_back(static_cast<uint8_t>(v & 0xFFu));
    input.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  };

  uint32_t rng = 0xCAFEBABEu;
  for (size_t b = 0; b < kTotalBlocks; ++b) {
    const size_t in_rsi = b % kRsi;
    if (in_rsi == 0 || in_rsi == 1) {
      // Zero blocks (short run before non-zero block).
      for (size_t i = 0; i < kBlockSize; ++i) push16(0);
    } else if (in_rsi == 2) {
      // Second extension friendly pairs {0, 1}.
      for (size_t i = 0; i < kBlockSize; ++i)
        push16(static_cast<uint16_t>(i & 1));
    } else if (in_rsi >= 3 && in_rsi <= 8) {
      // 6 zero blocks (>= 5, middle run).
      for (size_t i = 0; i < kBlockSize; ++i) push16(0);
    } else if (in_rsi == 9) {
      // k == 0 splitting block (small 0..1 values that favor FS with k=0 or
      // SE).
      for (size_t i = 0; i < kBlockSize; ++i) push16(i == 0 ? 1 : 0);
    } else if (in_rsi == 10) {
      // High FS value (>70 consecutive zero bits) triggering direct_get_fs
      // 56-bit refill loop while k > 0 keeps splitting cheaper than uncomp.
      push16(400);
      for (size_t i = 1; i < kBlockSize; ++i) push16(1);
    } else if (in_rsi == 11) {
      // Random high-entropy block triggering m_uncomp.
      for (size_t i = 0; i < kBlockSize; ++i) {
        rng = rng * 1664525u + 1013904223u;
        push16(static_cast<uint16_t>(rng >> 16));
      }
    } else {
      // Remaining 4..5 blocks at end of RSI: zeros ending in ROS when >=5.
      for (size_t i = 0; i < kBlockSize; ++i) push16(b % 2 == 0 ? 0 : 7);
    }
  }

  // 1. Test both unpreprocessed (ref == 0) and preprocessed (ref == 1).
  for (unsigned int flags :
       {0u, static_cast<unsigned int>(AEC_DATA_PREPROCESS)}) {
    const auto enc = EncodeBuffer(16, kBlockSize, kRsi, flags, input);
    EXPECT_THAT(enc, Not(IsEmpty()));
    VerifyBufferDecode(16, kBlockSize, kRsi, flags, enc, input);
  }

  // 2. Explicitly ensure ROS zero-block run and k==0 splitting fast path run
  // with plenty of trailing input space in an 8-bit stream.
  std::vector<uint8_t> ros_and_k0;
  for (int b = 0; b < 6; ++b) {
    for (size_t i = 0; i < kBlockSize; ++i) ros_and_k0.push_back(0);
  }
  const auto enc_ros = EncodeBuffer(8, kBlockSize, 6, 0, ros_and_k0);
  VerifyBufferDecode(8, kBlockSize, 6, 0, enc_ros, ros_and_k0);

  // 3. Fast-path direct_get across varying bit depths + k > 0 binary splits.
  std::vector<uint8_t> multi_k(10 * kBlockSize * 2);
  for (size_t i = 0; i < 10 * kBlockSize; ++i) {
    const uint16_t v =
        static_cast<uint16_t>(((i / kBlockSize) << 4) + (i % 13));
    multi_k[2 * i] = static_cast<uint8_t>(v & 0xFFu);
    multi_k[2 * i + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  }
  const auto enc_multi_k = EncodeBuffer(16, kBlockSize, 10, 0, multi_k);
  VerifyBufferDecode(16, kBlockSize, 10, 0, enc_multi_k, multi_k);
}

TEST(DecodeTest, SlowPathOneByteInOneSampleOutCoversAllStates) {
  // Exercise every slow-path state (m_id, m_low_entropy, m_low_entropy_ref,
  // m_split_fs, m_split_output, m_se_decode even & odd pairs, m_zero_output,
  // m_uncomp_copy) by streaming 1 byte in and 1 sample out at a time.
  constexpr unsigned int kBlockSize = 8;
  constexpr unsigned int kRsi = 10;
  std::vector<uint8_t> input;
  auto push16 = [&input](uint16_t v) {
    input.push_back(static_cast<uint8_t>(v & 0xFFu));
    input.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  };

  // Block 0 (ref == 1 when PP): SE pairs {2,2,...,1} -> odd sample_counter.
  for (int i = 0; i < 7; ++i) push16(2);
  push16(1);
  // Blocks 1..6: zero deltas -> zero run >= 5.
  for (int i = 0; i < 6 * 8; ++i) push16(1);
  // Block 7: SE with ref == 0 (all pairs equal except last -> {0,1,...}).
  push16(1);
  for (int i = 1; i < 8; ++i) push16(static_cast<uint16_t>(1 + (i & 1)));
  // Block 8: moderate deltas -> splitting k > 0.
  for (int i = 0; i < 8; ++i) push16(static_cast<uint16_t>(100 + i * 17));
  // Block 9: high-entropy deltas -> uncompressed block.
  uint32_t rng = 0x13572468u;
  for (int i = 0; i < 8; ++i) {
    rng = rng * 1664525u + 1013904223u;
    push16(static_cast<uint16_t>(rng >> 16));
  }

  // 1. 16-bit stream covering both unpreprocessed and preprocessed modes.
  for (unsigned int flags :
       {0u, static_cast<unsigned int>(AEC_DATA_PREPROCESS)}) {
    const auto enc = EncodeBuffer(16, kBlockSize, kRsi, flags, input);
    SCOPED_TRACE(flags);
    VerifySlowDecode(16, kBlockSize, kRsi, flags, 2, enc, input);
  }

  // 2. Specifically target m_split_output with k == 0 and k > 0 slow path plus
  // m_se_decode pause between odd/even pair outputs by using exact 1-sample
  // avail_out chunks on 8-bit unpreprocessed data (including large FS).
  constexpr auto kEightBit = std::to_array<uint8_t>({
      0, 1, 0, 1, 0, 1, 0, 1,  // SE (ref=0, pair outputs alternate)
      0, 0, 0, 0, 0, 0, 0, 1,  // Split k == 0
      4, 5, 4, 5, 6, 7, 4, 5,  // Split k == 1
  });
  const auto enc8 = EncodeBuffer(8, 8, 3, 0, kEightBit);
  VerifySlowDecode(8, 8, 3, 0, 1, enc8, kEightBit);
}

TEST(DecodeTest, SynthesizedPadRsiDecodesAcrossByteAlignedBoundaries) {
  // ENABLE_RSI_PADDING is disabled on encode in google3, so synthesize a
  // two-RSI 8-bit uncompressed CCSDS bitstream padded to a byte boundary after
  // each RSI (id = 0b111 + 8 * 8 bits = 67 bits + 5 zero pad bits = 9 bytes).
  BitWriter writer;
  constexpr auto kRsi0 =
      std::to_array<uint8_t>({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
  constexpr auto kRsi1 =
      std::to_array<uint8_t>({0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01, 0x02, 0x03});

  writer.WriteBits(0b111, 3);
  for (uint8_t s : kRsi0) writer.WriteBits(s, 8);
  writer.PadToByte();

  writer.WriteBits(0b111, 3);
  for (uint8_t s : kRsi1) writer.WriteBits(s, 8);
  const std::vector<uint8_t> padded = writer.Finish();

  std::vector<uint8_t> expected;
  expected.insert(expected.end(), kRsi0.begin(), kRsi0.end());
  expected.insert(expected.end(), kRsi1.begin(), kRsi1.end());

  VerifyBufferDecode(8, 8, 1, AEC_PAD_RSI, padded, expected);
}

TEST(DecodeTest, OffsetsSeekAndRangeDecodingWorkAndValidateBounds) {
  // 1. Encode a 4-RSI preprocessed stream using variable block lengths so RSI
  // bit offsets include non-byte-aligned bit positions.
  constexpr unsigned int kBlockSize = 8;
  constexpr unsigned int kRsi = 2;
  constexpr size_t kSamplesPerRsi = kBlockSize * kRsi;
  std::vector<uint8_t> input(4 * kSamplesPerRsi);
  for (size_t i = 0; i < input.size(); ++i) {
    // Alternate smooth and stepped patterns across RSIs.
    input[i] = static_cast<uint8_t>(
        (i < 2 * kSamplesPerRsi) ? (i * 3) : ((i * 37) & 0xFF));
  }
  const auto enc =
      EncodeBuffer(8, kBlockSize, kRsi, AEC_DATA_PREPROCESS, input);

  // 2. Decode with offsets enabled and verify offset API error checks.
  aec_stream dec{};
  dec.bits_per_sample = 8;
  dec.block_size = kBlockSize;
  dec.rsi = kRsi;
  dec.flags = AEC_DATA_PREPROCESS;
  ASSERT_EQ(AEC_OK, aec_decode_init(&dec));
  absl::Cleanup end_stream = [&dec] {
    EXPECT_EQ(AEC_OK, aec_decode_end(&dec));
  };

  size_t count = 99;
  EXPECT_EQ(AEC_RSI_OFFSETS_ERROR, aec_decode_count_offsets(&dec, &count));
  EXPECT_EQ(0u, count);
  std::array<size_t, 4> dummy_offsets{};
  EXPECT_EQ(
      AEC_RSI_OFFSETS_ERROR,
      aec_decode_get_offsets(&dec, dummy_offsets.data(), dummy_offsets.size()));

  ASSERT_EQ(AEC_OK, aec_decode_enable_offsets(&dec));
  EXPECT_EQ(AEC_RSI_OFFSETS_ERROR, aec_decode_enable_offsets(&dec));

  std::vector<uint8_t> full_out(input.size(), 0xA5);
  dec.next_in = enc.data();
  dec.avail_in = enc.size();
  dec.next_out = full_out.data();
  dec.avail_out = full_out.size();
  ASSERT_EQ(AEC_OK, aec_decode(&dec, AEC_FLUSH));
  EXPECT_THAT(full_out, ElementsAreArray(input));

  ASSERT_EQ(AEC_OK, aec_decode_count_offsets(&dec, &count));
  ASSERT_EQ(5u, count);  // Initial offset 0 + 4 RSI end offsets.

  std::array<size_t, 2> tiny_offsets{};
  EXPECT_EQ(AEC_MEM_ERROR, aec_decode_get_offsets(&dec, tiny_offsets.data(),
                                                  tiny_offsets.size()));

  std::vector<size_t> offsets(count);
  ASSERT_EQ(AEC_OK,
            aec_decode_get_offsets(&dec, offsets.data(), offsets.size()));
  EXPECT_EQ(0u, offsets[0]);
  for (size_t i = 1; i < offsets.size(); ++i) {
    EXPECT_GT(offsets[i], offsets[i - 1]);
  }

  // 3. Test aec_buffer_seek at bit_offset == 0 (offset 0), bit_offset > 0 (RSI
  // offset or non-multiple-of-8 bit offset), and out-of-bounds errors.
  {
    aec_stream seek_dec{};
    seek_dec.bits_per_sample = 8;
    seek_dec.block_size = kBlockSize;
    seek_dec.rsi = kRsi;
    seek_dec.flags = AEC_DATA_PREPROCESS;
    ASSERT_EQ(AEC_OK, aec_decode_init(&seek_dec));
    absl::Cleanup end_seek = [&seek_dec] {
      EXPECT_EQ(AEC_OK, aec_decode_end(&seek_dec));
    };

    seek_dec.next_in = enc.data();
    seek_dec.avail_in = enc.size();
    EXPECT_EQ(AEC_MEM_ERROR, aec_buffer_seek(&seek_dec, (enc.size() + 2) * 8));
    EXPECT_EQ(AEC_MEM_ERROR, aec_buffer_seek(&seek_dec, enc.size() * 8 + 3));
    // Reset input cursors after the mutating AEC_MEM_ERROR seek above.
    seek_dec.next_in = enc.data();
    seek_dec.avail_in = enc.size();
    ASSERT_EQ(AEC_OK, aec_buffer_seek(&seek_dec, offsets[2]));

    std::vector<uint8_t> tail(2 * kSamplesPerRsi, 0xA5);
    seek_dec.next_out = tail.data();
    seek_dec.avail_out = tail.size();
    ASSERT_EQ(AEC_OK, aec_decode(&seek_dec, AEC_FLUSH));
    EXPECT_THAT(tail, ElementsAreArray(std::span<const uint8_t>(input).subspan(
                          2 * kSamplesPerRsi)));
  }

  // 4. Test aec_decode_range valid slices (within exact input.size()) and
  // error checks (undersized out, overflow, RSI index OOB, bad seek, bad data).
  for (unsigned int range_flags :
       {static_cast<unsigned int>(AEC_DATA_PREPROCESS), 0u}) {
    const std::span<const uint8_t> r_input(input);
    const auto r_enc = EncodeBuffer(8, kBlockSize, kRsi, range_flags, r_input);

    aec_stream off_dec{};
    off_dec.bits_per_sample = 8;
    off_dec.block_size = kBlockSize;
    off_dec.rsi = kRsi;
    off_dec.flags = range_flags;
    ASSERT_EQ(AEC_OK, aec_decode_init(&off_dec));
    absl::Cleanup end_off = [&off_dec] {
      EXPECT_EQ(AEC_OK, aec_decode_end(&off_dec));
    };
    ASSERT_EQ(AEC_OK, aec_decode_enable_offsets(&off_dec));
    std::vector<uint8_t> tmp(r_input.size());
    off_dec.next_in = r_enc.data();
    off_dec.avail_in = r_enc.size();
    off_dec.next_out = tmp.data();
    off_dec.avail_out = tmp.size();
    ASSERT_EQ(AEC_OK, aec_decode(&off_dec, AEC_FLUSH));

    size_t r_count = 0;
    ASSERT_EQ(AEC_OK, aec_decode_count_offsets(&off_dec, &r_count));
    std::vector<size_t> r_offsets(r_count);
    ASSERT_EQ(AEC_OK, aec_decode_get_offsets(&off_dec, r_offsets.data(),
                                             r_offsets.size()));

    // Keep pos + size <= input.size() so temporary output is fully initialized.
    constexpr size_t kPos = kSamplesPerRsi + 3;
    constexpr size_t kSliceSize = 7;
    std::vector<uint8_t> slice(kSliceSize, 0xA5);

    off_dec.next_in = r_enc.data();
    off_dec.avail_in = r_enc.size();
    off_dec.next_out = slice.data();
    off_dec.avail_out = slice.size();
    ASSERT_EQ(AEC_OK, aec_decode_range(&off_dec, r_offsets.data(),
                                       r_offsets.size(), kPos, kSliceSize));
    EXPECT_THAT(slice, ElementsAreArray(r_input.subspan(kPos, kSliceSize)));

    if (range_flags == AEC_DATA_PREPROCESS) {
      // Undersized output buffer -> AEC_MEM_ERROR.
      off_dec.next_in = r_enc.data();
      off_dec.avail_in = r_enc.size();
      off_dec.next_out = slice.data();
      off_dec.avail_out = kSliceSize - 1;
      EXPECT_EQ(AEC_MEM_ERROR,
                aec_decode_range(&off_dec, r_offsets.data(), r_offsets.size(),
                                 kPos, kSliceSize));

      // Overflow guard -> AEC_DATA_ERROR before malloc.
      off_dec.avail_out = std::numeric_limits<size_t>::max();
      EXPECT_EQ(AEC_DATA_ERROR,
                aec_decode_range(&off_dec, r_offsets.data(), r_offsets.size(),
                                 0, std::numeric_limits<size_t>::max()));

      // Out-of-bounds RSI index -> AEC_DATA_ERROR.
      off_dec.avail_out = slice.size();
      EXPECT_EQ(AEC_DATA_ERROR,
                aec_decode_range(&off_dec, r_offsets.data(), r_offsets.size(),
                                 r_offsets.size() * kSamplesPerRsi, 1));

      // Seek error inside aec_decode_range -> AEC_MEM_ERROR.
      std::array<size_t, 1> bad_seek = {r_enc.size() * 8 + 16};
      EXPECT_EQ(AEC_MEM_ERROR, aec_decode_range(&off_dec, bad_seek.data(),
                                                bad_seek.size(), 0, 1));

      // Corrupt data at offset -> AEC_DATA_ERROR from inner aec_decode.
      BitWriter bad_writer;
      bad_writer.WriteBits(0b000, 3);  // Low entropy
      bad_writer.WriteBits(1, 1);      // Second extension
      bad_writer.WriteBits(0, 8);      // Reference sample (AEC_DATA_PREPROCESS)
      bad_writer.WriteZeroes(SE_TABLE_SIZE + 5);
      bad_writer.WriteBits(1, 1);
      std::vector<uint8_t> bad_stream = bad_writer.Finish();
      bad_stream.resize(128, 0x00);
      off_dec.next_in = bad_stream.data();
      off_dec.avail_in = bad_stream.size();
      EXPECT_EQ(AEC_DATA_ERROR, aec_decode_range(&off_dec, r_offsets.data(),
                                                 r_offsets.size(), 0, 4));
    }
  }
}

TEST(DecodeTest, RejectsCorruptStreamsAndTinyOutputBuffers) {
  // 1. Corrupt Second Extension code (> SE_TABLE_SIZE == 90) on BOTH fast path
  // (BUFFERSPACE true) and slow streaming path (1 byte in at a time).
  {
    BitWriter writer;
    writer.WriteBits(0b000, 3);  // Low entropy
    writer.WriteBits(1, 1);      // Second extension
    writer.WriteZeroes(SE_TABLE_SIZE + 5);
    writer.WriteBits(1, 1);
    std::vector<uint8_t> bad_se = writer.Finish();
    bad_se.resize(64, 0x00);

    std::vector<uint8_t> out(64, 0xA5);
    aec_stream fast_dec{};
    fast_dec.bits_per_sample = 8;
    fast_dec.block_size = 8;
    fast_dec.rsi = 1;
    fast_dec.next_in = bad_se.data();
    fast_dec.avail_in = bad_se.size();
    fast_dec.next_out = out.data();
    fast_dec.avail_out = out.size();
    EXPECT_EQ(AEC_DATA_ERROR, aec_buffer_decode(&fast_dec));

    aec_stream slow_dec{};
    slow_dec.bits_per_sample = 8;
    slow_dec.block_size = 8;
    slow_dec.rsi = 1;
    ASSERT_EQ(AEC_OK, aec_decode_init(&slow_dec));
    absl::Cleanup end_slow = [&slow_dec] {
      EXPECT_EQ(AEC_OK, aec_decode_end(&slow_dec));
    };
    int status = AEC_OK;
    for (size_t i = 0; i < bad_se.size() && status == AEC_OK; ++i) {
      slow_dec.next_in = &bad_se[i];
      slow_dec.avail_in = 1;
      slow_dec.next_out = out.data();
      slow_dec.avail_out = out.size();
      status = aec_decode(&slow_dec, AEC_NO_FLUSH);
    }
    EXPECT_EQ(AEC_DATA_ERROR, status);
  }

  // 2. Corrupt zero-block count overflowing RSI size -> AEC_DATA_ERROR.
  {
    BitWriter writer;
    writer.WriteBits(0b000, 3);  // Low entropy
    writer.WriteBits(0, 1);      // Zero blocks
    writer.WriteZeroes(6);       // zero_blocks = 7 - 1 = 6 > rsi (2)
    writer.WriteBits(1, 1);
    const std::vector<uint8_t> bad_zero = writer.Finish();

    std::vector<uint8_t> out(16, 0xA5);
    aec_stream dec{};
    dec.bits_per_sample = 8;
    dec.block_size = 8;
    dec.rsi = 2;
    dec.next_in = bad_zero.data();
    dec.avail_in = bad_zero.size();
    dec.next_out = out.data();
    dec.avail_out = out.size();
    EXPECT_EQ(AEC_DATA_ERROR, aec_buffer_decode(&dec));
  }

  // 3. Premature EOF on fast-path m_split: 9-bit k = 13 needs
  // binary_part = 113, while avail_in after m_id's 7-byte read is 89 >=
  // in_blklen (88) -> BUFFERSPACE passes and m_split returns M_ERROR.
  {
    BitWriter writer;
    writer.WriteBits(14, 4);  // id = 14 -> k = 13
    std::vector<uint8_t> short_split = writer.Finish();
    short_split.resize(96, 0x00);

    std::vector<uint8_t> out(128, 0xA5);
    aec_stream dec{};
    dec.bits_per_sample = 9;
    dec.block_size = 64;
    dec.rsi = 1;
    dec.next_in = short_split.data();
    dec.avail_in = short_split.size();
    dec.next_out = out.data();
    dec.avail_out = out.size();
    EXPECT_EQ(AEC_DATA_ERROR, aec_buffer_decode(&dec));
  }

  // 4. Premature EOF inside direct_get_fs (>7 bytes left all zeros, then <7
  // bytes remain) returns cleanly without OOB reads.
  {
    BitWriter writer;
    writer.WriteBits(0b010,
                     3);  // m_split k = 1 -> direct_get_fs with 0x00 tail
    std::vector<uint8_t> zero_fs = writer.Finish();
    // 32 >= in_blklen (24) + 7 so m_id's 7-byte read leaves avail_in >=
    // in_blklen, letting BUFFERSPACE pass and fast-path direct_get_fs run.
    zero_fs.resize(32, 0x00);

    std::vector<uint8_t> out(64, 0xA5);
    aec_stream dec{};
    dec.bits_per_sample = 8;
    dec.block_size = 8;
    dec.rsi = 1;
    dec.next_in = zero_fs.data();
    dec.avail_in = zero_fs.size();
    dec.next_out = out.data();
    dec.avail_out = out.size();
    EXPECT_EQ(AEC_OK, aec_buffer_decode(&dec));
  }

  // 5. Partial avail_out smaller than bytes_per_sample (1 byte on 16-bit
  // sample) triggers AEC_MEM_ERROR flush path.
  {
    std::vector<uint8_t> data16(16, 0x11);
    const auto enc16 = EncodeBuffer(16, 8, 1, AEC_DATA_PREPROCESS, data16);
    std::array<uint8_t, 1> too_tiny{};
    aec_stream dec{};
    dec.bits_per_sample = 16;
    dec.block_size = 8;
    dec.rsi = 1;
    dec.flags = AEC_DATA_PREPROCESS;
    dec.next_in = enc16.data();
    dec.avail_in = enc16.size();
    dec.next_out = too_tiny.data();
    dec.avail_out = too_tiny.size();
    EXPECT_EQ(AEC_MEM_ERROR, aec_buffer_decode(&dec));
  }
}

TEST(DecodeTest, ConcurrentThreadsDecodeIndependentStreamsSafely) {
  constexpr int kThreads = 4;
  std::vector<uint8_t> input(64);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<uint8_t>(i * 7 + 5);
  }
  const auto enc = EncodeBuffer(8, 8, 2, AEC_DATA_PREPROCESS, input);

  std::barrier<> sync_point(kThreads);
  std::vector<std::vector<uint8_t>> outputs(kThreads);
  {
    std::vector<std::jthread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t] {
        sync_point.arrive_and_wait();
        outputs[t].assign(input.size(), 0xA5);
        aec_stream dec{};
        dec.bits_per_sample = 8;
        dec.block_size = 8;
        dec.rsi = 2;
        dec.flags = AEC_DATA_PREPROCESS;
        dec.next_in = enc.data();
        dec.avail_in = enc.size();
        dec.next_out = outputs[t].data();
        dec.avail_out = outputs[t].size();
        EXPECT_EQ(AEC_OK, aec_buffer_decode(&dec));
        EXPECT_EQ(input.size(), dec.total_out);
      });
    }
  }
  for (int t = 0; t < kThreads; ++t) {
    EXPECT_THAT(outputs[t], ElementsAreArray(input));
  }
}

// TODO(schwehr): Cover direct_get() partial/wide refill branches, the
// zero_blocks overflow guard (> UINT32_MAX / block_size), and malloc OOM
// cleanup branches if targeted stream synthesis or fault injection is added.

}  // namespace
}  // namespace libaec
