// Unit tests for
// third_party/kakadu/vendor/coresys/transform/avx_colour_local.cpp.
//
// In Google3's active configuration, `third_party/kakadu/vendor/BUILD` leaves
// `KDU_SIMD_OPTIMIZATIONS_UNSAFE` undefined, and `08-simd_issues.patch` wraps
// `KD_SET_SIMD_FUNC_RGB_TO_YCC_IRREV32` and
// `KD_SET_SIMD_FUNC_YCC_TO_RGB_IRREV32` in `colour.cpp` inside `#ifdef
// KDU_SIMD_OPTIMIZATIONS_UNSAFE`. Consequently, although `avx_colour_local.cpp`
// compiles on x86_64 (`KDU_X86_INTRINSICS` defined, `KDU_NO_AVX` undefined),
// `avx_rgb_to_ycc_irrev32` and `avx_ycc_to_rgb_irrev32` are never installed
// into `kdu_convert_rgb_to_ycc_irrev32` / `kdu_convert_ycc_to_rgb_irrev32`
// (`0/36 = 0.00%` LCOV `DA:` patch-disabled ceiling on x86_64; compiled out
// under `KDU_X86_INTRINSICS` on `--cpu=arm`). This suite verifies the 32-bit
// floating-point irreversible ICT entrypoints and `KDU_MULTI_XFORM_PRECISE`
// codestream pipeline under Google3's safe scalar dispatch.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char* string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class AvxColourLocalTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kAvxColourEnv =
    ::testing::AddGlobalTestEnvironment(new AvxColourLocalTestEnvironment());

class VectorTarget final : public kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_byte>& out) : out_(out) {}

  bool write(const kdu_byte* buf, int num_bytes) override {
    out_.insert(out_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_byte>& out_;
};

class VectorSource final : public kdu_compressed_source {
 public:
  explicit VectorSource(const std::vector<kdu_byte>& data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0) {
      return false;
    }
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

 private:
  const std::vector<kdu_byte>& data_;
  size_t pos_ = 0;
};

struct ScopedCodestream {
  siz_params siz;
  kdu_codestream codestream;

  ~ScopedCodestream() {
    if (codestream.exists()) {
      codestream.destroy();
    }
  }
};

struct ScopedTile {
  kdu_tile tile;

  ~ScopedTile() {
    if (tile.exists()) {
      tile.close();
    }
  }
};

struct ScopedMultiAnalysis {
  kdu_multi_analysis engine;

  ~ScopedMultiAnalysis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedMultiSynthesis {
  kdu_multi_synthesis engine;

  ~ScopedMultiSynthesis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

TEST(AvxColourLocalTest, Aligned32BitFloatIrreversibleRgbYccRoundTrip) {
  ASSERT_NE(kdu_convert_rgb_to_ycc_irrev32, nullptr);
  ASSERT_NE(kdu_convert_ycc_to_rgb_irrev32, nullptr);

  // 32-byte aligned float buffers matching the 8-float AVX vector contract of
  // `avx_rgb_to_ycc_irrev32` and `avx_ycc_to_rgb_irrev32`.
  constexpr int kSamples = 32;
  alignas(32) float r[kSamples];
  alignas(32) float g[kSamples];
  alignas(32) float b[kSamples];
  float orig_r[kSamples];
  float orig_g[kSamples];
  float orig_b[kSamples];

  for (int i = 0; i < kSamples; ++i) {
    r[i] = orig_r[i] = -0.45f + 0.025f * static_cast<float>(i);
    g[i] = orig_g[i] = 0.40f - 0.022f * static_cast<float>(i);
    b[i] = orig_b[i] = -0.25f + 0.018f * static_cast<float>(i);
  }

  kdu_convert_rgb_to_ycc_irrev32(r, g, b, kSamples);
  kdu_convert_ycc_to_rgb_irrev32(r, g, b, kSamples);

  for (int i = 0; i < kSamples; ++i) {
    EXPECT_NEAR(r[i], orig_r[i], 1e-5f);
    EXPECT_NEAR(g[i], orig_g[i], 1e-5f);
    EXPECT_NEAR(b[i], orig_b[i], 1e-5f);
  }
}

TEST(AvxColourLocalTest, MultiTransform32BitIrreversibleIctCodestream) {
  constexpr int kWidth = 32;
  constexpr int kHeight = 16;
  std::vector<kdu_byte> bitstream;
  {
    VectorTarget target(bitstream);
    ScopedCodestream cs;
    cs.siz.set(Scomponents, 0, 0, 3);
    cs.siz.set(Sorigin, 0, 0, 0);
    cs.siz.set(Sorigin, 0, 1, 0);
    cs.siz.set(Ssize, 0, 0, kHeight);
    cs.siz.set(Ssize, 0, 1, kWidth);
    for (int c = 0; c < 3; ++c) {
      cs.siz.set(Sdims, c, 0, kHeight);
      cs.siz.set(Sdims, c, 1, kWidth);
      cs.siz.set(Ssampling, c, 0, 1);
      cs.siz.set(Ssampling, c, 1, 1);
      cs.siz.set(Sprecision, c, 0, 12);
      cs.siz.set(Ssigned, c, 0, false);
    }
    static_cast<kdu_params*>(&cs.siz)->finalize();

    cs.codestream.create(&cs.siz, &target);
    siz_params* params = cs.codestream.access_siz();
    ASSERT_TRUE(params->parse_string("Creversible=no"));
    ASSERT_TRUE(params->parse_string("Cycc=yes"));
    ASSERT_TRUE(params->parse_string("Clevels=1"));
    ASSERT_TRUE(params->parse_string("Qstep=0.00025"));
    params->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ScopedMultiAnalysis ana;
    ana.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                      KDU_MULTI_XFORM_PRECISE);
    for (int y = 0; y < kHeight; ++y) {
      for (int c = 0; c < 3; ++c) {
        kdu_line_buf* line = ana.engine.exchange_line(c, nullptr);
        ASSERT_NE(line, nullptr);
        kdu_sample32* buf = line->get_buf32();
        ASSERT_NE(buf, nullptr);
        for (int x = 0; x < kWidth; ++x) {
          const int centered = ((x * 37 + y * 53 + c * 400) % 3072) - 1536;
          buf[x].fval = static_cast<float>(centered) / 4096.0f;
        }
        ana.engine.exchange_line(c, line);
      }
    }
    ana.engine.destroy();
    tile.tile.close();
    kdu_long max_bytes = 1 << 20;
    cs.codestream.flush(&max_bytes, 1, nullptr, true, true);
    cs.codestream.destroy();
  }

  VectorSource source(bitstream);
  ScopedCodestream dec_cs;
  dec_cs.codestream.create(&source);
  ScopedTile dec_tile;
  dec_tile.tile = dec_cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  ScopedMultiSynthesis syn;
  syn.engine.create(dec_cs.codestream, dec_tile.tile, nullptr, nullptr,
                    KDU_MULTI_XFORM_PRECISE);
  double max_err = 0.0;
  for (int y = 0; y < kHeight; ++y) {
    for (int c = 0; c < 3; ++c) {
      kdu_line_buf* line = syn.engine.get_line(c);
      ASSERT_NE(line, nullptr);
      const kdu_sample32* buf = line->get_buf32();
      ASSERT_NE(buf, nullptr);
      for (int x = 0; x < kWidth; ++x) {
        const double expected =
            static_cast<double>(((x * 37 + y * 53 + c * 400) % 3072) - 1536) /
            4096.0;
        max_err = std::max(
            max_err, std::fabs(static_cast<double>(buf[x].fval) - expected));
      }
    }
  }
  syn.engine.destroy();
  dec_tile.tile.close();
  dec_cs.codestream.destroy();
  EXPECT_LT(max_err, 0.02);
}

}  // namespace
}  // namespace kdu_core
