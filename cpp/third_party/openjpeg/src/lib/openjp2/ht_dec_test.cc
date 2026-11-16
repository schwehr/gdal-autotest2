// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests internal OpenJPEG High-Throughput (HTJ2K / ISO 15444-15) block decoder
// APIs (src/lib/openjp2/ht_dec.c and src/lib/openjp2/t1_ht_luts.h).
//
// Caveats for google3:
// - Little-endian host; OPJ_BIG_ENDIAN, _MSC_VER, and non-GNUC fallback paths
//   in population_count / count_leading_zeros / read_le_uint32 are not
//   compiled.
// - ht_dec.c has a process-global one-way flag `only_cleanup_pass_is_decoded`
//   (triggered when `zero_bplanes == cblk->Mb && num_passes > 1`); only one
//   test exercises that condition and does not assert the one-shot warning text
//   so randomized execution orders under Testify remain order-independent.
// - White-box tests inspect opj_tcd_cblk_dec_t / opj_tcd_seg_t / opj_t1_t
//   internals (where t1->datasize is in samples, not bytes), pin upstream
//   error/warning substrings, and assume vlc_tbl0/1 pack {len:3, u_off:1,
//   rho:4, e1:4, ek:4}.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/ht_dec.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/t1_ht_luts.h

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"
#include "third_party/openjpeg2/src/lib/openjp2/t1_ht_luts.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_tcd_cblk_dec_t) > 0);
static_assert(J2K_CCP_CBLKSTY_VSC == 0x08);
static_assert(std::size(vlc_tbl0) == 1024);
static_assert(std::size(vlc_tbl1) == 1024);

// Forward bit-writer with 0xFF byte-stuffing (MSB of byte after 0xFF is 0).
// Matches frwd_init / frwd_read for MagSgn and SPP bitstreams.
class ForwardStuffedWriter {
 public:
  explicit ForwardStuffedWriter(bool pad_trailing_ff = true)
      : pad_trailing_ff_(pad_trailing_ff) {}

  void WriteBit(uint32_t bit) {
    const int limit = unstuff_ ? 7 : 8;
    cur_byte_ |= static_cast<uint8_t>((bit & 1U) << bit_pos_);
    ++bit_pos_;
    if (bit_pos_ == limit) {
      PushByte(cur_byte_);
      cur_byte_ = 0;
      bit_pos_ = 0;
    }
  }

  void WriteBits(uint32_t val, int num_bits) {
    for (int i = 0; i < num_bits; ++i) {
      WriteBit((val >> i) & 1U);
    }
  }

  bool saw_stuffing() const { return saw_stuffing_; }

  std::vector<OPJ_BYTE> Finish() {
    if (bit_pos_ > 0) {
      PushByte(cur_byte_);
      cur_byte_ = 0;
      bit_pos_ = 0;
    }
    if (pad_trailing_ff_ && unstuff_) {
      PushByte(0x00);
    }
    return bytes_;
  }

 private:
  void PushByte(uint8_t byte) {
    if (unstuff_) {
      saw_stuffing_ = true;
    }
    bytes_.push_back(byte);
    unstuff_ = (byte == 0xFF);
  }

  bool pad_trailing_ff_;
  bool unstuff_ = false;
  bool saw_stuffing_ = false;
  uint8_t cur_byte_ = 0;
  int bit_pos_ = 0;
  std::vector<OPJ_BYTE> bytes_;
};

// MSB-first MEL encoder with 0xFF byte-stuffing (MSB after 0xFF is 0).
class MelWriter {
 public:
  void EncodeEvent(int event) {
    constexpr auto kMelExp =
        std::to_array<int>({0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5});
    const int eval = kMelExp[k_];
    if (event == 0) {
      ++pending_zeros_;
      if (pending_zeros_ == (1 << eval)) {
        WriteBit(1);
        k_ = std::min(k_ + 1, 12);
        pending_zeros_ = 0;
      }
    } else {
      WriteBit(0);
      for (int b = eval - 1; b >= 0; --b) {
        WriteBit((pending_zeros_ >> b) & 1);
      }
      k_ = std::max(k_ - 1, 0);
      pending_zeros_ = 0;
    }
  }

  bool saw_stuffing() const { return saw_stuffing_; }

  std::vector<OPJ_BYTE> Finish() {
    if (pending_zeros_ > 0) {
      WriteBit(1);
      pending_zeros_ = 0;
    }
    if (bit_pos_ > 0 || bytes_.empty()) {
      PushByte(cur_byte_);
      cur_byte_ = 0;
      bit_pos_ = 0;
    }
    if (unstuff_) {
      PushByte(0x00);
    }
    return bytes_;
  }

 private:
  void WriteBit(int bit) {
    if (bit_pos_ == 0 && unstuff_) {
      saw_stuffing_ = true;
      bit_pos_ = 1;
    }
    if (bit != 0) {
      cur_byte_ |= static_cast<uint8_t>(1U << (7 - bit_pos_));
    }
    ++bit_pos_;
    if (bit_pos_ == 8) {
      PushByte(cur_byte_);
      cur_byte_ = 0;
      bit_pos_ = 0;
    }
  }

  void PushByte(uint8_t byte) {
    bytes_.push_back(byte);
    unstuff_ = (byte == 0xFF);
  }

  int k_ = 0;
  int pending_zeros_ = 0;
  bool unstuff_ = false;
  bool saw_stuffing_ = false;
  uint8_t cur_byte_ = 0;
  int bit_pos_ = 0;
  std::vector<OPJ_BYTE> bytes_;
};

// Backward-growing LSB-first bit-writer for VLC and MRP segments.
// Prevents >0x8F followed (backwards) by 0x7F/0xFF via 7-bit stuffing.
class ReverseStuffedWriter {
 public:
  explicit ReverseStuffedWriter(bool is_vlc)
      : in_first_nibble_(is_vlc), unstuff_(true) {}

  void WriteBit(uint32_t bit) {
    cur_bits_ |= (bit & 1U) << num_bits_;
    ++num_bits_;
    if (in_first_nibble_) {
      if (num_bits_ == 3 && (cur_bits_ & 0x7U) == 0x7U) {
        first_nibble_ = 0x7;
        unstuff_ = false;
        in_first_nibble_ = false;
        cur_bits_ = 0;
        num_bits_ = 0;
        saw_nibble_stuffing_ = true;
      } else if (num_bits_ == 4) {
        first_nibble_ = static_cast<uint8_t>(cur_bits_ & 0xFU);
        unstuff_ = (first_nibble_ > 0x8);
        in_first_nibble_ = false;
        cur_bits_ = 0;
        num_bits_ = 0;
      }
      return;
    }

    if (unstuff_ && num_bits_ == 7 && (cur_bits_ & 0x7FU) == 0x7FU) {
      saw_byte_stuffing_ = true;
      bytes_.push_back(0x7F);
      unstuff_ = false;
      cur_bits_ = 0;
      num_bits_ = 0;
    } else if (num_bits_ == 8) {
      const uint8_t b = static_cast<uint8_t>(cur_bits_ & 0xFFU);
      bytes_.push_back(b);
      unstuff_ = (b > 0x8F);
      cur_bits_ = 0;
      num_bits_ = 0;
    }
  }

  void WriteBits(uint32_t val, int num_bits) {
    for (int i = 0; i < num_bits; ++i) {
      WriteBit((val >> i) & 1U);
    }
  }

  bool saw_nibble_stuffing() const { return saw_nibble_stuffing_; }
  bool saw_byte_stuffing() const { return saw_byte_stuffing_; }

  std::vector<OPJ_BYTE> Finish(uint8_t* first_nibble_out = nullptr) {
    if (in_first_nibble_) {
      first_nibble_ = static_cast<uint8_t>(cur_bits_ & 0xFU);
      unstuff_ = (first_nibble_ > 0x8);
      in_first_nibble_ = false;
      cur_bits_ = 0;
      num_bits_ = 0;
    } else if (num_bits_ > 0) {
      const uint8_t b = static_cast<uint8_t>(cur_bits_ & 0xFFU);
      bytes_.push_back(b);
      unstuff_ = (b > 0x8F);
      cur_bits_ = 0;
      num_bits_ = 0;
    }
    if (first_nibble_out != nullptr) {
      *first_nibble_out = first_nibble_;
    }
    return std::vector<OPJ_BYTE>(bytes_.rbegin(), bytes_.rend());
  }

 private:
  bool in_first_nibble_;
  bool unstuff_;
  bool saw_nibble_stuffing_ = false;
  bool saw_byte_stuffing_ = false;
  uint32_t cur_bits_ = 0;
  int num_bits_ = 0;
  uint8_t first_nibble_ = 0;
  std::vector<OPJ_BYTE> bytes_;
};

enum class SamplePass {
  kZero,
  kCleanup,
  kSpp,
};

struct SampleSpec {
  SamplePass pass = SamplePass::kZero;
  uint32_t clean_mag = 0;  // Required >= 1 when pass == kCleanup.
  bool negative = false;
  uint8_t mrp_bit = 0;  // 0 or 1 when num_passes == 3 and pass == kCleanup.
};

struct EncodedHtBlock {
  int width = 0;
  int height = 0;
  uint32_t mb = 0;
  uint32_t numbps = 0;
  uint32_t num_passes = 1;
  bool stripe_causal = false;
  std::vector<OPJ_BYTE> seg0;
  std::vector<OPJ_BYTE> seg1;
  std::vector<OPJ_INT32> expected;
  bool saw_magsgn_stuffing = false;
  bool saw_mel_stuffing = false;
  bool saw_vlc_nibble_stuffing = false;
  bool saw_vlc_byte_stuffing = false;
  bool saw_spp_stuffing = false;
  bool saw_mrp_stuffing = false;
};

struct VlcMatch {
  uint32_t cwd = 0;
  int cwd_len = 0;
  uint32_t qinf = 0;
};

std::optional<VlcMatch> FindVlcMatch(bool is_initial_row, uint32_t c_q,
                                     uint32_t rho, uint32_t u_off,
                                     uint32_t actual_e1) {
  const OPJ_UINT16* table = is_initial_row ? vlc_tbl0 : vlc_tbl1;
  std::optional<VlcMatch> best;
  int best_ek_pop = -1;
  for (uint32_t cwd = 0; cwd < 128; ++cwd) {
    const uint16_t entry = table[(c_q << 7) | cwd];
    const int cwd_len = entry & 0x7;
    if (cwd_len == 0 || (cwd & ((1U << cwd_len) - 1U)) != cwd) {
      continue;
    }
    const uint32_t entry_u_off = (entry >> 3) & 1U;
    const uint32_t entry_rho = (entry >> 4) & 0xFU;
    const uint32_t entry_e1 = (entry >> 8) & 0xFU;
    const uint32_t entry_ek = (entry >> 12) & 0xFU;
    if (entry_rho != rho || entry_u_off != u_off) {
      continue;
    }
    if (u_off == 0) {
      if (entry_e1 != 0 || entry_ek != 0) {
        continue;
      }
    } else {
      if ((actual_e1 & entry_ek) != entry_e1 || (entry_e1 & ~entry_ek) != 0) {
        continue;
      }
    }
    const int ek_pop = std::popcount(entry_ek);
    if (ek_pop > best_ek_pop) {
      best_ek_pop = ek_pop;
      best = VlcMatch{.cwd = cwd, .cwd_len = cwd_len, .qinf = entry};
    }
  }
  return best;
}

void WriteUvlcPrefix(ReverseStuffedWriter& vlc, uint32_t val) {
  if (val == 1) {
    vlc.WriteBits(0x1U, 1);
  } else if (val == 2) {
    vlc.WriteBits(0x2U, 2);
  } else if (val <= 4) {
    vlc.WriteBits(0x4U, 3);
  } else {
    vlc.WriteBits(0x0U, 3);
  }
}

void WriteUvlcSuffix(ReverseStuffedWriter& vlc, uint32_t val) {
  if (val <= 2) {
    return;
  }
  if (val <= 4) {
    vlc.WriteBits(val - 3U, 1);
  } else {
    vlc.WriteBits(val - 5U, 5);
  }
}

class HtCodeblockEncoder {
 public:
  static std::optional<EncodedHtBlock> Encode(
      int width, int height, uint32_t mb, uint32_t numbps, uint32_t num_passes,
      bool stripe_causal, std::span<const SampleSpec> grid) {
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
        width * height > 4096 ||
        grid.size() != static_cast<size_t>(width * height) || mb == 0 ||
        mb > 30 || numbps == 0 || numbps > mb + 1 || num_passes == 0 ||
        num_passes > 3) {
      return std::nullopt;
    }
    if (num_passes > 1 && numbps < 2) {
      return std::nullopt;
    }

    const uint32_t zero_bplanes = (mb + 1U) - numbps;
    const uint32_t zero_bplanes_p1 = zero_bplanes + 1U;

    ForwardStuffedWriter magsgn(/*pad_trailing_ff=*/true);
    MelWriter mel;
    ReverseStuffedWriter vlc(/*is_vlc=*/true);
    ForwardStuffedWriter spp(/*pad_trailing_ff=*/true);
    ReverseStuffedWriter mrp(/*is_vlc=*/false);

    std::array<uint32_t, 132> sigma1{};
    std::array<uint32_t, 132> sigma2{};
    std::array<uint32_t, 132> mbr1{};
    std::array<uint32_t, 132> mbr2{};
    std::array<uint8_t, 528> line_state{};
    std::vector<bool> spp_visited(static_cast<size_t>(width * height), false);
    std::vector<OPJ_INT32> expected(static_cast<size_t>(width * height), 0);

    auto at = [&](int x, int y) -> const SampleSpec& {
      return grid[static_cast<size_t>(y * width + x)];
    };

    auto sample_vn = [&](int x, int y, uint32_t* e_out) -> uint32_t {
      if (x >= width || y >= height) {
        *e_out = 0;
        return 0;
      }
      const SampleSpec& s = at(x, y);
      if (s.pass != SamplePass::kCleanup) {
        *e_out = 0;
        return 0;
      }
      const uint32_t vn = 2U * s.clean_mag - 1U;
      *e_out = 32U - static_cast<uint32_t>(std::countl_zero(vn));
      return vn;
    };

    auto prepare_quad = [&](int qx, int qy, uint32_t kappa, uint32_t* rho_out,
                            uint32_t* u_q_out, uint32_t* u_off_out,
                            uint32_t* actual_e1_out, uint32_t* big_u_out) {
      uint32_t rho = 0;
      uint32_t e_max = 0;
      std::array<uint32_t, 4> vn{};
      std::array<uint32_t, 4> e{};
      const std::array<std::pair<int, int>, 4> coords = {
          {{qx, qy}, {qx, qy + 1}, {qx + 1, qy}, {qx + 1, qy + 1}}};
      for (int i = 0; i < 4; ++i) {
        vn[i] = sample_vn(coords[i].first, coords[i].second, &e[i]);
        if (vn[i] > 0) {
          rho |= (1U << i);
          e_max = std::max(e_max, e[i]);
        }
      }
      uint32_t big_u = std::max(kappa, e_max);
      uint32_t u_q = big_u - kappa;
      uint32_t u_off = u_q > 0 ? 1U : 0U;
      uint32_t actual_e1 = 0;
      if (u_off != 0) {
        for (int i = 0; i < 4; ++i) {
          if ((rho & (1U << i)) != 0 && ((vn[i] >> (big_u - 1U)) & 1U) != 0) {
            actual_e1 |= (1U << i);
          }
        }
      }
      *rho_out = rho;
      *u_q_out = u_q;
      *u_off_out = u_off;
      *actual_e1_out = actual_e1;
      *big_u_out = big_u;
    };

    auto encode_quad_magsgn = [&](int qx, int qy, uint32_t qinf, uint32_t big_u,
                                  uint8_t* lsp, bool is_init) {
      const std::array<std::pair<int, int>, 4> coords = {
          {{qx, qy}, {qx, qy + 1}, {qx + 1, qy}, {qx + 1, qy + 1}}};
      for (int idx = 0; idx < 4; ++idx) {
        const uint32_t sig_mask = 1U << (4 + idx);
        if ((qinf & sig_mask) != 0) {
          const SampleSpec& s = at(coords[idx].first, coords[idx].second);
          const uint32_t e_k_bit = (qinf >> (12 + idx)) & 1U;
          const uint32_t m_n = big_u - e_k_bit;
          const uint32_t vn = 2U * s.clean_mag - 1U;
          const uint32_t ms_bits =
              (s.negative ? 1U : 0U) | (vn & (((1U << m_n) - 1U) & ~1U));
          magsgn.WriteBits(ms_bits, static_cast<int>(m_n));

          uint32_t mag = (2U * s.clean_mag + 1U) << (numbps - 1U);
          if (num_passes == 3) {
            mag ^= (1U - static_cast<uint32_t>(s.mrp_bit)) << (numbps - 1U);
            mag |= 1U << (numbps - 2U);
          }
          expected[static_cast<size_t>(coords[idx].second * width +
                                       coords[idx].first)] =
              s.negative ? -static_cast<OPJ_INT32>(mag)
                         : static_cast<OPJ_INT32>(mag);

          if (idx == 1) {
            const uint32_t t = lsp[0] & 0x7FU;
            const uint32_t en =
                32U - static_cast<uint32_t>(std::countl_zero(vn));
            lsp[0] = static_cast<uint8_t>(0x80U | std::max(t, en));
          } else if (idx == 3) {
            const uint32_t en =
                32U - static_cast<uint32_t>(std::countl_zero(vn));
            lsp[0] = static_cast<uint8_t>(0x80U | en);
          }
        } else if (is_init && idx == 3) {
          lsp[0] = 0;
        }
        if (idx == 1) {
          ++lsp;
        }
      }
    };

    auto encode_mrp_stripe = [&](int stripe_y, const uint32_t* cur_sig) {
      for (int i = 0; i < width; i += 8) {
        const uint32_t sig = *cur_sig++;
        if (sig == 0) {
          continue;
        }
        uint32_t col_mask = 0xFU;
        for (int j = 0; j < 8; ++j, col_mask <<= 4) {
          if ((sig & col_mask) == 0) {
            continue;
          }
          uint32_t sample_mask = 0x11111111U & col_mask;
          for (int r = 0; r < 4; ++r, sample_mask <<= 1) {
            if ((sig & sample_mask) != 0) {
              mrp.WriteBit(at(i + j, stripe_y + r).mrp_bit & 1U);
            }
          }
        }
      }
    };

    auto build_mbr_stripe = [&](uint32_t* mbr, const uint32_t* sig) {
      uint32_t prev = 0;
      for (int i = 0; i < width; i += 8, ++mbr, ++sig) {
        uint32_t m = sig[0];
        m |= prev >> 28;
        m |= sig[0] << 4;
        m |= sig[0] >> 4;
        m |= sig[1] << 28;
        prev = sig[0];
        const uint32_t t = m;
        uint32_t z = m;
        z |= (t & 0x77777777U) << 1;
        z |= (t & 0xEEEEEEEEU) >> 1;
        mbr[0] = z & ~sig[0];
      }
    };

    auto process_spp_stripe = [&](int stripe_y, uint32_t pattern,
                                  bool merge_next_stripe, uint32_t* cur_sig,
                                  uint32_t* cur_mbr, uint32_t* nxt_sig,
                                  uint32_t* nxt_mbr) {
      if (merge_next_stripe) {
        uint32_t prev = 0;
        for (int i = 0; i < width; i += 8) {
          const int widx = i >> 3;
          uint32_t t = nxt_sig[widx];
          t |= prev >> 28;
          t |= nxt_sig[widx] << 4;
          t |= nxt_sig[widx] >> 4;
          t |= nxt_sig[widx + 1] << 28;
          prev = nxt_sig[widx];
          if (!stripe_causal) {
            cur_mbr[widx] |= (t & 0x11111111U) << 3;
          }
          cur_mbr[widx] &= ~cur_sig[widx];
        }
      }

      const uint32_t val = 3U << (numbps - 2U);
      static constexpr std::array<uint32_t, 4> kPropMasks = {0x32U, 0x74U,
                                                             0xE8U, 0xC0U};
      for (int i = 0; i < width;
           i += 8, ++cur_sig, ++cur_mbr, ++nxt_sig, ++nxt_mbr) {
        uint32_t mbr = *cur_mbr & pattern;
        uint32_t new_sig = 0;
        if (mbr != 0) {
          for (int n = 0; n < 8; n += 4) {
            uint32_t col_mask = 0xFU << (4 * n);
            const uint32_t inv_sig = ~cur_sig[0] & pattern;
            const int end = n + 4 + i < width ? n + 4 : width - i;
            for (int j = n; j < end; ++j, col_mask <<= 4) {
              if ((col_mask & mbr) == 0) {
                continue;
              }
              uint32_t sample_mask = 0x11111111U & col_mask;
              for (int r = 0; r < 4; ++r, sample_mask <<= 1) {
                if ((mbr & sample_mask) != 0) {
                  const int sx = i + j;
                  const int sy = stripe_y + r;
                  const SampleSpec& s = at(sx, sy);
                  if (s.pass == SamplePass::kSpp) {
                    spp.WriteBit(1);
                    new_sig |= sample_mask;
                    mbr |= (kPropMasks[static_cast<size_t>(r)] << (j * 4)) &
                           inv_sig;
                    spp_visited[static_cast<size_t>(sy * width + sx)] = true;
                  } else {
                    spp.WriteBit(0);
                  }
                }
              }
            }
            if ((new_sig & (0xFFFFU << (4 * n))) != 0) {
              uint32_t sign_col_mask = 0xFU << (4 * n);
              for (int j = n; j < end; ++j, sign_col_mask <<= 4) {
                if ((sign_col_mask & new_sig) == 0) {
                  continue;
                }
                uint32_t sample_mask = 0x11111111U & sign_col_mask;
                for (int r = 0; r < 4; ++r, sample_mask <<= 1) {
                  if ((new_sig & sample_mask) != 0) {
                    const int sx = i + j;
                    const int sy = stripe_y + r;
                    const SampleSpec& s = at(sx, sy);
                    spp.WriteBit(s.negative ? 1U : 0U);
                    expected[static_cast<size_t>(sy * width + sx)] =
                        s.negative ? -static_cast<OPJ_INT32>(val)
                                   : static_cast<OPJ_INT32>(val);
                  }
                }
              }
            }
            if (n == 4) {
              uint32_t t = new_sig >> 28;
              t |= ((t & 0xEU) >> 1) | ((t & 7U) << 1);
              cur_mbr[1] |= t & ~cur_sig[1];
            }
          }
        }
        new_sig |= cur_sig[0];
        const uint32_t ux = (new_sig & 0x88888888U) >> 3;
        const uint32_t tx = ux | (ux << 4) | (ux >> 4);
        if (i > 0) {
          nxt_mbr[-1] |= (ux << 28) & ~nxt_sig[-1];
        }
        nxt_mbr[0] |= tx & ~nxt_sig[0];
        nxt_mbr[1] |= (ux >> 28) & ~nxt_sig[1];
      }
    };

    // Initial two rows (y = 0, 1).
    uint32_t* sip = sigma1.data();
    uint32_t sip_shift = 0;
    uint8_t* lsp = line_state.data();
    lsp[0] = 0;
    uint32_t c_q = 0;

    for (int x = 0; x < width; x += 4) {
      std::array<uint32_t, 2> rho{};
      std::array<uint32_t, 2> u_q{};
      std::array<uint32_t, 2> u_off{};
      std::array<uint32_t, 2> actual_e1{};
      std::array<uint32_t, 2> big_u{};
      std::array<uint32_t, 2> qinf{};

      prepare_quad(x, 0, /*kappa=*/1, &rho[0], &u_q[0], &u_off[0],
                   &actual_e1[0], &big_u[0]);
      if (big_u[0] > zero_bplanes_p1) {
        return std::nullopt;
      }
      if (c_q == 0) {
        mel.EncodeEvent(rho[0] != 0 ? 1 : 0);
      }
      if (c_q != 0 || rho[0] != 0) {
        std::optional<VlcMatch> m = FindVlcMatch(
            /*is_initial_row=*/true, c_q, rho[0], u_off[0], actual_e1[0]);
        if (!m.has_value()) {
          return std::nullopt;
        }
        vlc.WriteBits(m->cwd, m->cwd_len);
        qinf[0] = m->qinf;
      }
      c_q = ((qinf[0] & 0x10U) >> 4) | ((qinf[0] & 0xE0U) >> 5);
      *sip |= (((qinf[0] & 0x30U) >> 4) | ((qinf[0] & 0xC0U) >> 2))
              << sip_shift;

      if (x + 2 < width) {
        prepare_quad(x + 2, 0, /*kappa=*/1, &rho[1], &u_q[1], &u_off[1],
                     &actual_e1[1], &big_u[1]);
        if (big_u[1] > zero_bplanes_p1) {
          return std::nullopt;
        }
        if (c_q == 0) {
          mel.EncodeEvent(rho[1] != 0 ? 1 : 0);
        }
        if (c_q != 0 || rho[1] != 0) {
          std::optional<VlcMatch> m = FindVlcMatch(
              /*is_initial_row=*/true, c_q, rho[1], u_off[1], actual_e1[1]);
          if (!m.has_value()) {
            return std::nullopt;
          }
          vlc.WriteBits(m->cwd, m->cwd_len);
          qinf[1] = m->qinf;
        }
        c_q = ((qinf[1] & 0x10U) >> 4) | ((qinf[1] & 0xE0U) >> 5);
      } else {
        u_q[1] = 0;
        u_off[1] = 0;
        big_u[1] = 1;
      }
      *sip |= (((qinf[1] & 0x30U) | ((qinf[1] & 0xC0U) << 2)))
              << (4 + sip_shift);

      sip += (x & 0x7) != 0 ? 1 : 0;
      sip_shift ^= 0x10U;

      const uint32_t uvlc_mode =
          ((qinf[0] & 0x8U) >> 3) | ((qinf[1] & 0x8U) >> 2);
      if (uvlc_mode == 1) {
        WriteUvlcPrefix(vlc, u_q[0]);
        WriteUvlcSuffix(vlc, u_q[0]);
      } else if (uvlc_mode == 2) {
        WriteUvlcPrefix(vlc, u_q[1]);
        WriteUvlcSuffix(vlc, u_q[1]);
      } else if (uvlc_mode == 3) {
        if (u_q[0] >= 3 && u_q[1] >= 3) {
          mel.EncodeEvent(1);
          const uint32_t v0 = u_q[0] - 2U;
          const uint32_t v1 = u_q[1] - 2U;
          WriteUvlcPrefix(vlc, v0);
          WriteUvlcPrefix(vlc, v1);
          WriteUvlcSuffix(vlc, v0);
          WriteUvlcSuffix(vlc, v1);
        } else {
          mel.EncodeEvent(0);
          if (u_q[0] >= 3) {
            if (u_q[1] > 2) {
              return std::nullopt;
            }
            WriteUvlcPrefix(vlc, u_q[0]);
            vlc.WriteBit(u_q[1] - 1U);
            WriteUvlcSuffix(vlc, u_q[0]);
          } else {
            WriteUvlcPrefix(vlc, u_q[0]);
            WriteUvlcPrefix(vlc, u_q[1]);
            WriteUvlcSuffix(vlc, u_q[0]);
            WriteUvlcSuffix(vlc, u_q[1]);
          }
        }
      }

      encode_quad_magsgn(x, 0, qinf[0], big_u[0], lsp, /*is_init=*/true);
      ++lsp;
      encode_quad_magsgn(x + 2, 0, qinf[1], big_u[1], lsp, /*is_init=*/true);
      ++lsp;
    }

    // Non-initial rows (y >= 2).
    for (int y = 2; y < height;) {
      sip_shift ^= 0x2U;
      sip_shift &= 0xFFFFFFEFU;
      sip = (y & 0x4) != 0 ? sigma2.data() : sigma1.data();

      lsp = line_state.data();
      uint8_t ls0 = lsp[0];
      lsp[0] = 0;
      c_q = 0;

      for (int x = 0; x < width; x += 4) {
        std::array<uint32_t, 2> rho{};
        std::array<uint32_t, 2> u_q{};
        std::array<uint32_t, 2> u_off{};
        std::array<uint32_t, 2> actual_e1{};
        std::array<uint32_t, 2> big_u{};
        std::array<uint32_t, 2> qinf{};

        // Quad 0 context & kappa.
        c_q |= (ls0 >> 7);
        c_q |= (lsp[1] >> 5) & 0x4U;
        uint32_t rho0_raw = 0;
        for (int idx = 0; idx < 4; ++idx) {
          uint32_t e_dummy = 0;
          const int sx = x + (idx >> 1);
          const int sy = y + (idx & 1);
          if (sample_vn(sx, sy, &e_dummy) > 0) {
            rho0_raw |= (1U << idx);
          }
        }
        uint32_t kappa0 = 1;
        if ((rho0_raw & (rho0_raw - 1U)) != 0) {
          const uint32_t e_above = std::max(ls0 & 0x7FU, lsp[1] & 0x7FU);
          kappa0 = e_above > 2U ? (e_above - 1U) : 1U;
        }
        prepare_quad(x, y, kappa0, &rho[0], &u_q[0], &u_off[0], &actual_e1[0],
                     &big_u[0]);
        if (big_u[0] > zero_bplanes_p1) {
          return std::nullopt;
        }
        if (c_q == 0) {
          mel.EncodeEvent(rho[0] != 0 ? 1 : 0);
        }
        if (c_q != 0 || rho[0] != 0) {
          std::optional<VlcMatch> m = FindVlcMatch(
              /*is_initial_row=*/false, c_q, rho[0], u_off[0], actual_e1[0]);
          if (!m.has_value()) {
            return std::nullopt;
          }
          vlc.WriteBits(m->cwd, m->cwd_len);
          qinf[0] = m->qinf;
        }
        c_q = ((qinf[0] & 0x40U) >> 5) | ((qinf[0] & 0x80U) >> 6);
        *sip |= (((qinf[0] & 0x30U) >> 4) | ((qinf[0] & 0xC0U) >> 2))
                << sip_shift;

        // Quad 1 context & kappa.
        if (x + 2 < width) {
          c_q |= (lsp[1] >> 7);
          c_q |= (lsp[2] >> 5) & 0x4U;
          uint32_t rho1_raw = 0;
          for (int idx = 0; idx < 4; ++idx) {
            uint32_t e_dummy = 0;
            const int sx = x + 2 + (idx >> 1);
            const int sy = y + (idx & 1);
            if (sample_vn(sx, sy, &e_dummy) > 0) {
              rho1_raw |= (1U << idx);
            }
          }
          uint32_t kappa1 = 1;
          if ((rho1_raw & (rho1_raw - 1U)) != 0) {
            const uint32_t e_above = std::max(lsp[1] & 0x7FU, lsp[2] & 0x7FU);
            kappa1 = e_above > 2U ? (e_above - 1U) : 1U;
          }
          prepare_quad(x + 2, y, kappa1, &rho[1], &u_q[1], &u_off[1],
                       &actual_e1[1], &big_u[1]);
          if (big_u[1] > zero_bplanes_p1) {
            return std::nullopt;
          }
          if (c_q == 0) {
            mel.EncodeEvent(rho[1] != 0 ? 1 : 0);
          }
          if (c_q != 0 || rho[1] != 0) {
            std::optional<VlcMatch> m = FindVlcMatch(
                /*is_initial_row=*/false, c_q, rho[1], u_off[1], actual_e1[1]);
            if (!m.has_value()) {
              return std::nullopt;
            }
            vlc.WriteBits(m->cwd, m->cwd_len);
            qinf[1] = m->qinf;
          }
          c_q = ((qinf[1] & 0x40U) >> 5) | ((qinf[1] & 0x80U) >> 6);
        } else {
          u_q[1] = 0;
          u_off[1] = 0;
          big_u[1] = 1;
        }
        *sip |= (((qinf[1] & 0x30U) | ((qinf[1] & 0xC0U) << 2)))
                << (4 + sip_shift);

        sip += (x & 0x7) != 0 ? 1 : 0;
        sip_shift ^= 0x10U;

        const uint32_t uvlc_mode =
            ((qinf[0] & 0x8U) >> 3) | ((qinf[1] & 0x8U) >> 2);
        if (uvlc_mode == 1) {
          WriteUvlcPrefix(vlc, u_q[0]);
          WriteUvlcSuffix(vlc, u_q[0]);
        } else if (uvlc_mode == 2) {
          WriteUvlcPrefix(vlc, u_q[1]);
          WriteUvlcSuffix(vlc, u_q[1]);
        } else if (uvlc_mode == 3) {
          WriteUvlcPrefix(vlc, u_q[0]);
          WriteUvlcPrefix(vlc, u_q[1]);
          WriteUvlcSuffix(vlc, u_q[0]);
          WriteUvlcSuffix(vlc, u_q[1]);
        }

        ls0 = lsp[2];
        lsp[1] = 0;
        lsp[2] = 0;

        encode_quad_magsgn(x, y, qinf[0], big_u[0], lsp, /*is_init=*/false);
        ++lsp;
        encode_quad_magsgn(x + 2, y, qinf[1], big_u[1], lsp,
                           /*is_init=*/false);
        ++lsp;
      }

      y += 2;
      if (num_passes > 1 && (y & 3) == 0) {
        if (num_passes > 2) {
          uint32_t* cur_sig = (y & 0x4) != 0 ? sigma1.data() : sigma2.data();
          encode_mrp_stripe(y - 4, cur_sig);
        }
        if (y >= 4) {
          uint32_t* sig = (y & 0x4) != 0 ? sigma1.data() : sigma2.data();
          uint32_t* mbr = (y & 0x4) != 0 ? mbr1.data() : mbr2.data();
          build_mbr_stripe(mbr, sig);
        }
        if (y >= 8) {
          uint32_t* cur_sig = (y & 0x4) != 0 ? sigma2.data() : sigma1.data();
          uint32_t* cur_mbr = (y & 0x4) != 0 ? mbr2.data() : mbr1.data();
          uint32_t* nxt_sig = (y & 0x4) != 0 ? sigma1.data() : sigma2.data();
          uint32_t* nxt_mbr = (y & 0x4) != 0 ? mbr1.data() : mbr2.data();
          process_spp_stripe(y - 8, 0xFFFFFFFFU, /*merge_next_stripe=*/true,
                             cur_sig, cur_mbr, nxt_sig, nxt_mbr);
          std::memset(cur_sig, 0,
                      ((((static_cast<uint32_t>(width) + 7U) >> 3) + 1U) << 2));
        }
      }
    }

    // Terminating SPP and MRP for remaining incomplete stripes.
    if (num_passes > 1) {
      if (num_passes > 2 && ((height & 3) == 1 || (height & 3) == 2)) {
        uint32_t* cur_sig = (height & 0x4) != 0 ? sigma2.data() : sigma1.data();
        encode_mrp_stripe(height & ~3, cur_sig);
      }
      if ((height & 3) == 1 || (height & 3) == 2) {
        uint32_t* sig = (height & 0x4) != 0 ? sigma2.data() : sigma1.data();
        uint32_t* mbr = (height & 0x4) != 0 ? mbr2.data() : mbr1.data();
        build_mbr_stripe(mbr, sig);
      }
      int st = height;
      st -= height > 6 ? (((height + 1) & 3) + 3) : height;
      for (int y = st; y < height; y += 4) {
        uint32_t pattern = 0xFFFFFFFFU;
        if (height - y == 3) {
          pattern = 0x77777777U;
        } else if (height - y == 2) {
          pattern = 0x33333333U;
        } else if (height - y == 1) {
          pattern = 0x11111111U;
        }
        uint32_t* cur_sig = (y & 0x4) != 0 ? sigma2.data() : sigma1.data();
        uint32_t* cur_mbr = (y & 0x4) != 0 ? mbr2.data() : mbr1.data();
        uint32_t* nxt_sig = (y & 0x4) != 0 ? sigma1.data() : sigma2.data();
        uint32_t* nxt_mbr = (y & 0x4) != 0 ? mbr1.data() : mbr2.data();
        process_spp_stripe(y, pattern, (height - y) > 4, cur_sig, cur_mbr,
                           nxt_sig, nxt_mbr);
      }
    }

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        if (at(x, y).pass == SamplePass::kSpp &&
            !spp_visited[static_cast<size_t>(y * width + x)]) {
          return std::nullopt;
        }
      }
    }

    const std::vector<OPJ_BYTE> magsgn_bytes = magsgn.Finish();
    const std::vector<OPJ_BYTE> mel_bytes = mel.Finish();
    uint8_t first_nibble = 0;
    const std::vector<OPJ_BYTE> vlc_bytes = vlc.Finish(&first_nibble);

    const size_t scup = mel_bytes.size() + vlc_bytes.size() + 2;
    if (scup < 2 || scup > 4079) {
      return std::nullopt;
    }

    std::vector<OPJ_BYTE> seg0;
    seg0.reserve(magsgn_bytes.size() + scup);
    seg0.insert(seg0.end(), magsgn_bytes.begin(), magsgn_bytes.end());
    seg0.insert(seg0.end(), mel_bytes.begin(), mel_bytes.end());
    seg0.insert(seg0.end(), vlc_bytes.begin(), vlc_bytes.end());
    seg0.push_back(static_cast<OPJ_BYTE>((first_nibble << 4) | (scup & 0xFU)));
    seg0.push_back(static_cast<OPJ_BYTE>((scup >> 4) & 0xFFU));

    std::vector<OPJ_BYTE> seg1;
    if (num_passes > 1) {
      const std::vector<OPJ_BYTE> spp_bytes = spp.Finish();
      const std::vector<OPJ_BYTE> mrp_bytes =
          (num_passes > 2) ? mrp.Finish() : std::vector<OPJ_BYTE>{};
      seg1.reserve(spp_bytes.size() + mrp_bytes.size() + 1);
      seg1.insert(seg1.end(), spp_bytes.begin(), spp_bytes.end());
      seg1.insert(seg1.end(), mrp_bytes.begin(), mrp_bytes.end());
      if (seg1.empty()) {
        seg1.push_back(0x00);
      }
    }

    return EncodedHtBlock{
        .width = width,
        .height = height,
        .mb = mb,
        .numbps = numbps,
        .num_passes = num_passes,
        .stripe_causal = stripe_causal,
        .seg0 = std::move(seg0),
        .seg1 = std::move(seg1),
        .expected = std::move(expected),
        .saw_magsgn_stuffing = magsgn.saw_stuffing(),
        .saw_mel_stuffing = mel.saw_stuffing(),
        .saw_vlc_nibble_stuffing = vlc.saw_nibble_stuffing(),
        .saw_vlc_byte_stuffing = vlc.saw_byte_stuffing(),
        .saw_spp_stuffing = spp.saw_stuffing(),
        .saw_mrp_stuffing = mrp.saw_byte_stuffing(),
    };
  }
};

// Decodes a single-chunk EncodedHtBlock into a fresh or caller-provided
// opj_t1_t.
OPJ_BOOL DecodeEncodedBlock(opj_t1_t* t1, const EncodedHtBlock& block,
                            opj_event_mgr_t* event_mgr = nullptr,
                            opj_mutex_t* mutex = nullptr, OPJ_INT32 x0 = 0,
                            OPJ_INT32 y0 = 0) {
  std::vector<OPJ_BYTE> payload;
  payload.reserve(block.seg0.size() + block.seg1.size());
  payload.insert(payload.end(), block.seg0.begin(), block.seg0.end());
  payload.insert(payload.end(), block.seg1.begin(), block.seg1.end());

  std::array<opj_tcd_seg_t, 2> segs{};
  segs[0].len = static_cast<OPJ_UINT32>(block.seg0.size());
  segs[0].real_num_passes = 1;
  if (block.num_passes > 1) {
    segs[1].len = static_cast<OPJ_UINT32>(block.seg1.size());
    segs[1].real_num_passes = block.num_passes - 1U;
  }

  opj_tcd_seg_data_chunk_t chunk{};
  chunk.data = payload.data();
  chunk.len = static_cast<OPJ_UINT32>(payload.size());

  opj_tcd_cblk_dec_t cblk{};
  cblk.segs = segs.data();
  cblk.chunks = &chunk;
  cblk.x0 = x0;
  cblk.y0 = y0;
  cblk.x1 = x0 + block.width;
  cblk.y1 = y0 + block.height;
  cblk.Mb = block.mb;
  cblk.numbps = block.numbps;
  cblk.numsegs = block.num_passes > 1 ? 2U : 1U;
  cblk.real_num_segs = cblk.numsegs;
  cblk.numchunks = 1U;

  const OPJ_UINT32 cblksty =
      J2K_CCP_CBLKSTY_HT | (block.stripe_causal ? J2K_CCP_CBLKSTY_VSC : 0U);
  return opj_t1_ht_decode_cblk(t1, &cblk, /*orient=*/0, /*roishift=*/0, cblksty,
                               event_mgr, mutex,
                               /*check_pterm=*/OPJ_FALSE);
}

std::vector<OPJ_INT32> ReadT1Samples(const opj_t1_t* t1) {
  const size_t count = static_cast<size_t>(t1->w) * t1->h;
  return std::vector<OPJ_INT32>(t1->data, t1->data + count);
}

// ============================================================================
// Parameter validation and error guard tests
// ============================================================================

TEST(HtDecTest, RejectsNonZeroRoiShiftWithAndWithoutMutex) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  std::array<OPJ_BYTE, 4> data = {0x00, 0x00, 0x63, 0x00};
  opj_tcd_seg_t seg{};
  seg.len = 4;
  seg.real_num_passes = 1;
  opj_tcd_seg_data_chunk_t chunk{.data = data.data(), .len = 4};
  opj_tcd_cblk_dec_t cblk{};
  cblk.segs = &seg;
  cblk.chunks = &chunk;
  cblk.x1 = 1;
  cblk.y1 = 1;
  cblk.Mb = 4;
  cblk.numbps = 2;
  cblk.numsegs = 1;
  cblk.numchunks = 1;

  for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
    EventCapture capture;
    opj_event_mgr_t mgr = MakeEventManager(&capture);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, /*orient=*/1,
                                    /*roishift=*/1, J2K_CCP_CBLKSTY_HT, &mgr, m,
                                    OPJ_TRUE));
    ASSERT_THAT(capture.errors, Not(IsEmpty()));
    EXPECT_THAT(capture.errors[0], HasSubstr("ROI"));
  }
}

TEST(HtDecTest, ReturnsTrueImmediatelyWhenMbIsZero) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  opj_tcd_cblk_dec_t cblk{};
  cblk.x1 = 4;
  cblk.y1 = 4;
  cblk.Mb = 0;

  EventCapture capture;
  opj_event_mgr_t mgr = MakeEventManager(&capture);
  EXPECT_EQ(OPJ_TRUE,
            opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                  &mgr, nullptr, OPJ_FALSE));
  EXPECT_THAT(capture.errors, IsEmpty());
  EXPECT_EQ(4U, t1->w);
  EXPECT_EQ(4U, t1->h);
  EXPECT_THAT(ReadT1Samples(t1.get()),
              ElementsAreArray(std::vector<OPJ_INT32>(16, 0)));
}

TEST(HtDecTest, HandlesZeroChunksWithAndWithoutCblkDataBuffer) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  opj_tcd_cblk_dec_t cblk{};
  cblk.x1 = 2;
  cblk.y1 = 2;
  cblk.Mb = 4;
  cblk.numbps = 4;
  cblk.numchunks = 0;

  // Without mustuse_cblkdatabuffer, numchunks == 0 returns OPJ_TRUE cleanly.
  t1->mustuse_cblkdatabuffer = OPJ_FALSE;
  EXPECT_EQ(OPJ_TRUE,
            opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                  nullptr, nullptr, OPJ_FALSE));

  // With mustuse_cblkdatabuffer and NULL cblkdatabuffer, cblk_len == 0 skips
  // allocation and hits the cblkdata == NULL guard.
  t1->mustuse_cblkdatabuffer = OPJ_TRUE;
  EXPECT_EQ(OPJ_FALSE,
            opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                  nullptr, nullptr, OPJ_FALSE));
}

TEST(HtDecTest, RejectsTooManyPassesAndWarnsOnZeroLengthRefinementSegment) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  const std::vector<SampleSpec> grid = {
      {.pass = SamplePass::kCleanup, .clean_mag = 1, .negative = false},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kZero},
  };
  // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
  // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
  const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
      /*width=*/2, /*height=*/2, /*mb=*/6, /*numbps=*/3, /*num_passes=*/1,
      /*stripe_causal=*/false, grid);
  ASSERT_TRUE(encoded.has_value());

  std::vector<OPJ_BYTE> payload = encoded->seg0;
  payload.push_back(0x00);
  payload.push_back(0x00);

  for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
    // 1. num_passes > 1 with lengths2 == 0 warns and decodes cleanup pass.
    std::array<opj_tcd_seg_t, 2> segs{};
    segs[0].len = static_cast<OPJ_UINT32>(encoded->seg0.size());
    segs[0].real_num_passes = 1;
    segs[1].len = 0;
    segs[1].real_num_passes = 1;
    opj_tcd_seg_data_chunk_t chunk{
        .data = payload.data(), .len = static_cast<OPJ_UINT32>(payload.size())};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = segs.data();
    cblk.chunks = &chunk;
    cblk.x1 = 2;
    cblk.y1 = 2;
    cblk.Mb = 6;
    cblk.numbps = 3;
    cblk.numsegs = 2;
    cblk.numchunks = 1;

    EventCapture warn_capture;
    opj_event_mgr_t warn_mgr = MakeEventManager(&warn_capture);
    EXPECT_EQ(OPJ_TRUE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &warn_mgr, m, OPJ_FALSE));
    ASSERT_THAT(warn_capture.warnings, Not(IsEmpty()));
    EXPECT_THAT(warn_capture.warnings[0], HasSubstr("zero length"));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));

    // 2. num_passes > 3 fails with error.
    segs[1].len = 2;
    segs[1].real_num_passes = 3;
    EventCapture err_capture;
    opj_event_mgr_t err_mgr = MakeEventManager(&err_capture);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &err_mgr, m, OPJ_FALSE));
    ASSERT_THAT(err_capture.errors, Not(IsEmpty()));
    EXPECT_THAT(err_capture.errors[0], HasSubstr("more than 3 coding passes"));
  }
}

TEST(HtDecTest, RejectsUnsupportedMbAndZeroBitplanesGreaterThanMb) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  std::array<OPJ_BYTE, 4> data = {0x00, 0x00, 0x63, 0x00};
  opj_tcd_seg_t seg{.len = 4, .real_num_passes = 1};
  opj_tcd_seg_data_chunk_t chunk{.data = data.data(), .len = 4};
  opj_tcd_cblk_dec_t cblk{};
  cblk.segs = &seg;
  cblk.chunks = &chunk;
  cblk.x1 = 1;
  cblk.y1 = 1;
  cblk.numsegs = 1;
  cblk.numchunks = 1;

  for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
    // Mb > 30.
    cblk.Mb = 31;
    cblk.numbps = 4;
    EventCapture mb_capture;
    opj_event_mgr_t mb_mgr = MakeEventManager(&mb_capture);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &mb_mgr, m, OPJ_FALSE));
    ASSERT_THAT(mb_capture.errors, Not(IsEmpty()));
    EXPECT_THAT(mb_capture.errors[0], HasSubstr("larger than 30"));

    // zero_bplanes = (Mb + 1) - numbps > Mb when numbps == 0.
    cblk.Mb = 8;
    cblk.numbps = 0;
    EventCapture zbp_capture;
    opj_event_mgr_t zbp_mgr = MakeEventManager(&zbp_capture);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &zbp_mgr, m, OPJ_FALSE));
    ASSERT_THAT(zbp_capture.errors, Not(IsEmpty()));
    EXPECT_THAT(zbp_capture.errors[0], HasSubstr("zero bitplanes"));
  }
}

// Single-threaded test hitting ht_dec.c's one-shot static flag
// `only_cleanup_pass_is_decoded`. Does not assert on warning text so Testify
// randomized ordering and repeated runs remain valid.
TEST(HtDecTest, ZeroBitplanesEqualToMbWithMultiplePassesDecodesCleanupOnly) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  const std::vector<SampleSpec> grid = {
      {.pass = SamplePass::kCleanup, .clean_mag = 1, .negative = true},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kCleanup, .clean_mag = 1, .negative = false},
      {.pass = SamplePass::kZero},
  };
  // numbps == 1 and Mb == 4 yields zero_bplanes == 4 == Mb.
  // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
  // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
  const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
      /*width=*/2, /*height=*/2, /*mb=*/4, /*numbps=*/1, /*num_passes=*/1,
      /*stripe_causal=*/false, grid);
  ASSERT_TRUE(encoded.has_value());

  std::vector<OPJ_BYTE> payload = encoded->seg0;
  payload.push_back(0x00);

  std::array<opj_tcd_seg_t, 2> segs{};
  segs[0].len = static_cast<OPJ_UINT32>(encoded->seg0.size());
  segs[0].real_num_passes = 1;
  segs[1].len = 1;
  segs[1].real_num_passes = 1;

  opj_tcd_seg_data_chunk_t chunk{
      .data = payload.data(), .len = static_cast<OPJ_UINT32>(payload.size())};
  opj_tcd_cblk_dec_t cblk{};
  cblk.segs = segs.data();
  cblk.chunks = &chunk;
  cblk.x1 = 2;
  cblk.y1 = 2;
  cblk.Mb = 4;
  cblk.numbps = 1;
  cblk.numsegs = 2;
  cblk.numchunks = 1;

  EventCapture capture;
  opj_event_mgr_t mgr = MakeEventManager(&capture);
  EXPECT_EQ(OPJ_TRUE,
            opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                  &mgr, mutex.get(), OPJ_FALSE));
  EXPECT_THAT(capture.errors, IsEmpty());
  EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
}

TEST(HtDecTest, RejectsInvalidSegmentAndChunkLengths) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  std::array<OPJ_BYTE, 8> data{};
  for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
    opj_tcd_seg_data_chunk_t chunk{.data = data.data(), .len = 6};
    std::array<opj_tcd_seg_t, 2> segs{};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = segs.data();
    cblk.chunks = &chunk;
    cblk.x1 = 2;
    cblk.y1 = 2;
    cblk.Mb = 6;
    cblk.numbps = 3;
    cblk.numchunks = 1;

    // 1. numsegs == 0 => lengths1 == 0 < 2.
    cblk.numsegs = 0;
    EventCapture cap0;
    opj_event_mgr_t mgr0 = MakeEventManager(&cap0);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &mgr0, m, OPJ_FALSE));
    ASSERT_THAT(cap0.errors, Not(IsEmpty()));
    EXPECT_THAT(cap0.errors[0], HasSubstr("Invalid codeblock length"));

    // 2. lengths1 == 1 < 2.
    cblk.numsegs = 1;
    segs[0].len = 1;
    segs[0].real_num_passes = 1;
    EventCapture cap1;
    opj_event_mgr_t mgr1 = MakeEventManager(&cap1);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &mgr1, m, OPJ_FALSE));
    ASSERT_THAT(cap1.errors, Not(IsEmpty()));

    // 3. lengths1 > cblk_len.
    segs[0].len = 10;
    EventCapture cap2;
    opj_event_mgr_t mgr2 = MakeEventManager(&cap2);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &mgr2, m, OPJ_FALSE));
    ASSERT_THAT(cap2.errors, Not(IsEmpty()));

    // 4. lengths1 + lengths2 > cblk_len.
    cblk.numsegs = 2;
    segs[0].len = 4;
    segs[1].len = 4;
    segs[1].real_num_passes = 1;
    EventCapture cap3;
    opj_event_mgr_t mgr3 = MakeEventManager(&cap3);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &mgr3, m, OPJ_FALSE));
    ASSERT_THAT(cap3.errors, Not(IsEmpty()));
  }
}

TEST(HtDecTest, RejectsOutOfRangeScupInCleanupTrailer) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  struct ScupCase {
    std::string_view name;
    size_t lcup;
    OPJ_BYTE b_lcup_minus_2;
    OPJ_BYTE b_lcup_minus_1;
  };
  constexpr ScupCase kCases[] = {
      {.name = "ScupZero",
       .lcup = 4,
       .b_lcup_minus_2 = 0x00,
       .b_lcup_minus_1 = 0x00},
      {.name = "ScupOne",
       .lcup = 4,
       .b_lcup_minus_2 = 0x01,
       .b_lcup_minus_1 = 0x00},
      {.name = "ScupGreaterThanLcup",
       .lcup = 4,
       .b_lcup_minus_2 = 0x05,
       .b_lcup_minus_1 = 0x00},
      {.name = "ScupGreaterThan4079",
       .lcup = 4080,
       .b_lcup_minus_2 = 0x00,
       .b_lcup_minus_1 = 0xFF},
  };

  for (const ScupCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::vector<OPJ_BYTE> data(tc.lcup, 0x00);
    data[tc.lcup - 2] = tc.b_lcup_minus_2;
    data[tc.lcup - 1] = tc.b_lcup_minus_1;

    opj_tcd_seg_t seg{.len = static_cast<OPJ_UINT32>(tc.lcup),
                      .real_num_passes = 1};
    opj_tcd_seg_data_chunk_t chunk{.data = data.data(),
                                   .len = static_cast<OPJ_UINT32>(tc.lcup)};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = &seg;
    cblk.chunks = &chunk;
    cblk.x1 = 2;
    cblk.y1 = 2;
    cblk.Mb = 4;
    cblk.numbps = 3;
    cblk.numsegs = 1;
    cblk.numchunks = 1;

    for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
      EventCapture capture;
      opj_event_mgr_t mgr = MakeEventManager(&capture);
      EXPECT_EQ(OPJ_FALSE,
                opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                      &mgr, m, OPJ_FALSE));
      ASSERT_THAT(capture.errors, Not(IsEmpty()));
      EXPECT_THAT(capture.errors[0], HasSubstr("2 <= Scup"));
    }
  }
}

TEST(HtDecTest, RejectsInvalidMelMarkerSequenceInMelInit) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  // Place 0xFF immediately followed by 0x90 (> 0x8F) at the start of MEL on
  // a 4-byte aligned address (num = 4 in mel_init) so mel_init sees unstuff
  // followed by > 0x8F and fails.
  std::vector<OPJ_BYTE> storage(16, 0x00);
  const uintptr_t base = reinterpret_cast<uintptr_t>(storage.data());
  const size_t align_offset = (4 - (base & 0x3U)) & 0x3U;
  OPJ_BYTE* data = storage.data() + align_offset;
  // lcup = 8, scup = 8: MEL starts at data[0], which is 4-byte aligned.
  data[0] = 0xFF;
  data[1] = 0x90;
  data[6] = 0x08;
  data[7] = 0x00;

  opj_tcd_seg_t seg{.len = 8, .real_num_passes = 1};
  opj_tcd_seg_data_chunk_t chunk{.data = data, .len = 8};
  opj_tcd_cblk_dec_t cblk{};
  cblk.segs = &seg;
  cblk.chunks = &chunk;
  cblk.x1 = 2;
  cblk.y1 = 2;
  cblk.Mb = 4;
  cblk.numbps = 3;
  cblk.numsegs = 1;
  cblk.numchunks = 1;

  for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
    EventCapture capture;
    opj_event_mgr_t mgr = MakeEventManager(&capture);
    EXPECT_EQ(OPJ_FALSE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    &mgr, m, OPJ_FALSE));
    ASSERT_THAT(capture.errors, Not(IsEmpty()));
    EXPECT_THAT(capture.errors[0], HasSubstr("Incorrect MEL segment sequence"));
  }
}

TEST(HtDecTest, RejectsExcessiveUqOnInitialAndNonInitialRows) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  // 4x4 grid with clean_mag = 5 (U_q = 4) placed at one of:
  // (0,0) -> initial row quad 0, (2,0) -> initial row quad 1,
  // (0,2) -> non-initial row quad 0, (2,2) -> non-initial row quad 1.
  struct UqTestCase {
    std::string_view name;
    int x;
    int y;
  };
  constexpr UqTestCase kCases[] = {
      {.name = "InitRowQuad0", .x = 0, .y = 0},
      {.name = "InitRowQuad1", .x = 2, .y = 0},
      {.name = "NonInitRowQuad0", .x = 0, .y = 2},
      {.name = "NonInitRowQuad1", .x = 2, .y = 2},
  };

  for (const UqTestCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::vector<SampleSpec> grid(16);
    grid[static_cast<size_t>(tc.y * 4 + tc.x)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 5, .negative = false};
    // Valid encode with Mb=8, numbps=6 => zero_bplanes_p1 = 4 >= U_q(4).
    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        4, 4, /*mb=*/8, /*numbps=*/6, 1, false, grid);
    ASSERT_TRUE(encoded.has_value());

    // Decode with numbps=8 => zero_bplanes = 1 => zero_bplanes_p1 = 2 < U_q(4).
    std::vector<OPJ_BYTE> payload = encoded->seg0;
    opj_tcd_seg_t seg{.len = static_cast<OPJ_UINT32>(payload.size()),
                      .real_num_passes = 1};
    opj_tcd_seg_data_chunk_t chunk{
        .data = payload.data(), .len = static_cast<OPJ_UINT32>(payload.size())};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = &seg;
    cblk.chunks = &chunk;
    cblk.x1 = 4;
    cblk.y1 = 4;
    cblk.Mb = 8;
    cblk.numbps = 8;
    cblk.numsegs = 1;
    cblk.numchunks = 1;

    for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
      EventCapture capture;
      opj_event_mgr_t mgr = MakeEventManager(&capture);
      EXPECT_EQ(OPJ_FALSE,
                opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                      &mgr, m, OPJ_FALSE));
      ASSERT_THAT(capture.errors, Not(IsEmpty()));
      EXPECT_THAT(capture.errors[0], HasSubstr("U_q"));
    }
  }
}

TEST(HtDecTest, RejectsSignificantSamplesOutsideCodeblockBounds) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  OpjMutexPtr mutex(opj_mutex_create());
  ASSERT_NE(nullptr, mutex.get());

  struct BoundsCase {
    std::string_view name;
    int enc_w;
    int enc_h;
    int sig_x;
    int sig_y;
    int dec_w;
    int dec_h;
  };
  constexpr BoundsCase kCases[] = {
      {.name = "InitRowExceedsHeight",
       .enc_w = 2,
       .enc_h = 2,
       .sig_x = 0,
       .sig_y = 1,
       .dec_w = 2,
       .dec_h = 1},
      {.name = "InitRowExceedsWidth",
       .enc_w = 4,
       .enc_h = 2,
       .sig_x = 3,
       .sig_y = 0,
       .dec_w = 3,
       .dec_h = 2},
      {.name = "NonInitRowExceedsHeight",
       .enc_w = 4,
       .enc_h = 4,
       .sig_x = 0,
       .sig_y = 3,
       .dec_w = 4,
       .dec_h = 3},
      {.name = "NonInitRowExceedsWidth",
       .enc_w = 4,
       .enc_h = 4,
       .sig_x = 3,
       .sig_y = 2,
       .dec_w = 3,
       .dec_h = 4},
  };

  for (const BoundsCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::vector<SampleSpec> grid(static_cast<size_t>(tc.enc_w * tc.enc_h));
    grid[static_cast<size_t>(tc.sig_y * tc.enc_w + tc.sig_x)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 1, .negative = false};
    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        tc.enc_w, tc.enc_h, /*mb=*/6, /*numbps=*/3, 1, false, grid);
    ASSERT_TRUE(encoded.has_value());

    std::vector<OPJ_BYTE> payload = encoded->seg0;
    opj_tcd_seg_t seg{.len = static_cast<OPJ_UINT32>(payload.size()),
                      .real_num_passes = 1};
    opj_tcd_seg_data_chunk_t chunk{
        .data = payload.data(), .len = static_cast<OPJ_UINT32>(payload.size())};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = &seg;
    cblk.chunks = &chunk;
    cblk.x1 = tc.dec_w;
    cblk.y1 = tc.dec_h;
    cblk.Mb = 6;
    cblk.numbps = 3;
    cblk.numsegs = 1;
    cblk.numchunks = 1;

    for (opj_mutex_t* m : {static_cast<opj_mutex_t*>(nullptr), mutex.get()}) {
      EventCapture capture;
      opj_event_mgr_t mgr = MakeEventManager(&capture);
      EXPECT_EQ(OPJ_FALSE,
                opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                      &mgr, m, OPJ_FALSE));
      ASSERT_THAT(capture.errors, Not(IsEmpty()));
      EXPECT_THAT(capture.errors[0], HasSubstr("outside the codeblock area"));
    }
  }
}

// ============================================================================
// Multi-chunk concatenation, buffer reuse, and alignment tests
// ============================================================================

TEST(HtDecTest, DecodesMultiChunkGrowsCblkDataBufferAndReusesT1Buffers) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  auto make_checker = [](int w, int h, uint32_t mb, uint32_t numbps,
                         uint32_t passes) {
    std::vector<SampleSpec> grid(static_cast<size_t>(w * h));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (((x + y) % 3) == 0) {
          grid[static_cast<size_t>(y * w + x)] = {
              .pass = SamplePass::kCleanup,
              .clean_mag = static_cast<uint32_t>((x + y) % 4 + 1),
              .negative = ((x ^ y) & 1) != 0,
              .mrp_bit = static_cast<uint8_t>((x + y) & 1),
          };
        }
      }
    }
    return HtCodeblockEncoder::Encode(w, h, mb, numbps, passes, false, grid);
  };

  // 1. Small 4x4 block split into 3 chunks (initial cblkdatabuffer alloc).
  const std::optional<EncodedHtBlock> small_blk = make_checker(4, 4, 10, 4, 1);
  ASSERT_TRUE(small_blk.has_value());
  ASSERT_GE(small_blk->seg0.size(), 3U);

  auto decode_3chunks = [&](const EncodedHtBlock& blk) {
    std::vector<OPJ_BYTE> full;
    full.insert(full.end(), blk.seg0.begin(), blk.seg0.end());
    full.insert(full.end(), blk.seg1.begin(), blk.seg1.end());
    const size_t cut1 = full.size() / 3;
    const size_t cut2 = (2 * full.size()) / 3;
    std::array<opj_tcd_seg_data_chunk_t, 3> chunks = {{
        {.data = full.data(), .len = static_cast<OPJ_UINT32>(cut1)},
        {.data = full.data() + cut1,
         .len = static_cast<OPJ_UINT32>(cut2 - cut1)},
        {.data = full.data() + cut2,
         .len = static_cast<OPJ_UINT32>(full.size() - cut2)},
    }};
    std::array<opj_tcd_seg_t, 2> segs{};
    segs[0].len = static_cast<OPJ_UINT32>(blk.seg0.size());
    segs[0].real_num_passes = 1;
    if (blk.num_passes > 1) {
      segs[1].len = static_cast<OPJ_UINT32>(blk.seg1.size());
      segs[1].real_num_passes = blk.num_passes - 1U;
    }
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = segs.data();
    cblk.chunks = chunks.data();
    cblk.x1 = blk.width;
    cblk.y1 = blk.height;
    cblk.Mb = blk.mb;
    cblk.numbps = blk.numbps;
    cblk.numsegs = blk.num_passes > 1 ? 2U : 1U;
    cblk.numchunks = 3U;
    ASSERT_EQ(OPJ_TRUE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    nullptr, nullptr, OPJ_FALSE));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(blk.expected));
  };

  decode_3chunks(*small_blk);
  const OPJ_UINT32 first_cblkbuf_size = t1->cblkdatabuffersize;
  EXPECT_GT(first_cblkbuf_size, 0U);

  // 2. Larger 12x12 3-pass block split into 3 chunks (grows t1->data and
  // cblkdatabuffer via opj_realloc).
  const std::optional<EncodedHtBlock> large_blk =
      make_checker(12, 12, 10, 4, 3);
  ASSERT_TRUE(large_blk.has_value());
  decode_3chunks(*large_blk);
  EXPECT_GT(t1->cblkdatabuffersize, first_cblkbuf_size);

  // 3. Sentinel-fill t1->data, then reuse t1 for a smaller 2x2 single-chunk
  // block with mustuse_cblkdatabuffer = OPJ_TRUE (reuses existing buffers).
  std::fill_n(t1->data, t1->datasize, -999);
  t1->mustuse_cblkdatabuffer = OPJ_TRUE;
  const std::optional<EncodedHtBlock> tiny_blk = make_checker(2, 2, 10, 4, 1);
  ASSERT_TRUE(tiny_blk.has_value());
  ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *tiny_blk));
  EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(tiny_blk->expected));
}

TEST(HtDecTest, DecodesAtAllFourBufferByteAlignments) {
  constexpr int kW = 12;
  constexpr int kH = 10;
  std::vector<SampleSpec> grid(static_cast<size_t>(kW * kH));
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      if ((x + y) % 2 == 0) {
        grid[static_cast<size_t>(y * kW + x)] = {
            .pass = SamplePass::kCleanup,
            .clean_mag = static_cast<uint32_t>((x * 3 + y) % 7 + 1),
            .negative = (x & 1) != 0,
            .mrp_bit = static_cast<uint8_t>((y >> 1) & 1),
        };
      } else if (x > 0 && y > 0) {
        grid[static_cast<size_t>(y * kW + x)] = {
            .pass = SamplePass::kSpp,
            .negative = (y & 1) != 0,
        };
      }
    }
  }
  // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
  // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
  const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
      kW, kH, /*mb=*/12, /*numbps=*/4,
      /*num_passes=*/3, /*stripe_causal=*/false, grid);
  ASSERT_TRUE(encoded.has_value());

  const size_t total_len = encoded->seg0.size() + encoded->seg1.size();
  std::vector<OPJ_BYTE> backing(total_len + 8, 0xAA);

  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  for (size_t offset = 0; offset < 4; ++offset) {
    SCOPED_TRACE(offset);
    OPJ_BYTE* ptr = backing.data() + offset;
    std::copy(encoded->seg0.begin(), encoded->seg0.end(), ptr);
    std::copy(encoded->seg1.begin(), encoded->seg1.end(),
              ptr + encoded->seg0.size());

    std::array<opj_tcd_seg_t, 2> segs{};
    segs[0].len = static_cast<OPJ_UINT32>(encoded->seg0.size());
    segs[0].real_num_passes = 1;
    segs[1].len = static_cast<OPJ_UINT32>(encoded->seg1.size());
    segs[1].real_num_passes = 2;

    opj_tcd_seg_data_chunk_t chunk{.data = ptr,
                                   .len = static_cast<OPJ_UINT32>(total_len)};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = segs.data();
    cblk.chunks = &chunk;
    cblk.x0 = 5;
    cblk.y0 = 7;
    cblk.x1 = 5 + kW;
    cblk.y1 = 7 + kH;
    cblk.Mb = encoded->mb;
    cblk.numbps = encoded->numbps;
    cblk.numsegs = 2;
    cblk.numchunks = 1;

    ASSERT_EQ(OPJ_TRUE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    nullptr, nullptr, OPJ_FALSE));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }
}

// ============================================================================
// Cleanup pass (1-pass) coverage tests
// ============================================================================

TEST(HtDecTest, DecodesMinimalScup2AndSevenPrefixInitialVlcNibble) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  // 1. Minimal 3-byte codeblock (lcup = 3, scup = 2):
  //    MagSgn = {0x00} (sign = +),
  //    d = 0x62 => upper nibble 0x6 (vlc_tbl0 cwd=0x06, len=4 => rho=1,
  //    u_off=0), MEL sees 0x62 | 0xF = 0x6F (MSB=0 => 1 event), scup = (0x00 <<
  //    4) | 2 = 2.
  {
    std::array<OPJ_BYTE, 3> data = {0x00, 0x62, 0x00};
    opj_tcd_seg_t seg{.len = 3, .real_num_passes = 1};
    opj_tcd_seg_data_chunk_t chunk{.data = data.data(), .len = 3};
    opj_tcd_cblk_dec_t cblk{};
    cblk.segs = &seg;
    cblk.chunks = &chunk;
    cblk.x1 = 1;
    cblk.y1 = 1;
    cblk.Mb = 4;
    cblk.numbps = 2;
    cblk.numsegs = 1;
    cblk.numchunks = 1;

    ASSERT_EQ(OPJ_TRUE,
              opj_t1_ht_decode_cblk(t1.get(), &cblk, 0, 0, J2K_CCP_CBLKSTY_HT,
                                    nullptr, nullptr, OPJ_FALSE));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAre(6));
  }

  // 2. Initial VLC nibble with (tmp & 7) == 7 (triggers 3-bit initial nibble
  //    in rev_init): (0,0) has clean_mag = 2 (U_q = 2, rho = 1, cwd = 0x3F len
  //    7).
  const std::vector<SampleSpec> grid = {
      {.pass = SamplePass::kCleanup, .clean_mag = 2, .negative = true},
  };
  const std::optional<EncodedHtBlock> encoded =
      HtCodeblockEncoder::Encode(1, 1, /*mb=*/6, /*numbps=*/3, 1, false, grid);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(encoded->saw_vlc_nibble_stuffing);
  ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
  EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
}

TEST(HtDecTest, DecodesOddAndBoundaryDimensionsCleanupPass) {
  struct GeomCase {
    std::string_view name;
    int width;
    int height;
    OPJ_INT32 x0;
    OPJ_INT32 y0;
  };
  constexpr GeomCase kCases[] = {
      {.name = "1x1", .width = 1, .height = 1, .x0 = 0, .y0 = 0},
      {.name = "2x1", .width = 2, .height = 1, .x0 = 1, .y0 = 0},
      {.name = "3x1", .width = 3, .height = 1, .x0 = 0, .y0 = 2},
      {.name = "1x2", .width = 1, .height = 2, .x0 = 3, .y0 = 1},
      {.name = "1x3", .width = 1, .height = 3, .x0 = 0, .y0 = 4},
      {.name = "3x7", .width = 3, .height = 7, .x0 = 2, .y0 = 3},
      {.name = "5x5", .width = 5, .height = 5, .x0 = 4, .y0 = 5},
      {.name = "6x6", .width = 6, .height = 6, .x0 = 8, .y0 = 8},
  };

  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  for (const GeomCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::vector<SampleSpec> grid(static_cast<size_t>(tc.width * tc.height));
    for (int y = 0; y < tc.height; ++y) {
      for (int x = 0; x < tc.width; ++x) {
        if (x == 0 || ((x + y) & 1) == 0) {
          grid[static_cast<size_t>(y * tc.width + x)] = {
              .pass = SamplePass::kCleanup,
              .clean_mag = static_cast<uint32_t>(((x * 2 + y) % 5) + 1),
              .negative = ((x + y * 3) & 1) != 0,
          };
        }
      }
    }
    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        tc.width, tc.height, /*mb=*/12, /*numbps=*/3, /*num_passes=*/1,
        /*stripe_causal=*/false, grid);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded, nullptr, nullptr,
                                           tc.x0, tc.y0));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }
}

TEST(HtDecTest, ExercisesAllInitialAndNonInitialUvlcModesAndPrefixes) {
  // 24x6 block with Mb=24, numbps=3 (zero_bplanes_p1 = 23) arranged so quad
  // pairs hit all UVLC modes (0, 1, 2, 3 d1<=2, 3 d1>2, 4 on y==0; 0, 1, 2, 3
  // on y>=2), all prefix lengths (1, 01, 001+1b, 000+5b), gamma_q=1 with both
  // E>2 and E<=2, and both lsp[0] > v_n and lsp[0] <= v_n transitions.
  constexpr int kW = 24;
  constexpr int kH = 6;
  std::vector<SampleSpec> grid(static_cast<size_t>(kW * kH));
  auto set_clean = [&](int x, int y, uint32_t mag, bool neg = false) {
    grid[static_cast<size_t>(y * kW + x)] = {
        .pass = SamplePass::kCleanup, .clean_mag = mag, .negative = neg};
  };

  // Row 0..1 quad pairs (x = 0, 4, 8, 12, 16, 20):
  // Pair 0 (x=0..3): mode 0 (u_off = 0, 0) + E=1, E=1 on bottom row.
  set_clean(0, 0, 1, false);
  set_clean(1, 1, 1, true);
  set_clean(2, 1, 1, false);
  // Pair 1 (x=4..7): mode 1 (u_off = 1, 0), u_q0 = 1 (mag=2).
  set_clean(4, 0, 2, false);
  set_clean(5, 1, 2, true);
  set_clean(6, 0, 1, false);
  // Pair 2 (x=8..11): mode 2 (u_off = 0, 1), u_q1 = 2 (mag=3).
  set_clean(8, 0, 1, true);
  set_clean(10, 0, 3, false);
  set_clean(11, 1, 3, true);
  // Pair 3 (x=12..15): mode 3 with d1 <= 2 (u_q0 = 2, u_q1 = 1).
  set_clean(12, 0, 4, false);
  set_clean(12, 1, 1, true);  // t > v_n in quad 0 lsp[0] update from (11,1)!
  set_clean(14, 0, 2, false);
  // Pair 4 (x=16..19): mode 3 with d1 > 2 (u_q0 = 5 via "000"+5b, u_q1 = 2);
  // (17, 1) sits on row 1 so line_state carries E_above = 6 to row 2.
  set_clean(17, 1, 20, false);  // E=6 => u_q0 = 5
  set_clean(18, 0, 3, true);    // E=3 => u_q1 = 2
  // Pair 5 (x=20..23): mode 4 (MEL=1, u_q0 = 3 => "1", u_q1 = 7 => "000"+5b).
  set_clean(20, 0, 5, true);  // E=4 => u_q0 = 3
  set_clean(21, 1, 6, false);
  set_clean(22, 0, 33, false);  // E=7
  set_clean(22, 1, 1, false);   // t > v_n in quad 1 lsp[0] update from (21,1)!
  set_clean(23, 1, 65, true);   // E=8 => u_q1 = 7 (v1 = 5)

  // Row 2..3: exercises decode_noninit_uvlc modes 0, 1, 2, 3 and gamma_q=1.
  // x=0..3: mode 0 with gamma_q=1 and E_above <= 2 (from pair 0 above).
  set_clean(0, 2, 1, false);
  set_clean(1, 3, 1, true);
  // x=4..7: mode 1, u_q0 = 3 ("001"+1b).
  set_clean(4, 2, 5, false);
  set_clean(6, 2, 1, true);
  // x=8..11: mode 2, u_q1 = 5 ("000"+5b).
  set_clean(8, 2, 1, false);
  set_clean(10, 2, 18, true);
  // x=12..15: mode 3, u_q0 = 1, u_q1 = 4 ("001"+1b).
  set_clean(12, 2, 2, false);
  set_clean(14, 2, 9, true);
  // x=16..19: gamma_q = 1 with E_above = 6 (> 2), so U_q += E - 2 gives
  // kappa = 5, E_max = 4 <= 5 => u_off = 0, AND x=18..19 has E=7 > kappa =>
  // u=2!
  set_clean(16, 2, 5, false);
  set_clean(17, 3, 6, true);
  set_clean(18, 2, 33, false);
  set_clean(19, 3, 1, false);
  // x=20..23: gamma_q = 1 on pair with large ls0 (E=8) => kappa = 7.
  set_clean(20, 2, 2, true);
  set_clean(21, 3, 3, false);
  set_clean(22, 2, 4, true);
  set_clean(23, 3, 5, false);

  // Row 4..5: sigma2 (y & 4) + all-zero pair followed by significant quad.
  set_clean(16, 4, 2, true);
  set_clean(22, 4, 10, false);

  // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
  // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
  const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
      kW, kH, /*mb=*/24, /*numbps=*/3,
      /*num_passes=*/1, /*stripe_causal=*/false, grid);
  ASSERT_TRUE(encoded.has_value());

  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
  EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
}

TEST(HtDecTest, ExercisesAllMelExponentStatesOnLargeZeroAndSparseBlocks) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  // 1. 64x64 all-zero block (4096 samples = 1024 quads) ramps MEL state k
  //    across all 13 mel_exp states (0..12) and fills mel_decode runs queue.
  {
    std::vector<SampleSpec> zeros(64 * 64);
    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        64, 64, /*mb=*/8, /*numbps=*/4, 1, false, zeros);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_TRUE(encoded->saw_mel_stuffing);
    ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }

  // 2. 32x32 sparse block alternating long zero runs and isolated quads to
  //    ramp MEL k up and down and exercise mel_read (>4 bytes and <=4 bytes)
  //    and rev_read (>3 bytes and <=3 bytes).
  {
    std::vector<SampleSpec> sparse(32 * 32);
    for (int y = 0; y < 32; y += 6) {
      for (int x = 0; x < 32; x += 12) {
        sparse[static_cast<size_t>(y * 32 + x)] = {
            .pass = SamplePass::kCleanup,
            .clean_mag = static_cast<uint32_t>((x + y) % 5 + 1),
            .negative = ((x + y) & 2) != 0,
        };
      }
    }
    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        32, 32, /*mb=*/12, /*numbps=*/4, 1, false, sparse);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }

  // 3. 64x64 alternating block (512 MEL events => >60 bytes of MEL, >200 bytes
  //    of VLC, >60 bytes of MagSgn) to guarantee >4 byte mel_read and >3 byte
  //    rev_read / frwd_read bulk loops regardless of heap alignment.
  {
    std::vector<SampleSpec> alt(64 * 64);
    for (int y = 0; y < 64; y += 2) {
      for (int x = 0; x < 64; x += 4) {
        alt[static_cast<size_t>(y * 64 + x)] = {
            .pass = SamplePass::kCleanup,
            .clean_mag = 1,
            .negative = ((x + y) & 4) != 0,
        };
      }
    }
    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        64, 64, /*mb=*/12, /*numbps=*/4, 1, false, alt);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }
}

TEST(HtDecTest, UnstuffsZeroXffAndSevenFSequencesInCleanupStreams) {
  // Row 0..1 sets line_state bits at x = 0, 4, 8, 12 so row 2..3 queries
  // (c_q=1, rho=1, u_off=1 => cwd=0x7F) and (c_q=4, rho=1, u_off=1 => cwd=0x7F)
  // plus u_q=1 (1 bit), producing >50 consecutive 1 bits in VLC (0xFF/0x7F
  // reverse unstuffing). Row 4..5 adds dense negative clean_mag=128 samples
  // for all-ones MagSgn bits (0xFF forward stuffing).
  constexpr int kW = 16;
  constexpr int kH = 6;
  std::vector<SampleSpec> grid(static_cast<size_t>(kW * kH));
  for (int x = 0; x < kW; x += 4) {
    grid[static_cast<size_t>(1 * kW + x)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 1, .negative = false};
  }
  for (int x = 0; x < kW - 2; x += 2) {
    grid[static_cast<size_t>(2 * kW + x)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 2, .negative = true};
  }
  for (int x = 0; x < kW; x += 2) {
    grid[static_cast<size_t>(4 * kW + x)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 128, .negative = true};
  }

  // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
  // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
  const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
      kW, kH, /*mb=*/20, /*numbps=*/4, /*num_passes=*/1,
      /*stripe_causal=*/false, grid);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_TRUE(encoded->saw_magsgn_stuffing);
  EXPECT_TRUE(encoded->saw_vlc_byte_stuffing);

  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());
  ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
  EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));

  // 4x2 block whose initial VLC nibble is 0xD (> 0x8) immediately followed by
  // 0x7F, triggering byte unstuffing inside rev_init itself.
  const std::vector<SampleSpec> rev_init_grid = {
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kCleanup, .clean_mag = 2, .negative = false},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kCleanup, .clean_mag = 2, .negative = true},
      {.pass = SamplePass::kZero},
      {.pass = SamplePass::kZero},
  };
  const std::optional<EncodedHtBlock> rev_init_enc = HtCodeblockEncoder::Encode(
      /*width=*/4, /*height=*/2, /*mb=*/8, /*numbps=*/3, /*num_passes=*/1,
      /*stripe_causal=*/false, rev_init_grid);
  ASSERT_TRUE(rev_init_enc.has_value());
  EXPECT_TRUE(rev_init_enc->saw_vlc_byte_stuffing);

  std::vector<OPJ_BYTE> payload = rev_init_enc->seg0;
  opj_tcd_seg_t seg{.len = static_cast<OPJ_UINT32>(payload.size()),
                    .real_num_passes = 1};
  opj_tcd_seg_data_chunk_t chunk{
      .data = payload.data(), .len = static_cast<OPJ_UINT32>(payload.size())};
  opj_tcd_cblk_dec_t cblk{};
  cblk.segs = &seg;
  cblk.chunks = &chunk;
  cblk.x1 = 4;
  cblk.y1 = 2;
  cblk.Mb = 8;
  cblk.numbps = 3;
  cblk.numsegs = 1;
  cblk.numchunks = 1;
  ASSERT_EQ(OPJ_TRUE, opj_t1_ht_decode_cblk(t1.get(), &cblk, /*orient=*/2,
                                            /*roishift=*/0, J2K_CCP_CBLKSTY_HT,
                                            nullptr, nullptr,
                                            /*check_pterm=*/OPJ_TRUE));
  EXPECT_THAT(ReadT1Samples(t1.get()),
              ElementsAreArray(rev_init_enc->expected));
}

// ============================================================================
// Multi-pass (2-pass CUP+SPP and 3-pass CUP+SPP+MRP) tests
// ============================================================================

TEST(HtDecTest, DecodesTwoAndThreePassesAcrossAllHeightModFourRemainders) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  // Heights 1..10 cover (height & 3) == 0, 1, 2, 3, height <= 6 vs > 6,
  // height - y == 1, 2, 3, 4, > 4, and both y & 4 == 0 and y & 4 != 0.
  for (int height = 1; height <= 10; ++height) {
    for (uint32_t passes : {2U, 3U}) {
      SCOPED_TRACE(height * 10 + static_cast<int>(passes));
      constexpr int kW = 5;
      std::vector<SampleSpec> grid(static_cast<size_t>(kW * height));
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < kW; ++x) {
          if (x == 0 && (y % 2) == 0) {
            grid[static_cast<size_t>(y * kW + x)] = {
                .pass = SamplePass::kCleanup,
                .clean_mag = static_cast<uint32_t>((y % 3) + 1),
                .negative = (y & 2) != 0,
                .mrp_bit = static_cast<uint8_t>((y >> 1) & 1),
            };
          } else if (x == 1 || (x == 0 && (y % 2) == 1)) {
            grid[static_cast<size_t>(y * kW + x)] = {
                .pass = SamplePass::kSpp,
                .negative = ((x + y) & 1) != 0,
            };
          } else if (x == 4 && y == height - 1) {
            grid[static_cast<size_t>(y * kW + x)] = {
                .pass = SamplePass::kCleanup,
                .clean_mag = 2,
                .negative = false,
                .mrp_bit = 1,
            };
          }
        }
      }

      // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
      // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
      const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
          kW, height, /*mb=*/10, /*numbps=*/4, passes,
          /*stripe_causal=*/false, grid);
      ASSERT_TRUE(encoded.has_value());
      ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
      EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
    }
  }
}

TEST(HtDecTest, DecodesWideMultiStripeThreePassesWithAndWithoutStripeCausal) {
  OpjT1Ptr t1(opj_t1_create(OPJ_FALSE));
  ASSERT_NE(nullptr, t1.get());

  // 1. 16x16 3-pass block comparing stripe_causal OFF vs ON:
  //    Row 4 (col 2), row 8 (col 2), and row 12 (col 2) are kCleanup while
  //    rows 3, 7, 11 at col 2 are insignificant in cleanup.
  //    - With stripe_causal == false: rows 3, 7, 11 become kSpp significant.
  //    - With stripe_causal == true: upward cross-stripe propagation is
  //    disabled. Also includes chained horizontal/vertical SPP across columns
  //    0..15 (exercising n=4 -> cur_mbr[1] and i>0 nxt_mbr[-1]), empty sig/mbr
  //    groups (cols 8..15 on rows 0..3), and dense mrp_bit=1 / SPP=1 for
  //    stuffing.
  for (bool stripe_causal : {false, true}) {
    SCOPED_TRACE(stripe_causal);
    constexpr int kW = 16;
    constexpr int kH = 16;
    std::vector<SampleSpec> grid(static_cast<size_t>(kW * kH));

    // Upward cross-stripe test anchor at col 2, rows 4, 8, 12.
    for (int r : {4, 8, 12}) {
      grid[static_cast<size_t>(r * kW + 2)] = {
          .pass = SamplePass::kCleanup,
          .clean_mag = 1,
          .negative = false,
          .mrp_bit = 1,
      };
      if (!stripe_causal) {
        grid[static_cast<size_t>((r - 1) * kW + 2)] = {
            .pass = SamplePass::kSpp, .negative = (r & 4) != 0};
      }
    }

    // Cols 6..9 across rows 4..15: test horizontal propagation at n=4 (col
    // 7->8) and vertical propagation across i=0 / i=8 boundary (row 7 col 8 ->
    // row 8 col 7 nxt_mbr[-1], and row 7 col 7 -> row 8 col 8 nxt_mbr[1]), plus
    // long runs of mrp_bit = 1 and SPP sig/neg = 1 for stream stuffing.
    for (int y = 4; y < kH; ++y) {
      for (int x = 6; x < 14; ++x) {
        if ((y % 4) == 0 && x == 6) {
          grid[static_cast<size_t>(y * kW + x)] = {
              .pass = SamplePass::kCleanup,
              .clean_mag = 3,
              .negative = true,
              .mrp_bit = 1,
          };
        } else if ((y % 4) == 3 && (x == 7 || x == 8)) {
          grid[static_cast<size_t>(y * kW + x)] = {
              .pass = SamplePass::kCleanup,
              .clean_mag = 2,
              .negative = (x == 8),
              .mrp_bit = static_cast<uint8_t>((x + y) & 1),
          };
        } else {
          grid[static_cast<size_t>(y * kW + x)] = {.pass = SamplePass::kSpp,
                                                   .negative = true};
        }
      }
    }

    // Add extra cleanup samples with mrp_bit = 1 across rows 0..3 (cols 0..1)
    // and cols 14..15 (all rows) to guarantee MRP >0x8F/0x7F reverse stuffing.
    for (int y = 0; y < kH; ++y) {
      grid[static_cast<size_t>(y * kW + 14)] = {
          .pass = SamplePass::kCleanup,
          .clean_mag = 1,
          .negative = (y & 1) != 0,
          .mrp_bit = 1,
      };
      grid[static_cast<size_t>(y * kW + 15)] = {
          .pass = SamplePass::kCleanup,
          .clean_mag = 2,
          .negative = false,
          .mrp_bit = 1,
      };
    }

    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        kW, kH, /*mb=*/12, /*numbps=*/5, /*num_passes=*/3, stripe_causal, grid);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_TRUE(encoded->saw_spp_stuffing);
    EXPECT_TRUE(encoded->saw_mrp_stuffing);

    ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }

  // 2. 12x10 3-pass block with stripe_causal ON/OFF exercising terminating
  //    height > 6, st = 4, height - y = 6 > 4 (y = 4) and height - y = 2 (y =
  //    8), including i > 0 nxt_mbr[-1] propagation in terminating SPP.
  for (bool stripe_causal : {false, true}) {
    SCOPED_TRACE(stripe_causal);
    constexpr int kW = 12;
    constexpr int kH = 10;
    std::vector<SampleSpec> grid(static_cast<size_t>(kW * kH));
    // Row 7 col 8 cleanup -> propagates down to row 8 col 7 (nxt_mbr[-1]) in
    // terminating SPP at y = 4.
    grid[static_cast<size_t>(7 * kW + 8)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 2, .mrp_bit = 0};
    grid[static_cast<size_t>(8 * kW + 7)] = {.pass = SamplePass::kSpp,
                                             .negative = true};
    // Row 8 col 4 cleanup -> propagates up to row 7 col 4 when !stripe_causal.
    grid[static_cast<size_t>(8 * kW + 4)] = {
        .pass = SamplePass::kCleanup, .clean_mag = 1, .mrp_bit = 1};
    if (!stripe_causal) {
      grid[static_cast<size_t>(7 * kW + 4)] = {.pass = SamplePass::kSpp,
                                               .negative = false};
    }
    // Row 9 col 9 cleanup -> terminating MRP on (height & 3) == 2.
    grid[static_cast<size_t>(9 * kW + 9)] = {
        .pass = SamplePass::kCleanup,
        .clean_mag = 3,
        .negative = true,
        .mrp_bit = 1,
    };

    // Keep numbps = 3 (zero_bplanes = 3 != Mb = 6) so this test does not flip
    // ht_dec.c's one-shot process-global `only_cleanup_pass_is_decoded` flag.
    const std::optional<EncodedHtBlock> encoded = HtCodeblockEncoder::Encode(
        kW, kH, /*mb=*/10, /*numbps=*/4, /*num_passes=*/3, stripe_causal, grid);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_EQ(OPJ_TRUE, DecodeEncodedBlock(t1.get(), *encoded));
    EXPECT_THAT(ReadT1Samples(t1.get()), ElementsAreArray(encoded->expected));
  }
}

// TODO(schwehr): Cover remaining OOM allocation guards in
// opj_t1_allocate_buffers / opj_t1_ht_decode_cblk, unreachable bits > 32
// checks in mel_read / rev_read_mrp, and rows 2-3 of the terminating
// partial-height MRP loop (blocked earlier by the VLC codeblock bounds check)
// in ht_dec.c.

}  // namespace
}  // namespace openjpeg2
