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
// Tests internal OpenJPEG MQ arithmetic coder and RAW/BYPASS coder APIs
// (src/lib/openjp2/mqc.c, mqc.h, and mqc_inl.h).
//
// Caveats:
// - opj_mqc_restart_enc is guarded by `#ifdef notdef` in mqc.c/mqc.h and is
//   not compiled.
// - opj_mqc_init_enc expects bp[-1] to be a valid writable non-0xFF fake byte
//   (matching opj_tcd_code_block_enc_allocate_data in tcd.c).
// - opj_mqc_init_dec and opj_mqc_raw_init_dec require len +
//   OPJ_COMMON_CBLK_DATA_EXTRA writable bytes and must pair with
//   opq_mqc_finish_dec (upstream `opq_` spelling).
// - White-box tests inspect internal opj_mqc_t / opj_mqc_state_t fields, and
//   hard-coded byte vectors pin deterministic ISO 15444-1 Annex C outputs.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/mqc.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/mqc.h
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/mqc_inl.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

static_assert(sizeof(opj_mqc_t) > 0);
static_assert(MQC_NUMCTXS == 19);
static_assert(T1_NUMCTXS == MQC_NUMCTXS);
static_assert(T1_CTXNO_ZC == 0 && T1_CTXNO_AGG == 17 && T1_CTXNO_UNI == 18);
static_assert(OPJ_COMMON_CBLK_DATA_EXTRA == 2);

constexpr std::array<OPJ_BYTE, OPJ_COMMON_CBLK_DATA_EXTRA> kDefaultTail = {
    0xA5, 0x5A};

// Manages an encoding buffer with one initialized pre-start fake byte,
// matching opj_tcd_code_block_enc_allocate_data() so opj_mqc_init_enc() can
// set bp = start - 1 and opj_mqc_byteout() can safely access bp[-1].
class MqcEncoderBuffer {
 public:
  explicit MqcEncoderBuffer(std::size_t capacity, OPJ_BYTE fill = 0xAA)
      : storage_(capacity + 1, fill) {
    storage_[0] = 0x00;
  }

  OPJ_BYTE* data() { return storage_.data() + 1; }
  const OPJ_BYTE* data() const { return storage_.data() + 1; }
  std::size_t capacity() const { return storage_.size() - 1; }

  std::span<const OPJ_BYTE> Written(OPJ_UINT32 num_bytes) const {
    return std::span<const OPJ_BYTE>(data(), num_bytes);
  }

 private:
  std::vector<OPJ_BYTE> storage_;
};

struct ContextInit {
  OPJ_UINT32 ctxno;
  OPJ_UINT32 msb;
  OPJ_INT32 prob;
};

struct SymbolOp {
  OPJ_UINT32 ctxno;
  OPJ_UINT32 bit;
};

// Encodes one MQ decision without DOWNLOAD_MQC_VARIABLES (which uses the
// C++20-removed `register` keyword).
void EncodeDecision(opj_mqc_t* mqc, OPJ_UINT32 bit) {
  const opj_mqc_state_t** curctx = mqc->curctx;
  OPJ_UINT32 c = mqc->c;
  OPJ_UINT32 a = mqc->a;
  OPJ_UINT32 ct = mqc->ct;
  opj_mqc_encode_macro(mqc, curctx, a, c, ct, bit);
  UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct);
}

// Decodes one MQ decision using opj_mqc_decode.
OPJ_UINT32 DecodeDecision(opj_mqc_t* mqc) {
  OPJ_UINT32 d = 0;
  opj_mqc_decode(d, mqc);
  return d;
}

std::vector<OPJ_BYTE> MakeDecodeBuffer(
    std::span<const OPJ_BYTE> payload,
    std::span<const OPJ_BYTE> tail = kDefaultTail) {
  std::vector<OPJ_BYTE> buf;
  buf.reserve(payload.size() + tail.size());
  buf.insert(buf.end(), payload.begin(), payload.end());
  buf.insert(buf.end(), tail.begin(), tail.end());
  return buf;
}

void ApplyContextInits(opj_mqc_t* mqc, std::span<const ContextInit> inits) {
  for (const ContextInit& init : inits) {
    opj_mqc_setstate(mqc, init.ctxno, init.msb, init.prob);
  }
}

void EncodeBitsSingleCtx(opj_mqc_t* mqc, OPJ_UINT32 ctxno,
                         std::span<const OPJ_UINT32> bits) {
  opj_mqc_setcurctx(mqc, ctxno);
  for (OPJ_UINT32 bit : bits) {
    EncodeDecision(mqc, bit);
  }
}

std::vector<OPJ_UINT32> DecodeBitsSingleCtx(
    std::span<const OPJ_BYTE> encoded, OPJ_UINT32 ctxno, std::size_t count,
    std::span<const ContextInit> inits) {
  std::vector<OPJ_BYTE> buf = MakeDecodeBuffer(encoded);
  opj_mqc_t dec{};
  opj_mqc_resetstates(&dec);
  ApplyContextInits(&dec, inits);
  opj_mqc_init_dec(&dec, buf.data(), static_cast<OPJ_UINT32>(encoded.size()),
                   OPJ_COMMON_CBLK_DATA_EXTRA);
  absl::Cleanup finish = [&dec] { opq_mqc_finish_dec(&dec); };

  opj_mqc_setcurctx(&dec, ctxno);
  std::vector<OPJ_UINT32> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(DecodeDecision(&dec));
  }
  std::move(finish).Invoke();
  EXPECT_EQ(kDefaultTail[0], buf[encoded.size()]);
  EXPECT_EQ(kDefaultTail[1], buf[encoded.size() + 1]);
  return out;
}

std::vector<OPJ_UINT32> DecodeSymbolOps(std::span<const OPJ_BYTE> encoded,
                                        std::span<const SymbolOp> ops,
                                        std::span<const ContextInit> inits,
                                        bool use_reset_enc = false) {
  std::vector<OPJ_BYTE> buf = MakeDecodeBuffer(encoded);
  opj_mqc_t dec{};
  if (use_reset_enc) {
    opj_mqc_reset_enc(&dec);
  } else {
    opj_mqc_resetstates(&dec);
  }
  ApplyContextInits(&dec, inits);
  opj_mqc_init_dec(&dec, buf.data(), static_cast<OPJ_UINT32>(encoded.size()),
                   OPJ_COMMON_CBLK_DATA_EXTRA);
  absl::Cleanup finish = [&dec] { opq_mqc_finish_dec(&dec); };

  std::vector<OPJ_UINT32> out;
  out.reserve(ops.size());
  for (const SymbolOp& op : ops) {
    opj_mqc_setcurctx(&dec, op.ctxno);
    out.push_back(DecodeDecision(&dec));
  }
  std::move(finish).Invoke();
  EXPECT_EQ(kDefaultTail[0], buf[encoded.size()]);
  EXPECT_EQ(kDefaultTail[1], buf[encoded.size() + 1]);
  return out;
}

std::vector<OPJ_UINT32> DecodeRawBits(std::span<const OPJ_BYTE> encoded,
                                      std::size_t bit_count) {
  std::vector<OPJ_BYTE> buf = MakeDecodeBuffer(encoded);
  opj_mqc_t dec{};
  opj_mqc_raw_init_dec(&dec, buf.data(),
                       static_cast<OPJ_UINT32>(encoded.size()),
                       OPJ_COMMON_CBLK_DATA_EXTRA);
  absl::Cleanup finish = [&dec] { opq_mqc_finish_dec(&dec); };

  std::vector<OPJ_UINT32> out;
  out.reserve(bit_count);
  for (std::size_t i = 0; i < bit_count; ++i) {
    out.push_back(opj_mqc_raw_decode(&dec));
  }
  std::move(finish).Invoke();
  EXPECT_EQ(kDefaultTail[0], buf[encoded.size()]);
  EXPECT_EQ(kDefaultTail[1], buf[encoded.size() + 1]);
  return out;
}

void ExpectNoInvalidMarkers(std::span<const OPJ_BYTE> bytes,
                            bool check_no_trailing_ff = true) {
  if (bytes.empty()) {
    return;
  }
  if (check_no_trailing_ff) {
    EXPECT_NE(0xFF, bytes.back());
  }
  for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (bytes[i] == 0xFF) {
      EXPECT_LE(bytes[i + 1], 0x8F) << "invalid marker pair at offset " << i;
    }
  }
}

TEST(MqcTest, ResetStatesInitializesAllContextsToStateZero) {
  opj_mqc_t mqc{};
  for (OPJ_UINT32 i = 0; i < MQC_NUMCTXS; ++i) {
    opj_mqc_setstate(&mqc, i, 1, 46);
    ASSERT_EQ(1U, mqc.ctxs[i]->mps);
    ASSERT_EQ(mqc.ctxs[i], mqc.ctxs[i]->nmps);
  }

  opj_mqc_resetstates(&mqc);
  for (OPJ_UINT32 i = 0; i < MQC_NUMCTXS; ++i) {
    ASSERT_NE(nullptr, mqc.ctxs[i]) << "ctx " << i;
    EXPECT_EQ(mqc.ctxs[0], mqc.ctxs[i]) << "ctx " << i;
    EXPECT_EQ(0x5601U, mqc.ctxs[i]->qeval) << "ctx " << i;
    EXPECT_EQ(0U, mqc.ctxs[i]->mps) << "ctx " << i;
    EXPECT_NE(mqc.ctxs[i], mqc.ctxs[i]->nmps) << "ctx " << i;
    EXPECT_NE(mqc.ctxs[i], mqc.ctxs[i]->nlps) << "ctx " << i;
  }
}

TEST(MqcTest, SetStateAndSetCurCtxConfigureProbabilitiesAndPolarity) {
  struct StateCase {
    OPJ_UINT32 ctxno;
    OPJ_UINT32 msb;
    OPJ_INT32 prob;
    OPJ_UINT32 expected_qeval;
    bool is_fixed_point;
  };
  constexpr StateCase kCases[] = {
      {.ctxno = 0,
       .msb = 0,
       .prob = 0,
       .expected_qeval = 0x5601U,
       .is_fixed_point = false},
      {.ctxno = 1,
       .msb = 1,
       .prob = 0,
       .expected_qeval = 0x5601U,
       .is_fixed_point = false},
      {.ctxno = 2,
       .msb = 0,
       .prob = 1,
       .expected_qeval = 0x3401U,
       .is_fixed_point = false},
      {.ctxno = T1_CTXNO_AGG,
       .msb = 0,
       .prob = 3,
       .expected_qeval = 0x0AC1U,
       .is_fixed_point = false},
      {.ctxno = T1_CTXNO_ZC,
       .msb = 1,
       .prob = 4,
       .expected_qeval = 0x0521U,
       .is_fixed_point = false},
      {.ctxno = T1_CTXNO_SC,
       .msb = 0,
       .prob = 23,
       .expected_qeval = 0x2201U,
       .is_fixed_point = false},
      {.ctxno = T1_CTXNO_MAG,
       .msb = 1,
       .prob = 44,
       .expected_qeval = 0x0005U,
       .is_fixed_point = false},
      {.ctxno = T1_CTXNO_UNI,
       .msb = 0,
       .prob = 46,
       .expected_qeval = 0x5601U,
       .is_fixed_point = true},
      {.ctxno = T1_CTXNO_UNI,
       .msb = 1,
       .prob = 46,
       .expected_qeval = 0x5601U,
       .is_fixed_point = true},
  };

  opj_mqc_t mqc{};
  opj_mqc_resetstates(&mqc);

  for (const StateCase& tc : kCases) {
    opj_mqc_setstate(&mqc, tc.ctxno, tc.msb, tc.prob);
    opj_mqc_setcurctx(&mqc, tc.ctxno);
    EXPECT_EQ(&mqc.ctxs[tc.ctxno], mqc.curctx);
    ASSERT_NE(nullptr, *mqc.curctx);
    EXPECT_EQ(tc.msb, (*mqc.curctx)->mps);
    EXPECT_EQ(tc.expected_qeval, (*mqc.curctx)->qeval);
    if (tc.is_fixed_point) {
      EXPECT_EQ(*mqc.curctx, (*mqc.curctx)->nmps);
      EXPECT_EQ(*mqc.curctx, (*mqc.curctx)->nlps);
    } else {
      EXPECT_NE(*mqc.curctx, (*mqc.curctx)->nmps);
      EXPECT_NE(*mqc.curctx, (*mqc.curctx)->nlps);
    }
  }
}

TEST(MqcTest, ResetEncSetsTier1StandardInitialStates) {
  opj_mqc_t mqc{};
  for (OPJ_UINT32 i = 0; i < MQC_NUMCTXS; ++i) {
    opj_mqc_setstate(&mqc, i, 1, 20);
  }

  opj_mqc_reset_enc(&mqc);
  EXPECT_EQ(0x0521U, mqc.ctxs[T1_CTXNO_ZC]->qeval);
  EXPECT_EQ(0U, mqc.ctxs[T1_CTXNO_ZC]->mps);
  for (OPJ_UINT32 i = 1; i < T1_CTXNO_AGG; ++i) {
    EXPECT_EQ(0x5601U, mqc.ctxs[i]->qeval) << "ctx " << i;
    EXPECT_EQ(0U, mqc.ctxs[i]->mps) << "ctx " << i;
    EXPECT_NE(mqc.ctxs[i], mqc.ctxs[i]->nmps) << "ctx " << i;
  }
  EXPECT_EQ(0x0AC1U, mqc.ctxs[T1_CTXNO_AGG]->qeval);
  EXPECT_EQ(0U, mqc.ctxs[T1_CTXNO_AGG]->mps);
  EXPECT_EQ(0x5601U, mqc.ctxs[T1_CTXNO_UNI]->qeval);
  EXPECT_EQ(0U, mqc.ctxs[T1_CTXNO_UNI]->mps);
  EXPECT_EQ(mqc.ctxs[T1_CTXNO_UNI], mqc.ctxs[T1_CTXNO_UNI]->nmps);
  EXPECT_EQ(mqc.ctxs[T1_CTXNO_UNI], mqc.ctxs[T1_CTXNO_UNI]->nlps);
}

TEST(MqcTest, InitEncAndEmptyFlushEmitCanonicalTerminator) {
  constexpr OPJ_BYTE kSentinel = 0xAA;
  MqcEncoderBuffer buf(16, kSentinel);
  opj_mqc_t mqc{};
  opj_mqc_resetstates(&mqc);

  opj_mqc_init_enc(&mqc, buf.data());
  EXPECT_EQ(&mqc.ctxs[0], mqc.curctx);
  EXPECT_EQ(0x8000U, mqc.a);
  EXPECT_EQ(0U, mqc.c);
  EXPECT_EQ(12U, mqc.ct);
  EXPECT_EQ(buf.data(), mqc.start);
  EXPECT_EQ(buf.data() - 1, mqc.bp);
  EXPECT_EQ(0U, mqc.end_of_byte_stream_counter);

  // Empty flush takes setbits (c >= tempc), triggers 0xFF stuffing on the
  // second byteout, and advances bp after emitting {0xFF, 0x7F}.
  opj_mqc_flush(&mqc);
  ASSERT_EQ(2U, opj_mqc_numbytes(&mqc));
  EXPECT_THAT(buf.Written(2), ElementsAre(0xFF, 0x7F));
  EXPECT_EQ(kSentinel, buf.data()[2]);
}

TEST(MqcTest, FlushCoversSetbitsBranchesAndTrailing0xFFDiscard) {
  // 1. Single LPS bit (1) at state 0 leaves c + a >= 0x10000 before setbits,
  // exercising the false branch of (mqc->c >= tempc) in opj_mqc_setbits.
  {
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    constexpr OPJ_UINT32 kBits[] = {1};
    EncodeBitsSingleCtx(&mqc, 0, kBits);
    opj_mqc_flush(&mqc);

    ASSERT_EQ(2U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(2), ElementsAre(0xFF, 0x7F));
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(2), 0, 1, {}), ElementsAre(1U));
  }

  // 2. Single MPS bit (0) at state 0 emits 0x7F followed by 0xFF in flush,
  // exercising the (*mqc->bp == 0xFF) discard branch in opj_mqc_flush.
  {
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    constexpr OPJ_UINT32 kBits[] = {0};
    EncodeBitsSingleCtx(&mqc, 0, kBits);
    opj_mqc_flush(&mqc);

    ASSERT_EQ(1U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(1), ElementsAre(0x7F));
    EXPECT_EQ(0xFF, buf.data()[1]);
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(1), 0, 1, {}), ElementsAre(0U));
  }
}

TEST(MqcTest, ByteoutCoversNormalStuffingAndCarryPropagationBranches) {
  // 1. Short sequence at prob=46 hitting 0xFE -> 0xFF carry propagation.
  {
    constexpr OPJ_UINT32 kBits[] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0};
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 46}};
    MqcEncoderBuffer buf(32);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, kBits);
    opj_mqc_flush(&mqc);

    ASSERT_EQ(2U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(2), ElementsAre(0xFF, 0x07));
    ExpectNoInvalidMarkers(buf.Written(2));
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(2), 0, std::size(kBits), kInit),
                ElementsAreArray(kBits));
  }

  // 2. Short sequence at prob=0 hitting 0xFE -> 0xFF carry propagation.
  {
    constexpr OPJ_UINT32 kBits[] = {1, 0, 1, 0, 1, 1, 1, 1, 0,
                                    0, 1, 1, 0, 1, 1, 0, 1};
    MqcEncoderBuffer buf(32);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, kBits);
    opj_mqc_flush(&mqc);

    ASSERT_EQ(3U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(3), ElementsAre(0xB8, 0xFF, 0x07));
    ExpectNoInvalidMarkers(buf.Written(3));
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(3), 0, std::size(kBits), {}),
                ElementsAreArray(kBits));
  }

  // 3. Repeating 0x9F pattern at prob=46 triggers mid-stream 0xFE->0xFF carry.
  {
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 46}};
    std::vector<OPJ_UINT32> bits(120);
    for (std::size_t i = 0; i < bits.size(); ++i) {
      bits[i] = (0x9FU >> (i % 8)) & 1U;
    }
    MqcEncoderBuffer buf(128);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, bits);
    opj_mqc_flush(&mqc);

    const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
    ASSERT_GT(num_bytes, 0U);
    ASSERT_LE(num_bytes, buf.capacity());
    ExpectNoInvalidMarkers(buf.Written(num_bytes));
    EXPECT_THAT(
        DecodeBitsSingleCtx(buf.Written(num_bytes), 0, bits.size(), kInit),
        ElementsAreArray(bits));
  }

  // 4. Long uniform-zero sequence triggers 0xFF stuffing, no-carry, and
  // non-0xFF carry propagation inside opj_mqc_byteout.
  {
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 46}};
    std::vector<OPJ_UINT32> zeros(400, 0U);
    MqcEncoderBuffer buf(256);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, zeros);
    opj_mqc_flush(&mqc);

    const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
    ASSERT_GT(num_bytes, 0U);
    ASSERT_LE(num_bytes, buf.capacity());
    ExpectNoInvalidMarkers(buf.Written(num_bytes));
    EXPECT_THAT(
        DecodeBitsSingleCtx(buf.Written(num_bytes), 0, zeros.size(), kInit),
        ElementsAreArray(zeros));
  }

  // 5. Skewed-context all-ones sequence and alternating-bit sequence.
  {
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 15}};
    std::vector<OPJ_UINT32> ones(400, 1U);
    MqcEncoderBuffer buf(256);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, ones);
    opj_mqc_flush(&mqc);

    const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
    ASSERT_GT(num_bytes, 0U);
    ASSERT_LE(num_bytes, buf.capacity());
    ExpectNoInvalidMarkers(buf.Written(num_bytes));
    EXPECT_THAT(
        DecodeBitsSingleCtx(buf.Written(num_bytes), 0, ones.size(), kInit),
        ElementsAreArray(ones));
  }
}

TEST(MqcTest, MultiContextLongSequenceRoundTrips) {
  std::vector<ContextInit> inits;
  inits.reserve(MQC_NUMCTXS);
  for (OPJ_UINT32 c = 0; c < MQC_NUMCTXS; ++c) {
    inits.push_back(ContextInit{
        .ctxno = c,
        .msb = c % 2,
        .prob = static_cast<OPJ_INT32>((c * 5U) % 47U),
    });
  }

  std::vector<SymbolOp> ops;
  std::vector<OPJ_UINT32> expected;
  ops.reserve(1200);
  expected.reserve(1200);
  uint32_t rng = 0x2468ACE1U;
  for (int i = 0; i < 1200; ++i) {
    rng = rng * 1664525U + 1013904223U;
    const OPJ_UINT32 ctx = (rng >> 16) % MQC_NUMCTXS;
    const OPJ_UINT32 bit = (rng >> 31) & 1U;
    ops.push_back(SymbolOp{.ctxno = ctx, .bit = bit});
    expected.push_back(bit);
  }

  MqcEncoderBuffer buf(1024);
  opj_mqc_t mqc{};
  opj_mqc_resetstates(&mqc);
  ApplyContextInits(&mqc, inits);
  opj_mqc_init_enc(&mqc, buf.data());
  for (const SymbolOp& op : ops) {
    opj_mqc_setcurctx(&mqc, op.ctxno);
    EncodeDecision(&mqc, op.bit);
  }
  opj_mqc_flush(&mqc);

  const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
  ASSERT_GT(num_bytes, 0U);
  ASSERT_LE(num_bytes, buf.capacity());
  ExpectNoInvalidMarkers(buf.Written(num_bytes));

  EXPECT_THAT(DecodeSymbolOps(buf.Written(num_bytes), ops, inits),
              ElementsAreArray(expected));
}

TEST(MqcTest, ErtermEncCoversLoopIterationsAndTrailing0xFFAndRoundTrips) {
  // 1. Zero symbols encoded (ct == 12, k == 0): 0 while-loop iterations and
  // extra byteout runs because bp[-1] != 0xFF.
  {
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    opj_mqc_erterm_enc(&mqc);
    EXPECT_EQ(0U, opj_mqc_numbytes(&mqc));
  }

  // 2. 9 zeros at prob=46 leaves ct == 3 (k == 9), running 2 loop iterations
  // plus extra byteout -> {0xAB, 0x55}.
  {
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 46}};
    constexpr std::array<OPJ_UINT32, 9> kZeros{};
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, kZeros);
    opj_mqc_erterm_enc(&mqc);

    ASSERT_EQ(2U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(2), ElementsAre(0xAB, 0x55));
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(2), 0, kZeros.size(), kInit),
                ElementsAreArray(kZeros));
  }

  // 3. 15 zeros at prob=46 leaves *mqc->bp == 0xFF after 1 loop iteration,
  // skipping the final extra byteout and producing 1 byte {0xAB}.
  {
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 46}};
    constexpr std::array<OPJ_UINT32, 15> kZeros{};
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, kZeros);
    opj_mqc_erterm_enc(&mqc);

    ASSERT_EQ(1U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(1), ElementsAre(0xAB));
    EXPECT_EQ(0xFF, buf.data()[1]);
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(1), 0, kZeros.size(), kInit),
                ElementsAreArray(kZeros));
  }

  // 4. 10 ones at prob=4 emits {0x00, 0x00, 0x01, 0x44} and round-trips.
  {
    constexpr ContextInit kInit[] = {{.ctxno = 0, .msb = 0, .prob = 4}};
    constexpr std::array<OPJ_UINT32, 10> kOnes = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInit);
    opj_mqc_init_enc(&mqc, buf.data());
    EncodeBitsSingleCtx(&mqc, 0, kOnes);
    opj_mqc_erterm_enc(&mqc);

    ASSERT_EQ(4U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(4), ElementsAre(0x00, 0x00, 0x01, 0x44));
    EXPECT_THAT(DecodeBitsSingleCtx(buf.Written(4), 0, kOnes.size(), kInit),
                ElementsAreArray(kOnes));
  }

  // 5. Longer multi-context sequence under erterm decodes losslessly.
  {
    constexpr ContextInit kInits[] = {
        {.ctxno = 0, .msb = 0, .prob = 4},
        {.ctxno = 5, .msb = 1, .prob = 12},
        {.ctxno = 17, .msb = 0, .prob = 3},
    };
    std::vector<SymbolOp> ops;
    std::vector<OPJ_UINT32> expected;
    ops.reserve(240);
    expected.reserve(240);
    uint32_t rng = 0x13572468U;
    for (int i = 0; i < 240; ++i) {
      rng = rng * 1664525U + 1013904223U;
      const OPJ_UINT32 ctx = kInits[(rng >> 20) % 3].ctxno;
      const OPJ_UINT32 bit = (rng >> 31) & 1U;
      ops.push_back(SymbolOp{.ctxno = ctx, .bit = bit});
      expected.push_back(bit);
    }

    MqcEncoderBuffer buf(256);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    ApplyContextInits(&mqc, kInits);
    opj_mqc_init_enc(&mqc, buf.data());
    for (const SymbolOp& op : ops) {
      opj_mqc_setcurctx(&mqc, op.ctxno);
      EncodeDecision(&mqc, op.bit);
    }
    opj_mqc_erterm_enc(&mqc);

    const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
    ASSERT_GT(num_bytes, 0U);
    ASSERT_LE(num_bytes, buf.capacity());
    ExpectNoInvalidMarkers(buf.Written(num_bytes),
                           /*check_no_trailing_ff=*/false);
    EXPECT_THAT(DecodeSymbolOps(buf.Written(num_bytes), ops, kInits),
                ElementsAreArray(expected));
  }
}

TEST(MqcTest, SegmarkEncCodesMarkerAcrossContextsAndRoundTrips) {
  // 1. Standard usage after opj_mqc_reset_enc (uniform ctx 18 = prob 46).
  {
    constexpr std::array<SymbolOp, 8> kOps = {{
        {.ctxno = 0, .bit = 1},
        {.ctxno = 0, .bit = 0},
        {.ctxno = 17, .bit = 1},
        {.ctxno = 0, .bit = 1},
        {.ctxno = T1_CTXNO_UNI, .bit = 1},
        {.ctxno = T1_CTXNO_UNI, .bit = 0},
        {.ctxno = T1_CTXNO_UNI, .bit = 1},
        {.ctxno = T1_CTXNO_UNI, .bit = 0},
    }};
    MqcEncoderBuffer buf(32);
    opj_mqc_t mqc{};
    opj_mqc_reset_enc(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    for (std::size_t i = 0; i < 4; ++i) {
      opj_mqc_setcurctx(&mqc, kOps[i].ctxno);
      EncodeDecision(&mqc, kOps[i].bit);
    }
    opj_mqc_segmark_enc(&mqc);
    opj_mqc_flush(&mqc);

    const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
    ASSERT_GT(num_bytes, 0U);
    EXPECT_EQ(&mqc.ctxs[T1_CTXNO_UNI], mqc.curctx);
    EXPECT_THAT(DecodeSymbolOps(buf.Written(num_bytes), kOps, {},
                                /*use_reset_enc=*/true),
                ElementsAre(1U, 0U, 1U, 1U, 1U, 0U, 1U, 0U));
  }

  // 2. Repeated segmark calls with non-uniform state on ctx 18 exercise both
  // renorm/no-renorm codemps/codelps branches and byteout via opj_mqc_renorme.
  for (const ContextInit init :
       {ContextInit{.ctxno = T1_CTXNO_UNI, .msb = 0, .prob = 4},
        ContextInit{.ctxno = T1_CTXNO_UNI, .msb = 1, .prob = 0}}) {
    SCOPED_TRACE(init.msb);
    MqcEncoderBuffer buf(32);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_setstate(&mqc, init.ctxno, init.msb, init.prob);
    opj_mqc_init_enc(&mqc, buf.data());

    for (int rep = 0; rep < 5; ++rep) {
      opj_mqc_segmark_enc(&mqc);
    }
    opj_mqc_flush(&mqc);

    const OPJ_UINT32 num_bytes = opj_mqc_numbytes(&mqc);
    ASSERT_GT(num_bytes, 0U);
    ExpectNoInvalidMarkers(buf.Written(num_bytes));
    const std::vector<OPJ_UINT32> decoded =
        DecodeBitsSingleCtx(buf.Written(num_bytes), T1_CTXNO_UNI, 20,
                            std::span<const ContextInit>(&init, 1));
    EXPECT_THAT(decoded, ElementsAre(1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U,
                                     0U, 1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U));
  }
}

TEST(MqcTest, BypassInitGetExtraBytesAndFlushPreserveMarkers) {
  // 1. Zero bypass bits after an empty MQ flush ({0xFF, 0x7F}) leaves the
  // preceding MQ pass intact because ct == BYPASS_CT_INIT != 8.
  {
    MqcEncoderBuffer buf(16);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    opj_mqc_flush(&mqc);
    ASSERT_EQ(2U, opj_mqc_numbytes(&mqc));

    opj_mqc_bypass_init_enc(&mqc);
    EXPECT_EQ(BYPASS_CT_INIT, mqc.ct);
    EXPECT_EQ(0U, opj_mqc_bypass_get_extra_bytes(&mqc, OPJ_FALSE));
    EXPECT_EQ(0U, opj_mqc_bypass_get_extra_bytes(&mqc, OPJ_TRUE));
    opj_mqc_bypass_flush_enc(&mqc, OPJ_FALSE);
    ASSERT_EQ(2U, opj_mqc_numbytes(&mqc));
    EXPECT_THAT(buf.Written(2), ElementsAre(0xFF, 0x7F));
  }

  struct BypassCase {
    std::string_view name;
    std::array<OPJ_UINT32, 16> bits;
    std::size_t num_bits;
    OPJ_BOOL erterm;
    OPJ_UINT32 expected_extra_false;
    OPJ_UINT32 expected_extra_true;
    std::array<OPJ_BYTE, 4> expected_bytes;
    std::size_t expected_len;
  };
  constexpr BypassCase kCases[] = {
      {.name = "SingleZeroBitCt7NotAfterFF",
       .bits = {0},
       .num_bits = 1,
       .erterm = OPJ_FALSE,
       .expected_extra_false = 1U,
       .expected_extra_true = 1U,
       .expected_bytes = {0x2A},
       .expected_len = 1},
      {.name = "SingleZeroBitCt7Erterm",
       .bits = {0},
       .num_bits = 1,
       .erterm = OPJ_TRUE,
       .expected_extra_false = 1U,
       .expected_extra_true = 1U,
       .expected_bytes = {0x2A},
       .expected_len = 1},
      {.name = "ThreeBitsCt5AlternatingPad",
       .bits = {1, 0, 1},
       .num_bits = 3,
       .erterm = OPJ_FALSE,
       .expected_extra_false = 1U,
       .expected_extra_true = 1U,
       .expected_bytes = {0xAA},
       .expected_len = 1},
      {.name = "FullByteNonFFCt8",
       .bits = {1, 0, 1, 0, 0, 1, 0, 1},
       .num_bits = 8,
       .erterm = OPJ_FALSE,
       .expected_extra_false = 0U,
       .expected_extra_true = 0U,
       .expected_bytes = {0xA5},
       .expected_len = 1},
      {.name = "EightOnesTrailingFFDiscardedWhenNotErterm",
       .bits = {1, 1, 1, 1, 1, 1, 1, 1},
       .num_bits = 8,
       .erterm = OPJ_FALSE,
       .expected_extra_false = 0U,
       .expected_extra_true = 1U,
       .expected_bytes = {},
       .expected_len = 0},
      {.name = "EightOnesTrailingFFKeptWith0x2AWhenErterm",
       .bits = {1, 1, 1, 1, 1, 1, 1, 1},
       .num_bits = 8,
       .erterm = OPJ_TRUE,
       .expected_extra_false = 0U,
       .expected_extra_true = 1U,
       .expected_bytes = {0xFF, 0x2A},
       .expected_len = 2},
      {.name = "FifteenOnesTrailingFF7FStrippedWhenNotErterm",
       .bits = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
       .num_bits = 15,
       .erterm = OPJ_FALSE,
       .expected_extra_false = 0U,
       .expected_extra_true = 0U,
       .expected_bytes = {},
       .expected_len = 0},
      {.name = "FifteenOnesTrailingFF7FKeptWhenErterm",
       .bits = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
       .num_bits = 15,
       .erterm = OPJ_TRUE,
       .expected_extra_false = 0U,
       .expected_extra_true = 0U,
       .expected_bytes = {0xFF, 0x7F},
       .expected_len = 2},
      {.name = "TenBitsAfter0xFFBitStuffing",
       .bits = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
       .num_bits = 10,
       .erterm = OPJ_FALSE,
       .expected_extra_false = 1U,
       .expected_extra_true = 1U,
       .expected_bytes = {0xFF, 0x0A},
       .expected_len = 2},
  };

  for (const BypassCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    MqcEncoderBuffer buf(32);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    opj_mqc_flush(&mqc);
    const OPJ_UINT32 prefix_len = opj_mqc_numbytes(&mqc);

    opj_mqc_bypass_init_enc(&mqc);
    for (std::size_t i = 0; i < tc.num_bits; ++i) {
      opj_mqc_bypass_enc(&mqc, tc.bits[i]);
    }
    EXPECT_EQ(tc.expected_extra_false,
              opj_mqc_bypass_get_extra_bytes(&mqc, OPJ_FALSE));
    EXPECT_EQ(tc.expected_extra_true,
              opj_mqc_bypass_get_extra_bytes(&mqc, OPJ_TRUE));

    opj_mqc_bypass_flush_enc(&mqc, tc.erterm);
    const OPJ_UINT32 total_len = opj_mqc_numbytes(&mqc);
    ASSERT_GE(total_len, prefix_len);
    const OPJ_UINT32 bypass_len = total_len - prefix_len;
    ASSERT_EQ(tc.expected_len, bypass_len);

    const std::span<const OPJ_BYTE> bypass_bytes(buf.data() + prefix_len,
                                                 bypass_len);
    for (std::size_t i = 0; i < tc.expected_len; ++i) {
      EXPECT_EQ(tc.expected_bytes[i], bypass_bytes[i]) << "byte " << i;
    }
    ExpectNoInvalidMarkers(buf.Written(total_len));

    const std::span<const OPJ_UINT32> expected_bits(tc.bits.data(),
                                                    tc.num_bits);
    EXPECT_THAT(DecodeRawBits(bypass_bytes, tc.num_bits),
                ElementsAreArray(expected_bits));
  }
}

TEST(MqcTest, BypassLongSequenceWithMultiple0xFFStuffingRoundTrips) {
  std::vector<OPJ_UINT32> bits;
  bits.reserve(240);
  uint32_t rng = 0x9E3779B9U;
  for (int i = 0; i < 240; ++i) {
    if (i < 16 || (i >= 64 && i < 79) || (i >= 160 && i < 168)) {
      bits.push_back(1U);
    } else {
      rng = rng * 1664525U + 1013904223U;
      bits.push_back((rng >> 30) & 1U);
    }
  }

  for (const OPJ_BOOL erterm : {OPJ_FALSE, OPJ_TRUE}) {
    SCOPED_TRACE(erterm);
    MqcEncoderBuffer buf(128);
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_enc(&mqc, buf.data());
    opj_mqc_flush(&mqc);
    const OPJ_UINT32 prefix_len = opj_mqc_numbytes(&mqc);

    opj_mqc_bypass_init_enc(&mqc);
    for (OPJ_UINT32 bit : bits) {
      opj_mqc_bypass_enc(&mqc, bit);
    }
    opj_mqc_bypass_flush_enc(&mqc, erterm);

    const OPJ_UINT32 total_len = opj_mqc_numbytes(&mqc);
    ASSERT_GT(total_len, prefix_len);
    ASSERT_LE(total_len, buf.capacity());
    ExpectNoInvalidMarkers(buf.Written(total_len));

    const std::span<const OPJ_BYTE> bypass_bytes(buf.data() + prefix_len,
                                                 total_len - prefix_len);
    EXPECT_THAT(DecodeRawBits(bypass_bytes, bits.size()),
                ElementsAreArray(bits));
  }
}

TEST(MqcTest, RestartInitEncSeparatesPassesAcrossFlushErtermAndBypass) {
  MqcEncoderBuffer buf(64);
  opj_mqc_t mqc{};
  opj_mqc_reset_enc(&mqc);
  opj_mqc_init_enc(&mqc, buf.data());

  // 1. Pass 0: MQ pass terminated with opj_mqc_flush.
  constexpr OPJ_UINT32 kPass0Bits[] = {1, 0, 1, 1, 0, 0, 1, 0};
  EncodeBitsSingleCtx(&mqc, 0, kPass0Bits);
  opj_mqc_flush(&mqc);
  const OPJ_UINT32 end0 = opj_mqc_numbytes(&mqc);
  ASSERT_EQ(2U, end0);
  const std::vector<OPJ_BYTE> pass0_copy(buf.data(), buf.data() + end0);

  // 2. Pass 1: restart_init_enc then a flush discarding trailing 0xFF.
  opj_mqc_restart_init_enc(&mqc);
  opj_mqc_setstate(&mqc, 0, 0, 0);
  constexpr OPJ_UINT32 kPass1Bits[] = {0};
  EncodeBitsSingleCtx(&mqc, 0, kPass1Bits);
  opj_mqc_flush(&mqc);
  const OPJ_UINT32 end1 = opj_mqc_numbytes(&mqc);
  ASSERT_EQ(3U, end1);
  const std::vector<OPJ_BYTE> pass1_copy(buf.data() + end0, buf.data() + end1);

  // 3. Pass 2: restart_init_enc followed by erterm_enc leaving trailing 0xFF.
  opj_mqc_restart_init_enc(&mqc);
  opj_mqc_setstate(&mqc, 0, 0, 46);
  constexpr std::array<OPJ_UINT32, 15> kPass2Bits{};
  EncodeBitsSingleCtx(&mqc, 0, kPass2Bits);
  opj_mqc_erterm_enc(&mqc);
  const OPJ_UINT32 end2 = opj_mqc_numbytes(&mqc);
  ASSERT_EQ(4U, end2);
  const std::vector<OPJ_BYTE> pass2_copy(buf.data() + end1, buf.data() + end2);

  // 4. Pass 3: bypass_init_enc -> RAW bits -> bypass_flush_enc(OPJ_FALSE).
  opj_mqc_bypass_init_enc(&mqc);
  constexpr OPJ_UINT32 kPass3Bits[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0};
  for (OPJ_UINT32 bit : kPass3Bits) {
    opj_mqc_bypass_enc(&mqc, bit);
  }
  opj_mqc_bypass_flush_enc(&mqc, OPJ_FALSE);
  const OPJ_UINT32 end3 = opj_mqc_numbytes(&mqc);
  ASSERT_EQ(6U, end3);
  const std::vector<OPJ_BYTE> pass3_copy(buf.data() + end2, buf.data() + end3);

  // 5. Pass 4: restart_init_enc + reset_enc + payload + segmark + flush.
  opj_mqc_restart_init_enc(&mqc);
  opj_mqc_reset_enc(&mqc);
  constexpr OPJ_UINT32 kPass4Bits[] = {0, 1, 0, 1};
  EncodeBitsSingleCtx(&mqc, 0, kPass4Bits);
  opj_mqc_segmark_enc(&mqc);
  opj_mqc_flush(&mqc);
  const OPJ_UINT32 end4 = opj_mqc_numbytes(&mqc);
  ASSERT_EQ(8U, end4);

  // Earlier segments must remain untouched by subsequent pass resets/flushes.
  EXPECT_THAT(std::span<const OPJ_BYTE>(buf.data(), end0),
              ElementsAreArray(pass0_copy));
  EXPECT_THAT(std::span<const OPJ_BYTE>(buf.data() + end0, end1 - end0),
              ElementsAreArray(pass1_copy));
  EXPECT_THAT(std::span<const OPJ_BYTE>(buf.data() + end1, end2 - end1),
              ElementsAreArray(pass2_copy));
  EXPECT_THAT(std::span<const OPJ_BYTE>(buf.data() + end2, end3 - end2),
              ElementsAreArray(pass3_copy));

  // Decode each pass independently.
  constexpr ContextInit kPass0Init[] = {{.ctxno = 0, .msb = 0, .prob = 4}};
  constexpr ContextInit kPass2Init[] = {{.ctxno = 0, .msb = 0, .prob = 46}};
  EXPECT_THAT(
      DecodeBitsSingleCtx(pass0_copy, 0, std::size(kPass0Bits), kPass0Init),
      ElementsAreArray(kPass0Bits));
  EXPECT_THAT(DecodeBitsSingleCtx(pass1_copy, 0, std::size(kPass1Bits), {}),
              ElementsAreArray(kPass1Bits));
  EXPECT_THAT(DecodeBitsSingleCtx(pass2_copy, 0, kPass2Bits.size(), kPass2Init),
              ElementsAreArray(kPass2Bits));
  EXPECT_THAT(DecodeRawBits(pass3_copy, std::size(kPass3Bits)),
              ElementsAreArray(kPass3Bits));

  constexpr std::array<SymbolOp, 8> kPass4Ops = {{
      {.ctxno = 0, .bit = 0},
      {.ctxno = 0, .bit = 1},
      {.ctxno = 0, .bit = 0},
      {.ctxno = 0, .bit = 1},
      {.ctxno = T1_CTXNO_UNI, .bit = 1},
      {.ctxno = T1_CTXNO_UNI, .bit = 0},
      {.ctxno = T1_CTXNO_UNI, .bit = 1},
      {.ctxno = T1_CTXNO_UNI, .bit = 0},
  }};
  EXPECT_THAT(
      DecodeSymbolOps(std::span<const OPJ_BYTE>(buf.data() + end3, end4 - end3),
                      kPass4Ops, {}, /*use_reset_enc=*/true),
      ElementsAre(0U, 1U, 0U, 1U, 1U, 0U, 1U, 0U));
}

TEST(MqcTest, InitDecAndFinishDecHandleEdgeLengthsAndRestoreSentinels) {
  // 1. len == 0: artificial 0xFF 0xFF at start triggers bytein marker branch
  // and opq_mqc_finish_dec restores original tail bytes.
  {
    std::array<OPJ_BYTE, OPJ_COMMON_CBLK_DATA_EXTRA + 2> buf = {0x12, 0x34,
                                                                0x77, 0x88};
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_dec(&mqc, buf.data(), 0, OPJ_COMMON_CBLK_DATA_EXTRA + 2);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    EXPECT_EQ(buf.data(), mqc.start);
    EXPECT_EQ(buf.data(), mqc.end);
    EXPECT_EQ(buf.data(), mqc.bp);
    EXPECT_EQ(0x8000U, mqc.a);
    EXPECT_EQ(1U, mqc.end_of_byte_stream_counter);
    EXPECT_EQ(0xFF, buf[0]);
    EXPECT_EQ(0xFF, buf[1]);
    EXPECT_EQ(0x77, buf[2]);
    EXPECT_EQ(0x88, buf[3]);

    std::array<OPJ_UINT32, 8> bits{};
    for (OPJ_UINT32& b : bits) {
      b = DecodeDecision(&mqc);
    }
    EXPECT_THAT(bits, ElementsAre(1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U));
    EXPECT_GE(mqc.end_of_byte_stream_counter, 1U);

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(0x12, 0x34, 0x77, 0x88));
  }

  // 2. len == 1 with non-0xFF and 0xFF single-byte buffers.
  for (const OPJ_BYTE single_byte : {OPJ_BYTE{0x7F}, OPJ_BYTE{0xFF}}) {
    SCOPED_TRACE(static_cast<unsigned>(single_byte));
    std::array<OPJ_BYTE, 1 + OPJ_COMMON_CBLK_DATA_EXTRA> buf = {single_byte,
                                                                0x5A, 0xC3};
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_dec(&mqc, buf.data(), 1, OPJ_COMMON_CBLK_DATA_EXTRA);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    EXPECT_EQ(buf.data(), mqc.start);
    EXPECT_EQ(buf.data() + 1, mqc.end);
    EXPECT_EQ(0xFF, buf[1]);
    EXPECT_EQ(0xFF, buf[2]);
    if (single_byte == 0xFF) {
      EXPECT_EQ(1U, mqc.end_of_byte_stream_counter);
    } else {
      EXPECT_EQ(0U, mqc.end_of_byte_stream_counter);
    }
    const OPJ_UINT32 d = DecodeDecision(&mqc);
    EXPECT_EQ(single_byte == 0xFF ? 1U : 0U, d);

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(single_byte, 0x5A, 0xC3));
  }

  // 3. len >= 2 covering stuffed (0xFF <= 0x8F), terminating (0xFF > 0x8F),
  // and normal pairs in opj_mqc_bytein.
  struct Len2Case {
    std::array<OPJ_BYTE, 2> payload;
    OPJ_UINT32 expected_init_eob;
    std::array<OPJ_UINT32, 16> expected_bits;
  };
  constexpr Len2Case kLen2Cases[] = {
      {.payload = {0xFF, 0x2A},
       .expected_init_eob = 0U,
       .expected_bits = {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
                         1U, 1U}},
      {.payload = {0xFF, 0x8F},
       .expected_init_eob = 0U,
       .expected_bits = {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
                         1U, 1U}},
      {.payload = {0xFF, 0x90},
       .expected_init_eob = 1U,
       .expected_bits = {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
                         1U, 1U}},
      {.payload = {0x12, 0x34},
       .expected_init_eob = 0U,
       .expected_bits = {0U, 1U, 1U, 1U, 1U, 0U, 0U, 0U, 1U, 1U, 0U, 0U, 0U, 1U,
                         0U, 1U}},
  };
  for (const Len2Case& tc : kLen2Cases) {
    std::array<OPJ_BYTE, 2 + OPJ_COMMON_CBLK_DATA_EXTRA> buf = {
        tc.payload[0], tc.payload[1], 0xDE, 0xAD};
    opj_mqc_t mqc{};
    opj_mqc_resetstates(&mqc);
    opj_mqc_init_dec(&mqc, buf.data(), 2, OPJ_COMMON_CBLK_DATA_EXTRA);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    EXPECT_EQ(tc.expected_init_eob, mqc.end_of_byte_stream_counter);
    EXPECT_EQ(0xFF, buf[2]);
    EXPECT_EQ(0xFF, buf[3]);
    std::array<OPJ_UINT32, 16> bits{};
    for (OPJ_UINT32& b : bits) {
      b = DecodeDecision(&mqc);
    }
    EXPECT_THAT(bits, ElementsAreArray(tc.expected_bits));

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(tc.payload[0], tc.payload[1], 0xDE, 0xAD));
  }
}

TEST(MqcTest, RawInitDecAndFinishDecHandleEdgeLengthsAndRestoreSentinels) {
  // 1. len == 0: artificial 0xFF 0xFF marker yields 1-bits and restores tail.
  {
    std::array<OPJ_BYTE, OPJ_COMMON_CBLK_DATA_EXTRA + 1> buf = {0x31, 0x42,
                                                                0x99};
    opj_mqc_t mqc{};
    opj_mqc_raw_init_dec(&mqc, buf.data(), 0, OPJ_COMMON_CBLK_DATA_EXTRA + 1);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    EXPECT_EQ(buf.data(), mqc.start);
    EXPECT_EQ(buf.data(), mqc.end);
    EXPECT_EQ(buf.data(), mqc.bp);
    EXPECT_EQ(0U, mqc.c);
    EXPECT_EQ(0U, mqc.ct);
    EXPECT_EQ(0xFF, buf[0]);
    EXPECT_EQ(0xFF, buf[1]);
    EXPECT_EQ(0x99, buf[2]);

    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(1U, opj_mqc_raw_decode(&mqc)) << "bit " << i;
    }

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(0x31, 0x42, 0x99));
  }

  // 2. len == 1: {0xA5} decodes 10100101; {0xFF} decodes 16 ones via tail.
  {
    std::array<OPJ_BYTE, 1 + OPJ_COMMON_CBLK_DATA_EXTRA> buf = {0xA5, 0x11,
                                                                0x22};
    opj_mqc_t mqc{};
    opj_mqc_raw_init_dec(&mqc, buf.data(), 1, OPJ_COMMON_CBLK_DATA_EXTRA);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    std::array<OPJ_UINT32, 8> bits{};
    for (OPJ_UINT32& b : bits) {
      b = opj_mqc_raw_decode(&mqc);
    }
    EXPECT_THAT(bits, ElementsAre(1U, 0U, 1U, 0U, 0U, 1U, 0U, 1U));
    EXPECT_EQ(1U, opj_mqc_numbytes(&mqc));

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(0xA5, 0x11, 0x22));
  }
  {
    std::array<OPJ_BYTE, 1 + OPJ_COMMON_CBLK_DATA_EXTRA> buf = {0xFF, 0x11,
                                                                0x22};
    opj_mqc_t mqc{};
    opj_mqc_raw_init_dec(&mqc, buf.data(), 1, OPJ_COMMON_CBLK_DATA_EXTRA);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(1U, opj_mqc_raw_decode(&mqc)) << "bit " << i;
    }
    EXPECT_EQ(1U, opj_mqc_numbytes(&mqc));

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(0xFF, 0x11, 0x22));
  }

  // 3. len >= 2: 0xFF followed by <= 0x8F uses 7-bit stuffed read; > 0x8F
  // synthesizes 0xFF without advancing past the marker.
  {
    std::array<OPJ_BYTE, 2 + OPJ_COMMON_CBLK_DATA_EXTRA> buf = {0xFF, 0x55,
                                                                0x66, 0x77};
    opj_mqc_t mqc{};
    opj_mqc_raw_init_dec(&mqc, buf.data(), 2, OPJ_COMMON_CBLK_DATA_EXTRA);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    std::array<OPJ_UINT32, 15> bits{};
    for (OPJ_UINT32& b : bits) {
      b = opj_mqc_raw_decode(&mqc);
    }
    EXPECT_THAT(bits, ElementsAre(1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 0U, 1U,
                                  0U, 1U, 0U, 1U));
    EXPECT_EQ(2U, opj_mqc_numbytes(&mqc));

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(0xFF, 0x55, 0x66, 0x77));
  }
  {
    std::array<OPJ_BYTE, 2 + OPJ_COMMON_CBLK_DATA_EXTRA> buf = {0xFF, 0x90,
                                                                0x66, 0x77};
    opj_mqc_t mqc{};
    opj_mqc_raw_init_dec(&mqc, buf.data(), 2, OPJ_COMMON_CBLK_DATA_EXTRA);
    absl::Cleanup finish = [&mqc] { opq_mqc_finish_dec(&mqc); };

    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(1U, opj_mqc_raw_decode(&mqc)) << "bit " << i;
    }
    EXPECT_EQ(1U, opj_mqc_numbytes(&mqc));

    std::move(finish).Invoke();
    EXPECT_THAT(buf, ElementsAre(0xFF, 0x90, 0x66, 0x77));
  }
}

// TODO(schwehr): Cover remaining defensive/unreachable error branches in mqc.c.

}  // namespace
}  // namespace openjpeg2
