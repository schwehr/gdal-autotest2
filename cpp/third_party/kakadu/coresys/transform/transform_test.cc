// Unit tests for third_party/kakadu/vendor/coresys/transform/transform.cpp and
// kd_lifting_step in
// third_party/kakadu/vendor/coresys/transform/transform_base.h.

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"
#include "third_party/kakadu/vendor/coresys/transform/transform_base.h"

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

class TransformTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kTransformEnv =
    ::testing::AddGlobalTestEnvironment(new TransformTestEnvironment());

class NullTarget final : public kdu_compressed_target {
 public:
  bool write(const kdu_byte* /*buf*/, int /*num_bytes*/) override {
    return true;
  }
};

struct ScopedCodestream {
  NullTarget target;
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

void InitCodestream(ScopedCodestream& ctx, int components, int precision,
                    int levels, const std::vector<std::string>& extra_params) {
  ASSERT_TRUE(ctx.siz.parse_string(
      ("Scomponents=" + std::to_string(components)).c_str()));
  ASSERT_TRUE(ctx.siz.parse_string("Ssize={32,32}"));
  for (int c = 0; c < components; ++c) {
    ctx.siz.set(Sdims, c, 0, 32);
    ctx.siz.set(Sdims, c, 1, 32);
    ctx.siz.set(Ssampling, c, 0, 1);
    ctx.siz.set(Ssampling, c, 1, 1);
    ctx.siz.set(Sprecision, c, 0, precision);
    ctx.siz.set(Ssigned, c, 0, false);
  }
  static_cast<kdu_params*>(&ctx.siz)->finalize();

  ctx.codestream.create(&ctx.siz, &ctx.target);
  siz_params* params = ctx.codestream.access_siz();
  ASSERT_TRUE(
      params->parse_string(("Clevels=" + std::to_string(levels)).c_str()));
  for (const std::string& opt : extra_params) {
    ASSERT_TRUE(params->parse_string(opt.c_str())) << opt;
  }
  ASSERT_TRUE(
      params->parse_string(("Rlevels=" + std::to_string(levels)).c_str()));
  params->finalize_all();
}

int QueryRecommendedPrecision(int components, int comp_idx, int precision,
                              int levels,
                              const std::vector<std::string>& extra_params,
                              bool& flexible, bool for_analysis,
                              int reference_precision = 0) {
  ScopedCodestream ctx;
  InitCodestream(ctx, components, precision, levels, extra_params);
  ScopedTile tile;
  tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(comp_idx);
  return kdu_transform_attributes::get_recommended_working_precision(
      tc, flexible, for_analysis, reference_precision);
}

TEST(TransformTest, RecommendedWorkingPrecisionReversibleW5X3AndRct) {
  bool flexible = true;

  // Non-RCT single component: <= 5 levels vs > 5 levels across bit depths.
  const int p8_l3_ana = QueryRecommendedPrecision(
      1, 0, 8, 3, {"Ckernels=W5X3"}, flexible, /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  EXPECT_GE(p8_l3_ana, 8);
  EXPECT_LE(p8_l3_ana, 16);

  const int p8_l3_syn = QueryRecommendedPrecision(
      1, 0, 8, 3, {"Ckernels=W5X3"}, flexible, /*for_analysis=*/false);
  EXPECT_FALSE(flexible);
  EXPECT_GE(p8_l3_syn, 8);

  const int p8_l6 = QueryRecommendedPrecision(1, 0, 8, 6, {"Ckernels=W5X3"},
                                              flexible, /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  EXPECT_GE(p8_l6, p8_l3_ana);

  // Reference precision clamps downward when ref_prec < component bit depth,
  // and leaves precision unchanged when ref_prec > component bit depth.
  const int p16_l3_ref8 = QueryRecommendedPrecision(
      1, 0, 16, 3, {"Ckernels=W5X3"}, flexible, /*for_analysis=*/true, 8);
  EXPECT_FALSE(flexible);
  EXPECT_LT(p16_l3_ref8, 16);

  const int p16_l3_ref24 = QueryRecommendedPrecision(
      1, 0, 16, 3, {"Ckernels=W5X3"}, flexible, /*for_analysis=*/true, 24);
  EXPECT_FALSE(flexible);
  EXPECT_GT(p16_l3_ref24, 16);

  // Higher bit depths push precision > 16 and > 32.
  const int p28_l6 = QueryRecommendedPrecision(1, 0, 28, 6, {"Ckernels=W5X3"},
                                               flexible, /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  EXPECT_GT(p28_l6, 32);

  const int p32_l3 = QueryRecommendedPrecision(
      1, 0, 32, 3, {"Ckernels=W5X3"}, flexible, /*for_analysis=*/false);
  EXPECT_FALSE(flexible);
  EXPECT_GT(p32_l3, 32);

  // 3-component RCT adds bit growth for luminance and chroma components.
  const int rct_c0 = QueryRecommendedPrecision(
      3, 0, 12, 3, {"Ckernels=W5X3", "Cycc=yes"}, flexible,
      /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  const int rct_c1 = QueryRecommendedPrecision(
      3, 1, 12, 3, {"Ckernels=W5X3", "Cycc=yes"}, flexible,
      /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  const int no_rct_c1 = QueryRecommendedPrecision(
      3, 1, 12, 3, {"Ckernels=W5X3", "Cycc=no"}, flexible,
      /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  EXPECT_GE(rct_c0, no_rct_c1);
  EXPECT_GE(rct_c1, rct_c0);

  // Zero DWT levels with and without RCT.
  const int rct_l0 = QueryRecommendedPrecision(
      3, 1, 12, 0, {"Ckernels=W5X3", "Cycc=yes"}, flexible,
      /*for_analysis=*/false);
  EXPECT_FALSE(flexible);
  EXPECT_GE(rct_l0, 12);
}

TEST(TransformTest, RecommendedWorkingPrecisionIrreversibleAndAtkKernels) {
  bool flexible = false;

  // W9X7 irreversible transform: flexible=true, respects reference_precision.
  const int w9x7_8 = QueryRecommendedPrecision(
      1, 0, 8, 5, {"Ckernels=W9X7", "Creversible=no"}, flexible,
      /*for_analysis=*/false);
  EXPECT_TRUE(flexible);
  EXPECT_GE(w9x7_8, 8);

  const int w9x7_16 = QueryRecommendedPrecision(
      1, 0, 16, 5, {"Ckernels=W9X7", "Creversible=no"}, flexible,
      /*for_analysis=*/true);
  EXPECT_TRUE(flexible);
  EXPECT_GT(w9x7_16, 16);

  const int w9x7_16_ref8 = QueryRecommendedPrecision(
      1, 0, 16, 5, {"Ckernels=W9X7", "Creversible=no"}, flexible,
      /*for_analysis=*/true, /*reference_precision=*/8);
  EXPECT_TRUE(flexible);
  EXPECT_LT(w9x7_16_ref8, w9x7_16);

  const int w9x7_rct = QueryRecommendedPrecision(
      3, 1, 12, 3, {"Ckernels=W9X7", "Creversible=no", "Cycc=yes"}, flexible,
      /*for_analysis=*/true);
  EXPECT_TRUE(flexible);
  EXPECT_GE(w9x7_rct, 12);

  // Creversible=no alone selects default irreversible W9X7 kernel.
  const int irrev_default = QueryRecommendedPrecision(
      1, 0, 8, 3, {"Creversible=no"}, flexible, /*for_analysis=*/true);
  EXPECT_TRUE(flexible);
  EXPECT_GE(irrev_default, 8);

  // Part-2 ATK lazy reversible R1X1 kernel.
  const int atk_r1x1 = QueryRecommendedPrecision(
      1, 0, 14, 3, {"Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"}, flexible,
      /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  EXPECT_GE(atk_r1x1, 14);

  const int atk_r1x1_rct = QueryRecommendedPrecision(
      3, 1, 16, 4, {"Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1", "Cycc=yes"},
      flexible, /*for_analysis=*/false);
  EXPECT_FALSE(flexible);
  EXPECT_GT(atk_r1x1_rct, 16);

  // Part-2 ATK irreversible I5X3 and I1X1 kernels.
  const int atk_i5x3 = QueryRecommendedPrecision(
      1, 0, 16, 3, {"Ckernels=ATK", "Catk=3", "Kkernels:I3=I5X3"}, flexible,
      /*for_analysis=*/false, /*reference_precision=*/10);
  EXPECT_TRUE(flexible);
  EXPECT_GE(atk_i5x3, 10);

  const int atk_i1x1 = QueryRecommendedPrecision(
      1, 0, 8, 3, {"Ckernels=ATK", "Catk=4", "Kkernels:I4=I1X1"}, flexible,
      /*for_analysis=*/true);
  EXPECT_TRUE(flexible);
  EXPECT_GE(atk_i1x1, 8);

  // Custom Part-2 ATK arbitrary reversible and irreversible kernels.
  const int custom_atk_rev = QueryRecommendedPrecision(
      1, 0, 12, 3,
      {"Ckernels=ATK", "Catk=5", "Kreversible:I5=yes", "Ksymmetric:I5=no",
       "Kextension:I5=CON", "Ksteps:I5={2,0,1,1},{2,-1,2,2}",
       "Kcoeffs:I5={-0.5},{-0.5},{0.25},{0.25}"},
      flexible, /*for_analysis=*/true);
  EXPECT_FALSE(flexible);
  EXPECT_GE(custom_atk_rev, 12);

  const int custom_atk_irrev = QueryRecommendedPrecision(
      1, 0, 12, 3,
      {"Ckernels=ATK", "Catk=6", "Kreversible:I6=no", "Ksymmetric:I6=no",
       "Kextension:I6=CON", "Ksteps:I6={2,0,0,0},{2,-1,0,0}",
       "Kcoeffs:I6={-0.5},{-0.5},{0.25},{0.25}"},
      flexible, /*for_analysis=*/false);
  EXPECT_TRUE(flexible);
  EXPECT_GE(custom_atk_irrev, 12);
}

struct CallbackTracker {
  int calls = 0;
  int last_samples = 0;
  bool last_synthesis = false;
};

CallbackTracker g_cb_tracker;

void MockVlift16(kdu_int16** /*src*/, kdu_int16* dst_in, kdu_int16* dst_out,
                 int num_samples, kd_core_local::kd_lifting_step* /*step*/,
                 bool synthesis) {
  g_cb_tracker.calls++;
  g_cb_tracker.last_samples = num_samples;
  g_cb_tracker.last_synthesis = synthesis;
  for (int i = 0; i < num_samples; ++i) {
    dst_out[i] = static_cast<kdu_int16>(dst_in[i] + 1);
  }
}

void MockVlift32(kdu_int32** /*src*/, kdu_int32* dst_in, kdu_int32* dst_out,
                 int num_samples, kd_core_local::kd_lifting_step* /*step*/,
                 bool synthesis) {
  g_cb_tracker.calls++;
  g_cb_tracker.last_samples = num_samples;
  g_cb_tracker.last_synthesis = synthesis;
  for (int i = 0; i < num_samples; ++i) {
    dst_out[i] = dst_in[i] + 2;
  }
}

void MockVlift64(kdu_int64** /*src*/, kdu_int64* dst_in, kdu_int64* dst_out,
                 int num_samples, kd_core_local::kd_lifting_step* /*step*/,
                 bool synthesis) {
  g_cb_tracker.calls++;
  g_cb_tracker.last_samples = num_samples;
  g_cb_tracker.last_synthesis = synthesis;
  for (int i = 0; i < num_samples; ++i) {
    dst_out[i] = dst_in[i] + 3;
  }
}

TEST(TransformTest, LiftingStepInt16SupportsAndRoundTrips) {
  kd_core_local::kd_lifting_step step;
  step.reset_func_ptrs();
  EXPECT_EQ(step.vlift_16_func, nullptr);
  EXPECT_EQ(step.vlift_32_func, nullptr);
  EXPECT_EQ(step.vlift_64_func, nullptr);

  constexpr int kBufSize = 64;
  std::array<kdu_int16, kBufSize> s0{}, s1{}, s2{}, dst_in{}, dst_out{};
  for (int i = 0; i < kBufSize; ++i) {
    s0[i] = static_cast<kdu_int16>(10 + i);
    s1[i] = static_cast<kdu_int16>(20 + 2 * i);
    s2[i] = static_cast<kdu_int16>(-5 + i);
    dst_in[i] = static_cast<kdu_int16>(100 + 3 * i);
  }
  std::array<kdu_int16*, 3> src_ptrs = {s0.data(), s1.data(), s2.data()};

  // Custom SIMD callback path.
  g_cb_tracker = {};
  step.vlift_16_func = &MockVlift16;
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 8,
                            0, /*for_synthesis=*/false);
  EXPECT_EQ(g_cb_tracker.calls, 1);
  EXPECT_EQ(g_cb_tracker.last_samples, 8);
  EXPECT_FALSE(g_cb_tracker.last_synthesis);
  EXPECT_EQ(dst_out[0], 101);
  step.reset_func_ptrs();

  // width <= 0 early return.
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 0,
                            /*start_loc=*/0, /*for_synthesis=*/false);

  // support_length == 1 (analysis + synthesis exact round-trip).
  {
    std::array<int, 1> icoeffs = {3};
    std::array<float, 1> coeffs = {0.75F};
    step.support_length = 1;
    step.reversible = true;
    step.downshift = 2;
    step.rounding_offset = 2;
    step.icoeffs = icoeffs.data();
    step.coeffs = coeffs.data();

    std::array<kdu_int16, kBufSize> mid{}, rec{};
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 12,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 12,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    for (int i = 0; i < 12; ++i) {
      EXPECT_EQ(rec[i], dst_in[i]);
    }
  }

  // support_length == 2 symmetric (icoeffs[0] == icoeffs[1]) across
  // i_lambda == 1, -1, and general weight.
  for (int lam : {1, -1, 3}) {
    std::array<int, 2> icoeffs = {lam, lam};
    std::array<float, 2> coeffs = {0.5F * static_cast<float>(lam),
                                   0.5F * static_cast<float>(lam)};
    step.support_length = 2;
    step.reversible = true;
    step.downshift = 1;
    step.rounding_offset = 1;
    step.icoeffs = icoeffs.data();
    step.coeffs = coeffs.data();

    std::array<kdu_int16, kBufSize> buf = dst_in;
    // In-place (dst_in == dst_out) analysis then synthesis, plus start_loc > k.
    step.perform_lifting_step(src_ptrs.data(), buf.data(), buf.data(), 12,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), buf.data(), buf.data(), 12,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    for (int i = 0; i < 12; ++i) {
      EXPECT_EQ(buf[i], dst_in[i]);
    }
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 6,
                              /*start_loc=*/25, /*for_synthesis=*/false);
  }

  // support_length == 2 asymmetric (icoeffs[0] != icoeffs[1]).
  {
    std::array<int, 2> icoeffs = {-1, 2};
    std::array<float, 2> coeffs = {-0.25F, 0.5F};
    step.support_length = 2;
    step.reversible = false;
    step.downshift = 2;
    step.rounding_offset = 2;
    step.icoeffs = icoeffs.data();
    step.coeffs = coeffs.data();

    std::array<kdu_int16, kBufSize> mid{};
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), mid.data(), dst_out.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    EXPECT_NE(mid[0], dst_in[0]);
  }

  // support_length == 3 general loop and support_length == 0.
  {
    std::array<int, 3> icoeffs = {1, -2, 1};
    std::array<float, 3> coeffs = {0.25F, -0.5F, 0.25F};
    step.support_length = 3;
    step.reversible = true;
    step.downshift = 2;
    step.rounding_offset = 2;
    step.icoeffs = icoeffs.data();
    step.coeffs = coeffs.data();

    std::array<kdu_int16, kBufSize> mid{}, rec{};
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    for (int i = 0; i < 10; ++i) {
      EXPECT_EQ(rec[i], dst_in[i]);
    }

    step.support_length = 0;
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 6,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 6,
                              /*start_loc=*/0, /*for_synthesis=*/true);
  }
}

TEST(TransformTest, LiftingStepInt32ReversibleAndIrreversibleBranches) {
  kd_core_local::kd_lifting_step step;
  step.reset_func_ptrs();

  constexpr int kBufSize = 64;
  std::array<kdu_int32, kBufSize> s0{}, s1{}, s2{}, dst_in{}, dst_out{};
  for (int i = 0; i < kBufSize; ++i) {
    s0[i] = 1000 + i * 7;
    s1[i] = -500 + i * 3;
    s2[i] = 200 - i * 5;
    dst_in[i] = 5000 + i * 11;
  }
  std::array<kdu_int32*, 3> src_ptrs = {s0.data(), s1.data(), s2.data()};

  // Custom SIMD callback path.
  g_cb_tracker = {};
  step.vlift_32_func = &MockVlift32;
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 6,
                            0, /*for_synthesis=*/true);
  EXPECT_EQ(g_cb_tracker.calls, 1);
  EXPECT_EQ(g_cb_tracker.last_samples, 6);
  EXPECT_TRUE(g_cb_tracker.last_synthesis);
  EXPECT_EQ(dst_out[0], 5002);
  step.reset_func_ptrs();

  // width <= 0 early return.
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 0,
                            /*start_loc=*/0, /*for_synthesis=*/false);

  // Reversible int32 across support_length = 1, 2 (sym/asym), 3.
  step.reversible = true;
  step.downshift = 2;
  step.rounding_offset = 2;
  {
    std::array<int, 3> icoeffs = {1, -1, 2};
    std::array<float, 3> coeffs = {0.25F, -0.25F, 0.5F};
    step.icoeffs = icoeffs.data();
    step.coeffs = coeffs.data();

    for (kdu_byte len : {1, 3}) {
      step.support_length = len;
      std::array<kdu_int32, kBufSize> mid{}, rec{};
      step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                                /*start_loc=*/0, /*for_synthesis=*/false);
      step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                                /*start_loc=*/0, /*for_synthesis=*/true);
      for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(rec[i], dst_in[i]);
      }
    }

    // Symmetric support_length == 2 requires coeffs[0] == coeffs[1].
    for (int lam : {1, -1, 3}) {
      icoeffs[0] = lam;
      icoeffs[1] = lam;
      coeffs[0] = 0.25F * static_cast<float>(lam);
      coeffs[1] = coeffs[0];
      step.support_length = 2;
      std::array<kdu_int32, kBufSize> mid{}, rec{};
      step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                                /*start_loc=*/0, /*for_synthesis=*/false);
      step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                                /*start_loc=*/0, /*for_synthesis=*/true);
      for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(rec[i], dst_in[i]);
      }
    }

    // Asymmetric support_length == 2 + start_loc > k.
    icoeffs[0] = -1;
    icoeffs[1] = 3;
    coeffs[0] = -0.25F;
    coeffs[1] = 0.75F;
    step.support_length = 2;
    std::array<kdu_int32, kBufSize> mid{}, rec{};
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 6,
                              /*start_loc=*/25, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    for (int i = 0; i < 10; ++i) {
      EXPECT_EQ(rec[i], dst_in[i]);
    }
  }

  // Irreversible float32 via real float backing arrays cast to kdu_int32*.
  std::array<float, kBufSize> fs0{}, fs1{}, fs2{}, fdst_in{}, fmid{}, frec{};
  for (int i = 0; i < kBufSize; ++i) {
    fs0[i] = 1.5F * static_cast<float>(i + 1);
    fs1[i] = -0.75F * static_cast<float>(i + 2);
    fs2[i] = 0.25F * static_cast<float>(i + 3);
    fdst_in[i] = 10.0F + 0.5F * static_cast<float>(i);
  }
  std::array<kdu_int32*, 3> fsrc_ptrs = {
      reinterpret_cast<kdu_int32*>(fs0.data()),
      reinterpret_cast<kdu_int32*>(fs1.data()),
      reinterpret_cast<kdu_int32*>(fs2.data()),
  };
  step.reversible = false;
  std::array<int, 3> dummy_icoeffs = {1, 1, 1};
  step.icoeffs = dummy_icoeffs.data();
  {
    std::array<float, 3> coeffs = {0.25F, 0.25F, -0.125F};
    step.coeffs = coeffs.data();
    for (kdu_byte len : {1, 2, 3}) {
      step.support_length = len;
      step.perform_lifting_step(
          fsrc_ptrs.data(), reinterpret_cast<kdu_int32*>(fdst_in.data()),
          reinterpret_cast<kdu_int32*>(fmid.data()), 10, 0, false);
      step.perform_lifting_step(
          fsrc_ptrs.data(), reinterpret_cast<kdu_int32*>(fmid.data()),
          reinterpret_cast<kdu_int32*>(frec.data()), 10, 0, true);
      for (int i = 0; i < 10; ++i) {
        EXPECT_NEAR(frec[i], fdst_in[i], 1e-4F);
      }
    }
    // Asymmetric float 2-tap.
    coeffs[1] = -0.375F;
    step.support_length = 2;
    step.perform_lifting_step(
        fsrc_ptrs.data(), reinterpret_cast<kdu_int32*>(fdst_in.data()),
        reinterpret_cast<kdu_int32*>(fmid.data()), 10, 0, false);
    step.perform_lifting_step(
        fsrc_ptrs.data(), reinterpret_cast<kdu_int32*>(fmid.data()),
        reinterpret_cast<kdu_int32*>(frec.data()), 10, 0, true);
    for (int i = 0; i < 10; ++i) {
      EXPECT_NEAR(frec[i], fdst_in[i], 1e-4F);
    }
  }
}

TEST(TransformTest, LiftingStepInt64ReversibleBranches) {
  kd_core_local::kd_lifting_step step;
  step.reset_func_ptrs();
  step.reversible = true;
  step.downshift = 2;
  step.rounding_offset = 2;

  constexpr int kBufSize = 64;
  std::array<kdu_int64, kBufSize> s0{}, s1{}, s2{}, dst_in{}, dst_out{};
  for (int i = 0; i < kBufSize; ++i) {
    s0[i] = (1LL << 40) + i * 13;
    s1[i] = -(1LL << 39) + i * 7;
    s2[i] = (1LL << 38) - i * 5;
    dst_in[i] = (1LL << 41) + i * 17;
  }
  std::array<kdu_int64*, 3> src_ptrs = {s0.data(), s1.data(), s2.data()};

  // Custom SIMD callback path.
  g_cb_tracker = {};
  step.vlift_64_func = &MockVlift64;
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 4,
                            0, /*for_synthesis=*/false);
  EXPECT_EQ(g_cb_tracker.calls, 1);
  EXPECT_EQ(g_cb_tracker.last_samples, 4);
  EXPECT_FALSE(g_cb_tracker.last_synthesis);
  EXPECT_EQ(dst_out[0], (1LL << 41) + 3);
  step.reset_func_ptrs();

  // width <= 0 early return.
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), dst_out.data(), 0,
                            /*start_loc=*/0, /*for_synthesis=*/false);

  std::array<int, 3> icoeffs = {-1, -1, 2};
  std::array<float, 3> coeffs = {-0.25F, -0.25F, 0.5F};
  step.icoeffs = icoeffs.data();
  step.coeffs = coeffs.data();

  for (kdu_byte len : {1, 3}) {
    step.support_length = len;
    std::array<kdu_int64, kBufSize> mid{}, rec{};
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    for (int i = 0; i < 10; ++i) {
      EXPECT_EQ(rec[i], dst_in[i]);
    }
  }

  for (int lam : {1, -1, 3}) {
    icoeffs[0] = lam;
    icoeffs[1] = lam;
    coeffs[0] = 0.25F * static_cast<float>(lam);
    coeffs[1] = coeffs[0];
    step.support_length = 2;
    std::array<kdu_int64, kBufSize> mid{}, rec{};
    step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/false);
    step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                              /*start_loc=*/0, /*for_synthesis=*/true);
    for (int i = 0; i < 10; ++i) {
      EXPECT_EQ(rec[i], dst_in[i]);
    }
  }

  // Asymmetric 2-tap and start_loc > k.
  icoeffs[0] = -1;
  icoeffs[1] = 3;
  coeffs[0] = -0.25F;
  coeffs[1] = 0.75F;
  step.support_length = 2;
  std::array<kdu_int64, kBufSize> mid{}, rec{};
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 6,
                            /*start_loc=*/25, /*for_synthesis=*/false);
  step.perform_lifting_step(src_ptrs.data(), dst_in.data(), mid.data(), 10,
                            /*start_loc=*/0, /*for_synthesis=*/false);
  step.perform_lifting_step(src_ptrs.data(), mid.data(), rec.data(), 10,
                            /*start_loc=*/0, /*for_synthesis=*/true);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(rec[i], dst_in[i]);
  }
}

}  // namespace
}  // namespace kdu_core
