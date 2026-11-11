// Unit tests for third_party/kakadu/vendor/coresys/common/kdu_arch.{h,cpp}.

#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"

#include "testing/base/public/gunit.h"

namespace kdu_core {
namespace {

static_assert((KDU_ALIGN_SAMPLES16 & (KDU_ALIGN_SAMPLES16 - 1)) == 0);
static_assert((KDU_ALIGN_SAMPLES32 & (KDU_ALIGN_SAMPLES32 - 1)) == 0);
static_assert((KDU_ALIGN_SAMPLES64 & (KDU_ALIGN_SAMPLES64 - 1)) == 0);
static_assert(KDU_PREALIGN_BYTES >= 2 * KDU_ALIGN_SAMPLES16);
static_assert(KDU_PREALIGN_BYTES >= 4 * KDU_ALIGN_SAMPLES32);
static_assert(KDU_PREALIGN_BYTES >= 8 * KDU_ALIGN_SAMPLES64);
static_assert(KDU_MAX_L2_CACHE_LINE == 64);
static_assert(KDU_CODE_BUFFER_ALIGN == 64);

TEST(KduArchTest, MmxLevelMatchesGlobalAndRange) {
  const int level = kdu_get_mmx_level();
  EXPECT_EQ(kdu_mmx_level, level);
  EXPECT_GE(level, 0);
  EXPECT_LE(level, 7);
#ifdef KDU_NO_AVX2
  EXPECT_LE(level, 6);
#endif
#if defined(__aarch64__)
  EXPECT_EQ(0, level);
#endif
}

TEST(KduArchTest, PentiumCmovMatchesGlobal) {
  const bool exists = kdu_get_pentium_cmov_exists();
  EXPECT_EQ(kdu_pentium_cmov_exists, exists);
#if defined(__x86_64__)
  EXPECT_TRUE(exists);
#elif defined(__aarch64__)
  EXPECT_FALSE(exists);
#endif
}

TEST(KduArchTest, X86Bmi2LevelMatchesGlobalAndRange) {
  const int level = kdu_get_x86_bmi2_level();
  EXPECT_EQ(kdu_x86_bmi2_level, level);
  EXPECT_GE(level, 0);
  EXPECT_LE(level, 2);
#if defined(__aarch64__)
  EXPECT_EQ(0, level);
#endif
}

TEST(KduArchTest, NeonLevelMatchesGlobal) {
  const int level = kdu_get_neon_level();
  EXPECT_EQ(kdu_neon_level, level);
#ifdef KDU_NEON_INTRINSICS
  EXPECT_GE(level, 1);
  EXPECT_LE(level, 2);
#else
  // Upstream kdu_arch.h checks __ARM_NEON__ rather than __ARM_NEON, so
  // --cpu=arm falls back to Unknown Architecture unless __ARM_NEON__ is set.
  EXPECT_EQ(0, level);
#endif
}

TEST(KduArchTest, SparcVisAndAltivecAbsentOnSupportedPlatforms) {
  EXPECT_FALSE(kdu_sparcvis_exists);
  EXPECT_FALSE(kdu_altivec_exists);
#if defined(KDU_X86_INTRINSICS) || defined(KDU_NEON_INTRINSICS) || \
    defined(KDU_SPARCVIS_GCC) || defined(KDU_ALTIVEC_GCC) ||       \
    defined(KDU_NO_CPUID_TEST)
  // Vendor kdu_arch.cpp omits kdu_get_{sparcvis,altivec}_exists() definitions
  // in its #else (Unknown Architecture) branch.
  EXPECT_EQ(kdu_sparcvis_exists, kdu_get_sparcvis_exists());
  EXPECT_EQ(kdu_altivec_exists, kdu_get_altivec_exists());
#endif
}

TEST(KduArchTest, NumProcessorsValid) {
  const int num_procs = kdu_get_num_processors();
  EXPECT_GE(num_procs, 1);
  EXPECT_LT(num_procs, 100000);
}

TEST(KduArchTest, SampleAlignmentMacrosConsistentWithLibrary) {
  EXPECT_TRUE(kdu_core_sample_alignment_checker(
      KDU_OVERREAD_BYTES, KDU_PREALIGN_BYTES, KDU_ALIGN_SAMPLES16,
      KDU_ALIGN_SAMPLES32, KDU_ALIGN_SAMPLES64, /*return_on_fail=*/true,
      /*strict=*/true));
  kdu_check_sample_alignment();
}

}  // namespace
}  // namespace kdu_core
