// Unit tests for
// third_party/kakadu/vendor/coresys/coding/ssse3_coder_local.cpp.
//
// In Google3's active configuration, `vendor/BUILD` leaves
// `KDU_SIMD_OPTIMIZATIONS_UNSAFE` undefined, so `08-simd_issues.patch` in
// `decoder.cpp` disables `job->simd_block_xfer`. Consequently,
// `ssse3_xfer_rev_decoded_block16` (lines 69-108) and
// `ssse3_xfer_rev_decoded_block32` (lines 115-162) are unreachable without
// defining `KDU_SIMD_OPTIMIZATIONS_UNSAFE` (`1271/1344 = 94.57%` x86_64
// coverage ceiling; on `--cpu=arm`, SSSE3 code is compiled out under
// `KDU_X86_INTRINSICS`). This suite covers 100% of all reachable SSSE3
// line-pair transfer and CPLEX paths (including counter-overflow flushes).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
  void put_text(const char* text) override { std::fputs(text, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class QuietWarningHandler final : public kdu_thread_safe_message {
 public:
  ~QuietWarningHandler() noexcept override = default;
  void put_text(const char* /*text*/) override {}
};

class Ssse3CoderTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const error_handler = new ThrowingErrorHandler();
    static auto* const warning_handler = new QuietWarningHandler();
    kdu_customize_errors(error_handler);
    kdu_customize_warnings(warning_handler);
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Ssse3CoderTestEnvironment());

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
  int read(kdu_byte* buf, int num_bytes) override {
    if (pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail = data_.size() - pos_;
    const int n = static_cast<int>(
        std::min(avail, static_cast<size_t>(std::max(num_bytes, 0))));
    std::copy_n(data_.data() + pos_, n, buf);
    pos_ += static_cast<size_t>(n);
    return n;
  }
  bool seek(kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }
  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }
  int get_capabilities() override { return KDU_SOURCE_CAP_SEEKABLE; }

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

std::vector<int32_t> BuildPattern(int width, int height, int precision,
                                  bool is_signed) {
  std::vector<int32_t> out(static_cast<size_t>(width) * height);
  const int32_t max_mag = (1 << (std::min(precision, 15) - 1)) - 1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int32_t val =
          ((x * 13 + y * 29 + (x ^ y) * 7) % (2 * max_mag + 1)) - max_mag;
      if (!is_signed) {
        val += max_mag;
      }
      out[static_cast<size_t>(y) * width + x] = val;
    }
  }
  return out;
}

std::vector<kdu_byte> EncodeWithMulti(
    int width, int height, int precision, bool is_signed, int origin_x,
    int xform_flags, const std::vector<std::string>& cod_params,
    kdu_long max_bytes = 0) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  cs.siz.set(Scomponents, 0, 0, 1);
  if (origin_x > 0) {
    cs.siz.set(Sorigin, 0, 0, 0);
    cs.siz.set(Sorigin, 0, 1, origin_x);
    cs.siz.set(Ssize, 0, 0, height);
    cs.siz.set(Ssize, 0, 1, width + origin_x);
  }
  cs.siz.set(Sdims, 0, 0, height);
  cs.siz.set(Sdims, 0, 1, width);
  cs.siz.set(Ssampling, 0, 0, 1);
  cs.siz.set(Ssampling, 0, 1, 1);
  cs.siz.set(Sprecision, 0, 0, precision);
  cs.siz.set(Ssigned, 0, 0, is_signed);
  static_cast<kdu_params*>(&cs.siz)->finalize();
  cs.codestream.create(&cs.siz, &target);

  siz_params* params = cs.codestream.access_siz();
  for (const std::string& p : cod_params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();
  if (max_bytes > 0) {
    cs.codestream.set_max_bytes(max_bytes);
  }

  const std::vector<int32_t> samples =
      BuildPattern(width, height, precision, is_signed);
  const double inv_scale = std::ldexp(1.0, -precision);
  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_multi_analysis engine;
    engine.create(cs.codestream, tile.tile, nullptr, nullptr, xform_flags);
    for (int y = 0; y < height; ++y) {
      kdu_line_buf* line = engine.exchange_line(0, nullptr, nullptr);
      EXPECT_NE(line, nullptr);
      const int32_t* row = samples.data() + static_cast<size_t>(y) * width;
      if (kdu_sample32* dst32 = line->get_buf32()) {
        const bool is_abs = line->is_absolute();
        for (int x = 0; x < width; ++x) {
          if (is_abs) {
            dst32[x].ival = row[x];
          } else {
            dst32[x].fval = static_cast<float>(row[x] * inv_scale);
          }
        }
      } else if (kdu_sample16* dst16 = line->get_buf16()) {
        const bool is_abs = line->is_absolute();
        for (int x = 0; x < width; ++x) {
          if (is_abs) {
            dst16[x].ival = static_cast<kdu_int16>(row[x]);
          } else {
            dst16[x].ival = static_cast<kdu_int16>(std::llround(
                row[x] * std::ldexp(1.0, KDU_FIX_POINT - precision)));
          }
        }
      }
      engine.exchange_line(0, line, nullptr);
    }
    engine.destroy();
  }
  kdu_long limit = max_bytes;
  cs.codestream.flush(&limit, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

std::vector<int32_t> DecodeWithMulti(const std::vector<kdu_byte>& bitstream,
                                     int xform_flags, int* out_width = nullptr,
                                     int* out_height = nullptr) {
  VectorSource src(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&src);
  kdu_dims dims;
  cs.codestream.get_dims(0, dims);
  if (out_width != nullptr) {
    *out_width = dims.size.x;
  }
  if (out_height != nullptr) {
    *out_height = dims.size.y;
  }
  std::vector<int32_t> out(static_cast<size_t>(dims.size.x) * dims.size.y);
  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_multi_synthesis engine;
    engine.create(cs.codestream, tile.tile, nullptr, nullptr, xform_flags);
    for (int y = 0; y < dims.size.y; ++y) {
      kdu_line_buf* line = engine.get_line(0, nullptr);
      EXPECT_NE(line, nullptr);
      int32_t* dst = out.data() + static_cast<size_t>(y) * dims.size.x;
      if (const kdu_sample32* src32 = line->get_buf32()) {
        const bool is_abs = line->is_absolute();
        for (int x = 0; x < dims.size.x; ++x) {
          dst[x] =
              is_abs
                  ? src32[x].ival
                  : static_cast<int32_t>(std::llround(src32[x].fval * 256.0f));
        }
      } else if (const kdu_sample16* src16 = line->get_buf16()) {
        const bool is_abs = line->is_absolute();
        for (int x = 0; x < dims.size.x; ++x) {
          dst[x] = is_abs ? src16[x].ival : (src16[x].ival >> 5);
        }
      }
    }
    engine.destroy();
  }
  cs.codestream.destroy();
  return out;
}

std::vector<kdu_byte> EncodeDirectSubband(
    int width, int height, int precision, int origin_x, kdu_byte sample_prec,
    const std::vector<std::string>& cod_params, bool use_cplex) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  cs.siz.set(Scomponents, 0, 0, 1);
  if (origin_x > 0) {
    cs.siz.set(Sorigin, 0, 0, 0);
    cs.siz.set(Sorigin, 0, 1, origin_x);
    cs.siz.set(Ssize, 0, 0, height);
    cs.siz.set(Ssize, 0, 1, width + origin_x);
  }
  cs.siz.set(Sdims, 0, 0, height);
  cs.siz.set(Sdims, 0, 1, width);
  cs.siz.set(Ssampling, 0, 0, 1);
  cs.siz.set(Ssampling, 0, 1, 1);
  cs.siz.set(Sprecision, 0, 0, precision);
  cs.siz.set(Ssigned, 0, 0, true);
  static_cast<kdu_params*>(&cs.siz)->finalize();
  cs.codestream.create(&cs.siz, &target);

  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string("Clevels=0"));
  for (const std::string& p : cod_params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  if (use_cplex) {
    cs.codestream.set_max_bytes(65536);
  }
  params->finalize_all();

  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution res = tc.access_resolution();
    kdu_subband band = res.access_subband(LL_BAND);

    kdu_sample_allocator allocator;
    kdu_push_pull_params extra;
    kdu_cplex_analysis cplex;
    if (use_cplex) {
      cplex.init(res.access_node(), nullptr, nullptr);
    }
    kdu_encoder encoder(band, &allocator, extra, sample_prec, 1.0F, 0, nullptr,
                        use_cplex ? &cplex : nullptr);
    kdu_byte line_flags = 0;
    if (sample_prec == KDU_SAMPLE_PREC16) {
      line_flags = KDU_LINE_BUF_SHORTS;
    }
    if (band.get_reversible()) {
      line_flags |= KDU_LINE_BUF_ABSOLUTE;
    }
    kdu_line_buf line;
    line.pre_create(&allocator, width, line_flags, 0, 0);
    allocator.finalize(cs.codestream);
    line.create();

    const std::vector<int32_t> pattern =
        BuildPattern(width, height, std::min(precision, 12), true);
    const double irv_fix_scale =
        std::ldexp(1.0, KDU_FIX_POINT - std::min(precision, 12));
    for (int y = 0; y < height; ++y) {
      const int32_t* row = pattern.data() + static_cast<size_t>(y) * width;
      if (sample_prec == KDU_SAMPLE_PREC16) {
        kdu_sample16* dst = line.get_buf16();
        if (band.get_reversible()) {
          for (int x = 0; x < width; ++x) {
            dst[x].ival = static_cast<kdu_int16>(row[x]);
          }
        } else {
          for (int x = 0; x < width; ++x) {
            dst[x].ival =
                static_cast<kdu_int16>(std::llround(row[x] * irv_fix_scale));
          }
        }
      } else {
        kdu_sample32* dst = line.get_buf32();
        if (band.get_reversible()) {
          for (int x = 0; x < width; ++x) {
            dst[x].ival = row[x];
          }
        } else {
          for (int x = 0; x < width; ++x) {
            dst[x].fval = static_cast<float>(row[x]) * (1.0F / 256.0F);
          }
        }
      }
      encoder.push(line, nullptr);
      if (use_cplex) {
        cplex.service(nullptr);
      }
    }
    if (use_cplex) {
      cplex.finish(nullptr);
    }
    while (!encoder.finish(nullptr)) {
    }
    encoder.destroy();
    line.destroy();
  }
  kdu_long limit = use_cplex ? 65536 : 0;
  cs.codestream.flush(&limit, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

TEST(Ssse3CoderLocalTest, DecodedBlock16And32AlignedUnalignedSingleAndMulti) {
  // Reversible 16-bit and 32-bit decode paths cover both 2-vector loop (n>16)
  // and 1-vector remainder (n>0) plus aligned/unaligned offsets.
  for (int origin_x : {0, 3}) {
    const std::vector<kdu_byte> bs8 = EncodeWithMulti(
        76, 20, 8, true, origin_x, KDU_MULTI_XFORM_DEFAULT_FLAGS,
        {"Creversible=yes", "Clevels=1", "Cblk={32,8}"});
    ASSERT_FALSE(bs8.empty());

    int dec_w = 0, dec_h = 0;
    const std::vector<int32_t> dec16 =
        DecodeWithMulti(bs8, KDU_MULTI_XFORM_DEFAULT_FLAGS, &dec_w, &dec_h);
    EXPECT_EQ(dec_w, 76);
    EXPECT_EQ(dec_h, 20);
    EXPECT_EQ(dec16, BuildPattern(76, 20, 8, true));

    const std::vector<int32_t> dec32 =
        DecodeWithMulti(bs8, KDU_MULTI_XFORM_PRECISE, &dec_w, &dec_h);
    EXPECT_EQ(dec32, BuildPattern(76, 20, 8, true));
  }

  // Small subband block width >= 8 (16-bit) / >= 4 (32-bit) that only uses a
  // single 16-byte vector on the last iteration.
  const std::vector<kdu_byte> bs_small =
      EncodeWithMulti(28, 12, 8, true, 1, KDU_MULTI_XFORM_DEFAULT_FLAGS,
                      {"Creversible=yes", "Clevels=1", "Cblk={32,8}"});
  ASSERT_FALSE(bs_small.empty());
  EXPECT_EQ(DecodeWithMulti(bs_small, KDU_MULTI_XFORM_DEFAULT_FLAGS),
            BuildPattern(28, 12, 8, true));
  EXPECT_EQ(DecodeWithMulti(bs_small, KDU_MULTI_XFORM_PRECISE),
            BuildPattern(28, 12, 8, true));
}

TEST(Ssse3CoderLocalTest, LinePairXferAllPrecisionAndReversibilityModes) {
  // 16_16_rev: single-block immediate exit + multi-block (width0 != nom_stride
  // and middle block loop).
  EXPECT_FALSE(EncodeWithMulti(16, 8, 8, true, 0, KDU_MULTI_XFORM_DEFAULT_FLAGS,
                               {"Creversible=yes", "Clevels=1", "Cblk={16,8}"})
                   .empty());
  EXPECT_FALSE(EncodeWithMulti(76, 16, 8, true, 3,
                               KDU_MULTI_XFORM_DEFAULT_FLAGS,
                               {"Creversible=yes", "Clevels=1", "Cblk={16,8}"})
                   .empty());

  // 16_16_irv: irreversible 16-bit input + 16-bit stripe (K_max < 16).
  EXPECT_FALSE(EncodeDirectSubband(
                   16, 8, 8, 0, KDU_SAMPLE_PREC16,
                   {"Creversible=no", "Qstep=0.05", "Cblk={16,8}"}, false)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(
                   76, 16, 8, 3, KDU_SAMPLE_PREC16,
                   {"Creversible=no", "Qstep=0.05", "Cblk={16,8}"}, false)
                   .empty());

  // 16_32_rev: reversible 16-bit line input + 32-bit stripe (K_max >= 16).
  EXPECT_FALSE(EncodeDirectSubband(16, 8, 16, 0, KDU_SAMPLE_PREC16,
                                   {"Creversible=yes", "Cblk={16,8}"}, false)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(76, 16, 16, 3, KDU_SAMPLE_PREC16,
                                   {"Creversible=yes", "Cblk={16,8}"}, false)
                   .empty());

  // 32_32_rev: reversible 32-bit line input + 32-bit stripe.
  EXPECT_FALSE(EncodeWithMulti(16, 8, 8, true, 0, KDU_MULTI_XFORM_PRECISE,
                               {"Creversible=yes", "Clevels=1", "Cblk={16,8}"})
                   .empty());
  EXPECT_FALSE(EncodeWithMulti(76, 16, 8, true, 3, KDU_MULTI_XFORM_PRECISE,
                               {"Creversible=yes", "Clevels=1", "Cblk={16,8}"})
                   .empty());
}

TEST(Ssse3CoderLocalTest, LinePairCplexAllModesAndCounterFlush) {
  // 16_16_rev + CPLEX: single block, multi-block, and wide strip counter flush
  // (max_count >= 256 - max_count_per_block). Note Cblk={rows, cols}.
  EXPECT_FALSE(EncodeWithMulti(32, 16, 8, true, 0,
                               KDU_MULTI_XFORM_DEFAULT_FLAGS,
                               {"Creversible=yes", "Clevels=1", "Cblk={8,16}",
                                "Cplex={6,EST,0.25,0}"},
                               16384)
                   .empty());
  EXPECT_FALSE(EncodeWithMulti(104, 16, 8, true, 3,
                               KDU_MULTI_XFORM_DEFAULT_FLAGS,
                               {"Creversible=yes", "Clevels=1", "Cblk={8,16}",
                                "Cplex={6,EST,0.25,0}"},
                               16384)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(
                   84, 16, 8, 3, KDU_SAMPLE_PREC16,
                   {"Creversible=yes", "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(
                   4160, 4, 8, 0, KDU_SAMPLE_PREC16,
                   {"Creversible=yes", "Cblk={4,64}", "Cplex={6,EST,0.25,0}"},
                   true)
                   .empty());

  // 16_16_irv + CPLEX: K_max < 16, KDU_SAMPLE_PREC16, irreversible.
  EXPECT_FALSE(EncodeDirectSubband(16, 8, 8, 0, KDU_SAMPLE_PREC16,
                                   {"Creversible=no", "Qstep=0.05",
                                    "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(84, 16, 8, 3, KDU_SAMPLE_PREC16,
                                   {"Creversible=no", "Qstep=0.05",
                                    "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(4160, 4, 8, 0, KDU_SAMPLE_PREC16,
                                   {"Creversible=no", "Qstep=0.05",
                                    "Cblk={4,64}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());

  // 16_32_irv + CPLEX: K_max >= 16, KDU_SAMPLE_PREC16, irreversible.
  EXPECT_FALSE(EncodeDirectSubband(16, 8, 8, 0, KDU_SAMPLE_PREC16,
                                   {"Creversible=no", "Qstep=0.00001",
                                    "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(84, 16, 8, 3, KDU_SAMPLE_PREC16,
                                   {"Creversible=no", "Qstep=0.00001",
                                    "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(4160, 4, 8, 0, KDU_SAMPLE_PREC16,
                                   {"Creversible=no", "Qstep=0.00001",
                                    "Cblk={4,64}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());

  // 32_32_rev + CPLEX: reversible 32-bit line input + 32-bit stripe.
  EXPECT_FALSE(EncodeWithMulti(16, 8, 8, true, 0, KDU_MULTI_XFORM_PRECISE,
                               {"Creversible=yes", "Clevels=1", "Cblk={8,16}",
                                "Cplex={6,EST,0.25,0}"},
                               16384)
                   .empty());
  EXPECT_FALSE(EncodeWithMulti(84, 16, 8, true, 3, KDU_MULTI_XFORM_PRECISE,
                               {"Creversible=yes", "Clevels=1", "Cblk={8,16}",
                                "Cplex={6,EST,0.25,0}"},
                               16384)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(
                   2112, 4, 8, 0, KDU_SAMPLE_PREC32,
                   {"Creversible=yes", "Cblk={4,64}", "Cplex={6,EST,0.25,0}"},
                   true)
                   .empty());

  // 32_32_irv + CPLEX: irreversible 32-bit line input + 32-bit stripe.
  EXPECT_FALSE(EncodeDirectSubband(16, 8, 8, 0, KDU_SAMPLE_PREC32,
                                   {"Creversible=no", "Qstep=0.002",
                                    "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(84, 16, 8, 3, KDU_SAMPLE_PREC32,
                                   {"Creversible=no", "Qstep=0.002",
                                    "Cblk={8,16}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
  EXPECT_FALSE(EncodeDirectSubband(2112, 4, 8, 0, KDU_SAMPLE_PREC32,
                                   {"Creversible=no", "Qstep=0.002",
                                    "Cblk={4,64}", "Cplex={6,EST,0.25,0}"},
                                   true)
                   .empty());
}

}  // namespace
}  // namespace kdu_core
