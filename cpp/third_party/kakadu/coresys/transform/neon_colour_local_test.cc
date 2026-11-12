// Unit tests for
// third_party/kakadu/vendor/coresys/transform/neon_colour_local.cpp.
//
// In Google3's active build configuration (`kdu_arch.h` gates
// `KDU_NEON_INTRINSICS` on `#ifdef __ARM_NEON__`), `neon_colour_local.cpp` is
// compiled out (`0/0` LCOV `DA:` ceiling on both x86_64 and `--cpu=arm`). This
// suite verifies that 16-bit and 32-bit reversible RCT and irreversible ICT
// colour transforms (both direct `kdu_convert_*` entrypoints on 32-byte-aligned
// buffers and `kdu_multi_*` codestream round-trips) fall back cleanly and
// accurately.

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

class NeonColourLocalTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kNeonColourEnv =
    ::testing::AddGlobalTestEnvironment(new NeonColourLocalTestEnvironment());

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

TEST(NeonColourLocalTest, AlignedDirectRctAndIctRoundTrips) {
  constexpr int kSamples = 64;

  // 16-bit reversible RCT.
  {
    alignas(32) kdu_int16 r[kSamples], g[kSamples], b[kSamples];
    kdu_int16 orig_r[kSamples], orig_g[kSamples], orig_b[kSamples];
    for (int i = 0; i < kSamples; ++i) {
      r[i] = orig_r[i] = static_cast<kdu_int16>((i - 32) * 31 + (i % 5));
      g[i] = orig_g[i] = static_cast<kdu_int16>((20 - i) * 29 - (i % 3));
      b[i] = orig_b[i] = static_cast<kdu_int16>((i - 15) * 37 + (i % 7));
    }
    kdu_convert_rgb_to_ycc_rev16(r, g, b, kSamples);
    kdu_convert_ycc_to_rgb_rev16(r, g, b, kSamples);
    for (int i = 0; i < kSamples; ++i) {
      EXPECT_EQ(r[i], orig_r[i]) << "i=" << i;
      EXPECT_EQ(g[i], orig_g[i]) << "i=" << i;
      EXPECT_EQ(b[i], orig_b[i]) << "i=" << i;
    }
  }

  // 32-bit reversible RCT.
  {
    alignas(32) kdu_int32 r[kSamples], g[kSamples], b[kSamples];
    kdu_int32 orig_r[kSamples], orig_g[kSamples], orig_b[kSamples];
    for (int i = 0; i < kSamples; ++i) {
      r[i] = orig_r[i] = (i - 32) * 50000 + i * 17;
      g[i] = orig_g[i] = (18 - i) * 40000 - i * 13;
      b[i] = orig_b[i] = (i - 10) * 60000 + i * 11;
    }
    kdu_convert_rgb_to_ycc_rev32(r, g, b, kSamples);
    kdu_convert_ycc_to_rgb_rev32(r, g, b, kSamples);
    for (int i = 0; i < kSamples; ++i) {
      EXPECT_EQ(r[i], orig_r[i]) << "i=" << i;
      EXPECT_EQ(g[i], orig_g[i]) << "i=" << i;
      EXPECT_EQ(b[i], orig_b[i]) << "i=" << i;
    }
  }

  // 16-bit fixed-point irreversible ICT.
  {
    alignas(32) kdu_int16 r[kSamples], g[kSamples], b[kSamples];
    kdu_int16 orig_r[kSamples], orig_g[kSamples], orig_b[kSamples];
    for (int i = 0; i < kSamples; ++i) {
      r[i] = orig_r[i] = static_cast<kdu_int16>((i - 32) * 90);
      g[i] = orig_g[i] = static_cast<kdu_int16>((25 - i) * 85);
      b[i] = orig_b[i] = static_cast<kdu_int16>((i - 16) * 80);
    }
    kdu_convert_rgb_to_ycc_irrev16(r, g, b, kSamples);
    kdu_convert_ycc_to_rgb_irrev16(r, g, b, kSamples);
    for (int i = 0; i < kSamples; ++i) {
      EXPECT_NEAR(r[i], orig_r[i], 4) << "i=" << i;
      EXPECT_NEAR(g[i], orig_g[i], 4) << "i=" << i;
      EXPECT_NEAR(b[i], orig_b[i], 4) << "i=" << i;
    }
  }

  // 32-bit float irreversible ICT.
  {
    alignas(32) float r[kSamples], g[kSamples], b[kSamples];
    float orig_r[kSamples], orig_g[kSamples], orig_b[kSamples];
    for (int i = 0; i < kSamples; ++i) {
      r[i] = orig_r[i] = -0.45F + 0.013F * static_cast<float>(i);
      g[i] = orig_g[i] = 0.42F - 0.012F * static_cast<float>(i);
      b[i] = orig_b[i] = -0.35F + 0.011F * static_cast<float>(i);
    }
    kdu_convert_rgb_to_ycc_irrev32(r, g, b, kSamples);
    kdu_convert_ycc_to_rgb_irrev32(r, g, b, kSamples);
    for (int i = 0; i < kSamples; ++i) {
      EXPECT_NEAR(r[i], orig_r[i], 1e-5F) << "i=" << i;
      EXPECT_NEAR(g[i], orig_g[i], 1e-5F) << "i=" << i;
      EXPECT_NEAR(b[i], orig_b[i], 1e-5F) << "i=" << i;
    }
  }
}

TEST(NeonColourLocalTest, MultiTransformCodestreamRctAndIctRoundTrip) {
  constexpr int kWidth = 64;
  constexpr int kHeight = 16;

  for (bool reversible : {true, false}) {
    for (bool precise : {false, true}) {
      const int bit_depth = precise ? 10 : 8;
      const int mod = 3 << (bit_depth - 2);
      const int bias = 1 << (bit_depth - 2);
      const double scale = static_cast<double>(1 << bit_depth);

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
          cs.siz.set(Sprecision, c, 0, bit_depth);
          cs.siz.set(Ssigned, c, 0, false);
        }
        static_cast<kdu_params*>(&cs.siz)->finalize();

        cs.codestream.create(&cs.siz, &target);
        siz_params* params = cs.codestream.access_siz();
        ASSERT_TRUE(params->parse_string(reversible ? "Creversible=yes"
                                                    : "Creversible=no"));
        ASSERT_TRUE(params->parse_string("Cycc=yes"));
        ASSERT_TRUE(params->parse_string("Clevels=1"));
        if (!reversible) {
          ASSERT_TRUE(params->parse_string("Qstep=0.00025"));
        }
        params->finalize_all();

        ScopedTile tile;
        tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
        ScopedMultiAnalysis ana;
        ana.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                          precise ? KDU_MULTI_XFORM_PRECISE : 0);
        for (int y = 0; y < kHeight; ++y) {
          for (int c = 0; c < 3; ++c) {
            kdu_line_buf* line = ana.engine.exchange_line(c, nullptr);
            ASSERT_NE(line, nullptr);
            kdu_sample32* buf32 = line->get_buf32();
            kdu_sample16* buf16 = line->get_buf16();
            ASSERT_TRUE(buf32 != nullptr || buf16 != nullptr);
            const bool is_abs = line->is_absolute();
            for (int x = 0; x < kWidth; ++x) {
              const int centered =
                  ((x * 23 + y * 41 + c * (bias / 2)) % mod) - bias;
              if (buf32 != nullptr && !is_abs) {
                buf32[x].fval =
                    static_cast<float>(centered) / static_cast<float>(scale);
              } else if (buf32 != nullptr) {
                buf32[x].ival = centered;
              } else if (is_abs) {
                buf16[x].ival = static_cast<kdu_int16>(centered);
              } else {
                buf16[x].ival = static_cast<kdu_int16>(
                    std::llround(static_cast<double>(centered) *
                                 (1 << KDU_FIX_POINT) / scale));
              }
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
                        precise ? KDU_MULTI_XFORM_PRECISE : 0);
      double max_err = 0.0;
      for (int y = 0; y < kHeight; ++y) {
        for (int c = 0; c < 3; ++c) {
          kdu_line_buf* line = syn.engine.get_line(c);
          ASSERT_NE(line, nullptr);
          const kdu_sample32* buf32 = line->get_buf32();
          const kdu_sample16* buf16 = line->get_buf16();
          ASSERT_TRUE(buf32 != nullptr || buf16 != nullptr);
          const bool is_abs = line->is_absolute();
          for (int x = 0; x < kWidth; ++x) {
            const int centered =
                ((x * 23 + y * 41 + c * (bias / 2)) % mod) - bias;
            if (buf32 != nullptr && !is_abs) {
              const double actual = static_cast<double>(buf32[x].fval) * scale;
              max_err = std::max(max_err, std::fabs(actual - centered));
            } else if (buf32 != nullptr) {
              EXPECT_EQ(buf32[x].ival, centered);
            } else if (is_abs) {
              EXPECT_EQ(buf16[x].ival, static_cast<kdu_int16>(centered));
            } else {
              const double actual = static_cast<double>(buf16[x].ival) * scale /
                                    (1 << KDU_FIX_POINT);
              max_err = std::max(max_err, std::fabs(actual - centered));
            }
          }
        }
      }
      syn.engine.destroy();
      dec_tile.tile.close();
      dec_cs.codestream.destroy();
      if (!reversible) {
        EXPECT_LT(max_err, 6.0) << "precise=" << precise;
      }
    }
  }
}

}  // namespace
}  // namespace kdu_core
