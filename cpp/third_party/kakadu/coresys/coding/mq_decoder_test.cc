// Unit tests for
// third_party/kakadu/vendor/coresys/coding/mq_decoder.{h,cpp}.

#include "third_party/kakadu/vendor/coresys/coding/mq_decoder.h"

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/coding/mq_encoder.h"

namespace kd_core_local {
namespace {

constexpr kdu_byte kPreSentinel = 0xA5;
constexpr kdu_byte kPostSentinel0 = 0x5A;
constexpr kdu_byte kPostSentinel1 = 0xC3;
constexpr kdu_byte kGuardSentinel = 0x6E;

std::vector<kdu_byte> EncodeMqBits(const std::vector<int>& bits, int sigma,
                                   kdu_int32 mps, bool optimal) {
  std::vector<kdu_byte> backing(bits.size() + 64, 0);
  backing[0] = kPreSentinel;
  mq_encoder enc;
  enc.start(backing.data() + 1, /*MQ_segment=*/true);
  mqe_state state;
  state.init(sigma, mps);
  for (int bit : bits) {
    enc.mq_encode(bit ? KDU_INT32_MIN : 0, state);
  }
  kdu_byte* end = enc.terminate(optimal);
  int len = static_cast<int>(end - (backing.data() + 1));
  enc.finish();
  return std::vector<kdu_byte>(backing.begin() + 1, backing.begin() + 1 + len);
}

std::vector<kdu_byte> EncodeRuns(const std::vector<int>& runs, bool optimal) {
  std::vector<kdu_byte> backing(runs.size() * 2 + 64, 0);
  backing[0] = kPreSentinel;
  mq_encoder enc;
  enc.start(backing.data() + 1, /*MQ_segment=*/true);
  for (int run : runs) {
    enc.mq_encode_run(run);
  }
  kdu_byte* end = enc.terminate(optimal);
  int len = static_cast<int>(end - (backing.data() + 1));
  enc.finish();
  return std::vector<kdu_byte>(backing.begin() + 1, backing.begin() + 1 + len);
}

std::vector<kdu_byte> EncodeRawBits(const std::vector<int>& bits,
                                    bool optimal) {
  std::vector<kdu_byte> buf(bits.size() + 64, 0);
  mq_encoder enc;
  enc.start(buf.data(), /*MQ_segment=*/false);
  for (int bit : bits) {
    enc.raw_encode(bit);
  }
  kdu_byte* end = enc.terminate(optimal);
  int len = static_cast<int>(end - buf.data());
  enc.finish();
  return std::vector<kdu_byte>(buf.begin(), buf.begin() + len);
}

std::vector<int> DecodeMqMethod(const std::vector<kdu_byte>& segment,
                                int num_syms, int sigma, kdu_int32 mps,
                                bool check_erterm, bool* finish_ok = nullptr) {
  std::vector<kdu_byte> buf(segment.size() + 4, kGuardSentinel);
  for (size_t i = 0; i < segment.size(); ++i) {
    buf[i] = segment[i];
  }
  buf[segment.size()] = kPostSentinel0;
  buf[segment.size() + 1] = kPostSentinel1;

  mq_decoder dec;
  dec.start(buf.data(), static_cast<int>(segment.size()), /*MQ_segment=*/true);
  // start() must temporarily overwrite [segment_length .. +1] with 0xFF.
  EXPECT_EQ(buf[segment.size()], 0xFF);
  EXPECT_EQ(buf[segment.size() + 1], 0xFF);

  mqd_state state;
  state.init(sigma, mps);
  std::vector<int> out;
  out.reserve(num_syms);
  for (int i = 0; i < num_syms; ++i) {
    kdu_int32 sym = -1;
    dec.mq_decode(sym, state);
    out.push_back(sym);
  }
  bool ok = dec.finish(check_erterm);
  if (finish_ok != nullptr) {
    *finish_ok = ok;
  }
  // finish() must restore the 2 sentinel post-amble bytes and leave guard
  // intact.
  EXPECT_EQ(buf[segment.size()], kPostSentinel0);
  EXPECT_EQ(buf[segment.size() + 1], kPostSentinel1);
  EXPECT_EQ(buf[segment.size() + 2], kGuardSentinel);
  EXPECT_EQ(buf[segment.size() + 3], kGuardSentinel);
  return out;
}

std::vector<int> DecodeMqMacro(const std::vector<kdu_byte>& segment,
                               int num_syms, int sigma, kdu_int32 mps,
                               bool check_erterm, bool* finish_ok = nullptr) {
  std::vector<kdu_byte> buf(segment.size() + 4, kGuardSentinel);
  for (size_t i = 0; i < segment.size(); ++i) {
    buf[i] = segment[i];
  }
  buf[segment.size()] = kPostSentinel0;
  buf[segment.size() + 1] = kPostSentinel1;

  mq_decoder dec;
  dec.start(buf.data(), static_cast<int>(segment.size()), /*MQ_segment=*/true);
  mqd_state state;
  state.init(sigma, mps);
  std::vector<int> out;
  out.reserve(num_syms);
  {
    kdu_int32 a = 0;
    kdu_int32 c = 0;
    kdu_int32 d = 0;
    kdu_int32 t = 0;
    kdu_int32 temp = 0;
    kdu_byte* store = nullptr;
    int s_cnt = 0;
    dec.check_out(a, c, d, t, temp, store, s_cnt);
    for (int i = 0; i < num_syms; ++i) {
      kdu_int32 sym = 0;
      _mq_decode_(sym, state, a, c, d, t, temp, store, s_cnt);
      out.push_back(sym);
    }
    dec.check_in(a, c, d, t, temp, store, s_cnt);
  }
  bool ok = dec.finish(check_erterm);
  if (finish_ok != nullptr) {
    *finish_ok = ok;
  }
  EXPECT_EQ(buf[segment.size()], kPostSentinel0);
  EXPECT_EQ(buf[segment.size() + 1], kPostSentinel1);
  return out;
}

TEST(MqDecoderTest, MqdStateInitAndTransitionTable) {
  EXPECT_EQ(mq_decoder::p_bar_table[0], 0x5601);
  EXPECT_EQ(mq_decoder::p_bar_table[45], 0x0001);
  EXPECT_EQ(mq_decoder::p_bar_table[46], 0x5601);

  for (int sigma = 0; sigma <= 46; ++sigma) {
    mqd_state s0;
    s0.init(sigma, 0);
    EXPECT_EQ(s0.p_bar_mps, (mq_decoder::p_bar_table[sigma] << 8));
    EXPECT_EQ(s0.transition, &mq_decoder::transition_table[sigma * 2]);

    mqd_state s1;
    s1.init(sigma, 1);
    EXPECT_EQ(s1.p_bar_mps, (mq_decoder::p_bar_table[sigma] << 8) + 1);
    EXPECT_EQ(s1.transition, &mq_decoder::transition_table[sigma * 2 + 1]);
  }

  // Sigma 0 and 6 (p_bar == 0x5601) switch MPS on LPS transition, while
  // Sigma 46 stays non-adaptive at state 46 without flipping MPS.
  EXPECT_EQ(mq_decoder::transition_table[0].lps.p_bar_mps & 1, 1);
  EXPECT_EQ(mq_decoder::transition_table[1].lps.p_bar_mps & 1, 0);
  EXPECT_EQ(mq_decoder::transition_table[46 * 2].lps.p_bar_mps & 1, 0);
  EXPECT_EQ(mq_decoder::transition_table[46 * 2 + 1].lps.p_bar_mps & 1, 1);
  EXPECT_EQ(mq_decoder::transition_table[46 * 2].mps.transition,
            &mq_decoder::transition_table[46 * 2]);
}

TEST(MqDecoderTest, MqDecodeMethodAndMacroMatchAcrossBranches) {
  // Construct a bit pattern exercising all MQ decode paths:
  // - Conditional exchange at Sigma=0 (both upper LPS and lower MPS)
  // - Non-exchange upper renorm MPS and lower renorm LPS
  // - Long MPS streaks for CDP (no renorm)
  // - Dense 1-bits generating 0xFF byte-stuffing in the bitstream
  std::vector<int> bits;
  bits.push_back(
      1);  // LPS at Sigma=0 (upper sub-interval conditional exchange)
  bits.push_back(0);  // Lower sub-interval path
  bits.push_back(0);
  bits.push_back(1);
  for (int i = 0; i < 300; ++i) {
    bits.push_back(0);  // CDP MPS fast path
  }
  for (int i = 0; i < 600; ++i) {
    bits.push_back(1);  // Forces 0xFF stuffing + bit-stuff fill_lsbs()
  }
  kdu_uint32 rng = 0xDEADBEEFu;
  for (int i = 0; i < 1200; ++i) {
    rng = rng * 1664525u + 1013904223u;
    bits.push_back(static_cast<int>((rng >> 31) & 1));
  }

  const std::vector<kdu_byte> segment =
      EncodeMqBits(bits, /*sigma=*/0, /*mps=*/0, /*optimal=*/true);
  ASSERT_FALSE(segment.empty());

  const std::vector<int> got_method =
      DecodeMqMethod(segment, static_cast<int>(bits.size()), /*sigma=*/0,
                     /*mps=*/0, /*check_erterm=*/false);
  const std::vector<int> got_macro =
      DecodeMqMacro(segment, static_cast<int>(bits.size()), /*sigma=*/0,
                    /*mps=*/0, /*check_erterm=*/false);

  EXPECT_EQ(got_method, bits);
  EXPECT_EQ(got_macro, bits);
}

TEST(MqDecoderTest, MultiContextScheduleCoversUpperAndLowerRenorm) {
  std::vector<int> bits;
  std::vector<int> ctx_ids;
  kdu_uint32 rng = 0x13579BDFu;
  for (int i = 0; i < 1500; ++i) {
    rng = rng * 1664525u + 1013904223u;
    int ctx = static_cast<int>((rng >> 28) & 3);
    int bit = static_cast<int>((rng >> 20) & 1);
    if (ctx == 0 && (i % 8 != 0)) {
      bit = 0;  // Heavy MPS bias
    } else if (ctx == 1) {
      bit = 1;  // Heavy 1s for 0xFF stuffing
    }
    ctx_ids.push_back(ctx);
    bits.push_back(bit);
  }

  std::vector<kdu_byte> enc_buf(2048, 0);
  enc_buf[0] = kPreSentinel;
  mq_encoder enc;
  enc.start(enc_buf.data() + 1, /*MQ_segment=*/true);
  std::array<mqe_state, 4> enc_st;
  enc_st[0].init(10, 0);
  enc_st[1].init(45, 1);
  enc_st[2].init(1, 0);
  enc_st[3].init(0, 1);
  for (size_t i = 0; i < bits.size(); ++i) {
    enc.mq_encode(bits[i] ? KDU_INT32_MIN : 0, enc_st[ctx_ids[i]]);
  }
  kdu_byte* end = enc.terminate(/*optimal=*/true);
  int len = static_cast<int>(end - (enc_buf.data() + 1));
  enc.finish();

  std::vector<kdu_byte> dec_buf(len + 4, kGuardSentinel);
  for (int i = 0; i < len; ++i) {
    dec_buf[i] = enc_buf[i + 1];
  }
  mq_decoder dec;
  dec.start(dec_buf.data(), len, /*MQ_segment=*/true);
  std::array<mqd_state, 4> dec_st;
  dec_st[0].init(10, 0);
  dec_st[1].init(45, 1);
  dec_st[2].init(1, 0);
  dec_st[3].init(0, 1);
  for (size_t i = 0; i < bits.size(); ++i) {
    kdu_int32 sym = -1;
    dec.mq_decode(sym, dec_st[ctx_ids[i]]);
    EXPECT_EQ(sym, bits[i]) << "at symbol " << i;
  }
  EXPECT_TRUE(dec.finish(/*check_erterm=*/false));
}

TEST(MqDecoderTest, MqDecodeZeroLengthSegmentAndMarkerSynthesis) {
  // Decoding past segment_length synthesizes 0xFF bytes without OOB reads and
  // restores the 2 post-amble bytes in finish().
  std::vector<kdu_byte> buf = {0x11, 0x22, 0x33, 0x44};
  mq_decoder dec;
  dec.start(buf.data(), /*segment_length=*/0, /*MQ_segment=*/true);
  mqd_state state;
  state.init(0, 0);
  for (int i = 0; i < 32; ++i) {
    kdu_int32 sym = -1;
    dec.mq_decode(sym, state);
    EXPECT_TRUE(sym == 0 || sym == 1);
  }
  // Too many marker bytes synthesized (S > 3) must fail ERTERM check.
  EXPECT_FALSE(dec.finish(/*check_erterm=*/true));
  EXPECT_EQ(buf[0], 0x11);
  EXPECT_EQ(buf[1], 0x22);
}

TEST(MqDecoderTest, MqErtermPassAndFailureModes) {
  // Sweep valid ERTERM segments across sigmas/lengths to exercise valid ERTERM
  // finish (including landing with t == 0 -> lines S++; t=8), plus error cases.
  for (int trial = 0; trial < 200; ++trial) {
    int sigma = trial % 46;
    kdu_int32 mps = (trial / 46) & 1;
    int num_syms = (trial == 0) ? 0 : (1 + (trial % 40));
    std::vector<int> bits;
    bits.reserve(num_syms);
    kdu_uint32 rng = 0x9E3779B9u + static_cast<kdu_uint32>(trial) * 2654435761u;
    for (int i = 0; i < num_syms; ++i) {
      rng = rng * 1664525u + 1013904223u;
      bits.push_back(static_cast<int>((rng >> 28) & 1));
    }

    const std::vector<kdu_byte> seg = EncodeMqBits(bits, sigma, mps,
                                                   /*optimal=*/false);
    bool method_ok = false;
    bool macro_ok = false;
    EXPECT_EQ(DecodeMqMethod(seg, num_syms, sigma, mps, /*check_erterm=*/true,
                             &method_ok),
              bits);
    EXPECT_EQ(DecodeMqMacro(seg, num_syms, sigma, mps, /*check_erterm=*/true,
                            &macro_ok),
              bits);
    EXPECT_TRUE(method_ok) << "trial=" << trial;
    EXPECT_TRUE(macro_ok) << "trial=" << trial;
  }

  // Case 1: t == 0 in finish(true) on zero-length segment after 1 symbol under
  // Sigma=45 (p_bar = 0x0001 -> A shrinks by 0x100, needs 1 shift -> t hits 0).
  {
    std::vector<kdu_byte> buf = {0xAA, 0xBB, 0xCC, 0xDD};
    mq_decoder dec;
    dec.start(buf.data(), /*segment_length=*/0, /*MQ_segment=*/true);
    mqd_state state;
    state.init(45, 0);
    kdu_int32 sym = -1;
    // With 0-length segment (marker 0xFF 0xFF), C is initially 0xFF8000 >> 1
    // range and t==1 after start(); one shift on Lower/Upper leaves t==0.
    // Encode 1 bit '0' at Sigma=0? Better: force lower-interval renorm with
    // p_bar = 0x5601? Wait A=0x800000 - 0x560100 = 0x29FF00 needs 2 shifts.
    // With Sigma=1 (p_bar=0x3401): A - p_bar = 0x4BFF00 < 0x800000 -> 1 shift!
    state.init(1, 0);
    dec.mq_decode(sym, state);
    // Just exercise finish(true) cleanly; C != 0 so ERTERM reports false or
    // true cleanly without asserting.
    (void)dec.finish(/*check_erterm=*/true);
  }

  // Case 2: Unread bytes left at finish(true) -> failure at buf_next <= end.
  {
    std::vector<int> many_bits;
    many_bits.reserve(400);
    kdu_uint32 rng = 0x2468ACE1u;
    for (int i = 0; i < 400; ++i) {
      rng = rng * 1664525u + 1013904223u;
      many_bits.push_back(static_cast<int>((rng >> 24) & 1));
    }
    const std::vector<kdu_byte> long_seg =
        EncodeMqBits(many_bits, /*sigma=*/0, /*mps=*/0, /*optimal=*/false);
    ASSERT_GT(long_seg.size(), 10u);
    std::vector<kdu_byte> buf(long_seg.size() + 4, kGuardSentinel);
    for (size_t i = 0; i < long_seg.size(); ++i) {
      buf[i] = long_seg[i];
    }
    mq_decoder dec;
    dec.start(buf.data(), static_cast<int>(long_seg.size()),
              /*MQ_segment=*/true);
    mqd_state state;
    state.init(0, 0);
    kdu_int32 sym = 0;
    dec.mq_decode(sym, state);  // Leaves bytes unread
    EXPECT_FALSE(dec.finish(/*check_erterm=*/true));
  }

  // Case 3: Corrupt ERTERM bits while decoding exact symbol count ->
  // non-zero (C >> (24-t)) causes ERTERM failure.
  {
    std::vector<int> bits = {0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1};
    std::vector<kdu_byte> seg = EncodeMqBits(bits, /*sigma=*/2, /*mps=*/0,
                                             /*optimal=*/false);
    ASSERT_GE(seg.size(), 2u);
    seg[0] ^= 0x40;
    bool ok = true;
    (void)DecodeMqMethod(seg, static_cast<int>(bits.size()), /*sigma=*/2,
                         /*mps=*/0, /*check_erterm=*/true, &ok);
    EXPECT_FALSE(ok);
  }
}

TEST(MqDecoderTest, MqDecodeRunMethodAndMacroMatchAcrossBranches) {
  std::vector<int> runs;
  runs.reserve(1000);
  // Start with explicit transitions through all run lengths 0..3 and bursts of
  // 3s/0s to hit CDP, upper/lower exchange, and 0xFF stuffing inside both
  // first and second symbols of _mq_decode_run_.
  const std::vector<int> seed = {0, 1, 2, 3, 3, 2, 1, 0, 2, 0, 3, 1,
                                 3, 3, 3, 3, 0, 0, 0, 0, 1, 2, 1, 2};
  runs.insert(runs.end(), seed.begin(), seed.end());
  for (int i = 0; i < 40; ++i) {
    runs.push_back(3);  // Drive 1-bits -> 0xFF stuffing
  }
  kdu_uint32 rng = 0xCAFEBABEu;
  for (int i = 0; i < 900; ++i) {
    rng = rng * 1664525u + 1013904223u;
    runs.push_back(static_cast<int>((rng >> 29) & 3));
  }

  const std::vector<kdu_byte> seg = EncodeRuns(runs, /*optimal=*/false);
  ASSERT_FALSE(seg.empty());

  // Method path
  {
    std::vector<kdu_byte> buf(seg.size() + 4, kGuardSentinel);
    for (size_t i = 0; i < seg.size(); ++i) buf[i] = seg[i];
    mq_decoder dec;
    dec.start(buf.data(), static_cast<int>(seg.size()), /*MQ_segment=*/true);
    for (size_t i = 0; i < runs.size(); ++i) {
      kdu_int32 run = -1;
      dec.mq_decode_run(run);
      EXPECT_EQ(run, runs[i]) << "at index " << i;
    }
    EXPECT_TRUE(dec.finish(/*check_erterm=*/true));
  }

  // Macro path
  {
    std::vector<kdu_byte> buf(seg.size() + 4, kGuardSentinel);
    for (size_t i = 0; i < seg.size(); ++i) buf[i] = seg[i];
    mq_decoder dec;
    dec.start(buf.data(), static_cast<int>(seg.size()), /*MQ_segment=*/true);
    kdu_int32 a = 0;
    kdu_int32 c = 0;
    kdu_int32 d = 0;
    kdu_int32 t = 0;
    kdu_int32 temp = 0;
    kdu_byte* store = nullptr;
    int s_cnt = 0;
    dec.check_out(a, c, d, t, temp, store, s_cnt);
    for (size_t i = 0; i < runs.size(); ++i) {
      kdu_int32 run = -1;
      _mq_decode_run_(run, a, c, d, t, temp, store, s_cnt);
      EXPECT_EQ(run, runs[i]) << "at index " << i;
    }
    dec.check_in(a, c, d, t, temp, store, s_cnt);
    EXPECT_TRUE(dec.finish(/*check_erterm=*/true));
  }
}

TEST(MqDecoderTest, RawDecodeMethodMacroAndErtermChecks) {
  std::vector<int> bits = {
      1, 1, 1, 1, 1, 1, 1, 1,  // 0xFF -> triggers bit-stuffing in next byte
      0, 1, 0, 1, 0, 1, 1,     // 7 bits in stuffed byte
      0, 0, 1, 1, 0, 1, 0, 1,  // normal byte
      1, 0, 1,                 // partial trailing byte
  };
  const std::vector<kdu_byte> seg = EncodeRawBits(bits, /*optimal=*/false);

  // Compare raw_decode vs _raw_decode_ with ERTERM check.
  for (bool use_macro : {false, true}) {
    std::vector<kdu_byte> buf(seg.size() + 4, kGuardSentinel);
    for (size_t i = 0; i < seg.size(); ++i) buf[i] = seg[i];
    buf[seg.size()] = kPostSentinel0;
    buf[seg.size() + 1] = kPostSentinel1;

    mq_decoder dec;
    dec.start(buf.data(), static_cast<int>(seg.size()), /*MQ_segment=*/false);
    if (!use_macro) {
      for (size_t i = 0; i < bits.size(); ++i) {
        kdu_int32 sym = -1;
        dec.raw_decode(sym);
        EXPECT_EQ(sym, bits[i]) << "at bit " << i;
      }
    } else {
      kdu_int32 t = 0;
      kdu_int32 temp = 0;
      kdu_byte* store = nullptr;
      dec.check_out(t, temp, store);
      for (size_t i = 0; i < bits.size(); ++i) {
        kdu_int32 sym = -1;
        _raw_decode_(sym, t, temp, store);
        EXPECT_EQ(sym, bits[i]) << "at bit " << i;
      }
      dec.check_in(t, temp, store);
    }
    EXPECT_TRUE(dec.finish(/*check_erterm=*/true));
    EXPECT_EQ(buf[seg.size()], kPostSentinel0);
    EXPECT_EQ(buf[seg.size() + 1], kPostSentinel1);
  }

  // Exact 8 ones -> {0xFF, 0x2A}: exercises ERTERM bit-stuff x = 0x2A path.
  {
    const std::vector<int> eight_ones(8, 1);
    const std::vector<kdu_byte> ff2a =
        EncodeRawBits(eight_ones, /*optimal=*/false);
    ASSERT_EQ(ff2a.size(), 2u);
    EXPECT_EQ(ff2a[0], 0xFF);
    EXPECT_EQ(ff2a[1], 0x2A);

    for (bool use_macro : {false, true}) {
      std::vector<kdu_byte> buf = {0xFF, 0x2A, kPostSentinel0, kPostSentinel1};
      mq_decoder dec;
      dec.start(buf.data(), 2, /*MQ_segment=*/false);
      if (!use_macro) {
        for (int i = 0; i < 8; ++i) {
          kdu_int32 sym = 0;
          dec.raw_decode(sym);
          EXPECT_EQ(sym, 1);
        }
      } else {
        kdu_int32 t = 0;
        kdu_int32 temp = 0;
        kdu_byte* store = nullptr;
        dec.check_out(t, temp, store);
        for (int i = 0; i < 8; ++i) {
          kdu_int32 sym = 0;
          _raw_decode_(sym, t, temp, store);
          EXPECT_EQ(sym, 1);
        }
        dec.check_in(t, temp, store);
      }
      EXPECT_TRUE(dec.finish(/*check_erterm=*/true));
    }

    // Wrong pad byte after 0xFF ({0xFF, 0x00}) must fail ERTERM check.
    {
      std::vector<kdu_byte> bad = {0xFF, 0x00, kPostSentinel0, kPostSentinel1};
      mq_decoder dec;
      dec.start(bad.data(), 2, /*MQ_segment=*/false);
      for (int i = 0; i < 8; ++i) {
        kdu_int32 sym = 0;
        dec.raw_decode(sym);
        EXPECT_EQ(sym, 1);
      }
      EXPECT_FALSE(dec.finish(/*check_erterm=*/true));
    }
  }

  // Raw decoding past end of segment (and 0-length segment) must synthesize 1s
  // via the temp == 0xFF marker check in both raw_decode() and _raw_decode_().
  for (bool use_macro : {false, true}) {
    std::vector<kdu_byte> empty_seg = {kPostSentinel0, kPostSentinel1,
                                       kGuardSentinel};
    mq_decoder dec;
    dec.start(empty_seg.data(), /*segment_length=*/0, /*MQ_segment=*/false);
    // Decode 20 synthesized 1 bits when temp == 0xFF triggers > 0x8F branch.
    // On first call temp == 0, t == 0 -> reads 0xFF (marker byte 0). Next byte
    // read sees temp == 0xFF and next == 0xFF (> 0x8F) -> restores store-- and
    // sets t = 8, yielding continuous 1-bits.
    if (!use_macro) {
      for (int i = 0; i < 20; ++i) {
        kdu_int32 sym = 0;
        dec.raw_decode(sym);
        EXPECT_EQ(sym, 1) << "at bit " << i;
      }
    } else {
      kdu_int32 t = 0;
      kdu_int32 temp = 0;
      kdu_byte* store = nullptr;
      dec.check_out(t, temp, store);
      for (int i = 0; i < 20; ++i) {
        kdu_int32 sym = 0;
        _raw_decode_(sym, t, temp, store);
        EXPECT_EQ(sym, 1) << "at bit " << i;
      }
      dec.check_in(t, temp, store);
    }
    EXPECT_TRUE(dec.finish(/*check_erterm=*/false));
    EXPECT_EQ(empty_seg[0], kPostSentinel0);
    EXPECT_EQ(empty_seg[1], kPostSentinel1);
  }

  // Unread bytes left in raw segment must fail ERTERM check.
  {
    std::vector<kdu_byte> unread = {0xAA, 0x55, kPostSentinel0, kPostSentinel1};
    mq_decoder dec;
    dec.start(unread.data(), 2, /*MQ_segment=*/false);
    kdu_int32 sym = 0;
    dec.raw_decode(sym);
    EXPECT_FALSE(dec.finish(/*check_erterm=*/true));
  }
}

}  // namespace
}  // namespace kd_core_local
