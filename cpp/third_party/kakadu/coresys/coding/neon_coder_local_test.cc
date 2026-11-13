// Unit tests covering fallback paths corresponding to
// third_party/kakadu/vendor/coresys/coding/neon_coder_local.cpp.
//
// In Google3's active build configuration, `kdu_arch.h` gates
// `KDU_NEON_INTRINSICS` on `#ifdef __ARM_NEON__`, and `neon_coder_local.cpp`
// wraps its entire body in
// `#if ((!defined KDU_NO_NEON) && (defined KDU_NEON_INTRINSICS))`.
// Consequently, `neon_coder_local.cpp` compiles out cleanly on both x86_64 and
// `--cpu=arm` (`0/0` instrumented `DA:` lines).
//
// This suite exercises the equivalent fallback encoder and decoder paths across
// the sample precisions, stripe widths, alignments, and geometries targeted by
// `neon_coder_local.cpp` (16/32/40-bit reversible and 16/32-bit irreversible
// decoded-block transfers including low-`K_max` 16-bit internal code-block
// storage, zero blocks, `pull64`, `block_xfer32`, `lpair_xfer`, and
// `lpair_cplex` overflow flushes).

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

class NeonCoderTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const error_handler = new ThrowingErrorHandler();
    static auto* const warning_handler = new QuietWarningHandler();
    kdu_customize_errors(error_handler);
    kdu_customize_warnings(warning_handler);
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new NeonCoderTestEnvironment());

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
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
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

std::vector<int64_t> MakeSamples(int width, int height, int bit_depth,
                                 bool sparse_zero_blocks, int64_t scale_bias) {
  std::vector<int64_t> out(static_cast<size_t>(width) * height, 0);
  const int64_t half = int64_t{1} << std::min(bit_depth - 1, 14);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (sparse_zero_blocks && ((x / 16 + y / 8) % 2 == 1)) {
        continue;
      }
      int64_t v =
          ((x * 29 + y * 37 + (x ^ y) * 17) % (2 * half - 1)) - (half - 1);
      if (scale_bias != 0 && ((x + y) % 5 != 0)) {
        v = (v >= 0 ? scale_bias : -scale_bias) + v;
      }
      out[static_cast<size_t>(y) * width + x] = v;
    }
  }
  return out;
}

kdu_byte LineFlagForPrec(kdu_byte sample_prec, bool reversible) {
  return sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
}

void PackLine(kdu_line_buf& line, const int64_t* row, int width, int bit_depth,
              bool reversible) {
  const double inv_scale = std::ldexp(1.0, -bit_depth);
  const double fix_scale = std::ldexp(1.0, KDU_FIX_POINT - bit_depth);
  if (kdu_sample16* p16 = line.get_buf16()) {
    for (int x = 0; x < width; ++x) {
      p16[x].ival =
          reversible ? static_cast<kdu_int16>(row[x])
                     : static_cast<kdu_int16>(std::llround(row[x] * fix_scale));
    }
    return;
  }
  if (kdu_sample64* p64 = line.get_buf64()) {
    for (int x = 0; x < width; ++x) {
      p64[x].ival = static_cast<kdu_int64>(row[x]);
    }
    return;
  }
  kdu_sample32* p32 = line.get_buf32();
  ASSERT_NE(p32, nullptr);
  for (int x = 0; x < width; ++x) {
    if (reversible) {
      p32[x].ival = static_cast<kdu_int32>(row[x]);
    } else {
      p32[x].fval = static_cast<float>(row[x] * inv_scale);
    }
  }
}

void UnpackLine(kdu_line_buf& line, int64_t* dst, int width, int bit_depth,
                bool reversible) {
  const double scale = std::ldexp(1.0, bit_depth);
  const double fix_inv = scale / (1 << KDU_FIX_POINT);
  if (const kdu_sample16* p16 = line.get_buf16()) {
    for (int x = 0; x < width; ++x) {
      dst[x] = reversible
                   ? static_cast<int64_t>(p16[x].ival)
                   : static_cast<int64_t>(std::llround(p16[x].ival * fix_inv));
    }
    return;
  }
  if (const kdu_sample64* p64 = line.get_buf64()) {
    for (int x = 0; x < width; ++x) {
      dst[x] = static_cast<int64_t>(p64[x].ival);
    }
    return;
  }
  const kdu_sample32* p32 = line.get_buf32();
  ASSERT_NE(p32, nullptr);
  for (int x = 0; x < width; ++x) {
    dst[x] = reversible
                 ? static_cast<int64_t>(p32[x].ival)
                 : static_cast<int64_t>(std::llround(p32[x].fval * scale));
  }
}

void InitSingleCompCodestream(ScopedCodestream& cs, kdu_compressed_target* tgt,
                              int width, int height, int origin_x,
                              int bit_depth) {
  cs.siz.set(Scomponents, 0, 0, 1);
  cs.siz.set(Sorigin, 0, 0, 0);
  cs.siz.set(Sorigin, 0, 1, origin_x);
  cs.siz.set(Ssize, 0, 0, height);
  cs.siz.set(Ssize, 0, 1, origin_x + width);
  cs.siz.set(Sdims, 0, 0, height);
  cs.siz.set(Sdims, 0, 1, width);
  cs.siz.set(Ssampling, 0, 0, 1);
  cs.siz.set(Ssampling, 0, 1, 1);
  cs.siz.set(Sprecision, 0, 0, bit_depth);
  cs.siz.set(Ssigned, 0, 0, true);
  static_cast<kdu_params*>(&cs.siz)->finalize();
  cs.codestream.create(&cs.siz, tgt);
}

struct DirectSpec {
  int width = 0;
  int height = 0;
  int origin_x = 0;
  int bit_depth = 8;
  bool reversible = true;
  kdu_byte sample_prec = KDU_SAMPLE_PREC16;
  std::vector<std::string> params;
};

std::vector<kdu_byte> DirectEncodeSubband(const DirectSpec& spec,
                                          const std::vector<int64_t>& samples) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  InitSingleCompCodestream(cs, &target, spec.width, spec.height, spec.origin_x,
                           spec.bit_depth);

  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(spec.reversible ? "Creversible=yes"
                                                   : "Creversible=no"));
  EXPECT_TRUE(params->parse_string("Clevels=0"));
  for (const std::string& p : spec.params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();

  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_subband band =
        tile.tile.access_component(0).access_resolution(0).access_subband(
            LL_BAND);

    kdu_sample_allocator alloc;
    kdu_push_pull_params pp_params;
    pp_params.set_opt_flags(KDU_BLOCK_CODING_ACCELERATE);
    kdu_encoder enc(band, &alloc, pp_params, spec.sample_prec);
    kdu_line_buf line;
    line.pre_create(&alloc, spec.width,
                    LineFlagForPrec(spec.sample_prec, spec.reversible), 0, 0);
    alloc.finalize(cs.codestream);
    line.create();
    enc.start(nullptr);

    for (int y = 0; y < spec.height; ++y) {
      PackLine(line, samples.data() + static_cast<size_t>(y) * spec.width,
               spec.width, spec.bit_depth, spec.reversible);
      EXPECT_TRUE(enc.push(line, nullptr));
    }
    EXPECT_TRUE(enc.finish(nullptr));
    enc.destroy();
    line.destroy();
  }
  kdu_long limit = 0;
  cs.codestream.flush(&limit, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

std::vector<int64_t> DirectDecodeSubband(const std::vector<kdu_byte>& bitstream,
                                         const DirectSpec& spec) {
  VectorSource src(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&src);
  kdu_dims dims;
  cs.codestream.get_dims(0, dims);
  std::vector<int64_t> out(static_cast<size_t>(dims.size.x) * dims.size.y);

  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_subband band =
        tile.tile.access_component(0).access_resolution(0).access_subband(
            LL_BAND);

    kdu_sample_allocator alloc;
    kdu_push_pull_params pp_params;
    pp_params.set_opt_flags(KDU_BLOCK_CODING_ACCELERATE);
    kdu_decoder dec(band, &alloc, pp_params, spec.sample_prec);
    kdu_line_buf line;
    line.pre_create(&alloc, dims.size.x,
                    LineFlagForPrec(spec.sample_prec, spec.reversible), 0, 0);
    alloc.finalize(cs.codestream);
    line.create();
    dec.start(nullptr);

    for (int y = 0; y < dims.size.y; ++y) {
      dec.pull(line, nullptr);
      UnpackLine(line, out.data() + static_cast<size_t>(y) * dims.size.x,
                 dims.size.x, spec.bit_depth, spec.reversible);
    }
    dec.destroy();
    line.destroy();
  }
  cs.codestream.destroy();
  return out;
}

TEST(NeonCoderLocalTest, ReversibleAndLowKmax16_32_40BitDecoderFallbacks) {
  struct Case {
    int width;
    int height;
    int origin_x;
    int bit_depth;
    kdu_byte prec;
    int64_t scale_bias;
    const char* cblk;
  };
  const Case cases[] = {
      // Low K_max <= 15 (16-bit internal code-block sample storage on NEON).
      {21, 12, 3, 6, KDU_SAMPLE_PREC16, 0, "Cblk={8,16}"},
      {73, 18, 0, 8, KDU_SAMPLE_PREC16, 0, "Cblk={8,32}"},
      // 32-bit internal code-block storage across aligned and unaligned
      // stripes.
      {73, 18, 5, 16, KDU_SAMPLE_PREC32, int64_t{1} << 13, "Cblk={8,32}"},
      // 40-bit storage (`pull64` ext_ignorable true and false).
      {41, 16, 3, 12, KDU_SAMPLE_PREC64, 0, "Cblk={8,16}"},
      {64, 16, 5, 35, KDU_SAMPLE_PREC64, int64_t{1} << 33, "Cblk={8,32}"},
  };

  for (const Case& c : cases) {
    for (bool sparse : {false, true}) {
      DirectSpec spec;
      spec.width = c.width;
      spec.height = c.height;
      spec.origin_x = c.origin_x;
      spec.bit_depth = c.bit_depth;
      spec.reversible = true;
      spec.sample_prec = c.prec;
      spec.params = {c.cblk};

      const std::vector<int64_t> src =
          MakeSamples(c.width, c.height, c.bit_depth, sparse, c.scale_bias);
      const std::vector<kdu_byte> bs = DirectEncodeSubband(spec, src);
      ASSERT_FALSE(bs.empty());

      const std::vector<int64_t> dst = DirectDecodeSubband(bs, spec);
      EXPECT_EQ(dst, src) << "width=" << c.width << " bit_depth=" << c.bit_depth
                          << " sparse=" << sparse;
    }
  }
}

TEST(NeonCoderLocalTest, Irreversible16And32BitDecoderFallbacks) {
  for (int width : {19, 64, 96}) {
    for (int origin_x : {0, 5}) {
      for (kdu_byte prec : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
        DirectSpec spec;
        spec.width = width;
        spec.height = 18;
        spec.origin_x = origin_x;
        spec.bit_depth = 8;
        spec.reversible = false;
        spec.sample_prec = prec;
        spec.params = {"Ckernels=W9X7", "Cblk={8,32}", "Qstep=0.0005"};

        const std::vector<int64_t> src =
            MakeSamples(spec.width, spec.height, spec.bit_depth, false, 0);
        const std::vector<kdu_byte> bs = DirectEncodeSubband(spec, src);
        ASSERT_FALSE(bs.empty());

        const std::vector<int64_t> dst = DirectDecodeSubband(bs, spec);
        ASSERT_EQ(dst.size(), src.size());
        int64_t max_err = 0;
        for (size_t i = 0; i < src.size(); ++i) {
          max_err = std::max(max_err, std::abs(dst[i] - src[i]));
        }
        EXPECT_LE(max_err, 5) << "width=" << width << " origin_x=" << origin_x
                              << " prec=" << static_cast<int>(prec);
      }
    }
  }
}

TEST(NeonCoderLocalTest, MultiTransformLinePairAndCplexOverflowFallbacks) {
  struct XformCase {
    int width;
    int height;
    int origin_x;
    int bit_depth;
    bool reversible;
    int xform_flags;
    const char* cblk;
    int64_t scale_bias;
  };
  const XformCase cases[] = {
      // 16_16_rev & 16_16_irv with CPLEX counter-overflow geometry
      // (Cblk={4,64}).
      {128, 72, 3, 8, true, KDU_MULTI_XFORM_DEFAULT_FLAGS, "Cblk={4,64}", 0},
      {128, 72, 3, 8, false, KDU_MULTI_XFORM_DEFAULT_FLAGS, "Cblk={4,64}", 0},
      // 16_32 & 32_32 paths.
      {96, 68, 5, 16, true, KDU_MULTI_XFORM_PRECISE, "Cblk={8,16}",
       int64_t{1} << 13},
      {96, 68, 5, 10, false, KDU_MULTI_XFORM_PRECISE, "Cblk={8,16}", 0},
      // 64_40_rev path (high dynamic range, odd origin & height).
      {85, 65, 3, 30, true, KDU_MULTI_XFORM_PRECISE, "Cblk={4,64}",
       int64_t{1} << 27},
  };

  for (const XformCase& c : cases) {
    const std::vector<int64_t> src =
        MakeSamples(c.width, c.height, c.bit_depth, false, c.scale_bias);
    std::vector<kdu_byte> bitstream;
    {
      VectorTarget target(bitstream);
      ScopedCodestream cs;
      InitSingleCompCodestream(cs, &target, c.width, c.height, c.origin_x,
                               c.bit_depth);

      siz_params* params = cs.codestream.access_siz();
      EXPECT_TRUE(params->parse_string(c.reversible ? "Creversible=yes"
                                                    : "Creversible=no"));
      EXPECT_TRUE(params->parse_string(c.reversible ? "Ckernels=W5X3"
                                                    : "Ckernels=W9X7"));
      EXPECT_TRUE(params->parse_string("Clevels=1"));
      EXPECT_TRUE(params->parse_string(c.cblk));
      if (!c.reversible) {
        EXPECT_TRUE(params->parse_string("Qstep=0.0005"));
      }
      params->finalize_all();

      {
        ScopedTile tile;
        tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
        kdu_multi_analysis engine;
        engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                      c.xform_flags);
        for (int y = 0; y < c.height; ++y) {
          kdu_line_buf* line = engine.exchange_line(0, nullptr, nullptr);
          ASSERT_NE(line, nullptr);
          PackLine(*line, src.data() + static_cast<size_t>(y) * c.width,
                   c.width, c.bit_depth, c.reversible);
          engine.exchange_line(0, line, nullptr);
        }
        engine.destroy();
      }
      kdu_long limit = 0;
      cs.codestream.flush(&limit, 1, nullptr, true, true);
      cs.codestream.destroy();
    }

    VectorSource vsrc(bitstream);
    ScopedCodestream dcs;
    dcs.codestream.create(&vsrc);
    std::vector<int64_t> out(src.size());
    {
      ScopedTile tile;
      tile.tile = dcs.codestream.open_tile(kdu_coords(0, 0), nullptr);
      kdu_multi_synthesis engine;
      engine.create(dcs.codestream, tile.tile, nullptr, nullptr, c.xform_flags);
      for (int y = 0; y < c.height; ++y) {
        kdu_line_buf* line = engine.get_line(0, nullptr);
        ASSERT_NE(line, nullptr);
        UnpackLine(*line, out.data() + static_cast<size_t>(y) * c.width,
                   c.width, c.bit_depth, c.reversible);
      }
      engine.destroy();
    }
    dcs.codestream.destroy();

    if (c.reversible) {
      EXPECT_EQ(out, src) << "bit_depth=" << c.bit_depth << " cblk=" << c.cblk;
    } else {
      int64_t max_err = 0;
      for (size_t i = 0; i < src.size(); ++i) {
        max_err = std::max(max_err, std::abs(out[i] - src[i]));
      }
      EXPECT_LE(max_err, 8)
          << "bit_depth=" << c.bit_depth << " cblk=" << c.cblk;
    }
  }
}

}  // namespace
}  // namespace kdu_core
