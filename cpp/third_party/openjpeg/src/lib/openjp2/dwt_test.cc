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
// Tests internal OpenJPEG 5-3 reversible and 9-7 irreversible discrete wavelet
// transforms, quantization norms/stepsizes, partial-tile windowed decoding,
// multi-threaded execution, and J2K round-trips (src/lib/openjp2/dwt.c).
//
// Caveats for google3:
// - STANDARD_SLOW_VERSION and TWO_PASS_VERSION are undefined.
// - SSE/SSE2 vertical paths compile on x86_64 while scalar fallback paths run
//   under --cpu=arm; AVX2/AVX512F branches depend on compiler target flags.
// - DirectTileComp / DecoderTcdContext inspect internal opj_tcd_* fields and
//   allocate decoded_data with opj_aligned_malloc to match opj_tcd teardown.
// - Empirical 9-7 float tolerances (kFloatRoundTripTol = 1e-2f, kFloatStrictTol
//   = 1e-4f, full J2K <= 6, windowed <= 1) are validated across x86 SSE2 and
//   --cpu=arm scalar paths.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/dwt.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/dwt.h

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/alloc_fault_injector.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(OPJ_FLOAT32) == sizeof(OPJ_INT32));

constexpr OPJ_INT32 kIntSentinel = -999999;
constexpr OPJ_FLOAT32 kFloatSentinel = -999.0f;
constexpr OPJ_FLOAT32 kOpjK = 1.230174105f;
constexpr OPJ_FLOAT32 kTwoInvK = 1.625732422f;
// Compensates opj_dwt_encode_1_real's opj_K high-pass scale against
// opj_v8dwt_decode's two_invK (BUG_WEIRD_TWO_INVK) on direct round-trips.
constexpr OPJ_FLOAT32 kDirect97HighPassScale = 1.0f / (kOpjK * kTwoInvK);
constexpr OPJ_FLOAT32 kFloatRoundTripTol = 1e-2f;
constexpr OPJ_FLOAT32 kFloatStrictTol = 1e-4f;

OpjImagePtr MakeSmoothJ2kTestImage(uint32_t width, uint32_t height,
                                   uint32_t numcomps,
                                   OPJ_COLOR_SPACE color_space, uint32_t dx,
                                   uint32_t dy, uint32_t x0, uint32_t y0) {
  std::vector<opj_image_cmptparm_t> params(numcomps);
  for (uint32_t c = 0; c < numcomps; ++c) {
    const uint32_t comp_dx = (c == 0) ? 1 : dx;
    const uint32_t comp_dy = (c == 0) ? 1 : dy;
    params[c].dx = comp_dx;
    params[c].dy = comp_dy;
    params[c].w =
        opj_uint_ceildiv(x0 + width, comp_dx) - opj_uint_ceildiv(x0, comp_dx);
    params[c].h =
        opj_uint_ceildiv(y0 + height, comp_dy) - opj_uint_ceildiv(y0, comp_dy);
    params[c].x0 = opj_uint_ceildiv(x0, comp_dx);
    params[c].y0 = opj_uint_ceildiv(y0, comp_dy);
    params[c].prec = 8;
    params[c].sgnd = 0;
  }
  OpjImagePtr image(opj_image_create(numcomps, params.data(), color_space));
  if (!image) {
    return nullptr;
  }
  image->x0 = x0;
  image->y0 = y0;
  image->x1 = x0 + width;
  image->y1 = y0 + height;

  for (uint32_t c = 0; c < numcomps; ++c) {
    const uint32_t cw = image->comps[c].w;
    const uint32_t ch = image->comps[c].h;
    for (uint32_t y = 0; y < ch; ++y) {
      for (uint32_t x = 0; x < cw; ++x) {
        // Continuous triangle-wave pattern in [24, 204] avoids modulo-256
        // wraparound cliffs that amplify 9-7 Gibbs ringing.
        const uint32_t phase = (x * 7U + y * 11U + c * 37U) % 360U;
        const uint32_t tri = (phase <= 180U) ? phase : (360U - phase);
        image->comps[c].data[static_cast<size_t>(y) * cw + x] =
            static_cast<OPJ_INT32>(24U + tri);
      }
    }
  }
  return image;
}

struct TileOrigin {
  std::string_view name;
  OPJ_INT32 x0;
  OPJ_INT32 y0;
};

constexpr auto kAllParities = std::to_array<TileOrigin>({
    {.name = "even_even", .x0 = 0, .y0 = 0},
    {.name = "odd_even", .x0 = 1, .y0 = 0},
    {.name = "even_odd", .x0 = 0, .y0 = 1},
    {.name = "odd_odd", .x0 = 1, .y0 = 1},
});

constexpr auto kMultiLevelParities = std::to_array<TileOrigin>({
    {.name = "even_even", .x0 = 0, .y0 = 0},
    {.name = "odd_even", .x0 = 1, .y0 = 0},
    {.name = "even_odd", .x0 = 0, .y0 = 1},
    {.name = "odd_odd", .x0 = 1, .y0 = 1},
    {.name = "mixed_3_5", .x0 = 3, .y0 = 5},
});

struct GeometryCase {
  std::string_view name;
  OPJ_UINT32 width;
  OPJ_UINT32 height;
  OPJ_UINT32 numres;
};

struct DirectTileComp {
  opj_tcd_t tcd{};
  opj_tcd_tilecomp_t tilec{};
  std::vector<opj_tcd_resolution_t> resolutions;
  OpjImageDataPtr buffer;

  void Init(opj_thread_pool_t* tp, OPJ_INT32 x0, OPJ_INT32 y0, OPJ_UINT32 width,
            OPJ_UINT32 height, OPJ_UINT32 numres,
            OPJ_BOOL whole_tile = OPJ_TRUE) {
    tcd = opj_tcd_t{};
    tcd.thread_pool = tp;
    tcd.whole_tile_decoding = whole_tile;

    tilec = opj_tcd_tilecomp_t{};
    tilec.x0 = x0;
    tilec.y0 = y0;
    tilec.x1 = x0 + static_cast<OPJ_INT32>(width);
    tilec.y1 = y0 + static_cast<OPJ_INT32>(height);
    tilec.numresolutions = numres;
    tilec.minimum_num_resolutions = numres;

    resolutions.assign(numres, opj_tcd_resolution_t{});
    for (OPJ_UINT32 resno = 0; resno < numres; ++resno) {
      const OPJ_UINT32 level = numres - 1U - resno;
      resolutions[resno].x0 = opj_int_ceildivpow2(tilec.x0, level);
      resolutions[resno].y0 = opj_int_ceildivpow2(tilec.y0, level);
      resolutions[resno].x1 = opj_int_ceildivpow2(tilec.x1, level);
      resolutions[resno].y1 = opj_int_ceildivpow2(tilec.y1, level);
      resolutions[resno].numbands = (resno == 0) ? 1U : 3U;
    }
    tilec.resolutions = resolutions.data();

    const size_t count = static_cast<size_t>(width) * height;
    if (count > 0) {
      buffer.reset(static_cast<OPJ_INT32*>(
          opj_image_data_alloc(count * sizeof(OPJ_INT32))));
      ASSERT_NE(nullptr, buffer.get());
      std::fill_n(buffer.get(), count, 0);
      tilec.data = buffer.get();
      tilec.data_size = count * sizeof(OPJ_INT32);
      tilec.data_size_needed = tilec.data_size;
    } else {
      buffer.reset();
      tilec.data = nullptr;
    }
  }
};

std::vector<OPJ_INT32> MakeDeterministicIntSamples(OPJ_UINT32 width,
                                                   OPJ_UINT32 height) {
  const size_t count = static_cast<size_t>(width) * height;
  std::vector<OPJ_INT32> samples(count);
  for (OPJ_UINT32 y = 0; y < height; ++y) {
    for (OPJ_UINT32 x = 0; x < width; ++x) {
      const int32_t raw = static_cast<int32_t>(
          (x * 37U + y * 73U + ((x ^ y) * 19U) + (x * y)) % 1021U);
      samples[static_cast<size_t>(y) * width + x] = raw - 510;
    }
  }
  return samples;
}

std::vector<OPJ_FLOAT32> MakeDeterministicFloatSamples(OPJ_UINT32 width,
                                                       OPJ_UINT32 height) {
  const size_t count = static_cast<size_t>(width) * height;
  std::vector<OPJ_FLOAT32> samples(count);
  for (OPJ_UINT32 y = 0; y < height; ++y) {
    for (OPJ_UINT32 x = 0; x < width; ++x) {
      const int32_t centered =
          static_cast<int32_t>((x * 41U + y * 83U + ((x ^ y) * 13U)) % 512U) -
          256;
      const OPJ_FLOAT32 frac =
          static_cast<OPJ_FLOAT32>((x + y * 3U) % 4U) * 0.25f;
      samples[static_cast<size_t>(y) * width + x] =
          static_cast<OPJ_FLOAT32>(centered) + frac;
    }
  }
  return samples;
}

void LoadFloatSamples(std::span<const OPJ_FLOAT32> src, OPJ_INT32* dst) {
  for (size_t i = 0; i < src.size(); ++i) {
    dst[i] = std::bit_cast<OPJ_INT32>(src[i]);
  }
}

std::vector<OPJ_FLOAT32> ExtractFloatSamples(const OPJ_INT32* src,
                                             size_t count) {
  std::vector<OPJ_FLOAT32> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = std::bit_cast<OPJ_FLOAT32>(src[i]);
  }
  return out;
}

// Scales HL, LH, and HH high-pass subbands in place inside an encoded 9-7
// tile-component buffer so direct opj_dwt_decode_real calls match
// opj_dwt_encode_real without t1/tcd stepsize compensation.
void ScaleEncoded97HighPassForDirectDecode(opj_tcd_tilecomp_t* tilec,
                                           OPJ_UINT32 numres) {
  const OPJ_UINT32 stride = static_cast<OPJ_UINT32>(tilec->x1 - tilec->x0);
  for (OPJ_UINT32 resno = 1; resno < numres; ++resno) {
    const opj_tcd_resolution_t& pres = tilec->resolutions[resno - 1];
    const opj_tcd_resolution_t& res = tilec->resolutions[resno];
    const OPJ_UINT32 sn_x = static_cast<OPJ_UINT32>(pres.x1 - pres.x0);
    const OPJ_UINT32 sn_y = static_cast<OPJ_UINT32>(pres.y1 - pres.y0);
    const OPJ_UINT32 rw = static_cast<OPJ_UINT32>(res.x1 - res.x0);
    const OPJ_UINT32 rh = static_cast<OPJ_UINT32>(res.y1 - res.y0);

    if (rw > 1) {
      for (OPJ_UINT32 y = 0; y < rh; ++y) {
        for (OPJ_UINT32 x = sn_x; x < rw; ++x) {
          const size_t idx = static_cast<size_t>(y) * stride + x;
          const OPJ_FLOAT32 val = std::bit_cast<OPJ_FLOAT32>(tilec->data[idx]);
          tilec->data[idx] =
              std::bit_cast<OPJ_INT32>(val * kDirect97HighPassScale);
        }
      }
    }
    if (rh > 1) {
      for (OPJ_UINT32 y = sn_y; y < rh; ++y) {
        for (OPJ_UINT32 x = 0; x < rw; ++x) {
          const size_t idx = static_cast<size_t>(y) * stride + x;
          const OPJ_FLOAT32 val = std::bit_cast<OPJ_FLOAT32>(tilec->data[idx]);
          tilec->data[idx] =
              std::bit_cast<OPJ_INT32>(val * kDirect97HighPassScale);
        }
      }
    }
  }
}

void VerifyDirect53RoundTrip(opj_thread_pool_t* tp, OPJ_INT32 x0, OPJ_INT32 y0,
                             OPJ_UINT32 width, OPJ_UINT32 height,
                             OPJ_UINT32 numres) {
  DirectTileComp dtc;
  ASSERT_NO_FATAL_FAILURE(dtc.Init(tp, x0, y0, width, height, numres));

  const std::vector<OPJ_INT32> orig =
      MakeDeterministicIntSamples(width, height);
  std::copy(orig.begin(), orig.end(), dtc.tilec.data);

  ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&dtc.tcd, &dtc.tilec));
  ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&dtc.tcd, &dtc.tilec, numres));

  EXPECT_THAT(std::span<const OPJ_INT32>(dtc.tilec.data, orig.size()),
              ElementsAreArray(orig));
}

void VerifyDirect97RoundTrip(opj_thread_pool_t* tp, OPJ_INT32 x0, OPJ_INT32 y0,
                             OPJ_UINT32 width, OPJ_UINT32 height,
                             OPJ_UINT32 numres) {
  DirectTileComp dtc;
  ASSERT_NO_FATAL_FAILURE(dtc.Init(tp, x0, y0, width, height, numres));

  const std::vector<OPJ_FLOAT32> orig =
      MakeDeterministicFloatSamples(width, height);
  LoadFloatSamples(orig, dtc.tilec.data);

  ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&dtc.tcd, &dtc.tilec));
  ScaleEncoded97HighPassForDirectDecode(&dtc.tilec, numres);
  ASSERT_EQ(OPJ_TRUE, opj_dwt_decode_real(&dtc.tcd, &dtc.tilec, numres));

  const std::vector<OPJ_FLOAT32> decoded =
      ExtractFloatSamples(dtc.tilec.data, orig.size());
  for (size_t i = 0; i < orig.size(); ++i) {
    EXPECT_NEAR(orig[i], decoded[i], kFloatRoundTripTol) << "index=" << i;
  }
}

struct PartialWindow {
  std::string_view name;
  OPJ_UINT32 x0;
  OPJ_UINT32 y0;
  OPJ_UINT32 x1;
  OPJ_UINT32 y1;
};

struct DecoderTcdContext {
  opj_image_cmptparm_t cmpt_param{};
  OpjImagePtr image;
  opj_tccp_t tccp{};
  opj_tcp_t tcp{};
  opj_cp_t cp{};
  opj_event_mgr_t event_mgr{};
  OpjTcdPtr tcd;

  void Init(opj_thread_pool_t* tp, OPJ_UINT32 x0, OPJ_UINT32 y0,
            OPJ_UINT32 width, OPJ_UINT32 height, OPJ_UINT32 numres,
            OPJ_UINT32 qmfbid, OPJ_UINT32 cblkw_exp = 4,
            OPJ_UINT32 cblkh_exp = 4, OPJ_UINT32 prcw_exp = 5,
            OPJ_UINT32 prch_exp = 5) {
    tcd.reset();
    cp = opj_cp_t{};
    tcp = opj_tcp_t{};
    tccp = opj_tccp_t{};
    event_mgr = opj_event_mgr_t{};

    cmpt_param = opj_image_cmptparm_t{};
    cmpt_param.dx = 1;
    cmpt_param.dy = 1;
    cmpt_param.w = width;
    cmpt_param.h = height;
    cmpt_param.x0 = x0;
    cmpt_param.y0 = y0;
    cmpt_param.prec = 16;
    cmpt_param.sgnd = 1;
    image.reset(opj_image_tile_create(1, &cmpt_param, OPJ_CLRSPC_GRAY));
    ASSERT_NE(nullptr, image);
    image->x0 = x0;
    image->y0 = y0;
    image->x1 = x0 + width;
    image->y1 = y0 + height;

    tccp.numresolutions = numres;
    tccp.qmfbid = qmfbid;
    tccp.cblkw = cblkw_exp;
    tccp.cblkh = cblkh_exp;
    tccp.qntsty = (qmfbid == 1) ? J2K_CCP_QNTSTY_NOQNT : J2K_CCP_QNTSTY_SEQNT;
    tccp.numgbits = 2;
    for (OPJ_UINT32 r = 0; r < numres; ++r) {
      tccp.prcw[r] = prcw_exp;
      tccp.prch[r] = prch_exp;
    }
    opj_dwt_calc_explicit_stepsizes(&tccp, cmpt_param.prec);

    tcp.tccps = &tccp;
    cp.m_is_decoder = 1;
    cp.tx0 = 0;
    cp.ty0 = 0;
    cp.tdx = image->x1;
    cp.tdy = image->y1;
    cp.tw = 1;
    cp.th = 1;
    cp.tcps = &tcp;

    tcd.reset(opj_tcd_create(OPJ_TRUE));
    ASSERT_NE(nullptr, tcd);
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init(tcd.get(), image.get(), &cp, tp));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(tcd.get(), 0, &event_mgr));
    tcd->tcp = &cp.tcps[0];
  }
};

// Copies wavelet coefficients from `enc_data` (stride `enc_stride`) into the
// decoder codeblock `decoded_data` buffers of `dec_ctx->tcd`. When
// `use_aoi_filter` is true, codeblocks outside the window's area of interest
// are left with `decoded_data == nullptr` to exercise sparse block skipping.
void PopulateDecoderCodeblocks(DecoderTcdContext* dec_ctx,
                               const OPJ_INT32* enc_data, OPJ_UINT32 enc_stride,
                               OPJ_UINT32 numres, bool use_aoi_filter) {
  opj_tcd_t* tcd = dec_ctx->tcd.get();
  opj_tcd_tilecomp_t* tilec = &tcd->tcd_image->tiles->comps[0];

  for (OPJ_UINT32 resno = 0; resno < numres; ++resno) {
    opj_tcd_resolution_t* res = &tilec->resolutions[resno];
    for (OPJ_UINT32 bandno = 0; bandno < res->numbands; ++bandno) {
      opj_tcd_band_t* band = &res->bands[bandno];
      if (opj_tcd_is_band_empty(band)) {
        continue;
      }
      for (OPJ_UINT32 precno = 0; precno < res->pw * res->ph; ++precno) {
        opj_tcd_precinct_t* prec = &band->precincts[precno];
        for (OPJ_UINT32 cblkno = 0; cblkno < prec->cw * prec->ch; ++cblkno) {
          opj_tcd_cblk_dec_t* cblk = &prec->cblks.dec[cblkno];
          const OPJ_UINT32 cblk_w =
              static_cast<OPJ_UINT32>(cblk->x1 - cblk->x0);
          const OPJ_UINT32 cblk_h =
              static_cast<OPJ_UINT32>(cblk->y1 - cblk->y0);
          if (cblk_w == 0 || cblk_h == 0) {
            continue;
          }
          if (use_aoi_filter && !opj_tcd_is_subband_area_of_interest(
                                    tcd, 0, resno, band->bandno,
                                    static_cast<OPJ_UINT32>(cblk->x0),
                                    static_cast<OPJ_UINT32>(cblk->y0),
                                    static_cast<OPJ_UINT32>(cblk->x1),
                                    static_cast<OPJ_UINT32>(cblk->y1))) {
            continue;
          }

          cblk->decoded_data = static_cast<OPJ_INT32*>(opj_aligned_malloc(
              static_cast<size_t>(cblk_w) * cblk_h * sizeof(OPJ_INT32)));
          ASSERT_NE(nullptr, cblk->decoded_data);

          OPJ_UINT32 base_x = static_cast<OPJ_UINT32>(cblk->x0 - band->x0);
          OPJ_UINT32 base_y = static_cast<OPJ_UINT32>(cblk->y0 - band->y0);
          if (band->bandno & 1) {
            const opj_tcd_resolution_t& pres = tilec->resolutions[resno - 1];
            base_x += static_cast<OPJ_UINT32>(pres.x1 - pres.x0);
          }
          if (band->bandno & 2) {
            const opj_tcd_resolution_t& pres = tilec->resolutions[resno - 1];
            base_y += static_cast<OPJ_UINT32>(pres.y1 - pres.y0);
          }
          for (OPJ_UINT32 r = 0; r < cblk_h; ++r) {
            const OPJ_INT32* src_row =
                enc_data + static_cast<size_t>(base_y + r) * enc_stride +
                base_x;
            OPJ_INT32* dst_row =
                cblk->decoded_data + static_cast<size_t>(r) * cblk_w;
            std::copy_n(src_row, cblk_w, dst_row);
          }
        }
      }
    }
  }
}

void ConfigurePartialWindow(opj_tcd_t* tcd, opj_tcd_tilecomp_t* tilec,
                            const PartialWindow& win, OPJ_INT32 sentinel) {
  tcd->whole_tile_decoding = OPJ_FALSE;
  tcd->win_x0 = win.x0;
  tcd->win_y0 = win.y0;
  tcd->win_x1 = win.x1;
  tcd->win_y1 = win.y1;

  tilec->win_x0 = win.x0;
  tilec->win_y0 = win.y0;
  tilec->win_x1 = win.x1;
  tilec->win_y1 = win.y1;

  for (OPJ_UINT32 resno = 0; resno < tilec->numresolutions; ++resno) {
    opj_tcd_resolution_t& res = tilec->resolutions[resno];
    const OPJ_UINT32 shift = tilec->numresolutions - 1U - resno;
    res.win_x0 = opj_uint_ceildivpow2(tilec->win_x0, shift);
    res.win_y0 = opj_uint_ceildivpow2(tilec->win_y0, shift);
    res.win_x1 = opj_uint_ceildivpow2(tilec->win_x1, shift);
    res.win_y1 = opj_uint_ceildivpow2(tilec->win_y1, shift);
  }

  const opj_tcd_resolution_t& tr_max =
      tilec->resolutions[tilec->numresolutions - 1];
  const size_t win_w = static_cast<size_t>(tr_max.win_x1 - tr_max.win_x0);
  const size_t win_h = static_cast<size_t>(tr_max.win_y1 - tr_max.win_y0);
  const size_t count = win_w * win_h;
  opj_image_data_free(tilec->data_win);
  tilec->data_win = nullptr;
  if (count > 0) {
    tilec->data_win = static_cast<OPJ_INT32*>(
        opj_image_data_alloc(count * sizeof(OPJ_INT32)));
    ASSERT_NE(nullptr, tilec->data_win);
    std::fill_n(tilec->data_win, count, sentinel);
  }
}

void VerifyPartial53Window(opj_thread_pool_t* tp, OPJ_UINT32 x0, OPJ_UINT32 y0,
                           OPJ_UINT32 width, OPJ_UINT32 height,
                           OPJ_UINT32 numres, const PartialWindow& win,
                           bool use_aoi_filter = true) {
  // Upstream opj_dwt_decode_partial_tile() allocates v.mem with
  // opj_aligned_32_malloc and opj_dwt_decode_partial_1_parallel() computes
  // signed shifts/additions across all 4 SIMD lanes even when nb_cols < 4,
  // triggering UBSan signed-integer overflow on non-zero ASan malloc fill
  // bytes unless zeroed. Under MSan, ScopedZeroOpjAligned32Alloc is a no-op
  // because MSan does not report arithmetic on uninitialized poison bits.
  // TODO(schwehr): Remove once upstream zeroes unused SIMD lanes in dwt.c.
  ScopedZeroOpjAligned32Alloc zero_unused_simd_lanes;
  DirectTileComp enc;
  ASSERT_NO_FATAL_FAILURE(enc.Init(tp, static_cast<OPJ_INT32>(x0),
                                   static_cast<OPJ_INT32>(y0), width, height,
                                   numres));
  const std::vector<OPJ_INT32> orig =
      MakeDeterministicIntSamples(width, height);
  std::copy(orig.begin(), orig.end(), enc.tilec.data);
  ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&enc.tcd, &enc.tilec));

  DecoderTcdContext dec;
  ASSERT_NO_FATAL_FAILURE(dec.Init(tp, x0, y0, width, height, numres,
                                   /*qmfbid=*/1));
  opj_tcd_tilecomp_t* dec_tilec = &dec.tcd->tcd_image->tiles->comps[0];
  ASSERT_NO_FATAL_FAILURE(
      ConfigurePartialWindow(dec.tcd.get(), dec_tilec, win, kIntSentinel));
  ASSERT_NO_FATAL_FAILURE(PopulateDecoderCodeblocks(&dec, enc.tilec.data, width,
                                                    numres, use_aoi_filter));

  ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(dec.tcd.get(), dec_tilec, numres));

  const OPJ_UINT32 win_w = win.x1 - win.x0;
  const OPJ_UINT32 win_h = win.y1 - win.y0;
  for (OPJ_UINT32 y = 0; y < win_h; ++y) {
    for (OPJ_UINT32 x = 0; x < win_w; ++x) {
      const size_t src_idx =
          static_cast<size_t>(win.y0 - y0 + y) * width + (win.x0 - x0 + x);
      const size_t dst_idx = static_cast<size_t>(y) * win_w + x;
      EXPECT_EQ(orig[src_idx], dec_tilec->data_win[dst_idx])
          << "win=" << win.name << " x=" << x << " y=" << y;
    }
  }
}

void VerifyPartial97Window(opj_thread_pool_t* tp, OPJ_UINT32 x0, OPJ_UINT32 y0,
                           OPJ_UINT32 width, OPJ_UINT32 height,
                           OPJ_UINT32 numres, const PartialWindow& win,
                           bool use_aoi_filter = false) {
  DirectTileComp enc;
  ASSERT_NO_FATAL_FAILURE(enc.Init(tp, static_cast<OPJ_INT32>(x0),
                                   static_cast<OPJ_INT32>(y0), width, height,
                                   numres));
  const std::vector<OPJ_FLOAT32> orig =
      MakeDeterministicFloatSamples(width, height);
  LoadFloatSamples(orig, enc.tilec.data);
  ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&enc.tcd, &enc.tilec));
  ScaleEncoded97HighPassForDirectDecode(&enc.tilec, numres);

  DecoderTcdContext dec;
  ASSERT_NO_FATAL_FAILURE(dec.Init(tp, x0, y0, width, height, numres,
                                   /*qmfbid=*/0, /*cblkw_exp=*/3,
                                   /*cblkh_exp=*/3));
  opj_tcd_tilecomp_t* dec_tilec = &dec.tcd->tcd_image->tiles->comps[0];
  ASSERT_NO_FATAL_FAILURE(ConfigurePartialWindow(
      dec.tcd.get(), dec_tilec, win, std::bit_cast<OPJ_INT32>(kFloatSentinel)));
  ASSERT_NO_FATAL_FAILURE(PopulateDecoderCodeblocks(&dec, enc.tilec.data, width,
                                                    numres, use_aoi_filter));

  ASSERT_EQ(OPJ_TRUE, opj_dwt_decode_real(dec.tcd.get(), dec_tilec, numres));

  const OPJ_UINT32 win_w = win.x1 - win.x0;
  const OPJ_UINT32 win_h = win.y1 - win.y0;
  const std::vector<OPJ_FLOAT32> actual = ExtractFloatSamples(
      dec_tilec->data_win, static_cast<size_t>(win_w) * win_h);
  for (OPJ_UINT32 y = 0; y < win_h; ++y) {
    for (OPJ_UINT32 x = 0; x < win_w; ++x) {
      const size_t src_idx =
          static_cast<size_t>(win.y0 - y0 + y) * width + (win.x0 - x0 + x);
      const size_t dst_idx = static_cast<size_t>(y) * win_w + x;
      EXPECT_NEAR(orig[src_idx], actual[dst_idx], kFloatRoundTripTol)
          << "win=" << win.name << " x=" << x << " y=" << y;
    }
  }
}

TEST(DwtTest, GetNormReturnsTabulatedConstantsAndClampsLevels) {
  constexpr std::array<std::array<OPJ_FLOAT64, 10>, 4> kExpectedNorms = {{
      {1.000, 1.500, 2.750, 5.375, 10.68, 21.34, 42.67, 85.33, 170.7, 341.3},
      {1.038, 1.592, 2.919, 5.703, 11.33, 22.64, 45.25, 90.48, 180.9, 180.9},
      {1.038, 1.592, 2.919, 5.703, 11.33, 22.64, 45.25, 90.48, 180.9, 180.9},
      {.7186, .9218, 1.586, 3.043, 6.019, 12.01, 24.00, 47.97, 95.93, 95.93},
  }};

  for (OPJ_UINT32 orient = 0; orient < 4; ++orient) {
    for (OPJ_UINT32 level = 0; level <= 12; ++level) {
      const OPJ_UINT32 clamped_idx =
          (orient == 0) ? std::min(level, 9U) : std::min(level, 8U);
      EXPECT_DOUBLE_EQ(kExpectedNorms[orient][clamped_idx],
                       opj_dwt_getnorm(level, orient))
          << "orient=" << orient << " level=" << level;
    }
  }
}

TEST(DwtTest, GetNormRealReturnsTabulatedConstantsAndClampsLevels) {
  constexpr std::array<std::array<OPJ_FLOAT64, 10>, 4> kExpectedNormsReal = {{
      {1.000, 1.965, 4.177, 8.403, 16.90, 33.84, 67.69, 135.3, 270.6, 540.9},
      {2.022, 3.989, 8.355, 17.04, 34.27, 68.63, 137.3, 274.6, 549.0, 549.0},
      {2.022, 3.989, 8.355, 17.04, 34.27, 68.63, 137.3, 274.6, 549.0, 549.0},
      {2.080, 3.865, 8.307, 17.18, 34.71, 69.59, 139.3, 278.6, 557.2, 557.2},
  }};

  for (OPJ_UINT32 orient = 0; orient < 4; ++orient) {
    for (OPJ_UINT32 level = 0; level <= 12; ++level) {
      const OPJ_UINT32 clamped_idx =
          (orient == 0) ? std::min(level, 9U) : std::min(level, 8U);
      EXPECT_DOUBLE_EQ(kExpectedNormsReal[orient][clamped_idx],
                       opj_dwt_getnorm_real(level, orient))
          << "orient=" << orient << " level=" << level;
    }
  }
}

TEST(DwtTest, CalcExplicitStepsizesHandlesNoQntAndQuantizedStyles) {
  // 1. J2K_CCP_QNTSTY_NOQNT (reversible 5-3): stepsize == 1.0 -> mant == 0,
  //    expn == prec + gain (0 for LL, 1 for HL/LH, 2 for HH).
  {
    opj_tccp_t tccp{};
    tccp.numresolutions = 4;
    tccp.qmfbid = 1;
    tccp.qntsty = J2K_CCP_QNTSTY_NOQNT;
    opj_dwt_calc_explicit_stepsizes(&tccp, /*prec=*/8);

    constexpr OPJ_UINT32 kNumBands = 3 * 4 - 2;
    for (OPJ_UINT32 b = 0; b < kNumBands; ++b) {
      const OPJ_UINT32 orient = (b == 0) ? 0U : ((b - 1U) % 3U + 1U);
      const OPJ_INT32 gain =
          (orient == 0) ? 0 : ((orient == 1 || orient == 2) ? 1 : 2);
      EXPECT_EQ(0, tccp.stepsizes[b].mant) << "band=" << b;
      EXPECT_EQ(8 + gain, tccp.stepsizes[b].expn) << "band=" << b;
    }
  }

  // 2. J2K_CCP_QNTSTY_SEQNT with qmfbid == 0 (9-7 irreversible): gain == 0
  //    for all bands; 12 resolutions spans both n < 0 and n >= 0 in
  //    opj_dwt_encode_stepsize as well as level clamping in getnorm_real.
  {
    opj_tccp_t tccp{};
    tccp.numresolutions = 12;
    tccp.qmfbid = 0;
    tccp.qntsty = J2K_CCP_QNTSTY_SEQNT;
    opj_dwt_calc_explicit_stepsizes(&tccp, /*prec=*/10);

    const OPJ_UINT32 numbands = 3 * tccp.numresolutions - 2;
    for (OPJ_UINT32 b = 0; b < numbands; ++b) {
      const OPJ_UINT32 resno = (b == 0) ? 0U : ((b - 1U) / 3U + 1U);
      const OPJ_UINT32 orient = (b == 0) ? 0U : ((b - 1U) % 3U + 1U);
      const OPJ_UINT32 level = tccp.numresolutions - 1U - resno;
      const OPJ_FLOAT64 norm = opj_dwt_getnorm_real(level, orient);
      const OPJ_INT32 raw_step =
          static_cast<OPJ_INT32>(std::floor((1.0 / norm) * 8192.0));
      const OPJ_INT32 flog2 = opj_int_floorlog2(raw_step);
      const OPJ_INT32 p = flog2 - 13;
      const OPJ_INT32 n = 11 - flog2;
      const OPJ_INT32 expected_mant =
          (n < 0 ? raw_step >> -n : raw_step << n) & 0x7ff;
      EXPECT_EQ(expected_mant, tccp.stepsizes[b].mant) << "band=" << b;
      EXPECT_EQ(10 - p, tccp.stepsizes[b].expn) << "band=" << b;
    }
  }

  // 3. J2K_CCP_QNTSTY_SIQNT with qmfbid == 1: includes subband gains 0, 1, 2.
  {
    opj_tccp_t tccp{};
    tccp.numresolutions = 3;
    tccp.qmfbid = 1;
    tccp.qntsty = J2K_CCP_QNTSTY_SIQNT;
    opj_dwt_calc_explicit_stepsizes(&tccp, /*prec=*/12);

    // At resno = 2 (level = 0): orient=1 (HL) has gain=1, norm=2.022 ->
    // raw_step = floor((2 / 2.022) * 8192) = 8102.
    const OPJ_UINT32 hl_top_band = 3 * 2 - 2;  // band 4
    const OPJ_INT32 raw_step =
        static_cast<OPJ_INT32>(std::floor((2.0 / 2.022) * 8192.0));
    const OPJ_INT32 flog2 = opj_int_floorlog2(raw_step);
    EXPECT_EQ((raw_step >> (flog2 - 11)) & 0x7ff,
              tccp.stepsizes[hl_top_band].mant);
    EXPECT_EQ(12 + 1 - (flog2 - 13), tccp.stepsizes[hl_top_band].expn);
  }
}

TEST(DwtTest, ReversibleKnownVectorsForwardAndInverseExact) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. 1x1 at (0, 0) is a no-op; 1x1 at (1, 1) scales by 2 per axis (x4 total).
  {
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(dtc.Init(tp.get(), 0, 0, 1, 1, 2));
    dtc.tilec.data[0] = 37;
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&dtc.tcd, &dtc.tilec));
    EXPECT_EQ(37, dtc.tilec.data[0]);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&dtc.tcd, &dtc.tilec, 2));
    EXPECT_EQ(37, dtc.tilec.data[0]);
  }
  {
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(dtc.Init(tp.get(), 1, 1, 1, 1, 2));
    dtc.tilec.data[0] = -19;
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&dtc.tcd, &dtc.tilec));
    EXPECT_EQ(-76, dtc.tilec.data[0]);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&dtc.tcd, &dtc.tilec, 2));
    EXPECT_EQ(-19, dtc.tilec.data[0]);
  }

  // 2. 1D horizontal and vertical exact 5-3 lifting values for cas=0 and cas=1.
  struct Known1DCase {
    std::string_view name;
    OPJ_INT32 x0;
    OPJ_INT32 y0;
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    std::vector<OPJ_INT32> input;
    std::vector<OPJ_INT32> expected_encoded;
  };

  const std::vector<Known1DCase> cases = {
      {.name = "h_4_cas0",
       .x0 = 0,
       .y0 = 0,
       .width = 4,
       .height = 1,
       .input = {10, 20, 30, 40},
       .expected_encoded = {10, 33, 0, 10}},
      {.name = "h_4_cas1",
       .x0 = 1,
       .y0 = 0,
       .width = 4,
       .height = 1,
       .input = {10, 20, 30, 40},
       .expected_encoded = {18, 40, -10, 0}},
      {.name = "h_3_cas0",
       .x0 = 0,
       .y0 = 0,
       .width = 3,
       .height = 1,
       .input = {10, 20, 30},
       .expected_encoded = {10, 30, 0}},
      {.name = "h_3_cas1",
       .x0 = 1,
       .y0 = 0,
       .width = 3,
       .height = 1,
       .input = {10, 20, 30},
       .expected_encoded = {20, -10, 10}},
      {.name = "v_4_cas0",
       .x0 = 0,
       .y0 = 0,
       .width = 1,
       .height = 4,
       .input = {10, 20, 30, 40},
       .expected_encoded = {10, 33, 0, 10}},
      {.name = "v_4_cas1",
       .x0 = 0,
       .y0 = 1,
       .width = 1,
       .height = 4,
       .input = {10, 20, 30, 40},
       .expected_encoded = {18, 40, -10, 0}},
      {.name = "v_3_cas0",
       .x0 = 0,
       .y0 = 0,
       .width = 1,
       .height = 3,
       .input = {10, 20, 30},
       .expected_encoded = {10, 30, 0}},
      {.name = "v_3_cas1",
       .x0 = 0,
       .y0 = 1,
       .width = 1,
       .height = 3,
       .input = {10, 20, 30},
       .expected_encoded = {20, -10, 10}},
  };

  for (const auto& tc : cases) {
    SCOPED_TRACE(std::string(tc.name));
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(
        dtc.Init(tp.get(), tc.x0, tc.y0, tc.width, tc.height, 2));
    std::copy(tc.input.begin(), tc.input.end(), dtc.tilec.data);

    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&dtc.tcd, &dtc.tilec));
    EXPECT_THAT(
        std::span<const OPJ_INT32>(dtc.tilec.data, tc.expected_encoded.size()),
        ElementsAreArray(tc.expected_encoded));

    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&dtc.tcd, &dtc.tilec, 2));
    EXPECT_THAT(std::span<const OPJ_INT32>(dtc.tilec.data, tc.input.size()),
                ElementsAreArray(tc.input));
  }
}

TEST(DwtTest, ReversibleEarlyReturnNumResOneAndZeroWidth) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. numres == 1 returns OPJ_TRUE without modifying samples.
  {
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(dtc.Init(tp.get(), 0, 0, 8, 8, 1));
    const std::vector<OPJ_INT32> orig = MakeDeterministicIntSamples(8, 8);
    std::copy(orig.begin(), orig.end(), dtc.tilec.data);

    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&dtc.tcd, &dtc.tilec));
    EXPECT_THAT(std::span<const OPJ_INT32>(dtc.tilec.data, orig.size()),
                ElementsAreArray(orig));

    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&dtc.tcd, &dtc.tilec, 1));
    EXPECT_THAT(std::span<const OPJ_INT32>(dtc.tilec.data, orig.size()),
                ElementsAreArray(orig));
  }

  // 2. w == 0 at minimum_num_resolutions triggers early OPJ_TRUE exit.
  {
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(dtc.Init(tp.get(), 0, 0, 0, 4, 2));
    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&dtc.tcd, &dtc.tilec, 2));
  }
}

TEST(DwtTest, ReversibleAllGeometriesAndOriginParitiesRoundTrip) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // Single-level geometries exercise every width/height parity, cols 1..8
  // switch cases in opj_dwt_deinterleave_v_cols, and SIMD + tail vertical
  // inverse 5-3 loops for both cas=0 and cas=1.
  constexpr auto kSingleLevelCases = std::to_array<GeometryCase>({
      {.name = "1x1", .width = 1, .height = 1, .numres = 2},
      {.name = "1x2", .width = 1, .height = 2, .numres = 2},
      {.name = "2x1", .width = 2, .height = 1, .numres = 2},
      {.name = "2x2", .width = 2, .height = 2, .numres = 2},
      {.name = "1x7", .width = 1, .height = 7, .numres = 2},
      {.name = "7x1", .width = 7, .height = 1, .numres = 2},
      {.name = "3x3", .width = 3, .height = 3, .numres = 2},
      {.name = "4x4", .width = 4, .height = 4, .numres = 2},
      {.name = "5x7", .width = 5, .height = 7, .numres = 2},
      {.name = "6x5", .width = 6, .height = 5, .numres = 2},
      {.name = "7x6", .width = 7, .height = 6, .numres = 2},
      {.name = "8x2", .width = 8, .height = 2, .numres = 2},
      {.name = "9x2", .width = 9, .height = 2, .numres = 2},
      {.name = "9x1", .width = 9, .height = 1, .numres = 2},
      {.name = "12x3", .width = 12, .height = 3, .numres = 2},
      {.name = "12x4", .width = 12, .height = 4, .numres = 2},
      {.name = "12x5", .width = 12, .height = 5, .numres = 2},
      {.name = "12x6", .width = 12, .height = 6, .numres = 2},
  });

  for (const auto& geom : kSingleLevelCases) {
    for (const auto& origin : kAllParities) {
      SCOPED_TRACE(::testing::Message()
                   << geom.name << " origin=" << origin.name);
      ASSERT_NO_FATAL_FAILURE(
          VerifyDirect53RoundTrip(tp.get(), origin.x0, origin.y0, geom.width,
                                  geom.height, geom.numres));
    }
  }

  // Multi-resolution geometries exercise >= 8, >= 16, and >= 32 column SIMD
  // loops plus scalar tails across mixed cas=0/cas=1 resolution levels.
  constexpr auto kMultiResCases = std::to_array<GeometryCase>({
      {.name = "13x15", .width = 13, .height = 15, .numres = 3},
      {.name = "36x36", .width = 36, .height = 36, .numres = 4},
      {.name = "37x35", .width = 37, .height = 35, .numres = 4},
      {.name = "72x40", .width = 72, .height = 40, .numres = 4},
      {.name = "128x64", .width = 128, .height = 64, .numres = 4},
  });

  for (const auto& geom : kMultiResCases) {
    for (const auto& origin : kMultiLevelParities) {
      SCOPED_TRACE(::testing::Message()
                   << geom.name << " origin=" << origin.name);
      ASSERT_NO_FATAL_FAILURE(
          VerifyDirect53RoundTrip(tp.get(), origin.x0, origin.y0, geom.width,
                                  geom.height, geom.numres));
    }
  }
}

TEST(DwtTest, ReversibleMultiThreadedMatchesSingleThreaded) {
  OpjThreadPoolPtr tp_single(opj_thread_pool_create(0));
  OpjThreadPoolPtr tp_two(opj_thread_pool_create(2));
  OpjThreadPoolPtr tp_four(opj_thread_pool_create(4));
  ASSERT_NE(nullptr, tp_single);
  ASSERT_NE(nullptr, tp_two);
  ASSERT_NE(nullptr, tp_four);

  struct MtCase {
    std::string_view name;
    OPJ_INT32 x0;
    OPJ_INT32 y0;
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    OPJ_UINT32 numres;
    int threads;
  };
  constexpr auto kMtCases = std::to_array<MtCase>({
      // 3x2 with 4 threads hits rh < num_jobs (encode/decode H) and
      // rw < num_jobs (decode V).
      {.name = "3x2_t4_even",
       .x0 = 0,
       .y0 = 0,
       .width = 3,
       .height = 2,
       .numres = 2,
       .threads = 4},
      {.name = "3x2_t4_odd",
       .x0 = 1,
       .y0 = 1,
       .width = 3,
       .height = 2,
       .numres = 2,
       .threads = 4},
      // 37x35 exercises multi-threaded encode V + decode V tail columns.
      {.name = "37x35_t2_even",
       .x0 = 0,
       .y0 = 0,
       .width = 37,
       .height = 35,
       .numres = 4,
       .threads = 2},
      {.name = "37x35_t4_mixed",
       .x0 = 3,
       .y0 = 5,
       .width = 37,
       .height = 35,
       .numres = 4,
       .threads = 4},
      {.name = "72x40_t2_odd",
       .x0 = 1,
       .y0 = 1,
       .width = 72,
       .height = 40,
       .numres = 4,
       .threads = 2},
      {.name = "128x64_t4_even",
       .x0 = 0,
       .y0 = 0,
       .width = 128,
       .height = 64,
       .numres = 4,
       .threads = 4},
  });

  for (const auto& tc : kMtCases) {
    SCOPED_TRACE(std::string(tc.name));
    opj_thread_pool_t* tp_mt = (tc.threads == 2) ? tp_two.get() : tp_four.get();
    const std::vector<OPJ_INT32> orig =
        MakeDeterministicIntSamples(tc.width, tc.height);

    DirectTileComp st;
    ASSERT_NO_FATAL_FAILURE(
        st.Init(tp_single.get(), tc.x0, tc.y0, tc.width, tc.height, tc.numres));
    std::copy(orig.begin(), orig.end(), st.tilec.data);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&st.tcd, &st.tilec));

    DirectTileComp mt;
    ASSERT_NO_FATAL_FAILURE(
        mt.Init(tp_mt, tc.x0, tc.y0, tc.width, tc.height, tc.numres));
    std::copy(orig.begin(), orig.end(), mt.tilec.data);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode(&mt.tcd, &mt.tilec));

    EXPECT_THAT(std::span<const OPJ_INT32>(mt.tilec.data, orig.size()),
                ElementsAreArray(st.tilec.data, orig.size()));

    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode(&mt.tcd, &mt.tilec, tc.numres));
    EXPECT_THAT(std::span<const OPJ_INT32>(mt.tilec.data, orig.size()),
                ElementsAreArray(orig));
  }
}

TEST(DwtTest, IrreversibleKnownVectorsAndConstantSignal) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. 1x1 at both (0, 0) and (1, 1) leaves the single float sample unchanged.
  for (const auto& origin : kAllParities) {
    SCOPED_TRACE(origin.name);
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(dtc.Init(tp.get(), origin.x0, origin.y0, 1, 1, 2));
    constexpr OPJ_FLOAT32 kVal = -42.5f;
    dtc.tilec.data[0] = std::bit_cast<OPJ_INT32>(kVal);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&dtc.tcd, &dtc.tilec));
    EXPECT_FLOAT_EQ(kVal, std::bit_cast<OPJ_FLOAT32>(dtc.tilec.data[0]));
    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode_real(&dtc.tcd, &dtc.tilec, 2));
    EXPECT_FLOAT_EQ(kVal, std::bit_cast<OPJ_FLOAT32>(dtc.tilec.data[0]));
  }

  // 2. Constant signal produces ~0 in every HL/LH/HH subband across cas=0/1.
  for (const auto& origin : kAllParities) {
    SCOPED_TRACE(origin.name);
    constexpr OPJ_UINT32 kSize = 8;
    constexpr OPJ_UINT32 kNumRes = 3;
    constexpr OPJ_FLOAT32 kConst = 123.5f;
    DirectTileComp dtc;
    ASSERT_NO_FATAL_FAILURE(
        dtc.Init(tp.get(), origin.x0, origin.y0, kSize, kSize, kNumRes));
    std::vector<OPJ_FLOAT32> input(kSize * kSize, kConst);
    LoadFloatSamples(input, dtc.tilec.data);

    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&dtc.tcd, &dtc.tilec));
    const OPJ_UINT32 ll0_w =
        static_cast<OPJ_UINT32>(dtc.resolutions[0].x1 - dtc.resolutions[0].x0);
    const OPJ_UINT32 ll0_h =
        static_cast<OPJ_UINT32>(dtc.resolutions[0].y1 - dtc.resolutions[0].y0);
    for (OPJ_UINT32 y = 0; y < kSize; ++y) {
      for (OPJ_UINT32 x = 0; x < kSize; ++x) {
        if (x >= ll0_w || y >= ll0_h) {
          const OPJ_FLOAT32 coeff = std::bit_cast<OPJ_FLOAT32>(
              dtc.tilec.data[static_cast<size_t>(y) * kSize + x]);
          EXPECT_NEAR(0.0f, coeff, kFloatStrictTol) << "x=" << x << " y=" << y;
        }
      }
    }

    ScaleEncoded97HighPassForDirectDecode(&dtc.tilec, kNumRes);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode_real(&dtc.tcd, &dtc.tilec, kNumRes));
    const std::vector<OPJ_FLOAT32> decoded =
        ExtractFloatSamples(dtc.tilec.data, input.size());
    EXPECT_THAT(decoded,
                Each(::testing::FloatNear(kConst, kFloatRoundTripTol)));
  }
}

TEST(DwtTest, IrreversibleEarlyReturnNumResOne) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  DirectTileComp dtc;
  ASSERT_NO_FATAL_FAILURE(dtc.Init(tp.get(), 0, 0, 8, 8, 1));
  const std::vector<OPJ_FLOAT32> orig = MakeDeterministicFloatSamples(8, 8);
  LoadFloatSamples(orig, dtc.tilec.data);

  ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&dtc.tcd, &dtc.tilec));
  EXPECT_THAT(ExtractFloatSamples(dtc.tilec.data, orig.size()),
              ElementsAreArray(orig));

  ASSERT_EQ(OPJ_TRUE, opj_dwt_decode_real(&dtc.tcd, &dtc.tilec, 1));
  EXPECT_THAT(ExtractFloatSamples(dtc.tilec.data, orig.size()),
              ElementsAreArray(orig));
}

TEST(DwtTest, IrreversibleAllGeometriesAndOriginParitiesRoundTrip) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // Heights 9..16 cover every remaining_height % 8 branch (1..7 + fast path 8)
  // in opj_v8dwt_interleave_h; 13x15 (odd sn=7) triggers the unaligned slow
  // path for remaining_height >= 8.
  constexpr auto kSingleLevelCases = std::to_array<GeometryCase>({
      {.name = "1x7", .width = 1, .height = 7, .numres = 2},
      {.name = "7x1", .width = 7, .height = 1, .numres = 2},
      {.name = "2x2", .width = 2, .height = 2, .numres = 2},
      {.name = "3x3", .width = 3, .height = 3, .numres = 2},
      {.name = "4x4", .width = 4, .height = 4, .numres = 2},
      {.name = "5x7", .width = 5, .height = 7, .numres = 2},
      {.name = "8x8", .width = 8, .height = 8, .numres = 2},
      {.name = "9x9", .width = 9, .height = 9, .numres = 2},
      {.name = "10x10", .width = 10, .height = 10, .numres = 2},
      {.name = "11x11", .width = 11, .height = 11, .numres = 2},
      {.name = "12x12", .width = 12, .height = 12, .numres = 2},
      {.name = "13x13", .width = 13, .height = 13, .numres = 2},
      {.name = "13x14", .width = 13, .height = 14, .numres = 2},
      {.name = "13x15", .width = 13, .height = 15, .numres = 2},
      {.name = "16x16", .width = 16, .height = 16, .numres = 2},
  });

  for (const auto& geom : kSingleLevelCases) {
    for (const auto& origin : kAllParities) {
      SCOPED_TRACE(::testing::Message()
                   << geom.name << " origin=" << origin.name);
      ASSERT_NO_FATAL_FAILURE(
          VerifyDirect97RoundTrip(tp.get(), origin.x0, origin.y0, geom.width,
                                  geom.height, geom.numres));
    }
  }

  constexpr auto kMultiResCases = std::to_array<GeometryCase>({
      {.name = "13x15", .width = 13, .height = 15, .numres = 3},
      {.name = "36x36", .width = 36, .height = 36, .numres = 4},
      {.name = "37x35", .width = 37, .height = 35, .numres = 4},
  });

  for (const auto& geom : kMultiResCases) {
    for (const auto& origin : kMultiLevelParities) {
      SCOPED_TRACE(::testing::Message()
                   << geom.name << " origin=" << origin.name);
      ASSERT_NO_FATAL_FAILURE(
          VerifyDirect97RoundTrip(tp.get(), origin.x0, origin.y0, geom.width,
                                  geom.height, geom.numres));
    }
  }
}

TEST(DwtTest, IrreversibleMultiThreadedMatchesSingleThreaded) {
  OpjThreadPoolPtr tp_single(opj_thread_pool_create(0));
  OpjThreadPoolPtr tp_two(opj_thread_pool_create(2));
  OpjThreadPoolPtr tp_four(opj_thread_pool_create(4));
  OpjThreadPoolPtr tp_six(opj_thread_pool_create(6));
  ASSERT_NE(nullptr, tp_single);
  ASSERT_NE(nullptr, tp_two);
  ASSERT_NE(nullptr, tp_four);
  ASSERT_NE(nullptr, tp_six);

  struct MtCase {
    std::string_view name;
    OPJ_INT32 x0;
    OPJ_INT32 y0;
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    OPJ_UINT32 numres;
    int threads;
  };
  constexpr auto kMtCases = std::to_array<MtCase>({
      // 19x20 with 6 threads triggers (rh / NB_ELTS_V8) < num_jobs,
      // (rw / NB_ELTS_V8) < num_jobs, and non-multiple-of-8 H and V tails.
      {.name = "19x20_t6_even",
       .x0 = 0,
       .y0 = 0,
       .width = 19,
       .height = 20,
       .numres = 2,
       .threads = 6},
      {.name = "19x20_t6_odd",
       .x0 = 1,
       .y0 = 1,
       .width = 19,
       .height = 20,
       .numres = 2,
       .threads = 6},
      {.name = "37x35_t4_mixed",
       .x0 = 3,
       .y0 = 5,
       .width = 37,
       .height = 35,
       .numres = 4,
       .threads = 4},
      {.name = "72x40_t2_even",
       .x0 = 0,
       .y0 = 0,
       .width = 72,
       .height = 40,
       .numres = 4,
       .threads = 2},
      {.name = "128x64_t4_odd",
       .x0 = 1,
       .y0 = 1,
       .width = 128,
       .height = 64,
       .numres = 4,
       .threads = 4},
  });

  for (const auto& tc : kMtCases) {
    SCOPED_TRACE(std::string(tc.name));
    opj_thread_pool_t* tp_mt = (tc.threads == 2)   ? tp_two.get()
                               : (tc.threads == 4) ? tp_four.get()
                                                   : tp_six.get();
    const std::vector<OPJ_FLOAT32> orig =
        MakeDeterministicFloatSamples(tc.width, tc.height);

    DirectTileComp st;
    ASSERT_NO_FATAL_FAILURE(
        st.Init(tp_single.get(), tc.x0, tc.y0, tc.width, tc.height, tc.numres));
    LoadFloatSamples(orig, st.tilec.data);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&st.tcd, &st.tilec));

    DirectTileComp mt;
    ASSERT_NO_FATAL_FAILURE(
        mt.Init(tp_mt, tc.x0, tc.y0, tc.width, tc.height, tc.numres));
    LoadFloatSamples(orig, mt.tilec.data);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_encode_real(&mt.tcd, &mt.tilec));

    const std::vector<OPJ_FLOAT32> st_enc =
        ExtractFloatSamples(st.tilec.data, orig.size());
    const std::vector<OPJ_FLOAT32> mt_enc =
        ExtractFloatSamples(mt.tilec.data, orig.size());
    for (size_t i = 0; i < orig.size(); ++i) {
      EXPECT_NEAR(st_enc[i], mt_enc[i], kFloatStrictTol) << "enc index=" << i;
    }

    ScaleEncoded97HighPassForDirectDecode(&mt.tilec, tc.numres);
    ASSERT_EQ(OPJ_TRUE, opj_dwt_decode_real(&mt.tcd, &mt.tilec, tc.numres));
    const std::vector<OPJ_FLOAT32> decoded =
        ExtractFloatSamples(mt.tilec.data, orig.size());
    for (size_t i = 0; i < orig.size(); ++i) {
      EXPECT_NEAR(orig[i], decoded[i], kFloatRoundTripTol) << "dec index=" << i;
    }
  }
}

TEST(DwtTest, PartialDecodeReversibleHandlesEdgesCasAndWindows) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. Empty tr_max dimensions (x0 == x1 and y0 == y1) return OPJ_TRUE cleanly.
  {
    DirectTileComp zero_w;
    ASSERT_NO_FATAL_FAILURE(zero_w.Init(tp.get(), 0, 0, 0, 4, 2, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE, opj_dwt_decode(&zero_w.tcd, &zero_w.tilec, 2));

    DirectTileComp zero_h;
    ASSERT_NO_FATAL_FAILURE(zero_h.Init(tp.get(), 0, 0, 4, 0, 2, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE, opj_dwt_decode(&zero_h.tcd, &zero_h.tilec, 2));
  }

  // 2. Empty window intersection (win_x0 == win_x1, data_win == nullptr).
  {
    DecoderTcdContext dec;
    ASSERT_NO_FATAL_FAILURE(dec.Init(tp.get(), 0, 0, 16, 16, 3, /*qmfbid=*/1));
    opj_tcd_tilecomp_t* tilec = &dec.tcd->tcd_image->tiles->comps[0];
    ASSERT_NO_FATAL_FAILURE(ConfigurePartialWindow(
        dec.tcd.get(), tilec,
        {.name = "empty", .x0 = 8, .y0 = 8, .x1 = 8, .y1 = 8}, kIntSentinel));
    EXPECT_EQ(nullptr, tilec->data_win);
    EXPECT_EQ(OPJ_TRUE, opj_dwt_decode(dec.tcd.get(), tilec, 3));
  }

  // 3. numres == 1 early sparse read path.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 0, 0, 32, 32, 1,
      {.name = "numres1", .x0 = 5, .y0 = 7, .x1 = 21, .y1 = 19}));

  // 4. 1D edge geometries covering len == 1 for cas=0 and cas=1 in both
  //    opj_dwt_decode_partial_1 and opj_dwt_decode_partial_1_parallel.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 0, 0, 1, 64, 3,
      {.name = "1x64_cas0", .x0 = 0, .y0 = 10, .x1 = 1, .y1 = 30}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 1, 0, 1, 64, 2,
      {.name = "1x64_cas1", .x0 = 1, .y0 = 10, .x1 = 2, .y1 = 30}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 0, 0, 64, 1, 3,
      {.name = "64x1_cas0", .x0 = 10, .y0 = 0, .x1 = 30, .y1 = 1}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 0, 1, 64, 1, 2,
      {.name = "64x1_cas1", .x0 = 10, .y0 = 1, .x1 = 30, .y1 = 2}));

  // 5. Tiny top-left window exercises tcx1/tcy1 <= 2^(nb-1) clamping in
  //    opj_dwt_get_band_coordinates.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 0, 0, 36, 36, 4,
      {.name = "36x36_tiny_tl", .x0 = 0, .y0 = 0, .x1 = 1, .y1 = 1}));

  // 6. 64x64 even-dim windows cover SSE2 + scalar vertical loops, interior
  //    win_tr_x1 < rw zeroing, odd-width vertical tail, and right/bottom edges.
  constexpr PartialWindow kWindows64[] = {
      {.name = "64x64_interior_even", .x0 = 16, .y0 = 16, .x1 = 48, .y1 = 48},
      {.name = "64x64_interior_odd", .x0 = 17, .y0 = 17, .x1 = 45, .y1 = 45},
      {.name = "64x64_bottom_right", .x0 = 40, .y0 = 40, .x1 = 64, .y1 = 64},
  };
  for (const auto& win : kWindows64) {
    SCOPED_TRACE(std::string(win.name));
    ASSERT_NO_FATAL_FAILURE(
        VerifyPartial53Window(tp.get(), 0, 0, 64, 64, 4, win));
  }

  // 7. Odd dimensions (65x65) at both (0, 0) and (1, 1) plus mixed (3, 5).
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 0, 0, 65, 65, 4,
      {.name = "65x65_even_br", .x0 = 41, .y0 = 41, .x1 = 65, .y1 = 65}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 1, 1, 65, 65, 3,
      {.name = "65x65_odd_int", .x0 = 17, .y0 = 17, .x1 = 49, .y1 = 49}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 1, 1, 65, 65, 3,
      {.name = "65x65_odd_br", .x0 = 41, .y0 = 41, .x1 = 66, .y1 = 66}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial53Window(
      tp.get(), 3, 5, 66, 67, 4,
      {.name = "66x67_mixed", .x0 = 20, .y0 = 21, .x1 = 51, .y1 = 53}));
}

TEST(DwtTest, PartialDecodeIrreversibleHandlesEdgesCasAndWindows) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. Empty tr_max dimensions (x0 == x1 and y0 == y1) return OPJ_TRUE cleanly.
  {
    DirectTileComp zero_w;
    ASSERT_NO_FATAL_FAILURE(zero_w.Init(tp.get(), 0, 0, 0, 4, 2, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE, opj_dwt_decode_real(&zero_w.tcd, &zero_w.tilec, 2));

    DirectTileComp zero_h;
    ASSERT_NO_FATAL_FAILURE(zero_h.Init(tp.get(), 0, 0, 4, 0, 2, OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE, opj_dwt_decode_real(&zero_h.tcd, &zero_h.tilec, 2));
  }

  // 2. Empty window intersection (win_x0 == win_x1, data_win == nullptr).
  {
    DecoderTcdContext dec;
    ASSERT_NO_FATAL_FAILURE(dec.Init(tp.get(), 0, 0, 16, 16, 3, /*qmfbid=*/0));
    opj_tcd_tilecomp_t* tilec = &dec.tcd->tcd_image->tiles->comps[0];
    ASSERT_NO_FATAL_FAILURE(ConfigurePartialWindow(
        dec.tcd.get(), tilec,
        {.name = "empty", .x0 = 8, .y0 = 8, .x1 = 8, .y1 = 8},
        std::bit_cast<OPJ_INT32>(kFloatSentinel)));
    EXPECT_EQ(nullptr, tilec->data_win);
    EXPECT_EQ(OPJ_TRUE, opj_dwt_decode_real(dec.tcd.get(), tilec, 3));
  }

  // 3. numres == 1 early sparse read path.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 32, 32, 1,
      {.name = "numres1", .x0 = 5, .y0 = 7, .x1 = 21, .y1 = 19}));

  // 4. 1D cas=1 edge strips (exercise !((sn > 0) || (dn > 1)) and rh == 1
  // tail).
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 1, 0, 1, 64, 2,
      {.name = "1x64_cas1", .x0 = 1, .y0 = 10, .x1 = 2, .y1 = 30}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 1, 64, 1, 2,
      {.name = "64x1_cas1", .x0 = 10, .y0 = 1, .x1 = 30, .y1 = 2}));

  // 5. 13x15 covers < 8 trailing strip overlap in opj_dwt_decode_partial_97.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 13, 15, 2,
      {.name = "13x15_tail", .x0 = 2, .y0 = 2, .x1 = 11, .y1 = 13}));

  // 6. Tiny top-left window exercises tcx1/tcy1 clamping in
  // get_band_coordinates.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 36, 36, 4,
      {.name = "36x36_tiny_tl", .x0 = 0, .y0 = 0, .x1 = 1, .y1 = 1}));

  // 7. 64x64 interior [24, 48) triggers start > 0 in opj_v8dwt_decode_step2
  //    (even after filter_width=4 growth), plus right/bottom edge [40, 64).
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 64, 64, 3,
      {.name = "64x64_interior", .x0 = 24, .y0 = 24, .x1 = 48, .y1 = 48}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 64, 64, 3,
      {.name = "64x64_bottom_right", .x0 = 40, .y0 = 40, .x1 = 64, .y1 = 64}));

  // 8. 65x65 tests tail row skip vs tail row hit, cas=1 odd origin, and
  //    sparse block skipping via opj_tcd_is_subband_area_of_interest.
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 65, 65, 3,
      {.name = "65x65_skip_tail", .x0 = 20, .y0 = 20, .x1 = 40, .y1 = 40},
      /*use_aoi_filter=*/true));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 0, 0, 65, 65, 3,
      {.name = "65x65_hit_tail", .x0 = 41, .y0 = 41, .x1 = 65, .y1 = 65}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 1, 1, 65, 65, 3,
      {.name = "65x65_odd_cas1", .x0 = 17, .y0 = 17, .x1 = 49, .y1 = 49}));
  ASSERT_NO_FATAL_FAILURE(VerifyPartial97Window(
      tp.get(), 3, 5, 66, 67, 4,
      {.name = "66x67_mixed", .x0 = 20, .y0 = 21, .x1 = 51, .y1 = 53}));
}

TEST(DwtTest, J2kRoundTripAndWindowedDecodeReversibleAndIrreversible) {
  struct J2kScenario {
    std::string_view name;
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    OPJ_UINT32 numcomps;
    OPJ_COLOR_SPACE color_space;
    OPJ_UINT32 dx;
    OPJ_UINT32 dy;
    OPJ_UINT32 x0;
    OPJ_UINT32 y0;
    int numres;
    int irreversible;
    bool tiled;
    OPJ_UINT32 tile_w;
    OPJ_UINT32 tile_h;
    int threads;
    PartialWindow window;
  };

  constexpr auto kScenarios = std::to_array<J2kScenario>({
      {.name = "53_numres1_window",
       .width = 32,
       .height = 32,
       .numcomps = 1,
       .color_space = OPJ_CLRSPC_GRAY,
       .dx = 1,
       .dy = 1,
       .x0 = 0,
       .y0 = 0,
       .numres = 1,
       .irreversible = 0,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 0,
       .window = {.name = "sub", .x0 = 6, .y0 = 8, .x1 = 22, .y1 = 24}},
      {.name = "97_numres1_window",
       .width = 32,
       .height = 32,
       .numcomps = 1,
       .color_space = OPJ_CLRSPC_GRAY,
       .dx = 1,
       .dy = 1,
       .x0 = 0,
       .y0 = 0,
       .numres = 1,
       .irreversible = 1,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 0,
       .window = {.name = "sub", .x0 = 6, .y0 = 8, .x1 = 22, .y1 = 24}},
      {.name = "53_odd_origin_65x65",
       .width = 65,
       .height = 65,
       .numcomps = 1,
       .color_space = OPJ_CLRSPC_GRAY,
       .dx = 1,
       .dy = 1,
       .x0 = 1,
       .y0 = 1,
       .numres = 3,
       .irreversible = 0,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 0,
       .window = {.name = "sub", .x0 = 17, .y0 = 19, .x1 = 51, .y1 = 55}},
      {.name = "97_odd_origin_65x65",
       .width = 65,
       .height = 65,
       .numcomps = 1,
       .color_space = OPJ_CLRSPC_GRAY,
       .dx = 1,
       .dy = 1,
       .x0 = 1,
       .y0 = 1,
       .numres = 3,
       .irreversible = 1,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 0,
       .window = {.name = "sub", .x0 = 17, .y0 = 19, .x1 = 51, .y1 = 55}},
      {.name = "53_tiled_mct_64x64",
       .width = 64,
       .height = 64,
       .numcomps = 3,
       .color_space = OPJ_CLRSPC_SRGB,
       .dx = 1,
       .dy = 1,
       .x0 = 0,
       .y0 = 0,
       .numres = 3,
       .irreversible = 0,
       .tiled = true,
       .tile_w = 32,
       .tile_h = 32,
       .threads = 0,
       .window =
           {.name = "cross_tile", .x0 = 16, .y0 = 16, .x1 = 48, .y1 = 48}},
      {.name = "97_tiled_mct_64x64",
       .width = 64,
       .height = 64,
       .numcomps = 3,
       .color_space = OPJ_CLRSPC_SRGB,
       .dx = 1,
       .dy = 1,
       .x0 = 0,
       .y0 = 0,
       .numres = 3,
       .irreversible = 1,
       .tiled = true,
       .tile_w = 32,
       .tile_h = 32,
       .threads = 0,
       .window =
           {.name = "cross_tile", .x0 = 16, .y0 = 16, .x1 = 48, .y1 = 48}},
      {.name = "53_subsampled_72x40",
       .width = 72,
       .height = 40,
       .numcomps = 3,
       .color_space = OPJ_CLRSPC_SYCC,
       .dx = 2,
       .dy = 2,
       .x0 = 0,
       .y0 = 0,
       .numres = 4,
       .irreversible = 0,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 0,
       .window = {.name = "sub", .x0 = 16, .y0 = 10, .x1 = 54, .y1 = 32}},
      {.name = "53_mt_128x64",
       .width = 128,
       .height = 64,
       .numcomps = 1,
       .color_space = OPJ_CLRSPC_GRAY,
       .dx = 1,
       .dy = 1,
       .x0 = 0,
       .y0 = 0,
       .numres = 4,
       .irreversible = 0,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 2,
       .window = {.name = "sub", .x0 = 24, .y0 = 16, .x1 = 96, .y1 = 48}},
      {.name = "97_mt_128x64",
       .width = 128,
       .height = 64,
       .numcomps = 1,
       .color_space = OPJ_CLRSPC_GRAY,
       .dx = 1,
       .dy = 1,
       .x0 = 0,
       .y0 = 0,
       .numres = 4,
       .irreversible = 1,
       .tiled = false,
       .tile_w = 0,
       .tile_h = 0,
       .threads = 2,
       .window = {.name = "sub", .x0 = 24, .y0 = 16, .x1 = 96, .y1 = 48}},
  });

  for (const auto& sc : kScenarios) {
    SCOPED_TRACE(std::string(sc.name));

    // 1. Encode synthetic test image via J2K. CompressImageToBytes() clones
    //    `src` internally so opj_encode() (l_reuse_data) does not consume it.
    OpjImagePtr src =
        MakeSmoothJ2kTestImage(sc.width, sc.height, sc.numcomps, sc.color_space,
                               sc.dx, sc.dy, sc.x0, sc.y0);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected_pixels =
        SnapshotImagePixels(src.get());
    std::vector<OPJ_UINT32> expected_w(sc.numcomps);
    std::vector<OPJ_UINT32> expected_h(sc.numcomps);
    for (OPJ_UINT32 c = 0; c < sc.numcomps; ++c) {
      expected_w[c] = src->comps[c].w;
      expected_h[c] = src->comps[c].h;
    }

    opj_cparameters_t cparams = MakeLosslessParams(sc.numres);
    cparams.irreversible = sc.irreversible;
    cparams.tcp_mct = (sc.numcomps == 3 && sc.dx == 1 && sc.dy == 1) ? 1 : 0;
    if (sc.tiled) {
      cparams.tile_size_on = OPJ_TRUE;
      cparams.cp_tx0 = static_cast<int>(sc.x0);
      cparams.cp_ty0 = static_cast<int>(sc.y0);
      cparams.cp_tdx = static_cast<int>(sc.tile_w);
      cparams.cp_tdy = static_cast<int>(sc.tile_h);
    }

    const std::optional<std::vector<uint8_t>> encoded = CompressImageToBytes(
        src.get(), OPJ_CODEC_J2K, &cparams, nullptr, nullptr, sc.threads);
    ASSERT_TRUE(encoded.has_value());

    // 2. Full-image decode matches source pixels (exact for 5-3, bounded for
    // 9-7).
    const std::optional<int> opt_threads =
        (sc.threads > 0) ? std::optional<int>(sc.threads) : std::nullopt;
    OpjImagePtr full_decoded = DecompressBytesToImage(*encoded, OPJ_CODEC_J2K,
                                                      {.threads = opt_threads});
    ASSERT_NE(nullptr, full_decoded);
    ASSERT_EQ(sc.numcomps, full_decoded->numcomps);
    for (OPJ_UINT32 c = 0; c < sc.numcomps; ++c) {
      ASSERT_EQ(expected_w[c], full_decoded->comps[c].w);
      ASSERT_EQ(expected_h[c], full_decoded->comps[c].h);
      const size_t count = expected_pixels[c].size();
      for (size_t i = 0; i < count; ++i) {
        if (sc.irreversible == 0) {
          EXPECT_EQ(expected_pixels[c][i], full_decoded->comps[c].data[i])
              << "comp=" << c << " i=" << i;
        } else {
          EXPECT_LE(
              std::abs(expected_pixels[c][i] - full_decoded->comps[c].data[i]),
              6)
              << "comp=" << c << " i=" << i;
        }
      }
    }

    // 3. Partial-tile windowed decode via opj_set_decode_area matches the
    //    corresponding subregion of the full decoded image.
    const std::array<int32_t, 4> win_area = {
        static_cast<int32_t>(sc.window.x0), static_cast<int32_t>(sc.window.y0),
        static_cast<int32_t>(sc.window.x1), static_cast<int32_t>(sc.window.y1)};
    OpjImagePtr win_decoded = DecompressBytesToImage(
        *encoded, OPJ_CODEC_J2K, {.threads = opt_threads, .area = win_area});
    ASSERT_NE(nullptr, win_decoded);
    for (OPJ_UINT32 c = 0; c < sc.numcomps; ++c) {
      const OPJ_UINT32 dx = win_decoded->comps[c].dx;
      const OPJ_UINT32 dy = win_decoded->comps[c].dy;
      const OPJ_UINT32 full_x0 = full_decoded->comps[c].x0;
      const OPJ_UINT32 full_y0 = full_decoded->comps[c].y0;
      const OPJ_UINT32 full_w = full_decoded->comps[c].w;
      const OPJ_UINT32 sub_x0 = win_decoded->comps[c].x0;
      const OPJ_UINT32 sub_y0 = win_decoded->comps[c].y0;
      ASSERT_EQ(opj_uint_ceildiv(sc.window.x1, dx) - sub_x0,
                win_decoded->comps[c].w);
      ASSERT_EQ(opj_uint_ceildiv(sc.window.y1, dy) - sub_y0,
                win_decoded->comps[c].h);
      for (OPJ_UINT32 y = 0; y < win_decoded->comps[c].h; ++y) {
        for (OPJ_UINT32 x = 0; x < win_decoded->comps[c].w; ++x) {
          const size_t full_idx =
              static_cast<size_t>(sub_y0 - full_y0 + y) * full_w +
              (sub_x0 - full_x0 + x);
          const size_t win_idx =
              static_cast<size_t>(y) * win_decoded->comps[c].w + x;
          EXPECT_LE(std::abs(full_decoded->comps[c].data[full_idx] -
                             win_decoded->comps[c].data[win_idx]),
                    sc.irreversible == 0 ? 0 : 1)
              << "comp=" << c << " x=" << x << " y=" << y;
        }
      }
    }

    // 4. Reduced-resolution partial decode when numres >= 3 exercises
    //    numres < tilec->numresolutions in opj_dwt_decode_partial_*.
    if (sc.numres >= 3 && sc.dx == 1 && sc.dy == 1) {
      OpjImagePtr reduced_full = DecompressBytesToImage(
          *encoded, OPJ_CODEC_J2K, {.resolution_factor = 1U});
      OpjImagePtr reduced_win = DecompressBytesToImage(
          *encoded, OPJ_CODEC_J2K, {.resolution_factor = 1U, .area = win_area});
      ASSERT_NE(nullptr, reduced_full);
      ASSERT_NE(nullptr, reduced_win);
      for (OPJ_UINT32 c = 0; c < sc.numcomps; ++c) {
        const OPJ_UINT32 rx0 =
            opj_uint_ceildivpow2(reduced_win->comps[c].x0, 1);
        const OPJ_UINT32 ry0 =
            opj_uint_ceildivpow2(reduced_win->comps[c].y0, 1);
        const OPJ_UINT32 fx0 =
            opj_uint_ceildivpow2(reduced_full->comps[c].x0, 1);
        const OPJ_UINT32 fy0 =
            opj_uint_ceildivpow2(reduced_full->comps[c].y0, 1);
        const OPJ_UINT32 fw = reduced_full->comps[c].w;
        for (OPJ_UINT32 y = 0; y < reduced_win->comps[c].h; ++y) {
          for (OPJ_UINT32 x = 0; x < reduced_win->comps[c].w; ++x) {
            const size_t full_idx =
                static_cast<size_t>(ry0 - fy0 + y) * fw + (rx0 - fx0 + x);
            const size_t win_idx =
                static_cast<size_t>(y) * reduced_win->comps[c].w + x;
            EXPECT_LE(std::abs(reduced_full->comps[c].data[full_idx] -
                               reduced_win->comps[c].data[win_idx]),
                      sc.irreversible == 0 ? 0 : 1)
                << "reduced comp=" << c << " x=" << x << " y=" << y;
          }
        }
      }
    }
  }
}

// TODO(schwehr): Cover remaining OOM allocation failure branches, impossible
// 64-bit size overflow guards, and target-gated AVX2/AVX512 branches in dwt.c.

}  // namespace
}  // namespace openjpeg2
