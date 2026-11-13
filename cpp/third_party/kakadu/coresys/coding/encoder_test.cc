// Unit tests for
// third_party/kakadu/vendor/coresys/coding/encoder.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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

class EncoderTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kEncoderTestEnv =
    ::testing::AddGlobalTestEnvironment(new EncoderTestEnvironment());

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

struct ScopedMultiAnalysis {
  kdu_multi_analysis engine;
  kdu_thread_env* env = nullptr;
  ~ScopedMultiAnalysis() {
    if (engine.exists()) {
      engine.destroy(env);
    }
  }
};

struct ScopedMultiSynthesis {
  kdu_multi_synthesis engine;
  kdu_thread_env* env = nullptr;
  ~ScopedMultiSynthesis() {
    if (engine.exists()) {
      engine.destroy(env);
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

class HalfMaskRoiImage final : public kdu_roi_image {
 public:
  kdu_roi_node* acquire_node(int /*comp_idx*/,
                             kdu_dims /*comp_region*/) override {
    nodes_.push_back(std::make_unique<HalfMaskRoiNode>());
    return nodes_.back().get();
  }

 private:
  std::vector<std::unique_ptr<HalfMaskRoiNode>> nodes_;
};

std::vector<int64_t> MakeSamples(int width, int height, int precision,
                                 bool is_signed, bool sparse_blocks = false,
                                 int64_t custom_scale = 0) {
  std::vector<int64_t> out(static_cast<size_t>(width) * height, 0);
  const int64_t half_range = int64_t{1} << std::max(1, precision - 2);
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

struct DirectEncodeConfig {
  int width = 32;
  int height = 24;
  int ox = 0;
  int oy = 0;
  int precision = 8;
  bool is_signed = true;
  bool reversible = true;
  kdu_byte sample_prec = KDU_SAMPLE_PREC32;
  int push_offset = 0;
  int extended_info = 0;
  int extend_right = 0;
  bool exchangeable = true;
  bool use_prequant = false;
  bool use_roi = false;
  bool use_cplex = false;
  bool transpose = false;
  bool vflip = false;
  bool hflip = false;
  bool call_start = true;
  bool call_start_twice = false;
  int opt_flags = -1;
  int max_hires_stripes = -1;
  bool extra_lowres_stripe = false;
  int min_jobs_across = 0;
  int log2_min_samples = 0;
  int log2_ideal_samples = 0;
  int num_threads = 0;
  int push_limit = -1;
  kdu_long max_bytes = 0;
  float normalization = 1.0F;
  std::vector<std::string> extra_params;
};

struct DirectEncodeResult {
  std::vector<kdu_byte> bitstream;
  float prequant_scale = 0.0F;
};

DirectEncodeResult RunDirectEncoder(const DirectEncodeConfig& cfg,
                                    const std::vector<int64_t>& samples) {
  DirectEncodeResult res_out;
  VectorTarget target(res_out.bitstream);
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

  cs.codestream.create(&cs.siz, &target, nullptr, 0, 0, env_ptr);
  if (cfg.transpose || cfg.vflip || cfg.hflip) {
    cs.codestream.change_appearance(cfg.transpose, cfg.vflip, cfg.hflip);
  }
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(cfg.reversible ? "Creversible=yes"
                                                  : "Creversible=no"));
  EXPECT_TRUE(params->parse_string("Clevels=0"));
  if (cfg.reversible) {
    EXPECT_TRUE(params->parse_string("Ckernels=W5X3"));
  }
  for (const std::string& p : cfg.extra_params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  if (cfg.max_bytes > 0) {
    cs.codestream.set_max_bytes(cfg.max_bytes);
  }
  params->finalize_all();
  if (env_ptr != nullptr) {
    env_ptr->attach_queue(&root_queue, nullptr, "encoder_test_root");
  }

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), env_ptr);
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution res = tc.access_resolution();
  kdu_subband band = res.access_subband(LL_BAND);

  kdu_dims band_dims;
  band.get_dims(band_dims);

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
  HalfMaskRoiNode local_roi;
  kdu_cplex_analysis cplex;
  if (cfg.use_cplex) {
    cplex.init(res.access_node(), nullptr, nullptr);
  }
  float* pq_ptr = cfg.use_prequant ? &res_out.prequant_scale : nullptr;
  ScopedPush enc;
  enc.engine = kdu_encoder(
      band, &alloc, pp_params, cfg.sample_prec, cfg.normalization,
      cfg.push_offset, cfg.use_roi ? &local_roi : nullptr,
      cfg.use_cplex ? &cplex : nullptr, env_ptr,
      env_ptr != nullptr ? &root_queue : nullptr, cfg.extended_info, pq_ptr);

  const bool prequant_active = res_out.prequant_scale > 0.0F;
  const kdu_byte line_prec =
      prequant_active ? KDU_LINE_BUF_SHORTS : cfg.sample_prec;
  const bool line_abs = cfg.reversible || prequant_active;
  kdu_byte line_flags = line_prec | (line_abs ? KDU_LINE_BUF_ABSOLUTE : 0);
  if (cfg.exchangeable) {
    line_flags |= KDU_LINE_BUF_EXCHANGEABLE;
  }

  ScopedLineBuf line;
  line.line.pre_create(&alloc, band_dims.size.x + cfg.push_offset, line_flags,
                       0, cfg.extend_right);
  alloc.finalize(cs.codestream);
  line.line.create();

  if (cfg.call_start) {
    enc.engine.start(env_ptr);
    if (cfg.call_start_twice) {
      enc.engine.start(env_ptr);
    }
  }

  const int height = band_dims.size.y;
  const int width = band_dims.size.x;
  const int lines_to_push =
      (cfg.push_limit >= 0) ? std::min(cfg.push_limit, height) : height;
  const int64_t dc = cfg.is_signed ? 0 : (int64_t{1} << (cfg.precision - 1));
  const double inv_scale = std::ldexp(1.0, -cfg.precision);

  for (int y = 0; y < lines_to_push; ++y) {
    const int src_y = cfg.vflip ? (height - 1 - y) : y;
    auto sample_at = [&](int x) -> int64_t {
      const int sx = cfg.hflip ? (width - 1 - x) : x;
      if (cfg.transpose) {
        return samples[static_cast<size_t>(sx) * height + src_y];
      }
      return samples[static_cast<size_t>(src_y) * width + sx];
    };

    if (kdu_sample64* dst64 = line.line.get_buf64()) {
      kdu_sample64* dst = dst64 + cfg.push_offset;
      for (int x = 0; x < width; ++x) {
        dst[x].ival = static_cast<kdu_int64>(sample_at(x) - dc);
      }
    } else if (kdu_sample32* dst32 = line.line.get_buf32()) {
      kdu_sample32* dst = dst32 + cfg.push_offset;
      for (int x = 0; x < width; ++x) {
        if (cfg.reversible) {
          dst[x].ival = static_cast<kdu_int32>(sample_at(x) - dc);
        } else {
          dst[x].fval = static_cast<float>(
              static_cast<double>(sample_at(x) - dc) * inv_scale /
              static_cast<double>(cfg.normalization));
        }
      }
    } else {
      kdu_sample16* dst16 = line.line.get_buf16();
      EXPECT_NE(dst16, nullptr);
      if (dst16 == nullptr) {
        break;
      }
      kdu_sample16* dst = dst16 + cfg.push_offset;
      for (int x = 0; x < width; ++x) {
        if (prequant_active) {
          dst[x].ival = static_cast<kdu_int16>(
              std::llround(static_cast<double>(sample_at(x) - dc) * inv_scale *
                           static_cast<double>(res_out.prequant_scale)));
        } else if (cfg.reversible) {
          dst[x].ival = static_cast<kdu_int16>(sample_at(x) - dc);
        } else {
          dst[x].ival = static_cast<kdu_int16>(
              std::llround(static_cast<double>(sample_at(x) - dc) *
                           std::ldexp(1.0, KDU_FIX_POINT - cfg.precision) /
                           static_cast<double>(cfg.normalization)));
        }
      }
    }
    enc.engine.push(line.line, env_ptr);
    if (cfg.use_cplex) {
      cplex.service(env_ptr);
    }
  }

  if (lines_to_push == height) {
    if (cfg.use_cplex) {
      cplex.finish(env_ptr);
    }
    while (!enc.engine.finish(env_ptr)) {
    }
  }
  if (env_ptr != nullptr) {
    if (lines_to_push == height) {
      env_ptr->join(&root_queue, true);
    } else {
      env_ptr->terminate(&root_queue, true);
    }
  }
  enc.engine.destroy();
  tile.tile.close(env_ptr);
  if (env_ptr != nullptr) {
    env_ptr->terminate(&root_queue, false);
    env_ptr->cs_terminate(cs.codestream);
  }
  if (lines_to_push == height && width > 0 && height > 0) {
    cs.codestream.flush(nullptr, 1, nullptr, true, true);
  }
  cs.codestream.destroy();
  return res_out;
}

std::vector<int64_t> DecodeComponentInt(const std::vector<kdu_byte>& bitstream,
                                        int width, int height, int precision,
                                        bool is_signed) {
  std::vector<int64_t> out(static_cast<size_t>(width) * height, 0);
  VectorSource src(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&src);
  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0));
  ScopedMultiSynthesis synth;
  synth.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                      KDU_MULTI_XFORM_VPRECISE);
  const int64_t dc = is_signed ? 0 : (int64_t{1} << (precision - 1));
  for (int y = 0; y < height; ++y) {
    kdu_line_buf* line = synth.engine.get_line(0, nullptr);
    EXPECT_NE(line, nullptr);
    if (line == nullptr) {
      break;
    }
    const size_t row_off = static_cast<size_t>(y) * width;
    if (const kdu_sample64* p64 = line->get_buf64()) {
      for (int x = 0; x < width; ++x) {
        out[row_off + x] = static_cast<int64_t>(p64[x].ival) + dc;
      }
    } else if (const kdu_sample32* p32 = line->get_buf32()) {
      for (int x = 0; x < width; ++x) {
        out[row_off + x] = static_cast<int64_t>(p32[x].ival) + dc;
      }
    } else if (const kdu_sample16* p16 = line->get_buf16()) {
      for (int x = 0; x < width; ++x) {
        out[row_off + x] = static_cast<int64_t>(p16[x].ival) + dc;
      }
    }
  }
  synth.engine.destroy();
  tile.tile.close();
  cs.codestream.destroy();
  return out;
}

std::vector<kdu_byte> RunMultiAnalysis(
    int width, int height, int precision, bool is_signed, bool reversible,
    int clevels, int xform_flags, kdu_long max_bytes, int num_threads,
    const std::vector<std::string>& params_list,
    const std::vector<int64_t>& samples, bool use_roi = false) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedThreadEnv thread_ctx;
  kdu_thread_env* env_ptr = nullptr;
  kdu_thread_queue root_queue;
  if (num_threads > 0) {
    thread_ctx.env.create();
    thread_ctx.created = true;
    for (int i = 1; i < num_threads; ++i) {
      EXPECT_TRUE(thread_ctx.env.add_thread());
    }
    env_ptr = &thread_ctx.env;
  }

  ScopedCodestream cs;
  cs.siz.set(Scomponents, 0, 0, 1);
  cs.siz.set(Sdims, 0, 0, height);
  cs.siz.set(Sdims, 0, 1, width);
  cs.siz.set(Sprecision, 0, 0, precision);
  cs.siz.set(Ssigned, 0, 0, is_signed);
  static_cast<kdu_params*>(&cs.siz)->finalize();

  cs.codestream.create(&cs.siz, &target, nullptr, 0, 0, env_ptr);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(
      params->parse_string(reversible ? "Creversible=yes" : "Creversible=no"));
  EXPECT_TRUE(
      params->parse_string(("Clevels=" + std::to_string(clevels)).c_str()));
  if (reversible) {
    EXPECT_TRUE(params->parse_string("Ckernels=W5X3"));
  }
  for (const std::string& p : params_list) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();
  if (use_roi) {
    cs.codestream.configure_simultaneous_processing_fragments(0, 0, 0);
  }
  if (max_bytes > 0) {
    cs.codestream.set_max_bytes(max_bytes);
  }
  if (env_ptr != nullptr) {
    env_ptr->attach_queue(&root_queue, nullptr, "multi_analysis_root");
  }

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), env_ptr);
  HalfMaskRoiImage roi_img;
  ScopedMultiAnalysis multi;
  multi.env = env_ptr;
  multi.engine.create(cs.codestream, tile.tile, env_ptr,
                      env_ptr != nullptr ? &root_queue : nullptr, xform_flags,
                      use_roi ? &roi_img : nullptr);

  const int64_t dc = is_signed ? 0 : (int64_t{1} << (precision - 1));
  const double inv_scale = std::ldexp(1.0, -precision);
  for (int y = 0; y < height; ++y) {
    kdu_line_buf* line = multi.engine.exchange_line(0, nullptr, env_ptr);
    EXPECT_NE(line, nullptr);
    if (line == nullptr) {
      break;
    }
    const int64_t* row = samples.data() + static_cast<size_t>(y) * width;
    if (kdu_sample64* dst = line->get_buf64()) {
      for (int x = 0; x < width; ++x) {
        dst[x].ival = static_cast<kdu_int64>(row[x] - dc);
      }
    } else if (kdu_sample32* dst32 = line->get_buf32()) {
      for (int x = 0; x < width; ++x) {
        if (reversible) {
          dst32[x].ival = static_cast<kdu_int32>(row[x] - dc);
        } else {
          dst32[x].fval =
              static_cast<float>(static_cast<double>(row[x] - dc) * inv_scale);
        }
      }
    } else {
      kdu_sample16* dst16 = line->get_buf16();
      EXPECT_NE(dst16, nullptr);
      if (dst16 == nullptr) {
        break;
      }
      for (int x = 0; x < width; ++x) {
        if (reversible) {
          dst16[x].ival = static_cast<kdu_int16>(row[x] - dc);
        } else {
          dst16[x].ival = static_cast<kdu_int16>(
              std::llround(static_cast<double>(row[x] - dc) *
                           std::ldexp(1.0, KDU_FIX_POINT - precision)));
        }
      }
    }
    multi.engine.exchange_line(0, line, env_ptr);
  }

  if (env_ptr != nullptr) {
    env_ptr->join(&root_queue, true);
  }
  multi.engine.destroy(env_ptr);
  tile.tile.close(env_ptr);
  if (env_ptr != nullptr) {
    env_ptr->terminate(&root_queue, false);
    env_ptr->cs_terminate(cs.codestream);
  }
  kdu_long flush_limit = (max_bytes > 0) ? max_bytes : 0;
  cs.codestream.flush(&flush_limit, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

TEST(EncoderTest, DirectReversiblePrecisionsOffsetsOddDimsAndNonExchangeable) {
  for (const char* blk_param : {"Cblk={32,32}", "Cblk={4,4}"}) {
    for (bool odd_dims : {false, true}) {
      DirectEncodeConfig enc;
      enc.width = odd_dims ? 37 : 40;
      enc.height = odd_dims ? 25 : 24;
      enc.ox = odd_dims ? 1 : 0;
      enc.oy = odd_dims ? 1 : 0;
      enc.precision = 8;
      enc.is_signed = false;
      enc.reversible = true;
      enc.extra_params = {blk_param};
      const std::vector<int64_t> src =
          MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed,
                      /*sparse_blocks=*/true);

      for (kdu_byte sp :
           {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32, KDU_SAMPLE_PREC64}) {
        DirectEncodeConfig cur = enc;
        cur.sample_prec = sp;
        cur.push_offset = (sp == KDU_SAMPLE_PREC32) ? 5 : 0;
        cur.exchangeable = (sp != KDU_SAMPLE_PREC64) && !odd_dims;
        cur.call_start = (sp != KDU_SAMPLE_PREC16);
        cur.call_start_twice = (sp == KDU_SAMPLE_PREC32);
        cur.extend_right = (sp == KDU_SAMPLE_PREC16) ? 1 : 0;
        cur.extended_info =
            (sp == KDU_SAMPLE_PREC16) ? KDU_LINE_WILL_BE_EXTENDED : 0;
        cur.opt_flags = (sp == KDU_SAMPLE_PREC32)
                            ? KDU_BLOCK_CODING_NO_ACCELERATE
                            : KDU_BLOCK_CODING_ACCELERATE;
        const DirectEncodeResult res = RunDirectEncoder(cur, src);
        EXPECT_FALSE(res.bitstream.empty());
        EXPECT_EQ(DecodeComponentInt(res.bitstream, enc.width, enc.height,
                                     enc.precision, enc.is_signed),
                  src);
      }
    }
  }
}

TEST(EncoderTest, DirectIrreversibleAndPrequantScaleAndGeometry) {
  for (const char* blk_param : {"Cblk={32,32}", "Cblk={4,4}"}) {
    DirectEncodeConfig enc;
    enc.width = 37;
    enc.height = 25;
    enc.precision = 8;
    enc.is_signed = true;
    enc.reversible = false;
    enc.extra_params = {blk_param, "Qstep=0.0005"};
    const std::vector<int64_t> src = MakeSamples(
        enc.width, enc.height, enc.precision, enc.is_signed, /*sparse=*/true);

    // 16-bit fixed-point, 32-bit float, and 32-bit prequant_scale downgrade.
    for (int mode = 0; mode < 3; ++mode) {
      DirectEncodeConfig cur = enc;
      cur.sample_prec = (mode == 0) ? KDU_SAMPLE_PREC16 : KDU_SAMPLE_PREC32;
      cur.use_prequant = (mode == 2);
      cur.exchangeable = (mode != 1);
      const DirectEncodeResult res = RunDirectEncoder(cur, src);
      EXPECT_FALSE(res.bitstream.empty());
      if (mode == 2) {
        EXPECT_GT(res.prequant_scale, 0.0F);
      }
    }

    // Geometry flags (transpose / vflip / hflip).
    for (kdu_byte sp : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
      DirectEncodeConfig cur = enc;
      cur.sample_prec = sp;
      cur.transpose = true;
      cur.vflip = true;
      cur.hflip = true;
      const DirectEncodeResult res = RunDirectEncoder(cur, src);
      EXPECT_FALSE(res.bitstream.empty());
    }
  }
}

TEST(EncoderTest, HighBitDepth40BitStripePathsAndExtraMissingMsbs) {
  // 1. Small 12-bit values in 34-bit codestream -> extra_missing_msbs > 0
  // downshifts to 32-bit block sample_buffer.
  {
    DirectEncodeConfig enc;
    enc.width = 25;
    enc.height = 23;
    enc.precision = 34;
    enc.is_signed = true;
    enc.reversible = true;
    enc.sample_prec = KDU_SAMPLE_PREC64;
    enc.extra_params = {"Cblk={8,8}"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, /*precision=*/12, enc.is_signed,
                    /*sparse_blocks=*/true);
    const DirectEncodeResult res = RunDirectEncoder(enc, src);
    EXPECT_EQ(DecodeComponentInt(res.bitstream, enc.width, enc.height,
                                 enc.precision, enc.is_signed),
              src);
  }

  // 2. Full 35-bit range + smaller block so both sample_buffer64 and 32-bit
  // extra_missing_msbs paths execute cleanly, plus PREC16 with stripe_prec=32.
  {
    DirectEncodeConfig enc;
    enc.width = 25;
    enc.height = 23;
    enc.precision = 35;
    enc.is_signed = true;
    enc.reversible = true;
    enc.sample_prec = KDU_SAMPLE_PREC64;
    enc.extra_params = {"Cblk={8,8}"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, /*precision=*/12, enc.is_signed,
                    /*sparse_blocks=*/true, int64_t{1} << 33);
    const DirectEncodeResult res = RunDirectEncoder(enc, src);
    EXPECT_EQ(DecodeComponentInt(res.bitstream, enc.width, enc.height,
                                 enc.precision, enc.is_signed),
              src);
  }

  // 3. PREC16 with Clevels=1 and precision=14 so HH band has K_max >= 16 ->
  // stripe_prec == 32 from 16-bit incoming lines.
  {
    const int w = 25;
    const int h = 23;
    const std::vector<int64_t> src =
        MakeSamples(w, h, /*precision=*/14, /*is_signed=*/true);
    const std::vector<kdu_byte> bs = RunMultiAnalysis(
        w, h, /*precision=*/14, /*is_signed=*/true, /*reversible=*/true,
        /*clevels=*/1, KDU_MULTI_XFORM_FAST, /*max_bytes=*/0,
        /*num_threads=*/0, {"Cblk={8,8}"}, src);
    EXPECT_EQ(DecodeComponentInt(bs, w, h, /*precision=*/14, true), src);
  }
}

TEST(EncoderTest, RoiMaxshiftWeightedAndErrorPaths) {
  // 1. 32-bit ROI (Rshift=12 on 8-bit) and 64-bit ROI (Rshift=24 on 10-bit).
  for (bool transpose : {false, true}) {
    DirectEncodeConfig enc;
    enc.width = 25;
    enc.height = 23;
    enc.precision = 8;
    enc.is_signed = true;
    enc.reversible = true;
    enc.sample_prec = KDU_SAMPLE_PREC16;
    enc.use_roi = true;
    enc.transpose = transpose;
    enc.extra_params = {"Cblk={8,8}", "Rshift=12"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);
    const DirectEncodeResult res = RunDirectEncoder(enc, src);
    EXPECT_FALSE(res.bitstream.empty());

    DirectEncodeConfig enc64 = enc;
    enc64.precision = 10;
    enc64.sample_prec = KDU_SAMPLE_PREC32;
    enc64.extra_params = {"Cblk={8,8}", "Rshift=24"};
    const std::vector<int64_t> src64 = MakeSamples(
        enc64.width, enc64.height, enc64.precision, enc64.is_signed);
    const DirectEncodeResult res64 = RunDirectEncoder(enc64, src64);
    EXPECT_FALSE(res64.bitstream.empty());
  }

  // Upstream bug in encoder.cpp:4609-4627: non-MAXSHIFT ROI weighting
  // (`roi_node != nullptr && K_max_prime == K_max`) allocates 101 floats (404
  // bytes) for `per_plane_wmse`, leaving `push_state` 4-byte aligned and
  // triggering UBSan `misaligned-pointer-use` (SIGILL under fastbuild).

  // 2. ROI Rshift smaller than subband K_max throws kdu_exception.
  {
    DirectEncodeConfig enc;
    enc.width = 16;
    enc.height = 16;
    enc.precision = 12;
    enc.is_signed = true;
    enc.reversible = true;
    enc.use_roi = true;
    enc.extra_params = {"Cblk={8,8}", "Rshift=2"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);
    EXPECT_THROW(RunDirectEncoder(enc, src), kdu_exception);
  }

  // Upstream bug in encoder.cpp:186-189: `kdu_encoder::kdu_encoder`
  // allocates `new kd_encoder` before `kd_encoder::init` throws (e.g. ROI with
  // non-zero `push_offset` -> `0x06031901` at `encoder.cpp:4061`), leaking the
  // 600-byte `kd_encoder` object because the constructor does not free `state`
  // on exception.
}

TEST(EncoderTest, VisualMaskingCvisLlAndSubbandsAndErrors) {
  // 1. Reversible and irreversible visual masking across PREC16 & PREC32,
  // exercising both LL_BAND (Clevels=0) and subbands (Clevels=1) + MT.
  for (int clevels : {0, 1}) {
    for (bool reversible : {true, false}) {
      for (int xform_flags : {KDU_MULTI_XFORM_FAST, KDU_MULTI_XFORM_PRECISE}) {
        const int w = (clevels == 0) ? 25 : 49;
        // Keep subband height a multiple of stripe height (8) so the final
        // stripe is even-height; otherwise `push()` (`encoder.cpp:3809`)
        // calls `transfer_visual_masking_weights()` after `push_stripe_line++`
        // reaches `stripe->height`, tripping the assert at `encoder.cpp:3625`.
        const int h = (clevels == 0) ? 24 : 48;
        const std::vector<int64_t> src =
            MakeSamples(w, h, /*precision=*/8, /*is_signed=*/true);
        // `Cblk={height,width}`: keep code-block width (`64`) >= subband width
        // so `hor_offset == 0` (avoiding upstream `encoder.cpp:319` bug that
        // scales `hor_offset` in samples rather than 4x4 cells), while height
        // `8` exercises multiple vertical stripes + MT.
        std::vector<std::string> params = {"Cblk={8,64}", "Cvis=0.05"};
        if (!reversible) {
          params.push_back("Qstep=0.001");
        }
        const std::vector<kdu_byte> bs = RunMultiAnalysis(
            w, h, /*precision=*/8, /*is_signed=*/true, reversible, clevels,
            xform_flags | KDU_MULTI_XFORM_MT_DWT, /*max_bytes=*/0,
            /*num_threads=*/2, params, src);
        EXPECT_FALSE(bs.empty());
      }
    }
  }

  // 2. Non-multiple-of-4 trailing widths (`last_cell_width` in 1..3) with
  // single horizontal block (`<= 8`) and `cells_across >= 2` (`w >= 5`,
  // avoiding `encoder.cpp:5049` `cell_row[1]` overrun when `cells_across==1`).
  for (int w : {5, 6, 7}) {
    const int h = 7;
    const std::vector<int64_t> src =
        MakeSamples(w, h, /*precision=*/8, /*is_signed=*/true);
    const std::vector<kdu_byte> bs = RunMultiAnalysis(
        w, h, /*precision=*/8, /*is_signed=*/true, /*reversible=*/true,
        /*clevels=*/0, KDU_MULTI_XFORM_PRECISE, /*max_bytes=*/0,
        /*num_threads=*/0, {"Cblk={8,8}", "Cvis=0.05"}, src);
    EXPECT_FALSE(bs.empty());
  }

  // 3. Cvis error condition rejected during parameter finalization (`Cblk <
  // 4`). Note: `KDU_SAMPLE_PREC64` with `Cvis` throws inside
  // `kd_mask_encoder::init`
  // (`encoder.cpp:5088`) after `kdu_encoder::kdu_encoder` (`encoder.cpp:177`)
  // has already `new`-allocated `kd_mask_encoder`, leaking it under
  // HeapLeakChecker/LSan.
  {
    const std::vector<int64_t> src =
        MakeSamples(16, 16, /*precision=*/8, /*is_signed=*/true);
    EXPECT_THROW(
        RunMultiAnalysis(16, 16, 8, true, true, 0, KDU_MULTI_XFORM_PRECISE, 0,
                         0, {"Cblk={2,2}", "Cvis=0.05"}, src),
        kdu_exception);
  }
}

TEST(EncoderTest, CplexEstScalarAndSimdLinePairAndIsolatedLinePaths) {
  // Exercise Cplex=EST across:
  // - Scalar (Cblk={4,4}) and SSSE3 SIMD (Cblk={32,32})
  // - Reversible & irreversible
  // - PREC16 (stripe_prec=16 and stripe_prec=32 via Clevels=1, prec=14),
  //   PREC32, and PREC64 (stripe_prec=32 and stripe_prec=36)
  // - Odd dimensions (xfer_width & 1 and isolated last line)
  // - Both negative delay (Cplex={6,EST,0.25,-1}) and positive delay, plus MT
  // - Content_type=PHOTO vs Content_type=SCREEN
  struct CplexCase {
    int width;
    int height;
    int precision;
    bool reversible;
    int clevels;
    int xform_flags;
    int num_threads;
    const char* cblk;
    const char* cplex;
    const char* content_type;
    int64_t custom_scale;
  };
  const CplexCase kCases[] = {
      {25, 25, 8, true, 0, KDU_MULTI_XFORM_FAST, 0, "Cblk={4,4}",
       "Cplex={6,EST,0.25,8}", "Content_type=PHOTO", 0},
      {25, 25, 8, false, 0, KDU_MULTI_XFORM_FAST, 0, "Cblk={4,4}",
       "Cplex={6,EST,0.25,8}", "Content_type=SCREEN", 0},
      {25, 25, 14, true, 1, KDU_MULTI_XFORM_FAST, 0, "Cblk={8,8}",
       "Cplex={6,EST,0.25,8}", "Content_type=PHOTO", 0},
      {25, 25, 8, true, 0, KDU_MULTI_XFORM_PRECISE, 0, "Cblk={4,4}",
       "Cplex={6,EST,0.25,8}", "Content_type=PHOTO", 0},
      {25, 25, 8, false, 0, KDU_MULTI_XFORM_PRECISE, 0, "Cblk={4,4}",
       "Cplex={6,EST,0.25,8}", "Content_type=SCREEN", 0},
      {25, 25, 12, true, 0, KDU_MULTI_XFORM_VPRECISE, 0, "Cblk={4,4}",
       "Cplex={6,EST,0.25,8}", "Content_type=PHOTO", 0},
      {25, 25, 35, true, 0, KDU_MULTI_XFORM_VPRECISE, 0, "Cblk={4,4}",
       "Cplex={6,EST,0.25,8}", "Content_type=PHOTO", int64_t{1} << 33},
      // SIMD Cplex line-pair transfers with Cblk={32,32} and multiple stripes.
      // Single-threaded here to avoid upstream UBSan `nullptr + 1` in
      // `compressed.cpp:9588` during multi-threaded
      // `advance_bkgnd_cplex_state`.
      {73, 73, 8, true, 0, KDU_MULTI_XFORM_FAST, 0, "Cblk={32,32}",
       "Cplex={6,EST,0.25,-1}", "Content_type=PHOTO", 0},
      {73, 73, 8, false, 0, KDU_MULTI_XFORM_FAST, 0, "Cblk={32,32}",
       "Cplex={6,EST,0.25,32}", "Content_type=SCREEN", 0},
      {73, 73, 8, true, 0, KDU_MULTI_XFORM_PRECISE, 0, "Cblk={32,32}",
       "Cplex={6,EST,0.25,-1}", "Content_type=PHOTO", 0},
      {73, 73, 8, false, 0, KDU_MULTI_XFORM_PRECISE, 0, "Cblk={32,32}",
       "Cplex={6,EST,0.25,32}", "Content_type=SCREEN", 0},
      // Cplex paired with visual masking on PREC32 (`Cblk={height,width}`:
      // single block horizontally to keep `hor_offset == 0` around
      // `encoder.cpp:319`, even final stripe height around `encoder.cpp:3625`,
      // and 64-byte-aligned `cell4x4` stripe size around `encoder.cpp:831` vs
      // `5194`).
      {64, 64, 8, false, 1, KDU_MULTI_XFORM_PRECISE, 0, "Cblk={8,64}",
       "Cplex={6,EST,0.25,16}", "Content_type=PHOTO", 0},
  };

  for (size_t idx = 0; idx < sizeof(kCases) / sizeof(kCases[0]); ++idx) {
    const CplexCase& c = kCases[idx];
    SCOPED_TRACE(idx);
    const std::vector<int64_t> src = MakeSamples(
        c.width, c.height, std::min(c.precision, 12),
        /*is_signed=*/true, /*sparse_blocks=*/false, c.custom_scale);
    std::vector<std::string> params = {c.cblk, c.cplex, c.content_type};
    if (!c.reversible) {
      params.push_back("Qstep=0.001");
    }
    if (idx == 11) {
      params.push_back("Cvis=0.05");
    }
    const std::vector<kdu_byte> bs =
        RunMultiAnalysis(c.width, c.height, c.precision, /*is_signed=*/true,
                         c.reversible, c.clevels, c.xform_flags,
                         /*max_bytes=*/1024, c.num_threads, params, src);
    EXPECT_FALSE(bs.empty());
  }

  // Note: `Cplex=EST` combined with ROI throws inside `kd_encoder::init`
  // (`encoder.cpp:4139`) after `kdu_encoder::kdu_encoder` (`encoder.cpp:186`)
  // has already `new`-allocated `kd_encoder`, leaking it under
  // HeapLeakChecker/LSan.
}

TEST(EncoderTest, EmptySubbandAndMultiThreadedStripesAndEarlyDestroy) {
  // 1. 0-column HL subband (1x16 with Clevels=1).
  {
    std::vector<kdu_byte> bitstream;
    VectorTarget vec_target(bitstream);
    ScopedCodestream cs;
    cs.siz.set(Scomponents, 0, 0, 1);
    cs.siz.set(Sdims, 0, 0, 16);
    cs.siz.set(Sdims, 0, 1, 1);
    cs.siz.set(Sprecision, 0, 0, 8);
    cs.siz.set(Ssigned, 0, 0, true);
    static_cast<kdu_params*>(&cs.siz)->finalize();
    cs.codestream.create(&cs.siz, &vec_target);
    siz_params* params = cs.codestream.access_siz();
    EXPECT_TRUE(params->parse_string("Creversible=yes"));
    EXPECT_TRUE(params->parse_string("Clevels=1"));
    EXPECT_TRUE(params->parse_string("Ckernels=W5X3"));
    params->finalize_all();
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
    ScopedPush enc;
    enc.engine = kdu_encoder(hl_band, &alloc, pp_params, KDU_SAMPLE_PREC16);
    ScopedLineBuf empty_line;
    // Leave `empty_line` unallocated: `kdu_sample_allocator::alloc16`
    // (`kdu_sample_processing.h:653`) omits `~` on its alignment mask, so
    // `width == 0` asks for 15 samples from a 0-byte reservation.
    alloc.finalize(cs.codestream);
    enc.engine.start(nullptr);
    EXPECT_TRUE(enc.engine.push(empty_line.line, nullptr));
    EXPECT_TRUE(enc.engine.finish(nullptr));
    enc.engine.destroy();
    tile.tile.close();
    cs.codestream.destroy();
  }

  // 2. Multi-threaded stripes (1..4 stripes, multi-job stripes) + early
  // termination mid-stream.
  {
    DirectEncodeConfig enc;
    enc.width = 160;
    enc.height = 97;
    enc.precision = 8;
    enc.is_signed = true;
    enc.reversible = true;
    enc.num_threads = 4;
    enc.min_jobs_across = 4;
    enc.log2_min_samples = 8;
    enc.log2_ideal_samples = 9;
    enc.extra_params = {"Cblk={8,8}"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, enc.precision, enc.is_signed);

    for (int stripes : {1, 2, 3, 4, -1}) {
      DirectEncodeConfig cur = enc;
      cur.sample_prec = (stripes == 4) ? KDU_SAMPLE_PREC32 : KDU_SAMPLE_PREC16;
      cur.max_hires_stripes = stripes;
      cur.extra_lowres_stripe = (stripes == 4);
      const DirectEncodeResult res = RunDirectEncoder(cur, src);
      EXPECT_EQ(DecodeComponentInt(res.bitstream, enc.width, enc.height,
                                   enc.precision, enc.is_signed),
                src);
    }

    for (int push_limit : {0, 1, 10, 40}) {
      DirectEncodeConfig cur = enc;
      cur.sample_prec = KDU_SAMPLE_PREC32;
      cur.max_hires_stripes = 4;
      cur.push_limit = push_limit;
      const DirectEncodeResult res = RunDirectEncoder(cur, src);
      (void)res;
    }
  }
}

std::vector<int64_t> NegateSamples(const std::vector<int64_t>& src) {
  std::vector<int64_t> out(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    out[i] = -src[i];
  }
  return out;
}

TEST(EncoderTest, DirectFastPathsAndExtraMissingMsbsAndCvisGeometry) {
  // Block with zero extension bytes (extra_missing_msbs = 8 -> encode16) next
  // to a block requiring full 18-bit precision.
  {
    DirectEncodeConfig enc;
    enc.width = 64;
    enc.height = 16;
    enc.precision = 18;
    enc.is_signed = true;
    enc.reversible = true;
    enc.sample_prec = KDU_SAMPLE_PREC32;
    enc.extra_params = {"Cblk={8,32}"};
    std::vector<int64_t> src(static_cast<size_t>(enc.width) * enc.height, 0);
    for (int y = 0; y < enc.height; ++y) {
      for (int x = 0; x < enc.width; ++x) {
        src[static_cast<size_t>(y) * enc.width + x] =
            (x < 32) ? ((x + y) & 1 ? -5 : 7) : ((x & 1 ? -1 : 1) * 50000);
      }
    }
    const DirectEncodeResult res = RunDirectEncoder(enc, src);
    EXPECT_EQ(DecodeComponentInt(res.bitstream, enc.width, enc.height,
                                 enc.precision, enc.is_signed),
              src);
  }

  // stripe_prec == 40 with small values -> extra_missing_msbs = 8 -> encode32.
  {
    DirectEncodeConfig enc;
    enc.width = 32;
    enc.height = 16;
    enc.precision = 34;
    enc.is_signed = true;
    enc.reversible = true;
    enc.sample_prec = KDU_SAMPLE_PREC64;
    enc.extra_params = {"Cblk={8,32}"};
    const std::vector<int64_t> src =
        MakeSamples(enc.width, enc.height, /*precision=*/8, /*is_signed=*/true);
    const DirectEncodeResult res = RunDirectEncoder(enc, src);
    EXPECT_EQ(DecodeComponentInt(res.bitstream, enc.width, enc.height,
                                 enc.precision, enc.is_signed),
              src);
  }

  // Full 32-bit stripe_prec fast paths (encode32 reversible and irreversible).
  {
    DirectEncodeConfig rev32;
    rev32.width = 32;
    rev32.height = 16;
    rev32.precision = 25;
    rev32.is_signed = true;
    rev32.reversible = true;
    rev32.sample_prec = KDU_SAMPLE_PREC32;
    rev32.extra_params = {"Cblk={8,32}"};
    const std::vector<int64_t> src32 =
        MakeSamples(rev32.width, rev32.height, rev32.precision, rev32.is_signed,
                    /*sparse_blocks=*/false, int64_t{1} << 22);
    EXPECT_EQ(DecodeComponentInt(RunDirectEncoder(rev32, src32).bitstream,
                                 rev32.width, rev32.height, rev32.precision,
                                 rev32.is_signed),
              src32);

    DirectEncodeConfig irrev32;
    irrev32.width = 32;
    irrev32.height = 16;
    irrev32.precision = 12;
    irrev32.is_signed = true;
    irrev32.reversible = false;
    irrev32.sample_prec = KDU_SAMPLE_PREC32;
    irrev32.extra_params = {"Cblk={8,32}", "Qstep=0.000002", "Qguard=7"};
    const std::vector<int64_t> src12 = MakeSamples(
        irrev32.width, irrev32.height, irrev32.precision, irrev32.is_signed);
    EXPECT_FALSE(RunDirectEncoder(irrev32, src12).bitstream.empty());
  }

  // Visual masking cell4x4 compaction with geometry permutations (single block
  // across so hor_offset == 0 and 64-byte L2 stripe stride matches).
  {
    const std::vector<int64_t> src =
        MakeSamples(32, 32, /*precision=*/8, /*is_signed=*/true);
    for (int mask = 1; mask <= 7; ++mask) {
      if (mask != 1 && mask != 2 && mask != 4 && mask != 7) {
        continue;
      }
      DirectEncodeConfig cur;
      cur.width = 32;
      cur.height = 32;
      cur.precision = 8;
      cur.is_signed = true;
      cur.reversible = false;
      cur.sample_prec = KDU_SAMPLE_PREC32;
      cur.transpose = (mask & 1) != 0;
      cur.vflip = (mask & 2) != 0;
      cur.hflip = (mask & 4) != 0;
      cur.extra_params = {"Cblk={32,32}", "Cvis=0.05", "Qstep=0.001"};
      EXPECT_FALSE(RunDirectEncoder(cur, src).bitstream.empty());
    }
  }
}

TEST(EncoderTest, DirectScalarLineTransfersAndCplexAndAvx2Cplex64) {
  struct LineTransferCase {
    bool use_cplex;
    bool reversible;
    kdu_byte sample_prec;
    int encode_precision;
    int sample_magnitude_bits;
    std::vector<std::string> extra_params;
  };
  const std::vector<LineTransferCase> cases = {
      {false, true, KDU_SAMPLE_PREC16, 20, 8, {"Cblk={8,8}", "Qguard=7"}},
      {false,
       false,
       KDU_SAMPLE_PREC16,
       8,
       8,
       {"Cblk={8,8}", "Qstep=0.000001", "Qguard=7"}},
      {false, true, KDU_SAMPLE_PREC32, 25, 25, {"Cblk={8,8}", "Qguard=4"}},
      {false,
       false,
       KDU_SAMPLE_PREC32,
       8,
       8,
       {"Cblk={8,8}", "Qstep=0.000001", "Qguard=7"}},
      {true,
       true,
       KDU_SAMPLE_PREC16,
       8,
       8,
       {"Cblk={8,8}", "Cplex={6,EST,0.25,8}"}},
      {true,
       false,
       KDU_SAMPLE_PREC16,
       8,
       8,
       {"Cblk={8,8}", "Qstep=0.001", "Cplex={6,EST,0.25,8}"}},
      {true,
       true,
       KDU_SAMPLE_PREC16,
       20,
       8,
       {"Cblk={8,8}", "Qguard=7", "Cplex={6,EST,0.25,8}"}},
      {true,
       false,
       KDU_SAMPLE_PREC16,
       8,
       8,
       {"Cblk={8,8}", "Qstep=0.000001", "Qguard=7", "Cplex={6,EST,0.25,8}"}},
      {true,
       true,
       KDU_SAMPLE_PREC32,
       25,
       25,
       {"Cblk={8,8}", "Qguard=4", "Cplex={6,EST,0.25,8}"}},
      {true,
       false,
       KDU_SAMPLE_PREC32,
       8,
       8,
       {"Cblk={8,8}", "Qstep=0.000001", "Qguard=7", "Cplex={6,EST,0.25,8}"}},
      {true,
       true,
       KDU_SAMPLE_PREC64,
       34,
       34,
       {"Cblk={8,8}", "Cplex={6,EST,0.25,8}"}},
      // Excessive Cplex delay clamping path.
      {true,
       true,
       KDU_SAMPLE_PREC16,
       8,
       8,
       {"Cblk={8,8}", "Cplex={6,EST,0.25,1000}"}},
  };

  for (const LineTransferCase& c : cases) {
    for (int width : {3, 11, 25}) {
      DirectEncodeConfig enc;
      enc.width = width;
      enc.height = 15;
      enc.precision = c.encode_precision;
      enc.is_signed = true;
      enc.reversible = c.reversible;
      enc.sample_prec = c.sample_prec;
      enc.use_cplex = c.use_cplex;
      enc.max_bytes = c.use_cplex ? 4096 : 0;
      enc.opt_flags = 0;
      enc.extra_params = c.extra_params;
      if (width == 11 && enc.extra_params[0] == "Cblk={8,8}") {
        enc.extra_params[0] = "Cblk={8,4}";
      }
      const std::vector<int64_t> pos_src =
          MakeSamples(enc.width, enc.height, c.sample_magnitude_bits, true);
      const std::vector<int64_t> neg_src = NegateSamples(pos_src);
      EXPECT_FALSE(RunDirectEncoder(enc, pos_src).bitstream.empty());
      EXPECT_FALSE(RunDirectEncoder(enc, neg_src).bitstream.empty());
    }
  }

  // SIMD 64->40bit non-Cplex (4715-4727) & Cplex paths + max_K_est clamp
  // (4367).
  for (bool use_cplex : {false, true}) {
    DirectEncodeConfig simd64;
    simd64.width = 33;
    simd64.height = 17;
    simd64.precision = 35;
    simd64.is_signed = true;
    simd64.reversible = true;
    simd64.sample_prec = KDU_SAMPLE_PREC64;
    simd64.use_cplex = use_cplex;
    simd64.max_bytes = use_cplex ? 8192 : 0;
    simd64.extra_params =
        use_cplex
            ? std::vector<std::string>{"Cblk={8,16}", "Cplex={6,EST,0.25,8}"}
            : std::vector<std::string>{"Cblk={8,16}"};
    const std::vector<int64_t> src =
        MakeSamples(simd64.width, simd64.height, simd64.precision, true);
    if (!use_cplex) {
      EXPECT_EQ(DecodeComponentInt(RunDirectEncoder(simd64, src).bitstream,
                                   simd64.width, simd64.height,
                                   simd64.precision, simd64.is_signed),
                src);
    } else {
      EXPECT_FALSE(RunDirectEncoder(simd64, src).bitstream.empty());
    }
  }

  {
    DirectEncodeConfig clamp_kest;
    clamp_kest.width = 64;
    clamp_kest.height = 16;
    clamp_kest.precision = 20;
    clamp_kest.is_signed = true;
    clamp_kest.reversible = true;
    clamp_kest.sample_prec = KDU_SAMPLE_PREC16;
    clamp_kest.use_cplex = true;
    clamp_kest.max_bytes = 4096;
    clamp_kest.extra_params = {"Cblk={8,32}", "Cplex={6,EST,0.25,8}"};
    EXPECT_FALSE(RunDirectEncoder(clamp_kest, MakeSamples(64, 16, 8, true))
                     .bitstream.empty());
  }
}

TEST(EncoderTest, Direct64BitMaxshiftRoiAndMtRoiAndCplexInit) {
  auto MakeSamples64 = [](int w, int h) {
    std::vector<int64_t> out = MakeSamples(w, h, /*precision=*/32, true);
    for (int64_t& v : out) {
      v = (v >= 0) ? (v | (int64_t{1} << 32)) : -((-v) | (int64_t{1} << 32));
    }
    return out;
  };
  // KDU_SAMPLE_PREC64 geometry without ROI, plus MAXSHIFT ROI (including K>63
  // warning, !have_background, !have_foreground, transpose/vflip/hflip).
  {
    const std::vector<int64_t> src24 = MakeSamples64(24, 24);
    for (int mask : {2, 4, 7}) {
      DirectEncodeConfig enc;
      enc.width = 24;
      enc.height = 24;
      enc.precision = 34;
      enc.is_signed = true;
      enc.reversible = true;
      enc.sample_prec = KDU_SAMPLE_PREC64;
      enc.transpose = (mask & 1) != 0;
      enc.vflip = (mask & 2) != 0;
      enc.hflip = (mask & 4) != 0;
      enc.extra_params = {"Cblk={8,8}"};
      EXPECT_FALSE(RunDirectEncoder(enc, src24).bitstream.empty());
    }

    // All-foreground block (!have_background) vs all-background
    // (!have_foreground).
    for (int w : {16, 24}) {
      const std::vector<int64_t> src = MakeSamples64(w, 16);
      for (int mask : {0, 2, 4, 7}) {
        if (w == 16 && mask != 0) {
          continue;
        }
        DirectEncodeConfig roi64;
        roi64.width = w;
        roi64.height = 16;
        roi64.precision = 34;
        roi64.is_signed = true;
        roi64.reversible = true;
        roi64.sample_prec = KDU_SAMPLE_PREC64;
        roi64.use_roi = true;
        roi64.transpose = (mask & 1) != 0;
        roi64.vflip = (mask & 2) != 0;
        roi64.hflip = (mask & 4) != 0;
        roi64.extra_params = {"Cblk={8,8}", "Rshift=38"};
        EXPECT_FALSE(RunDirectEncoder(roi64, src).bitstream.empty());
      }
    }
  }

  // K < 32 MAXSHIFT ROI with flip/transpose flags.
  {
    const std::vector<int64_t> src = MakeSamples(24, 24, 8, true);
    for (int mask : {2, 4, 5}) {
      DirectEncodeConfig roi32;
      roi32.width = 24;
      roi32.height = 24;
      roi32.precision = 8;
      roi32.is_signed = true;
      roi32.reversible = true;
      roi32.sample_prec = KDU_SAMPLE_PREC32;
      roi32.use_roi = true;
      roi32.transpose = (mask & 1) != 0;
      roi32.vflip = (mask & 2) != 0;
      roi32.hflip = (mask & 4) != 0;
      roi32.extra_params = {"Cblk={8,8}", "Rshift=12"};
      EXPECT_FALSE(RunDirectEncoder(roi32, src).bitstream.empty());
    }
  }

  // Multi-threaded ROI transfers (roi_context->acquire_lock) & MT Cplex /
  // job-splitting & small Cblk={4,4} alignment.
  {
    const std::vector<int64_t> src = MakeSamples(25, 15, 8, true);
    for (kdu_byte sp : {KDU_SAMPLE_PREC16, KDU_SAMPLE_PREC32}) {
      DirectEncodeConfig mt_roi;
      mt_roi.width = 25;
      mt_roi.height = 15;
      mt_roi.precision = 8;
      mt_roi.is_signed = true;
      mt_roi.reversible = true;
      mt_roi.sample_prec = sp;
      mt_roi.use_roi = true;
      mt_roi.num_threads = 2;
      mt_roi.max_hires_stripes = 1;
      mt_roi.extra_params = {"Cblk={8,8}", "Rshift=12"};
      EXPECT_FALSE(RunDirectEncoder(mt_roi, src).bitstream.empty());
    }

    DirectEncodeConfig small_cblk;
    small_cblk.width = 13;
    small_cblk.height = 13;
    small_cblk.precision = 8;
    small_cblk.is_signed = true;
    small_cblk.reversible = true;
    small_cblk.sample_prec = KDU_SAMPLE_PREC16;
    small_cblk.opt_flags = 0;
    small_cblk.extra_params = {"Cblk={4,4}"};
    EXPECT_FALSE(RunDirectEncoder(small_cblk, MakeSamples(13, 13, 8, true))
                     .bitstream.empty());

    DirectEncodeConfig mt_jobs;
    mt_jobs.width = 32;
    mt_jobs.height = 24;
    mt_jobs.precision = 8;
    mt_jobs.is_signed = true;
    mt_jobs.reversible = true;
    mt_jobs.sample_prec = KDU_SAMPLE_PREC16;
    mt_jobs.num_threads = 2;
    mt_jobs.min_jobs_across = 8;
    mt_jobs.log2_min_samples = 6;
    mt_jobs.log2_ideal_samples = 7;
    mt_jobs.max_hires_stripes = 1;
    mt_jobs.extra_params = {"Cblk={8,8}"};
    EXPECT_FALSE(RunDirectEncoder(mt_jobs, MakeSamples(32, 24, 8, true))
                     .bitstream.empty());

    DirectEncodeConfig mt_cplex = mt_jobs;
    mt_cplex.width = 32;
    mt_cplex.height = 160;
    mt_cplex.use_cplex = true;
    mt_cplex.max_bytes = 8192;
    mt_cplex.min_jobs_across = 2;
    mt_cplex.extra_params = {"Cblk={8,8}", "Cplex={6,EST,0.25,256}"};
    EXPECT_FALSE(RunDirectEncoder(mt_cplex, MakeSamples(32, 160, 8, true))
                     .bitstream.empty());

    // Early gate delay > subband_rows clamp (4270) & num_buffer_stripes clamp
    // (4292) & ideal_stripes > block_indices.size.y (4258).
    DirectEncodeConfig short_cplex;
    short_cplex.width = 16;
    short_cplex.height = 12;
    short_cplex.precision = 8;
    short_cplex.is_signed = true;
    short_cplex.reversible = true;
    short_cplex.sample_prec = KDU_SAMPLE_PREC16;
    short_cplex.use_cplex = true;
    short_cplex.max_bytes = 4096;
    short_cplex.extra_params = {"Cblk={8,8}", "Cplex={6,EST,0.25,256}"};
    EXPECT_FALSE(RunDirectEncoder(short_cplex, MakeSamples(16, 12, 8, true))
                     .bitstream.empty());

    DirectEncodeConfig tall_clamped = mt_jobs;
    tall_clamped.height = 16;
    tall_clamped.max_hires_stripes = 4;
    EXPECT_FALSE(RunDirectEncoder(tall_clamped, MakeSamples(32, 16, 8, true))
                     .bitstream.empty());

    // Job-splitting loops (4220-4235).
    for (int w_blocks : {3, 5}) {
      DirectEncodeConfig split_cfg;
      split_cfg.width = w_blocks * 64;
      split_cfg.height = 16;
      split_cfg.precision = 8;
      split_cfg.is_signed = true;
      split_cfg.reversible = true;
      split_cfg.sample_prec = KDU_SAMPLE_PREC16;
      split_cfg.num_threads = 2;
      split_cfg.min_jobs_across = 2;
      split_cfg.log2_min_samples = 6;
      split_cfg.log2_ideal_samples = 12;
      split_cfg.extra_params = {"Cblk={8,64}"};
      EXPECT_FALSE(
          RunDirectEncoder(split_cfg, MakeSamples(split_cfg.width, 16, 8, true))
              .bitstream.empty());
    }

    // Sub-4 first/last block alignment with nominal_block_width == 4 (4389,
    // 4400-4402) & ROI node release when Rshift==0 (4459-4461).
    DirectEncodeConfig ox_align;
    ox_align.ox = 1;
    ox_align.width = 6;
    ox_align.height = 8;
    ox_align.precision = 8;
    ox_align.is_signed = true;
    ox_align.reversible = true;
    ox_align.sample_prec = KDU_SAMPLE_PREC16;
    ox_align.opt_flags = 0;
    ox_align.extra_params = {"Cblk={4,4}"};
    EXPECT_FALSE(RunDirectEncoder(ox_align, MakeSamples(6, 8, 8, true))
                     .bitstream.empty());

    DirectEncodeConfig noop_roi;
    noop_roi.width = 16;
    noop_roi.height = 16;
    noop_roi.precision = 8;
    noop_roi.is_signed = true;
    noop_roi.reversible = true;
    noop_roi.sample_prec = KDU_SAMPLE_PREC32;
    noop_roi.use_roi = true;
    noop_roi.extra_params = {"Cblk={8,8}"};
    EXPECT_FALSE(RunDirectEncoder(noop_roi, MakeSamples(16, 16, 8, true))
                     .bitstream.empty());
  }
}

}  // namespace
}  // namespace kdu_core
