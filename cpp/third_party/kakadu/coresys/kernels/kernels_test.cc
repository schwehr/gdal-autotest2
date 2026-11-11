// Unit tests for third_party/kakadu/vendor/coresys/kernels/kernels.cpp
// and third_party/kakadu/vendor/coresys/common/kdu_kernels.h.

#include <array>
#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_kernels.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      kdu_thread_safe_message::flush(true);
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_thread_safe_message::flush(false);
  }
};

class SilentWarningHandler final : public kdu_thread_safe_message {};

kdu_message* GlobalErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

kdu_message* GlobalWarningHandler() {
  static auto* const handler = new SilentWarningHandler();
  return handler;
}

class KernelsTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_customize_errors(GlobalErrorHandler());
    kdu_customize_warnings(GlobalWarningHandler());
  }
};

const ::testing::Environment* const kKernelsEnv =
    ::testing::AddGlobalTestEnvironment(new KernelsTestEnvironment());

TEST(KernelsTest, StepInfoDefaultsAndEnumeratedKernels) {
  kdu_kernel_step_info default_step;
  EXPECT_EQ(0, default_step.support_length);
  EXPECT_EQ(0, default_step.support_min);
  EXPECT_EQ(0, default_step.downshift);
  EXPECT_EQ(0, default_step.rounding_offset);

  // Reversible and irreversible 5/3 via both two-arg ctor and init().
  kdu_kernels rev_w5x3(Ckernels_W5X3, /*reversible=*/true);
  EXPECT_EQ(Ckernels_W5X3, rev_w5x3.get_id());
  EXPECT_TRUE(rev_w5x3.is_reversible());
  EXPECT_TRUE(rev_w5x3.is_symmetric());
  EXPECT_TRUE(rev_w5x3.get_symmetric_extension());

  int num_steps = 0;
  int max_len = 0;
  const kdu_kernel_step_info* steps =
      rev_w5x3.get_step_info(num_steps, max_len);
  ASSERT_NE(nullptr, steps);
  EXPECT_EQ(2, num_steps);
  EXPECT_EQ(2, max_len);
  EXPECT_EQ(1, steps[0].downshift);
  EXPECT_EQ(1, steps[0].rounding_offset);
  EXPECT_EQ(2, steps[1].downshift);
  EXPECT_EQ(2, steps[1].rounding_offset);

  int downshift = -1;
  EXPECT_TRUE(rev_w5x3.get_lifting_downshift(0, downshift));
  EXPECT_EQ(1, downshift);
  EXPECT_TRUE(rev_w5x3.get_lifting_downshift(1, downshift));
  EXPECT_EQ(2, downshift);

  float dc_scale = 0.0f;
  float nyq_scale = 0.0f;
  const float* rev_factors =
      rev_w5x3.get_lifting_factors(num_steps, dc_scale, nyq_scale);
  ASSERT_NE(nullptr, rev_factors);
  EXPECT_FLOAT_EQ(1.0f, dc_scale);
  EXPECT_FLOAT_EQ(1.0f, nyq_scale);

  kdu_kernels irr_w5x3;
  irr_w5x3.init(Ckernels_W5X3, /*reversible=*/false);
  EXPECT_FALSE(irr_w5x3.is_reversible());
  EXPECT_FALSE(irr_w5x3.get_lifting_downshift(0, downshift));

  kdu_kernels i5x3(128 + Kkernels_I5X3, /*reversible=*/false);
  EXPECT_EQ(128 + Kkernels_I5X3, i5x3.get_id());
  EXPECT_TRUE(i5x3.is_symmetric());
  EXPECT_FALSE(i5x3.is_reversible());

  // Lazy 1x1 kernels (I1X1 and R1X1). Note: Kkernels_{I,R}2X2 + 128 are
  // omitted because vendor kernels.cpp leaves lifting_factors[1] uninitialized.
  kdu_kernels lazy_i1x1(128 + Kkernels_I1X1, /*reversible=*/false);
  EXPECT_EQ(128 + Kkernels_I1X1, lazy_i1x1.get_id());
  EXPECT_FALSE(lazy_i1x1.is_symmetric());
  EXPECT_FALSE(lazy_i1x1.get_symmetric_extension());

  kdu_kernels lazy_r1x1(128 + Kkernels_R1X1, /*reversible=*/true);
  EXPECT_EQ(128 + Kkernels_R1X1, lazy_r1x1.get_id());
  EXPECT_TRUE(lazy_r1x1.is_reversible());

  // Error paths use an already-constructed kdu_kernels so cleanup runs via
  // RAII.
  kdu_kernels err_k;
  EXPECT_THROW(err_k.init(Ckernels_W9X7, /*reversible=*/true), kdu_exception);
  EXPECT_THROW(err_k.init(/*kernel_id=*/9999, /*reversible=*/false),
               kdu_exception);
}

TEST(KernelsTest, ArbitraryKernelZeroPaddingNormalizationAndBoundsErrors) {
  kdu_kernel_step_info steps[2];
  steps[0].support_length = 1;
  steps[0].support_min = 0;
  steps[1].support_length = 3;
  steps[1].support_min = -1;
  constexpr std::array<float, 4> kCoeffs = {-0.25f, 0.1f, 0.2f, 0.1f};

  kdu_kernels atk;
  atk.init(/*num_steps=*/2, steps, kCoeffs.data(), /*symmetric=*/false,
           /*symmetric_extension=*/false, /*reversible=*/false);
  EXPECT_EQ(-1, atk.get_id());
  EXPECT_FALSE(atk.is_symmetric());
  EXPECT_FALSE(atk.get_symmetric_extension());
  EXPECT_FALSE(atk.is_reversible());

  int num_steps = 0;
  int max_len = 0;
  ASSERT_NE(nullptr, atk.get_step_info(num_steps, max_len));
  EXPECT_EQ(2, num_steps);
  EXPECT_EQ(3, max_len);

  float dc_scale = 0.0f;
  float nyq_scale = 0.0f;
  const float* factors =
      atk.get_lifting_factors(num_steps, dc_scale, nyq_scale);
  ASSERT_NE(nullptr, factors);
  // Step 0 has support_length 1 vs max_step_length 3, so slots 1 and 2 are
  // zero-padded.
  EXPECT_FLOAT_EQ(-0.25f, factors[0]);
  EXPECT_FLOAT_EQ(0.0f, factors[2]);
  EXPECT_FLOAT_EQ(0.0f, factors[4]);
  EXPECT_GT(dc_scale, 0.0f);
  EXPECT_GT(nyq_scale, 0.0f);

  // Re-initialize the same ATK as reversible.
  atk.init(/*num_steps=*/2, steps, kCoeffs.data(), /*symmetric=*/false,
           /*symmetric_extension=*/false, /*reversible=*/true);
  EXPECT_TRUE(atk.is_reversible());
  EXPECT_NE(nullptr, atk.get_lifting_factors(num_steps, dc_scale, nyq_scale));
  EXPECT_FLOAT_EQ(1.0f, dc_scale);
  EXPECT_FLOAT_EQ(1.0f, nyq_scale);

  // Bogus ATK with safe_L > 2048 throws 0x18081701 in init().
  kdu_kernel_step_info huge_step;
  huge_step.support_length = 1;
  huge_step.support_min = -3000;
  const float huge_coeff = 0.5f;
  EXPECT_THROW(atk.init(/*num_steps=*/1, &huge_step, &huge_coeff,
                        /*symmetric=*/false, /*symmetric_extension=*/false,
                        /*reversible=*/true),
               kdu_exception);

  // ATK with safe_L <= 2048 at 1 level, but > 65536 across deep BIBO gains,
  // throws 0x18081702 inside get_bibo_gains().
  kdu_kernel_step_info mid_step;
  mid_step.support_length = 1;
  mid_step.support_min = -1500;
  atk.init(/*num_steps=*/1, &mid_step, &huge_coeff, /*symmetric=*/false,
           /*symmetric_extension=*/false, /*reversible=*/true);
  bool extra_false[3] = {false, false, false};
  double low_gain = 0.0;
  double high_gain = 0.0;
  EXPECT_THROW(atk.get_bibo_gains(4, 3, extra_false, low_gain, high_gain),
               kdu_exception);
}

TEST(KernelsTest, ImpulseResponsesGainsCachesLegacyOverloadsAndReInit) {
  kdu_kernels k(Ckernels_W9X7, /*reversible=*/false);
  EXPECT_EQ(Ckernels_W9X7, k.get_id());
  EXPECT_FALSE(k.is_reversible());
  EXPECT_TRUE(k.is_symmetric());
  EXPECT_TRUE(k.get_symmetric_extension());

  int base_half = 0;
  int detail_half = 0;
  k.get_analysis_support(base_half, detail_half);
  EXPECT_EQ(4, base_half);
  EXPECT_EQ(3, detail_half);

  int bmin = 0;
  int bmax = 0;
  int dmin = 0;
  int dmax = 0;
  k.get_synthesis_support(bmin, bmax, dmin, dmax);
  EXPECT_EQ(-3, bmin);
  EXPECT_EQ(3, bmax);
  EXPECT_EQ(-4, dmin);
  EXPECT_EQ(4, dmax);

  // Exercise all four kdu_kernel_type values with both null and non-null
  // support outputs.
  constexpr std::array<kdu_kernel_type, 4> kTypes = {
      KDU_ANALYSIS_LOW,
      KDU_ANALYSIS_HIGH,
      KDU_SYNTHESIS_LOW,
      KDU_SYNTHESIS_HIGH,
  };
  for (kdu_kernel_type which : kTypes) {
    int h_null = -1;
    const float* taps_null = k.get_impulse_response(which, h_null);
    ASSERT_NE(nullptr, taps_null);

    int h = -1;
    int smin = 0;
    int smax = 0;
    const float* taps = k.get_impulse_response(which, h, &smin, &smax);
    ASSERT_NE(nullptr, taps);
    EXPECT_EQ(h_null, h);
    EXPECT_EQ(-h, smin);
    EXPECT_EQ(h, smax);
    EXPECT_GT(std::fabs(taps[0]), 0.0f);
  }

  // get_energy_gain beyond max_initial_lowpass_stages (4), including buffer
  // growth on first call and cache hit on repeat call.
  bool extra_tf[2] = {true, false};
  const double energy1 = k.get_energy_gain(12, 2, extra_tf);
  const double energy2 = k.get_energy_gain(12, 2, extra_tf);
  EXPECT_GT(energy1, 0.0);
  EXPECT_DOUBLE_EQ(energy1, energy2);

  // get_bibo_gain across 0 stages, cached initial lowpass stages, clamped
  // depth, and high-pass trailing extra stage.
  EXPECT_DOUBLE_EQ(1.0, k.get_bibo_gain(0, 0, nullptr));
  const double b2_a = k.get_bibo_gain(2, 0, nullptr);
  const double b2_b = k.get_bibo_gain(2, 0, nullptr);
  EXPECT_GT(b2_a, 1.0);
  EXPECT_DOUBLE_EQ(b2_a, b2_b);

  const double b12_a = k.get_bibo_gain(12, 2, extra_tf);
  const double b12_b = k.get_bibo_gain(4, 2, extra_tf);
  EXPECT_GT(b12_a, 1.0);
  EXPECT_DOUBLE_EQ(b12_a, b12_b);

  bool extra_ftt[3] = {false, true, true};
  const double b_ftt = k.get_bibo_gain(2, 3, extra_ftt);
  EXPECT_GT(b_ftt, 0.0);

  // get_bibo_gains with two_tap_step_input_accumulator_gains across packet
  // branches.
  double low_gain = 0.0;
  double high_gain = 0.0;
  std::array<float, 4> accum_gains{};
  const double* step_gains =
      k.get_bibo_gains(2, 2, extra_tf, low_gain, high_gain, accum_gains.data());
  ASSERT_NE(nullptr, step_gains);
  EXPECT_GT(low_gain, 0.0);
  EXPECT_GT(high_gain, 0.0);
  for (int i = 0; i < 4; ++i) {
    EXPECT_GT(step_gains[i], 0.0);
    EXPECT_GT(accum_gains[i], 0.0f);
  }

  // Legacy Part-1 gain overloads in kdu_kernels.h.
  EXPECT_DOUBLE_EQ(1.0, k.get_energy_gain(KDU_SYNTHESIS_LOW, 0));
  EXPECT_DOUBLE_EQ(0.0, k.get_energy_gain(KDU_SYNTHESIS_HIGH, 0));
  EXPECT_GT(k.get_energy_gain(KDU_SYNTHESIS_HIGH, 2), 0.0);

  double legacy_low = 0.0;
  double legacy_high = 0.0;
  EXPECT_EQ(nullptr, k.get_bibo_gains(0, legacy_low, legacy_high));
  EXPECT_DOUBLE_EQ(1.0, legacy_low);
  EXPECT_DOUBLE_EQ(0.0, legacy_high);
  EXPECT_NE(nullptr, k.get_bibo_gains(2, legacy_low, legacy_high));
  EXPECT_GT(legacy_low, 0.0);
  EXPECT_GT(legacy_high, 0.0);

  // Re-initialize after get_energy_gain / get_bibo_gains so reset() frees
  // work1, work2, bibo_step_gains, and all tap/step arrays cleanly.
  k.init(Ckernels_W5X3, /*reversible=*/true);
  EXPECT_EQ(Ckernels_W5X3, k.get_id());
  EXPECT_TRUE(k.is_reversible());
}

}  // namespace
}  // namespace kdu_core
