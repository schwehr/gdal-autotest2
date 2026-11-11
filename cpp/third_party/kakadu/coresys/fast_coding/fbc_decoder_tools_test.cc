#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_decoder_tools.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_common.h"
#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_encoder_tools.h"

namespace kd_core_local {
namespace {

TEST(FbcDecoderToolsTest, MelShortSegmentLengthsAndUnalignedOffsets) {
  // Segment lengths 1..8 exercise both the segment_length <= 4 path and the
  // unaligned byte-advance loop inside fbc_e_decoder::start().
  for (int seg_len = 1; seg_len <= 10; ++seg_len) {
    alignas(16) std::array<kdu_byte, 128> buf{};
    buf.fill(0);
    // Pattern mixes 0xAA, 0xFF + 0x00 stuffed byte, and 0x55 across lengths.
    for (int i = 0; i < seg_len; ++i) {
      if (i % 3 == 0) {
        buf[i] = 0xAA;
      } else if (i % 3 == 1) {
        buf[i] = 0xFF;
      } else {
        buf[i] = 0x55;
      }
    }
    // Ensure a clean termination marker area after seg_len so reading does
    // not depend on stale stack bytes.
    std::memset(buf.data() + seg_len, 0xFF, 8);

    fbc_e_decoder mel_dec;
    mel_dec.start(buf.data(), seg_len);
    kdu_uint32 state = 0;
    for (int iter = 0; iter < 12; ++iter) {
      int run = -1;
      mel_dec.decode_mel(run, state);
      EXPECT_GE(run, 0);
    }
    mel_dec.finish();
  }
}

TEST(FbcDecoderToolsTest, MelRoundTripMethodAndMacros) {
  std::vector<int> runs = {0,  0,   1,   0,  2,  3,  7,   15,  0,  31,
                           63, 127, 255, 5,  19, 0,  0,   0,   42, 100,
                           1,  0,   8,   16, 32, 64, 128, 256, 3,  0};
  // Append many zero-runs so MEL outputs plenty of 1-bits and hits 0xFF byte
  // stuffing inside prepare_mel_bits.
  for (int i = 0; i < 64; ++i) {
    runs.push_back(200);
  }
  runs.push_back(0);
  runs.push_back(5);

  alignas(16) std::array<kdu_byte, 512> enc_buf{};
  enc_buf.fill(0);
  kdu_byte* mel_start = enc_buf.data() + 16;

  fbc_e_encoder mel_enc;
  mel_enc.start(mel_start);
  kdu_uint32 enc_state = 0;
  for (int run : runs) {
    mel_enc.encode_mel(run, enc_state);
  }
  alignas(16) std::array<kdu_byte, 8> rev_tail{};
  rev_tail.fill(0);
  kdu_byte* mel_end = mel_enc.terminate(rev_tail.data(), 1);
  // If terminate() overlapped the last MEL byte into rev_tail[0], place that
  // byte at *mel_end so start() sees the complete MEL segment.
  *mel_end = rev_tail[0];
  const int seg_len = static_cast<int>(mel_end - mel_start) + 1;
  ASSERT_GT(seg_len, 8);

  // Decode via direct member decode_mel().
  {
    std::vector<kdu_byte> dec_storage(enc_buf.begin(), enc_buf.end());
    fbc_e_decoder dec;
    dec.start(dec_storage.data() + 16, seg_len);
    kdu_uint32 dec_state = 0;
    for (int expected_run : runs) {
      int decoded_run = -1;
      dec.decode_mel(decoded_run, dec_state);
      EXPECT_EQ(expected_run, decoded_run);
    }
    dec.finish();
  }

  // Decode via check_out_mel() + _mel_decode_() + check_in_mel().
  {
    std::vector<kdu_byte> dec_storage(enc_buf.begin(), enc_buf.end());
    fbc_e_decoder dec;
    dec.start(dec_storage.data() + 16, seg_len);
    kdu_uint32 dec_state = 0;
    kdu_int32 mel_t = 0;
    kdu_uint64 mel_temp = 0;
    kdu_byte* mel_store = nullptr;
    dec.check_out_mel(dec_state, mel_t, mel_temp, mel_store);
    for (int expected_run : runs) {
      int decoded_run = -1;
      _mel_decode_(decoded_run, dec_state, mel_t, mel_temp, mel_store);
      EXPECT_EQ(expected_run, decoded_run);
    }
    dec.check_in_mel();
    dec.finish();
  }
}

TEST(FbcDecoderToolsTest, MelMultiRunPartialAndHighCumSpan) {
  // Craft a long run of zeros followed by short runs so MEL generates many
  // consecutive 1-bits (cum_span > 1024) as well as 0xFF stuffed bytes.
  alignas(16) std::array<kdu_byte, 512> enc_buf{};
  enc_buf.fill(0);
  kdu_byte* mel_start = enc_buf.data() + 16;

  fbc_e_encoder mel_enc;
  mel_enc.start(mel_start);
  kdu_uint32 enc_state = 0;
  for (int i = 0; i < 40; ++i) {
    mel_enc.encode_mel(150, enc_state);
  }
  for (int i = 0; i < 32; ++i) {
    mel_enc.encode_mel(i % 7, enc_state);
  }
  alignas(16) std::array<kdu_byte, 8> rev_tail{};
  rev_tail.fill(0);
  kdu_byte* mel_end = mel_enc.terminate(rev_tail.data(), 1);
  *mel_end = rev_tail[0];
  const int seg_len = static_cast<int>(mel_end - mel_start) + 1;
  ASSERT_GT(seg_len, 4);

  // 1) Decode in valid chunks (min_ptr <= run_ptr + 5) preserving cum_span
  // across calls so cum_span > 1024 triggers and appends five 1024 sentinels.
  {
    std::vector<kdu_byte> dec_storage(enc_buf.begin(), enc_buf.end());
    fbc_e_decoder dec;
    dec.start(dec_storage.data() + 16, seg_len);
    std::vector<kdu_uint16> runs_buf(512, 0);
    kdu_uint16* run_ptr = runs_buf.data();
    kdu_uint16 cum_span = 0;
    kdu_uint32 dec_state = 0;
    for (int step = 0; step < 30 && cum_span <= 1024; ++step) {
      dec.decode_mel_multi(run_ptr, run_ptr + 5, cum_span, dec_state);
    }
    EXPECT_GT(cum_span, 1024);
    ASSERT_GE(run_ptr - runs_buf.data(), 5);
    for (int k = 1; k <= 5; ++k) {
      EXPECT_EQ(run_ptr[-k], 1024);
    }
    dec.finish();
  }

  // 2) Exercise check_out_mel_m + _mel_m_decode_ + check_in_mel_m +
  // ifc_mel_m_get hitting cum_span > 1024 and appending five 1024 sentinels.
  {
    std::vector<kdu_byte> dec_storage(enc_buf.begin(), enc_buf.end());
    fbc_e_decoder dec;
    dec.start(dec_storage.data() + 16, seg_len);

    kdu_uint32 mod_state = 0;
    fbc_mel_decoder_ifc ifc{};
    dec.ifc_mel_m_get(ifc, mod_state);
    EXPECT_NE(ifc.buf, nullptr);

    kdu_int32 mel_t = 0;
    kdu_uint64 mel_temp = 0;
    kdu_byte* mel_store = nullptr;
    dec.check_out_mel_m(mod_state, mel_t, mel_temp, mel_store);

    std::vector<kdu_uint16> runs_buf(512, 0);
    kdu_uint16* run_ptr = runs_buf.data();
    kdu_uint16 cum_span = 0;
    for (int step = 0; step < 30 && cum_span <= 1024; ++step) {
      _mel_m_decode_(run_ptr, run_ptr + 5, cum_span, mod_state, mel_t, mel_temp,
                     mel_store);
    }
    EXPECT_GT(cum_span, 1024);
    ASSERT_GE(run_ptr - runs_buf.data(), 5);
    for (int k = 1; k <= 5; ++k) {
      EXPECT_EQ(run_ptr[-k], 1024);
    }
    dec.check_in_mel_m();
    dec.finish();
  }

  // 3) Verify exact decoded run equivalence between decode_mel_multi() and
  // _mel_m_decode_() on a compact stream where cum_span stays <= 1024.
  {
    const std::vector<int> short_runs = {0, 1, 2, 0, 3, 5, 1, 0, 4, 2,
                                         7, 1, 0, 2, 3, 0, 6, 1, 2, 0,
                                         1, 4, 0, 3, 2, 1, 0, 5, 1, 0};
    alignas(16) std::array<kdu_byte, 256> short_enc{};
    short_enc.fill(0);
    fbc_e_encoder s_enc;
    s_enc.start(short_enc.data() + 16);
    kdu_uint32 s_state = 0;
    for (int r : short_runs) {
      s_enc.encode_mel(r, s_state);
    }
    alignas(16) std::array<kdu_byte, 8> s_tail{};
    s_tail.fill(0);
    kdu_byte* s_end = s_enc.terminate(s_tail.data(), 1);
    *s_end = s_tail[0];
    const int s_len = static_cast<int>(s_end - (short_enc.data() + 16)) + 1;

    std::vector<kdu_byte> buf_a(short_enc.begin(), short_enc.end());
    fbc_e_decoder dec_a;
    dec_a.start(buf_a.data() + 16, s_len);
    std::vector<kdu_uint16> runs_a(256, 0);
    kdu_uint16* ptr_a = runs_a.data();
    kdu_uint16 span_a = 0;
    kdu_uint32 state_a = 0;
    while (static_cast<size_t>(ptr_a - runs_a.data()) < short_runs.size()) {
      dec_a.decode_mel_multi(ptr_a, ptr_a + 3, span_a, state_a);
    }
    dec_a.finish();

    std::vector<kdu_byte> buf_b(short_enc.begin(), short_enc.end());
    fbc_e_decoder dec_b;
    dec_b.start(buf_b.data() + 16, s_len);
    kdu_uint32 state_b = 0;
    kdu_int32 t_b = 0;
    kdu_uint64 temp_b = 0;
    kdu_byte* store_b = nullptr;
    dec_b.check_out_mel_m(state_b, t_b, temp_b, store_b);
    std::vector<kdu_uint16> runs_b(256, 0);
    kdu_uint16* ptr_b = runs_b.data();
    kdu_uint16 span_b = 0;
    while (static_cast<size_t>(ptr_b - runs_b.data()) < short_runs.size()) {
      _mel_m_decode_(ptr_b, ptr_b + 3, span_b, state_b, t_b, temp_b, store_b);
    }
    dec_b.check_in_mel_m();
    dec_b.finish();

    for (size_t i = 0; i < short_runs.size(); ++i) {
      EXPECT_EQ(runs_a[i], short_runs[i]);
      EXPECT_EQ(runs_b[i], short_runs[i]);
    }
  }

  // 3) Force partial-run continuation across small min_ptr steps (min_ptr =
  // run_ptr + 1) on a long-run stream so state & 0x80000000 triggers.
  {
    alignas(16) std::array<kdu_byte, 256> long_enc{};
    long_enc.fill(0);
    fbc_e_encoder l_enc;
    l_enc.start(long_enc.data() + 16);
    kdu_uint32 l_state = 0;
    for (int i = 0; i < 8; ++i) {
      l_enc.encode_mel(800, l_state);
    }
    l_enc.encode_mel(3, l_state);
    alignas(16) std::array<kdu_byte, 8> l_tail{};
    l_tail.fill(0);
    kdu_byte* l_end = l_enc.terminate(l_tail.data(), 1);
    *l_end = l_tail[0];
    const int l_len = static_cast<int>(l_end - (long_enc.data() + 16)) + 1;

    fbc_e_decoder dec;
    dec.start(long_enc.data() + 16, l_len);
    std::vector<kdu_uint16> runs_buf(512, 0);
    kdu_uint16* run_ptr = runs_buf.data();
    kdu_uint32 dec_state = 0;
    bool saw_continued = false;
    for (int step = 0; step < 30 && (run_ptr - runs_buf.data()) < 300; ++step) {
      kdu_uint16 cum_span = 0;
      kdu_uint16* min_ptr = run_ptr + 1;
      dec.decode_mel_multi(run_ptr, min_ptr, cum_span, dec_state);
      if ((dec_state & 0x80000000u) != 0) {
        saw_continued = true;
      }
    }
    EXPECT_TRUE(saw_continued);
    dec.finish();
  }
}

TEST(FbcDecoderToolsTest, MelDirectSyntheticStuffedBufferAndTermination) {
  // Explicitly craft 0xFF 0x7F repeated bytes ending with an unstuffed 0xFF
  // sequence so prepare_mel_bits hits both buf_next[-5] == 0xFF stuffing and
  // termination padding (0xFFFF0000).
  alignas(16) std::array<kdu_byte, 128> buf{};
  buf.fill(0);
  kdu_byte* p = buf.data() + 16;
  const int seg_len = 36;
  for (int i = 0; i < seg_len; i += 2) {
    p[i] = 0xFF;
    p[i + 1] = 0x7F;
  }
  p[seg_len - 2] = 0xFE;
  p[seg_len - 1] = 0xF0;

  fbc_e_decoder dec;
  dec.start(p, seg_len);
  std::vector<kdu_uint16> runs_buf(256, 0);
  kdu_uint16* run_ptr = runs_buf.data();
  kdu_uint16 cum_span = 0;
  kdu_uint32 state = 0;
  dec.decode_mel_multi(run_ptr, runs_buf.data() + 4, cum_span, state);
  EXPECT_GE(run_ptr, runs_buf.data() + 4);
  dec.finish();
}

TEST(FbcDecoderToolsTest, RrawStartRejectsTooShortSegments) {
  std::array<kdu_byte, 64> buf{};
  buf.fill(0xFF);

  {
    fbc_rraw_decoder dec;
    EXPECT_FALSE(dec.start(buf.data() + 16, 0, true));
  }
  {
    fbc_rraw_decoder dec;
    EXPECT_FALSE(dec.start(buf.data() + 16, 1, true));
  }
  {
    fbc_rraw_decoder dec;
    EXPECT_FALSE(dec.start(buf.data() + 16, 0, false));
  }

  // Minimum valid segment lengths succeed and finish cleanly.
  fbc_rraw_decoder valid_dec;
  EXPECT_TRUE(valid_dec.start(buf.data() + 16, 2, true));
  valid_dec.finish();
  EXPECT_TRUE(valid_dec.start(buf.data() + 16, 1, false));
  valid_dec.finish();
}

TEST(FbcDecoderToolsTest, RrawRoundTripWithAndWithoutIlw) {
  for (bool use_ilw : {true, false}) {
    std::vector<std::pair<kdu_uint32, int>> words;
    for (int i = 0; i < 80; ++i) {
      const int bits = (i % 7) + 1;
      const kdu_uint32 val = (0xA55A1357u >> (i % 17)) & ((1u << bits) - 1u);
      words.push_back({val, bits});
    }
    // Include dense 1-bits and mixed 0/1 patterns to trigger 0xFF <0x80
    // reverse bit-stuffing during encoding and unstuffing during decoding.
    for (int i = 0; i < 40; ++i) {
      words.push_back({0x7Fu, 7});
      words.push_back({(i & 1) ? 0x15u : 0x2Au, 6});
    }

    std::array<kdu_byte, 512> raw_buf{};
    raw_buf.fill(0);
    // fbc_rraw_encoder writes backwards from buffer + buffer_span. Leave plenty
    // of initialized space before raw_buf[64] and after raw_buf[320].
    constexpr int kSegSpan = 256;
    kdu_byte* seg_base = raw_buf.data() + 64;

    fbc_rraw_encoder enc;
    enc.start(seg_base, kSegSpan, use_ilw);
    for (const auto& [val, nbits] : words) {
      enc.emit_word(val, nbits);
    }
    int bits_used = 0;
    kdu_byte* tail = enc.terminate(bits_used);
    ASSERT_NE(tail, nullptr);
    const int offset = static_cast<int>(tail - seg_base);
    const int seg_len = kSegSpan - offset;
    ASSERT_GT(seg_len, 2);

    // Prepare decode buffer copying [seg_base + offset, seg_base + kSegSpan)
    // to dec_seg with initialized padding before and after.
    std::array<kdu_byte, 512> dec_buf{};
    dec_buf.fill(0);
    kdu_byte* dec_seg = dec_buf.data() + 128;
    std::memcpy(dec_seg, tail, seg_len);
    int start_len = seg_len;
    if (use_ilw) {
      // Overwrite last 12 bits with 1s per fbc_rraw_decoder::start() contract.
      dec_seg[seg_len - 1] = 0xFF;
      dec_seg[seg_len - 2] |= 0x0F;
    } else {
      // fbc_rraw_encoder(..., want_ilw=false) emitted the 8 initial 1's byte
      // at dec_seg[seg_len - 1]; strip it like the refinement muxer would and
      // append the extra 0xFF byte expected by fbc_rraw_decoder::start().
      EXPECT_EQ(dec_seg[seg_len - 1], 0xFF);
      dec_seg[seg_len - 1] = 0xFF;
    }

    // 1) Method API: open32, advance32, close32, consume_bit.
    {
      fbc_rraw_decoder dec;
      ASSERT_TRUE(dec.start(dec_seg, start_len, use_ilw));

      fbc_rraw_decoder_ifc ifc{};
      dec.ifc_get(ifc);
      EXPECT_EQ(ifc.buf_start, dec_seg);

      kdu_uint32 peek_val = 0;
      dec.open32(peek_val);
      dec.close32(0);

      for (const auto& [expected, nbits] : words) {
        if (nbits == 1) {
          kdu_uint32 bit = 0;
          dec.consume_bit(bit);
          EXPECT_EQ(expected, bit);
        } else {
          kdu_uint32 val = 0;
          dec.open32(val);
          const kdu_uint32 got = val & ((1u << nbits) - 1u);
          EXPECT_EQ(expected, got);
          kdu_uint32 adv = static_cast<kdu_uint32>(nbits);
          dec.advance32(adv);
          dec.close32(0);
        }
      }
      dec.finish();
    }

    // 2) Fast macro API: _rraw_open32_, _rraw_advance32_, _rraw_close32_,
    // _rraw_consume_bit_, _rraw_consume_word_.
    {
      fbc_rraw_decoder dec;
      ASSERT_TRUE(dec.start(dec_seg, start_len, use_ilw));
      int spt = 0;
      kdu_uint64 temp = 0;
      kdu_uint64 ldval = 0;
      kdu_uint32 usbuf[FBC_RRAW_UNSTUFF_LEN]{};
      dec.check_out(spt, temp, ldval, usbuf);

      for (size_t idx = 0; idx < words.size(); ++idx) {
        const auto& [expected, nbits] = words[idx];
        if (nbits == 1) {
          kdu_uint32 bit = 0;
          _rraw_consume_bit_(bit, spt, temp, ldval, usbuf, dec);
          EXPECT_EQ(expected, bit);
        } else if (idx % 2 == 0) {
          kdu_uint32 val = 0;
          _rraw_consume_word_(val, nbits, spt, temp, ldval, usbuf, dec);
          EXPECT_EQ(expected, val & ((1u << nbits) - 1u));
        } else {
          kdu_uint32 val = 0;
          _rraw_open32_(val, spt, temp, ldval, usbuf);
          EXPECT_EQ(expected, val & ((1u << nbits) - 1u));
          kdu_uint32 cnt = static_cast<kdu_uint32>(nbits);
          _rraw_advance32_(cnt, spt, temp, ldval, usbuf);
          _rraw_close32_(0, spt, temp, ldval, usbuf, dec);
        }
      }
      dec.check_in(spt, temp, usbuf);
      dec.finish();
    }
  }
}

TEST(FbcDecoderToolsTest, RrawHeavyBitStuffingReloadAndClose32) {
  // Craft a backwards stuffed stream containing long runs of 0xFF paired with
  // bytes <= 0x8F so reverse unstuffing steals multiple bits per byte and
  // forces activate() and close32() to refill while maintaining t >= 32.
  for (int pad = 0; pad < 4; ++pad) {
    std::array<kdu_byte, 512> dec_buf{};
    dec_buf.fill(0x55);
    kdu_byte* seg = dec_buf.data() + 128;
    const int seg_len = 80 + pad;
    // Reverse unstuffing inspects consecutive bytes moving backwards where
    // high bits trigger unstuffing when a byte >= 0x80 precedes/follows 0xFF.
    for (int i = 0; i < seg_len; ++i) {
      seg[i] = (i % 2 == 0) ? 0xFF : 0x7F;
    }
    seg[seg_len - 1] = 0xFF;

    fbc_rraw_decoder dec;
    ASSERT_TRUE(dec.start(seg, seg_len, false));
    for (int i = 0; i < 40; ++i) {
      kdu_uint32 w = 0;
      dec.open32(w);
      kdu_uint32 adv = (i % 7) + 1;
      dec.advance32(adv);
      dec.close32((i % 5) + 1);
    }
    dec.finish();
  }
}

TEST(FbcDecoderToolsTest, MsrawRoundTripStuffingFillBytesAndMacros) {
  std::vector<std::pair<kdu_uint32, int>> words;
  for (int i = 0; i < 120; ++i) {
    const int bits = (i % 15) + 1;
    const kdu_uint32 val = (0xDEADBEEFu >> (i % 13)) & ((1u << bits) - 1u);
    words.push_back({val, bits});
  }
  // Emit runs of 1-bits to trigger 0xFF -> <0x80 forward bit-stuffing.
  for (int i = 0; i < 80; ++i) {
    words.push_back({0xFFu, 8});
  }

  std::array<kdu_byte, 1024> enc_buf{};
  enc_buf.fill(0);
  kdu_byte* prefix = enc_buf.data() + 64;
  fbc_raw_encoder enc;
  enc.start(prefix);
  for (const auto& [val, nbits] : words) {
    enc.emit_word(val, nbits);
  }
  kdu_byte* end_ptr = enc.terminate(nullptr, -1);
  const int prefix_len = static_cast<int>(end_ptr - prefix);
  ASSERT_GT(prefix_len, 16);

  // 1) Direct method API: open32, advance32, close32, consume_word, ifc_get.
  {
    std::array<kdu_byte, 1024> dec_buf = enc_buf;
    kdu_byte* dec_prefix = dec_buf.data() + 64;
    // Append >= 64 initialized bytes (0xFF) after prefix_len per contract.
    std::memset(dec_prefix + prefix_len, 0xFF, 96);

    fbc_msraw_decoder dec;
    dec.start(dec_prefix, prefix_len);
    EXPECT_EQ(dec_prefix[-1], 0);

    fbc_raw_decoder_ifc ifc{};
    dec.ifc_get(ifc);
    EXPECT_EQ(ifc.buf_lim, dec_prefix + prefix_len);

    for (size_t i = 0; i < words.size(); ++i) {
      const auto& [expected, nbits] = words[i];
      if (i % 2 == 0) {
        kdu_uint32 w = 0;
        dec.consume_word(w, nbits);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
      } else {
        kdu_uint32 w = 0;
        dec.open32(w);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
        kdu_uint32 adv = static_cast<kdu_uint32>(nbits);
        dec.advance32(adv);
        dec.close32(0);
      }
    }
    dec.finish();
  }

  // 2) Macro API: check_out, _msr_open32_, _msr_advance32_, _msr_close32_,
  // _msr_consume_word_, check_in.
  {
    std::array<kdu_byte, 1024> dec_buf = enc_buf;
    kdu_byte* dec_prefix = dec_buf.data() + 64;
    std::memset(dec_prefix + prefix_len, 0xFF, 96);

    fbc_msraw_decoder dec;
    dec.start(dec_prefix, prefix_len);
    int spt = 0;
    kdu_uint64 temp = 0;
    kdu_uint64 ldval = 0;
    kdu_uint32 usbuf[FBC_RAW_UNSTUFF_LEN]{};
    dec.check_out(spt, temp, ldval, usbuf);

    for (size_t i = 0; i < words.size(); ++i) {
      const auto& [expected, nbits] = words[i];
      if (i % 3 == 0) {
        kdu_uint32 w = 0;
        _msr_consume_word_(w, nbits, spt, temp, ldval, usbuf, dec);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
      } else {
        kdu_uint32 w = 0;
        _msr_open32_(w, spt, temp, ldval, usbuf);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
        kdu_uint32 adv = static_cast<kdu_uint32>(nbits);
        _msr_advance32_(adv, spt, temp, ldval, usbuf);
        _msr_close32_(0, spt, temp, ldval, usbuf, dec);
      }
    }
    dec.check_in(spt, temp, usbuf);
    dec.finish();
  }
}

TEST(FbcDecoderToolsTest, MsrawFillByteTerminationAtAllOffsets) {
  // Place an unstuffed (0xFF, 0x80) pair at byte offsets 0..15 and read long
  // words across them so each unrolled byte position in load_unstuff_buf hits
  // the fill-byte break path, returning 1s for trailing bits. Also exercise
  // close32(non_zero_bits) and dense 0xFF 0x7F bit-stuffing stolen-bit reload.
  for (int offset = 0; offset < 16; ++offset) {
    std::array<kdu_byte, 512> buf{};
    buf.fill(0x55);
    kdu_byte* prefix = buf.data() + 64;
    const int prefix_len = offset + 32;
    for (int i = 0; i < offset; ++i) {
      prefix[i] = static_cast<kdu_byte>((i * 17) & 0x7F);
    }
    prefix[offset] = 0xFF;
    prefix[offset + 1] = 0x80;
    std::memset(prefix + prefix_len, 0xFF, 96);

    fbc_msraw_decoder dec;
    dec.start(prefix, prefix_len);
    for (int i = 0; i < offset; ++i) {
      kdu_uint32 b = 0;
      dec.consume_word(b, 8);
      EXPECT_EQ(static_cast<kdu_uint32>((i * 17) & 0x7F), b & 0xFFu);
    }
    // Consume the 0xFF byte followed by synthetic 1-fill bits once 0x80
    // triggers the invalid-stuffing / marker termination fill-byte mode.
    kdu_uint32 ff_byte = 0;
    dec.consume_word(ff_byte, 8);
    EXPECT_EQ(0xFFu, ff_byte & 0xFFu);

    kdu_uint32 tail_bits = 0;
    dec.open32(tail_bits);
    EXPECT_EQ(0xFFFFFFFFu, tail_bits);
    kdu_uint32 adv = 8;
    dec.advance32(adv);
    dec.close32(8);
    dec.finish();
  }

  // Heavy valid stuffing (0xFF 0x7F repeated) to trigger stolen-bits reload.
  std::array<kdu_byte, 512> stuff_buf{};
  stuff_buf.fill(0);
  kdu_byte* prefix = stuff_buf.data() + 64;
  for (int i = 0; i < 160; ++i) {
    prefix[i] = (i % 2 == 0) ? 0xFF : 0x7F;
  }
  std::memset(prefix + 160, 0xFF, 96);

  fbc_msraw_decoder dec;
  dec.start(prefix, 160);
  for (int i = 0; i < 40; ++i) {
    kdu_uint32 w = 0;
    dec.open32(w);
    EXPECT_EQ(0xFFFFFFFFu, w);
    dec.close32(15);
  }
  dec.finish();
}

TEST(FbcDecoderToolsTest, SprawRoundTripStuffingAndMacros) {
  std::vector<std::pair<kdu_uint32, int>> words;
  for (int i = 0; i < 100; ++i) {
    const int bits = (i % 13) + 1;
    const kdu_uint32 val = (0xCAFEBABEu >> (i % 11)) & ((1u << bits) - 1u);
    words.push_back({val, bits});
  }
  for (int i = 0; i < 60; ++i) {
    words.push_back({0xFFu, 8});
  }

  std::array<kdu_byte, 1024> enc_buf{};
  enc_buf.fill(0);
  kdu_byte* prefix = enc_buf.data() + 64;
  fbc_raw_encoder enc;
  enc.start(prefix);
  for (const auto& [val, nbits] : words) {
    enc.emit_word(val, nbits);
  }
  kdu_byte* end_ptr = enc.terminate(nullptr, -1);
  const int prefix_len = static_cast<int>(end_ptr - prefix);
  ASSERT_GT(prefix_len, 16);

  // 1) Direct method API + verifying prefix[-1] is preserved on finish().
  {
    std::array<kdu_byte, 1024> dec_buf = enc_buf;
    kdu_byte* dec_prefix = dec_buf.data() + 64;
    dec_prefix[-1] = 0x5A;
    std::memset(dec_prefix + prefix_len, 0x00, 96);

    fbc_spraw_decoder dec;
    dec.start(dec_prefix, prefix_len);

    fbc_raw_decoder_ifc ifc{};
    dec.ifc_get(ifc);
    EXPECT_EQ(ifc.buf_lim, dec_prefix + prefix_len);

    for (size_t i = 0; i < words.size(); ++i) {
      const auto& [expected, nbits] = words[i];
      if (i % 2 == 0) {
        kdu_uint32 w = 0;
        dec.consume_word(w, nbits);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
      } else {
        kdu_uint32 w = 0;
        dec.open32(w);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
        kdu_uint32 adv = static_cast<kdu_uint32>(nbits);
        dec.advance32(adv);
        dec.close32(0);
      }
    }
    dec.finish();
    EXPECT_EQ(dec_prefix[-1], 0x5A);
  }

  // 2) Macro API: check_out, _spr_open32_, _spr_advance32_, _spr_close32_,
  // _spr_consume_word_, check_in.
  {
    std::array<kdu_byte, 1024> dec_buf = enc_buf;
    kdu_byte* dec_prefix = dec_buf.data() + 64;
    std::memset(dec_prefix + prefix_len, 0x00, 96);

    fbc_spraw_decoder dec;
    dec.start(dec_prefix, prefix_len);
    int spt = 0;
    kdu_uint64 temp = 0;
    kdu_uint64 ldval = 0;
    kdu_uint32 usbuf[FBC_RAW_UNSTUFF_LEN]{};
    dec.check_out(spt, temp, ldval, usbuf);

    for (size_t i = 0; i < words.size(); ++i) {
      const auto& [expected, nbits] = words[i];
      if (i % 3 == 0) {
        kdu_uint32 w = 0;
        _spr_consume_word_(w, nbits, spt, temp, ldval, usbuf, dec);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
      } else {
        kdu_uint32 w = 0;
        _spr_open32_(w, spt, temp, ldval, usbuf);
        EXPECT_EQ(expected, w & ((1u << nbits) - 1u));
        kdu_uint32 adv = static_cast<kdu_uint32>(nbits);
        _spr_advance32_(adv, spt, temp, ldval, usbuf);
        _spr_close32_(0, spt, temp, ldval, usbuf, dec);
      }
    }
    dec.check_in(spt, temp, usbuf);
    dec.finish();
  }
}

TEST(FbcDecoderToolsTest, SprawInvalidStuffingAndHeavyStuffingReload) {
  // Exercise (0xFF, >=0x80) at every byte alignment mod 16 and heavy 0xFF 0x7F
  // stuffing reload + non-zero close32(bits).
  for (int offset = 0; offset < 16; ++offset) {
    std::array<kdu_byte, 512> buf{};
    buf.fill(0);
    kdu_byte* prefix = buf.data() + 64;
    const int prefix_len = offset + 32;
    for (int i = 0; i < offset; ++i) {
      prefix[i] = static_cast<kdu_byte>((i * 13) & 0x7F);
    }
    prefix[offset] = 0xFF;
    prefix[offset + 1] = 0x9A;

    fbc_spraw_decoder dec;
    dec.start(prefix, prefix_len);
    for (int i = 0; i < offset; ++i) {
      kdu_uint32 b = 0;
      dec.consume_word(b, 8);
      EXPECT_EQ(static_cast<kdu_uint32>((i * 13) & 0x7F), b & 0xFFu);
    }
    kdu_uint32 w = 0;
    dec.open32(w);
    dec.close32(12);
    dec.finish();
  }

  std::array<kdu_byte, 512> stuff_buf{};
  stuff_buf.fill(0);
  kdu_byte* prefix = stuff_buf.data() + 64;
  for (int i = 0; i < 160; ++i) {
    prefix[i] = (i % 2 == 0) ? 0xFF : 0x7F;
  }

  fbc_spraw_decoder dec;
  dec.start(prefix, 160);
  for (int i = 0; i < 40; ++i) {
    kdu_uint32 w = 0;
    dec.open32(w);
    EXPECT_EQ(0xFFFFFFFFu, w);
    dec.close32(15);
  }
  dec.finish();
}

}  // namespace
}  // namespace kd_core_local
