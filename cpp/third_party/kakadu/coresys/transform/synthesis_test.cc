// Unit tests for third_party/kakadu/vendor/coresys/transform/synthesis.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
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

class SynthesisTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kSynthesisEnv =
    ::testing::AddGlobalTestEnvironment(new SynthesisTestEnvironment());

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

struct ScopedAnalysis {
  template <typename... Args>
  explicit ScopedAnalysis(Args&&... args)
      : engine(std::forward<Args>(args)...) {}
  kdu_analysis engine;
  ~ScopedAnalysis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedSynthesis {
  template <typename... Args>
  explicit ScopedSynthesis(Args&&... args)
      : engine(std::forward<Args>(args)...) {}
  kdu_synthesis engine;
  ~ScopedSynthesis() {
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

struct SynthOptions {
  int width = 32;
  int height = 24;
  int ox = 0;
  int oy = 0;
  int precision = 8;
  bool is_signed = false;
  bool reversible = true;
  int dwt_levels = 1;
  std::vector<std::string> extra_params;
  kdu_byte sample_prec = KDU_LINE_BUF_SHORTS;
  float normalization = 1.0F;
  int pull_offset = 0;
  int extend_right = 0;
  int extended_info = 0;
  bool use_resolution_ctor = false;
  bool transpose = false;
  bool call_start = true;
  bool call_start_twice = false;
  int num_pull_lines = -1;  // -1 => pull full height
  bool sharp_step_pattern = false;
  bool set_overshoot = false;
  float overshoot_limit = 0.0F;
  bool overshoot_errors = false;
  bool has_roi = false;
  kdu_dims roi_dims;
  int max_layers = 0;
  std::vector<kdu_long> flush_bytes = {1 << 20};
  int num_threads = 0;
};

std::vector<int64_t> MakePatternSamples(int width, int height, int precision,
                                        bool is_signed, bool sharp_step) {
  std::vector<int64_t> out(static_cast<size_t>(width) * height);
  const int64_t max_mag = (int64_t{1} << (precision - 1)) - 1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int64_t val = 0;
      if (sharp_step) {
        const bool hi = ((x / 4 + y / 4) & 1) == 0;
        if (is_signed) {
          val = hi ? max_mag : -max_mag;
        } else {
          val = hi ? ((int64_t{1} << precision) - 1) : 0;
        }
      } else {
        const int64_t raw = ((x * 13 + y * 29 + (x ^ y) * 7) % 121) - 60;
        if (is_signed) {
          val = std::clamp(raw, -max_mag, max_mag);
        } else {
          const int64_t mid = int64_t{1} << (precision - 1);
          const int64_t max_u = (int64_t{1} << precision) - 1;
          val = std::clamp(mid + raw, int64_t{0}, max_u);
        }
      }
      out[static_cast<size_t>(y) * width + x] = val;
    }
  }
  return out;
}

void FillAnalysisLine(kdu_line_buf& line, const int64_t* row, int width,
                      int precision, bool is_signed, bool reversible,
                      kdu_byte ana_prec, float normalization) {
  const int64_t dc_offset = is_signed ? 0 : (int64_t{1} << (precision - 1));
  const float inv_scale = 1.0F / static_cast<float>(int64_t{1} << precision);
  if (ana_prec == KDU_LINE_BUF_SHORTS) {
    kdu_sample16* buf = line.get_buf16();
    for (int x = 0; x < width; ++x) {
      const int64_t centered = row[x] - dc_offset;
      if (reversible) {
        buf[x].ival = static_cast<kdu_int16>(centered);
      } else {
        const float norm =
            static_cast<float>(centered) * inv_scale * normalization;
        buf[x].ival =
            static_cast<kdu_int16>(std::lround(norm * (1 << KDU_FIX_POINT)));
      }
    }
  } else if (ana_prec == KDU_LINE_BUF_VPRECISE && reversible) {
    kdu_sample64* buf = line.get_buf64();
    for (int x = 0; x < width; ++x) {
      buf[x].ival = static_cast<kdu_int64>(row[x] - dc_offset);
    }
  } else {
    kdu_sample32* buf = line.get_buf32();
    for (int x = 0; x < width; ++x) {
      const int64_t centered = row[x] - dc_offset;
      if (reversible) {
        buf[x].ival = static_cast<kdu_int32>(centered);
      } else {
        buf[x].fval = static_cast<float>(centered) * inv_scale * normalization;
      }
    }
  }
}

std::vector<kdu_byte> EncodeSingleComponent(const SynthOptions& opts,
                                            const std::vector<int64_t>& src) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  EXPECT_TRUE(cs.siz.parse_string("Scomponents=1"));
  cs.siz.set(Sorigin, 0, 0, opts.oy);
  cs.siz.set(Sorigin, 0, 1, opts.ox);
  cs.siz.set(Sdims, 0, 0, opts.height);
  cs.siz.set(Sdims, 0, 1, opts.width);
  cs.siz.set(Ssize, 0, 0, opts.oy + opts.height);
  cs.siz.set(Ssize, 0, 1, opts.ox + opts.width);
  cs.siz.set(Ssampling, 0, 0, 1);
  cs.siz.set(Ssampling, 0, 1, 1);
  cs.siz.set(Sprecision, 0, 0, opts.precision);
  cs.siz.set(Ssigned, 0, 0, opts.is_signed);
  static_cast<kdu_params*>(&cs.siz)->finalize();

  cs.codestream.create(&cs.siz, &target);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(opts.reversible ? "Creversible=yes"
                                                   : "Creversible=no"));
  EXPECT_TRUE(params->parse_string(
      ("Clevels=" + std::to_string(opts.dwt_levels)).c_str()));
  for (const std::string& opt : opts.extra_params) {
    EXPECT_TRUE(params->parse_string(opt.c_str())) << opt;
  }
  params->finalize_all();

  {
    kdu_sample_allocator allocator;
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution res = tc.access_resolution();

    const kdu_byte ana_prec =
        (!opts.reversible && opts.sample_prec == KDU_LINE_BUF_VPRECISE)
            ? static_cast<kdu_byte>(0)
            : opts.sample_prec;
    const kdu_byte line_flags =
        ana_prec | (opts.reversible ? KDU_LINE_BUF_ABSOLUTE : 0);

    kdu_push_pull_params pp_params;
    ScopedAnalysis ana(res, &allocator, pp_params, ana_prec,
                       opts.normalization);
    ScopedLineBuf line;
    line.line.pre_create(&allocator, opts.width, line_flags, 0, 0);
    allocator.finalize(cs.codestream);
    line.line.create();
    ana.engine.start(nullptr);

    for (int y = 0; y < opts.height; ++y) {
      FillAnalysisLine(line.line, &src[static_cast<size_t>(y) * opts.width],
                       opts.width, opts.precision, opts.is_signed,
                       opts.reversible, ana_prec, opts.normalization);
      while (!ana.engine.push(line.line, nullptr)) {
      }
    }
    while (!ana.engine.finish(nullptr)) {
    }
    ana.engine.destroy();
    tile.tile.close();
  }

  std::vector<kdu_long> layer_bytes = opts.flush_bytes;
  cs.codestream.flush(layer_bytes.data(), static_cast<int>(layer_bytes.size()));
  cs.codestream.destroy();
  return bitstream;
}

struct DecodeResult {
  std::vector<int64_t> int_samples;
  std::vector<double> float_samples;
  bool overshoot_violated = false;
  float reported_overshoot = 0.0F;
  int out_width = 0;
  int out_height = 0;
};

DecodeResult RunSynthesisDecode(const std::vector<kdu_byte>& bitstream,
                                const SynthOptions& opts) {
  DecodeResult result;
  VectorSource source(bitstream);
  ScopedThreadEnv thread_ctx;
  kdu_thread_env* env_ptr = nullptr;
  kdu_thread_queue root_queue;
  if (opts.num_threads > 0) {
    thread_ctx.env.create();
    thread_ctx.created = true;
    for (int i = 1; i < opts.num_threads; ++i) {
      EXPECT_TRUE(thread_ctx.env.add_thread());
    }
    env_ptr = &thread_ctx.env;
  }

  ScopedCodestream cs;
  cs.codestream.create(&source, env_ptr);
  if (opts.transpose) {
    cs.codestream.change_appearance(true, false, false);
  }
  if (opts.has_roi || opts.max_layers > 0) {
    cs.codestream.apply_input_restrictions(
        0, 0, 0, opts.max_layers, opts.has_roi ? &opts.roi_dims : nullptr,
        KDU_WANT_OUTPUT_COMPONENTS, env_ptr);
  }
  if (opts.set_overshoot) {
    cs.codestream.set_synth_overshoot_limit(opts.overshoot_limit,
                                            opts.overshoot_errors);
  }

  kdu_sample_allocator allocator;
  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), env_ptr);
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution top_res = tc.access_resolution();
  kdu_node top_node = top_res.access_node();
  kdu_dims node_dims;
  top_node.get_dims(node_dims);
  result.out_width = node_dims.size.x;
  result.out_height = node_dims.size.y;

  if (env_ptr != nullptr) {
    env_ptr->attach_queue(&root_queue, nullptr, "synth_test_root");
  }

  kdu_push_pull_params pp_params;
  ScopedSynthesis syn =
      opts.use_resolution_ctor
          ? ScopedSynthesis(top_res, &allocator, pp_params, opts.sample_prec,
                            opts.normalization, env_ptr,
                            env_ptr != nullptr ? &root_queue : nullptr)
          : ScopedSynthesis(top_node, &allocator, pp_params, opts.sample_prec,
                            opts.normalization, opts.pull_offset, env_ptr,
                            env_ptr != nullptr ? &root_queue : nullptr,
                            opts.extended_info);

  const kdu_byte line_flags =
      opts.sample_prec | (opts.reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
  ScopedLineBuf line;
  line.line.pre_create(&allocator, result.out_width + opts.pull_offset,
                       line_flags, 0, opts.extend_right);
  allocator.finalize(cs.codestream);
  line.line.create();

  if (opts.call_start) {
    syn.engine.start(env_ptr);
    if (opts.call_start_twice) {
      syn.engine.start(env_ptr);
    }
  }

  const int lines_to_pull =
      opts.num_pull_lines >= 0
          ? std::min(opts.num_pull_lines, result.out_height)
          : result.out_height;
  const int64_t dc_offset =
      opts.is_signed ? 0 : (int64_t{1} << (opts.precision - 1));
  const double scale_p = static_cast<double>(int64_t{1} << opts.precision);

  result.int_samples.resize(static_cast<size_t>(result.out_width) *
                            lines_to_pull);
  result.float_samples.resize(static_cast<size_t>(result.out_width) *
                              lines_to_pull);

  for (int y = 0; y < lines_to_pull; ++y) {
    syn.engine.pull(line.line, env_ptr);
    const size_t row_off = static_cast<size_t>(y) * result.out_width;
    if (opts.sample_prec == KDU_LINE_BUF_SHORTS) {
      const kdu_sample16* buf = line.line.get_buf16() + opts.pull_offset;
      for (int x = 0; x < result.out_width; ++x) {
        if (opts.reversible) {
          result.int_samples[row_off + x] =
              static_cast<int64_t>(buf[x].ival) + dc_offset;
        } else {
          const double v = (static_cast<double>(buf[x].ival) /
                            static_cast<double>(1 << KDU_FIX_POINT)) /
                           static_cast<double>(opts.normalization);
          result.float_samples[row_off + x] = v * scale_p + dc_offset;
        }
      }
    } else if (opts.sample_prec == KDU_LINE_BUF_VPRECISE && opts.reversible) {
      const kdu_sample64* buf = line.line.get_buf64() + opts.pull_offset;
      for (int x = 0; x < result.out_width; ++x) {
        result.int_samples[row_off + x] =
            static_cast<int64_t>(buf[x].ival) + dc_offset;
      }
    } else {
      const kdu_sample32* buf = line.line.get_buf32() + opts.pull_offset;
      for (int x = 0; x < result.out_width; ++x) {
        if (opts.reversible) {
          result.int_samples[row_off + x] =
              static_cast<int64_t>(buf[x].ival) + dc_offset;
        } else {
          const double v = static_cast<double>(buf[x].fval) /
                           static_cast<double>(opts.normalization);
          result.float_samples[row_off + x] = v * scale_p + dc_offset;
        }
      }
    }
  }

  if (env_ptr != nullptr) {
    env_ptr->terminate(&root_queue, true);
  }
  syn.engine.destroy();
  tile.tile.close(env_ptr);
  if (env_ptr != nullptr) {
    env_ptr->terminate(&root_queue, false);
    env_ptr->cs_terminate(cs.codestream);
  }
  if (opts.set_overshoot) {
    result.overshoot_violated =
        cs.codestream.report_synth_overshoot(0, result.reported_overshoot);
  }
  cs.codestream.destroy();
  return result;
}

void VerifyReversibleRoundTrip(const SynthOptions& opts) {
  const std::vector<int64_t> src =
      MakePatternSamples(opts.width, opts.height, opts.precision,
                         opts.is_signed, opts.sharp_step_pattern);
  const std::vector<kdu_byte> bs = EncodeSingleComponent(opts, src);
  const DecodeResult dec = RunSynthesisDecode(bs, opts);
  ASSERT_EQ(dec.out_width, opts.width);
  ASSERT_EQ(dec.out_height, opts.height);
  if (!opts.transpose && opts.num_pull_lines < 0 && opts.max_layers == 0 &&
      !opts.has_roi) {
    EXPECT_EQ(dec.int_samples, src);
  }
}

void VerifyIrreversibleRoundTrip(const SynthOptions& opts, double max_abs_err) {
  const std::vector<int64_t> src =
      MakePatternSamples(opts.width, opts.height, opts.precision,
                         opts.is_signed, opts.sharp_step_pattern);
  const std::vector<kdu_byte> bs = EncodeSingleComponent(opts, src);
  const DecodeResult dec = RunSynthesisDecode(bs, opts);
  ASSERT_EQ(dec.out_width, opts.width);
  ASSERT_EQ(dec.out_height, opts.height);
  if (!opts.transpose && opts.num_pull_lines < 0 && opts.max_layers == 0 &&
      !opts.has_roi && max_abs_err > 0.0) {
    for (size_t i = 0; i < src.size(); ++i) {
      ASSERT_TRUE(std::isfinite(dec.float_samples[i]));
      EXPECT_NEAR(dec.float_samples[i], static_cast<double>(src[i]),
                  max_abs_err)
          << "index=" << i;
    }
  }
}

TEST(SynthesisTest, ReversibleW5X3AllPrecisionsAndConstructors) {
  // 16-bit, 32-bit, and 64-bit reversible multi-level W5X3 via node and
  // resolution constructors, including wide (SIMD) and narrow (scalar) widths,
  // odd origins, lazy pull() without explicit start(), and double start().
  for (kdu_byte prec :
       {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0), KDU_LINE_BUF_VPRECISE}) {
    SynthOptions multi_level;
    multi_level.width = 40;
    multi_level.height = 36;
    multi_level.ox = 3;
    multi_level.oy = 5;
    multi_level.dwt_levels = 2;
    multi_level.sample_prec = prec;
    multi_level.use_resolution_ctor = (prec == KDU_LINE_BUF_SHORTS);
    multi_level.call_start = (prec != 0);
    multi_level.call_start_twice = (prec == KDU_LINE_BUF_VPRECISE);
    VerifyReversibleRoundTrip(multi_level);

    SynthOptions narrow;
    narrow.width = 7;
    narrow.height = 9;
    narrow.ox = 1;
    narrow.oy = 1;
    narrow.sample_prec = prec;
    VerifyReversibleRoundTrip(narrow);
  }
}

TEST(SynthesisTest, IrreversibleW9X7AllPrecisionsAndNormalizations) {
  // 16-bit W9X7 with normalizing_upshift > 0 (norm=2 on 8-bit, norm=128 on
  // low-amplitude 16-bit) and == 0 (norm=1), 32-bit float W9X7, and
  // KDU_LINE_BUF_VPRECISE fallback to 32-bit float.
  for (int w : {9, 48}) {
    SynthOptions opts16;
    opts16.width = w;
    opts16.height = 22;
    opts16.ox = (w == 9) ? 1 : 0;
    opts16.oy = (w == 9) ? 1 : 0;
    opts16.reversible = false;
    opts16.dwt_levels = 2;
    opts16.extra_params = {"Ckernels=W9X7"};
    opts16.sample_prec = KDU_LINE_BUF_SHORTS;
    opts16.normalization = (w == 9) ? 1.0F : 2.0F;
    VerifyIrreversibleRoundTrip(opts16, 4.0);

    SynthOptions opts32 = opts16;
    opts32.sample_prec = 0;
    opts32.normalization = 1.0F;
    opts32.use_resolution_ctor = true;
    VerifyIrreversibleRoundTrip(opts32, 3.0);
  }

  SynthOptions large_norm16;
  large_norm16.width = 48;
  large_norm16.height = 24;
  large_norm16.precision = 16;
  large_norm16.reversible = false;
  large_norm16.dwt_levels = 2;
  large_norm16.extra_params = {"Ckernels=W9X7", "Qstep=0.0005"};
  large_norm16.sample_prec = KDU_LINE_BUF_SHORTS;
  large_norm16.normalization = 128.0F;
  VerifyIrreversibleRoundTrip(large_norm16, 160.0);

  SynthOptions vprecise_fallback;
  vprecise_fallback.width = 24;
  vprecise_fallback.height = 18;
  vprecise_fallback.reversible = false;
  vprecise_fallback.extra_params = {"Ckernels=W9X7"};
  vprecise_fallback.sample_prec = KDU_LINE_BUF_VPRECISE;
  vprecise_fallback.normalization = 1.0F;
  VerifyIrreversibleRoundTrip(vprecise_fallback, 3.0);
}

TEST(SynthesisTest, TransposedVerticalFirstAllPrecisionsAndEarlyDestroy) {
  // Transposed reversible 2D synthesis selects vertical_first and triggers
  // tmp != vline_out copy across 16-bit, 32-bit, and 64-bit precisions.
  for (kdu_byte prec :
       {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0), KDU_LINE_BUF_VPRECISE}) {
    SynthOptions transposed;
    transposed.width = 24;
    transposed.height = 20;
    transposed.ox = 1;
    transposed.oy = 1;
    transposed.sample_prec = prec;
    transposed.transpose = true;
    const std::vector<int64_t> src =
        MakePatternSamples(transposed.width, transposed.height,
                           transposed.precision, transposed.is_signed, false);
    const std::vector<kdu_byte> bs = EncodeSingleComponent(transposed, src);
    const DecodeResult dec = RunSynthesisDecode(bs, transposed);
    EXPECT_EQ(dec.out_width, 20);
    EXPECT_EQ(dec.out_height, 24);
    for (int y = 0; y < 20; ++y) {
      for (int x = 0; x < 24; ++x) {
        EXPECT_EQ(dec.int_samples[static_cast<size_t>(x) * 20 + y],
                  src[static_cast<size_t>(y) * 24 + x]);
      }
    }
  }

  // Pull fewer than full height to verify partial teardown.
  SynthOptions early;
  early.width = 32;
  early.height = 24;
  early.dwt_levels = 2;
  early.num_pull_lines = 5;
  const std::vector<int64_t> src = MakePatternSamples(32, 24, 8, false, false);
  const std::vector<kdu_byte> bs = EncodeSingleComponent(early, src);
  const DecodeResult dec = RunSynthesisDecode(bs, early);
  EXPECT_EQ(dec.int_samples.size(), 32U * 5U);
}

TEST(SynthesisTest, DirectionalDecompositionsAndDegenerateSizes) {
  // Horizontal-only Cdecomp=V(-) and vertical-only Cdecomp=H(-) across all
  // precisions (plus irreversible W9X7 for 16-bit and 32-bit float).
  for (const std::string& decomp : {"Cdecomp=V(-)", "Cdecomp=H(-)"}) {
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0),
                          KDU_LINE_BUF_VPRECISE}) {
      SynthOptions rev;
      rev.width = 32;
      rev.height = 18;
      rev.ox = 1;
      rev.oy = 1;
      rev.extra_params = {decomp};
      rev.sample_prec = prec;
      VerifyReversibleRoundTrip(rev);
    }
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0)}) {
      SynthOptions irrev;
      irrev.width = 32;
      irrev.height = 18;
      irrev.reversible = false;
      irrev.extra_params = {"Ckernels=W9X7", decomp};
      irrev.sample_prec = prec;
      irrev.normalization = (prec == KDU_LINE_BUF_SHORTS) ? 2.0F : 1.0F;
      VerifyIrreversibleRoundTrip(irrev, 3.0);
    }
  }

  // Packet wavelet multi-level decomposition Cdecomp=H(B(-:-:-)),B(-:-:-).
  SynthOptions packet;
  packet.width = 24;
  packet.height = 20;
  packet.dwt_levels = 2;
  packet.extra_params = {"Cdecomp=H(-),V(-)"};
  packet.sample_prec = 0;
  VerifyReversibleRoundTrip(packet);

  // Unit height (1 row) and unit width (1 column) at even and odd origins
  // across 16-bit, 32-bit, 64-bit reversible and 16-bit/32-bit irreversible.
  for (int parity : {0, 1}) {
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0),
                          KDU_LINE_BUF_VPRECISE}) {
      SynthOptions uh;
      uh.width = 16;
      uh.height = 1;
      uh.oy = parity;
      uh.sample_prec = prec;
      VerifyReversibleRoundTrip(uh);

      SynthOptions uw;
      uw.width = 1;
      uw.height = 12;
      uw.ox = parity;
      uw.sample_prec = prec;
      VerifyReversibleRoundTrip(uw);
    }
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0)}) {
      SynthOptions uh_i;
      uh_i.width = 16;
      uh_i.height = 1;
      uh_i.oy = parity;
      uh_i.reversible = false;
      uh_i.extra_params = {"Ckernels=W9X7"};
      uh_i.sample_prec = prec;
      uh_i.normalization = (prec == KDU_LINE_BUF_SHORTS) ? 2.0F : 1.0F;
      VerifyIrreversibleRoundTrip(uh_i, 3.0);

      SynthOptions uw_i = uh_i;
      uw_i.width = 1;
      uw_i.height = 12;
      uw_i.ox = parity;
      uw_i.oy = 0;
      VerifyIrreversibleRoundTrip(uw_i, 3.0);
    }
  }
}

TEST(SynthesisTest, EmptyDimensionNode) {
  // Encode a 2-component codestream where component 1 has size 0 due to
  // Sorigin=(1,1), Ssize=(2,2), Ssampling=(2,2) => subsampled [1,1).
  std::vector<kdu_byte> bitstream;
  {
    VectorTarget target(bitstream);
    ScopedCodestream cs;
    EXPECT_TRUE(cs.siz.parse_string("Scomponents=2"));
    cs.siz.set(Sorigin, 0, 0, 1);
    cs.siz.set(Sorigin, 0, 1, 1);
    cs.siz.set(Ssize, 0, 0, 5);
    cs.siz.set(Ssize, 0, 1, 5);
    cs.siz.set(Ssampling, 0, 0, 1);
    cs.siz.set(Ssampling, 0, 1, 1);
    cs.siz.set(Sprecision, 0, 0, 8);
    cs.siz.set(Ssigned, 0, 0, false);
    cs.siz.set(Ssampling, 1, 0, 2);
    cs.siz.set(Ssampling, 1, 1, 2);
    cs.siz.set(Sprecision, 1, 0, 8);
    cs.siz.set(Ssigned, 1, 0, false);
    static_cast<kdu_params*>(&cs.siz)->finalize();

    cs.codestream.create(&cs.siz, &target);
    EXPECT_TRUE(cs.codestream.access_siz()->parse_string("Creversible=yes"));
    cs.codestream.access_siz()->finalize_all();
    kdu_sample_allocator allocator;
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_push_pull_params pp;
    ScopedAnalysis ana0(tile.tile.access_component(0).access_resolution(),
                        &allocator, pp, KDU_LINE_BUF_SHORTS);
    ScopedAnalysis ana1(tile.tile.access_component(1).access_resolution(),
                        &allocator, pp, KDU_LINE_BUF_SHORTS);
    kdu_dims dims1;
    tile.tile.access_component(1).access_resolution().get_dims(dims1);
    kdu_line_buf line0;
    line0.pre_create(&allocator, 4, KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,
                     0, 0);
    kdu_line_buf line1;
    if (dims1.area() > 0) {
      line1.pre_create(&allocator, dims1.size.x,
                       KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0, 0);
    }
    allocator.finalize(cs.codestream);
    line0.create();
    if (dims1.area() > 0) {
      line1.create();
    }
    ana0.engine.start(nullptr);
    ana1.engine.start(nullptr);
    for (int y = 0; y < 4; ++y) {
      std::memset(line0.get_buf16(), 0, 4 * sizeof(kdu_sample16));
      while (!ana0.engine.push(line0, nullptr)) {
      }
    }
    for (int y = 0; y < dims1.size.y; ++y) {
      std::memset(line1.get_buf16(), 0,
                  static_cast<size_t>(dims1.size.x) * sizeof(kdu_sample16));
      while (!ana1.engine.push(line1, nullptr)) {
      }
    }
    while (!ana0.engine.finish(nullptr)) {
    }
    while (!ana1.engine.finish(nullptr)) {
    }
    ana0.engine.destroy();
    ana1.engine.destroy();
    if (dims1.area() > 0) {
      line1.destroy();
    }
    line0.destroy();
    tile.tile.close();
    kdu_long max_bytes = 1 << 20;
    cs.codestream.flush(&max_bytes, 1);
    cs.codestream.destroy();
  }

  // Now shrink component 1 region to zero by applying a 1x1 ROI at (1, 1).
  VectorSource source(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&source);
  kdu_dims empty_roi;
  empty_roi.pos = kdu_coords(1, 1);
  empty_roi.size = kdu_coords(1, 1);
  cs.codestream.apply_input_restrictions(0, 0, 0, 0, &empty_roi,
                                         KDU_WANT_OUTPUT_COMPONENTS);
  kdu_sample_allocator allocator;
  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  kdu_resolution res = tile.tile.access_component(1).access_resolution();
  kdu_dims rdims;
  res.get_dims(rdims);
  EXPECT_EQ(rdims.area(), 0);
  kdu_push_pull_params pp;
  ScopedSynthesis syn(res, &allocator, pp, KDU_LINE_BUF_SHORTS);
  allocator.finalize(cs.codestream);
  syn.engine.start(nullptr);
  kdu_line_buf empty_line;
  syn.engine.pull(empty_line, nullptr);
  syn.engine.destroy();
  tile.tile.close();
  cs.codestream.destroy();
}

TEST(SynthesisTest, CustomAtkKernelsAllPrecisionsAndLongSupport) {
  // 1. Custom reversible ATK 5/3 with Kextension=SYM hits i_lambda == -1 and +1
  // scalar lifting loops across 16-bit, 32-bit, and 64-bit.
  const std::vector<std::string> atk_rev_5x3 = {
      "Ckernels=ATK",
      "Catk=3",
      "Kreversible:I3=yes",
      "Ksymmetric:I3=yes",
      "Kextension:I3=SYM",
      "Ksteps:I3={2,0,1,1},{2,-1,2,2}",
      "Kcoeffs:I3=-0.5,-0.5,0.25,0.25",
  };
  // 2. Symmetric 2-tap reversible ATK with i_lambda == 3 (`0.375` = 3/8) hits
  // `i_lambda != +-1`.
  const std::vector<std::string> atk_sym_3_8 = {
      "Ckernels=ATK",
      "Catk=4",
      "Kreversible:I4=yes",
      "Ksymmetric:I4=yes",
      "Kextension:I4=SYM",
      "Ksteps:I4={2,0,1,1},{2,-1,3,4}",
      "Kcoeffs:I4=-0.5,-0.5,0.375,0.375",
  };
  // 3. Reversible asymmetric 6-step ATK with CON extension, 5-tap step and
  // zero-length step (support_length == 0, support_length == 5, >4 steps).
  const std::vector<std::string> atk_many_rev = {
      "Ckernels=ATK",
      "Catk=5",
      "Kreversible:I5=yes",
      "Ksymmetric:I5=no",
      "Kextension:I5=CON",
      "Ksteps:I5={5,-2,2,2},{2,-1,2,2},{0,0,0,0},{4,-1,2,2},{2,0,1,1},{2,-1,2,"
      "2}",
      "Kcoeffs:I5=-0.25,-0.5,-0.25,0.0,0.0,0.25,0.25,-0.25,0.25,0.25,-0.25,0."
      "5,-0.5,0.25,0.25",
  };
  for (const auto& extra : {atk_rev_5x3, atk_sym_3_8, atk_many_rev}) {
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0),
                          KDU_LINE_BUF_VPRECISE}) {
      SynthOptions opts;
      opts.width = 32;
      opts.height = 24;
      opts.precision = 12;
      opts.extra_params = extra;
      opts.sample_prec = prec;
      VerifyReversibleRoundTrip(opts);
    }
  }

  // 4. Built-in I5X3 ATK + irreversible 6-step ATK (with 0-tap + 5-tap).
  const std::vector<std::string> atk_irrev_i5x3 = {"Ckernels=ATK", "Catk=7",
                                                   "Kkernels:I7=I5X3"};
  const std::vector<std::string> atk_many_irrev = {
      "Ckernels=ATK",
      "Catk=6",
      "Kreversible:I6=no",
      "Ksymmetric:I6=no",
      "Kextension:I6=CON",
      "Ksteps:I6={5,-2,0,0},{0,0,0,0},{2,0,0,0},{4,-1,0,0},{2,0,0,0},{2,-1,0,"
      "0}",
      "Kcoeffs:I6=-0.25,-0.5,-0.25,0.0,0.0,0.1,-0.1,-0.1,0.2,0.2,-0.1,0.2,-0."
      "2,0.25,0.25",
  };
  for (const auto& extra : {atk_irrev_i5x3, atk_many_irrev}) {
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0)}) {
      SynthOptions opts;
      opts.width = 32;
      opts.height = 24;
      opts.reversible = false;
      opts.extra_params = extra;
      opts.sample_prec = prec;
      opts.normalization = (prec == KDU_LINE_BUF_SHORTS) ? 2.0F : 1.0F;
      VerifyIrreversibleRoundTrip(opts, 16.0);
    }
  }
}

TEST(SynthesisTest, OvershootMonitoringPullOffsetRoiExtendAndThreads) {
  // 1. Overshoot monitoring (monitor_max_value = true) across 16-bit rev/irrev,
  // 32-bit int/float, 64-bit rev, with hor_xform_exists and !hor_xform_exists,
  // and both narrow and wide widths.
  for (const std::string& decomp : {"Cdecomp=B(-:-:-)", "Cdecomp=V(-)"}) {
    // Irreversible ringing with coarse layer truncation + overshoot_limit=0.0f.
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0)}) {
      for (int width : {16, 48}) {
        SynthOptions opts;
        opts.width = width;
        opts.height = 24;
        opts.reversible = false;
        opts.extra_params = {"Ckernels=W9X7", "Clayers=2", decomp};
        opts.sample_prec = prec;
        opts.normalization = (prec == KDU_LINE_BUF_SHORTS) ? 2.0F : 1.0F;
        opts.sharp_step_pattern = true;
        opts.flush_bytes = {120, 1 << 20};
        opts.set_overshoot = true;
        opts.overshoot_limit = 0.0F;
        opts.max_layers = 1;
        const std::vector<int64_t> src = MakePatternSamples(
            opts.width, opts.height, opts.precision, opts.is_signed, true);
        const std::vector<kdu_byte> bs = EncodeSingleComponent(opts, src);
        const DecodeResult dec = RunSynthesisDecode(bs, opts);
        EXPECT_EQ(dec.out_width, opts.width);

        // Also exercise normalization=1.0F (normalizing_upshift == 0) on
        // SHORTS.
        if (prec == KDU_LINE_BUF_SHORTS && width == 48) {
          SynthOptions zero_upshift = opts;
          zero_upshift.normalization = 1.0F;
          zero_upshift.max_layers = 0;
          const DecodeResult dec2 = RunSynthesisDecode(bs, zero_upshift);
          EXPECT_EQ(dec2.out_width, opts.width);
        }
      }
    }

    // Reversible with 2 layers and max_layers=1 triggers rev overshoot check.
    for (kdu_byte prec : {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0),
                          KDU_LINE_BUF_VPRECISE}) {
      SynthOptions rev_mon;
      rev_mon.width = 48;
      rev_mon.height = 24;
      rev_mon.extra_params = {"Clayers=2", decomp};
      rev_mon.sample_prec = prec;
      rev_mon.sharp_step_pattern = true;
      rev_mon.flush_bytes = {120, 1 << 20};
      rev_mon.set_overshoot = true;
      rev_mon.overshoot_limit = 0.0F;
      rev_mon.max_layers = 1;
      const std::vector<int64_t> src =
          MakePatternSamples(rev_mon.width, rev_mon.height, rev_mon.precision,
                             rev_mon.is_signed, true);
      const std::vector<kdu_byte> bs = EncodeSingleComponent(rev_mon, src);
      const DecodeResult dec = RunSynthesisDecode(bs, rev_mon);
      EXPECT_EQ(dec.out_width, rev_mon.width);
    }
  }

  // 2. KDU_LINE_WILL_BE_EXTENDED, extend_right, and restricted ROI with
  // asymmetric kernel + SYM extension (triggers left_fill / right_fill and
  // internal child pull offsets across DWT levels).
  const std::vector<std::string> asym_sym = {
      "Ckernels=ATK",
      "Catk=5",
      "Kreversible:I5=yes",
      "Ksymmetric:I5=no",
      "Kextension:I5=SYM",
      "Ksteps:I5={3,-1,2,2},{2,-1,2,2}",
      "Kcoeffs:I5=-0.25,-0.75,0.0,0.25,0.25",
  };
  const std::vector<int64_t> roi_src =
      MakePatternSamples(48, 32, 8, false, false);
  for (kdu_byte prec :
       {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0), KDU_LINE_BUF_VPRECISE}) {
    SynthOptions roi;
    roi.width = 48;
    roi.height = 32;
    roi.dwt_levels = 2;
    roi.extra_params = asym_sym;
    roi.sample_prec = prec;
    roi.pull_offset = 0;
    roi.extend_right = 1;
    roi.extended_info = KDU_LINE_WILL_BE_EXTENDED;
    roi.has_roi = true;
    roi.roi_dims.pos = kdu_coords(7, 5);
    roi.roi_dims.size = kdu_coords(23, 19);
    const std::vector<kdu_byte> bs = EncodeSingleComponent(roi, roi_src);
    const DecodeResult dec = RunSynthesisDecode(bs, roi);
    ASSERT_EQ(dec.out_width, 23);
    ASSERT_EQ(dec.out_height, 19);
    ASSERT_EQ(dec.int_samples.size(), size_t{23} * 19);
    for (int y = 0; y < 19; ++y) {
      for (int x = 0; x < 23; ++x) {
        EXPECT_EQ(dec.int_samples[static_cast<size_t>(y) * 23 + x],
                  roi_src[static_cast<size_t>(5 + y) * 48 + (7 + x)]);
      }
    }
  }

  // 3. Multi-threaded synthesis via kdu_thread_env + kdu_thread_queue.
  SynthOptions mt;
  mt.width = 64;
  mt.height = 48;
  mt.dwt_levels = 2;
  mt.sample_prec = KDU_LINE_BUF_SHORTS;
  mt.num_threads = 3;
  VerifyReversibleRoundTrip(mt);
}

}  // namespace
}  // namespace kdu_core
