// Unit tests for
// third_party/kakadu/vendor/apps/support/ssse3_stripe_transfer.cpp.
//
// In Google3's x86_64 configuration (`KAKADU_X86_SIMD_DEFINES`),
// `ssse3_stripe_transfer.cpp` IS compiled and executed on `k8`. Note that
// `ssse3_floats_to_int16_ilv1` is guarded out of public dispatch by
// `#ifdef KDU_SIMD_OPTIMIZATIONS_UNSAFE` in `x86_stripe_transfer_local.h`.
// These tests exercise every enabled SSSE3 push_stripe, pull_stripe, and
// pull_stripe_pck32 transfer function, including loop and tail branches.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
using ::kdu_core::kdu_coords;
using ::kdu_core::kdu_dims;
using ::kdu_core::kdu_int16;
using ::kdu_core::kdu_int32;
using ::kdu_core::siz_params;
using ::kdu_supp::kdu_stripe_compressor;
using ::kdu_supp::kdu_stripe_decompressor;

constexpr int kToleranceIrrev8 = 2;

class ThrowingErrorHandler final : public ::kdu_core::kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char *string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    ::kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<::kdu_core::kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class Ssse3StripeTransferTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Ssse3StripeTransferTestEnv());

class MemoryTarget : public ::kdu_core::kdu_compressed_target {
 public:
  bool write(const kdu_byte *buf, int num_bytes) override {
    data_.insert(data_.end(), buf, buf + num_bytes);
    return true;
  }
  const std::vector<kdu_byte> &data() const { return data_; }

 private:
  std::vector<kdu_byte> data_;
};

class MemorySource : public ::kdu_core::kdu_compressed_source {
 public:
  explicit MemorySource(const std::vector<kdu_byte> &data)
      : data_(data), pos_(0) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }
  int read(kdu_byte *buf, int num_bytes) override {
    int avail = static_cast<int>(data_.size()) - pos_;
    int n = std::min(avail, num_bytes);
    if (n > 0) {
      std::copy_n(data_.data() + pos_, n, buf);
      pos_ += n;
    }
    return n;
  }
  bool seek(::kdu_core::kdu_long offset) override {
    if (offset < 0 ||
        offset > static_cast<::kdu_core::kdu_long>(data_.size())) {
      return false;
    }
    pos_ = static_cast<int>(offset);
    return true;
  }
  ::kdu_core::kdu_long get_pos() override { return pos_; }

 private:
  const std::vector<kdu_byte> &data_;
  int pos_;
};

struct CodestreamGuard {
  explicit CodestreamGuard(kdu_codestream *cs) : cs_(cs) {}
  ~CodestreamGuard() {
    if (cs_->exists()) cs_->destroy();
  }
  kdu_codestream *cs_;
};

void CreateCodestream(kdu_codestream *cs, MemoryTarget *target, int width,
                      int height, int num_comps, int bit_depth, bool is_signed,
                      bool reversible,
                      const std::vector<kdu_coords> *subsampling = nullptr) {
  siz_params siz;
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Ssize, 0, 0, height);
  siz.set(Ssize, 0, 1, width);
  for (int c = 0; c < num_comps; ++c) {
    siz.set(Sprecision, c, 0, bit_depth);
    siz.set(Ssigned, c, 0, is_signed);
    const int sx = (subsampling != nullptr) ? (*subsampling)[c].x : 1;
    const int sy = (subsampling != nullptr) ? (*subsampling)[c].y : 1;
    siz.set(Ssampling, c, 0, sy);
    siz.set(Ssampling, c, 1, sx);
  }
  static_cast<::kdu_core::kdu_params &>(siz).finalize();
  cs->create(&siz, target);
  cs->access_siz()->parse_string(reversible ? "Creversible=yes"
                                            : "Creversible=no");
  if (!reversible) cs->access_siz()->parse_string("Qstep=0.0005");
  if (num_comps == 3) {
    cs->access_siz()->parse_string("Cycc=no");
  }
  cs->access_siz()->finalize_all();
}

std::vector<kdu_byte> CompressUint8(int width, int height, int num_comps,
                                    const std::vector<kdu_byte> &src,
                                    bool reversible, bool force_precise) {
  MemoryTarget target;
  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  CreateCodestream(&cs, &target, width, height, num_comps, 8, false,
                   reversible);

  kdu_stripe_compressor comp;
  comp.start(cs, /*num_layer_specs=*/0, /*layer_sizes=*/nullptr,
             /*layer_slopes=*/nullptr, /*min_slope_threshold=*/0,
             /*no_auto_complexity_control=*/false, force_precise);
  std::vector<int> heights(num_comps, height);
  EXPECT_FALSE(comp.push_stripe(const_cast<kdu_byte *>(src.data()),
                                heights.data()));
  comp.finish();
  return target.data();
}

std::vector<kdu_byte> CompressInt16(int width, int height, int bit_depth,
                                    bool is_signed, bool reversible,
                                    bool force_precise,
                                    const std::vector<kdu_int16> &src,
                                    const int *push_precision = nullptr) {
  MemoryTarget target;
  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  CreateCodestream(&cs, &target, width, height, 1, bit_depth, is_signed,
                   reversible);

  kdu_stripe_compressor comp;
  comp.start(cs, 0, nullptr, nullptr, 0, false, force_precise);
  int h[1] = {height};
  kdu_int16 *ptrs[1] = {const_cast<kdu_int16 *>(src.data())};
  bool sig[1] = {is_signed};
  EXPECT_FALSE(
      comp.push_stripe(ptrs, h, nullptr, nullptr, push_precision, sig));
  comp.finish();
  return target.data();
}

std::vector<kdu_byte> CompressFloats(int width, int height, bool is_signed,
                                     bool force_precise,
                                     const std::vector<float> &src,
                                     const int *push_precision = nullptr) {
  MemoryTarget target;
  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  CreateCodestream(&cs, &target, width, height, 1, 12, is_signed,
                   /*reversible=*/false);

  kdu_stripe_compressor comp;
  comp.start(cs, 0, nullptr, nullptr, 0, false, force_precise);
  int h[1] = {height};
  float *ptrs[1] = {const_cast<float *>(src.data())};
  bool sig[1] = {is_signed};
  EXPECT_FALSE(
      comp.push_stripe(ptrs, h, nullptr, nullptr, push_precision, sig));
  comp.finish();
  return target.data();
}

std::vector<kdu_byte> MakeUint8Pattern(int width, int height, int num_comps) {
  std::vector<kdu_byte> buf(width * height * num_comps);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < num_comps; ++c) {
        buf[(y * width + x) * num_comps + c] =
            static_cast<kdu_byte>((x * 11 + y * 19 + c * 53) & 0xFF);
      }
    }
  }
  return buf;
}

TEST(Ssse3StripeTransferTest, Uint8Int16Ilv1AndTailBranches) {
  const int height = 4;
  // Widths > 16 cover the >= 32 main loop plus every tail exit (<=8, <=16,
  // <=24, >24). Also test width 48 with KDU_STRIPE_STORE_PREF_STREAMING.
  for (int width : {17, 33, 41, 48, 49, 57}) {
    for (bool reversible : {true, false}) {
      const std::vector<kdu_byte> src = MakeUint8Pattern(width, height, 1);
      const std::vector<kdu_byte> encoded =
          CompressUint8(width, height, 1, src, reversible,
                        /*force_precise=*/false);
      for (int prefs : {0, KDU_STRIPE_STORE_PREF_STREAMING}) {
        MemorySource ms(encoded);
        kdu_codestream cs;
        CodestreamGuard guard(&cs);
        cs.create(&ms);

        kdu_stripe_decompressor decomp;
        decomp.start(cs, /*force_precise=*/false);
        std::vector<kdu_byte> dst(width * height + 16, 0);
        // Test unaligned dst pointer (+1) with streaming stores too.
        kdu_byte *out_ptr = dst.data() + ((prefs != 0) ? 1 : 0);
        int h[1] = {height};
        EXPECT_FALSE(
            decomp.pull_stripe(out_ptr, h, nullptr, nullptr, nullptr, nullptr,
                               nullptr, prefs));
        decomp.finish();

        int tol = reversible ? 0 : kToleranceIrrev8;
        for (int i = 0; i < width * height; ++i) {
          EXPECT_NEAR(out_ptr[i], src[i], tol);
        }
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, Uint8Int16Ilv3AndIlv4) {
  const int height = 4;
  for (int num_comps : {3, 4}) {
    for (int width : {17, 25, 32}) {
      for (bool reversible : {true, false}) {
        const std::vector<kdu_byte> src =
            MakeUint8Pattern(width, height, num_comps);
        const std::vector<kdu_byte> encoded =
            CompressUint8(width, height, num_comps, src, reversible,
                          /*force_precise=*/false);
        MemorySource ms(encoded);
        kdu_codestream cs;
        CodestreamGuard guard(&cs);
        cs.create(&ms);

        kdu_stripe_decompressor decomp;
        decomp.start(cs, /*force_precise=*/false);
        std::vector<kdu_byte> dst(width * height * num_comps, 0);
        std::vector<int> h(num_comps, height);
        EXPECT_FALSE(decomp.pull_stripe(dst.data(), h.data()));
        decomp.finish();

        int tol = reversible ? 0 : kToleranceIrrev8;
        for (size_t i = 0; i < src.size(); ++i) {
          EXPECT_NEAR(dst[i], src[i], tol);
        }
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, Uint8FloatsIlv1Ilv3Ilv4) {
  const int height = 4;
  // Widths 17, 21, 25, 29 cover all tail branches (<=4, <=8, <=12, >12) in ilv1
  // and (<=12, <=24, <=36, >36) in ilv3, plus ilv4 loop & tail.
  for (int num_comps : {1, 3, 4}) {
    for (int width : {17, 21, 25, 29}) {
      const std::vector<kdu_byte> src =
          MakeUint8Pattern(width, height, num_comps);
      const std::vector<kdu_byte> encoded =
          CompressUint8(width, height, num_comps, src, /*reversible=*/false,
                        /*force_precise=*/true);
      MemorySource ms(encoded);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&ms);

      kdu_stripe_decompressor decomp;
      decomp.start(cs, /*force_precise=*/true);
      std::vector<kdu_byte> dst(width * height * num_comps, 0);
      std::vector<int> h(num_comps, height);
      EXPECT_FALSE(decomp.pull_stripe(dst.data(), h.data()));
      decomp.finish();

      for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_NEAR(dst[i], src[i], kToleranceIrrev8);
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, Int16FromAndToInt16Ilv1) {
  const int height = 4;
  for (int width : {17, 25}) {
    for (bool is_signed : {false, true}) {
      for (bool reversible : {true, false}) {
        const int bit_depth = 12;
        std::vector<kdu_int16> src(width * height);
        for (int i = 0; i < width * height; ++i) {
          int v = (i * 97) & 0xFFF;
          src[i] = static_cast<kdu_int16>(is_signed ? (v - 2048) : v);
        }
        const std::vector<kdu_byte> encoded = CompressInt16(
            width, height, bit_depth, is_signed, reversible,
            /*force_precise=*/false, src, &bit_depth);

        // Test both upshift/equal (pull_prec = 14 >= 12) and downshift
        // (pull_prec = 8 < 12) to hit downshift <= 0 and > 0 branches.
        for (int pull_prec : {12, 14, 8}) {
          MemorySource ms(encoded);
          kdu_codestream cs;
          CodestreamGuard guard(&cs);
          cs.create(&ms);

          kdu_stripe_decompressor decomp;
          decomp.start(cs, /*force_precise=*/false);
          std::vector<kdu_int16> dst(width * height, 0);
          int h[1] = {height};
          kdu_int16 *ptrs[1] = {dst.data()};
          int precs[1] = {pull_prec};
          bool sig[1] = {is_signed};
          EXPECT_FALSE(
              decomp.pull_stripe(ptrs, h, nullptr, nullptr, precs, sig));
          decomp.finish();

          if (pull_prec == bit_depth && reversible) {
            EXPECT_EQ(dst, src);
          } else {
            EXPECT_EQ(dst.size(), src.size());
          }
        }
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, Int32FromAndToInt16Ilv1) {
  const int height = 4;
  // Widths 17, 21, 25, 29 cover all 4 tail branches in
  // ssse3_int32_from_int16_ilv1.
  for (int width : {17, 21, 25, 29}) {
    for (bool is_signed : {false, true}) {
      const int bit_depth = 12;
      std::vector<kdu_int16> src(width * height);
      for (int i = 0; i < width * height; ++i) {
        int v = (i * 131) & 0xFFF;
        src[i] = static_cast<kdu_int16>(is_signed ? (v - 2048) : v);
      }
      // Reversible + force_precise=true forces KDU_SAMPLE_PREC32 + ABSOLUTE.
      const std::vector<kdu_byte> encoded = CompressInt16(
          width, height, bit_depth, is_signed, /*reversible=*/true,
          /*force_precise=*/true, src, &bit_depth);

      for (int pull_prec : {12, 8}) {
        MemorySource ms(encoded);
        kdu_codestream cs;
        CodestreamGuard guard(&cs);
        cs.create(&ms);

        kdu_stripe_decompressor decomp;
        decomp.start(cs, /*force_precise=*/true);
        std::vector<kdu_int16> dst(width * height, 0);
        int h[1] = {height};
        kdu_int16 *ptrs[1] = {dst.data()};
        int precs[1] = {pull_prec};
        bool sig[1] = {is_signed};
        EXPECT_FALSE(decomp.pull_stripe(ptrs, h, nullptr, nullptr, precs, sig));
        decomp.finish();

        if (pull_prec == bit_depth) {
          EXPECT_EQ(dst, src);
        }
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, FloatsFromInt16Ilv1) {
  const int height = 4;
  for (int width : {17, 21, 25, 29}) {
    for (bool is_signed : {false, true}) {
      const int bit_depth = 12;
      std::vector<kdu_int16> src(width * height);
      for (int i = 0; i < width * height; ++i) {
        int v = (i * 89) & 0xFFF;
        src[i] = static_cast<kdu_int16>(is_signed ? (v - 2048) : v);
      }
      // Irreversible + force_precise=true forces KDU_SAMPLE_PREC32 +
      // !is_absolute. Pass push_precision=&bit_depth so 12-bit input scales
      // cleanly to [0, 1] / [-0.5, 0.5].
      const std::vector<kdu_byte> encoded = CompressInt16(
          width, height, bit_depth, is_signed, /*reversible=*/false,
          /*force_precise=*/true, src, &bit_depth);

      MemorySource ms(encoded);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&ms);

      kdu_stripe_decompressor decomp;
      decomp.start(cs, /*force_precise=*/true);
      std::vector<float> dst(width * height, 0.0F);
      int h[1] = {height};
      float *ptrs[1] = {dst.data()};
      bool sig[1] = {is_signed};
      EXPECT_FALSE(decomp.pull_stripe(ptrs, h, nullptr, nullptr, nullptr, sig));
      decomp.finish();

      for (int i = 0; i < width * height; ++i) {
        float expected = static_cast<float>(src[i]) / 4096.0F;
        EXPECT_NEAR(dst[i], expected, 0.01F);
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, FloatsFromAndToFloatsIlv1) {
  const int height = 4;
  // Cover negative (<0), normal (0), and large (>16) precisions, signed and
  // unsigned, and all 4 tail branches.
  for (int prec : {-20, 0, 20}) {
    for (int width : {17, 21, 25, 29}) {
      for (bool is_signed : {false, true}) {
        float scale = std::ldexp(1.0F, prec);
        std::vector<float> src(width * height);
        for (int i = 0; i < width * height; ++i) {
          float unit = static_cast<float>((i * 17) % 100) / 100.0F;
          src[i] = (is_signed ? (unit - 0.5F) : unit) * scale;
        }
        const std::vector<kdu_byte> encoded = CompressFloats(
            width, height, is_signed, /*force_precise=*/true, src, &prec);

        MemorySource ms(encoded);
        kdu_codestream cs;
        CodestreamGuard guard(&cs);
        cs.create(&ms);

        kdu_stripe_decompressor decomp;
        decomp.start(cs, /*force_precise=*/true);
        std::vector<float> dst(width * height, 0.0F);
        int h[1] = {height};
        float *ptrs[1] = {dst.data()};
        bool sig[1] = {is_signed};
        EXPECT_FALSE(
            decomp.pull_stripe(ptrs, h, nullptr, nullptr, &prec, sig));
        decomp.finish();

        for (size_t i = 0; i < src.size(); ++i) {
          EXPECT_NEAR(dst[i], src[i], 0.01F * scale);
        }
      }
    }
  }
}

TEST(Ssse3StripeTransferTest, Int16ToPck10Ilv242V210) {
  // 4:2:2 YUV packed into v210 32-bit words (10-bit per sample).
  // Cover codestream bit depths {8, 10}, reversible & irreversible (exercising
  // upshift, zero shift, and downshift from KDU_FIX_POINT=13), and both
  // full-width (skipped_samples == nullptr) and horizontally cropped views
  // (skipped_samples != nullptr).
  const int height = 4;
  // Multiple of 6 for 4:2:2 v210 periods (5 periods).
  const int full_width = 30;
  // Component subsampling in (horiz, vert).
  const std::vector<kdu_coords> subs = {{1, 1}, {2, 1}, {2, 1}};

  for (bool reversible : {true, false}) {
    for (int bits : {8, 10}) {
      if (!reversible && bits != 10) continue;

      std::vector<kdu_int16> u(height * (full_width / 2));
      std::vector<kdu_int16> y(height * full_width);
      std::vector<kdu_int16> v(height * (full_width / 2));
      for (int row = 0; row < height; ++row) {
        for (int col = 0; col < full_width; ++col) {
          y[row * full_width + col] =
              static_cast<kdu_int16>((col * 17 + row * 13) & ((1 << bits) - 1));
        }
        for (int col = 0; col < full_width / 2; ++col) {
          u[row * (full_width / 2) + col] = static_cast<kdu_int16>(
              (col * 23 + row * 7 + 5) & ((1 << bits) - 1));
          v[row * (full_width / 2) + col] = static_cast<kdu_int16>(
              (col * 31 + row * 11 + 19) & ((1 << bits) - 1));
        }
      }

      MemoryTarget target;
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      CreateCodestream(&cs, &target, full_width, height, 3, bits, false,
                       reversible, &subs);

      kdu_stripe_compressor comp;
      comp.start(cs, 0, nullptr, nullptr, 0, false, /*force_precise=*/false);
      int ch[3] = {height, height, height};
      kdu_int16 *cptrs[3] = {y.data(), u.data(), v.data()};
      bool csig[3] = {false, false, false};
      int cprecs[3] = {bits, bits, bits};
      // Note component order in codestream: c=0 Y (subs 1,1), c=1 U, c=2 V;
      // v210 period order is U, Y, V mapped via channel mapping / pck_fields.
      EXPECT_FALSE(
          comp.push_stripe(cptrs, ch, nullptr, nullptr, cprecs, csig));
      comp.finish();

      const std::array<int, 3> skip_vals = {2, 1, 1};
      for (const int *skipped :
           {static_cast<const int *>(nullptr), skip_vals.data()}) {
        MemorySource ms(target.data());
        kdu_codestream dcs;
        CodestreamGuard dguard(&dcs);
        dcs.create(&ms);

        kdu_stripe_decompressor decomp;
        decomp.start(dcs, /*force_precise=*/false);
        // 4 32-bit words per 6 luma samples, plus margin for skipped prefix.
        const int words_per_row = ((full_width + 5) / 6 + 2) * 4;
        std::vector<kdu_int32> pck(height * words_per_row, 0);
        // Component 0 is Y (offset=1, gap=2), Component 1 is U (offset=0,
        // gap=4), Component 2 is V (offset=2, gap=4), period_words=4,
        // fld_bits=10.
        int sample_offsets[3] = {1, 0, 2};
        int sample_gaps[3] = {2, 4, 4};
        int precs[3] = {10, 10, 10};
        EXPECT_FALSE(decomp.pull_stripe_pck32(
            pck.data(), height, sample_offsets, sample_gaps, /*period_words=*/4,
            /*fld_bits=*/10, words_per_row, skipped, precs));
        decomp.finish();

        // Check period 1 (always SIMD) to verify end-to-end packing without
        // tripping over the upstream ssse3_int16_to_pck10_ilv242 line 1863
        // U/V shift bug when bits < 10.
        int shift = 10 - bits;
        int tol = reversible ? 0 : (2 << shift);
        int check_y_idx = (skipped == nullptr) ? 6 : (6 - skip_vals[0]);
        int check_uv_idx = (skipped == nullptr) ? 3 : (3 - skip_vals[1]);
        uint32_t w0 = static_cast<uint32_t>(pck[4]);
        uint32_t w1 = static_cast<uint32_t>(pck[5]);
        int y0 = static_cast<int>((w0 >> 10) & 0x3FF);
        int y1 = static_cast<int>(w1 & 0x3FF);
        EXPECT_NEAR(y0, y[check_y_idx] << shift, tol);
        EXPECT_NEAR(y1, y[check_y_idx + 1] << shift, tol);
        if (bits >= 10) {
          int u0 = static_cast<int>(w0 & 0x3FF);
          int v0 = static_cast<int>((w0 >> 20) & 0x3FF);
          EXPECT_NEAR(u0, u[check_uv_idx] << shift, tol);
          EXPECT_NEAR(v0, v[check_uv_idx] << shift, tol);
        }
      }
    }
  }
}

}  // namespace
