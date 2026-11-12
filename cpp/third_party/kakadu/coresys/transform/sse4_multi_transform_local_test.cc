// Unit tests for
// third_party/kakadu/vendor/coresys/transform/sse4_multi_transform_local.cpp.
//
// Exercises `kd_core_simd::sse4_smag_int32` and
// `kd_core_simd::sse4_umag_int32` through the public `kdu_multi_analysis` and
// `kdu_multi_synthesis` Part-2 NLT (`Sextensions=NLT`, `NLType=SMAG`/`UMAG`)
// paths with `KDU_MULTI_XFORM_PRECISE` (32-bit sample buffers). Tests all
// three runtime paths in both functions (`src_absolute && dst_absolute`
// reversible int-to-int, `!dst_absolute` irreversible int-to-float analysis,
// and `!src_absolute` irreversible float-to-int synthesis). On `--cpu=arm`,
// `kdu_arch` compiles without x86 intrinsics and falls back to scalar
// implementations with identical round-trip behavior.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

class Sse4MultiTransformLocalTestEnvironment final
    : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kSse4MultiTransformEnv =
    ::testing::AddGlobalTestEnvironment(
        new Sse4MultiTransformLocalTestEnvironment());

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

struct ComponentData {
  int precision = 8;
  bool is_signed = false;
  std::string nl_type;
  std::vector<int32_t> samples;
};

std::vector<kdu_byte> EncodeNlt32Bit(
    int width, int height, bool reversible,
    const std::vector<ComponentData>& components,
    const char* extra_coding_param = nullptr) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  const int num_comps = static_cast<int>(components.size());
  EXPECT_TRUE(cs.siz.parse_string(
      ("Scomponents=" + std::to_string(num_comps)).c_str()));
  cs.siz.set(Sorigin, 0, 0, 0);
  cs.siz.set(Sorigin, 0, 1, 0);
  cs.siz.set(Ssize, 0, 0, height);
  cs.siz.set(Ssize, 0, 1, width);
  std::string n_signed = "Nsigned=";
  for (int c = 0; c < num_comps; ++c) {
    cs.siz.set(Sdims, c, 0, height);
    cs.siz.set(Sdims, c, 1, width);
    cs.siz.set(Ssampling, c, 0, 1);
    cs.siz.set(Ssampling, c, 1, 1);
    cs.siz.set(Sprecision, c, 0, components[c].precision);
    cs.siz.set(Ssigned, c, 0, components[c].is_signed);
    if (c > 0) {
      n_signed += ',';
    }
    n_signed += (components[c].is_signed ? "yes" : "no");
  }
  EXPECT_TRUE(cs.siz.parse_string("Sextensions=NLT"));
  EXPECT_TRUE(cs.siz.parse_string(
      ("Ncomponents=" + std::to_string(num_comps)).c_str()));
  EXPECT_TRUE(cs.siz.parse_string(
      ("Nprecision=" + std::to_string(components[0].precision)).c_str()));
  EXPECT_TRUE(cs.siz.parse_string(n_signed.c_str()));
  static_cast<kdu_params*>(&cs.siz)->finalize();

  cs.codestream.create(&cs.siz, &target);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(
      params->parse_string(reversible ? "Creversible=yes" : "Creversible=no"));
  EXPECT_TRUE(params->parse_string("Cycc=no"));
  EXPECT_TRUE(params->parse_string("Clevels=1"));
  for (int c = 0; c < num_comps; ++c) {
    EXPECT_TRUE(params->parse_string(
        ("NLType:C" + std::to_string(c) + "=" + components[c].nl_type)
            .c_str()));
  }
  if (extra_coding_param != nullptr) {
    EXPECT_TRUE(params->parse_string(extra_coding_param));
  }
  params->finalize_all();

  kdu_dims tile_indices;
  cs.codestream.get_valid_tiles(tile_indices);
  for (int ty = tile_indices.pos.y;
       ty < tile_indices.pos.y + tile_indices.size.y; ++ty) {
    for (int tx = tile_indices.pos.x;
         tx < tile_indices.pos.x + tile_indices.size.x; ++tx) {
      ScopedTile tile;
      tile.tile = cs.codestream.open_tile(kdu_coords(tx, ty), nullptr);
      ScopedMultiAnalysis ana;
      ana.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                        KDU_MULTI_XFORM_PRECISE);
      std::vector<int> rows_done(num_comps, 0);
      bool any_left = true;
      while (any_left) {
        any_left = false;
        for (int c = 0; c < num_comps; ++c) {
          if (rows_done[c] >= height) {
            continue;
          }
          any_left = true;
          kdu_line_buf* line = ana.engine.exchange_line(c, nullptr);
          if (line != nullptr) {
            // `KDU_MULTI_XFORM_PRECISE` selects 32-bit `kdu_sample32` buffers
            // whose `usmag32_func` hook resolves to SSE4.1 `sse4_*_int32`.
            kdu_sample32* dst = line->get_buf32();
            EXPECT_NE(dst, nullptr);
            if (dst == nullptr) {
              return bitstream;
            }
            const int32_t dc =
                components[c].is_signed
                    ? 0
                    : (int32_t{1} << (components[c].precision - 1));
            const int32_t* row = components[c].samples.data() +
                                 static_cast<size_t>(rows_done[c]) * width;
            if (line->is_absolute()) {
              for (int x = 0; x < width; ++x) {
                dst[x].ival = static_cast<kdu_int32>(row[x] - dc);
              }
            } else {
              const double inv_scale =
                  1.0 /
                  static_cast<double>(int32_t{1} << components[c].precision);
              for (int x = 0; x < width; ++x) {
                dst[x].fval = static_cast<float>(
                    static_cast<double>(row[x] - dc) * inv_scale);
              }
            }
            ana.engine.exchange_line(c, line);
            rows_done[c]++;
          }
        }
      }
      ana.engine.destroy();
      tile.tile.close();
    }
  }

  kdu_long max_bytes = 1 << 20;
  cs.codestream.flush(&max_bytes, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

std::vector<std::vector<int32_t>> DecodeNlt32Bit(
    const std::vector<kdu_byte>& bitstream) {
  VectorSource source(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&source);
  const int num_comps = cs.codestream.get_num_components(true);
  std::vector<int> precisions(num_comps);
  std::vector<bool> is_signed(num_comps);
  std::vector<int> widths(num_comps);
  std::vector<int> heights(num_comps);
  std::vector<std::vector<int32_t>> output(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    kdu_dims dims;
    cs.codestream.get_dims(c, dims, true);
    widths[c] = dims.size.x;
    heights[c] = dims.size.y;
    precisions[c] = cs.codestream.get_bit_depth(c, true, false);
    is_signed[c] = cs.codestream.get_signed(c, true);
    output[c].reserve(static_cast<size_t>(widths[c]) * heights[c]);
  }

  kdu_dims tile_indices;
  cs.codestream.get_valid_tiles(tile_indices);
  for (int ty = tile_indices.pos.y;
       ty < tile_indices.pos.y + tile_indices.size.y; ++ty) {
    for (int tx = tile_indices.pos.x;
         tx < tile_indices.pos.x + tile_indices.size.x; ++tx) {
      ScopedTile tile;
      tile.tile = cs.codestream.open_tile(kdu_coords(tx, ty), nullptr);
      ScopedMultiSynthesis syn;
      syn.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                        KDU_MULTI_XFORM_PRECISE);
      std::vector<int> rows_left(num_comps);
      for (int c = 0; c < num_comps; ++c) {
        rows_left[c] = heights[c];
      }
      bool any_left = true;
      while (any_left) {
        any_left = false;
        for (int c = 0; c < num_comps; ++c) {
          if (rows_left[c] <= 0) {
            continue;
          }
          any_left = true;
          kdu_line_buf* line = syn.engine.get_line(c);
          if (line != nullptr) {
            const kdu_sample32* src = line->get_buf32();
            EXPECT_NE(src, nullptr);
            if (src == nullptr) {
              return output;
            }
            const int32_t dc =
                is_signed[c] ? 0 : (int32_t{1} << (precisions[c] - 1));
            if (line->is_absolute()) {
              for (int x = 0; x < widths[c]; ++x) {
                output[c].push_back(static_cast<int32_t>(src[x].ival) + dc);
              }
            } else {
              const double scale =
                  static_cast<double>(int32_t{1} << precisions[c]);
              for (int x = 0; x < widths[c]; ++x) {
                output[c].push_back(
                    static_cast<int32_t>(std::llround(
                        static_cast<double>(src[x].fval) * scale)) +
                    dc);
              }
            }
            rows_left[c]--;
          }
        }
      }
      syn.engine.destroy();
      tile.tile.close();
    }
  }
  cs.codestream.destroy();
  return output;
}

TEST(Sse4MultiTransformLocalTest, Reversible32BitSmagAndUmagRoundTrip) {
  // 20x16 (>4 samples per line for multiple 4-wide SSE4 vector iterations) with
  // positive, zero, negative, and boundary 8-bit values across SMAG and UMAG.
  constexpr int kWidth = 20;
  constexpr int kHeight = 16;
  std::vector<int32_t> smag_samples(kWidth * kHeight);
  std::vector<int32_t> umag_samples(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const size_t idx = static_cast<size_t>(y) * kWidth + x;
      smag_samples[idx] = ((x * 23 - y * 19 + 300) % 256) - 128;
      umag_samples[idx] = (x * 17 + y * 31) % 256;
    }
  }
  smag_samples[0] = -128;
  smag_samples[1] = -1;
  smag_samples[2] = 0;
  smag_samples[3] = 127;
  umag_samples[0] = 0;
  umag_samples[1] = 1;
  umag_samples[2] = 128;
  umag_samples[3] = 255;

  const std::vector<ComponentData> comps = {
      {8, true, "SMAG", smag_samples},
      {8, false, "UMAG", umag_samples},
  };
  // 1. Reversible encode + decode covers the `else` (`src_absolute &&
  // dst_absolute`) path in both `sse4_smag_int32` and `sse4_umag_int32`.
  const std::vector<kdu_byte> bs = EncodeNlt32Bit(kWidth, kHeight, true, comps);
  const std::vector<std::vector<int32_t>> dec = DecodeNlt32Bit(bs);
  ASSERT_EQ(dec.size(), 2u);
  EXPECT_EQ(dec[0], smag_samples);
  EXPECT_EQ(dec[1], umag_samples);
}

TEST(Sse4MultiTransformLocalTest,
     Irreversible32BitSmagAndUmagAnalysisAndSynthesisWithClamping) {
  // 1. Smooth 12-bit signals verify irreversible `!dst_absolute` analysis and
  // `!src_absolute` synthesis round-trip accuracy for both SMAG and UMAG.
  // In P=12 sign-magnitude (SMAG), negative values near `min_val` (-2048)
  // represent small negative magnitudes (`~0`), so a wave encoded via SMAG
  // stays smooth in the transformed domain across both positive and negative
  // values.
  constexpr int kWidth = 24;
  constexpr int kHeight = 20;
  std::vector<int32_t> smag_smooth(kWidth * kHeight);
  std::vector<int32_t> umag_smooth(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const size_t idx = static_cast<size_t>(y) * kWidth + x;
      const int32_t mag = (x * 40 + y * 20) % 1500;
      smag_smooth[idx] = ((x / 12) % 2 == 0) ? mag : (-2048 + (mag / 2));
      umag_smooth[idx] = 500 + mag;
    }
  }
  const std::vector<ComponentData> smooth_comps = {
      {12, true, "SMAG", smag_smooth},
      {12, false, "UMAG", umag_smooth},
  };
  const std::vector<kdu_byte> smooth_bs =
      EncodeNlt32Bit(kWidth, kHeight, false, smooth_comps, "Qstep=0.0005");
  const std::vector<std::vector<int32_t>> smooth_dec =
      DecodeNlt32Bit(smooth_bs);
  ASSERT_EQ(smooth_dec.size(), 2u);
  ASSERT_EQ(smooth_dec[0].size(), smag_smooth.size());
  ASSERT_EQ(smooth_dec[1].size(), umag_smooth.size());
  double umag_sum_abs = 0.0;
  int smag_small_err_count = 0;
  for (size_t i = 0; i < umag_smooth.size(); ++i) {
    EXPECT_GE(smooth_dec[0][i], -2048);
    EXPECT_LE(smooth_dec[0][i], 2047);
    EXPECT_GE(smooth_dec[1][i], 0);
    EXPECT_LE(smooth_dec[1][i], 4095);
    umag_sum_abs +=
        std::fabs(static_cast<double>(smooth_dec[1][i] - umag_smooth[i]));
    if (std::fabs(static_cast<double>(smooth_dec[0][i] - smag_smooth[i])) <=
        64.0) {
      smag_small_err_count++;
    }
  }
  EXPECT_LT(umag_sum_abs / umag_smooth.size(), 25.0);
  EXPECT_GT(smag_small_err_count, static_cast<int>(smag_smooth.size() / 2));

  // 2. High-contrast 12-bit checkerboard with coarse quantization triggers
  // DWT overshoot beyond `[min_val, max_val]`, verifying clamping during
  // float-to-SMAG/UMAG synthesis.
  std::vector<int32_t> smag_clip(kWidth * kHeight);
  std::vector<int32_t> umag_clip(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const size_t idx = static_cast<size_t>(y) * kWidth + x;
      const bool high = ((x / 4) + (y / 4)) % 2 == 0;
      smag_clip[idx] = high ? 2047 : -1;
      umag_clip[idx] = high ? 4095 : 0;
    }
  }
  const std::vector<ComponentData> clip_comps = {
      {12, true, "SMAG", smag_clip},
      {12, false, "UMAG", umag_clip},
  };
  const std::vector<kdu_byte> clip_bs =
      EncodeNlt32Bit(kWidth, kHeight, false, clip_comps, "Qstep=0.05");
  const std::vector<std::vector<int32_t>> clip_dec = DecodeNlt32Bit(clip_bs);
  ASSERT_EQ(clip_dec.size(), 2u);
  ASSERT_EQ(clip_dec[0].size(), smag_clip.size());
  ASSERT_EQ(clip_dec[1].size(), umag_clip.size());
  for (size_t i = 0; i < clip_dec[0].size(); ++i) {
    EXPECT_GE(clip_dec[0][i], -2048);
    EXPECT_LE(clip_dec[0][i], 2047);
    EXPECT_GE(clip_dec[1][i], 0);
    EXPECT_LE(clip_dec[1][i], 4095);
  }
}

}  // namespace
}  // namespace kdu_core
