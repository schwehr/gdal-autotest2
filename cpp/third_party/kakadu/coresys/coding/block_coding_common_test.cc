// Unit tests for
// third_party/kakadu/vendor/coresys/coding/block_coding_common.{h,cpp}.

#include "third_party/kakadu/vendor/coresys/coding/block_coding_common.h"

#include <algorithm>
#include <cstdint>

#include "testing/base/public/gunit.h"

namespace kd_core_local {
namespace {

static_assert(KAPPA_SIG_BASE == 0);
static_assert(KAPPA_NUM_STATES == 18);
static_assert(NBRHD_MASK == __NBRHD_MASK);
static_assert(SIGMA_CC_BIT == __SIGMA_CC_BIT);
static_assert(CHI_BIT == __CHI_BIT);
static_assert(MU_BIT == __MU_BIT);
static_assert(PI_BIT == __PI_BIT);
static_assert(SIG_PROP_MEMBER_MASK == __SIG_PROP_MEMBER_MASK);
static_assert(CLEANUP_MEMBER_MASK == __CLEANUP_MEMBER_MASK);
static_assert(PREV_CHI_BIT == __PREV_CHI_BIT);
static_assert(static_cast<uint32_t>(NEXT_CHI_BIT) ==
              static_cast<uint32_t>(__NEXT_CHI_BIT));

int Bit(int mask, int pos) { return (mask >> pos) & 1; }

int TransposeHlIndexToLh(int idx) {
  int out = idx & (SIGMA_TL_BIT | SIGMA_TR_BIT | SIGMA_BL_BIT | SIGMA_BR_BIT |
                   SIGMA_CC_BIT);
  out |= Bit(idx, SIGMA_TC_POS) << SIGMA_CL_POS;
  out |= Bit(idx, SIGMA_BC_POS) << SIGMA_CR_POS;
  out |= Bit(idx, SIGMA_CL_POS) << SIGMA_TC_POS;
  out |= Bit(idx, SIGMA_CR_POS) << SIGMA_BC_POS;
  return out;
}

TEST(BlockCodingCommonTest, SignificanceLutsMatchJpeg2000Rules) {
  for (int idx = 0; idx < 512; ++idx) {
    SCOPED_TRACE(idx);
    const int vert = Bit(idx, SIGMA_TC_POS) + Bit(idx, SIGMA_BC_POS);
    const int horiz = Bit(idx, SIGMA_CL_POS) + Bit(idx, SIGMA_CR_POS);
    const int diag = Bit(idx, SIGMA_TL_POS) + Bit(idx, SIGMA_TR_POS) +
                     Bit(idx, SIGMA_BL_POS) + Bit(idx, SIGMA_BR_POS);

    // JPEG 2000 Part 1 Table D.1 (LH / LL) and transposed HL.
    auto expected_hv = [](int primary, int secondary, int diagonals) -> int {
      if (primary == 2) return 8;
      if (primary == 1) {
        if (secondary > 0) return 7;
        return diagonals > 0 ? 6 : 5;
      }
      if (secondary > 0) return 2 + secondary;
      return std::min(2, diagonals);
    };

    // Note: in Kakadu, hl_sig_lut weights vertical neighbors primarily (LH band
    // orientation under JPEG 2000 Part 1 D.1 conventions) and lh_sig_lut
    // weights horizontal neighbors primarily.
    EXPECT_EQ(expected_hv(vert, horiz, diag), hl_sig_lut[idx]);
    EXPECT_EQ(expected_hv(horiz, vert, diag), lh_sig_lut[idx]);
    EXPECT_EQ(hl_sig_lut[idx], lh_sig_lut[TransposeHlIndexToLh(idx)]);

    // Center-sample significance (bit 4) must not affect any significance LUT.
    EXPECT_EQ(hl_sig_lut[idx], hl_sig_lut[idx ^ SIGMA_CC_BIT]);
    EXPECT_EQ(lh_sig_lut[idx], lh_sig_lut[idx ^ SIGMA_CC_BIT]);
    EXPECT_EQ(hh_sig_lut[idx], hh_sig_lut[idx ^ SIGMA_CC_BIT]);

    // JPEG 2000 Part 1 Table D.2 (HH subband: primary = diagonals).
    const int hv = vert + horiz;
    int expected_hh = 0;
    if (diag >= 3) {
      expected_hh = 8;
    } else if (diag == 2) {
      expected_hh = hv >= 1 ? 7 : 6;
    } else if (diag == 1) {
      expected_hh = 3 + std::min(2, hv);
    } else {
      expected_hh = std::min(2, hv);
    }
    EXPECT_EQ(expected_hh, hh_sig_lut[idx]);
  }
}

TEST(BlockCodingCommonTest, SignLutMatchesJpeg2000TableD3) {
  auto axis_contribution = [](int s0, int c0, int s1, int c1) -> int {
    int contrib = 0;
    if (s0) contrib += c0 ? -1 : 1;
    if (s1) contrib += c1 ? -1 : 1;
    return std::clamp(contrib, -1, 1);
  };

  for (int idx = 0; idx < 256; ++idx) {
    SCOPED_TRACE(idx);
    const int v = axis_contribution(
        Bit(idx, UP_NBR_SIGMA_POS), Bit(idx, UP_NBR_CHI_POS),
        Bit(idx, DOWN_NBR_SIGMA_POS), Bit(idx, DOWN_NBR_CHI_POS));
    const int h = axis_contribution(
        Bit(idx, LEFT_NBR_SIGMA_POS), Bit(idx, LEFT_NBR_CHI_POS),
        Bit(idx, RIGHT_NBR_SIGMA_POS), Bit(idx, RIGHT_NBR_CHI_POS));

    int expected_predict = 0;
    int expected_kappa = 0;
    if (h == 0 && v == 0) {
      expected_kappa = 0;
      expected_predict = 0;
    } else if (h == 0) {
      expected_predict = v < 0 ? 1 : 0;
      expected_kappa = 1;
    } else {
      expected_predict = h < 0 ? 1 : 0;
      const int signed_v = expected_predict ? -v : v;
      expected_kappa = 3 + signed_v;
    }
    EXPECT_EQ((expected_kappa << 1) | expected_predict, sign_lut[idx]);
  }
}

}  // namespace
}  // namespace kd_core_local
