#include "third_party/kakadu/vendor/coresys/fast_coding/fbc_common.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"

namespace kd_core_local {
namespace {

TEST(FbcCommonTest, StaticInitAndConstants) {
  EXPECT_TRUE(fbc_common_do_static_init());
  EXPECT_EQ(FBC_AZC_INITIAL_MEL_STATE, 0);
  EXPECT_EQ(FBC_MEL_STATES, 13);
  EXPECT_EQ(FBC_MEL_LAST, 12);
  EXPECT_EQ(FBC_MEL_ENC_HT_LIM, 51);
  EXPECT_EQ(FBC_U0_VAL, 2);

  EXPECT_TRUE(FBC_LIKELY(true));
  EXPECT_FALSE(FBC_LIKELY(false));
  EXPECT_FALSE(FBC_UNLIKELY(false));
  EXPECT_TRUE(FBC_UNLIKELY(true));
}

TEST(FbcCommonTest, MelTablesMatchDecoderCopies) {
  std::vector<kdu_uint16> m_decode(FBC_MEL_STATES * 64, 0xFFFFU);
  std::vector<kdu_uint16> m_interp(360, 0xFFFFU);
  fbc_get_mel_m_tables(m_decode.data(), m_interp.data());

  for (int i = 0; i < FBC_MEL_STATES * 64; ++i) {
    EXPECT_EQ(m_decode[i], fbc_dec_tables.lut_mel_m_decode[i])
        << "Mismatch in lut_mel_m_decode at " << i;
  }
  for (int i = 0; i < 360; ++i) {
    EXPECT_EQ(m_interp[i], fbc_dec_tables.lut_mel_m_interp[i])
        << "Mismatch in lut_mel_m_interp at " << i;
  }
}

TEST(FbcCommonTest, EncoderAndDecoderVlcConsistency) {
  // Invalid (EMB & ~rho) entries and (c==0, rho==0) entries must be 0 in
  // encoder LUTs.
  for (int c = 0; c < 8; ++c) {
    for (int rho = 0; rho < 16; ++rho) {
      for (int emb = 0; emb < 16; ++emb) {
        const int idx = (c << 8) | (rho << 4) | emb;
        const kdu_uint16 enc0 = fbc_enc_tables.sig0_vlc[idx];
        const kdu_uint16 enc1 = fbc_enc_tables.sig1_vlc[idx];
        if ((emb & ~rho) != 0 || (c == 0 && rho == 0)) {
          EXPECT_EQ(enc0, 0U) << "idx=" << idx;
          EXPECT_EQ(enc1, 0U) << "idx=" << idx;
        } else {
          const int len0 = enc0 & 7;
          const int len1 = enc1 & 7;
          EXPECT_GE(len0, 1);
          EXPECT_LE(len0, 7);
          EXPECT_GE(len1, 1);
          EXPECT_LE(len1, 7);
          EXPECT_EQ(enc0 & 0x108U, 0U);
          EXPECT_EQ(enc1 & 0x108U, 0U);

          // Verify encoded codeword round-trips against decoder VLC table.
          const int cwd0 = (enc0 >> 9) & ((1 << len0) - 1);
          const int cwd1 = (enc1 >> 9) & ((1 << len1) - 1);
          for (int fill = 0; fill < (1 << (7 - len0)); ++fill) {
            const int prefix7 = cwd0 | (fill << len0);
            const kdu_uint16 dec = fbc_dec_vlc.sig0_vlc[(c << 7) | prefix7];
            EXPECT_EQ(dec & 7, len0);
            EXPECT_EQ((dec >> 3) & 0x0F, rho);
          }
          for (int fill = 0; fill < (1 << (7 - len1)); ++fill) {
            const int prefix7 = cwd1 | (fill << len1);
            const kdu_uint16 dec = fbc_dec_vlc.sig1_vlc[(c << 7) | prefix7];
            EXPECT_EQ(dec & 7, len1);
            EXPECT_EQ((dec >> 3) & 0x0F, rho);
          }
        }
      }
    }
  }

  // mod_sig*_vlc zeroes context 0 and preserves contexts 1..7.
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(fbc_dec_vlc.mod_sig0_vlc[i], 0U);
    EXPECT_EQ(fbc_dec_vlc.mod_sig1_vlc[i], 0U);
  }
  for (int i = 128; i < 8 * 128; ++i) {
    EXPECT_EQ(fbc_dec_vlc.mod_sig0_vlc[i], fbc_dec_vlc.sig0_vlc[i]);
    EXPECT_EQ(fbc_dec_vlc.mod_sig1_vlc[i], fbc_dec_vlc.sig1_vlc[i]);
  }
  for (int i = 0; i < 8 * 128; ++i) {
    EXPECT_EQ(fbc_dec_tables.lut_sig0_vlc[i], fbc_dec_vlc.sig0_vlc[i]);
    EXPECT_EQ(fbc_dec_tables.lut_sig1_vlc[i], fbc_dec_vlc.sig1_vlc[i]);
  }
}

TEST(FbcCommonTest, EncoderAndDecoderUTablesInvariants) {
  for (int u2 = 0; u2 < 16; ++u2) {
    for (int u1 = 0; u1 < 16; ++u1) {
      EXPECT_EQ(fbc_enc_tables.u0_pair16[u1 + u2 * 16],
                fbc_enc_tables.u0_pair32[u1 + u2 * 32]);
      EXPECT_EQ(fbc_enc_tables.u1_pair16[u1 + u2 * 16],
                fbc_enc_tables.u1_pair32[u1 + u2 * 32]);
    }
  }
  for (int i = 0; i < 32 * 32; ++i) {
    EXPECT_LE(fbc_enc_tables.u0_pair32[i] & 0xFFU, 16U);
    EXPECT_LE(fbc_enc_tables.u1_pair32[i] & 0xFFU, 16U);
  }

  EXPECT_EQ(fbc_enc_tables.u_parts[0], 0U);
  for (int u = 1; u < 64; ++u) {
    const kdu_uint32 entry = fbc_enc_tables.u_parts[u];
    const int len_a = static_cast<int>(entry & 0x3U);
    const int len_b = static_cast<int>((entry >> 2) & 0x7U);
    const int len_c = static_cast<int>((entry >> 5) & 0x7U);
    EXPECT_GE(len_a, 1);
    EXPECT_LE(len_a, 3);
    EXPECT_TRUE(len_b == 0 || len_b == 1 || len_b == 5);
    EXPECT_TRUE(len_c == 0 || len_c == 4);
  }

  for (int i = 0; i < 256; ++i) {
    const kdu_uint16 v = fbc_dec_tables.lut_u1_pair[i];
    EXPECT_LE(v & 0x7U, 6U);
    EXPECT_LE((v >> 3) & 0xFU, 10U);
    EXPECT_LE((v >> 7) & 0x7U, 5U);
  }
  for (int i = 0; i < 256 + 64; ++i) {
    const kdu_uint16 v = fbc_dec_tables.lut_u0_pair[i];
    EXPECT_LE(v & 0x7U, 6U);
    EXPECT_LE((v >> 3) & 0xFU, 10U);
    EXPECT_LE((v >> 7) & 0x7U, 5U);
  }
}

TEST(FbcCommonTest, AcceleratedSigPropDecodeTables) {
  ASSERT_NE(fbc_acc_spdec_4_lut_align32, nullptr);
  ASSERT_NE(fbc_acc_spdec_4_len_align16, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(fbc_acc_spdec_4_lut_align32) % 32U,
            0U);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(fbc_acc_spdec_4_len_align16) % 16U,
            0U);

  for (int i = 0; i < 2048; ++i) {
    EXPECT_EQ(fbc_acc_spdec_4_lut_align32[i], fbc_acc_spdec_4_lut[i]);
    EXPECT_EQ(fbc_acc_spdec_4_len_align16[i], fbc_acc_spdec_4_lut[i] & 0x0FU);
    EXPECT_LE(fbc_acc_spdec_4_lut[i] & 0x0FU, 8U);
    EXPECT_LE(((fbc_acc_spdec_2_lut[i] & 0x0FU) +
               ((fbc_acc_spdec_2_lut[i] >> 8) & 0x0FU)),
              8U);
    EXPECT_LE(fbc_acc_spdec_1_lut[i] & 0x0FU, 8U);
  }
}

}  // namespace
}  // namespace kd_core_local
