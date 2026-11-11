// Unit tests for
// third_party/kakadu/vendor/coresys/coding/mq_encoder.{h,cpp}.

#include "third_party/kakadu/vendor/coresys/coding/mq_encoder.h"

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/coding/mq_decoder.h"

namespace kd_core_local {
namespace {

constexpr kdu_byte kPreSentinel = 0xA5;
constexpr kdu_byte kPostSentinel = 0x5A;

std::vector<int> RoundTripMqDecode(kdu_byte* segment, int length, int num_syms,
                                   int initial_sigma, kdu_int32 initial_mps,
                                   bool check_erterm,
                                   bool* erterm_ok = nullptr) {
  std::vector<kdu_byte> buf(length + 4, kPostSentinel);
  for (int i = 0; i < length; ++i) {
    buf[i] = segment[i];
  }
  mq_decoder dec;
  dec.start(buf.data(), length, /*MQ_segment=*/true);
  mqd_state state;
  state.init(initial_sigma, initial_mps);
  std::vector<int> out;
  out.reserve(num_syms);
  for (int i = 0; i < num_syms; ++i) {
    kdu_int32 sym = 0;
    dec.mq_decode(sym, state);
    out.push_back(sym);
  }
  bool ok = dec.finish(check_erterm);
  if (erterm_ok != nullptr) {
    *erterm_ok = ok;
  }
  return out;
}

TEST(MqEncoderTest, MqeStateInitAndTables) {
  EXPECT_EQ(mq_encoder::p_bar_table[0], 0x5601);
  EXPECT_EQ(mq_encoder::p_bar_table[45], 0x0001);
  EXPECT_EQ(mq_encoder::p_bar_table[46], 0x5601);

  for (int sigma = 0; sigma <= 46; ++sigma) {
    mqe_state s0;
    s0.init(sigma, 0);
    EXPECT_EQ(s0.mps_p_bar, mq_encoder::p_bar_table[sigma]);
    EXPECT_EQ(s0.transition, &mq_encoder::transition_table[sigma * 2]);

    mqe_state s1;
    s1.init(sigma, 1);
    EXPECT_EQ(s1.mps_p_bar, mq_encoder::p_bar_table[sigma] | KDU_INT32_MIN);
    EXPECT_EQ(s1.transition, &mq_encoder::transition_table[sigma * 2 + 1]);
  }

  // Sigma 0 and 6 (p_bar == 0x5601) switch MPS on LPS transition, whereas
  // Sigma 46 stays at state 46 without switching MPS.
  EXPECT_LT(mq_encoder::transition_table[0].lps.mps_p_bar, 0);
  EXPECT_GE(mq_encoder::transition_table[1].lps.mps_p_bar, 0);
  EXPECT_GE(mq_encoder::transition_table[46 * 2].lps.mps_p_bar, 0);
  EXPECT_LT(mq_encoder::transition_table[46 * 2 + 1].lps.mps_p_bar, 0);
}

TEST(MqEncoderTest, MqEncodeMethodAndMacrosRoundTripAndMatch) {
  std::vector<int> bits;
  bits.reserve(600);
  for (int i = 0; i < 64; ++i) bits.push_back(0);  // MPS streak
  for (int i = 0; i < 64; ++i) bits.push_back(1);  // LPS / MPS flip streak
  for (int i = 0; i < 472; ++i) {
    bits.push_back(((i * 37 + 13) % 5 == 0) ? 1 : 0);
  }

  std::vector<kdu_byte> backing_fn(1024, kPostSentinel);
  backing_fn[0] = kPreSentinel;
  mq_encoder enc_fn;
  enc_fn.start(backing_fn.data() + 1, /*MQ_segment=*/true);
  mqe_state st_fn;
  st_fn.init(4, 0);
  for (int bit : bits) {
    enc_fn.mq_encode(bit ? KDU_INT32_MIN : 0, st_fn);
  }
  kdu_byte* end_fn = enc_fn.terminate(/*optimal=*/true);
  int len_fn = static_cast<int>(end_fn - (backing_fn.data() + 1));
  EXPECT_GT(len_fn, 0);
  EXPECT_EQ(backing_fn[0], kPreSentinel);
  enc_fn.finish();

  std::vector<kdu_byte> backing_macro(1024, kPostSentinel);
  backing_macro[0] = kPreSentinel;
  mq_encoder enc_macro;
  enc_macro.start(backing_macro.data() + 1, /*MQ_segment=*/true);
  mqe_state st_macro;
  st_macro.init(4, 0);
  {
    kdu_int32 a = 0;
    kdu_int32 c = 0;
    kdu_int32 t = 0;
    kdu_int32 temp = 0;
    kdu_byte* store = nullptr;
    enc_macro.check_out(a, c, t, temp, store);
    for (int bit : bits) {
      kdu_int32 sym = bit ? KDU_INT32_MIN : 0;
      _mq_encode_(sym, st_macro, a, c, t, temp, store);
    }
    enc_macro.check_in(a, c, t, temp, store);
  }
  kdu_byte* end_macro = enc_macro.terminate(/*optimal=*/true);
  int len_macro = static_cast<int>(end_macro - (backing_macro.data() + 1));
  ASSERT_EQ(len_macro, len_fn);
  for (int i = 0; i < len_fn; ++i) {
    EXPECT_EQ(backing_fn[i + 1], backing_macro[i + 1]) << "at byte " << i;
  }
  enc_macro.finish();

  std::vector<int> decoded = RoundTripMqDecode(
      backing_fn.data() + 1, len_fn, static_cast<int>(bits.size()),
      /*initial_sigma=*/4, /*initial_mps=*/0, /*check_erterm=*/false);
  EXPECT_EQ(decoded, bits);
}

TEST(MqEncoderTest, MqEncodeTriggersBitStuffCarryAndConditionalExchange) {
  // Drive heavy 1-bit / 0xFF / carry pressure across many contexts and states.
  std::vector<kdu_byte> backing(4096, kPostSentinel);
  backing[0] = kPreSentinel;

  mq_encoder enc;
  enc.start(backing.data() + 1, /*MQ_segment=*/true);
  mqe_state s0, s1;
  s0.init(0, 0);  // p_bar = 0x5601 -> conditional exchange on first MPS/LPS
  s1.init(1, 0);  // p_bar = 0x3401 -> non-exchange MPS & LPS branches
  std::vector<int> sent;
  // Start with LPS/MPS pairs in Sigma=0 and Sigma=1 to cover conditional
  // exchange both ways.
  enc.mq_encode(KDU_INT32_MIN, s0);
  sent.push_back(1);
  enc.mq_encode(0, s1);
  sent.push_back(0);
  enc.mq_encode(KDU_INT32_MIN, s1);
  sent.push_back(1);

  // Feed runs of alternating high/low bits that saturate C with 1s and
  // generate 0xFF stuffing + carry propagation into temp == 0xFF.
  mqe_state s_hi, s_lo;
  s_hi.init(45, 1);
  s_lo.init(45, 0);
  for (int i = 0; i < 600; ++i) {
    // Coding LPS under Sigma=45 pushes C up toward carry repeatedly.
    enc.mq_encode(0, s_hi);
    sent.push_back(0);
  }
  for (int i = 0; i < 2000; ++i) {
    int b = ((i % 19) == 0) ? 0 : 1;
    enc.mq_encode(b ? KDU_INT32_MIN : 0, (i & 1) ? s_hi : s_lo);
    sent.push_back(b);
  }

  kdu_byte* end = enc.terminate(/*optimal=*/true);
  int len = static_cast<int>(end - (backing.data() + 1));
  EXPECT_GT(len, 100);
  EXPECT_EQ(backing[0], kPreSentinel);
  enc.finish();

  // Verify decoding with the exact same initial context schedule.
  std::vector<kdu_byte> dec_buf(len + 4, kPostSentinel);
  for (int i = 0; i < len; ++i) dec_buf[i] = backing[i + 1];
  mq_decoder dec;
  dec.start(dec_buf.data(), len, /*MQ_segment=*/true);
  mqd_state d0, d1, d_hi, d_lo;
  d0.init(0, 0);
  d1.init(1, 0);
  d_hi.init(45, 1);
  d_lo.init(45, 0);

  kdu_int32 sym = 0;
  size_t idx = 0;
  dec.mq_decode(sym, d0);
  EXPECT_EQ(sym, sent[idx++]);
  dec.mq_decode(sym, d1);
  EXPECT_EQ(sym, sent[idx++]);
  dec.mq_decode(sym, d1);
  EXPECT_EQ(sym, sent[idx++]);
  for (int i = 0; i < 600; ++i) {
    dec.mq_decode(sym, d_hi);
    EXPECT_EQ(sym, sent[idx++]);
  }
  for (int i = 0; i < 2000; ++i) {
    dec.mq_decode(sym, (i & 1) ? d_hi : d_lo);
    EXPECT_EQ(sym, sent[idx++]);
  }
  EXPECT_TRUE(dec.finish(/*check_erterm=*/false));
}

TEST(MqEncoderTest, MqEncodeRunMethodAndMacroRoundTrip) {
  const std::vector<int> runs = {0, 1, 2, 3, 2, 0, 3, 1, 0, 3, 2, 1, 3, 3, 0};

  std::vector<kdu_byte> fn_buf(256, kPostSentinel);
  fn_buf[0] = kPreSentinel;
  mq_encoder enc_fn;
  enc_fn.start(fn_buf.data() + 1, /*MQ_segment=*/true);
  for (int r : runs) {
    enc_fn.mq_encode_run(r);
  }
  kdu_byte* fn_end = enc_fn.terminate(/*optimal=*/false);
  int fn_len = static_cast<int>(fn_end - (fn_buf.data() + 1));
  enc_fn.finish();

  std::vector<kdu_byte> macro_buf(256, kPostSentinel);
  macro_buf[0] = kPreSentinel;
  mq_encoder enc_macro;
  enc_macro.start(macro_buf.data() + 1, /*MQ_segment=*/true);
  {
    kdu_int32 a = 0;
    kdu_int32 c = 0;
    kdu_int32 t = 0;
    kdu_int32 temp = 0;
    kdu_byte* store = nullptr;
    enc_macro.check_out(a, c, t, temp, store);
    for (int r : runs) {
      _mq_encode_run_(r, a, c, t, temp, store);
    }
    enc_macro.check_in(a, c, t, temp, store);
  }
  kdu_byte* macro_end = enc_macro.terminate(/*optimal=*/false);
  int macro_len = static_cast<int>(macro_end - (macro_buf.data() + 1));
  ASSERT_EQ(macro_len, fn_len);
  for (int i = 0; i < fn_len; ++i) {
    EXPECT_EQ(fn_buf[i + 1], macro_buf[i + 1]);
  }
  enc_macro.finish();

  std::vector<kdu_byte> dec_buf(fn_len + 4, kPostSentinel);
  for (int i = 0; i < fn_len; ++i) dec_buf[i] = fn_buf[i + 1];
  mq_decoder dec;
  dec.start(dec_buf.data(), fn_len, /*MQ_segment=*/true);
  for (int expected : runs) {
    kdu_int32 got = -1;
    dec.mq_decode_run(got);
    EXPECT_EQ(got, expected);
  }
  EXPECT_TRUE(dec.finish(/*check_erterm=*/true));
}

TEST(MqEncoderTest, MqErtermDiscardsTrailing0xFF) {
  // Check empty MQ segment and varying ERTERM patterns/lengths to cover
  // trailing 0xFF discard in terminate(false) as well as ERTERM round-trip.
  bool saw_trailing_ff_discard = false;
  for (int trial = 0; trial < 2500; ++trial) {
    const int num_syms = (trial == 0) ? 0 : (4 + (trial % 37));
    const int sigma = (trial / 7) % 46;
    const kdu_int32 mps = trial & 1;
    std::vector<kdu_byte> buf(128, 0x00);
    buf[0] = kPreSentinel;
    mq_encoder enc;
    enc.start(buf.data() + 1, /*MQ_segment=*/true);
    mqe_state st;
    st.init(sigma, mps);
    std::vector<int> expected;
    expected.reserve(num_syms);
    kdu_uint32 rng = 0x1234567u + static_cast<kdu_uint32>(trial) * 2654435761u;
    for (int i = 0; i < num_syms; ++i) {
      rng = rng * 1664525u + 1013904223u;
      int bit = ((rng >> 24) & 1) ? 1 : 0;
      if (trial < 60) {
        bit = 1;  // Bias heavily toward 1s to drive C/temp toward 0xFF.
      }
      enc.mq_encode(bit ? KDU_INT32_MIN : 0, st);
      expected.push_back(bit);
    }
    kdu_byte* end = enc.terminate(/*optimal=*/false);
    int len = static_cast<int>(end - (buf.data() + 1));
    EXPECT_EQ(buf[0], kPreSentinel);
    if (buf[1 + len] == 0xFF) {
      saw_trailing_ff_discard = true;
    }
    enc.finish();

    bool erterm_ok = false;
    std::vector<int> got =
        RoundTripMqDecode(buf.data() + 1, len, num_syms, sigma, mps,
                          /*check_erterm=*/true, &erterm_ok);
    EXPECT_EQ(got, expected) << "trial=" << trial;
    EXPECT_TRUE(erterm_ok) << "trial=" << trial;
    if (saw_trailing_ff_discard && trial > 300) {
      break;
    }
  }
  EXPECT_TRUE(saw_trailing_ff_discard);
}

TEST(MqEncoderTest, RawEncodeOptimalAndErtermMethodAndMacros) {
  // Test empty raw segments (t == 8 at termination) under both optimal and
  // non-optimal termination.
  for (bool optimal : {true, false}) {
    std::vector<kdu_byte> buf(16, kPostSentinel);
    mq_encoder enc;
    enc.start(buf.data(), /*MQ_segment=*/false);
    kdu_byte* end = enc.terminate(optimal);
    EXPECT_EQ(end, buf.data());
    enc.finish();
  }

  // Test raw_encode with exact 8 consecutive 1-bits (temp == 0xFF && t == 0)
  // followed by ERTERM termination (triggers stuff bit + 0x2A pad byte).
  {
    std::vector<kdu_byte> buf(32, kPostSentinel);
    mq_encoder enc;
    enc.start(buf.data(), /*MQ_segment=*/false);
    for (int i = 0; i < 8; ++i) {
      enc.raw_encode(1);
    }
    kdu_byte* end = enc.terminate(/*optimal=*/false);
    int len = static_cast<int>(end - buf.data());
    ASSERT_EQ(len, 2);
    EXPECT_EQ(buf[0], 0xFF);
    EXPECT_EQ(buf[1], 0x2A);
    enc.finish();

    std::vector<kdu_byte> dec_buf(len + 4, kPostSentinel);
    for (int i = 0; i < len; ++i) dec_buf[i] = buf[i];
    mq_decoder dec;
    dec.start(dec_buf.data(), len, /*MQ_segment=*/false);
    for (int i = 0; i < 8; ++i) {
      kdu_int32 sym = 0;
      dec.raw_decode(sym);
      EXPECT_EQ(sym, 1);
    }
    EXPECT_TRUE(dec.finish(/*check_erterm=*/true));
  }

  // Test raw_encode triggering internal 0xFF bit-stuffing (9+ ones) plus
  // partial byte, comparing method vs macro API and optimal vs ERTERM.
  std::vector<int> raw_bits = {
      1, 1, 1, 1, 1, 1, 1, 1,  // 0xFF triggers bit stuff on next symbol
      0, 1, 0, 1, 1, 0, 1,     // 7 bits into stuffed byte
      1, 0, 1,                 // 3 bits into next byte
  };
  for (bool optimal : {true, false}) {
    std::vector<kdu_byte> fn_buf(64, kPostSentinel);
    mq_encoder enc_fn;
    enc_fn.start(fn_buf.data(), /*MQ_segment=*/false);
    for (int b : raw_bits) {
      enc_fn.raw_encode(b);
    }
    kdu_byte* fn_end = enc_fn.terminate(optimal);
    int fn_len = static_cast<int>(fn_end - fn_buf.data());
    enc_fn.finish();

    std::vector<kdu_byte> macro_buf(64, kPostSentinel);
    mq_encoder enc_macro;
    enc_macro.start(macro_buf.data(), /*MQ_segment=*/false);
    {
      kdu_int32 t = 0;
      kdu_int32 temp = 0;
      kdu_byte* store = nullptr;
      enc_macro.check_out(t, temp, store);
      for (int b : raw_bits) {
        _raw_encode_(b, t, temp, store);
      }
      enc_macro.check_in(t, temp, store);
    }
    kdu_byte* macro_end = enc_macro.terminate(optimal);
    int macro_len = static_cast<int>(macro_end - macro_buf.data());
    ASSERT_EQ(macro_len, fn_len);
    for (int i = 0; i < fn_len; ++i) {
      EXPECT_EQ(fn_buf[i], macro_buf[i]);
    }
    enc_macro.finish();

    std::vector<kdu_byte> dec_buf(fn_len + 4, kPostSentinel);
    for (int i = 0; i < fn_len; ++i) dec_buf[i] = fn_buf[i];
    mq_decoder dec;
    dec.start(dec_buf.data(), fn_len, /*MQ_segment=*/false);
    for (int expected : raw_bits) {
      kdu_int32 got = -1;
      dec.raw_decode(got);
      EXPECT_EQ(got, expected);
    }
    EXPECT_TRUE(dec.finish(/*check_erterm=*/!optimal));
  }

  // Test optimal raw termination discarding trailing 0xFF / 0xFF7F pairs.
  {
    std::vector<kdu_byte> buf(64, kPostSentinel);
    mq_encoder enc;
    enc.start(buf.data(), /*MQ_segment=*/false);
    // Emit 0x42 followed by 15 ones so padded final bytes are 0xFF, 0x7F.
    const int bits_val[] = {0, 1, 0, 0, 0, 0, 1, 0};  // 0x42
    for (int b : bits_val) enc.raw_encode(b);
    for (int i = 0; i < 15; ++i) enc.raw_encode(1);
    kdu_byte* end = enc.terminate(/*optimal=*/true);
    int len = static_cast<int>(end - buf.data());
    EXPECT_EQ(len, 1);
    EXPECT_EQ(buf[0], 0x42);
    enc.finish();
  }
}

TEST(MqEncoderTest, ContinuesChainIncrementalLengthAugmentBufferAndReuse) {
  std::vector<kdu_byte> buf1(128, kPostSentinel);
  buf1[0] = kPreSentinel;

  mq_encoder p1, p2, p3, p4;
  p1.start(buf1.data() + 1, /*MQ_segment=*/true);
  mqe_state st;
  st.init(10, 0);

  // Pass 1: encode enough symbols to emit several bytes.
  std::vector<int> all_bits;
  for (int i = 0; i < 60; ++i) {
    int b = (i % 3 == 0) ? 1 : 0;
    p1.mq_encode(b ? KDU_INT32_MIN : 0, st);
    all_bits.push_back(b);
  }
  EXPECT_GT(p1.get_bytes_used(), 0);

  // Grow buffer and relocate pointers via augment_buffer.
  std::vector<kdu_byte> buf2(2048, kPostSentinel);
  for (size_t i = 0; i < buf1.size(); ++i) {
    buf2[i] = buf1[i];
  }
  p1.augment_buffer(buf1.data() + 1, buf2.data() + 1);

  bool final1 = true;
  int len1_active = p1.get_incremental_length(final1);
  EXPECT_FALSE(final1);
  EXPECT_GE(len1_active, 0);

  // Pass 2 continues Pass 1, initially < 5 extra bytes.
  p2.continues(&p1);
  p2.mq_encode(0, st);
  all_bits.push_back(0);
  bool final1_short = true;
  (void)p1.get_incremental_length(final1_short);
  EXPECT_FALSE(final1_short);

  // Encode enough in Pass 2 so p2 exceeds p1 by >= 5 bytes -> finalizes p1.
  for (int i = 0; i < 150; ++i) {
    int b = ((i * 7 + 1) % 4 == 0) ? 1 : 0;
    p2.mq_encode(b ? KDU_INT32_MIN : 0, st);
    all_bits.push_back(b);
  }
  bool final1_ready = false;
  int len1_final = p1.get_incremental_length(final1_ready);
  EXPECT_TRUE(final1_ready);
  EXPECT_GT(len1_final, 0);

  // Querying again takes the cached truncation_point_found fast path.
  bool final1_cached = false;
  EXPECT_EQ(p1.get_incremental_length(final1_cached), len1_final);
  EXPECT_TRUE(final1_cached);

  // Pass 3 continues Pass 2 briefly (< 5 bytes ahead) so p2 is not finalized
  // yet.
  p3.continues(&p2);
  p3.mq_encode(KDU_INT32_MIN, st);
  all_bits.push_back(1);
  bool final2 = true;
  (void)p2.get_incremental_length(final2);
  EXPECT_FALSE(final2);

  // Encode more symbols in Pass 3 (carry / F_min path), then continue to p4.
  for (int i = 0; i < 120; ++i) {
    int b = (i % 5 != 0) ? 1 : 0;
    p3.mq_encode(b ? KDU_INT32_MIN : 0, st);
    all_bits.push_back(b);
  }
  p4.continues(&p3);
  p4.mq_encode(KDU_INT32_MIN, st);
  all_bits.push_back(1);

  kdu_byte* seg_end = p4.terminate(/*optimal=*/true);
  int total_len = static_cast<int>(seg_end - (buf2.data() + 1));
  EXPECT_GT(total_len, len1_final);
  EXPECT_EQ(buf2[0], kPreSentinel);

  bool final3 = false;
  int len3 = p3.get_incremental_length(final3);
  EXPECT_TRUE(final3);
  EXPECT_GE(len3, 0);
  bool final4 = false;
  int len4 = p4.get_incremental_length(final4);
  EXPECT_TRUE(final4);
  EXPECT_GE(len4, 0);

  p4.finish();
  // Calling augment_buffer after finish() is a safe no-op.
  p4.augment_buffer(buf2.data() + 1, buf2.data() + 1);

  // Verify entire multi-pass stream decodes cleanly.
  std::vector<int> decoded = RoundTripMqDecode(
      buf2.data() + 1, total_len, static_cast<int>(all_bits.size()),
      /*initial_sigma=*/10, /*initial_mps=*/0, /*check_erterm=*/false);
  EXPECT_EQ(decoded, all_bits);

  // Re-using encoders in the chain after finish() must work.
  p1.start(buf2.data() + 1, /*MQ_segment=*/true);
  mqe_state reuse_st;
  reuse_st.init(0, 1);
  p1.mq_encode(KDU_INT32_MIN, reuse_st);
  kdu_byte* reuse_end = p1.terminate(/*optimal=*/false);
  EXPECT_GE(reuse_end - (buf2.data() + 1), 0);
  p1.finish();
}

}  // namespace
}  // namespace kd_core_local
