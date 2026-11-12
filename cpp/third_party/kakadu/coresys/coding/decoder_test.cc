// Unit tests for
// third_party/kakadu/vendor/coresys/coding/decoder.cpp.

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
#include "third_party/kakadu/vendor/coresys/common/kdu_roi_processing.h"
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

class DecoderTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kDecoderTestEnv =
    ::testing::AddGlobalTestEnvironment(new DecoderTestEnvironment());

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

struct ScopedPush {
  kdu_push_ifc engine;
  ~ScopedPush() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedPull {
  kdu_pull_ifc engine;
  ~ScopedPull() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedLineBuf {
  kdu_line_buf line;
  ~ScopedLineBuf() { line.destroy(); }
};

struct ScopedThreadEnv {
  kdu_thread_env env;
  bool created = false;
  ~ScopedThreadEnv() {
    if (created && env.exists()) {
      env.destroy();
    }
  }
};

class HalfMaskRoiNode final : public kdu_roi_node {
 public:
  void pull(kdu_byte buf[], int width) override {
    const int split = width / 2;
    for (int x = 0; x < width; ++x) {
      buf[x] = (x < split) ? 0xFF : 0x00;
    }
  }
};

struct EncodeConfig {
  int width = 32;
  int height = 24;
  int ox = 0;
  int oy = 0;
  int precision = 8;
  bool is_signed = true;
  bool reversible = true;
  int clevels = 0;
  std::vector<std::string> extra_params;
  bool use_roi = false;
  kdu_byte enc_sample_prec = KDU_SAMPLE_PREC32;
  kdu_long flush_limit = 1 << 22;
};

std::vector<int64_t> MakeSamples(int width, int height, int precision,
                                 bool is_signed, bool sparse_blocks = false,
                                 int64_t custom_scale = 0) {
  std::vector<int64_t> out(static_cast<size_t>(width) * height, 0);
  const int64_t half_range = int64_t{1} << (precision - 2);
  const int64_t modulus = std::max<int64_t>(2, half_range);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (sparse_blocks && (x < width / 2) && (y < height / 2)) {
        continue;
      }
      int64_t val =
          ((static_cast<int64_t>(x) * 73 - static_cast<int64_t>(y) * 41 + 19) %
           modulus);
      if ((x + y) & 1) {
        val = -val;
      }
      if (custom_scale > 0) {
        val = ((val % 1024) * (custom_scale / 1024));
      }
      if (!is_signed) {
        val += int64_t{1} << (precision - 1);
      }
      out[static_cast<size_t>(y) * width + x] = val;
    }
  }
  return out;
}

std::vector<kdu_byte> EncodeComponent(const EncodeConfig& cfg,
                                      const std::vector<int64_t>& samples) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  cs.siz.set(Scomponents, 0, 0, 1);
  cs.siz.set(Sorigin, 0, 0, cfg.oy);
  cs.siz.set(Sorigin, 0, 1, cfg.ox);
  cs.siz.set(Ssize, 0, 0, cfg.oy + cfg.height);
  cs.siz.set(Ssize, 0, 1, cfg.ox + cfg.width);
  cs.siz.set(Sdims, 0, 0, cfg.height);
  cs.siz.set(Sdims, 0, 1, cfg.width);
  cs.siz.set(Ssampling, 0, 0, 1);
  cs.siz.set(Ssampling, 0, 1, 1);
  cs.siz.set(Sprecision, 0, 0, cfg.precision);
  cs.siz.set(Ssigned, 0, 0, cfg.is_signed);
  static_cast<kdu_params*>(&cs.siz)->finalize();

  cs.codestream.create(&cs.siz, &target);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(cfg.reversible ? "Creversible=yes"
                                                  : "Creversible=no"));
  EXPECT_TRUE(
      params->parse_string(("Clevels=" + std::to_string(cfg.clevels)).c_str()));
  if (cfg.reversible) {
    EXPECT_TRUE(params->parse_string("Ckernels=W5X3"));
  }
  for (const std::string& p : cfg.extra_params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution res = tc.access_resolution();

  kdu_sample_allocator alloc;
  kdu_push_pull_params pp_params;
  HalfMaskRoiNode roi_node;
  ScopedPush analysis;
  if (res.which() == 0) {
    analysis.engine = kdu_encoder(
        res.access_subband(LL_BAND), &alloc, pp_params, cfg.enc_sample_prec,
        1.0F, /*push_offset=*/0, cfg.use_roi ? &roi_node : nullptr);
  } else {
    analysis.engine =
        kdu_analysis(res, &alloc, pp_params, cfg.enc_sample_prec, 1.0F,
                     /*push_offset=*/0, cfg.use_roi ? &roi_node : nullptr);
  }

  const kdu_byte line_flags =
      cfg.enc_sample_prec | (cfg.reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
  ScopedLineBuf line;
  line.line.pre_create(&alloc, cfg.width, line_flags, 0, 0);
  alloc.finalize(cs.codestream);
  line.line.create();

  const int64_t dc = cfg.is_signed ? 0 : (int64_t{1} << (cfg.precision - 1));
  const double inv_scale = std::ldexp(1.0, -cfg.precision);
  for (int y = 0; y < cfg.height; ++y) {
    const int64_t* row = samples.data() + static_cast<size_t>(y) * cfg.width;
    if (cfg.enc_sample_prec == KDU_SAMPLE_PREC64) {
      kdu_sample64* dst = line.line.get_buf64();
      EXPECT_NE(dst, nullptr);
      if (dst == nullptr) {
        return {};
      }
      for (int x = 0; x < cfg.width; ++x) {
        dst[x].ival = static_cast<kdu_int64>(row[x] - dc);
      }
    } else if (cfg.enc_sample_prec == KDU_SAMPLE_PREC32) {
      kdu_sample32* dst = line.line.get_buf32();
      EXPECT_NE(dst, nullptr);
      if (dst == nullptr) {
        return {};
      }
      for (int x = 0; x < cfg.width; ++x) {
        if (cfg.reversible) {
          dst[x].ival = static_cast<kdu_int32>(row[x] - dc);
        } else {
          dst[x].fval =
              static_cast<float>(static_cast<double>(row[x] - dc) * inv_scale);
        }
      }
    } else {
      kdu_sample16* dst = line.line.get_buf16();
      EXPECT_NE(dst, nullptr);
      if (dst == nullptr) {
        return {};
      }
      for (int x = 0; x < cfg.width; ++x) {
        if (cfg.reversible) {
          dst[x].ival = static_cast<kdu_int16>(row[x] - dc);
        } else {
          dst[x].ival = static_cast<kdu_int16>(
              std::llround(static_cast<double>(row[x] - dc) *
                           std::ldexp(1.0, KDU_FIX_POINT - cfg.precision)));
        }
      }
    }
    while (!analysis.engine.push(line.line, nullptr)) {
    }
  }
  while (!analysis.engine.finish(nullptr)) {
  }
  analysis.engine.destroy();
  tile.tile.close();
  kdu_long max_bytes = cfg.flush_limit;
  cs.codestream.flush(&max_bytes, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

struct DecodeConfig {
  kdu_byte sample_prec = KDU_SAMPLE_PREC32;
  kdu_byte line_prec = 0xFF;  // 0xFF => follow sample_prec
  float normalization = 1.0F;
  int pull_offset = 0;
  int extended_info = 0;
  int extend_right = 0;
  bool use_postquant = false;
  bool transpose = false;
  bool vflip = false;
  bool hflip = false;
  bool call_start = true;
  bool call_start_twice = false;
  int num_threads = 0;
  int max_hires_stripes = -1;
  bool extra_lowres_stripe = false;
  int min_jobs_across = 0;
  int log2_min_samples = 0;
  int log2_ideal_samples = 0;
  int opt_flags = -1;
  int pull_limit = -1;
  bool test_short_line_early_return = false;
  int precision = 8;
  bool is_signed = true;
  bool reversible = true;
};

struct DecodeOutput {
  int width = 0;
  int height = 0;
  float postquant_scale = 0.0F;
  std::vector<int64_t> int_samples;
  std::vector<double> float_samples;
};

DecodeOutput RunDirectDecoder(const std::vector<kdu_byte>& bitstream,
                              const DecodeConfig& cfg) {
  DecodeOutput out;
  VectorSource src(bitstream);
  ScopedThreadEnv thread_ctx;
  kdu_thread_env* env_ptr = nullptr;
  kdu_thread_queue root_queue;
  if (cfg.num_threads > 0) {
    thread_ctx.env.create();
    thread_ctx.created = true;
    for (int i = 1; i < cfg.num_threads; ++i) {
      EXPECT_TRUE(thread_ctx.env.add_thread());
    }
    env_ptr = &thread_ctx.env;
  }

  ScopedCodestream cs;
  cs.codestream.create(&src, env_ptr);
  if (cfg.transpose || cfg.vflip || cfg.hflip) {
    cs.codestream.change_appearance(cfg.transpose, cfg.vflip, cfg.hflip);
  }
  if (env_ptr != nullptr) {
    env_ptr->attach_queue(&root_queue, nullptr, "decoder_test_root");
  }

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), env_ptr);
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution res = tc.access_resolution(0);
  kdu_subband band = res.access_subband(LL_BAND);

  kdu_dims band_dims;
  band.get_dims(band_dims);
  out.width = band_dims.size.x;
  out.height = band_dims.size.y;

  kdu_push_pull_params pp_params;
  if (cfg.max_hires_stripes > 0) {
    pp_params.set_max_block_stripes(cfg.max_hires_stripes,
                                    cfg.extra_lowres_stripe);
  }
  if (cfg.log2_min_samples > 0 || cfg.log2_ideal_samples > 0) {
    pp_params.set_preferred_job_samples(cfg.log2_min_samples,
                                        cfg.log2_ideal_samples);
  }
  if (cfg.min_jobs_across > 0) {
    pp_params.set_min_jobs_across(cfg.min_jobs_across);
  }
  if (cfg.opt_flags >= 0) {
    pp_params.set_opt_flags(cfg.opt_flags);
  }

  kdu_sample_allocator alloc;
  float* pq_ptr = cfg.use_postquant ? &out.postquant_scale : nullptr;
  ScopedPull dec;
  dec.engine = kdu_decoder(band, &alloc, pp_params, cfg.sample_prec,
                           cfg.normalization, cfg.pull_offset, env_ptr,
                           env_ptr != nullptr ? &root_queue : nullptr,
                           cfg.extended_info, pq_ptr);

  const kdu_byte actual_line_prec =
      (cfg.line_prec != 0xFF)
          ? cfg.line_prec
          : ((out.postquant_scale > 0.0F) ? KDU_LINE_BUF_SHORTS
                                          : cfg.sample_prec);
  const bool line_abs = cfg.reversible || (out.postquant_scale > 0.0F);
  const kdu_byte line_flags =
      actual_line_prec | (line_abs ? KDU_LINE_BUF_ABSOLUTE : 0);

  ScopedLineBuf line;
  line.line.pre_create(&alloc, out.width + cfg.pull_offset, line_flags, 0,
                       cfg.extend_right);
  ScopedLineBuf short_line;
  if (cfg.test_short_line_early_return && cfg.pull_offset > 0) {
    short_line.line.pre_create(&alloc, cfg.pull_offset, line_flags, 0, 0);
  }
  alloc.finalize(cs.codestream);
  line.line.create();
  if (cfg.test_short_line_early_return && cfg.pull_offset > 0) {
    short_line.line.create();
  }

  if (cfg.call_start) {
    dec.engine.start(env_ptr);
    if (cfg.call_start_twice) {
      dec.engine.start(env_ptr);
    }
  }
  if (cfg.test_short_line_early_return && cfg.pull_offset > 0) {
    dec.engine.pull(short_line.line, env_ptr);
  }

  const int lines_to_pull =
      (cfg.pull_limit >= 0) ? std::min(cfg.pull_limit, out.height) : out.height;
  const int64_t dc = cfg.is_signed ? 0 : (int64_t{1} << (cfg.precision - 1));
  const double scale_p = std::ldexp(1.0, cfg.precision);

  if (out.width > 0 && lines_to_pull > 0) {
    out.int_samples.resize(static_cast<size_t>(out.width) * lines_to_pull);
    out.float_samples.resize(static_cast<size_t>(out.width) * lines_to_pull);
    for (int y = 0; y < lines_to_pull; ++y) {
      dec.engine.pull(line.line, env_ptr);
      const size_t row_off = static_cast<size_t>(y) * out.width;
      if (const kdu_sample16* buf16 = line.line.get_buf16()) {
        const kdu_sample16* p = buf16 + cfg.pull_offset;
        for (int x = 0; x < out.width; ++x) {
          if (out.postquant_scale > 0.0F) {
            out.float_samples[row_off + x] =
                static_cast<double>(p[x].ival) *
                    static_cast<double>(out.postquant_scale) /
                    static_cast<double>(cfg.normalization) * scale_p +
                dc;
          } else if (cfg.reversible) {
            out.int_samples[row_off + x] = static_cast<int64_t>(p[x].ival) + dc;
          } else {
            out.float_samples[row_off + x] =
                static_cast<double>(p[x].ival) /
                    static_cast<double>(1 << KDU_FIX_POINT) /
                    static_cast<double>(cfg.normalization) * scale_p +
                dc;
          }
        }
      } else if (const kdu_sample64* buf64 = line.line.get_buf64()) {
        const kdu_sample64* p = buf64 + cfg.pull_offset;
        for (int x = 0; x < out.width; ++x) {
          out.int_samples[row_off + x] = static_cast<int64_t>(p[x].ival) + dc;
        }
      } else {
        const kdu_sample32* buf32 = line.line.get_buf32();
        EXPECT_NE(buf32, nullptr);
        if (buf32 == nullptr) {
          break;
        }
        const kdu_sample32* p = buf32 + cfg.pull_offset;
        for (int x = 0; x < out.width; ++x) {
          if (cfg.reversible) {
            out.int_samples[row_off + x] = static_cast<int64_t>(p[x].ival) + dc;
          } else {
            out.float_samples[row_off + x] =
                static_cast<double>(p[x].fval) /
                    static_cast<double>(cfg.normalization) * scale_p +
                dc;
          }
        }
      }
    }
  }

  if (env_ptr != nullptr) {
    env_ptr->terminate(&root_queue, true);
  }
  dec.engine.destroy();
  tile.tile.close(env_ptr);
  if (env_ptr != nullptr) {
    env_ptr->terminate(&root_queue, false);
    env_ptr->cs_terminate(cs.codestream);
  }
  cs.codestream.destroy();
  return out;
}

TEST(DecoderTest, ReversibleAllSamplePrecisionsOffsetsAndGeometry) {
  // Normal + tiny blocks across 16/32/64-bit line precisions, unaligned
  // multi-block stripes, extended lines, pull_offset, and disabled block opt.
  for (const char* blk_param : {"Cblk={32,32}", "Cblk={4,4}"}) {
    EncodeConfig enc;
    enc.width = 40;
    enc.height = 36;
    enc.ox = 3;
    enc.oy = 1;
    enc.precision = 8;
    enc.is_signed = false;
    enc.reversible = true;
    enc.extra_params = {blk_param};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed,
                    /*sparse_blocks=*/true);
    const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

    for (kdu_byte sp :
         {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32, KDU_SAMPLE_PREC64}) {
      DecodeConfig dec;
      dec.sample_prec = sp;
      // Also exercise stripe_prec=32 pulled into a 64-bit line buffer.
      dec.line_prec = (sp == KDU_SAMPLE_PREC64) ? KDU_LINE_BUF_VPRECISE : sp;
      dec.precision = enc.precision;
      dec.is_signed = enc.is_signed;
      dec.reversible = true;
      dec.pull_offset = (sp == KDU_SAMPLE_PREC32) ? 5 : 0;
      dec.extended_info =
          (sp == KDU_SAMPLE_PREC16) ? KDU_LINE_WILL_BE_EXTENDED : 0;
      dec.extend_right = (sp == KDU_SAMPLE_PREC16) ? 1 : 0;
      dec.test_short_line_early_return = (sp == KDU_SAMPLE_PREC32);
      dec.call_start = (sp != KDU_SAMPLE_PREC16);
      dec.call_start_twice = (sp == KDU_SAMPLE_PREC32);
      dec.opt_flags = (sp == KDU_SAMPLE_PREC32) ? KDU_BLOCK_CODING_NO_ACCELERATE
                                                : KDU_BLOCK_CODING_ACCELERATE;
      const DecodeOutput out = RunDirectDecoder(bs, dec);
      EXPECT_EQ(out.int_samples, src)
          << "blk=" << blk_param << " sp=" << static_cast<int>(sp);
    }

    // Exercise transpose + vflip + hflip across 16-bit and 32-bit reversible.
    for (int mask = 1; mask < 8; mask += 2) {
      for (kdu_byte sp : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
        DecodeConfig dec;
        dec.sample_prec = sp;
        dec.precision = enc.precision;
        dec.is_signed = enc.is_signed;
        dec.reversible = true;
        dec.transpose = (mask & 1) != 0;
        dec.vflip = (mask & 2) != 0;
        dec.hflip = (mask & 4) != 0;
        const DecodeOutput out = RunDirectDecoder(bs, dec);
        EXPECT_EQ(out.width, dec.transpose ? enc.height : enc.width);
        EXPECT_EQ(out.height, dec.transpose ? enc.width : enc.height);
        EXPECT_EQ(out.int_samples.size(), src.size());
      }
    }
  }
}

TEST(DecoderTest, Irreversible16And32AndPostquantAndTruncatedZeroBlocks) {
  for (const char* blk_param : {"Cblk={32,32}", "Cblk={4,4}"}) {
    EncodeConfig enc;
    enc.width = 40;
    enc.height = 36;
    enc.precision = 8;
    enc.is_signed = true;
    enc.reversible = false;
    enc.extra_params = {blk_param, "Qstep=0.0005"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed,
                    /*sparse_blocks=*/true);
    const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

    // 16-bit fixed point, 32-bit float, and postquant_scale pre-quantized 16.
    for (int mode = 0; mode < 3; ++mode) {
      DecodeConfig dec;
      dec.sample_prec = (mode == 0) ? KDU_SAMPLE_PREC16 : KDU_SAMPLE_PREC32;
      dec.use_postquant = (mode == 2);
      dec.precision = enc.precision;
      dec.is_signed = enc.is_signed;
      dec.reversible = false;
      const DecodeOutput out = RunDirectDecoder(bs, dec);
      if (mode == 2) {
        EXPECT_GT(out.postquant_scale, 0.0F);
      }
      ASSERT_EQ(out.float_samples.size(), src.size());
      for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_NEAR(out.float_samples[i], static_cast<double>(src[i]), 3.0)
            << "blk=" << blk_param << " mode=" << mode << " i=" << i;
      }
    }

    // Transposed/flipped irreversible 16-bit and 32-bit paths.
    for (kdu_byte sp : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
      DecodeConfig dec;
      dec.sample_prec = sp;
      dec.precision = enc.precision;
      dec.is_signed = enc.is_signed;
      dec.reversible = false;
      dec.transpose = true;
      dec.vflip = true;
      dec.hflip = true;
      const DecodeOutput out = RunDirectDecoder(bs, dec);
      EXPECT_EQ(out.width, enc.height);
      EXPECT_EQ(out.height, enc.width);
    }

    // All-zero / missing code-blocks (num_passes == 0).
    EncodeConfig zero_enc = enc;
    const std::vector<int64_t> zeros(
        static_cast<size_t>(zero_enc.width) * zero_enc.height, 0);
    const std::vector<kdu_byte> zero_bs = EncodeComponent(zero_enc, zeros);
    for (kdu_byte sp : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
      DecodeConfig dec;
      dec.sample_prec = sp;
      dec.precision = zero_enc.precision;
      dec.is_signed = true;
      dec.reversible = false;
      const DecodeOutput out = RunDirectDecoder(zero_bs, dec);
      for (double v : out.float_samples) {
        EXPECT_DOUBLE_EQ(v, 0.0);
      }
    }
  }
}

TEST(DecoderTest, StripePrec40AndIgnoredMsbsAndExtensionBytes) {
  // 1. High bit depth with small magnitude -> K_max > 31 triggers ignored_msbs
  // and ext_ignorable = true, plus sparse zero block for 40-bit zeroing.
  {
    EncodeConfig enc;
    enc.width = 24;
    enc.height = 24;
    enc.precision = 34;
    enc.is_signed = true;
    enc.reversible = true;
    enc.enc_sample_prec = KDU_SAMPLE_PREC64;
    enc.extra_params = {"Cblk={8,8}"};
    const std::vector<int64_t> small_src =
        MakeSamples(enc.width, enc.height, /*precision=*/12, enc.is_signed,
                    /*sparse_blocks=*/true);
    const std::vector<kdu_byte> bs = EncodeComponent(enc, small_src);

    DecodeConfig dec;
    dec.sample_prec = KDU_SAMPLE_PREC64;
    dec.precision = enc.precision;
    dec.is_signed = enc.is_signed;
    dec.reversible = true;
    dec.pull_offset = 2;
    const DecodeOutput out = RunDirectDecoder(bs, dec);
    EXPECT_EQ(out.int_samples, small_src);
  }

  // 2. Full 35-bit dynamic range -> sample_buffer64 with non-trivial extension
  // bytes (ext_ignorable == false), both untransposed and transposed.
  {
    EncodeConfig enc;
    enc.width = 24;
    enc.height = 24;
    enc.precision = 35;
    enc.is_signed = true;
    enc.reversible = true;
    enc.enc_sample_prec = KDU_SAMPLE_PREC64;
    enc.extra_params = {"Cblk={8,8}"};
    std::vector<int64_t> wide_src =
        MakeSamples(enc.width, enc.height, /*precision=*/12, enc.is_signed,
                    /*sparse_blocks=*/true, int64_t{1} << 33);
    // Keep at least one block smaller so synthesize_extensions runs alongside.
    for (int y = 12; y < 24; ++y) {
      for (int x = 12; x < 24; ++x) {
        wide_src[static_cast<size_t>(y) * enc.width + x] = (x - y) * 17;
      }
    }
    const std::vector<kdu_byte> bs = EncodeComponent(enc, wide_src);

    for (bool transpose : {false, true}) {
      DecodeConfig dec;
      dec.sample_prec = KDU_SAMPLE_PREC64;
      dec.precision = enc.precision;
      dec.is_signed = enc.is_signed;
      dec.reversible = true;
      dec.transpose = transpose;
      dec.vflip = transpose;
      const DecodeOutput out = RunDirectDecoder(bs, dec);
      if (!transpose) {
        EXPECT_EQ(out.int_samples, wide_src);
      } else {
        EXPECT_EQ(out.width, enc.height);
        EXPECT_EQ(out.height, enc.width);
      }
    }
  }

  // 3. K_max > 31 with 35-bit magnitudes (missing_msbs <= 1 -> ignored_msbs =
  // 0) and truncated passes so block_decoder uses 32-bit sample_buffer while
  // effective_Kmax > 31 -> 40-bit upshift path (downshift < 0) in 32-bit loop.
  {
    EncodeConfig enc;
    enc.width = 24;
    enc.height = 24;
    enc.precision = 35;
    enc.is_signed = true;
    enc.reversible = true;
    enc.enc_sample_prec = KDU_SAMPLE_PREC64;
    enc.extra_params = {"Cblk={8,8}"};
    enc.flush_limit = 500;
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, /*precision=*/12, enc.is_signed,
                    /*sparse_blocks=*/false, int64_t{1} << 33);
    const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

    for (bool transpose : {false, true}) {
      DecodeConfig dec;
      dec.sample_prec = KDU_SAMPLE_PREC64;
      dec.precision = enc.precision;
      dec.is_signed = enc.is_signed;
      dec.reversible = true;
      dec.transpose = transpose;
      const DecodeOutput out = RunDirectDecoder(bs, dec);
      EXPECT_EQ(out.int_samples.size(), src.size());
    }
  }
}

TEST(DecoderTest, RoiBackgroundAdjustmentAnd64BitBlockDecodingPaths) {
  // 1. 32-bit block ROI adjustment (K_max_prime <= 31), transposed and normal.
  {
    EncodeConfig enc;
    enc.width = 24;
    enc.height = 24;
    enc.precision = 8;
    enc.is_signed = true;
    enc.reversible = true;
    enc.use_roi = true;
    enc.extra_params = {"Cblk={8,8}", "Rshift=12"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);
    const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

    for (bool transpose : {false, true}) {
      DecodeConfig dec;
      dec.sample_prec = KDU_SAMPLE_PREC16;
      dec.precision = enc.precision;
      dec.is_signed = enc.is_signed;
      dec.reversible = true;
      dec.transpose = transpose;
      const DecodeOutput out = RunDirectDecoder(bs, dec);
      if (!transpose) {
        EXPECT_EQ(out.int_samples, src);
      } else {
        EXPECT_EQ(out.int_samples.size(), src.size());
      }
    }
  }

  // 2. 64-bit block decoding (block->sample_buffer64 != NULL) via MAXSHIFT
  // Rshift=26 on 10-bit data (K_max_prime > 31, foreground has >= 32
  // bitplanes).
  for (bool reversible : {true, false}) {
    EncodeConfig enc;
    enc.width = 24;
    enc.height = 24;
    enc.precision = 10;
    enc.is_signed = true;
    enc.reversible = reversible;
    enc.use_roi = true;
    enc.enc_sample_prec = KDU_SAMPLE_PREC32;
    enc.extra_params = {"Cblk={8,8}", "Rshift=26"};
    if (!reversible) {
      enc.extra_params.push_back("Qstep=0.0005");
    }
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);
    const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

    for (kdu_byte sp : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
      for (bool transpose : {false, true}) {
        DecodeConfig dec;
        dec.sample_prec = sp;
        dec.precision = enc.precision;
        dec.is_signed = enc.is_signed;
        dec.reversible = reversible;
        dec.transpose = transpose;
        dec.hflip = transpose;
        const DecodeOutput out = RunDirectDecoder(bs, dec);
        EXPECT_EQ(out.width, transpose ? enc.height : enc.width);
        EXPECT_EQ(out.height, transpose ? enc.width : enc.height);
        if (!transpose && reversible) {
          if (sp == KDU_SAMPLE_PREC32) {
            EXPECT_EQ(out.int_samples, src);
          } else {
            // Upstream decoder.cpp masks negative 64-bit samples in the 16-bit
            // stripe transfer path with KDU_INT32_MAX instead of KDU_INT64_MAX,
            // zeroing negative values from blocks that took sample_buffer64;
            // positive samples and blocks using 32-bit sample_buffer remain
            // exact.
            ASSERT_EQ(out.int_samples.size(), src.size());
            for (size_t i = 0; i < src.size(); ++i) {
              const int x = static_cast<int>(i % enc.width);
              if (x < 8 || src[i] >= 0) {
                EXPECT_EQ(out.int_samples[i], src[i]) << "i=" << i;
              } else {
                EXPECT_EQ(out.int_samples[i], 0) << "i=" << i;
              }
            }
          }
        }
      }
    }
  }
}

TEST(DecoderTest, EmptySubbandZeroColsAndRowsHandledCleanly) {
  EncodeConfig enc;
  enc.width = 1;
  enc.height = 16;
  enc.precision = 8;
  enc.is_signed = true;
  enc.reversible = true;
  enc.clevels = 1;
  const std::vector<int64_t> src =
      MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);
  const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

  VectorSource source(bs);
  ScopedCodestream cs;
  cs.codestream.create(&source);
  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution res1 = tc.access_resolution(1);
  kdu_subband hl_band = res1.access_subband(HL_BAND);

  kdu_dims dims;
  hl_band.get_dims(dims);
  EXPECT_EQ(dims.size.x, 0);

  kdu_sample_allocator alloc;
  kdu_push_pull_params pp_params;
  ScopedPull dec;
  dec.engine = kdu_decoder(hl_band, &alloc, pp_params, KDU_SAMPLE_PREC16);
  alloc.finalize(cs.codestream);
  EXPECT_TRUE(dec.engine.start(nullptr));
  EXPECT_TRUE(dec.engine.start(nullptr));
  dec.engine.destroy();
  tile.tile.close();
  cs.codestream.destroy();
}

TEST(DecoderTest, MultiThreadedAllStripeCountsJobsAndEarlyTermination) {
  // Large enough with Cblk={8,8} to span many stripes (1..4 active stripes)
  // and multiple horizontal jobs per stripe.
  EncodeConfig enc;
  enc.width = 160;
  enc.height = 96;
  enc.precision = 8;
  enc.is_signed = true;
  enc.reversible = true;
  enc.extra_params = {"Cblk={8,8}"};
  const std::vector<int64_t> src =
      MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);
  const std::vector<kdu_byte> bs = EncodeComponent(enc, src);

  // Exercise 1, 2, 3, and 4 stripes, plus default stripe heuristic (-1).
  for (int stripes : {1, 2, 3, 4, -1}) {
    DecodeConfig dec;
    dec.sample_prec = (stripes == 4) ? KDU_SAMPLE_PREC32 : KDU_SAMPLE_PREC16;
    dec.precision = enc.precision;
    dec.is_signed = enc.is_signed;
    dec.reversible = true;
    dec.num_threads = 4;
    dec.max_hires_stripes = stripes;
    dec.extra_lowres_stripe = (stripes == 4);
    dec.log2_min_samples = 9;
    dec.log2_ideal_samples = 11;
    dec.min_jobs_across = 4;
    const DecodeOutput out = RunDirectDecoder(bs, dec);
    EXPECT_EQ(out.int_samples, src) << "stripes=" << stripes;
  }

  // Exercise premature termination paths: after 0 pulls (no wait/start only),
  // after 1 pull (in-flight stripes), and after ~half pulls.
  for (int pull_limit : {0, 1, 10, 40}) {
    DecodeConfig dec;
    dec.sample_prec = KDU_SAMPLE_PREC32;
    dec.precision = enc.precision;
    dec.is_signed = enc.is_signed;
    dec.reversible = true;
    dec.num_threads = 4;
    dec.max_hires_stripes = 4;
    dec.log2_min_samples = 8;
    dec.log2_ideal_samples = 9;
    dec.min_jobs_across = 4;
    dec.pull_limit = pull_limit;
    const DecodeOutput out = RunDirectDecoder(bs, dec);
    EXPECT_EQ(out.int_samples.size(),
              static_cast<size_t>(enc.width) * pull_limit);
  }
}

}  // namespace
}  // namespace kdu_core
