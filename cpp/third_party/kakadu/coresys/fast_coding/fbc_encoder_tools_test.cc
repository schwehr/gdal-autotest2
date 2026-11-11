#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_encoder_tools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_common.h"
#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_decoder_tools.h"

namespace kd_core_local {
namespace {

using kdu_core::kdu_byte;
using kdu_core::kdu_int32;
using kdu_core::kdu_uint16;
using kdu_core::kdu_uint32;
using kdu_core::kdu_uint64;

// Builds a byte-aligned reverse VLC tail containing specific pattern bytes and
// a 4-byte-aligned workspace suitable for fbc_e_encoder and fbc_e_decoder.
struct MelSegmentWorkspace {
  alignas(4) std::array<kdu_byte, 512> buf{};
};

void DecodeAndCheckMelRuns(const std::vector<int>& expected_runs,
                           kdu_byte* mel_end,
                           const std::vector<kdu_byte>& rev_tail,
                           kdu_byte* buf_start, int rev_offset_from_end) {
  // Append reverse tail right after `mel_end` if overlapping, or leave as-is if
  // already placed in-buffer. Either way copy into aligned buffer with >= 8
  // trailing padding bytes for fbc_e_decoder.
  const int mel_len = static_cast<int>(mel_end - buf_start);
  MelSegmentWorkspace dec_ws;
  ASSERT_LE(mel_len + static_cast<int>(rev_tail.size()) + 16,
            static_cast<int>(dec_ws.buf.size()));
  std::fill(dec_ws.buf.begin(), dec_ws.buf.end(), kdu_byte{0xAA});
  std::memcpy(dec_ws.buf.data(), buf_start, mel_len);
  std::memcpy(dec_ws.buf.data() + mel_len, rev_tail.data(), rev_tail.size());
  const int seg_len = mel_len + static_cast<int>(rev_tail.size());
  (void)rev_offset_from_end;

  fbc_e_decoder dec;
  dec.start(dec_ws.buf.data(), seg_len);
  kdu_uint32 state = FBC_AZC_INITIAL_MEL_STATE;
  for (size_t i = 0; i < expected_runs.size(); ++i) {
    int run = -1;
    dec.decode_mel(run, state);
    EXPECT_EQ(expected_runs[i], run) << "at run index " << i;
  }
  dec.finish();
}

void VerifyRrawRoundtrip(const std::vector<std::pair<kdu_uint32, int>>& words,
                         bool want_ilw, bool use_macros) {
  std::array<kdu_byte, 1024> buf{};
  fbc_rraw_encoder enc;
  enc.start(buf.data(), static_cast<int>(buf.size()), want_ilw);

  if (use_macros) {
    int t_val = 0;
    kdu_uint64 temp_val = 0;
    int off_stp = 0;
    kdu_uint32 st_buf[FBC_RRAW_STUFF_LEN]{};
    enc.check_out(t_val, temp_val, off_stp, st_buf);
    for (const auto& [val, nbits] : words) {
      if (nbits == 1) {
        _rraw_emit_bit_(static_cast<int>(val), t_val, temp_val, off_stp, st_buf,
                        enc);
      } else {
        _rraw_emit_word_(val, nbits, t_val, temp_val, off_stp, st_buf, enc);
      }
    }
    enc.check_in(t_val, temp_val, off_stp, st_buf);
  } else {
    for (const auto& [val, nbits] : words) {
      if (nbits == 1) {
        enc.emit_bit(static_cast<int>(val));
      } else {
        enc.emit_word(val, nbits);
      }
    }
  }

  int bits_used = 0;
  kdu_byte* tail = enc.terminate(bits_used);
  const int byte_len = static_cast<int>(buf.data() + buf.size() - tail);
  EXPECT_GT(byte_len, 0);
  EXPECT_GE(bits_used, want_ilw ? 12 : 8);

  // Copy into padded buffer for fbc_rraw_decoder.
  std::vector<kdu_byte> dec_buf(byte_len + 128, 0);
  std::memcpy(dec_buf.data() + 64, tail, byte_len);

  fbc_rraw_decoder dec;
  ASSERT_TRUE(dec.start(dec_buf.data() + 64, byte_len, want_ilw));
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& [expected_val, nbits] = words[i];
    if (nbits == 0) continue;
    kdu_uint32 actual = 0;
    for (int b = 0; b < nbits; ++b) {
      kdu_uint32 bit = 0;
      dec.consume_bit(bit);
      actual |= (bit << b);
    }
    EXPECT_EQ(expected_val, actual) << "word index " << i << " nbits=" << nbits;
  }
  dec.finish();
}

void VerifyMsrawRoundtrip(const std::vector<std::pair<kdu_uint32, int>>& words,
                          bool use_macros) {
  std::array<kdu_byte, 1024> buf{};
  fbc_raw_encoder enc;
  enc.start(buf.data());

  if (use_macros) {
    kdu_int32 spt = 0;
    kdu_uint64 temp = 0;
    kdu_uint32 st_buf[FBC_RAW_STUFF_LEN]{};
    enc.check_out(spt, temp, st_buf);
    for (const auto& [val, nbits] : words) {
      if (nbits == 1) {
        _raw_emit_bit_(static_cast<int>(val), spt, temp, st_buf, enc);
      } else {
        _raw_emit_word_(val, nbits, spt, temp, st_buf, enc);
      }
    }
    enc.check_in(spt, temp, st_buf);
  } else {
    for (const auto& [val, nbits] : words) {
      if (nbits == 1) {
        enc.emit_bit(static_cast<kdu_int32>(val));
      } else {
        enc.emit_word(val, nbits);
      }
    }
  }

  kdu_byte* end = enc.terminate(nullptr, -1);
  const int byte_len = static_cast<int>(end - buf.data());
  EXPECT_EQ(byte_len, enc.get_truncation_length());

  std::vector<kdu_byte> dec_buf(byte_len + 128, 0);
  dec_buf[63] = 0x77;  // Verify msraw_decoder preserves buffer[-1].
  std::memcpy(dec_buf.data() + 64, buf.data(), byte_len);

  fbc_msraw_decoder dec;
  dec.start(dec_buf.data() + 64, byte_len);
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& [expected_val, nbits] = words[i];
    if (nbits == 0) continue;
    kdu_uint32 raw = 0;
    dec.consume_word(raw, nbits);
    const kdu_uint32 mask =
        (nbits == 32) ? 0xFFFFFFFFU : ((kdu_uint32{1} << nbits) - 1U);
    EXPECT_EQ(expected_val, raw & mask)
        << "word index " << i << " nbits=" << nbits;
  }
  dec.finish();
  // fbc_msraw_decoder::start zeroes buffer[-1] for bit-unstuffing without
  // saving/restoring it on finish().
  EXPECT_EQ(kdu_byte{0x00}, dec_buf[63]);
}

void VerifySoloSprawRoundtrip(
    const std::vector<std::pair<kdu_uint32, int>>& words) {
  std::array<kdu_byte, 1024> buf{};
  fbc_raw_encoder enc;
  enc.start(buf.data());
  for (const auto& [val, nbits] : words) {
    if (nbits == 1) {
      enc.emit_bit(static_cast<kdu_int32>(val));
    } else {
      enc.emit_word(val, nbits);
    }
  }
  const int pre_trunc_len = enc.get_truncation_length();
  kdu_byte* end = enc.terminate(nullptr, 0);
  const int byte_len = static_cast<int>(end - buf.data());
  EXPECT_LE(byte_len, pre_trunc_len);
  EXPECT_EQ(byte_len, enc.get_truncation_length());
  if (byte_len > 0) {
    EXPECT_NE(kdu_byte{0xFF}, end[-1]);
  }

  std::vector<kdu_byte> dec_buf(byte_len + 128, 0);
  dec_buf[63] = 0x55;
  std::memcpy(dec_buf.data() + 64, buf.data(), byte_len);

  fbc_spraw_decoder dec;
  dec.start(dec_buf.data() + 64, byte_len);
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& [expected_val, nbits] = words[i];
    if (nbits == 0) continue;
    kdu_uint32 raw = 0;
    dec.consume_word(raw, nbits);
    const kdu_uint32 mask =
        (nbits == 32) ? 0xFFFFFFFFU : ((kdu_uint32{1} << nbits) - 1U);
    EXPECT_EQ(expected_val, raw & mask) << "word index " << i;
  }
  dec.finish();
  EXPECT_EQ(kdu_byte{0x55}, dec_buf[63]);
}

TEST(FbcEncoderToolsTest, StaticInitAndEntropy) {
  EXPECT_TRUE(fbc_encoder_tools_do_static_init());

  EXPECT_FLOAT_EQ(0.0f, fbc_get_entropy(0, 0));
  EXPECT_FLOAT_EQ(0.0f, fbc_get_entropy(0, -5));
  EXPECT_GE(fbc_get_entropy(0, 100), 0.0f);
  EXPECT_LT(fbc_get_entropy(0, 100), 0.05f);
  EXPECT_NEAR(0.0f, fbc_get_entropy(100, 100), 1e-2f);
  EXPECT_NEAR(100.0f, fbc_get_entropy(50, 100), 1.5f);
  EXPECT_GT(fbc_get_entropy(25, 100), 75.0f);
  EXPECT_LT(fbc_get_entropy(25, 100), 85.0f);
}

TEST(FbcEncoderToolsTest, DistortionFactorsLossyAndLossless) {
  float prev_d = 0.0f;
  float prev_delta_d = 0.0f;

  // Lossless formulas: d = 0.25*(1-F), delta_d = F.
  fbc_get_d_factors(0, 4, true, prev_d, prev_delta_d);
  EXPECT_FLOAT_EQ(0.25f, prev_d);
  EXPECT_FLOAT_EQ(0.0f, prev_delta_d);

  fbc_get_d_factors(2, 4, true, prev_d, prev_delta_d);
  EXPECT_FLOAT_EQ(0.125f, prev_d);
  EXPECT_FLOAT_EQ(0.5f, prev_delta_d);

  fbc_get_d_factors(4, 4, true, prev_d, prev_delta_d);
  EXPECT_FLOAT_EQ(0.0f, prev_d);
  EXPECT_FLOAT_EQ(1.0f, prev_delta_d);

  // Lossy F >= 0.5 falls back to uniform model (1/12, 2.25).
  fbc_get_d_factors(5, 10, false, prev_d, prev_delta_d);
  EXPECT_NEAR(1.0f / 12.0f, prev_d, 1e-6f);
  EXPECT_FLOAT_EQ(2.25f, prev_delta_d);

  fbc_get_d_factors(10, 10, false, prev_d, prev_delta_d);
  EXPECT_NEAR(1.0f / 12.0f, prev_d, 1e-6f);
  EXPECT_FLOAT_EQ(2.25f, prev_delta_d);

  // Lossy F < 0.5 uses Laplacian table entries (skewed toward 1, so distortion
  // around 1.5 exceeds uniform 1/12 while remaining below 0.25).
  fbc_get_d_factors(0, 100, false, prev_d, prev_delta_d);
  EXPECT_GT(prev_d, 1.0f / 12.0f);
  EXPECT_LT(prev_d, 0.25f);
  EXPECT_GT(prev_delta_d, 1.0f);
  EXPECT_LT(prev_delta_d, 2.25f);
}

TEST(FbcEncoderToolsTest, SlopeLogConversions) {
  EXPECT_EQ(kdu_uint16{2}, fbc_slope_to_log(0.0f));
  EXPECT_EQ(kdu_uint16{2}, fbc_slope_to_log(1e-30f));
  EXPECT_EQ(kdu_uint16{0xFFFF}, fbc_slope_to_log(1e38f));

  const std::array<float, 5> test_slopes = {1e-3f, 0.25f, 1.0f, 1024.0f, 1e12f};
  for (float s : test_slopes) {
    const kdu_uint16 log_s = fbc_slope_to_log(s);
    const float recovered = fbc_slope_from_log(log_s);
    EXPECT_NEAR(s, recovered, s * 0.01f);
  }
}

TEST(FbcEncoderToolsTest, ConvexHullAllBranches) {
  // Exercises:
  // z=0: valid point (L=10, dD=100 -> slope=10)
  // z=1: dD <= 0 -> excluded immediately (log_slope=0)
  // z=2: negative/zero delta_L against prior point (L=10) or superseded point
  // z=3: shallower slope retained on hull (L=30, dD=20 -> slope=1)
  // z=4: steeper slope than z=3, pruning z=3 off the hull (L=40, dD=50 -> 5)
  std::array<int, 5> lengths = {10, 15, 8, 30, 40};
  std::array<float, 5> d_changes = {100.0f, -5.0f, 15.0f, 20.0f, 50.0f};
  std::array<kdu_uint16, 5> log_slopes{};

  fbc_find_convex_hull(lengths.data(), d_changes.data(), log_slopes.data(), 5);
  EXPECT_EQ(kdu_uint16{0},
            log_slopes[0]);  // Pruned by L=8, higher cumulative dD
  EXPECT_EQ(kdu_uint16{0}, log_slopes[1]);  // Non-positive dD excluded
  EXPECT_GT(log_slopes[2], 0);
  EXPECT_EQ(kdu_uint16{0}, log_slopes[3]);  // Pruned by steeper z=4
  EXPECT_GT(log_slopes[4], 0);
  EXPECT_GE(log_slopes[2], log_slopes[4]);
}

TEST(FbcEncoderToolsTest, MelEncoderShortLongAndMacroRoundtrip) {
  // Exercise short runs (< 51), intermediate runs (51..62), long runs (>= 63),
  // and very long runs (> 30 hits, up to 1024) across state transitions.
  const std::vector<int> runs = {
      0, 1, 2, 5, 17, 50, 51, 62, 63, 64, 150, 511, 1024, 0, 3, 1024,
  };

  for (bool use_macros : {false, true}) {
    MelSegmentWorkspace ws;
    fbc_e_encoder enc;
    enc.start(ws.buf.data());
    kdu_uint32 state = FBC_AZC_INITIAL_MEL_STATE;
    if (use_macros) {
      kdu_uint32 mod_state = state;
      kdu_int32 t = 0;
      kdu_uint64 temp = 0;
      kdu_byte* store = nullptr;
      enc.check_out_mel(mod_state, t, temp, store);
      for (int r : runs) {
        int r_copy = r;
        _mel_encode_(r_copy, mod_state, t, temp, store);
      }
      enc.check_in_mel(mod_state, t, temp, store);
      state = mod_state;
    } else {
      for (int r : runs) {
        enc.encode_mel(r, state);
      }
    }
    enc.set_mel_open_suffix(static_cast<int>(state));
    // Non-overlapping reverse byte stream (`0xFF` sets all 8 LSBs when
    // reverse_bits == 16).
    std::vector<kdu_byte> rev_tail = {0xFF, 0xFF};
    kdu_byte* mel_end = enc.terminate(rev_tail.data(), 16);
    DecodeAndCheckMelRuns(runs, mel_end, rev_tail, ws.buf.data(), 0);
  }
}

TEST(FbcEncoderToolsTest, MelTransferBytesStuffingAndTerminationOverlapCases) {
  // Craft specific bit patterns via check_out_mel / check_in_mel to cover every
  // bit-stuffing path in transfer_bytes_mel() and overlap edge cases in
  // terminate().
  auto run_mel_transfer = [](kdu_uint32 word, int extra_bits) {
    MelSegmentWorkspace ws;
    fbc_e_encoder enc;
    enc.start(ws.buf.data());
    kdu_uint32 state = FBC_AZC_INITIAL_MEL_STATE;
    kdu_uint32 mod_state = state;
    kdu_int32 t = 0;
    kdu_uint64 temp = 0;
    kdu_byte* store = nullptr;
    enc.check_out_mel(mod_state, t, temp, store);
    // Load `word` as top 32 bits plus extra low bits so mel_t > 32 triggers
    // transfer_bytes_mel() cleanly after check_in_mel.
    t = 32 + extra_bits;
    temp = (static_cast<kdu_uint64>(word) << extra_bits);
    enc.check_in_mel(mod_state, t, temp, store);

    std::array<kdu_byte, 4> rev_tail = {0x05, 0x00, 0x00, 0x00};
    kdu_byte* mel_end = enc.terminate(rev_tail.data(), 4);
    EXPECT_GE(mel_end, ws.buf.data());
  };

  // Exercises transfer_bytes_mel when top word has 0xFF in different bytes.
  auto trigger_transfer_via_encode = [](kdu_uint32 prefix_val, int start_t,
                                        kdu_uint32 start_state, int run) {
    MelSegmentWorkspace ws;
    fbc_e_encoder enc;
    enc.start(ws.buf.data());
    kdu_uint32 mod_state = start_state;
    kdu_int32 t = 0;
    kdu_uint64 temp = 0;
    kdu_byte* store = nullptr;
    enc.check_out_mel(mod_state, t, temp, store);
    t = start_t;
    temp = prefix_val;
    enc.check_in_mel(mod_state, t, temp, store);
    kdu_uint32 state = mod_state;
    // Pushing mel_t > 32 triggers transfer_bytes_mel().
    enc.encode_mel(run, state);
    std::array<kdu_byte, 2> rev_tail = {0x01, 0x00};
    kdu_byte* mel_end = enc.terminate(rev_tail.data(), 1);
    EXPECT_GT(mel_end, ws.buf.data());
  };

  // Hit every stuffing branch in transfer_bytes_mel:
  // 1) word >= 0xFF000000 and (word & 0xFFFF) >= 0xFF00 (state 11, run 0 -> 5
  // bits)
  trigger_transfer_via_encode(0x3FC03FC0U, 30, 11, 0);
  // 2) word >= 0xFF000000 only (byte 1 is FF)
  trigger_transfer_via_encode(0x3FC00000U, 30, 11, 0);
  // 3) (word & 0x00FFFFFF) >= 0x00FF0000 (byte 2 is FF, byte 1 != FF)
  trigger_transfer_via_encode(0x003FC000U, 30, 11, 0);
  // 4) (word & 0x0000FFFF) >= 0x0000FF00 (byte 3 is FF, bytes 1,2 != FF)
  trigger_transfer_via_encode(0x00003FC0U, 30, 11, 0);
  // 5) (word & 0xFF) == 0xFF (t=32 + 1 bit from state 0 run 0 -> mel_t=1)
  trigger_transfer_via_encode(0x010203FFU, 32, 0, 0);

  // Hit terminate() branches directly:
  // (a) mel_t > 8 in terminate, preceded by emitted 0xFF leaving stuffed MSB in
  //     last_mel_byte with mel_open_suffix > mel_t (num_fixed_bits <= 0,
  //     buf_next[-1] == 0xFF!).
  {
    MelSegmentWorkspace ws;
    fbc_e_encoder enc;
    enc.start(ws.buf.data());
    kdu_uint32 mod_state = 11;
    kdu_int32 t = 0;
    kdu_uint64 temp = 0;
    kdu_byte* store = nullptr;
    enc.check_out_mel(mod_state, t, temp, store);
    // 10 bits: upper 8 bits = 0xFF (flushed in terminate while(mel_t>8) loop),
    // remaining 2 bits = 0 (stuffed 0 bit + 1 data 0 bit = 0x00).
    t = 10;
    temp = (kdu_uint64{0xFF} << 2);
    enc.check_in_mel(mod_state, t, temp, store);
    enc.set_mel_open_suffix(11);  // open_suffix = 6 >= mel_t=3 -> num_fixed<=0
    std::array<kdu_byte, 2> rev_tail = {0x01, 0x00};
    kdu_byte* mel_end = enc.terminate(rev_tail.data(), 1);
    EXPECT_EQ(kdu_byte{0xFF}, ws.buf[0]);
    EXPECT_EQ(ws.buf.data() + 1, mel_end);  // overlapped!
    EXPECT_EQ(kdu_byte{0x01}, rev_tail[0]);
  }

  // (b) False-marker check when ovlp_byte == 0xFF, reverse_bits > 8, and
  //     (reverse_tail[1] & 0x80), forcing no-overlap + extra 0x00 byte!
  {
    MelSegmentWorkspace ws;
    fbc_e_encoder enc;
    enc.start(ws.buf.data());
    kdu_uint32 mod_state = 0;
    kdu_int32 t = 0;
    kdu_uint64 temp = 0;
    kdu_byte* store = nullptr;
    enc.check_out_mel(mod_state, t, temp, store);
    t = 4;
    temp = 0x0F;  // last_mel_byte = 0xF0
    enc.check_in_mel(mod_state, t, temp, store);
    std::array<kdu_byte, 2> rev_tail = {0x0F, 0x80};  // 12 bits, tail[1]&0x80
    kdu_byte* mel_end = enc.terminate(rev_tail.data(), 12);
    EXPECT_GT(mel_end, ws.buf.data());  // Could not overlap due to 0xFF + >=80
  }

  // (c) last_mel_byte == 0xFF and (last_rev_byte & 0x80), emitting 0xFF, 0x00!
  {
    MelSegmentWorkspace ws;
    fbc_e_encoder enc;
    enc.start(ws.buf.data());
    kdu_uint32 mod_state = 0;
    kdu_int32 t = 0;
    kdu_uint64 temp = 0;
    kdu_byte* store = nullptr;
    enc.check_out_mel(mod_state, t, temp, store);
    t = 8;
    temp = 0xFF;  // last_mel_byte = 0xFF
    enc.check_in_mel(mod_state, t, temp, store);
    std::array<kdu_byte, 2> rev_tail = {0x81, 0x00};
    kdu_byte* mel_end = enc.terminate(rev_tail.data(), 8);
    EXPECT_EQ(kdu_byte{0xFF}, ws.buf[0]);
    EXPECT_EQ(kdu_byte{0x00}, ws.buf[1]);
    EXPECT_EQ(ws.buf.data() + 2, mel_end);
  }
  run_mel_transfer(0xFF00FF00U, 2);
}

TEST(FbcEncoderToolsTest, RrawEncoderBitStuffingMacrosAndIfcRoundtrip) {
  // Craft words designed to trigger every bit-stuffing check in flush_stuff_buf
  // (bytes 0, 1, 2, 3) and extra 32-bit spill (`t > downshift`), both with
  // member calls and macros.
  std::vector<std::pair<kdu_uint32, int>> words;
  // Trigger last_val > 0x8FFFFFFF followed by (val & 0x7F) == 0x7F, plus
  // > 0x7F8F in bytes 1..2, bytes 2..3, and top 16 bits (> 0x7F8F0000).
  for (int rep = 0; rep < 3; ++rep) {
    words.push_back({0xFFFFFFFFU, 32});
    words.push_back({0x907F8FFFU, 32});
    words.push_back({0x7F90FFFFU, 32});
    words.push_back({0xFFFF907FU, 32});
    words.push_back({0xF1234567U, 32});
    words.push_back({0x01U, 1});
    words.push_back({0x00U, 1});
    words.push_back({0x15U, 5});
    words.push_back({0x7F8F7F7FU, 31});
  }
  // Extra tail bits to exercise various final bit alignments and downshift.
  words.push_back({0xFFFFU, 16});
  words.push_back({0x7FFFU, 15});
  words.push_back({0x01U, 1});

  VerifyRrawRoundtrip(words, /*want_ilw=*/true, /*use_macros=*/false);
  VerifyRrawRoundtrip(words, /*want_ilw=*/false, /*use_macros=*/true);

  // Also test empty rraw terminate (only ILW or 8-bit prefix) + while(t <= -8).
  {
    std::array<kdu_byte, 16> buf{};
    fbc_rraw_encoder enc;
    enc.start(buf.data(), static_cast<int>(buf.size()), true);
    enc.emit_word(0x00000001U, 1);
    int bits_used = 0;
    kdu_byte* tail = enc.terminate(bits_used);
    EXPECT_EQ(13, bits_used);
    EXPECT_EQ(2, static_cast<int>(buf.data() + buf.size() - tail));
  }

  // Test full_check_out / full_check_in and ifc_open / ifc_close, and small
  // buffer clamping path (`reverse_tail -= 4` < `buf_start`).
  {
    std::array<kdu_byte, 64> buf{};
    fbc_rraw_encoder enc;
    enc.start(buf.data(), static_cast<int>(buf.size()), false);
    int t_val = 0;
    kdu_uint64 temp_val = 0;
    int off_stp = 0;
    kdu_uint32 st_buf[FBC_RRAW_STUFF_LEN]{};
    kdu_byte* store_tail = nullptr;
    kdu_byte* store_min = nullptr;
    enc.full_check_out(t_val, temp_val, off_stp, st_buf, store_tail, store_min);
    EXPECT_EQ(buf.data(), store_min);
    enc.full_check_in(t_val, temp_val, off_stp, st_buf, store_tail);

    fbc_rraw_encoder_ifc ifc{};
    enc.ifc_open(ifc);
    EXPECT_EQ(buf.data(), ifc.store_min);
    enc.ifc_close(ifc);
  }
  {
    // Intentionally small span (4 bytes) while writing many words so buffer
    // guard clamps cleanly without out-of-bounds writes.
    std::array<kdu_byte, 4> tiny_buf{};
    fbc_rraw_encoder enc;
    enc.start(tiny_buf.data(), static_cast<int>(tiny_buf.size()), true);
    for (int i = 0; i < 20; ++i) {
      enc.emit_word(0xFFFFFFFFU, 32);
    }
    int bits_used = 0;
    kdu_byte* tail = enc.terminate(bits_used);
    EXPECT_GE(tail, tiny_buf.data());
  }
}

TEST(FbcEncoderToolsTest, RawEncoderMsrawSprawStuffingMacrosAndIfc) {
  // Trigger every byte-stuffing check (`0xFF000000`, `0xFF`, `0xFF00`,
  // `0xFF0000`) in flush_stuff_buf AND flush_completed_bytes, plus 32-bit
  // extra flush (`spt >= downshift`).
  std::vector<std::pair<kdu_uint32, int>> stuff_words;
  for (int rep = 0; rep < 3; ++rep) {
    stuff_words.push_back({0xFFFFFFFFU, 32});
    stuff_words.push_back({0xFF00FFFFU, 32});
    stuff_words.push_back({0x00FF00FFU, 32});
    stuff_words.push_back({0xFF12FF34U, 32});
    stuff_words.push_back({0x1U, 1});
    stuff_words.push_back({0x0U, 1});
    stuff_words.push_back({0xABCDEF01U, 32});
  }
  VerifyMsrawRoundtrip(stuff_words, /*use_macros=*/false);
  VerifyMsrawRoundtrip(stuff_words, /*use_macros=*/true);
  VerifySoloSprawRoundtrip(stuff_words);

  // MagSgn terminal cases:
  // 1) t > 0 with last_was_FF and remaining bits filling to 0x7F
  // 2) t > 0 with all 1s producing 0xFF (discarded)
  // 3) t == 0 with last_was_FF backing up over final 0xFF
  {
    std::array<kdu_byte, 64> buf{};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);
    enc.emit_word(0x05U, 3);  // t=3 after stuffing FF -> fills to 0x7F
    kdu_byte* end = enc.terminate(nullptr, -1);
    EXPECT_EQ(2, static_cast<int>(end - buf.data()));
    EXPECT_EQ(kdu_byte{0xFF}, buf[0]);
    EXPECT_EQ(kdu_byte{0x7D}, buf[1]);  // 0x05 | (0x7F & (0xFF<<3)) = 0x7D
  }
  {
    std::array<kdu_byte, 64> buf{};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);
    kdu_byte* end = enc.terminate(nullptr, -1);  // t == 0 and last_was_FF
    EXPECT_EQ(0, static_cast<int>(end - buf.data()));
  }

  // Solo SigProp terminal cases: trailing zeros stripped and 0x00 appended
  // when ending on 0xFF.
  {
    std::array<kdu_byte, 64> buf{};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);
    enc.emit_word(0x00U, 15);  // trailing stuffed 0 byte
    EXPECT_EQ(2, enc.get_truncation_length());
    kdu_byte* end = enc.terminate(nullptr, 0);
    EXPECT_EQ(2, static_cast<int>(end - buf.data()));
    EXPECT_EQ(kdu_byte{0xFF}, buf[0]);
    EXPECT_EQ(kdu_byte{0x00}, buf[1]);
  }

  // full_check_out/full_check_in and ifc_open/ifc_close.
  {
    std::array<kdu_byte, 64> buf{};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    int spt = 0;
    kdu_uint64 temp = 0;
    kdu_uint32 st_buf[FBC_RAW_STUFF_LEN]{};
    kdu_byte* store_next = nullptr;
    enc.full_check_out(spt, temp, st_buf, store_next);
    EXPECT_EQ(buf.data(), store_next);
    enc.full_check_in(spt, temp, st_buf, store_next);

    fbc_raw_encoder_ifc ifc{};
    enc.ifc_open(ifc);
    EXPECT_EQ(buf.data(), ifc.store_next);
    enc.ifc_close(ifc);
  }
}

TEST(FbcEncoderToolsTest, RawEncoderSigPropIntoReverseRawFusionCases) {
  // 1) Empty forward SigProp stream
  {
    std::array<kdu_byte, 64> buf{};
    std::array<kdu_byte, 4> rev = {0x01, 0xFF};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    kdu_byte* end = enc.terminate(rev.data(), 10);
    EXPECT_EQ(buf.data(), end);
    EXPECT_EQ(0, enc.get_truncation_length());
  }

  // 2) Overlap succeeds with t == 0, non-zero last_fwd_byte == 0xFF,
  //    ovlp_byte == 0xFF (reverse_bytes == 2 and rev[1] < 0x90 -> extra trunc)
  {
    std::array<kdu_byte, 64> buf{};
    std::array<kdu_byte, 2> rev = {0x01, 0x10};  // 9 bits: valid LSB mask 0x01
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);  // t == 0 pulls 0xFF back leaving buf_next==start
    kdu_byte* end = enc.terminate(rev.data(), 9);
    EXPECT_EQ(buf.data(), end);
    EXPECT_EQ(kdu_byte{0xFF}, rev[0]);
    EXPECT_EQ(2, enc.get_truncation_length());
  }

  // 3) Overlap succeeds with last_fwd_byte == 0 after trailing zeros + FF
  {
    std::array<kdu_byte, 64> buf{};
    std::array<kdu_byte, 2> rev = {0x40, 0x00};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);
    enc.emit_word(0x00U, 6);  // t=6 leaves 0 in temp; bit 6 of rev merges!
    kdu_byte* end = enc.terminate(rev.data(), 7);
    EXPECT_EQ(buf.data() + 1, end);  // fused final 0x00 byte; kept 0xFF
    EXPECT_EQ(kdu_byte{0x40}, rev[0]);
    EXPECT_EQ(2, enc.get_truncation_length());
  }

  // 4) Overlap blocked -> last_fwd_byte == 0xFF and last_rev_byte > 0x8F
  //    inserts 0x00!
  {
    std::array<kdu_byte, 64> buf{};
    std::array<kdu_byte, 2> rev = {0x9F, 0xFF};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);  // t == 0 -> pulls buf_next[-1] == 0xFF
    kdu_byte* end = enc.terminate(rev.data(), 16);
    EXPECT_EQ(2, static_cast<int>(end - buf.data()));
    EXPECT_EQ(kdu_byte{0xFF}, buf[0]);
    EXPECT_EQ(kdu_byte{0x00}, buf[1]);
    EXPECT_EQ(2, enc.get_truncation_length());
  }

  // 5) Overlap blocked with non-FF, non-zero last_fwd_byte AND zero
  // last_fwd_byte
  {
    std::array<kdu_byte, 64> buf{};
    std::array<kdu_byte, 2> rev = {0xFF, 0xFF};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0x55U, 8);
    kdu_byte* end1 = enc.terminate(rev.data(), 16);
    EXPECT_EQ(1, static_cast<int>(end1 - buf.data()));
    EXPECT_EQ(1, enc.get_truncation_length());

    enc.start(buf.data());
    enc.emit_word(0xFFU, 8);
    enc.emit_word(0x00U, 7);  // last_fwd_byte == 0, pre-byte == 0xFF,
                              // rev[0] == 0xFF blocks overlap since
                              // (0xFF^0)&rev_mask? No, ovlp = 0xFF and
                              // rev_tail[1] >= 0x90 blocks overlap!
    kdu_byte* end2 = enc.terminate(rev.data(), 16);
    EXPECT_EQ(2, static_cast<int>(end2 - buf.data()));
    EXPECT_EQ(2, enc.get_truncation_length());
  }
}

TEST(FbcEncoderToolsTest, BitEmissionSpillsAndTrailingZeroFusion) {
  // 1) Convex hull pruning when quantized log slopes tie (`>=`) despite a
  //    slightly smaller float slope.
  {
    std::array<int, 2> lengths = {1000, 2000};
    std::array<float, 2> d_changes = {999.5f, 999.4f};
    std::array<kdu_uint16, 2> log_slopes{};
    fbc_find_convex_hull(lengths.data(), d_changes.data(), log_slopes.data(),
                         2);
    EXPECT_EQ(kdu_uint16{0}, log_slopes[0]);
    EXPECT_GT(log_slopes[1], 0);
  }

  // 2) Pure emit_bit() loops exceeding FBC_RRAW_STUFF_LEN / FBC_RAW_STUFF_LEN
  //    to trigger bit-level stuff-buf flushes and verify decoder round-trips.
  {
    std::vector<std::pair<kdu_uint32, int>> bits;
    bits.reserve(460);
    for (int i = 0; i < 460; ++i) {
      bits.push_back({static_cast<kdu_uint32>((i % 13) != 0 ? 1U : 0U), 1});
    }
    VerifyRrawRoundtrip(bits, /*want_ilw=*/true, /*use_macros=*/false);
    VerifyMsrawRoundtrip(bits, /*use_macros=*/false);
  }

  // 3) rraw terminate `t > downshift` spill + bit-stuffing inside extra flush.
  {
    const std::vector<std::pair<kdu_uint32, int>> rraw_spill = {
        {0xFFFFFFFFU, 32},
        {0xFFFFFFFFU, 32},
        {0x7F90FFFFU, 32},
        {0x7FFFFFU, 23},
    };
    VerifyRrawRoundtrip(rraw_spill, /*want_ilw=*/false, /*use_macros=*/false);
  }

  // 4) raw flush_stuff_buf `spt >= downshift` extra 32-bit spill +
  //    flush_completed_bytes stuffing branches.
  {
    std::vector<std::pair<kdu_uint32, int>> raw_spill;
    raw_spill.push_back({0x7FFFFFFFU, 31});
    for (int i = 0; i < 12; ++i) {
      raw_spill.push_back({0xFFFFFFFFU, 32});
    }
    VerifyMsrawRoundtrip(raw_spill, /*use_macros=*/false);

    const std::vector<std::pair<kdu_uint32, int>> partial_flush = {
        {0xFF000000U, 32},
        {0x12FF0000U, 32},
        {0xFFU, 8},
    };
    VerifyMsrawRoundtrip(partial_flush, /*use_macros=*/false);
  }

  // 5) SigProp -> RRAW trailing-zero trimming for both fused and non-fused
  //    streams (`last_fwd_byte == 0` with non-FF predecessor).
  {
    std::array<kdu_byte, 64> buf{};
    std::array<kdu_byte, 2> rev_ok = {0x10, 0x00};
    fbc_raw_encoder enc;
    enc.start(buf.data());
    enc.emit_word(0x11U, 8);
    enc.emit_word(0x00U, 8);
    enc.emit_word(0x00U, 4);
    kdu_byte* end_ok = enc.terminate(rev_ok.data(), 5);
    EXPECT_EQ(buf.data() + 2, end_ok);
    EXPECT_EQ(1, enc.get_truncation_length());

    std::array<kdu_byte, 2> rev_blocked = {0x81, 0xFF};
    enc.start(buf.data());
    enc.emit_word(0x11U, 8);
    enc.emit_word(0x00U, 8);
    enc.emit_word(0x00U, 4);
    kdu_byte* end_blocked = enc.terminate(rev_blocked.data(), 16);
    EXPECT_EQ(buf.data() + 3, end_blocked);
    EXPECT_EQ(1, enc.get_truncation_length());
  }
}

}  // namespace
}  // namespace kd_core_local
