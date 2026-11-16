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
// Tests internal OpenJPEG tier-2 packet encoder and decoder routines
// (src/lib/openjp2/t2.c), covering lifecycle, multi-layer/multi-resolution/
// multi-precinct/multi-component packetization, SOP/EPH/PPM/PPT/PLT markers,
// TERMALL/LAZY/HT code-block segment headers, THRESH_CALC vs FINAL_PASS,
// codestream info recording, strict vs permissive truncation, and packet
// skipping.
//
// Caveats for google3:
// - USE_JPWL and TODO_MSD are not defined, so those preprocessor branches in
//   t2.c are compiled out.
// - The layno > 0 precno bounds failure in opj_t2_encode_packet() leaks its
//   local opj_bio_t handle upstream and is intentionally not triggered under
//   LeakSanitizer.
// - White-box tests inspect opj_tcd_cblk_* / opj_cp_t / opj_tcp_t internals,
//   pin exact upstream error/warning substrings, and rely on kMaxStaticPasses
//   (100) matching tcd.c's opj_tcd_code_block_enc_allocate() fixed arrays.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/t2.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/t2.h

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_t2_t) > 0);

constexpr uint32_t kMaxStaticPasses = 100;

struct OpjT2Deleter {
  void operator()(opj_t2_t* t2) const { opj_t2_destroy(t2); }
};
using OpjT2Ptr = std::unique_ptr<opj_t2_t, OpjT2Deleter>;

uint32_t FloorLog2(uint32_t value) {
  return value > 1U ? static_cast<uint32_t>(std::bit_width(value) - 1U) : 0U;
}

// Mirrors static opj_t2_putcommacode / opj_t2_putnumpasses in t2.c; keep
// synced with upstream updates.

void WriteCommaCode(opj_bio_t* bio, int32_t value) {
  for (int32_t i = 0; i < value; ++i) {
    opj_bio_putbit(bio, 1);
  }
  opj_bio_putbit(bio, 0);
}

void WriteNumPasses(opj_bio_t* bio, uint32_t n) {
  if (n == 1U) {
    opj_bio_putbit(bio, 0);
  } else if (n == 2U) {
    opj_bio_write(bio, 2U, 2U);
  } else if (n <= 5U) {
    opj_bio_write(bio, 0xCU | (n - 3U), 4U);
  } else if (n <= 36U) {
    opj_bio_write(bio, 0x1E0U | (n - 6U), 9U);
  } else {
    opj_bio_write(bio, 0xFF80U | (n - 37U), 16U);
  }
}

struct DirectT2Config {
  uint32_t width = 16;
  uint32_t height = 16;
  uint32_t numcomps = 1;
  uint32_t prec = 8;
  uint32_t numlayers = 1;
  uint32_t num_layers_to_decode = 1;
  uint32_t numresolutions = 1;
  uint32_t cblkw = 4;
  uint32_t cblkh = 4;
  uint32_t prcw = 15;
  uint32_t prch = 15;
  uint32_t csty = 0;
  uint32_t cblksty = 0;
  OPJ_PROG_ORDER prg = OPJ_LRCP;
  uint32_t reduce = 0;
  OPJ_BOOL strict = OPJ_TRUE;
  uint16_t rsiz = OPJ_PROFILE_NONE;
  int32_t stepsize_expn = 8;
  uint32_t numgbits = 2;
};

struct DirectT2Context {
  OpjImagePtr image;
  opj_cp_t cp{};
  std::vector<opj_tcp_t> tcps;
  std::vector<std::vector<opj_tccp_t>> tccps;
  EventCapture events;
  opj_event_mgr_t mgr{};
  OpjTcdPtr tcd;
  OpjT2Ptr t2;

  [[nodiscard]] bool Init(const DirectT2Config& cfg, bool is_decoder) {
    t2.reset();
    tcd.reset();
    events.Clear();
    mgr = MakeEventManager(&events);

    const OPJ_COLOR_SPACE color_space =
        (cfg.numcomps >= 3U) ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
    image = CreateTestImage(cfg.width, cfg.height, cfg.numcomps, cfg.prec,
                            /*sgnd=*/0, color_space);
    if (!image) {
      return false;
    }

    cp = opj_cp_t{};
    cp.rsiz = cfg.rsiz;
    cp.tx0 = image->x0;
    cp.ty0 = image->y0;
    cp.tdx = image->x1 - image->x0;
    cp.tdy = image->y1 - image->y0;
    cp.tw = 1;
    cp.th = 1;
    cp.strict = cfg.strict;
    cp.m_is_decoder = is_decoder ? 1U : 0U;
    if (is_decoder) {
      cp.m_specific_param.m_dec.m_reduce = cfg.reduce;
      cp.m_specific_param.m_dec.m_layer = cfg.num_layers_to_decode;
    }

    tcps.assign(1, opj_tcp_t{});
    tccps.assign(1, std::vector<opj_tccp_t>(cfg.numcomps));

    opj_tcp_t& tcp = tcps[0];
    tcp.csty = cfg.csty;
    tcp.prg = cfg.prg;
    tcp.numlayers = cfg.numlayers;
    tcp.num_layers_to_decode = cfg.num_layers_to_decode;
    tcp.tccps = tccps[0].data();

    for (uint32_t c = 0; c < cfg.numcomps; ++c) {
      opj_tccp_t& tccp = tccps[0][c];
      tccp.csty = (cfg.csty & J2K_CP_CSTY_PRT) ? J2K_CCP_CSTY_PRT : 0U;
      tccp.numresolutions = cfg.numresolutions;
      tccp.cblkw = cfg.cblkw;
      tccp.cblkh = cfg.cblkh;
      tccp.cblksty = cfg.cblksty;
      tccp.qmfbid = 1;
      tccp.qntsty = J2K_CCP_QNTSTY_NOQNT;
      tccp.numgbits = cfg.numgbits;
      for (uint32_t r = 0; r < OPJ_J2K_MAXRLVLS; ++r) {
        tccp.prcw[r] = cfg.prcw;
        tccp.prch[r] = cfg.prch;
      }
      for (uint32_t b = 0; b < OPJ_J2K_MAXBANDS; ++b) {
        tccp.stepsizes[b].expn = cfg.stepsize_expn;
        tccp.stepsizes[b].mant = 0;
      }
    }
    cp.tcps = tcps.data();

    tcd = OpjTcdPtr(opj_tcd_create(is_decoder ? OPJ_TRUE : OPJ_FALSE));
    if (!tcd) {
      return false;
    }
    if (!opj_tcd_init(tcd.get(), image.get(), &cp, nullptr)) {
      return false;
    }
    tcd->tcd_tileno = 0;
    tcd->tcp = &tcps[0];
    if (is_decoder) {
      tcd->win_x0 = image->x0;
      tcd->win_y0 = image->y0;
      tcd->win_x1 = image->x1;
      tcd->win_y1 = image->y1;
      tcd->whole_tile_decoding = OPJ_TRUE;
      if (!opj_tcd_init_decode_tile(tcd.get(), 0, &mgr)) {
        return false;
      }
    } else {
      if (!opj_tcd_init_encode_tile(tcd.get(), 0, &mgr)) {
        return false;
      }
    }

    t2 = OpjT2Ptr(opj_t2_create(image.get(), &cp));
    return t2 != nullptr;
  }

  opj_tcd_tile_t* tile() const { return tcd->tcd_image->tiles; }

  opj_tcd_cblk_enc_t* enc_cblk(uint32_t comp, uint32_t res, uint32_t band,
                               uint32_t prec, uint32_t cblk) const {
    return &tile()
                ->comps[comp]
                .resolutions[res]
                .bands[band]
                .precincts[prec]
                .cblks.enc[cblk];
  }

  opj_tcd_cblk_dec_t* dec_cblk(uint32_t comp, uint32_t res, uint32_t band,
                               uint32_t prec, uint32_t cblk) const {
    return &tile()
                ->comps[comp]
                .resolutions[res]
                .bands[band]
                .precincts[prec]
                .cblks.dec[cblk];
  }
};

void PopulateEncCblkPasses(opj_tcd_cblk_enc_t* cblk, uint32_t numbps,
                           std::span<const uint32_t> layer_pass_counts,
                           std::span<const uint32_t> pass_lens,
                           std::span<const uint8_t> pass_terms,
                           uint8_t payload_seed, double disto_per_pass = 12.5) {
  ASSERT_NE(nullptr, cblk);
  ASSERT_EQ(pass_lens.size(), pass_terms.size());
  ASSERT_LE(layer_pass_counts.size(), kMaxStaticPasses);
  ASSERT_LE(pass_lens.size(), kMaxStaticPasses);

  uint32_t total_len = 0;
  for (uint32_t len : pass_lens) {
    total_len += len;
  }
  ASSERT_LE(total_len, cblk->data_size);
  for (uint32_t i = 0; i < total_len; ++i) {
    cblk->data[i] = static_cast<OPJ_BYTE>((payload_seed + i) & 0xFFU);
  }

  cblk->numbps = numbps;
  cblk->totalpasses = static_cast<OPJ_UINT32>(pass_lens.size());
  for (size_t p = 0; p < pass_lens.size(); ++p) {
    cblk->passes[p].len = pass_lens[p];
    cblk->passes[p].term = pass_terms[p] ? 1U : 0U;
  }

  uint32_t pass_cursor = 0;
  uint32_t byte_cursor = 0;
  for (size_t l = 0; l < layer_pass_counts.size(); ++l) {
    const uint32_t n = layer_pass_counts[l];
    ASSERT_LE(pass_cursor + n, pass_lens.size());
    uint32_t layer_len = 0;
    for (uint32_t i = 0; i < n; ++i) {
      layer_len += pass_lens[pass_cursor + i];
    }
    cblk->layers[l].numpasses = n;
    cblk->layers[l].len = layer_len;
    cblk->layers[l].data = cblk->data + byte_cursor;
    cblk->layers[l].disto = static_cast<double>(n) * disto_per_pass;
    pass_cursor += n;
    byte_cursor += layer_len;
  }
}

struct SingleCblkPacketSpec {
  bool prepend_sop = false;
  uint16_t sop_seq = 0;
  bool present = true;
  bool included = true;
  uint32_t zero_bplanes = 0;
  uint32_t numpasses = 1;
  int32_t increment = 0;
  bool ht_mode = false;
  std::vector<uint32_t> seg_lens;
  bool append_eph = false;
  std::vector<uint8_t> body;
};

std::vector<uint8_t> BuildSingleCblkPacket(const SingleCblkPacketSpec& spec) {
  std::vector<uint8_t> out;
  if (spec.prepend_sop) {
    out.push_back(0xFF);
    out.push_back(0x91);
    out.push_back(0x00);
    out.push_back(0x04);
    out.push_back(static_cast<uint8_t>((spec.sop_seq >> 8U) & 0xFFU));
    out.push_back(static_cast<uint8_t>(spec.sop_seq & 0xFFU));
  }

  std::vector<uint8_t> hdr_buf(256, 0);
  OpjBioPtr bio(opj_bio_create());
  EXPECT_NE(nullptr, bio);
  opj_bio_init_enc(bio.get(), hdr_buf.data(),
                   static_cast<OPJ_UINT32>(hdr_buf.size()));

  opj_bio_putbit(bio.get(), spec.present ? 1U : 0U);
  if (spec.present) {
    OpjTgtTreePtr incl(opj_tgt_create(1U, 1U, nullptr));
    EXPECT_NE(nullptr, incl);
    opj_tgt_setvalue(incl.get(), 0U, spec.included ? 0 : 1);
    opj_tgt_encode(bio.get(), incl.get(), 0U, 1);

    if (spec.included) {
      OpjTgtTreePtr imsb(opj_tgt_create(1U, 1U, nullptr));
      EXPECT_NE(nullptr, imsb);
      opj_tgt_setvalue(imsb.get(), 0U,
                       static_cast<OPJ_INT32>(spec.zero_bplanes));
      opj_tgt_encode(bio.get(), imsb.get(), 0U, 999);

      WriteNumPasses(bio.get(), spec.numpasses);
      WriteCommaCode(bio.get(), spec.increment);

      const uint32_t numlenbits =
          3U + static_cast<uint32_t>(std::max<int32_t>(0, spec.increment));
      if (spec.ht_mode) {
        const uint32_t len0 = spec.seg_lens.empty() ? 0U : spec.seg_lens[0];
        if (numlenbits <= 32U) {
          opj_bio_write(bio.get(), len0, numlenbits);
        } else {
          opj_bio_write(bio.get(), 0U, 8U);
        }
        if (spec.numpasses > 1U && numlenbits <= 32U) {
          const uint32_t rem = spec.numpasses - 1U;
          const uint32_t bits1 = numlenbits + FloorLog2(rem);
          const uint32_t len1 =
              (spec.seg_lens.size() > 1U) ? spec.seg_lens[1] : 0U;
          if (bits1 <= 32U) {
            opj_bio_write(bio.get(), len1, bits1);
          } else {
            opj_bio_write(bio.get(), 0U, 8U);
          }
        }
      } else {
        const uint32_t bits0 = numlenbits + FloorLog2(spec.numpasses);
        const uint32_t len0 = spec.seg_lens.empty() ? 0U : spec.seg_lens[0];
        if (bits0 <= 32U) {
          opj_bio_write(bio.get(), len0, bits0);
        } else {
          opj_bio_write(bio.get(), 0U, 8U);
        }
      }
    }
  }

  EXPECT_EQ(OPJ_TRUE, opj_bio_flush(bio.get()));
  const ptrdiff_t hdr_bytes = opj_bio_numbytes(bio.get());
  EXPECT_GT(hdr_bytes, 0);
  out.insert(out.end(), hdr_buf.begin(), hdr_buf.begin() + hdr_bytes);

  if (spec.append_eph) {
    out.push_back(0xFF);
    out.push_back(0x92);
  }
  out.insert(out.end(), spec.body.begin(), spec.body.end());
  return out;
}

std::vector<uint8_t> ExtractChunkBytes(const opj_tcd_cblk_dec_t& cblk) {
  std::vector<uint8_t> out;
  for (uint32_t i = 0; i < cblk.numchunks; ++i) {
    const uint8_t* ptr = cblk.chunks[i].data;
    out.insert(out.end(), ptr, ptr + cblk.chunks[i].len);
  }
  return out;
}

void ExpectImagesEqual(const opj_image_t* expected, const opj_image_t* actual) {
  ASSERT_NE(nullptr, expected);
  ExpectImagePixelsEqual(SnapshotImagePixels(expected), actual);
}

void WriteBe32(std::span<uint8_t> data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
  data[offset + 1U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  data[offset + 2U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  data[offset + 3U] = static_cast<uint8_t>(value & 0xFFU);
}

struct TilePartOffsets {
  size_t sot_pos = 0;
  size_t sod_pos = 0;
};

std::optional<TilePartOffsets> FindFirstTilePartMarkers(
    std::span<const uint8_t> codestream) {
  if (codestream.size() < 4U || ReadBe16(codestream, 0) != J2K_MS_SOC) {
    return std::nullopt;
  }
  TilePartOffsets offsets{};
  size_t pos = 2;
  while (pos + 4U <= codestream.size()) {
    const uint16_t marker = ReadBe16(codestream, pos);
    if (marker == J2K_MS_SOD) {
      offsets.sod_pos = pos;
      return offsets;
    }
    if (marker == J2K_MS_SOT) {
      offsets.sot_pos = pos;
    }
    const uint16_t seg_len = ReadBe16(codestream, pos + 2U);
    if (seg_len < 2U || pos + 2U + seg_len > codestream.size()) {
      return std::nullopt;
    }
    pos += 2U + seg_len;
  }
  return std::nullopt;
}

TEST(T2Test, CreateAndDestroyLifecycleAndNullDestroyIsSafe) {
  OpjImagePtr image = CreateTestImage(8, 8, 1);
  ASSERT_NE(nullptr, image);
  opj_cp_t cp{};

  OpjT2Ptr valid(opj_t2_create(image.get(), &cp));
  ASSERT_NE(nullptr, valid);
  EXPECT_EQ(image.get(), valid->image);
  EXPECT_EQ(&cp, valid->cp);
  valid.reset();

  OpjT2Ptr null_args(opj_t2_create(nullptr, nullptr));
  ASSERT_NE(nullptr, null_args);
  EXPECT_EQ(nullptr, null_args->image);
  EXPECT_EQ(nullptr, null_args->cp);
  null_args.reset();

  opj_t2_destroy(nullptr);
}

TEST(T2Test, DirectEncodeDecodeMultiLayerPassCountsAndCstrInfoRoundTrip) {
  // 1. Configure a 16x16, 2-component, 3-layer tile with 8x8 codeblocks (4
  // codeblocks in comp 0 res 0) and 40 passes on comp 1 to cover all 5
  // pass-count VLC ranges (1, 2, 3..5, 6..36, 37..164) plus comma-code length
  // increments.
  DirectT2Config cfg{
      .width = 16,
      .height = 16,
      .numcomps = 2,
      .prec = 12,
      .numlayers = 3,
      .num_layers_to_decode = 3,
      .numresolutions = 1,
      .cblkw = 3,
      .cblkh = 3,
      .prcw = 4,
      .prch = 4,
      .csty = J2K_CP_CSTY_SOP | J2K_CP_CSTY_EPH,
      .stepsize_expn = 12,
      .numgbits = 2,
  };
  DirectT2Context enc;
  ASSERT_TRUE(enc.Init(cfg, /*is_decoder=*/false));
  ASSERT_EQ(4U,
            enc.tile()->comps[0].resolutions[0].bands[0].precincts[0].cw *
                enc.tile()->comps[0].resolutions[0].bands[0].precincts[0].ch);

  // Comp 0, cblk 0: layer 0 has 1 pass (len 3), layer 1 has 2 passes (len 12,
  // triggering Lblock increment > 0), layer 2 has 0 passes (re-excluded bit 0).
  constexpr auto kC0B0Layers = std::to_array<uint32_t>({1, 2, 0});
  constexpr auto kC0B0Lens = std::to_array<uint32_t>({3, 5, 7});
  constexpr auto kC0B0Terms = std::to_array<uint8_t>({0, 0, 1});
  PopulateEncCblkPasses(enc.enc_cblk(0, 0, 0, 0, 0), /*numbps=*/10, kC0B0Layers,
                        kC0B0Lens, kC0B0Terms, /*payload_seed=*/0x10);

  // Comp 0, cblk 1: skipped in layer 0, 4 passes in layer 1, 10 passes in
  // layer 2.
  constexpr auto kC0B1Layers = std::to_array<uint32_t>({0, 4, 10});
  std::array<uint32_t, 14> c0b1_lens{};
  c0b1_lens.fill(1U);
  std::array<uint8_t, 14> c0b1_terms{};
  c0b1_terms[3] = 1;
  c0b1_terms[13] = 1;
  PopulateEncCblkPasses(enc.enc_cblk(0, 0, 0, 0, 1), /*numbps=*/9, kC0B1Layers,
                        c0b1_lens, c0b1_terms, /*payload_seed=*/0x40);

  // Comp 1, cblk 0: 40 passes in layer 0 (covers 37..164 VLC range).
  constexpr auto kC1B0Layers = std::to_array<uint32_t>({40, 0, 0});
  std::array<uint32_t, 40> c1b0_lens{};
  c1b0_lens.fill(1U);
  std::array<uint8_t, 40> c1b0_terms{};
  c1b0_terms[39] = 1;
  PopulateEncCblkPasses(enc.enc_cblk(1, 0, 0, 0, 0), /*numbps=*/11, kC1B0Layers,
                        c1b0_lens, c1b0_terms, /*payload_seed=*/0x80);

  // 2. Encode packets with cstr_info and PLT marker_info enabled.
  std::vector<opj_packet_info_t> pack_infos(6);
  std::vector<opj_tile_info_t> tile_infos(1);
  tile_infos[0].end_header = 20;
  tile_infos[0].packet = pack_infos.data();
  opj_codestream_info_t cstr_info{};
  cstr_info.index_write = 1;
  cstr_info.tile = tile_infos.data();

  OpjMarkerInfoPtr marker_info(opj_tcd_marker_info_create(OPJ_TRUE));
  ASSERT_NE(nullptr, marker_info);

  std::vector<uint8_t> packet_buf(2048, 0xAA);
  OPJ_UINT32 bytes_written = 0;
  ASSERT_EQ(OPJ_TRUE, opj_t2_encode_packets(
                          enc.t2.get(), /*tileno=*/0, enc.tile(),
                          /*maxlayers=*/3, packet_buf.data(), &bytes_written,
                          static_cast<OPJ_UINT32>(packet_buf.size()),
                          &cstr_info, marker_info.get(), /*tpnum=*/0,
                          /*tppos=*/0, /*pino=*/0, FINAL_PASS, &enc.mgr));
  ASSERT_GT(bytes_written, 0U);
  EXPECT_EQ(6, cstr_info.packno);
  EXPECT_EQ(6U, enc.tile()->packno);
  EXPECT_EQ(6U, marker_info->packet_count);
  EXPECT_EQ(21, pack_infos[0].start_pos);
  EXPECT_GT(pack_infos[0].end_ph_pos, pack_infos[0].start_pos);
  EXPECT_EQ(pack_infos[0].end_pos + 1, pack_infos[1].start_pos);
  EXPECT_EQ(pack_infos[0].start_pos + static_cast<OPJ_OFF_T>(bytes_written) - 1,
            pack_infos[5].end_pos);
  EXPECT_GT(cstr_info.D_max, 0.0);

  uint32_t plt_sum = 0;
  for (uint32_t i = 0; i < marker_info->packet_count; ++i) {
    plt_sum += marker_info->p_packet_size[i];
  }
  EXPECT_EQ(bytes_written, plt_sum);

  // 3. Also test p_maxlayers truncation (encodes only 1 of 3 layers) with
  // m_tp_on == 0 (skipping layno >= maxlayers) and with m_tp_on == 1 using a
  // pre-populated start_pos branch in cstr_info.
  {
    DirectT2Context enc_maxlay;
    ASSERT_TRUE(enc_maxlay.Init(cfg, /*is_decoder=*/false));
    PopulateEncCblkPasses(enc_maxlay.enc_cblk(0, 0, 0, 0, 0), 10, kC0B0Layers,
                          kC0B0Lens, kC0B0Terms, 0x10);
    std::vector<uint8_t> maxlay_buf(1024, 0);
    OPJ_UINT32 maxlay_written = 0;
    ASSERT_EQ(OPJ_TRUE, opj_t2_encode_packets(
                            enc_maxlay.t2.get(), 0, enc_maxlay.tile(),
                            /*maxlayers=*/1, maxlay_buf.data(), &maxlay_written,
                            static_cast<OPJ_UINT32>(maxlay_buf.size()), nullptr,
                            nullptr, 0, 0, 0, FINAL_PASS, &enc_maxlay.mgr));
    EXPECT_EQ(2U, enc_maxlay.tile()->packno);
  }

  DirectT2Context enc_tp;
  ASSERT_TRUE(enc_tp.Init(cfg, /*is_decoder=*/false));
  enc_tp.cp.m_specific_param.m_enc.m_tp_on = 1;
  enc_tp.cp.m_specific_param.m_enc.m_tp_flag = 'L';
  PopulateEncCblkPasses(enc_tp.enc_cblk(0, 0, 0, 0, 0), 10, kC0B0Layers,
                        kC0B0Lens, kC0B0Terms, 0x10);
  std::vector<opj_packet_info_t> tp_pack_infos(6);
  tp_pack_infos[1].start_pos = 777;
  std::vector<opj_tile_info_t> tp_tile_infos(1);
  tp_tile_infos[0].end_header = 10;
  tp_tile_infos[0].packet = tp_pack_infos.data();
  opj_codestream_info_t tp_cstr_info{};
  tp_cstr_info.index_write = 1;
  tp_cstr_info.tile = tp_tile_infos.data();
  std::vector<uint8_t> tp_buf(1024, 0);
  OPJ_UINT32 tp_written = 0;
  ASSERT_EQ(OPJ_TRUE,
            opj_t2_encode_packets(
                enc_tp.t2.get(), 0, enc_tp.tile(), /*maxlayers=*/1,
                tp_buf.data(), &tp_written,
                static_cast<OPJ_UINT32>(tp_buf.size()), &tp_cstr_info, nullptr,
                /*tpnum=*/0, /*tppos=*/0, /*pino=*/0, FINAL_PASS, &enc_tp.mgr));
  EXPECT_EQ(2, tp_cstr_info.packno);
  EXPECT_EQ(777, tp_pack_infos[1].start_pos);

  // 4. Decode all 3 layers and verify exact codeblock chunk payloads.
  DirectT2Context dec;
  ASSERT_TRUE(dec.Init(cfg, /*is_decoder=*/true));
  OPJ_UINT32 bytes_read = 0;
  ASSERT_EQ(OPJ_TRUE,
            opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                  packet_buf.data(), &bytes_read, bytes_written,
                                  nullptr, &dec.mgr));
  EXPECT_EQ(bytes_written, bytes_read);
  EXPECT_TRUE(dec.events.errors.empty());
  EXPECT_TRUE(dec.events.warnings.empty());

  const opj_tcd_cblk_dec_t* c0b0 = dec.dec_cblk(0, 0, 0, 0, 0);
  EXPECT_EQ(1U, c0b0->numsegs);
  EXPECT_EQ(3U, c0b0->segs[0].numpasses);
  EXPECT_EQ(15U, c0b0->segs[0].len);
  const std::vector<uint8_t> c0b0_bytes = ExtractChunkBytes(*c0b0);
  ASSERT_EQ(15U, c0b0_bytes.size());
  for (uint32_t i = 0; i < 15U; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((0x10U + i) & 0xFFU), c0b0_bytes[i]);
  }

  const opj_tcd_cblk_dec_t* c0b1 = dec.dec_cblk(0, 0, 0, 0, 1);
  EXPECT_EQ(1U, c0b1->numsegs);
  EXPECT_EQ(14U, c0b1->segs[0].numpasses);
  EXPECT_EQ(14U, c0b1->segs[0].len);
  const std::vector<uint8_t> c0b1_bytes = ExtractChunkBytes(*c0b1);
  ASSERT_EQ(14U, c0b1_bytes.size());
  for (uint32_t i = 0; i < 14U; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((0x40U + i) & 0xFFU), c0b1_bytes[i]);
  }

  const opj_tcd_cblk_dec_t* c1b0 = dec.dec_cblk(1, 0, 0, 0, 0);
  EXPECT_EQ(1U, c1b0->numsegs);
  EXPECT_EQ(40U, c1b0->segs[0].numpasses);
  EXPECT_EQ(40U, c1b0->segs[0].len);
  const std::vector<uint8_t> c1b0_bytes = ExtractChunkBytes(*c1b0);
  ASSERT_EQ(40U, c1b0_bytes.size());
  for (uint32_t i = 0; i < 40U; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((0x80U + i) & 0xFFU), c1b0_bytes[i]);
  }
}

TEST(T2Test, DirectEncodeThreshCalcCinema4kMaxCompSizeAndProgUnknown) {
  // 1. Setup a 3-component, 2-resolution CINEMA_4K tile with 2 POC entries so
  // THRESH_CALC runs the pocno == 2 and l_max_comp == numcomps loops.
  DirectT2Config cfg{
      .width = 16,
      .height = 16,
      .numcomps = 3,
      .prec = 12,
      .numlayers = 2,
      .num_layers_to_decode = 2,
      .numresolutions = 2,
      .cblkw = 4,
      .cblkh = 4,
      .prg = OPJ_CPRL,
      .rsiz = OPJ_PROFILE_CINEMA_4K,
      .stepsize_expn = 12,
  };
  DirectT2Context ctx;
  ASSERT_TRUE(ctx.Init(cfg, /*is_decoder=*/false));
  ctx.cp.m_specific_param.m_enc.m_tp_on = 1;
  ctx.cp.m_specific_param.m_enc.m_tp_flag = 'C';
  ctx.cp.m_specific_param.m_enc.m_max_comp_size = 1024;
  ctx.tcps[0].POC = 1;
  ctx.tcps[0].numpocs = 1;
  ctx.tcps[0].pocs[0] = opj_poc_t{
      .resno0 = 0,
      .compno0 = 0,
      .layno1 = 2,
      .resno1 = 1,
      .compno1 = 3,
      .prg1 = OPJ_CPRL,
      .tile = 1,
  };
  ctx.tcps[0].pocs[1] = opj_poc_t{
      .resno0 = 1,
      .compno0 = 0,
      .layno1 = 2,
      .resno1 = 2,
      .compno1 = 3,
      .prg1 = OPJ_CPRL,
      .tile = 1,
  };

  constexpr auto kLayers = std::to_array<uint32_t>({1, 0});
  constexpr auto kLens = std::to_array<uint32_t>({8});
  constexpr auto kTerms = std::to_array<uint8_t>({1});
  PopulateEncCblkPasses(ctx.enc_cblk(0, 0, 0, 0, 0), 10, kLayers, kLens, kTerms,
                        0x21);

  std::vector<uint8_t> out(2048, 0xCC);
  OPJ_UINT32 written = 0;
  ASSERT_EQ(OPJ_TRUE,
            opj_t2_encode_packets(ctx.t2.get(), 0, ctx.tile(), /*maxlayers=*/1,
                                  out.data(), &written,
                                  static_cast<OPJ_UINT32>(out.size()), nullptr,
                                  nullptr, 0, 0, 0, THRESH_CALC, &ctx.mgr));
  EXPECT_GT(written, 0U);
  // THRESH_CALC skips copying the packet body bytes while advancing the length.
  EXPECT_EQ(0U, ctx.tile()->packno);

  // 2. Exceeding m_max_comp_size causes THRESH_CALC to return OPJ_FALSE.
  ctx.cp.m_specific_param.m_enc.m_max_comp_size = 2;
  EXPECT_EQ(OPJ_FALSE,
            opj_t2_encode_packets(ctx.t2.get(), 0, ctx.tile(), /*maxlayers=*/2,
                                  out.data(), &written,
                                  static_cast<OPJ_UINT32>(out.size()), nullptr,
                                  nullptr, 0, 0, 0, THRESH_CALC, &ctx.mgr));

  // 3. OPJ_PROG_UNKNOWN fails cleanly in both THRESH_CALC and FINAL_PASS.
  DirectT2Config bad_cfg = cfg;
  bad_cfg.rsiz = OPJ_PROFILE_NONE;
  bad_cfg.numresolutions = 1;
  bad_cfg.numcomps = 1;
  bad_cfg.prg = OPJ_PROG_UNKNOWN;
  DirectT2Context bad_ctx;
  ASSERT_TRUE(bad_ctx.Init(bad_cfg, /*is_decoder=*/false));
  EXPECT_EQ(OPJ_FALSE,
            opj_t2_encode_packets(bad_ctx.t2.get(), 0, bad_ctx.tile(), 1,
                                  out.data(), &written,
                                  static_cast<OPJ_UINT32>(out.size()), nullptr,
                                  nullptr, 0, 0, 0, THRESH_CALC, &bad_ctx.mgr));
  EXPECT_EQ(OPJ_FALSE,
            opj_t2_encode_packets(bad_ctx.t2.get(), 0, bad_ctx.tile(), 1,
                                  out.data(), &written,
                                  static_cast<OPJ_UINT32>(out.size()), nullptr,
                                  nullptr, 0, 0, 0, FINAL_PASS, &bad_ctx.mgr));
}

TEST(T2Test, DirectEncodeBufferExhaustionAndInvalidPrecinct) {
  DirectT2Config cfg{
      .width = 16,
      .height = 16,
      .numcomps = 1,
      .numlayers = 1,
      .numresolutions = 1,
      .csty = J2K_CP_CSTY_SOP | J2K_CP_CSTY_EPH,
  };
  DirectT2Context ctx;
  ASSERT_TRUE(ctx.Init(cfg, /*is_decoder=*/false));

  constexpr auto kLayers = std::to_array<uint32_t>({1});
  constexpr auto kLens = std::to_array<uint32_t>({10});
  constexpr auto kTerms = std::to_array<uint8_t>({1});
  PopulateEncCblkPasses(ctx.enc_cblk(0, 0, 0, 0, 0), 8, kLayers, kLens, kTerms,
                        0x33);

  std::vector<opj_packet_info_t> pack_infos(1);
  std::vector<opj_tile_info_t> tile_infos(1);
  tile_infos[0].end_header = -1;
  tile_infos[0].packet = pack_infos.data();
  opj_codestream_info_t cstr_info{};
  cstr_info.index_write = 1;
  cstr_info.tile = tile_infos.data();

  std::vector<uint8_t> full_buf(256, 0);
  OPJ_UINT32 full_written = 0;
  ASSERT_EQ(OPJ_TRUE,
            opj_t2_encode_packets(
                ctx.t2.get(), 0, ctx.tile(), 1, full_buf.data(), &full_written,
                static_cast<OPJ_UINT32>(full_buf.size()), &cstr_info, nullptr,
                0, 0, 0, FINAL_PASS, &ctx.mgr));
  // end_ph_pos includes the 6-byte SOP, BIO header, and 2-byte EPH.
  const uint32_t hdr_with_sop_eph =
      static_cast<uint32_t>(pack_infos[0].end_ph_pos + 1);
  ASSERT_GT(hdr_with_sop_eph, 8U);
  ASSERT_EQ(hdr_with_sop_eph + 10U, full_written);

  const std::array<uint32_t, 3> fail_limits = {
      5U,                     // < 6 bytes for SOP
      hdr_with_sop_eph - 1U,  // < 2 bytes remaining for EPH
      hdr_with_sop_eph + 5U,  // < layer->len (10) remaining for body
  };
  std::vector<uint8_t> scratch(256, 0);
  OPJ_UINT32 written = 0;

  for (const uint32_t limit : fail_limits) {
    // THRESH_CALC fails silently.
    ctx.events.Clear();
    EXPECT_EQ(OPJ_FALSE,
              opj_t2_encode_packets(ctx.t2.get(), 0, ctx.tile(), 1,
                                    scratch.data(), &written, limit, nullptr,
                                    nullptr, 0, 0, 0, THRESH_CALC, &ctx.mgr))
        << "limit=" << limit;
    EXPECT_TRUE(ctx.events.errors.empty());

    // FINAL_PASS emits EVT_ERROR.
    ctx.events.Clear();
    EXPECT_EQ(OPJ_FALSE,
              opj_t2_encode_packets(ctx.t2.get(), 0, ctx.tile(), 1,
                                    scratch.data(), &written, limit, nullptr,
                                    nullptr, 0, 0, 0, FINAL_PASS, &ctx.mgr))
        << "limit=" << limit;
    EXPECT_THAT(ctx.events.errors, Not(IsEmpty())) << "limit=" << limit;
  }

  // BIO flush failure when no SOP is enabled and buffer cannot hold the header.
  ctx.tcps[0].csty = 0;
  ctx.events.Clear();
  EXPECT_EQ(OPJ_FALSE,
            opj_t2_encode_packets(ctx.t2.get(), 0, ctx.tile(), 1,
                                  scratch.data(), &written, 0, nullptr, nullptr,
                                  0, 0, 0, FINAL_PASS, &ctx.mgr));

  // Invalid precinct index at layno == 0 (before opj_bio_create).
  opj_tcd_resolution_t& res0 = ctx.tile()->comps[0].resolutions[0];
  const OPJ_UINT32 saved_pw = res0.pw;
  absl::Cleanup restore_pw([&]() { res0.pw = saved_pw; });
  res0.pw = 0;
  ctx.events.Clear();
  EXPECT_EQ(OPJ_FALSE, opj_t2_encode_packets(
                           ctx.t2.get(), 0, ctx.tile(), 1, scratch.data(),
                           &written, static_cast<OPJ_UINT32>(scratch.size()),
                           nullptr, nullptr, 0, 0, 0, FINAL_PASS, &ctx.mgr));
  EXPECT_THAT(ctx.events.errors, Not(IsEmpty()));
}

TEST(T2Test, DirectEncodeDecodeTermallAndLazyMultiSegmentRealloc) {
  // 1. TERMALL with 12 passes in layer 0 + 2 passes in layer 1 forces
  // opj_t2_init_seg() to realloc segs (>10) and read_packet_data() to grow
  // chunks.
  DirectT2Config termall_cfg{
      .width = 16,
      .height = 16,
      .numcomps = 1,
      .numlayers = 2,
      .num_layers_to_decode = 2,
      .numresolutions = 1,
      .cblksty = J2K_CCP_CBLKSTY_TERMALL,
  };
  DirectT2Context termall_enc;
  ASSERT_TRUE(termall_enc.Init(termall_cfg, /*is_decoder=*/false));

  constexpr auto kTermallLayers = std::to_array<uint32_t>({12, 2});
  std::array<uint32_t, 14> termall_lens{};
  termall_lens.fill(2U);
  std::array<uint8_t, 14> termall_terms{};
  termall_terms.fill(1U);
  PopulateEncCblkPasses(termall_enc.enc_cblk(0, 0, 0, 0, 0), 8, kTermallLayers,
                        termall_lens, termall_terms, /*payload_seed=*/0x50);

  std::vector<uint8_t> termall_buf(1024, 0);
  OPJ_UINT32 termall_written = 0;
  ASSERT_EQ(OPJ_TRUE, opj_t2_encode_packets(
                          termall_enc.t2.get(), 0, termall_enc.tile(), 2,
                          termall_buf.data(), &termall_written,
                          static_cast<OPJ_UINT32>(termall_buf.size()), nullptr,
                          nullptr, 0, 0, 0, FINAL_PASS, &termall_enc.mgr));

  DirectT2Context termall_dec;
  ASSERT_TRUE(termall_dec.Init(termall_cfg, /*is_decoder=*/true));
  OPJ_UINT32 termall_read = 0;
  ASSERT_EQ(OPJ_TRUE, opj_t2_decode_packets(
                          termall_dec.tcd.get(), termall_dec.t2.get(), 0,
                          termall_dec.tile(), termall_buf.data(), &termall_read,
                          termall_written, nullptr, &termall_dec.mgr));
  EXPECT_EQ(termall_written, termall_read);
  const opj_tcd_cblk_dec_t* td_cblk = termall_dec.dec_cblk(0, 0, 0, 0, 0);
  EXPECT_EQ(14U, td_cblk->numsegs);
  EXPECT_GE(td_cblk->m_current_max_segs, 2U * OPJ_J2K_DEFAULT_NB_SEGS);
  EXPECT_EQ(14U, td_cblk->numchunks);
  const std::vector<uint8_t> termall_bytes = ExtractChunkBytes(*td_cblk);
  ASSERT_EQ(28U, termall_bytes.size());
  for (uint32_t i = 0; i < 28U; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((0x50U + i) & 0xFFU), termall_bytes[i]);
  }

  // 2. LAZY mode with 14 passes in layer 0 (maxpasses: 10 -> 2 -> 1 -> 2),
  // 1 pass in layer 1 (appends into seg 3 since numpasses 1 < maxpasses 2),
  // and 1 pass in layer 2 (starts seg 4 with maxpasses 1).
  DirectT2Config lazy_cfg = termall_cfg;
  lazy_cfg.numlayers = 3;
  lazy_cfg.num_layers_to_decode = 3;
  lazy_cfg.cblksty = J2K_CCP_CBLKSTY_LAZY;
  DirectT2Context lazy_enc;
  ASSERT_TRUE(lazy_enc.Init(lazy_cfg, /*is_decoder=*/false));

  constexpr auto kLazyLayers = std::to_array<uint32_t>({14, 1, 1});
  std::array<uint32_t, 16> lazy_lens{};
  lazy_lens.fill(2U);
  constexpr auto kLazyTerms =
      std::to_array<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1});
  PopulateEncCblkPasses(lazy_enc.enc_cblk(0, 0, 0, 0, 0), 8, kLazyLayers,
                        lazy_lens, kLazyTerms, /*payload_seed=*/0x70);

  std::vector<uint8_t> lazy_buf(1024, 0);
  OPJ_UINT32 lazy_written = 0;
  ASSERT_EQ(OPJ_TRUE,
            opj_t2_encode_packets(
                lazy_enc.t2.get(), 0, lazy_enc.tile(), 3, lazy_buf.data(),
                &lazy_written, static_cast<OPJ_UINT32>(lazy_buf.size()),
                nullptr, nullptr, 0, 0, 0, FINAL_PASS, &lazy_enc.mgr));

  DirectT2Context lazy_dec;
  ASSERT_TRUE(lazy_dec.Init(lazy_cfg, /*is_decoder=*/true));
  OPJ_UINT32 lazy_read = 0;
  ASSERT_EQ(OPJ_TRUE,
            opj_t2_decode_packets(lazy_dec.tcd.get(), lazy_dec.t2.get(), 0,
                                  lazy_dec.tile(), lazy_buf.data(), &lazy_read,
                                  lazy_written, nullptr, &lazy_dec.mgr));
  EXPECT_EQ(lazy_written, lazy_read);
  const opj_tcd_cblk_dec_t* ld_cblk = lazy_dec.dec_cblk(0, 0, 0, 0, 0);
  ASSERT_EQ(5U, ld_cblk->numsegs);
  EXPECT_EQ(10U, ld_cblk->segs[0].numpasses);
  EXPECT_EQ(2U, ld_cblk->segs[1].numpasses);
  EXPECT_EQ(1U, ld_cblk->segs[2].numpasses);
  EXPECT_EQ(2U, ld_cblk->segs[3].numpasses);
  EXPECT_EQ(1U, ld_cblk->segs[4].numpasses);
  const std::vector<uint8_t> lazy_bytes = ExtractChunkBytes(*ld_cblk);
  ASSERT_EQ(32U, lazy_bytes.size());
  for (uint32_t i = 0; i < 32U; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((0x70U + i) & 0xFFU), lazy_bytes[i]);
  }
}

TEST(T2Test, DirectDecodeHtPacketHeadersAndValidationEdges) {
  // 1. Single-pass HT packet (CUP only).
  DirectT2Config ht_cfg{
      .width = 8,
      .height = 8,
      .numcomps = 1,
      .numlayers = 1,
      .num_layers_to_decode = 1,
      .numresolutions = 1,
      .cblksty = J2K_CCP_CBLKSTY_HT,
  };
  {
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(ht_cfg, /*is_decoder=*/true));
    std::vector<uint8_t> pkt = BuildSingleCblkPacket({
        .numpasses = 1,
        .increment = 0,
        .ht_mode = true,
        .seg_lens = {4},
        .body = {0xA1, 0xA2, 0xA3, 0xA4},
    });
    OPJ_UINT32 bytes_read = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    pkt.data(), &bytes_read,
                                    static_cast<OPJ_UINT32>(pkt.size()),
                                    nullptr, &dec.mgr));
    EXPECT_EQ(pkt.size(), bytes_read);
    const opj_tcd_cblk_dec_t* cblk = dec.dec_cblk(0, 0, 0, 0, 0);
    ASSERT_EQ(1U, cblk->numsegs);
    EXPECT_EQ(1U, cblk->segs[0].numpasses);
    EXPECT_EQ(4U, cblk->segs[0].len);
    EXPECT_THAT(ExtractChunkBytes(*cblk), ElementsAre(0xA1, 0xA2, 0xA3, 0xA4));
  }

  // 2. Three-pass HT packet (CUP in seg 0 + SPP/MRP in seg 1); HTMIXED is set
  // only to confirm t2.c ignores it and splits segments on J2K_CCP_CBLKSTY_HT.
  {
    DirectT2Config mixed_cfg = ht_cfg;
    mixed_cfg.cblksty = J2K_CCP_CBLKSTY_HT | J2K_CCP_CBLKSTY_HTMIXED;
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(mixed_cfg, /*is_decoder=*/true));
    std::vector<uint8_t> pkt = BuildSingleCblkPacket({
        .zero_bplanes = 2,
        .numpasses = 3,
        .increment = 1,
        .ht_mode = true,
        .seg_lens = {5, 6},
        .body = {1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16},
    });
    OPJ_UINT32 bytes_read = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    pkt.data(), &bytes_read,
                                    static_cast<OPJ_UINT32>(pkt.size()),
                                    nullptr, &dec.mgr));
    EXPECT_EQ(pkt.size(), bytes_read);
    const opj_tcd_cblk_dec_t* cblk = dec.dec_cblk(0, 0, 0, 0, 0);
    ASSERT_EQ(2U, cblk->numsegs);
    EXPECT_EQ(1U, cblk->segs[0].numpasses);
    EXPECT_EQ(5U, cblk->segs[0].len);
    EXPECT_EQ(2U, cblk->segs[1].numpasses);
    EXPECT_EQ(6U, cblk->segs[1].len);
    EXPECT_THAT(ExtractChunkBytes(*cblk),
                ElementsAre(1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16));
  }

  // 3. bit_number > 32 triggers an error in both HT and non-HT modes.
  for (const bool is_ht : {true, false}) {
    DirectT2Config err_cfg = ht_cfg;
    err_cfg.cblksty = is_ht ? J2K_CCP_CBLKSTY_HT : 0U;
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(err_cfg, /*is_decoder=*/true));
    std::vector<uint8_t> pkt = BuildSingleCblkPacket({
        .numpasses = 1,
        .increment = 30,  // numlenbits = 3 + 30 = 33 > 32
        .ht_mode = is_ht,
        .seg_lens = {0},
    });
    OPJ_UINT32 bytes_read = 0;
    EXPECT_EQ(OPJ_FALSE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    pkt.data(), &bytes_read,
                                    static_cast<OPJ_UINT32>(pkt.size()),
                                    nullptr, &dec.mgr));
    EXPECT_THAT(dec.events.errors,
                ElementsAre(HasSubstr("Invalid bit number 33")));
  }

  // 4. Zero-bitplane count exceeding band->numbps + 1 avoids unsigned underflow
  // (OpenJPEG PR #1488).
  {
    DirectT2Config zbp_cfg = ht_cfg;
    zbp_cfg.cblksty = 0;
    zbp_cfg.stepsize_expn = 1;
    zbp_cfg.numgbits = 1;  // band->numbps = 1
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(zbp_cfg, /*is_decoder=*/true));
    std::vector<uint8_t> pkt = BuildSingleCblkPacket({
        .zero_bplanes = 4,  // 1 + 1 < 4
        .numpasses = 1,
        .increment = 0,
        .ht_mode = false,
        .seg_lens = {2},
        .body = {0xCA, 0xFE},
    });
    OPJ_UINT32 bytes_read = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    pkt.data(), &bytes_read,
                                    static_cast<OPJ_UINT32>(pkt.size()),
                                    nullptr, &dec.mgr));
    const opj_tcd_cblk_dec_t* cblk = dec.dec_cblk(0, 0, 0, 0, 0);
    EXPECT_EQ(1U, cblk->Mb);
    // opj_tgt_decode() exits at i = zero_bplanes + 1 = 5, so 1 + 1 - 5 = -3.
    EXPECT_EQ(static_cast<OPJ_UINT32>(-3), cblk->numbps);
  }
}

TEST(T2Test, DirectDecodeSopEphEmptyPacketsAndHeaderErrors) {
  // 1. SOP warnings when missing or buffer < 6 bytes, plus valid recovery.
  {
    DirectT2Config sop_cfg{
        .width = 8,
        .height = 8,
        .numcomps = 1,
        .numlayers = 1,
        .num_layers_to_decode = 1,
        .numresolutions = 1,
        .csty = J2K_CP_CSTY_SOP,
    };

    // Valid SOP + EPH packet decodes cleanly with zero warnings.
    {
      DirectT2Config sop_eph_cfg = sop_cfg;
      sop_eph_cfg.csty = J2K_CP_CSTY_SOP | J2K_CP_CSTY_EPH;
      DirectT2Context valid_dec;
      ASSERT_TRUE(valid_dec.Init(sop_eph_cfg, /*is_decoder=*/true));
      std::vector<uint8_t> pkt = BuildSingleCblkPacket({
          .prepend_sop = true,
          .sop_seq = 3,
          .numpasses = 1,
          .seg_lens = {2},
          .append_eph = true,
          .body = {0x5A, 0xA5},
      });
      OPJ_UINT32 read_bytes = 0;
      ASSERT_EQ(OPJ_TRUE, opj_t2_decode_packets(
                              valid_dec.tcd.get(), valid_dec.t2.get(), 0,
                              valid_dec.tile(), pkt.data(), &read_bytes,
                              static_cast<OPJ_UINT32>(pkt.size()), nullptr,
                              &valid_dec.mgr));
      EXPECT_EQ(pkt.size(), read_bytes);
      EXPECT_TRUE(valid_dec.events.warnings.empty());
      EXPECT_THAT(ExtractChunkBytes(*valid_dec.dec_cblk(0, 0, 0, 0, 0)),
                  ElementsAre(0x5A, 0xA5));
    }

    // Buffer < 6 bytes emits "Not enough space for expected SOP marker" and
    // decodes empty packet.
    DirectT2Context short_dec;
    ASSERT_TRUE(short_dec.Init(sop_cfg, /*is_decoder=*/true));
    std::vector<uint8_t> short_empty = {0x00};
    OPJ_UINT32 read_bytes = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(short_dec.tcd.get(), short_dec.t2.get(), 0,
                                    short_dec.tile(), short_empty.data(),
                                    &read_bytes, 1U, nullptr, &short_dec.mgr));
    EXPECT_EQ(1U, read_bytes);
    EXPECT_THAT(short_dec.events.warnings,
                ElementsAre(HasSubstr("Not enough space for expected SOP")));

    // Buffer >= 6 without 0xFF 0x91 (testing both first-byte != 0xFF and
    // second-byte != 0x91) emits "Expected SOP marker" and still decodes.
    for (const uint8_t b0 : {uint8_t{0x00}, uint8_t{0xFF}}) {
      DirectT2Context warn_dec;
      ASSERT_TRUE(warn_dec.Init(sop_cfg, /*is_decoder=*/true));
      std::vector<uint8_t> pkt = BuildSingleCblkPacket({
          .numpasses = 1,
          .seg_lens = {2},
          .body = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
      });
      if (b0 == 0xFFU) {
        // numpasses=37 packs 8 leading ones (0xFF) followed by a BIO-stuffed
        // 0x78 byte (!= 0x91), exercising the second branch of the SOP check.
        pkt = BuildSingleCblkPacket({
            .numpasses = 37,
            .seg_lens = {1},
            .body = {0x99, 0x00, 0x00},
        });
        ASSERT_EQ(0xFFU, pkt[0]);
        ASSERT_NE(0x91U, pkt[1]);
      }
      read_bytes = 0;
      ASSERT_EQ(OPJ_TRUE,
                opj_t2_decode_packets(warn_dec.tcd.get(), warn_dec.t2.get(), 0,
                                      warn_dec.tile(), pkt.data(), &read_bytes,
                                      static_cast<OPJ_UINT32>(pkt.size()),
                                      nullptr, &warn_dec.mgr));
      EXPECT_THAT(warn_dec.events.warnings,
                  ElementsAre(HasSubstr("Expected SOP marker")));
    }
  }

  // 2. EPH markers on empty and non-empty packets (valid, < 2 bytes, wrong).
  {
    DirectT2Config eph_cfg{
        .width = 8,
        .height = 8,
        .numcomps = 1,
        .numlayers = 1,
        .num_layers_to_decode = 1,
        .numresolutions = 1,
        .csty = J2K_CP_CSTY_EPH,
    };

    // Empty packet with valid EPH.
    {
      DirectT2Context dec;
      ASSERT_TRUE(dec.Init(eph_cfg, /*is_decoder=*/true));
      std::vector<uint8_t> pkt = {0x00, 0xFF, 0x92};
      OPJ_UINT32 read_bytes = 0;
      ASSERT_EQ(OPJ_TRUE,
                opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0,
                                      dec.tile(), pkt.data(), &read_bytes,
                                      static_cast<OPJ_UINT32>(pkt.size()),
                                      nullptr, &dec.mgr));
      EXPECT_EQ(3U, read_bytes);
      EXPECT_EQ(0U, dec.dec_cblk(0, 0, 0, 0, 0)->numsegs);
    }

    // Empty packet missing EPH (< 2 bytes remaining) or corrupt EPH bytes.
    const std::vector<std::vector<uint8_t>> bad_empty_eph = {
        {0x00, 0xFF},        // only 1 byte left for EPH
        {0x00, 0x00, 0x92},  // first byte != 0xFF
        {0x00, 0xFF, 0x00},  // second byte != 0x92
    };
    for (std::vector<uint8_t> pkt : bad_empty_eph) {
      DirectT2Context dec;
      ASSERT_TRUE(dec.Init(eph_cfg, /*is_decoder=*/true));
      OPJ_UINT32 read_bytes = 0;
      EXPECT_EQ(OPJ_FALSE,
                opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0,
                                      dec.tile(), pkt.data(), &read_bytes,
                                      static_cast<OPJ_UINT32>(pkt.size()),
                                      nullptr, &dec.mgr));
      EXPECT_THAT(dec.events.errors, Not(IsEmpty()));
    }

    // Non-empty packet with missing/corrupt EPH.
    const std::vector<uint8_t> valid_hdr =
        BuildSingleCblkPacket({.numpasses = 1, .seg_lens = {1}});
    {
      // Truncated right after header (0 bytes for EPH).
      DirectT2Context dec;
      ASSERT_TRUE(dec.Init(eph_cfg, /*is_decoder=*/true));
      std::vector<uint8_t> pkt = valid_hdr;
      OPJ_UINT32 read_bytes = 0;
      EXPECT_EQ(OPJ_FALSE,
                opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0,
                                      dec.tile(), pkt.data(), &read_bytes,
                                      static_cast<OPJ_UINT32>(pkt.size()),
                                      nullptr, &dec.mgr));
      EXPECT_THAT(dec.events.errors,
                  ElementsAre(HasSubstr("Not enough space for required EPH")));
    }
    for (const auto bad_eph : {std::array<uint8_t, 2>{0x00, 0x92},
                               std::array<uint8_t, 2>{0xFF, 0x00}}) {
      DirectT2Context dec;
      ASSERT_TRUE(dec.Init(eph_cfg, /*is_decoder=*/true));
      std::vector<uint8_t> pkt = valid_hdr;
      pkt.insert(pkt.end(), bad_eph.begin(), bad_eph.end());
      OPJ_UINT32 read_bytes = 0;
      EXPECT_EQ(OPJ_FALSE,
                opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0,
                                      dec.tile(), pkt.data(), &read_bytes,
                                      static_cast<OPJ_UINT32>(pkt.size()),
                                      nullptr, &dec.mgr));
      EXPECT_THAT(dec.events.errors,
                  ElementsAre(HasSubstr("Expected EPH marker")));
    }
  }

  // 3. Truncated packet header ending in unstuffed 0xFF fails opj_bio_inalign.
  {
    DirectT2Config cfg{.width = 8, .height = 8};
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(cfg, /*is_decoder=*/true));
    // 0xEE encodes present=1, incl=1, zbp=0, numpasses=1, increment=3 (3+3=6
    // length bits), then 0xFF supplies the 6 length bits leaving bio->buf &
    // 0xff == 0xff at EOF.
    std::vector<uint8_t> unstuffed_ff_hdr = {0xEE, 0xFF};
    OPJ_UINT32 read_bytes = 0;
    EXPECT_EQ(OPJ_FALSE, opj_t2_decode_packets(
                             dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                             unstuffed_ff_hdr.data(), &read_bytes,
                             static_cast<OPJ_UINT32>(unstuffed_ff_hdr.size()),
                             nullptr, &dec.mgr));
  }

  // 4. Invalid precinct (precincts_data_size == 0) emits "Invalid precinct".
  {
    DirectT2Config cfg{.width = 8, .height = 8};
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(cfg, /*is_decoder=*/true));
    opj_tcd_band_t& band0 = dec.tile()->comps[0].resolutions[0].bands[0];
    const OPJ_UINT32 saved_size = band0.precincts_data_size;
    absl::Cleanup restore_size(
        [&]() { band0.precincts_data_size = saved_size; });
    band0.precincts_data_size = 0;
    std::vector<uint8_t> pkt = {0x00};
    OPJ_UINT32 read_bytes = 0;
    EXPECT_EQ(OPJ_FALSE, opj_t2_decode_packets(
                             dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                             pkt.data(), &read_bytes, 1U, nullptr, &dec.mgr));
    EXPECT_THAT(dec.events.errors, ElementsAre(HasSubstr("Invalid precinct")));
  }

  // 5. OPJ_PROG_UNKNOWN and opj_pi_create_decode() failure (huge numlayers)
  // return OPJ_FALSE, and num_layers_to_decode == 0 sets resno_decoded via
  // first_pass_failed.
  {
    DirectT2Config cfg{
        .width = 16,
        .height = 16,
        .numcomps = 2,
        .numlayers = 1,
        .num_layers_to_decode = 0,
        .numresolutions = 3,
        .reduce = 1,  // minimum_num_resolutions = 2
    };
    DirectT2Context dec;
    ASSERT_TRUE(dec.Init(cfg, /*is_decoder=*/true));
    std::vector<uint8_t> empty_pkts(6, 0x00);
    OPJ_UINT32 read_bytes = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    empty_pkts.data(), &read_bytes,
                                    static_cast<OPJ_UINT32>(empty_pkts.size()),
                                    nullptr, &dec.mgr));
    EXPECT_EQ(6U, read_bytes);
    EXPECT_EQ(1U, dec.image->comps[0].resno_decoded);
    EXPECT_EQ(1U, dec.image->comps[1].resno_decoded);

    dec.tcps[0].prg = OPJ_PROG_UNKNOWN;
    EXPECT_EQ(OPJ_FALSE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    empty_pkts.data(), &read_bytes,
                                    static_cast<OPJ_UINT32>(empty_pkts.size()),
                                    nullptr, &dec.mgr));

    dec.tcps[0].prg = OPJ_LRCP;
    dec.tcps[0].numlayers = 0x80000000U;
    EXPECT_EQ(OPJ_FALSE,
              opj_t2_decode_packets(dec.tcd.get(), dec.t2.get(), 0, dec.tile(),
                                    empty_pkts.data(), &read_bytes,
                                    static_cast<OPJ_UINT32>(empty_pkts.size()),
                                    nullptr, &dec.mgr));
  }
}

TEST(T2Test, DirectDecodeStrictVsPermissiveTruncationAndSkipPacket) {
  // 1. Encode a 2-layer, 2-codeblock precinct (16x16 with 8x8 cblks) where both
  // cblks have data in both layers, plus a second test with 2 resolutions (3x1
  // image yielding empty + non-empty bands in res 1) and TERMALL.
  DirectT2Config cfg{
      .width = 16,
      .height = 16,
      .numcomps = 1,
      .numlayers = 2,
      .num_layers_to_decode = 2,
      .numresolutions = 1,
      .cblkw = 3,
      .cblkh = 3,
      .prcw = 4,
      .prch = 4,
  };
  DirectT2Context enc;
  ASSERT_TRUE(enc.Init(cfg, /*is_decoder=*/false));
  constexpr auto kLayers = std::to_array<uint32_t>({1, 1});
  constexpr auto kLens = std::to_array<uint32_t>({8, 4});
  constexpr auto kTerms = std::to_array<uint8_t>({0, 1});
  PopulateEncCblkPasses(enc.enc_cblk(0, 0, 0, 0, 0), 8, kLayers, kLens, kTerms,
                        0x10);
  PopulateEncCblkPasses(enc.enc_cblk(0, 0, 0, 0, 1), 8, kLayers, kLens, kTerms,
                        0x30);

  std::vector<opj_packet_info_t> pack_infos(2);
  std::vector<opj_tile_info_t> tile_infos(1);
  tile_infos[0].end_header = -1;
  tile_infos[0].packet = pack_infos.data();
  opj_codestream_info_t cstr_info{};
  cstr_info.index_write = 1;
  cstr_info.tile = tile_infos.data();

  std::vector<uint8_t> buf(512, 0);
  OPJ_UINT32 written = 0;
  ASSERT_EQ(OPJ_TRUE, opj_t2_encode_packets(
                          enc.t2.get(), 0, enc.tile(), 2, buf.data(), &written,
                          static_cast<OPJ_UINT32>(buf.size()), &cstr_info,
                          nullptr, 0, 0, 0, FINAL_PASS, &enc.mgr));
  const uint32_t pkt0_hdr_len =
      static_cast<uint32_t>(pack_infos[0].end_ph_pos + 1);
  // Truncate inside layer 0 cblk 0 body (requires 8 bytes, offer only 3).
  const uint32_t trunc_len = pkt0_hdr_len + 3U;
  ASSERT_LT(trunc_len, written);

  // Strict mode rejects truncated packet body with EVT_ERROR.
  {
    DirectT2Context dec_strict;
    ASSERT_TRUE(dec_strict.Init(cfg, /*is_decoder=*/true));
    OPJ_UINT32 bytes_read = 0;
    EXPECT_EQ(OPJ_FALSE, opj_t2_decode_packets(
                             dec_strict.tcd.get(), dec_strict.t2.get(), 0,
                             dec_strict.tile(), buf.data(), &bytes_read,
                             trunc_len, nullptr, &dec_strict.mgr));
    EXPECT_THAT(dec_strict.events.errors,
                ElementsAre(HasSubstr("read: segment too long")));
  }

  // Non-strict mode warns, marks cblk 0 & cblk 1 corrupted via partial_buffer,
  // and returns OPJ_TRUE.
  {
    DirectT2Config perm_cfg = cfg;
    perm_cfg.strict = OPJ_FALSE;
    DirectT2Context dec_perm;
    ASSERT_TRUE(dec_perm.Init(perm_cfg, /*is_decoder=*/true));
    OPJ_UINT32 bytes_read = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec_perm.tcd.get(), dec_perm.t2.get(), 0,
                                    dec_perm.tile(), buf.data(), &bytes_read,
                                    trunc_len, nullptr, &dec_perm.mgr));
    EXPECT_EQ(trunc_len, bytes_read);
    EXPECT_EQ(OPJ_TRUE, dec_perm.dec_cblk(0, 0, 0, 0, 0)->corrupted);
    EXPECT_EQ(OPJ_TRUE, dec_perm.dec_cblk(0, 0, 0, 0, 1)->corrupted);
    EXPECT_EQ(0U, dec_perm.dec_cblk(0, 0, 0, 0, 0)->numchunks);
    EXPECT_THAT(dec_perm.events.warnings,
                ElementsAre(HasSubstr("read: segment too long")));
  }

  // 2. Test opj_t2_skip_packet() and opj_t2_skip_packet_data() on a 12x1 image
  // with 2 resolutions (empty LH/HH bands + non-empty HL band in res 1),
  // 2 layers, LAZY mode (multi-segment in skipped packets, exercising both
  // numpasses == maxpasses and numpasses != maxpasses), and empty packet.
  {
    DirectT2Config skip_cfg{
        .width = 12,
        .height = 1,
        .numcomps = 1,
        .numlayers = 2,
        .num_layers_to_decode = 1,  // skip layer 1
        .numresolutions = 2,
        .cblkw = 2,
        .cblkh = 2,
        .cblksty = J2K_CCP_CBLKSTY_LAZY,
        .reduce = 1,  // minimum_num_resolutions = 1, skip res 1
    };
    DirectT2Context skip_enc;
    ASSERT_TRUE(skip_enc.Init(skip_cfg, /*is_decoder=*/false));
    ASSERT_EQ(
        2U,
        skip_enc.tile()->comps[0].resolutions[0].bands[0].precincts[0].cw *
            skip_enc.tile()->comps[0].resolutions[0].bands[0].precincts[0].ch);
    // Res 0 cblk 0: 12 passes in layer 0 (10 + 2 LAZY segs, hitting maxpasses
    // == 2), 2 passes in layer 1 (1 + 1 LAZY segs).
    constexpr auto kRes0LayersA = std::to_array<uint32_t>({12, 2});
    std::array<uint32_t, 14> res0_lens_a{};
    res0_lens_a.fill(2U);
    constexpr auto kRes0TermsA =
        std::to_array<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1});
    PopulateEncCblkPasses(skip_enc.enc_cblk(0, 0, 0, 0, 0), 8, kRes0LayersA,
                          res0_lens_a, kRes0TermsA, 0x10);
    // Res 0 cblk 1: 11 passes in layer 0 (10 + 1 LAZY seg, so numpasses == 1
    // != maxpasses == 2), 2 passes in layer 1.
    constexpr auto kRes0LayersB = std::to_array<uint32_t>({11, 2});
    std::array<uint32_t, 13> res0_lens_b{};
    res0_lens_b.fill(2U);
    constexpr auto kRes0TermsB =
        std::to_array<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1});
    PopulateEncCblkPasses(skip_enc.enc_cblk(0, 0, 0, 0, 1), 8, kRes0LayersB,
                          res0_lens_b, kRes0TermsB, 0x40);
    // Res 1, band 0 (HL1): 1 pass in layer 0, 0 passes in layer 1 (tests
    // !numnewpasses in skip_packet_data).
    constexpr auto kRes1Layers = std::to_array<uint32_t>({1, 0});
    constexpr auto kRes1Lens = std::to_array<uint32_t>({4});
    constexpr auto kRes1Terms = std::to_array<uint8_t>({1});
    PopulateEncCblkPasses(skip_enc.enc_cblk(0, 1, 0, 0, 0), 8, kRes1Layers,
                          kRes1Lens, kRes1Terms, 0x90);

    std::vector<uint8_t> skip_buf(512, 0);
    OPJ_UINT32 skip_written = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_encode_packets(
                  skip_enc.t2.get(), 0, skip_enc.tile(), 2, skip_buf.data(),
                  &skip_written, static_cast<OPJ_UINT32>(skip_buf.size()),
                  nullptr, nullptr, 0, 0, 0, FINAL_PASS, &skip_enc.mgr));

    // Full skip decoding consumes all bytes cleanly.
    DirectT2Context skip_dec;
    ASSERT_TRUE(skip_dec.Init(skip_cfg, /*is_decoder=*/true));
    OPJ_UINT32 skip_read = 0;
    ASSERT_EQ(OPJ_TRUE, opj_t2_decode_packets(
                            skip_dec.tcd.get(), skip_dec.t2.get(), 0,
                            skip_dec.tile(), skip_buf.data(), &skip_read,
                            skip_written, nullptr, &skip_dec.mgr));
    EXPECT_EQ(skip_written, skip_read);
    EXPECT_EQ(2U, skip_dec.dec_cblk(0, 0, 0, 0, 0)->real_num_segs);
    EXPECT_EQ(4U, skip_dec.dec_cblk(0, 0, 0, 0, 0)->numsegs);
    EXPECT_EQ(2U, skip_dec.dec_cblk(0, 0, 0, 0, 1)->real_num_segs);
    EXPECT_EQ(3U, skip_dec.dec_cblk(0, 0, 0, 0, 1)->numsegs);

    // Truncating inside skipped packet body fails in strict mode and succeeds
    // with warning in non-strict mode.
    const uint32_t skip_trunc = skip_written - 2U;
    {
      DirectT2Context dec_skip_strict;
      ASSERT_TRUE(dec_skip_strict.Init(skip_cfg, /*is_decoder=*/true));
      OPJ_UINT32 r = 0;
      EXPECT_EQ(OPJ_FALSE,
                opj_t2_decode_packets(
                    dec_skip_strict.tcd.get(), dec_skip_strict.t2.get(), 0,
                    dec_skip_strict.tile(), skip_buf.data(), &r, skip_trunc,
                    nullptr, &dec_skip_strict.mgr));
      EXPECT_THAT(dec_skip_strict.events.errors,
                  ElementsAre(HasSubstr("skip: segment too long")));
    }
    {
      DirectT2Config skip_perm_cfg = skip_cfg;
      skip_perm_cfg.strict = OPJ_FALSE;
      DirectT2Context dec_skip_perm;
      ASSERT_TRUE(dec_skip_perm.Init(skip_perm_cfg, /*is_decoder=*/true));
      OPJ_UINT32 r = 0;
      ASSERT_EQ(OPJ_TRUE, opj_t2_decode_packets(
                              dec_skip_perm.tcd.get(), dec_skip_perm.t2.get(),
                              0, dec_skip_perm.tile(), skip_buf.data(), &r,
                              skip_trunc, nullptr, &dec_skip_perm.mgr));
      EXPECT_EQ(skip_trunc, r);
      EXPECT_THAT(dec_skip_perm.events.warnings,
                  ElementsAre(HasSubstr("skip: segment too long")));
    }

    // Truncating inside skipped packet header (unstuffed 0xFF with EPH
    // required) fails opj_t2_skip_packet via opj_t2_read_packet_header.
    {
      DirectT2Config skip_hdr_fail_cfg = skip_cfg;
      skip_hdr_fail_cfg.numlayers = 1;
      skip_hdr_fail_cfg.num_layers_to_decode = 0;
      skip_hdr_fail_cfg.numresolutions = 1;
      skip_hdr_fail_cfg.reduce = 0;
      skip_hdr_fail_cfg.csty = J2K_CP_CSTY_EPH;
      DirectT2Context dec_hdr_fail;
      ASSERT_TRUE(dec_hdr_fail.Init(skip_hdr_fail_cfg, /*is_decoder=*/true));
      std::vector<uint8_t> bad_empty = {0x00};  // missing required EPH
      OPJ_UINT32 r = 0;
      EXPECT_EQ(OPJ_FALSE, opj_t2_decode_packets(
                               dec_hdr_fail.tcd.get(), dec_hdr_fail.t2.get(), 0,
                               dec_hdr_fail.tile(), bad_empty.data(), &r, 1U,
                               nullptr, &dec_hdr_fail.mgr));
    }
  }
}

TEST(T2Test, DirectDecodePpmAndPptPacketHeaderBuffers) {
  // 1. Encode a 2-layer packet sequence with EPH so cstr_info gives exact
  // header vs body boundaries for each packet.
  DirectT2Config cfg{
      .width = 8,
      .height = 8,
      .numcomps = 1,
      .numlayers = 2,
      .num_layers_to_decode = 2,
      .numresolutions = 1,
      .csty = J2K_CP_CSTY_EPH,
  };
  DirectT2Context enc;
  ASSERT_TRUE(enc.Init(cfg, /*is_decoder=*/false));
  constexpr auto kLayers = std::to_array<uint32_t>({1, 2});
  constexpr auto kLens = std::to_array<uint32_t>({3, 2, 2});
  constexpr auto kTerms = std::to_array<uint8_t>({0, 0, 1});
  PopulateEncCblkPasses(enc.enc_cblk(0, 0, 0, 0, 0), 8, kLayers, kLens, kTerms,
                        0x41);

  std::vector<opj_packet_info_t> pack_infos(2);
  std::vector<opj_tile_info_t> tile_infos(1);
  tile_infos[0].end_header = -1;
  tile_infos[0].packet = pack_infos.data();
  opj_codestream_info_t cstr_info{};
  cstr_info.index_write = 1;
  cstr_info.tile = tile_infos.data();

  std::vector<uint8_t> combined(256, 0);
  OPJ_UINT32 written = 0;
  ASSERT_EQ(OPJ_TRUE, opj_t2_encode_packets(
                          enc.t2.get(), 0, enc.tile(), 2, combined.data(),
                          &written, static_cast<OPJ_UINT32>(combined.size()),
                          &cstr_info, nullptr, 0, 0, 0, FINAL_PASS, &enc.mgr));

  std::vector<uint8_t> hdrs;
  std::vector<uint8_t> bodies;
  for (const opj_packet_info_t& info : pack_infos) {
    const size_t start = static_cast<size_t>(info.start_pos);
    const size_t end_ph = static_cast<size_t>(info.end_ph_pos);
    const size_t end_p = static_cast<size_t>(info.end_pos);
    hdrs.insert(hdrs.end(), combined.begin() + start,
                combined.begin() + end_ph + 1);
    bodies.insert(bodies.end(), combined.begin() + end_ph + 1,
                  combined.begin() + end_p + 1);
  }
  ASSERT_EQ(7U, bodies.size());

  // 2. Decode with cp.ppm == 1 (PPM) and verify ppm_len reaches 0.
  {
    DirectT2Context dec_ppm;
    ASSERT_TRUE(dec_ppm.Init(cfg, /*is_decoder=*/true));
    std::vector<uint8_t> ppm_copy = hdrs;
    dec_ppm.cp.ppm = 1;
    dec_ppm.cp.ppm_data = ppm_copy.data();
    dec_ppm.cp.ppm_len = static_cast<OPJ_UINT32>(ppm_copy.size());

    OPJ_UINT32 read_bytes = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec_ppm.tcd.get(), dec_ppm.t2.get(), 0,
                                    dec_ppm.tile(), bodies.data(), &read_bytes,
                                    static_cast<OPJ_UINT32>(bodies.size()),
                                    nullptr, &dec_ppm.mgr));
    EXPECT_EQ(bodies.size(), read_bytes);
    EXPECT_EQ(0U, dec_ppm.cp.ppm_len);
    EXPECT_THAT(ExtractChunkBytes(*dec_ppm.dec_cblk(0, 0, 0, 0, 0)),
                ElementsAre(0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47));
  }

  // 3. Decode with tcp.ppt == 1 (PPT) and also verify corrupted cblk in layer 0
  // triggers l_cblk->corrupted in layer 1 while PPT header remains valid.
  {
    DirectT2Context dec_ppt;
    ASSERT_TRUE(dec_ppt.Init(cfg, /*is_decoder=*/true));
    std::vector<uint8_t> ppt_copy = hdrs;
    dec_ppt.tcps[0].ppt = 1;
    dec_ppt.tcps[0].ppt_data = ppt_copy.data();
    dec_ppt.tcps[0].ppt_len = static_cast<OPJ_UINT32>(ppt_copy.size());

    OPJ_UINT32 read_bytes = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_t2_decode_packets(dec_ppt.tcd.get(), dec_ppt.t2.get(), 0,
                                    dec_ppt.tile(), bodies.data(), &read_bytes,
                                    static_cast<OPJ_UINT32>(bodies.size()),
                                    nullptr, &dec_ppt.mgr));
    EXPECT_EQ(bodies.size(), read_bytes);
    EXPECT_EQ(0U, dec_ppt.tcps[0].ppt_len);
    EXPECT_THAT(ExtractChunkBytes(*dec_ppt.dec_cblk(0, 0, 0, 0, 0)),
                ElementsAre(0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47));

    DirectT2Config perm_cfg = cfg;
    perm_cfg.strict = OPJ_FALSE;
    DirectT2Context dec_ppt_trunc;
    ASSERT_TRUE(dec_ppt_trunc.Init(perm_cfg, /*is_decoder=*/true));
    std::vector<uint8_t> ppt_copy2 = hdrs;
    dec_ppt_trunc.tcps[0].ppt = 1;
    dec_ppt_trunc.tcps[0].ppt_data = ppt_copy2.data();
    dec_ppt_trunc.tcps[0].ppt_len = static_cast<OPJ_UINT32>(ppt_copy2.size());

    OPJ_UINT32 trunc_read = 0;
    ASSERT_EQ(OPJ_TRUE, opj_t2_decode_packets(
                            dec_ppt_trunc.tcd.get(), dec_ppt_trunc.t2.get(), 0,
                            dec_ppt_trunc.tile(), bodies.data(), &trunc_read,
                            1U, nullptr, &dec_ppt_trunc.mgr));
    EXPECT_EQ(OPJ_TRUE, dec_ppt_trunc.dec_cblk(0, 0, 0, 0, 0)->corrupted);
    EXPECT_EQ(0U, dec_ppt_trunc.dec_cblk(0, 0, 0, 0, 0)->numchunks);
  }
}

TEST(T2Test, EndToEndLosslessRoundTripSopEphPltAndEmptyBands) {
  // 1. Multi-layer, multi-resolution, multi-component lossless round-trip with
  // SOP, EPH, PRT, TERMALL, and PLT=YES.
  {
    OpjImagePtr src =
        CreateTestImage(64, 64, 3, 8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);

    opj_cparameters_t cparams{};
    opj_set_default_encoder_parameters(&cparams);
    cparams.tcp_numlayers = 3;
    cparams.tcp_rates[0] = 8.0f;
    cparams.tcp_rates[1] = 3.0f;
    cparams.tcp_rates[2] = 0.0f;
    cparams.cp_disto_alloc = 1;
    cparams.numresolution = 3;
    cparams.csty = J2K_CP_CSTY_SOP | J2K_CP_CSTY_EPH | J2K_CP_CSTY_PRT;
    cparams.res_spec = 2;
    cparams.prcw_init[0] = 32;
    cparams.prch_init[0] = 32;
    cparams.prcw_init[1] = 16;
    cparams.prch_init[1] = 16;
    cparams.mode = J2K_CCP_CBLKSTY_TERMALL;
    constexpr const char* kOptions[] = {"PLT=YES", nullptr};

    EventCapture enc_events;
    const std::optional<std::vector<uint8_t>> j2k = CompressImageToBytes(
        src.get(), OPJ_CODEC_J2K, &cparams, kOptions, &enc_events);
    ASSERT_TRUE(j2k.has_value())
        << (enc_events.errors.empty() ? "" : enc_events.errors.front());

    OpjImagePtr dst = DecompressBytesToImage(*j2k, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, dst);
    ExpectImagesEqual(src.get(), dst.get());
  }

  // 2. Tiled edge tiles of size 1x1 and 3x1 with 4x4 nominal tile size and
  // 3 resolutions create empty subbands (opj_tcd_is_band_empty) during both T2
  // encode and T2 decode while satisfying tdx/tdy >= 2^(numresolution - 1).
  struct TileEdgeSize {
    uint32_t w;
    uint32_t h;
  };
  constexpr TileEdgeSize kEdgeSizes[] = {{.w = 5U, .h = 5U},
                                         {.w = 7U, .h = 5U}};
  for (const TileEdgeSize& tc : kEdgeSizes) {
    OpjImagePtr src =
        CreateTestImage(tc.w, tc.h, 1, 8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
    ASSERT_NE(nullptr, src);
    opj_cparameters_t cparams =
        MakeLosslessParams(3, TileGeometry{.tdx = 4, .tdy = 4});

    const std::optional<std::vector<uint8_t>> j2k =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(j2k.has_value()) << tc.w << "x" << tc.h;
    OpjImagePtr dst = DecompressBytesToImage(*j2k, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, dst) << tc.w << "x" << tc.h;
    ExpectImagesEqual(src.get(), dst.get());
  }
}

TEST(T2Test, EndToEndTilePartsProgressionOrdersAndPartialDecodeArea) {
  // 1. Exercise all 5 progression orders and tile-part split flags (L, R, C,
  // P).
  struct ProgTpCase {
    std::string_view name;
    OPJ_PROG_ORDER prg;
    char tp_flag;
  };
  constexpr ProgTpCase kCases[] = {
      {.name = "LRCP_L", .prg = OPJ_LRCP, .tp_flag = 'L'},
      {.name = "RLCP_R", .prg = OPJ_RLCP, .tp_flag = 'R'},
      {.name = "RPCL_P", .prg = OPJ_RPCL, .tp_flag = 'P'},
      {.name = "PCRL_C", .prg = OPJ_PCRL, .tp_flag = 'C'},
      {.name = "CPRL_C", .prg = OPJ_CPRL, .tp_flag = 'C'},
  };

  OpjImagePtr src = CreateTestImage(32, 32, 3, 8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
  ASSERT_NE(nullptr, src);

  for (const ProgTpCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    opj_cparameters_t cparams{};
    opj_set_default_encoder_parameters(&cparams);
    cparams.tcp_numlayers = 2;
    cparams.tcp_rates[0] = 4.0f;
    cparams.tcp_rates[1] = 0.0f;
    cparams.cp_disto_alloc = 1;
    cparams.numresolution = 3;
    cparams.prog_order = tc.prg;
    cparams.tp_on = 1;
    cparams.tp_flag = tc.tp_flag;

    const std::optional<std::vector<uint8_t>> j2k =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(j2k.has_value());

    OpjImagePtr full = DecompressBytesToImage(*j2k, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, full);
    ExpectImagesEqual(src.get(), full.get());

    // Decode with cp_layer=1 and cp_reduce=1 to exercise packet skipping.
    OpjImagePtr reduced = DecompressBytesToImage(
        *j2k, OPJ_CODEC_J2K, {.layer = 1U, .resolution_factor = 1U});
    ASSERT_NE(nullptr, reduced);
    EXPECT_EQ(16U, reduced->comps[0].w);
    EXPECT_EQ(16U, reduced->comps[0].h);
  }

  // 2. Partial decode area on a multi-precinct image skips out-of-ROI
  // precincts via opj_tcd_is_subband_area_of_interest().
  OpjImagePtr large_src =
      CreateTestImage(64, 64, 1, 8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
  ASSERT_NE(nullptr, large_src);
  opj_cparameters_t prt_params = MakeLosslessParams(3);
  prt_params.csty = J2K_CP_CSTY_PRT;
  prt_params.res_spec = 2;
  prt_params.prcw_init[0] = 16;
  prt_params.prch_init[0] = 16;
  prt_params.prcw_init[1] = 8;
  prt_params.prch_init[1] = 8;

  const std::optional<std::vector<uint8_t>> prt_j2k =
      CompressImageToBytes(large_src.get(), OPJ_CODEC_J2K, &prt_params);
  ASSERT_TRUE(prt_j2k.has_value());

  OpjImagePtr area_img = DecompressBytesToImage(
      *prt_j2k, OPJ_CODEC_J2K, {.area = std::array<int32_t, 4>{0, 0, 16, 16}});
  ASSERT_NE(nullptr, area_img);
  ASSERT_EQ(16U, area_img->comps[0].w);
  ASSERT_EQ(16U, area_img->comps[0].h);
  for (uint32_t y = 0; y < 16U; ++y) {
    for (uint32_t x = 0; x < 16U; ++x) {
      EXPECT_EQ(large_src->comps[0].data[y * 64U + x],
                area_img->comps[0].data[y * 16U + x])
          << "(" << x << "," << y << ")";
    }
  }
}

TEST(T2Test, EndToEndCinema4kProfileRoundTrip) {
  OpjImagePtr src = CreateTestImage(64, 64, 3, 12, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
  ASSERT_NE(nullptr, src);

  opj_cparameters_t cparams{};
  opj_set_default_encoder_parameters(&cparams);
  cparams.rsiz = OPJ_PROFILE_CINEMA_4K;
  cparams.numresolution = 3;
  cparams.max_cs_size = OPJ_CINEMA_24_CS;
  cparams.max_comp_size = OPJ_CINEMA_24_COMP;

  const std::optional<std::vector<uint8_t>> j2k =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(j2k.has_value());
  EXPECT_LE(j2k->size(), static_cast<size_t>(OPJ_CINEMA_24_CS));

  OpjImagePtr dst = DecompressBytesToImage(*j2k, OPJ_CODEC_J2K);
  ASSERT_NE(nullptr, dst);
  EXPECT_EQ(64U, dst->comps[0].w);
  EXPECT_EQ(64U, dst->comps[0].h);
}

TEST(T2Test, EndToEndSynthesizedPpmAndPptMarkersDecodeLosslessly) {
  // 1. Encode a single-packet 8x8 lossless codestream with EPH so 0xFF 0x92
  // unambiguously delimits the packet header from the packet body.
  OpjImagePtr src = CreateTestImage(8, 8, 1, 8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
  ASSERT_NE(nullptr, src);
  opj_cparameters_t cparams{};
  opj_set_default_encoder_parameters(&cparams);
  cparams.tcp_numlayers = 1;
  cparams.tcp_rates[0] = 0.0f;
  cparams.cp_disto_alloc = 1;
  cparams.numresolution = 1;
  cparams.csty = J2K_CP_CSTY_EPH;

  const std::optional<std::vector<uint8_t>> base_j2k =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(base_j2k.has_value());

  const std::optional<TilePartOffsets> offsets =
      FindFirstTilePartMarkers(*base_j2k);
  ASSERT_TRUE(offsets.has_value());
  ASSERT_LT(offsets->sot_pos, offsets->sod_pos);

  const size_t ph_begin = offsets->sod_pos + 2U;
  constexpr auto kEph = std::to_array<uint8_t>({0xFF, 0x92});
  const auto eph_it =
      std::search(base_j2k->begin() + ph_begin, base_j2k->end() - 2U,
                  kEph.begin(), kEph.end());
  ASSERT_NE(base_j2k->end() - 2U, eph_it);
  const size_t ph_end =
      static_cast<size_t>(std::distance(base_j2k->begin(), eph_it)) + 2U;
  const uint32_t ph_len = static_cast<uint32_t>(ph_end - ph_begin);
  const uint32_t orig_psot = ReadBe32(*base_j2k, offsets->sot_pos + 6U);

  // 2. Move the packet header + EPH into a PPT marker in the tile-part header.
  {
    std::vector<uint8_t> ppt_j2k = *base_j2k;
    const uint16_t lppt = static_cast<uint16_t>(3U + ph_len);
    std::vector<uint8_t> ppt_marker = {
        0xFF, 0x61, static_cast<uint8_t>(lppt >> 8U),
        static_cast<uint8_t>(lppt & 0xFFU), 0x00};
    ppt_marker.insert(ppt_marker.end(), base_j2k->begin() + ph_begin,
                      base_j2k->begin() + ph_end);
    ppt_j2k.erase(ppt_j2k.begin() + ph_begin, ppt_j2k.begin() + ph_end);
    ppt_j2k.insert(ppt_j2k.begin() + offsets->sod_pos, ppt_marker.begin(),
                   ppt_marker.end());
    WriteBe32(ppt_j2k, offsets->sot_pos + 6U, orig_psot + 5U);

    OpjImagePtr dst = DecompressBytesToImage(ppt_j2k, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, dst);
    ExpectImagesEqual(src.get(), dst.get());
  }

  // 3. Move the packet header + EPH into a PPM marker in the main header.
  {
    std::vector<uint8_t> ppm_j2k = *base_j2k;
    const uint16_t lppm = static_cast<uint16_t>(7U + ph_len);
    std::vector<uint8_t> ppm_marker = {
        0xFF,
        0x60,
        static_cast<uint8_t>(lppm >> 8U),
        static_cast<uint8_t>(lppm & 0xFFU),
        0x00,
        static_cast<uint8_t>((ph_len >> 24U) & 0xFFU),
        static_cast<uint8_t>((ph_len >> 16U) & 0xFFU),
        static_cast<uint8_t>((ph_len >> 8U) & 0xFFU),
        static_cast<uint8_t>(ph_len & 0xFFU),
    };
    ppm_marker.insert(ppm_marker.end(), base_j2k->begin() + ph_begin,
                      base_j2k->begin() + ph_end);
    ppm_j2k.erase(ppm_j2k.begin() + ph_begin, ppm_j2k.begin() + ph_end);
    ppm_j2k.insert(ppm_j2k.begin() + offsets->sot_pos, ppm_marker.begin(),
                   ppm_marker.end());
    WriteBe32(ppm_j2k, offsets->sot_pos + ppm_marker.size() + 6U,
              orig_psot - ph_len);

    OpjImagePtr dst = DecompressBytesToImage(ppm_j2k, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, dst);
    ExpectImagesEqual(src.get(), dst.get());
  }
}

TEST(T2Test, EndToEndMissingSopWarningAndTruncatedStreamStrictVsPermissive) {
  // 1. Remove the 6-byte SOP marker from a single-packet SOP codestream and
  // confirm the decoder warns and recovers pixels losslessly.
  OpjImagePtr src = CreateTestImage(16, 16, 1, 8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
  ASSERT_NE(nullptr, src);
  opj_cparameters_t cparams = MakeLosslessParams(1);
  cparams.csty = J2K_CP_CSTY_SOP;

  const std::optional<std::vector<uint8_t>> base_j2k =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(base_j2k.has_value());

  const std::optional<TilePartOffsets> offsets =
      FindFirstTilePartMarkers(*base_j2k);
  ASSERT_TRUE(offsets.has_value());
  ASSERT_GE(base_j2k->size(), offsets->sod_pos + 8U);
  ASSERT_EQ(0xFFU, (*base_j2k)[offsets->sod_pos + 2U]);
  ASSERT_EQ(0x91U, (*base_j2k)[offsets->sod_pos + 3U]);

  std::vector<uint8_t> no_sop = *base_j2k;
  no_sop.erase(no_sop.begin() + offsets->sod_pos + 2U,
               no_sop.begin() + offsets->sod_pos + 8U);
  const uint32_t orig_psot = ReadBe32(*base_j2k, offsets->sot_pos + 6U);
  WriteBe32(no_sop, offsets->sot_pos + 6U, orig_psot - 6U);

  EventCapture events;
  OpjImagePtr recovered =
      DecompressBytesToImage(no_sop, OPJ_CODEC_J2K, {.events = &events});
  ASSERT_NE(nullptr, recovered);
  EXPECT_THAT(events.warnings, ElementsAre(HasSubstr("Expected SOP marker")));
  ExpectImagesEqual(src.get(), recovered.get());

  // 2. Truncate the codestream inside the packet body (and set Psot=0 for
  // unknown tile-part length) to compare strict failure vs permissive recovery.
  std::vector<uint8_t> truncated = no_sop;
  ASSERT_GT(truncated.size(), offsets->sod_pos + 12U);
  WriteBe32(truncated, offsets->sot_pos + 6U, 0U);
  truncated.resize(truncated.size() - 8U);

  EventCapture strict_events;
  EXPECT_EQ(nullptr,
            DecompressBytesToImage(truncated, OPJ_CODEC_J2K,
                                   {.events = &strict_events, .strict = true}));
  EXPECT_THAT(strict_events.errors, Not(IsEmpty()));

  EventCapture perm_events;
  OpjImagePtr partial = DecompressBytesToImage(
      truncated, OPJ_CODEC_J2K, {.events = &perm_events, .strict = false});
  ASSERT_NE(nullptr, partial);
  EXPECT_THAT(perm_events.warnings, Not(IsEmpty()));
}

// TODO(schwehr): Cover remaining allocator OOM checks in t2.c (excluding the
// layno > 0 precno bounds branch in opj_t2_encode_packet() which leaks its
// local opj_bio_t upstream).

}  // namespace
}  // namespace openjpeg2
