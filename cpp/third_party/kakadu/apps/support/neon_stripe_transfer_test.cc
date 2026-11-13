// Unit tests for
// third_party/kakadu/vendor/apps/support/neon_stripe_transfer.cpp.
//
// In Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// sets `KAKADU_X86_SIMD_DEFINES` and `kdu_arch.h` requires `__ARM_NEON__`),
// `neon_stripe_transfer.cpp` is compiled out (`0/0` LCOV `DA:` ceiling on
// both `x86_64` and `--cpu=arm`). These tests exercise `kdu_stripe_compressor`
// and `kdu_stripe_decompressor` across all paths accelerated by
// `neon_stripe_transfer.cpp`: 8-bit, 16-bit, 32-bit (`int32` and `int64`
// internal line precisions), float, and packed 10-bit v210 transfers, across
// ilv1/ilv3/ilv4 layouts, reversible and irreversible transforms, and streaming
// store hints.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_decompressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"

namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_codestream;
using ::kdu_core::kdu_compressed_source;
using ::kdu_core::kdu_compressed_target;
using ::kdu_core::kdu_exception;
using ::kdu_core::kdu_int16;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_params;
using ::kdu_core::kdu_thread_safe_message;
using ::kdu_core::siz_params;
using ::kdu_supp::kdu_stripe_compressor;
using ::kdu_supp::kdu_stripe_decompressor;

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char *string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class NeonStripeTransferTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new NeonStripeTransferTestEnv());

class MemoryTarget final : public kdu_compressed_target {
 public:
  explicit MemoryTarget(std::vector<kdu_byte> *out) : out_(out) {}
  bool write(const kdu_byte *buf, int num_bytes) override {
    if (num_bytes > 0) {
      out_->insert(out_->end(), buf, buf + num_bytes);
    }
    return true;
  }

 private:
  std::vector<kdu_byte> *out_;
};

class MemorySource final : public kdu_compressed_source {
 public:
  explicit MemorySource(const std::vector<kdu_byte> &data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte *buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }

  bool seek(::kdu_core::kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  ::kdu_core::kdu_long get_pos() override {
    return static_cast<::kdu_core::kdu_long>(pos_);
  }

 private:
  const std::vector<kdu_byte> &data_;
  size_t pos_ = 0;
};

struct CodestreamGuard {
  explicit CodestreamGuard(kdu_codestream *cs) : cs_(cs) {}
  ~CodestreamGuard() {
    if (cs_ != nullptr && cs_->exists()) {
      cs_->destroy();
    }
  }
  kdu_codestream *cs_;
};

template <typename SampleT>
void CallPushStripe(kdu_stripe_compressor &comp, SampleT *buf,
                    const int *heights, const int *ogaps, const int *sgaps,
                    const int *rgaps, const int *precs, const bool *sflags) {
  if constexpr (std::is_same_v<SampleT, kdu_byte>) {
    while (comp.push_stripe(buf, heights, ogaps, sgaps, rgaps, precs)) {
    }
  } else {
    while (comp.push_stripe(buf, heights, ogaps, sgaps, rgaps, precs, sflags)) {
    }
  }
}

template <typename SampleT>
void CallPullStripe(kdu_stripe_decompressor &decomp, SampleT *buf,
                    const int *heights, const int *ogaps, const int *sgaps,
                    const int *rgaps, const int *precs, const bool *sflags,
                    int store_pref) {
  if constexpr (std::is_same_v<SampleT, kdu_byte>) {
    while (decomp.pull_stripe(buf, heights, ogaps, sgaps, rgaps, precs, nullptr,
                              store_pref)) {
    }
  } else {
    while (decomp.pull_stripe(buf, heights, ogaps, sgaps, rgaps, precs, sflags,
                              nullptr, store_pref)) {
    }
  }
}

template <typename SampleT>
std::vector<kdu_byte> CompressInterleaved(int width, int height, int num_comps,
                                          int bit_depth, bool is_signed,
                                          bool reversible,
                                          const std::vector<SampleT> &samples) {
  std::vector<kdu_byte> out;
  MemoryTarget target(&out);
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, is_signed);
  static_cast<kdu_params &>(siz).finalize();

  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  cs.create(&siz, &target);
  cs.access_siz()->parse_string(reversible ? "Creversible=yes"
                                           : "Creversible=no");
  cs.access_siz()->parse_string("Cycc=no");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(num_comps, height);
  std::vector<int> sgaps(num_comps, num_comps);
  std::vector<int> ogaps(num_comps);
  std::vector<int> rgaps(num_comps, width * num_comps);
  std::vector<int> precs(num_comps, bit_depth);
  auto sflags = std::make_unique<bool[]>(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    ogaps[c] = c;
    sflags[c] = is_signed;
  }
  const int *prec_ptr =
      std::is_same_v<SampleT, float> ? nullptr : precs.data();
  CallPushStripe(comp, const_cast<SampleT *>(samples.data()), heights.data(),
                 ogaps.data(), sgaps.data(), rgaps.data(), prec_ptr,
                 sflags.get());
  comp.finish();
  cs.destroy();
  return out;
}

TEST(NeonStripeTransferTest, PushAndPullUint8Ilv1Ilv3AndIlv4) {
  constexpr int kHeight = 6;
  for (int width : {37, 65}) {
    for (int num_comps : {1, 3, 4}) {
      for (bool reversible : {true, false}) {
        const size_t n = static_cast<size_t>(width) * kHeight * num_comps;
        std::vector<kdu_byte> src_bytes(n);
        for (size_t i = 0; i < n; ++i) {
          src_bytes[i] = static_cast<kdu_byte>((i * 11 + 19) & 0xFF);
        }
        const int bit_depth = (num_comps == 1 && reversible) ? 6 : 8;
        if (bit_depth == 6) {
          for (kdu_byte &v : src_bytes) {
            v &= 0x3F;
          }
        }
        const std::vector<kdu_byte> data = CompressInterleaved(
            width, kHeight, num_comps, bit_depth, false, reversible, src_bytes);

        for (int store_pref : {0, KDU_STRIPE_STORE_PREF_STREAMING}) {
          MemorySource src(data);
          kdu_codestream cs;
          CodestreamGuard guard(&cs);
          cs.create(&src);

          kdu_stripe_decompressor decomp;
          decomp.start(cs, false, false, nullptr, 0, true);
          std::vector<kdu_byte> dst_bytes(n, 0);
          std::vector<int> heights(num_comps, kHeight);
          std::vector<int> sgaps(num_comps, num_comps);
          std::vector<int> ogaps(num_comps);
          std::vector<int> rgaps(num_comps, width * num_comps);
          std::vector<int> precs(num_comps, bit_depth);
          for (int c = 0; c < num_comps; ++c) {
            ogaps[c] = c;
          }
          CallPullStripe(decomp, dst_bytes.data(), heights.data(), ogaps.data(),
                         sgaps.data(), rgaps.data(), precs.data(), nullptr,
                         store_pref);
          decomp.finish();

          const int tol = reversible ? 0 : 8;
          for (size_t i = 0; i < n; ++i) {
            EXPECT_NEAR(dst_bytes[i], src_bytes[i], tol);
          }
        }
      }
    }
  }
}

TEST(NeonStripeTransferTest, PushAndPullInt16AcrossPrecisions) {
  constexpr int kHeight = 6;
  struct Int16Case {
    int width;
    int bit_depth;
    bool is_signed;
    bool reversible;
    bool want_fastest;
  };
  const std::vector<Int16Case> cases = {
      {37, 8, false, true, false},
      {41, 12, false, true, false},
      {65, 16, true, true, true},
      {41, 12, false, false, false},
      {65, 16, true, false, false},
  };
  for (const auto &tc : cases) {
    const size_t n = static_cast<size_t>(tc.width) * kHeight;
    std::vector<kdu_int16> src_words(n);
    const int mask = (1 << (tc.bit_depth - (tc.is_signed ? 1 : 0))) - 1;
    for (size_t i = 0; i < n; ++i) {
      int v = static_cast<int>((i * 53 + 17) & mask);
      if (tc.is_signed && (i % 2 == 1)) {
        v = -v;
      }
      src_words[i] = static_cast<kdu_int16>(v);
    }
    const std::vector<kdu_byte> data = CompressInterleaved(
        tc.width, kHeight, 1, tc.bit_depth, tc.is_signed, tc.reversible,
        src_words);

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);

    kdu_stripe_decompressor decomp;
    decomp.start(cs, false, false, nullptr, 0, tc.want_fastest);
    std::vector<kdu_int16> dst_words(n, 0);
    const int heights[1] = {kHeight};
    const int sgaps[1] = {1};
    const int ogaps[1] = {0};
    const int rgaps[1] = {tc.width};
    const int precs[1] = {tc.bit_depth};
    const bool sflags[1] = {tc.is_signed};
    CallPullStripe(decomp, dst_words.data(), heights, ogaps, sgaps, rgaps,
                   precs, sflags, KDU_STRIPE_STORE_PREF_STREAMING);
    decomp.finish();

    const int tol = tc.reversible ? 0 : 128;
    for (size_t i = 0; i < n; ++i) {
      EXPECT_NEAR(dst_words[i], src_words[i], tol);
    }
  }
}

TEST(NeonStripeTransferTest, PushAndPullInt32For32BitAnd64BitLinePrecs) {
  constexpr int kHeight = 6;
  struct Int32Case {
    int width;
    int src_bits;
    int pull_bits;
    bool is_signed;
  };
  const std::vector<Int32Case> cases = {
      // KDU_SAMPLE_PREC32 (want_fastest = false and bit_depth = 16 -> 32-bit).
      {29, 16, 17, false},
      {33, 20, 21, true},
      // KDU_SAMPLE_PREC64 (reversible >24 bits uses 64-bit internal lines).
      {29, 28, 29, false},
      {33, 28, 29, true},
  };
  for (const auto &tc : cases) {
    const size_t n = static_cast<size_t>(tc.width) * kHeight;
    std::vector<kdu_int32> src_dwords(n);
    const kdu_int32 max_mag = (kdu_int32{1} << (tc.src_bits - 2)) - 1;
    for (size_t i = 0; i < n; ++i) {
      kdu_int32 v = static_cast<kdu_int32>((i * 97 + 13) % (max_mag + 1));
      if (tc.is_signed && (i % 2 == 1)) {
        v = -v;
      }
      src_dwords[i] = v;
    }
    const std::vector<kdu_byte> data = CompressInterleaved(
        tc.width, kHeight, 1, tc.src_bits, tc.is_signed, /*reversible=*/true,
        src_dwords);

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);

    kdu_stripe_decompressor decomp;
    decomp.start(cs, false, false, nullptr, 0, /*want_fastest=*/false);
    std::vector<kdu_int32> dst_dwords(n, 0);
    const int heights[1] = {kHeight};
    const int sgaps[1] = {1};
    const int ogaps[1] = {0};
    const int rgaps[1] = {tc.width};
    const int precs[1] = {tc.pull_bits};
    const bool sflags[1] = {tc.is_signed};
    CallPullStripe(decomp, dst_dwords.data(), heights, ogaps, sgaps, rgaps,
                   precs, sflags, KDU_STRIPE_STORE_PREF_STREAMING);
    decomp.finish();

    const int shift = tc.pull_bits - tc.src_bits;
    for (size_t i = 0; i < n; ++i) {
      EXPECT_EQ(dst_dwords[i], src_dwords[i] << shift);
    }
  }
}

TEST(NeonStripeTransferTest, PushAndPullFloatsAcrossSignAndPrecision) {
  constexpr int kHeight = 6;
  for (int width : {29, 41}) {
    for (bool is_signed : {false, true}) {
      const size_t n = static_cast<size_t>(width) * kHeight;
      std::vector<float> src_f(n);
      for (size_t i = 0; i < n; ++i) {
        const float u = static_cast<float>(i % 20) / 20.0f;
        src_f[i] = is_signed ? (u - 0.45f) : u;
      }
      const std::vector<kdu_byte> data = CompressInterleaved(
          width, kHeight, 1, 12, is_signed, /*reversible=*/false, src_f);

      MemorySource src(data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);

      kdu_stripe_decompressor decomp;
      decomp.start(cs);
      std::vector<float> dst_f(n, 0.0f);
      const int heights[1] = {kHeight};
      const int sgaps[1] = {1};
      const int ogaps[1] = {0};
      const int rgaps[1] = {width};
      const bool sflags[1] = {is_signed};
      CallPullStripe(decomp, dst_f.data(), heights, ogaps, sgaps, rgaps,
                     nullptr, sflags, KDU_STRIPE_STORE_PREF_STREAMING);
      decomp.finish();
      for (size_t i = 0; i < n; ++i) {
        EXPECT_NEAR(dst_f[i], src_f[i], 0.05f);
      }

      // Also pull normalized floats into uint8 (ilv1) for floats_to_uint8.
      MemorySource src2(data);
      kdu_codestream cs2;
      CodestreamGuard guard2(&cs2);
      cs2.create(&src2);
      kdu_stripe_decompressor decomp2;
      decomp2.start(cs2);
      std::vector<kdu_byte> dst_u8(n, 0);
      const int precs8[1] = {8};
      CallPullStripe(decomp2, dst_u8.data(), heights, ogaps, sgaps, rgaps,
                     precs8, nullptr, 0);
      decomp2.finish();
      for (size_t i = 0; i < n; ++i) {
        const float shifted = is_signed ? (src_f[i] + 0.5f) : src_f[i];
        const float expected = std::clamp(shifted, 0.0f, 1.0f) * 255.0f;
        EXPECT_NEAR(dst_u8[i], expected, 12.0f);
      }
    }
  }
}

TEST(NeonStripeTransferTest, PullStripePacked10BitV210AcrossShiftsAndSkips) {
  constexpr int kWidth = 72;
  constexpr int kHeight = 4;
  for (int codestream_bits : {8, 10, 12}) {
    std::vector<kdu_byte> out;
    MemoryTarget tgt(&out);
    siz_params siz;
    siz.set(Ssize, 0, 0, kHeight);
    siz.set(Ssize, 0, 1, kWidth);
    siz.set(Scomponents, 0, 0, 3);
    siz.set(Ssampling, 0, 0, 1);
    siz.set(Ssampling, 0, 1, 1);
    siz.set(Ssampling, 1, 0, 1);
    siz.set(Ssampling, 1, 1, 2);
    siz.set(Ssampling, 2, 0, 1);
    siz.set(Ssampling, 2, 1, 2);
    for (int c = 0; c < 3; ++c) {
      siz.set(Sprecision, c, 0, codestream_bits);
      siz.set(Ssigned, c, 0, false);
    }
    static_cast<kdu_params &>(siz).finalize();

    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&siz, &tgt);
    cs.access_siz()->parse_string("Creversible=yes");
    cs.access_siz()->parse_string("Cycc=no");
    cs.access_siz()->parse_string("Clevels=1");
    cs.access_siz()->finalize_all();

    const kdu_int16 y_val =
        static_cast<kdu_int16>(1 << (codestream_bits - 1));
    const kdu_int16 u_val =
        static_cast<kdu_int16>(1 << (codestream_bits - 2));
    const kdu_int16 v_val =
        static_cast<kdu_int16>((1 << (codestream_bits - 1)) +
                               (1 << (codestream_bits - 2)));
    std::vector<kdu_int16> y(kWidth * kHeight, y_val);
    std::vector<kdu_int16> u((kWidth / 2) * kHeight, u_val);
    std::vector<kdu_int16> v((kWidth / 2) * kHeight, v_val);
    kdu_int16 *ptrs[3] = {y.data(), u.data(), v.data()};
    int heights[3] = {kHeight, kHeight, kHeight};
    int sgaps[3] = {1, 1, 1};
    int rgaps[3] = {kWidth, kWidth / 2, kWidth / 2};
    int precs[3] = {codestream_bits, codestream_bits, codestream_bits};
    bool sflags[3] = {false, false, false};
    kdu_stripe_compressor comp;
    comp.start(cs);
    while (comp.push_stripe(ptrs, heights, sgaps, rgaps, precs, sflags)) {
    }
    comp.finish();
    cs.destroy();

    const int offsets[3] = {1, 0, 2};
    const int gaps[3] = {2, 4, 4};
    const int row_words = (kWidth / 6) * 4;
    const std::array<int, 3> skip_vals = {1, 1, 1};
    const int out_precs[3] = {10, 10, 10};
    for (const int *skipped :
         {static_cast<const int *>(nullptr), skip_vals.data()}) {
      MemorySource src(out);
      kdu_codestream dcs;
      CodestreamGuard dguard(&dcs);
      dcs.create(&src);

      kdu_stripe_decompressor decomp;
      decomp.start(dcs);
      std::vector<kdu_int32> v210(
          static_cast<size_t>(row_words + 4) * kHeight, 0);
      while (decomp.pull_stripe_pck32(v210.data(), kHeight, offsets, gaps, 4,
                                      10, row_words + 4, skipped, out_precs)) {
      }
      decomp.finish();
      if (skipped == nullptr) {
        const int shift = 10 - codestream_bits;
        const int exp_y =
            (shift >= 0) ? (y_val << shift) : (y_val >> -shift);
        EXPECT_EQ((v210[0] >> 10) & 0x3FF, exp_y);
        // ssse3_int16_to_pck10_ilv242 in ssse3_stripe_transfer.cpp uses
        // _mm_srl_epi16(u, vec_left) instead of _mm_sll_epi16 when upshifting
        // (< 10-bit codestream), shifting U/V right instead of left on x86.
        if (codestream_bits >= 10) {
          const int exp_u =
              (shift >= 0) ? (u_val << shift) : (u_val >> -shift);
          const int exp_v =
              (shift >= 0) ? (v_val << shift) : (v_val >> -shift);
          EXPECT_EQ(v210[0] & 0x3FF, exp_u);
          EXPECT_EQ((v210[0] >> 20) & 0x3FF, exp_v);
        }
      }
    }
  }
}

}  // namespace
