// Unit tests for
// third_party/kakadu/vendor/coresys/transform/ssse3_colour_local.cpp.
//
// In Google3's active configuration, `third_party/kakadu/vendor/BUILD` leaves
// `KDU_SIMD_OPTIMIZATIONS_UNSAFE` undefined, and `08-simd_issues.patch` wraps
// `KD_SET_SIMD_FUNC_RGB_TO_YCC_IRREV16` and
// `KD_SET_SIMD_FUNC_YCC_TO_RGB_IRREV16` in `colour.cpp` inside `#ifdef
// KDU_SIMD_OPTIMIZATIONS_UNSAFE`. Consequently, `ssse3_rgb_to_ycc_irrev16` and
// `ssse3_ycc_to_rgb_irrev16` are never installed into the public
// `kdu_convert_rgb_to_ycc_irrev16` / `kdu_convert_ycc_to_rgb_irrev16` function
// pointers (`0/43 = 0.00%` LCOV `DA:` patch-disabled ceiling on x86_64;
// compiled out under `KDU_X86_INTRINSICS` on `--cpu=arm`). This suite verifies
// that the corresponding 16-bit irreversible ICT entrypoints and codestream
// pipeline operate deterministically under Google3's safe scalar dispatch.

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

class Ssse3ColourLocalTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kSsse3ColourEnv =
    ::testing::AddGlobalTestEnvironment(new Ssse3ColourLocalTestEnvironment());

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

TEST(Ssse3ColourLocalTest, Aligned16BitIrreversibleRgbYccRoundTrip) {
  ASSERT_NE(kdu_convert_rgb_to_ycc_irrev16, nullptr);
  ASSERT_NE(kdu_convert_ycc_to_rgb_irrev16, nullptr);

  // 16-byte aligned 16-bit fixed-point sample vectors matching the
  // 8-sample-per- iteration contract of `ssse3_rgb_to_ycc_irrev16` /
  // `ssse3_ycc_to_rgb_irrev16`.
  constexpr int kSamples = 32;
  alignas(16) kdu_int16 r[kSamples];
  alignas(16) kdu_int16 g[kSamples];
  alignas(16) kdu_int16 b[kSamples];
  kdu_int16 orig_r[kSamples];
  kdu_int16 orig_g[kSamples];
  kdu_int16 orig_b[kSamples];

  const double scale = static_cast<double>(1 << KDU_FIX_POINT);
  for (int i = 0; i < kSamples; ++i) {
    const double rv = -0.45 + 0.025 * i;
    const double gv = 0.40 - 0.022 * i;
    const double bv = -0.25 + 0.018 * i;
    r[i] = orig_r[i] = static_cast<kdu_int16>(std::llround(rv * scale));
    g[i] = orig_g[i] = static_cast<kdu_int16>(std::llround(gv * scale));
    b[i] = orig_b[i] = static_cast<kdu_int16>(std::llround(bv * scale));
  }

  kdu_convert_rgb_to_ycc_irrev16(r, g, b, kSamples);
  kdu_convert_ycc_to_rgb_irrev16(r, g, b, kSamples);

  for (int i = 0; i < kSamples; ++i) {
    EXPECT_NEAR(r[i], orig_r[i], 32);
    EXPECT_NEAR(g[i], orig_g[i], 32);
    EXPECT_NEAR(b[i], orig_b[i], 32);
  }
}

TEST(Ssse3ColourLocalTest, MultiTransform16BitIrreversibleIctCodestream) {
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
      cs.siz.set(Sprecision, c, 0, 8);
      cs.siz.set(Ssigned, c, 0, false);
    }
    static_cast<kdu_params*>(&cs.siz)->finalize();

    cs.codestream.create(&cs.siz, &target);
    siz_params* params = cs.codestream.access_siz();
    ASSERT_TRUE(params->parse_string("Creversible=no"));
    ASSERT_TRUE(params->parse_string("Cycc=yes"));
    ASSERT_TRUE(params->parse_string("Clevels=1"));
    ASSERT_TRUE(params->parse_string("Qstep=0.0005"));
    params->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ScopedMultiAnalysis ana;
    ana.engine.create(cs.codestream, tile.tile, nullptr, nullptr, 0);
    const double scale = static_cast<double>(1 << KDU_FIX_POINT) / 256.0;
    for (int y = 0; y < kHeight; ++y) {
      for (int c = 0; c < 3; ++c) {
        kdu_line_buf* line = ana.engine.exchange_line(c, nullptr);
        ASSERT_NE(line, nullptr);
        kdu_sample16* buf = line->get_buf16();
        ASSERT_NE(buf, nullptr);
        for (int x = 0; x < kWidth; ++x) {
          const int centered = ((x * 5 + y * 9 + c * 40) % 192) - 96;
          buf[x].ival = static_cast<kdu_int16>(std::llround(centered * scale));
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
  syn.engine.create(dec_cs.codestream, dec_tile.tile, nullptr, nullptr, 0);
  const double inv_scale = 256.0 / static_cast<double>(1 << KDU_FIX_POINT);
  double max_err = 0.0;
  for (int y = 0; y < kHeight; ++y) {
    for (int c = 0; c < 3; ++c) {
      kdu_line_buf* line = syn.engine.get_line(c);
      ASSERT_NE(line, nullptr);
      const kdu_sample16* buf = line->get_buf16();
      ASSERT_NE(buf, nullptr);
      for (int x = 0; x < kWidth; ++x) {
        const int expected = ((x * 5 + y * 9 + c * 40) % 192) - 96;
        const double got = static_cast<double>(buf[x].ival) * inv_scale;
        max_err = std::max(max_err, std::fabs(got - expected));
      }
    }
  }
  syn.engine.destroy();
  dec_tile.tile.close();
  dec_cs.codestream.destroy();
  EXPECT_LT(max_err, 6.0);
}

}  // namespace
}  // namespace kdu_core
