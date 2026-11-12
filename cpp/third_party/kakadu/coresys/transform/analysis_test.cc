#include <algorithm>
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
  void put_text(const char* text) override { pending_ += text; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      std::fprintf(stderr, "KDU_ERROR: %s\n", pending_.c_str());
      last_message_ = pending_;
      pending_.clear();
      kdu_thread_safe_message::flush(end_of_message);
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_thread_safe_message::flush(end_of_message);
  }

  const std::string& last_message() const { return last_message_; }

 private:
  std::string pending_;
  std::string last_message_;
};

kdu_thread_safe_message* GlobalErrorHandler() {
  static auto* handler = new ThrowingErrorHandler();
  return handler;
}

class AnalysisTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { kdu_customize_errors(GlobalErrorHandler()); }
};

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
  kdu_thread_env* env = nullptr;

  ~ScopedTile() {
    if (tile.exists()) {
      tile.close(env);
    }
  }
};

struct ScopedAnalysis {
  kdu_push_ifc engine;
  kdu_thread_env* env = nullptr;

  ~ScopedAnalysis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedLineBuf {
  kdu_line_buf line;

  ~ScopedLineBuf() { line.destroy(); }
};

class TestRoiNode final : public kdu_roi_node {
 public:
  explicit TestRoiNode(kdu_byte fill_val) : fill_val_(fill_val) {}

  void release() override { released_ = true; }

  void pull(kdu_byte buf[], int width) override {
    ++pull_calls_;
    if (width > 0) {
      std::memset(buf, fill_val_, static_cast<size_t>(width));
    }
  }

  bool released() const { return released_; }
  int pull_calls() const { return pull_calls_; }

 private:
  kdu_byte fill_val_;
  bool released_ = false;
  int pull_calls_ = 0;
};

void SetupCodestream(ScopedCodestream& ctx, VectorTarget& target, int width,
                     int height, int bit_depth, bool is_signed, bool reversible,
                     int clevels, const std::vector<std::string>& extra_params,
                     int origin_y = 0, int origin_x = 0, int sampling_x = 1) {
  const std::string ssize = "Ssize={" + std::to_string(origin_y + height) +
                            "," + std::to_string(origin_x + width) + "}";
  const std::string sdims =
      "Sdims={" + std::to_string(height) + "," + std::to_string(width) + "}";
  const std::string sorigin = "Sorigin={" + std::to_string(origin_y) + "," +
                              std::to_string(origin_x) + "}";
  const std::string ssamp = "Ssampling={1," + std::to_string(sampling_x) + "}";
  const std::string sprec = "Sprecision=" + std::to_string(bit_depth);
  const std::string ssigned =
      std::string("Ssigned=") + (is_signed ? "yes" : "no");
  ASSERT_TRUE(ctx.siz.parse_string("Scomponents=1"));
  ASSERT_TRUE(ctx.siz.parse_string(ssize.c_str()));
  if (sampling_x == 1) {
    ASSERT_TRUE(ctx.siz.parse_string(sdims.c_str()));
  }
  ASSERT_TRUE(ctx.siz.parse_string(sorigin.c_str()));
  ASSERT_TRUE(ctx.siz.parse_string(ssamp.c_str()));
  ASSERT_TRUE(ctx.siz.parse_string(sprec.c_str()));
  ASSERT_TRUE(ctx.siz.parse_string(ssigned.c_str()));
  static_cast<kdu_params&>(ctx.siz).finalize();

  ctx.codestream.create(&ctx.siz, &target);
  ctx.codestream.set_persistent();
  kdu_params* cod = ctx.codestream.access_siz()->access_cluster(COD_params);
  ASSERT_NE(cod, nullptr);
  ASSERT_TRUE(cod->parse_string(
      (std::string("Creversible=") + (reversible ? "yes" : "no")).c_str()));
  ASSERT_TRUE(
      cod->parse_string(("Clevels=" + std::to_string(clevels)).c_str()));
  for (const std::string& p : extra_params) {
    ASSERT_TRUE(ctx.codestream.access_siz()->parse_string(p.c_str()))
        << "Failed param: " << p;
  }
}

void FillSampleLine(kdu_line_buf& line, int width, int offset, int y,
                    bool is_shorts, bool is_vprecise, bool is_absolute,
                    float scale = 0.001f) {
  if (is_shorts) {
    kdu_sample16* buf = line.get_buf16();
    ASSERT_NE(buf, nullptr);
    const int total = width + offset;
    for (int x = 0; x < total; ++x) {
      buf[x].ival = static_cast<kdu_int16>(((x + y * 7) % 97) - 48 +
                                           (is_absolute ? 0 : 128));
    }
  } else if (is_vprecise && is_absolute) {
    kdu_sample64* buf = line.get_buf64();
    ASSERT_NE(buf, nullptr);
    const int total = width + offset;
    for (int x = 0; x < total; ++x) {
      buf[x].ival = static_cast<kdu_int64>(((x - y * 5) % 500) - 250);
    }
  } else {
    kdu_sample32* buf = line.get_buf32();
    ASSERT_NE(buf, nullptr);
    const int total = width + offset;
    for (int x = 0; x < total; ++x) {
      if (is_absolute) {
        buf[x].ival = ((x * 3 + y * 11) % 301) - 150;
      } else {
        buf[x].fval = static_cast<float>(((x + y) % 64) - 32) * scale;
      }
    }
  }
}

void PushAllLines(kdu_push_ifc& engine, kdu_line_buf& line, int width,
                  int height, int offset, bool is_shorts, bool is_vprecise,
                  bool is_absolute, float scale = 0.001f) {
  for (int y = 0; y < height; ++y) {
    ASSERT_NO_FATAL_FAILURE(FillSampleLine(line, width, offset, y, is_shorts,
                                           is_vprecise, is_absolute, scale));
    while (!engine.push(line, nullptr)) {
    }
  }
  EXPECT_TRUE(engine.finish(nullptr));
  // Calling finish a second time exercises the idempotent !finished branch.
  EXPECT_TRUE(engine.finish(nullptr));
}

void RunNodeAnalysisCase(int width, int height, int bit_depth, bool is_signed,
                         bool reversible, int clevels,
                         const std::vector<std::string>& extra_params,
                         kdu_byte sample_prec, float normalization = 1.0f,
                         int push_offset = 0, bool call_start = true,
                         bool transpose = false, bool vflip = false,
                         bool hflip = false, int origin_y = 0, int origin_x = 0,
                         int extended_flags = 0) {
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(SetupCodestream(cs, target, width, height, bit_depth,
                                          is_signed, reversible, clevels,
                                          extra_params, origin_y, origin_x));
  cs.codestream.access_siz()->finalize_all();
  if (transpose || vflip || hflip) {
    cs.codestream.change_appearance(transpose, vflip, hflip);
  }

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  ASSERT_TRUE(tile.tile.exists());

  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);
  kdu_node top_node = top_res.access_node();
  kdu_dims dims;
  top_node.get_dims(dims);

  kdu_sample_allocator allocator;
  kdu_push_pull_params params;
  ScopedLineBuf line;
  const kdu_byte line_flags =
      sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
  line.line.pre_create(&allocator, dims.size.x + push_offset, line_flags, 0, 0);

  ScopedAnalysis analysis;
  analysis.engine = kdu_analysis(top_node, &allocator, params, sample_prec,
                                 normalization, push_offset, nullptr, nullptr,
                                 nullptr, nullptr, extended_flags);
  allocator.finalize(cs.codestream);
  line.line.create();
  if (call_start) {
    analysis.engine.start(nullptr);
    // Second start() verifies the already-initialized guard.
    analysis.engine.start(nullptr);
  } else {
    // Subbands must still be started before push() feeds them when top-level
    // start() is omitted so lazy initialization in push() runs safely.
    // Let top_node children stay compatible via standard start path if needed;
    // when top-level has no vert xform we still want start() lazy branch only
    // when subbands don't require pre-start, or test start() on child.
  }

  const bool is_shorts = (sample_prec == KDU_LINE_BUF_SHORTS);
  const bool is_vprecise = (sample_prec == KDU_LINE_BUF_VPRECISE);
  ASSERT_NO_FATAL_FAILURE(PushAllLines(analysis.engine, line.line, dims.size.x,
                                       dims.size.y, push_offset, is_shorts,
                                       is_vprecise, reversible));
  analysis.engine.destroy();
  tile.tile.close();
  if (cs.codestream.ready_for_flush()) {
    kdu_long max_bytes = 1 << 20;
    cs.codestream.flush(&max_bytes, 1);
    EXPECT_FALSE(encoded.empty());
  }
}

TEST_F(AnalysisTest, ReversibleAndIrreversible16BitPaths) {
  // Narrow width (< 16 samples -> ilv_pairs < 8) keeps deinterleave_16_func ==
  // NULL and exercises odd/even origins with normalization downshift = 0 and
  // >0.
  RunNodeAnalysisCase(10, 12, 8, false, true, 2, {}, KDU_LINE_BUF_SHORTS, 1.0f,
                      0, true, false, false, false, 1, 1);
  // Wide width (>= 32 samples) exercises SIMD deinterleave_16 + W5X3/W9X7.
  RunNodeAnalysisCase(36, 20, 10, true, true, 2, {}, KDU_LINE_BUF_SHORTS, 1.0f,
                      4, true, false, false, false, 0, 0);
  // Large normalization triggers BIBO overflow normalizing_downshift > 0
  // alongside W9X7 vert_add_shorts_first on 16-bit deinterleave (narrow +
  // wide).
  RunNodeAnalysisCase(10, 14, 8, false, false, 2, {"Ckernels=W9X7"},
                      KDU_LINE_BUF_SHORTS, 128.0f);
  RunNodeAnalysisCase(36, 18, 8, false, false, 2, {"Ckernels=W9X7"},
                      KDU_LINE_BUF_SHORTS, 128.0f);
  // Vertical-only transform (`!hor_xform_exists`) with 16-bit normalization
  // downshift == 0 and > 0.
  RunNodeAnalysisCase(16, 12, 8, false, true, 1, {"Cdecomp=V(-)"},
                      KDU_LINE_BUF_SHORTS, 1.0f);
  RunNodeAnalysisCase(16, 12, 8, false, false, 1,
                      {"Ckernels=W9X7", "Cdecomp=V(-)"}, KDU_LINE_BUF_SHORTS,
                      128.0f);
}

TEST_F(AnalysisTest, ResolutionAndLegacyGdalConstructorsWith32Bit) {
  // Exercise kdu_analysis(kdu_resolution, ..., sample_prec=0) and
  // KDU_LINE_BUF_VPRECISE fallback when Creversible=no.
  for (bool use_irrev : {false, true}) {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    const std::vector<std::string> extra =
        use_irrev ? std::vector<std::string>{"Ckernels=W9X7"}
                  : std::vector<std::string>{};
    ASSERT_NO_FATAL_FAILURE(
        SetupCodestream(cs, target, 24, 18, 12, true, !use_irrev, 2, extra));
    cs.codestream.access_siz()->finalize_all();
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);

    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    ScopedLineBuf line;
    const kdu_byte req_prec = use_irrev ? KDU_LINE_BUF_VPRECISE : 0;
    const kdu_byte line_flags =
        req_prec | (!use_irrev ? KDU_LINE_BUF_ABSOLUTE : 0);
    line.line.pre_create(&allocator, 24, line_flags, 0, 0);

    ScopedAnalysis analysis;
    analysis.engine =
        kdu_analysis(top_res, &allocator, params, req_prec, 1.0f, 0, nullptr);
    allocator.finalize(cs.codestream);
    line.line.create();
    analysis.engine.start(nullptr);
    ASSERT_NO_FATAL_FAILURE(PushAllLines(analysis.engine, line.line, 24, 18, 0,
                                         false, false, !use_irrev));
    analysis.engine.destroy();
    tile.tile.close();
  }

  // Exercise the Google-preserved deprecated GDAL constructor (use_shorts=true
  // and use_shorts=false).
  for (bool use_shorts : {true, false}) {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(
        SetupCodestream(cs, target, 20, 16, 8, false, true, 1, {}));
    cs.codestream.access_siz()->finalize_all();
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);

    kdu_sample_allocator allocator;
    ScopedLineBuf line;
    const kdu_byte flags =
        (use_shorts ? KDU_LINE_BUF_SHORTS : 0) | KDU_LINE_BUF_ABSOLUTE;
    line.line.pre_create(&allocator, 20, flags, 0, 0);

    ScopedAnalysis analysis;
    analysis.engine = kdu_analysis(top_res, &allocator, use_shorts);
    allocator.finalize(cs.codestream);
    line.line.create();
    analysis.engine.start(nullptr);
    ASSERT_NO_FATAL_FAILURE(PushAllLines(analysis.engine, line.line, 20, 16, 0,
                                         use_shorts, false, true));
    analysis.engine.destroy();
    tile.tile.close();
  }

  // Vertical-only and horizontal-only 32-bit paths.
  RunNodeAnalysisCase(18, 14, 12, false, true, 1, {"Cdecomp=V(-)"}, 0);
  RunNodeAnalysisCase(18, 14, 12, false, false, 1,
                      {"Ckernels=W9X7", "Cdecomp=H(-)"}, 0);
}

TEST_F(AnalysisTest, Precise64BitAndDegenerateUnitDimensions) {
  // 64-bit W5X3 2D analysis (uses KDU_LINE_BUF_VPRECISE end-to-end).
  RunNodeAnalysisCase(22, 16, 16, true, true, 2, {}, KDU_LINE_BUF_VPRECISE);
  // 64-bit vertical-only (`!hor_xform_exists`) and horizontal-only.
  RunNodeAnalysisCase(16, 12, 16, false, true, 1, {"Cdecomp=V(-)"},
                      KDU_LINE_BUF_VPRECISE);
  RunNodeAnalysisCase(16, 12, 16, false, true, 1, {"Cdecomp=H(-)"},
                      KDU_LINE_BUF_VPRECISE);

  // Unit height (1 row) at odd y origin (`vsub_parity == 1`) doubles samples on
  // 16-bit, 32-bit, and 64-bit reversible paths; also even y origin.
  for (int oy : {0, 1}) {
    RunNodeAnalysisCase(12, 1, 16, false, true, 1, {}, KDU_LINE_BUF_SHORTS,
                        1.0f, 0, true, false, false, false, oy, 0);
    RunNodeAnalysisCase(12, 1, 16, false, true, 1, {}, 0, 1.0f, 0, true, false,
                        false, false, oy, 0);
    RunNodeAnalysisCase(12, 1, 16, false, true, 1, {}, KDU_LINE_BUF_VPRECISE,
                        1.0f, 0, true, false, false, false, oy, 0);
  }

  // Unit width (1 col) at odd x origin (`x_min & 1`) doubles samples on 16-bit,
  // 32-bit, and 64-bit reversible paths; also even x origin.
  for (int ox : {0, 1}) {
    RunNodeAnalysisCase(1, 10, 16, false, true, 1, {}, KDU_LINE_BUF_SHORTS,
                        1.0f, 0, true, false, false, false, 0, ox);
    RunNodeAnalysisCase(1, 10, 16, false, true, 1, {}, 0, 1.0f, 0, true, false,
                        false, false, 0, ox);
    RunNodeAnalysisCase(1, 10, 16, false, true, 1, {}, KDU_LINE_BUF_VPRECISE,
                        1.0f, 0, true, false, false, false, 0, ox);
  }
}

TEST_F(AnalysisTest, AtkScalar16Bit32Bit64BitAndManySteps) {
  // Reversible 5/3 expressed via custom ATK hits i_lambda == -1 and +1 scalar
  // horizontal branches across 16-bit, 32-bit, and 64-bit (no SSE2 5x3).
  const std::vector<std::string> atk_rev_5x3 = {
      "Ckernels=ATK",
      "Catk=3",
      "Kreversible:I3=yes",
      "Ksymmetric:I3=yes",
      "Kextension:I3=SYM",
      "Ksteps:I3={2,0,1,1},{2,-1,2,2}",
      "Kcoeffs:I3=-0.5,-0.5,0.25,0.25",
  };
  for (kdu_byte prec :
       {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0), KDU_LINE_BUF_VPRECISE}) {
    RunNodeAnalysisCase(24, 18, 12, false, true, 1, atk_rev_5x3, prec);
  }
  // Built-in irreversible I5X3 ATK exercises non-reversible ATK with 16-bit and
  // 32-bit samples.
  const std::vector<std::string> atk_irrev_i5x3 = {"Ckernels=ATK", "Catk=7",
                                                   "Kkernels:I7=I5X3"};
  RunNodeAnalysisCase(24, 18, 8, false, false, 1, atk_irrev_i5x3,
                      KDU_LINE_BUF_SHORTS);
  RunNodeAnalysisCase(24, 18, 8, false, false, 1, atk_irrev_i5x3, 0);

  // Symmetric 2-tap reversible ATK with i_lambda == 3 (`0.375` = 3/8) on step 1
  // (low-pass DC gain = 1 since step 0 sums to -1) hits `i_lambda != +-1`.
  const std::vector<std::string> atk_sym_3_8 = {
      "Ckernels=ATK",
      "Catk=4",
      "Kreversible:I4=yes",
      "Ksymmetric:I4=yes",
      "Kextension:I4=SYM",
      "Ksteps:I4={2,0,1,1},{2,-1,3,4}",
      "Kcoeffs:I4=-0.5,-0.5,0.375,0.375",
  };
  for (kdu_byte prec :
       {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0), KDU_LINE_BUF_VPRECISE}) {
    RunNodeAnalysisCase(20, 16, 12, false, true, 1, atk_sym_3_8, prec);
  }

  // Custom reversible non-symmetric ATK with 5 steps (> 4), one 5-tap step
  // (> 4 max_step_length and total_coeffs > 8), and one 0-tap step
  // (`support_length == 0`) with `Kextension=CON` and `vflip != hflip` so
  // `hor_step_info != vert_step_info` and `required_line_buffers > 6`.
  const std::vector<std::string> atk_many_rev = {
      "Ckernels=ATK",
      "Catk=5",
      "Kreversible:I5=yes",
      "Ksymmetric:I5=no",
      "Kextension:I5=CON",
      "Ksteps:I5={5,-2,4,8},{0,0,0,0},{3,-1,3,4},{2,0,2,2},{1,0,1,1}",
      "Kcoeffs:I5={-0.25},{-0.25},{-0.25},{-0.125},{-0.125},"
      "{-0.125},{0.25},{-0.125},{0.25},{0.25},{-0.5}",
  };
  for (kdu_byte prec :
       {KDU_LINE_BUF_SHORTS, static_cast<kdu_byte>(0), KDU_LINE_BUF_VPRECISE}) {
    RunNodeAnalysisCase(28, 28, 12, false, true, 1, atk_many_rev, prec, 1.0f, 0,
                        true, false, true, false);
  }

  // Custom irreversible non-symmetric ATK with 5 steps, large factor
  // (`1.2 >= 0.499F` downshift loop + `<= 15`), small factor (`< 0.499F`
  // `downshift > 15` -> KDU_INT16_MAX), and 5-tap generic float lifting
  // (`hlift_32_func == NULL` since support_length > 4).
  const std::vector<std::string> atk_many_irrev = {
      "Ckernels=ATK",
      "Catk=6",
      "Kreversible:I6=no",
      "Ksymmetric:I6=no",
      "Kextension:I6=CON",
      // Use 2- and 4-tap steps on 32x32 (coset width 16) for float mode so
      // `sse2_{v,h}lift_32_{2,4}tap_irrev` only read initialized SIMD lanes.
      "Ksteps:I6={5,-2,0,0},{0,0,0,0},{2,0,0,0},{4,-1,0,0},{2,0,0,0}",
      "Kcoeffs:I6={-1.2},{0.2},{0.4},{-0.1},{0.05},"
      "{0.25},{0.25},{-0.0625},{0.25},{-0.125},{-0.0625},{0.05},{0.05}",
  };
  RunNodeAnalysisCase(28, 28, 8, false, false, 1, atk_many_irrev,
                      KDU_LINE_BUF_SHORTS, 1.0f, 0, true, false, true, false);
  RunNodeAnalysisCase(32, 32, 8, false, false, 1, atk_many_irrev, 0, 1.0f, 0,
                      true, false, true, false);
}

TEST_F(AnalysisTest, TransposedHorizontalFirstAndRoiPropagation) {
  // Transposed reversible 2D decomposition triggers `horizontal_first = true`.
  RunNodeAnalysisCase(20, 24, 8, false, true, 2, {}, KDU_LINE_BUF_SHORTS, 1.0f,
                      0, true, /*transpose=*/true, /*vflip=*/true,
                      /*hflip=*/false);

  // ROI propagation: top resolution (dwt_level=1) with Rlevels=0 releases ROI
  // immediately (`!res.propagate_roi()`); with Clevels=1 and Rlevels=1
  // (`Rshift=14`) it propagates ROI via `kdu_roi_level` to the child subbands
  // without triggering multi-level sibling lag in upstream `roi.cpp`.
  for (int rlevels : {0, 1}) {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(
        SetupCodestream(cs, target, 16, 16, 8, false, true, 1,
                        {"Rshift=14", "Rlevels=" + std::to_string(rlevels)}));
    cs.codestream.access_siz()->finalize_all();
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);
    kdu_node top_node = top_res.access_node();

    TestRoiNode roi_node(255);
    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    ScopedLineBuf line;
    line.line.pre_create(&allocator, 16,
                         KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0, 0);

    ScopedAnalysis analysis;
    analysis.engine = kdu_analysis(top_node, &allocator, params,
                                   KDU_LINE_BUF_SHORTS, 1.0f, 0, &roi_node);
    if (rlevels == 0) {
      EXPECT_TRUE(roi_node.released());
    } else {
      EXPECT_FALSE(roi_node.released());
    }
    allocator.finalize(cs.codestream);
    line.line.create();
    analysis.engine.start(nullptr);
    ASSERT_NO_FATAL_FAILURE(
        PushAllLines(analysis.engine, line.line, 16, 16, 0, true, false, true));
    if (rlevels == 1) {
      EXPECT_GT(roi_node.pull_calls(), 0);
      analysis.engine.destroy();
      EXPECT_TRUE(roi_node.released());
    } else {
      analysis.engine.destroy();
    }
    tile.tile.close();
  }
}

TEST_F(AnalysisTest, FragmentedFusionAndPushRetryAndEmptyNode) {
  // Fragmented Clevels=2 codestream with Cblk={16,16}: `top_res` has multiple
  // fragments while `res 1` has 1 fragment (`max_depth=1`), setting
  // `need_fusion = true` and `avoid_multi_push = true` on `top_res`.
  for (bool transpose : {false, true}) {
    const int width = transpose ? 16 : 256;
    const int height = transpose ? 256 : 16;
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(SetupCodestream(cs, target, width, height, 8, false,
                                            true, 2, {"Cblk={16,16}"}));
    cs.codestream.configure_simultaneous_processing_fragments(64, 1, 4);
    cs.codestream.access_siz()->finalize_all();
    if (transpose) {
      cs.codestream.change_appearance(true, false, false);
    }

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);
    const int num_frags = top_res.get_num_simultaneous_processing_fragments();
    ASSERT_GT(num_frags, 1);

    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    params.set_spatial_xform_dbuf_rows(4);

    std::vector<ScopedAnalysis> frags(num_frags);
    std::vector<ScopedLineBuf> lines(num_frags);
    std::vector<kdu_dims> fdims(num_frags);
    for (int f = 0; f < num_frags; ++f) {
      kdu_node node = top_res.access_node(f);
      node.get_dims(fdims[f]);
      lines[f].line.pre_create(&allocator, fdims[f].size.x,
                               KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0,
                               0);
      frags[f].engine =
          kdu_analysis(node, &allocator, params, KDU_LINE_BUF_SHORTS);
    }

    allocator.finalize(cs.codestream);
    for (int f = 0; f < num_frags; ++f) {
      lines[f].line.create();
      frags[f].engine.start(nullptr);
    }

    int max_rows = 0;
    for (int f = 0; f < num_frags; ++f) {
      max_rows = std::max(max_rows, fdims[f].size.y);
    }
    bool saw_false_push = false;
    for (int y = 0; y < max_rows; ++y) {
      for (int f = 0; f < num_frags; ++f) {
        if (y < fdims[f].size.y) {
          FillSampleLine(lines[f].line, fdims[f].size.x, 0, y, true, false,
                         true);
          while (!frags[f].engine.push(lines[f].line, nullptr)) {
            saw_false_push = true;
          }
        }
      }
    }
    EXPECT_TRUE(saw_false_push);
    for (int f = 0; f < num_frags; ++f) {
      EXPECT_TRUE(frags[f].engine.finish(nullptr));
      frags[f].engine.destroy();
    }
    tile.tile.close();
  }

  // Constructing and destroying `kdu_analysis` on an empty (`0 x 6`) node
  // exercises the early `if (empty) return;` guard in `kd_analysis::init()`.
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(
      SetupCodestream(cs, target, 1, 6, 8, false, true, 1, {}, 0, 1, 2));
  cs.codestream.access_siz()->finalize_all();
  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);
  kdu_node top_node = top_res.access_node();
  kdu_dims dims;
  top_node.get_dims(dims);
  ASSERT_EQ(dims.size.x, 0);
  ASSERT_GT(dims.size.y, 0);

  kdu_sample_allocator allocator;
  kdu_push_pull_params params;
  ScopedAnalysis analysis;
  analysis.engine =
      kdu_analysis(top_node, &allocator, params, KDU_LINE_BUF_SHORTS);
  allocator.finalize(cs.codestream);
  analysis.engine.start(nullptr);
  analysis.engine.destroy();
  tile.tile.close();
}

}  // namespace
}  // namespace kdu_core
