// Unit tests for
// third_party/kakadu/vendor/apps/support/avx2_stripe_transfer.cpp.
//
// In Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// sets `KAKADU_X86_SIMD_DEFINES` with `-DKDU_NO_AVX2` on x86_64),
// `avx2_stripe_transfer.cpp` is compiled out (`0/0` LCOV `DA:` ceiling on both
// x86_64 and `--cpu=arm`). These tests exercise `kdu_stripe_compressor` and
// `kdu_stripe_decompressor` across 8-bit, 16-bit, 32-bit, float, and packed
// 10-bit (v210 / r210) transfers, ilv1/ilv3/ilv4 organizations, widths > 64,
// and streaming vs. non-streaming stores.

#include <algorithm>
#include <array>
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

class Avx2StripeTransferTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Avx2StripeTransferTestEnv());

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
std::vector<kdu_byte> CompressInterleaved(int width, int height, int num_comps,
                                          int bit_depth, bool is_signed,
                                          bool reversible,
                                          const std::vector<SampleT> &samples,
                                          bool is_normalized_float = true) {
  std::vector<kdu_byte> out;
  MemoryTarget tgt(&out);
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, is_signed);
  static_cast<kdu_params &>(siz).finalize();

  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  cs.create(&siz, &tgt);
  cs.access_siz()->parse_string(reversible ? "Creversible=yes"
                                           : "Creversible=no");
  cs.access_siz()->parse_string("Cycc=no");
  cs.access_siz()->parse_string("Clevels=2");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(num_comps, height);
  std::vector<int> precisions(num_comps, bit_depth);
  // kdu_stripe_compressor takes bool* rather than vector<bool>.
  auto sflags = std::make_unique<bool[]>(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    sflags[c] = is_signed;
  }
  if constexpr (std::is_same_v<SampleT, kdu_byte>) {
    while (comp.push_stripe(const_cast<kdu_byte *>(samples.data()),
                            heights.data(), nullptr, nullptr, nullptr,
                            precisions.data())) {
    }
  } else if constexpr (std::is_same_v<SampleT, float>) {
    while (comp.push_stripe(const_cast<float *>(samples.data()),
                            heights.data(), nullptr, nullptr, nullptr,
                            is_normalized_float ? nullptr : precisions.data(),
                            sflags.get())) {
    }
  } else {
    while (comp.push_stripe(const_cast<SampleT *>(samples.data()),
                            heights.data(), nullptr, nullptr, nullptr,
                            precisions.data(), sflags.get())) {
    }
  }
  comp.finish();
  return out;
}

TEST(Avx2StripeTransferTest, PushAndPullUint8Ilv1Ilv3Ilv4) {
  constexpr int kHeight = 6;
  for (bool reversible : {true, false}) {
    for (int width : {73, 135}) {
      for (int num_comps : {1, 3, 4}) {
        const size_t n = static_cast<size_t>(width) * kHeight * num_comps;
        std::vector<kdu_byte> src_pixels(n);
        for (size_t i = 0; i < n; ++i) {
          src_pixels[i] = static_cast<kdu_byte>((i * 13 + 7) & 0xFF);
        }
        const std::vector<kdu_byte> data = CompressInterleaved(
            width, kHeight, num_comps, 8, false, reversible, src_pixels);

        for (int pref : {0, KDU_STRIPE_STORE_PREF_STREAMING}) {
          for (int offset : {0, 1}) {
            MemorySource src(data);
            kdu_codestream cs;
            CodestreamGuard guard(&cs);
            cs.create(&src);

            kdu_stripe_decompressor decomp;
            decomp.start(cs);
            // Allocate extra padding so an aligned sub-offset is available.
            std::vector<kdu_byte> out(n + 80, 0);
            const uintptr_t base = reinterpret_cast<uintptr_t>(out.data());
            const size_t align_pad = (64 - (base & 63)) & 63;
            kdu_byte *dst_ptr = out.data() + align_pad + offset;
            std::vector<int> heights(num_comps, kHeight);
            while (decomp.pull_stripe(dst_ptr, heights.data(), nullptr, nullptr,
                                      nullptr, nullptr, nullptr, pref)) {
            }
            decomp.finish();
            const int tol = reversible ? 0 : 12;
            for (size_t i = 0; i < n; ++i) {
              EXPECT_NEAR(dst_ptr[i], src_pixels[i], tol);
            }
          }
        }
      }
    }
  }
}

TEST(Avx2StripeTransferTest, PushAndPullInt16AndInt32SignedUnsigned) {
  constexpr int kWidth = 73;
  constexpr int kHeight = 6;
  for (bool is_signed : {false, true}) {
    for (bool reversible : {true, false}) {
      for (int num_comps : {1, 3, 4}) {
        const size_t n = static_cast<size_t>(kWidth) * kHeight * num_comps;
        // 12-bit int16 round-trip.
        {
          std::vector<kdu_int16> src16(n);
          for (size_t i = 0; i < n; ++i) {
            const int v = static_cast<int>((i * 19) & 0xFFF);
            src16[i] = static_cast<kdu_int16>(is_signed ? v - 2048 : v);
          }
          const std::vector<kdu_byte> data = CompressInterleaved(
              kWidth, kHeight, num_comps, 12, is_signed, reversible, src16);

          for (int pref : {0, KDU_STRIPE_STORE_PREF_STREAMING}) {
            MemorySource src(data);
            kdu_codestream cs;
            CodestreamGuard guard(&cs);
            cs.create(&src);

            kdu_stripe_decompressor decomp;
            decomp.start(cs);
            std::vector<kdu_int16> dst16(n, 0);
            std::vector<int> heights(num_comps, kHeight);
            std::vector<int> precs(num_comps, 12);
            auto sflags = std::make_unique<bool[]>(num_comps);
            for (int c = 0; c < num_comps; ++c) {
              sflags[c] = is_signed;
            }
            while (decomp.pull_stripe(dst16.data(), heights.data(), nullptr,
                                      nullptr, nullptr, precs.data(),
                                      sflags.get(), nullptr, pref)) {
            }
            decomp.finish();
            const int tol = reversible ? 0 : 96;
            for (size_t i = 0; i < n; ++i) {
              EXPECT_NEAR(dst16[i], src16[i], tol);
            }
          }
        }

        // 10-bit in int32 round-trip (uses 16-bit internal line buffers).
        {
          std::vector<kdu_int32> src32(n);
          for (size_t i = 0; i < n; ++i) {
            const int v = static_cast<int>((i * 37) & 0x3FF);
            src32[i] = is_signed ? v - 512 : v;
          }
          const std::vector<kdu_byte> data = CompressInterleaved(
              kWidth, kHeight, num_comps, 10, is_signed, reversible, src32);

          MemorySource src(data);
          kdu_codestream cs;
          CodestreamGuard guard(&cs);
          cs.create(&src);

          kdu_stripe_decompressor decomp;
          decomp.start(cs, /*force_precise=*/false, /*want_fastest=*/true);
          std::vector<kdu_int32> dst32(n, 0);
          std::vector<int> heights(num_comps, kHeight);
          std::vector<int> precs(num_comps, 10);
          auto sflags = std::make_unique<bool[]>(num_comps);
          for (int c = 0; c < num_comps; ++c) {
            sflags[c] = is_signed;
          }
          while (decomp.pull_stripe(dst32.data(), heights.data(), nullptr,
                                    nullptr, nullptr, precs.data(),
                                    sflags.get(), nullptr,
                                    KDU_STRIPE_STORE_PREF_STREAMING)) {
          }
          decomp.finish();
          const int tol = reversible ? 0 : 24;
          for (size_t i = 0; i < n; ++i) {
            EXPECT_NEAR(dst32[i], src32[i], tol);
          }
        }

        // 16-bit reversible in int32 round-trip (32-bit internal line buffers).
        if (reversible) {
          std::vector<kdu_int32> src32(n);
          for (size_t i = 0; i < n; ++i) {
            const int v = static_cast<int>((i * 257) & 0xFFFF);
            src32[i] = is_signed ? v - 32768 : v;
          }
          const std::vector<kdu_byte> data = CompressInterleaved(
              kWidth, kHeight, num_comps, 16, is_signed, true, src32);

          MemorySource src(data);
          kdu_codestream cs;
          CodestreamGuard guard(&cs);
          cs.create(&src);

          kdu_stripe_decompressor decomp;
          decomp.start(cs, /*force_precise=*/false, /*want_fastest=*/true);
          std::vector<kdu_int32> dst32(n, 0);
          std::vector<int> heights(num_comps, kHeight);
          // Scalar transfer_dwords fallback omits dst_bits == original_bits
          // for reversible 32-bit line buffers, so shift up 1 bit.
          std::vector<int> precs(num_comps, 17);
          auto sflags = std::make_unique<bool[]>(num_comps);
          for (int c = 0; c < num_comps; ++c) {
            sflags[c] = is_signed;
          }
          while (decomp.pull_stripe(dst32.data(), heights.data(), nullptr,
                                    nullptr, nullptr, precs.data(),
                                    sflags.get(), nullptr,
                                    KDU_STRIPE_STORE_PREF_STREAMING)) {
          }
          decomp.finish();
          for (size_t i = 0; i < n; ++i) {
            EXPECT_EQ(dst32[i], src32[i] * 2);
          }
        }
      }
    }
  }
}

TEST(Avx2StripeTransferTest, PushAndPullFloatsAcrossOutputs) {
  constexpr int kWidth = 73;
  constexpr int kHeight = 6;
  for (int num_comps : {1, 3, 4}) {
    const size_t n = static_cast<size_t>(kWidth) * kHeight * num_comps;
    std::vector<float> src_f(n);
    for (size_t i = 0; i < n; ++i) {
      src_f[i] = static_cast<float>(i % 100) / 100.0f;
    }
    const std::vector<kdu_byte> data = CompressInterleaved(
        kWidth, kHeight, num_comps, 8, false, /*reversible=*/false, src_f,
        /*is_normalized_float=*/true);

    // Pull to float (normalized) with streaming and default stores.
    auto sflags = std::make_unique<bool[]>(num_comps);
    for (int c = 0; c < num_comps; ++c) {
      sflags[c] = false;
    }
    for (int pref : {0, KDU_STRIPE_STORE_PREF_STREAMING}) {
      MemorySource src(data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);
      kdu_stripe_decompressor decomp;
      decomp.start(cs);
      std::vector<float> dst_f(n, 0.0f);
      std::vector<int> heights(num_comps, kHeight);
      while (decomp.pull_stripe(dst_f.data(), heights.data(), nullptr, nullptr,
                                nullptr, nullptr, sflags.get(), nullptr,
                                pref)) {
      }
      decomp.finish();
      for (size_t i = 0; i < n; ++i) {
        EXPECT_NEAR(dst_f[i], src_f[i], 0.06f);
      }
    }

    // Pull same float-compressed codestream to uint8 bytes.
    {
      MemorySource src(data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);
      kdu_stripe_decompressor decomp;
      decomp.start(cs);
      std::vector<kdu_byte> dst_u8(n, 0);
      std::vector<int> heights(num_comps, kHeight);
      while (decomp.pull_stripe(dst_u8.data(), heights.data())) {
      }
      decomp.finish();
      for (size_t i = 0; i < n; ++i) {
        const float expected = src_f[i] * 255.0f;
        EXPECT_NEAR(dst_u8[i], expected, 16.0f);
      }
    }
  }
}

TEST(Avx2StripeTransferTest, PullStripePacked10BitV210AndR210) {
  // 1. v210 4:2:2 YUV (Y=comp 0 at 72x4, U/V=comps 1,2 at 36x4).
  {
    constexpr int kWidth = 72;
    constexpr int kHeight = 4;
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
    siz.set(Sprecision, 0, 0, 10);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params &>(siz).finalize();

    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&siz, &tgt);
    cs.access_siz()->parse_string("Creversible=yes");
    cs.access_siz()->parse_string("Cycc=no");
    cs.access_siz()->parse_string("Clevels=1");
    cs.access_siz()->finalize_all();

    std::vector<kdu_int16> y(kWidth * kHeight, 512);
    std::vector<kdu_int16> u((kWidth / 2) * kHeight, 256);
    std::vector<kdu_int16> v((kWidth / 2) * kHeight, 768);
    kdu_int16 *ptrs[3] = {y.data(), u.data(), v.data()};
    int heights[3] = {kHeight, kHeight, kHeight};
    int sgaps[3] = {1, 1, 1};
    int rgaps[3] = {kWidth, kWidth / 2, kWidth / 2};
    int precs[3] = {10, 10, 10};
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
    for (const int *skipped :
         {static_cast<const int *>(nullptr), skip_vals.data()}) {
      MemorySource src(out);
      kdu_codestream dcs;
      CodestreamGuard dguard(&dcs);
      dcs.create(&src);

      kdu_stripe_decompressor decomp;
      decomp.start(dcs);
      std::vector<kdu_int32> v210(static_cast<size_t>(row_words + 4) * kHeight,
                                  0);
      while (decomp.pull_stripe_pck32(v210.data(), kHeight, offsets, gaps, 4,
                                      10, row_words + 4, skipped, precs)) {
      }
      decomp.finish();
      // First word of row 0 contains U in [9:0], Y0 in [19:10], V in [29:20].
      if (skipped == nullptr) {
        EXPECT_EQ(v210[0] & 0x3FF, 256);
        EXPECT_EQ((v210[0] >> 10) & 0x3FF, 512);
        EXPECT_EQ((v210[0] >> 20) & 0x3FF, 768);
      }
    }
  }

  // 2. r210 4:4:4 RGB (width=73, height=4).
  {
    constexpr int kWidth = 73;
    constexpr int kHeight = 4;
    const size_t n = static_cast<size_t>(kWidth) * kHeight * 3;
    std::vector<kdu_int16> rgb(n);
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
      rgb[i * 3 + 0] = 600;
      rgb[i * 3 + 1] = 300;
      rgb[i * 3 + 2] = 100;
    }
    const std::vector<kdu_byte> data = CompressInterleaved(
        kWidth, kHeight, 3, 10, false, /*reversible=*/true, rgb);

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);

    kdu_stripe_decompressor decomp;
    decomp.start(cs);
    const int offsets[3] = {2, 1, 0};
    const int gaps[3] = {3, 3, 3};
    const int precs[3] = {10, 10, 10};
    std::vector<kdu_int32> r210(static_cast<size_t>(kWidth) * kHeight, 0);
    while (decomp.pull_stripe_pck32(r210.data(), kHeight, offsets, gaps, 1, 10,
                                    kWidth, nullptr, precs)) {
    }
    decomp.finish();
    EXPECT_EQ(r210[0] & 0x3FF, 100);
    EXPECT_EQ((r210[0] >> 10) & 0x3FF, 300);
    EXPECT_EQ((r210[0] >> 20) & 0x3FF, 600);
  }
}

}  // namespace
