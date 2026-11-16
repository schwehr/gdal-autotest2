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
// Tests internal OpenJPEG tile coder/decoder (TCD) lifecycle, tile buffer
// management, rate allocation, MCT/DC-level-shift/DWT/T1/T2 orchestration, and
// partial/windowed decoding (src/lib/openjp2/tcd.c).
//
// Google3 build caveats:
// - USE_JPIP, USE_JPWL, TODO_MSD, DEBUG_VERBOSE, and DEBUG_RATE_ALLOC in tcd.c
//   are compiled out in google3 builds.
// - Google3 targets little-endian LP64 Linux and compiles with -funsigned-char
//   and UBSan by default; --cpu=arm exercises non-x86 scalar paths.
// - opj_tcd_copy_tile_data / opj_tcd_update_tile_data case 1 (prec <= 8) cast
//   bytes through OPJ_CHAR (plain char), so signedness of 8-bit packed buffers
//   matches host OPJ_CHAR signedness.
// - opj_tcd_rateallocate computes (1 << prec) and opj_tcd_dc_level_shift_decode
//   computes (1 << (prec - 1)) / (1U << prec); prec == 32 is exercised only on
//   buffer-sizing and copy/update APIs (max encoding prec <= 28).
// - Google3 always builds -DMUTEX_pthread=1. Tests inspect internal opj_cp_t /
//   opj_tcd_* fields and pin exact upstream error strings (ElementsAre pins
//   single-error cardinality).
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/tcd.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/tcd.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_tcd_t) > 0);
static_assert(sizeof(opj_tcd_marker_info_t) > 0);
static_assert(sizeof(opj_codestream_info_t) > 0);

constexpr OPJ_INT32 kSampleSentinel = -999999;
constexpr OPJ_BYTE kByteSentinel = 0xA5;
// Empirical max abs error across 9-7 float DWT + ICT rounding on x86 SIMD and
// --cpu=arm scalar paths.
constexpr OPJ_INT32 kIrreversibleMaxAbsError = 4;

struct DirectEventContext {
  EventCapture capture;
  opj_event_mgr_t manager{};

  DirectEventContext() : manager(MakeEventManager(&capture)) {}
};

struct DirectTcdConfig {
  OPJ_BOOL is_decoder = OPJ_FALSE;
  OPJ_UINT32 width = 16;
  OPJ_UINT32 height = 16;
  OPJ_UINT32 x0 = 0;
  OPJ_UINT32 y0 = 0;
  OPJ_UINT32 numcomps = 1;
  OPJ_UINT32 prec = 8;
  OPJ_UINT32 sgnd = 0;
  OPJ_UINT32 dx = 1;
  OPJ_UINT32 dy = 1;
  OPJ_COLOR_SPACE color_space = OPJ_CLRSPC_UNSPECIFIED;
  OPJ_UINT32 tx0 = 0;
  OPJ_UINT32 ty0 = 0;
  OPJ_UINT32 tdx = 16;
  OPJ_UINT32 tdy = 16;
  OPJ_UINT32 tw = 1;
  OPJ_UINT32 th = 1;
  OPJ_UINT32 numresolutions = 3;
  OPJ_UINT32 cblkw = 6;
  OPJ_UINT32 cblkh = 6;
  OPJ_UINT32 prcw = 15;
  OPJ_UINT32 prch = 15;
  OPJ_UINT32 qmfbid = 1;
  OPJ_UINT32 cblksty = 0;
  OPJ_UINT32 numlayers = 1;
  OPJ_UINT32 mct = 0;
  OPJ_UINT32 m_reduce = 0;
  OPJ_INT32 roishift = 0;
  OPJ_UINT16 rsiz = OPJ_PROFILE_NONE;
  J2K_QUALITY_LAYER_ALLOCATION_STRATEGY alloc_strategy = RATE_DISTORTION_RATIO;
  OPJ_UINT32 max_comp_size = 0;
  OPJ_PROG_ORDER prog_order = OPJ_LRCP;
  int num_threads = 0;
  bool allocate_image_data = true;
};

// Owns image, cp/tcp/tccp tables, optional thread pool, and opj_tcd_t in strict
// reverse-destruction order (tcd destroyed first before image/cp/pool).
struct DirectTcdHarness {
  DirectEventContext events;
  OpjImagePtr image;
  std::vector<opj_tcp_t> tcps;
  std::vector<opj_tccp_t> tccps;
  opj_cp_t cp{};
  std::vector<OPJ_FLOAT32> mct_coding_matrix;
  std::vector<OPJ_FLOAT32> mct_decoding_matrix;
  std::vector<OPJ_FLOAT64> mct_norms;
  OpjThreadPoolPtr thread_pool;
  OpjTcdPtr tcd;

  bool Init(const DirectTcdConfig& cfg) {
    if (cfg.numcomps == 0 || cfg.tw == 0 || cfg.th == 0) {
      return false;
    }

    std::vector<opj_image_cmptparm_t> cmpts(cfg.numcomps);
    for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
      const OPJ_UINT32 cdx = (c == 0) ? 1 : cfg.dx;
      const OPJ_UINT32 cdy = (c == 0) ? 1 : cfg.dy;
      cmpts[c] = opj_image_cmptparm_t{
          .dx = cdx,
          .dy = cdy,
          .w = (cfg.width + cdx - 1) / cdx,
          .h = (cfg.height + cdy - 1) / cdy,
          .x0 = cfg.x0,
          .y0 = cfg.y0,
          .prec = cfg.prec,
          .sgnd = cfg.sgnd,
      };
    }

    if (cfg.allocate_image_data) {
      image.reset(
          opj_image_create(cfg.numcomps, cmpts.data(), cfg.color_space));
    } else {
      image.reset(
          opj_image_tile_create(cfg.numcomps, cmpts.data(), cfg.color_space));
    }
    if (!image) {
      return false;
    }
    image->x0 = cfg.x0;
    image->y0 = cfg.y0;
    image->x1 = cfg.x0 + cfg.width;
    image->y1 = cfg.y0 + cfg.height;

    const OPJ_UINT32 numtiles = cfg.tw * cfg.th;
    tcps.assign(numtiles, opj_tcp_t{});
    tccps.assign(static_cast<size_t>(numtiles) * cfg.numcomps, opj_tccp_t{});

    cp = opj_cp_t{};
    cp.rsiz = cfg.rsiz;
    cp.tx0 = cfg.tx0;
    cp.ty0 = cfg.ty0;
    cp.tdx = cfg.tdx;
    cp.tdy = cfg.tdy;
    cp.tw = cfg.tw;
    cp.th = cfg.th;
    cp.tcps = tcps.data();
    cp.m_is_decoder = cfg.is_decoder ? 1 : 0;
    if (cfg.is_decoder) {
      cp.m_specific_param.m_dec.m_reduce = cfg.m_reduce;
      cp.m_specific_param.m_dec.m_layer = 0;
    } else {
      cp.m_specific_param.m_enc.m_quality_layer_alloc_strategy =
          cfg.alloc_strategy;
      cp.m_specific_param.m_enc.m_max_comp_size = cfg.max_comp_size;
    }

    for (OPJ_UINT32 t = 0; t < numtiles; ++t) {
      tcps[t].prg = cfg.prog_order;
      tcps[t].numlayers = cfg.numlayers;
      tcps[t].num_layers_to_decode = cfg.numlayers;
      tcps[t].mct = cfg.mct;
      tcps[t].tccps = tccps.data() + static_cast<size_t>(t) * cfg.numcomps;

      for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
        opj_tccp_t& tccp = tcps[t].tccps[c];
        tccp.numresolutions = cfg.numresolutions;
        tccp.cblkw = cfg.cblkw;
        tccp.cblkh = cfg.cblkh;
        tccp.cblksty = cfg.cblksty;
        tccp.qmfbid = cfg.qmfbid;
        tccp.qntsty =
            (cfg.qmfbid == 1) ? J2K_CCP_QNTSTY_NOQNT : J2K_CCP_QNTSTY_SIQNT;
        tccp.numgbits = 2;
        tccp.roishift = cfg.roishift;
        tccp.m_dc_level_shift =
            (cfg.sgnd != 0 || cfg.prec == 0 || cfg.prec >= 31)
                ? 0
                : static_cast<OPJ_INT32>(1U << (cfg.prec - 1));
        for (OPJ_UINT32 r = 0; r < OPJ_J2K_MAXRLVLS; ++r) {
          tccp.prcw[r] = cfg.prcw;
          tccp.prch[r] = cfg.prch;
        }
        if (cfg.numresolutions > 0 && cfg.numresolutions <= OPJ_J2K_MAXRLVLS &&
            cfg.prec > 0 && cfg.prec <= 30) {
          opj_dwt_calc_explicit_stepsizes(&tccp, cfg.prec);
        }
      }
    }

    thread_pool.reset(opj_thread_pool_create(cfg.num_threads));
    if (!thread_pool) {
      return false;
    }

    tcd.reset(opj_tcd_create(cfg.is_decoder));
    if (!tcd) {
      return false;
    }
    return opj_tcd_init(tcd.get(), image.get(), &cp, thread_pool.get()) ==
           OPJ_TRUE;
  }

  void SetCustomMct(std::span<const OPJ_FLOAT32> encode_matrix,
                    std::span<const OPJ_FLOAT32> decode_matrix,
                    std::span<const OPJ_INT32> dc_shifts) {
    mct_coding_matrix.assign(encode_matrix.begin(), encode_matrix.end());
    mct_decoding_matrix.assign(decode_matrix.begin(), decode_matrix.end());
    mct_norms.assign(image->numcomps, 1.0);
    if (!mct_coding_matrix.empty() &&
        mct_coding_matrix.size() ==
            static_cast<size_t>(image->numcomps) * image->numcomps) {
      opj_calculate_norms(mct_norms.data(), image->numcomps,
                          mct_coding_matrix.data());
    }
    for (OPJ_UINT32 t = 0; t < tcps.size(); ++t) {
      tcps[t].mct = 2;
      tcps[t].m_mct_coding_matrix =
          mct_coding_matrix.empty() ? nullptr : mct_coding_matrix.data();
      tcps[t].m_mct_decoding_matrix =
          mct_decoding_matrix.empty() ? nullptr : mct_decoding_matrix.data();
      tcps[t].mct_norms = mct_norms.empty() ? nullptr : mct_norms.data();
      for (OPJ_UINT32 c = 0; c < image->numcomps && c < dc_shifts.size(); ++c) {
        tcps[t].tccps[c].m_dc_level_shift = dc_shifts[c];
      }
    }
  }
};

std::vector<OPJ_INT32> ExtractTileSamples(const opj_tcd_tilecomp_t& tilec) {
  const OPJ_UINT32 w = static_cast<OPJ_UINT32>(tilec.x1 - tilec.x0);
  const OPJ_UINT32 h = static_cast<OPJ_UINT32>(tilec.y1 - tilec.y0);
  const size_t count = static_cast<size_t>(w) * h;
  return std::vector<OPJ_INT32>(tilec.data, tilec.data + count);
}

std::vector<uint8_t> PackTilePayload(OPJ_UINT32 tile_index,
                                     std::span<const OPJ_UINT32> comp_widths,
                                     std::span<const OPJ_UINT32> comp_heights,
                                     OPJ_UINT32 prec, OPJ_UINT32 sgnd) {
  OPJ_UINT32 bytes_per_sample = (prec >> 3) + ((prec & 7u) != 0 ? 1u : 0u);
  if (bytes_per_sample == 3) {
    bytes_per_sample = 4;
  }
  size_t total_bytes = 0;
  for (size_t c = 0; c < comp_widths.size(); ++c) {
    total_bytes += static_cast<size_t>(comp_widths[c]) * comp_heights[c] *
                   bytes_per_sample;
  }

  std::vector<uint8_t> buf(total_bytes, 0);
  const uint64_t mask = (prec >= 32) ? 0xFFFFFFFFULL : ((1ULL << prec) - 1ULL);
  const int64_t bias =
      (sgnd != 0) ? static_cast<int64_t>(1ULL << (prec - 1)) : 0;

  size_t offset = 0;
  for (size_t c = 0; c < comp_widths.size(); ++c) {
    const size_t count = static_cast<size_t>(comp_widths[c]) * comp_heights[c];
    for (size_t i = 0; i < count; ++i) {
      const uint64_t raw = ((static_cast<uint64_t>(tile_index) * 37ULL +
                             c * 53ULL + i * 19ULL + (i % 7ULL) * 11ULL) &
                            mask);
      const int64_t signed_val = static_cast<int64_t>(raw) - bias;
      if (bytes_per_sample == 1) {
        const auto ch = static_cast<OPJ_CHAR>(signed_val);
        std::memcpy(buf.data() + offset, &ch, sizeof(ch));
      } else if (bytes_per_sample == 2) {
        const auto v16 = static_cast<OPJ_INT16>(signed_val);
        std::memcpy(buf.data() + offset, &v16, sizeof(v16));
      } else {
        const auto v32 = static_cast<OPJ_INT32>(signed_val);
        std::memcpy(buf.data() + offset, &v32, sizeof(v32));
      }
      offset += bytes_per_sample;
    }
  }
  return buf;
}

TEST(TcdTest, CreateDestroyMarkerInfoSegmentAndEmptyBandLifecycle) {
  // 1. Destroy null handles safely.
  opj_tcd_destroy(nullptr);
  opj_tcd_marker_info_destroy(nullptr);

  // 2. Create encoder and decoder TCD handles and destroy before opj_tcd_init
  // (exercises opj_tcd_free_tile early return when tcd_image->tiles is null).
  OpjTcdPtr enc_tcd(opj_tcd_create(OPJ_FALSE));
  ASSERT_NE(nullptr, enc_tcd);
  EXPECT_EQ(0u, enc_tcd->m_is_decoder);
  ASSERT_NE(nullptr, enc_tcd->tcd_image);
  EXPECT_EQ(nullptr, enc_tcd->tcd_image->tiles);

  OpjTcdPtr dec_tcd(opj_tcd_create(OPJ_TRUE));
  ASSERT_NE(nullptr, dec_tcd);
  EXPECT_EQ(1u, dec_tcd->m_is_decoder);
  ASSERT_NE(nullptr, dec_tcd->tcd_image);

  // 3. Marker info create/destroy with both need_PLT states and packet array.
  OpjMarkerInfoPtr no_plt(opj_tcd_marker_info_create(OPJ_FALSE));
  ASSERT_NE(nullptr, no_plt);
  EXPECT_EQ(OPJ_FALSE, no_plt->need_PLT);
  EXPECT_EQ(0u, no_plt->packet_count);
  EXPECT_EQ(nullptr, no_plt->p_packet_size);

  OpjMarkerInfoPtr with_plt(opj_tcd_marker_info_create(OPJ_TRUE));
  ASSERT_NE(nullptr, with_plt);
  EXPECT_EQ(OPJ_TRUE, with_plt->need_PLT);
  with_plt->packet_count = 2;
  with_plt->p_packet_size =
      static_cast<OPJ_UINT32*>(opj_malloc(2 * sizeof(OPJ_UINT32)));
  ASSERT_NE(nullptr, with_plt->p_packet_size);
  with_plt->p_packet_size[0] = 10;
  with_plt->p_packet_size[1] = 20;

  // 4. Segment reinitialization clears all fields to zero.
  opj_tcd_seg_t seg{};
  seg.len = 128;
  seg.numpasses = 5;
  seg.real_num_passes = 4;
  seg.maxpasses = 10;
  seg.numnewpasses = 2;
  seg.newlen = 64;
  opj_tcd_reinit_segment(&seg);
  EXPECT_EQ(0u, seg.len);
  EXPECT_EQ(0u, seg.numpasses);
  EXPECT_EQ(0u, seg.real_num_passes);
  EXPECT_EQ(0u, seg.maxpasses);
  EXPECT_EQ(0u, seg.numnewpasses);
  EXPECT_EQ(0u, seg.newlen);

  // 5. Sub-band emptiness checks zero width, zero height, both, and non-empty.
  opj_tcd_band_t band{};
  band = {.x0 = 4, .y0 = 0, .x1 = 4, .y1 = 8};
  EXPECT_EQ(OPJ_TRUE, opj_tcd_is_band_empty(&band));
  band = {.x0 = 0, .y0 = 5, .x1 = 8, .y1 = 5};
  EXPECT_EQ(OPJ_TRUE, opj_tcd_is_band_empty(&band));
  band = {.x0 = 2, .y0 = 2, .x1 = 2, .y1 = 2};
  EXPECT_EQ(OPJ_TRUE, opj_tcd_is_band_empty(&band));
  band = {.x0 = 1, .y0 = 2, .x1 = 5, .y1 = 6};
  EXPECT_EQ(OPJ_FALSE, opj_tcd_is_band_empty(&band));
}

TEST(TcdTest, InitAndAllocTileComponentDataPaths) {
  DirectTcdHarness harness;
  ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                            .width = 8,
                            .height = 8,
                            .numcomps = 2,
                            .num_threads = 1}));
  EXPECT_EQ(harness.image.get(), harness.tcd->image);
  EXPECT_EQ(&harness.cp, harness.tcd->cp);
  EXPECT_EQ(harness.thread_pool.get(), harness.tcd->thread_pool);
  ASSERT_NE(nullptr, harness.tcd->tcd_image->tiles);
  EXPECT_EQ(2u, harness.tcd->tcd_image->tiles->numcomps);

  // Populate used_component so opj_tcd_destroy frees it cleanly as well.
  harness.tcd->used_component =
      static_cast<OPJ_BOOL*>(opj_calloc(2, sizeof(OPJ_BOOL)));
  ASSERT_NE(nullptr, harness.tcd->used_component);
  harness.tcd->used_component[0] = OPJ_TRUE;

  // 1. data == nullptr and data_size_needed == 0 succeeds cleanly.
  opj_tcd_tilecomp_t tilec{};
  EXPECT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
  EXPECT_EQ(nullptr, tilec.data);
  EXPECT_EQ(0u, tilec.data_size);
  EXPECT_EQ(OPJ_TRUE, tilec.ownsData);

  // 2. Initial allocation when data == nullptr.
  tilec.data_size_needed = 64;
  ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
  OPJ_INT32* first_ptr = tilec.data;
  ASSERT_NE(nullptr, first_ptr);
  EXPECT_EQ(64u, tilec.data_size);
  EXPECT_EQ(OPJ_TRUE, tilec.ownsData);

  // 3. Smaller or equal size reuses existing owned buffer.
  tilec.data_size_needed = 32;
  ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
  EXPECT_EQ(first_ptr, tilec.data);
  EXPECT_EQ(64u, tilec.data_size);

  // 4. Larger size with ownsData == OPJ_TRUE frees old and reallocates.
  tilec.data_size_needed = 128;
  ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
  ASSERT_NE(nullptr, tilec.data);
  EXPECT_EQ(128u, tilec.data_size);
  EXPECT_EQ(OPJ_TRUE, tilec.ownsData);
  opj_image_data_free(tilec.data);
  tilec = opj_tcd_tilecomp_t{};

  // 5. External unowned buffer (ownsData == OPJ_FALSE) is reused when large
  // enough and replaced with an owned buffer (without freeing caller memory)
  // when a larger size is requested.
  auto* external_buf = static_cast<OPJ_INT32*>(opj_image_data_alloc(64));
  ASSERT_NE(nullptr, external_buf);
  absl::Cleanup free_external = [external_buf] {
    opj_image_data_free(external_buf);
  };
  tilec.data = external_buf;
  tilec.data_size = 64;
  tilec.ownsData = OPJ_FALSE;
  tilec.data_size_needed = 64;
  ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
  EXPECT_EQ(external_buf, tilec.data);
  EXPECT_EQ(OPJ_FALSE, tilec.ownsData);

  tilec.data_size_needed = 256;
  ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
  EXPECT_NE(external_buf, tilec.data);
  EXPECT_EQ(256u, tilec.data_size);
  EXPECT_EQ(OPJ_TRUE, tilec.ownsData);
  opj_image_data_free(tilec.data);
}

TEST(TcdTest, GetDecodedTileSizeAndOverflows) {
  struct PrecisionSizeCase {
    OPJ_UINT32 prec;
    OPJ_UINT32 expected_bytes_per_sample;
  };
  constexpr auto kPrecisionCases = std::to_array<PrecisionSizeCase>({
      {.prec = 1, .expected_bytes_per_sample = 1},
      {.prec = 5, .expected_bytes_per_sample = 1},
      {.prec = 8, .expected_bytes_per_sample = 1},
      {.prec = 9, .expected_bytes_per_sample = 2},
      {.prec = 16, .expected_bytes_per_sample = 2},
      {.prec = 17, .expected_bytes_per_sample = 4},
      {.prec = 24, .expected_bytes_per_sample = 4},
      {.prec = 25, .expected_bytes_per_sample = 4},
      {.prec = 32, .expected_bytes_per_sample = 4},
  });

  // 1. Single-tile component sizing across all precision buckets (1, 2, 3->4B).
  for (const auto& tc : kPrecisionCases) {
    SCOPED_TRACE(tc.prec);
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 8,
                              .height = 4,
                              .numcomps = 1,
                              .prec = tc.prec,
                              .tdx = 8,
                              .tdy = 4,
                              .numresolutions = 2}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    EXPECT_EQ(32u * tc.expected_bytes_per_sample,
              opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_FALSE));
  }

  // 2. Partial vs whole decoding on subsampled (dx=2, dy=2) 2-component tile,
  // plus zero-height partial window (h == 0).
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 16,
                              .height = 16,
                              .numcomps = 2,
                              .prec = 12,
                              .dx = 2,
                              .dy = 2,
                              .tdx = 16,
                              .tdy = 16,
                              .numresolutions = 2}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    harness.tcd->whole_tile_decoding = OPJ_FALSE;
    opj_tcd_tile_t* tile = harness.tcd->tcd_image->tiles;
    // Comp 0 (16x16) win 6x5, Comp 1 (8x8) win 3x2, prec=12 -> 2 bytes each.
    tile->comps[0].resolutions[1].win_x0 = 2;
    tile->comps[0].resolutions[1].win_x1 = 8;
    tile->comps[0].resolutions[1].win_y0 = 1;
    tile->comps[0].resolutions[1].win_y1 = 6;
    tile->comps[1].resolutions[1].win_x0 = 1;
    tile->comps[1].resolutions[1].win_x1 = 4;
    tile->comps[1].resolutions[1].win_y0 = 1;
    tile->comps[1].resolutions[1].win_y1 = 3;

    EXPECT_EQ((16u * 16u + 8u * 8u) * 2u,
              opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_FALSE));
    EXPECT_EQ((6u * 5u + 3u * 2u) * 2u,
              opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_TRUE));

    // Zero-height window skips w * h overflow check and returns 0.
    tile->comps[0].resolutions[1].win_y1 = 1;
    tile->comps[1].resolutions[1].win_y1 = 1;
    EXPECT_EQ(0u, opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_TRUE));
  }

  // 3. Overflow check #1: UINT_MAX / w < h.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 8,
                              .height = 8,
                              .numcomps = 1,
                              .prec = 8,
                              .tdx = 8,
                              .tdy = 8,
                              .numresolutions = 1}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    harness.tcd->whole_tile_decoding = OPJ_TRUE;
    opj_tcd_resolution_t& res =
        harness.tcd->tcd_image->tiles->comps[0].resolutions[0];
    res.x0 = 0;
    res.x1 = 65536;
    res.y0 = 0;
    res.y1 = 65536;
    EXPECT_EQ(std::numeric_limits<OPJ_UINT32>::max(),
              opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_FALSE));
  }

  // 4. Overflow check #2: UINT_MAX / l_size_comp < w * h.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 8,
                              .height = 8,
                              .numcomps = 1,
                              .prec = 16,
                              .tdx = 8,
                              .tdy = 8,
                              .numresolutions = 1}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    harness.tcd->whole_tile_decoding = OPJ_TRUE;
    opj_tcd_resolution_t& res =
        harness.tcd->tcd_image->tiles->comps[0].resolutions[0];
    res.x0 = 0;
    res.x1 = 65536;
    res.y0 = 0;
    res.y1 = 65535;
    EXPECT_EQ(std::numeric_limits<OPJ_UINT32>::max(),
              opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_FALSE));
  }

  // 5. Overflow check #3: l_temp > UINT_MAX - l_data_size across components.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 8,
                              .height = 8,
                              .numcomps = 2,
                              .prec = 8,
                              .tdx = 8,
                              .tdy = 8,
                              .numresolutions = 1}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    harness.tcd->whole_tile_decoding = OPJ_TRUE;
    opj_tcd_tilecomp_t* comps = harness.tcd->tcd_image->tiles->comps;
    comps[0].resolutions[0].x0 = 0;
    comps[0].resolutions[0].x1 = 65535;
    comps[0].resolutions[0].y0 = 0;
    comps[0].resolutions[0].y1 = 65535;
    comps[1].resolutions[0].x0 = 0;
    comps[1].resolutions[0].x1 = 1000;
    comps[1].resolutions[0].y0 = 0;
    comps[1].resolutions[0].y1 = 1000;
    EXPECT_EQ(std::numeric_limits<OPJ_UINT32>::max(),
              opj_tcd_get_decoded_tile_size(harness.tcd.get(), OPJ_FALSE));
  }
}

TEST(TcdTest, InitTileValidationOverflowsAndBufferGrowth) {
  // 1. Tile X coordinate out of image range.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init(
        {.is_decoder = OPJ_TRUE, .tx0 = 100, .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("Tile X coordinates")));
  }

  // 2. Tile Y coordinate out of image range.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init(
        {.is_decoder = OPJ_TRUE, .ty0 = 100, .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("Tile Y coordinates")));
  }

  // 3. Zero resolutions rejected.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .numresolutions = 0,
                              .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("at least one resolution")));
  }

  // 4. Precinct X end > INT_MAX overflow.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 0x40000001u,
                              .height = 16,
                              .tdx = 0x40000001u,
                              .tdy = 16,
                              .numresolutions = 1,
                              .prcw = 30,
                              .prch = 15,
                              .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("Integer overflow")));
  }

  // 5. Precinct Y end > INT_MAX overflow.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 16,
                              .height = 0x40000001u,
                              .tdx = 16,
                              .tdy = 0x40000001u,
                              .numresolutions = 1,
                              .prcw = 15,
                              .prch = 30,
                              .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("Integer overflow")));
  }

  // 6. pw * ph UINT32 overflow before precinct allocation.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 65536,
                              .height = 65536,
                              .tdx = 65536,
                              .tdy = 65536,
                              .numresolutions = 1,
                              .prcw = 0,
                              .prch = 0,
                              .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("exceeds system limits")));
  }

  // 7. Precinct array byte size overflow (pw * ph fits uint32, * sizeof wraps).
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 65536,
                              .height = 2000,
                              .tdx = 65536,
                              .tdy = 2000,
                              .numresolutions = 1,
                              .prcw = 0,
                              .prch = 0,
                              .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(harness.events.capture.errors,
                ElementsAre(HasSubstr("exceeds system limits")));
  }

  // 8. Codeblock array byte size overflow on single large precinct.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 20000,
                              .height = 20000,
                              .tdx = 20000,
                              .tdy = 20000,
                              .numresolutions = 1,
                              .cblkw = 0,
                              .cblkh = 0,
                              .prcw = 15,
                              .prch = 15,
                              .allocate_image_data = false}));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                  &harness.events.manager));
    EXPECT_THAT(
        harness.events.capture.errors,
        ElementsAre(HasSubstr("code block data exceeds system limits")));
  }

  // 9. numresolutions < m_reduce clamps minimum_num_resolutions to 1, and an
  // odd 1x1 tile at numresolutions = 2 yields pw == 0 / ph == 0 at res 0 and
  // empty HL/LH/HH bands on encoder init.
  {
    DirectTcdHarness dec_harness;
    ASSERT_TRUE(dec_harness.Init({.is_decoder = OPJ_TRUE,
                                  .width = 1,
                                  .height = 1,
                                  .x0 = 1,
                                  .y0 = 1,
                                  .tx0 = 0,
                                  .ty0 = 0,
                                  .tdx = 8,
                                  .tdy = 8,
                                  .numresolutions = 2,
                                  .m_reduce = 5,
                                  .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(dec_harness.tcd.get(), 0,
                                                 &dec_harness.events.manager));
    const opj_tcd_tilecomp_t& tc = dec_harness.tcd->tcd_image->tiles->comps[0];
    EXPECT_EQ(1u, tc.minimum_num_resolutions);
    EXPECT_EQ(0u, tc.resolutions[0].pw);
    EXPECT_EQ(0u, tc.resolutions[0].ph);

    DirectTcdHarness enc_harness;
    ASSERT_TRUE(enc_harness.Init({.is_decoder = OPJ_FALSE,
                                  .width = 1,
                                  .height = 1,
                                  .x0 = 1,
                                  .y0 = 1,
                                  .tx0 = 0,
                                  .ty0 = 0,
                                  .tdx = 8,
                                  .tdy = 8,
                                  .numresolutions = 2}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(enc_harness.tcd.get(), 0,
                                                 &enc_harness.events.manager));
    EXPECT_EQ(OPJ_TRUE,
              opj_tcd_is_band_empty(&enc_harness.tcd->tcd_image->tiles->comps[0]
                                         .resolutions[1]
                                         .bands[1]));
  }

  // 10. Encoder tile reuse grows resolutions, precincts, codeblocks (+
  // tgt_init), and per-codeblock encoded data buffers without leaks.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_FALSE,
                              .width = 8,
                              .height = 8,
                              .tdx = 32,
                              .tdy = 32,
                              .numresolutions = 2,
                              .cblkw = 6,
                              .cblkh = 6,
                              .prcw = 15,
                              .prch = 15,
                              .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    EXPECT_EQ(2u * sizeof(opj_tcd_resolution_t),
              harness.tcd->tcd_image->tiles->comps[0].resolutions_size);

    EXPECT_GT(harness.tcd->tcd_image->tiles->comps[0].resolutions[0].numbands,
              0u);

    // Step 2: grow image and reduce cblkw/cblkh and precincts to grow
    // resolutions, precincts, cblks, and call opj_tgt_init on
    // incltree/imsbtree.
    harness.image->x1 = 32;
    harness.image->y1 = 32;
    harness.tccps[0].numresolutions = 4;
    harness.tccps[0].cblkw = 1;
    harness.tccps[0].cblkh = 1;
    for (OPJ_UINT32 r = 0; r < 4; ++r) {
      harness.tccps[0].prcw[r] = 2;
      harness.tccps[0].prch[r] = 2;
    }
    opj_dwt_calc_explicit_stepsizes(&harness.tccps[0], 8);
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    EXPECT_EQ(4u * sizeof(opj_tcd_resolution_t),
              harness.tcd->tcd_image->tiles->comps[0].resolutions_size);

    // Step 3: larger precincts & codeblocks grows cblk->data via free(data-1).
    harness.tccps[0].cblkw = 5;
    harness.tccps[0].cblkh = 5;
    for (OPJ_UINT32 r = 0; r < 4; ++r) {
      harness.tccps[0].prcw[r] = 15;
      harness.tccps[0].prch[r] = 15;
    }
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
  }

  // 11. Decoder tile reuse frees stale data_win and cblk->decoded_data and
  // sanitizes segments/chunks via opj_tcd_reinit_segment: setup, then grow with
  // small precincts, then grow larger precincts/cblks (free(data-1)).
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 16,
                              .height = 16,
                              .tdx = 32,
                              .tdy = 32,
                              .numresolutions = 2,
                              .cblkw = 3,
                              .cblkh = 3,
                              .prcw = 15,
                              .prch = 15,
                              .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    opj_tcd_tilecomp_t& tc = harness.tcd->tcd_image->tiles->comps[0];
    tc.data_win = static_cast<OPJ_INT32*>(opj_image_data_alloc(64));
    ASSERT_NE(nullptr, tc.data_win);
    opj_tcd_cblk_dec_t& cblk =
        tc.resolutions[0].bands[0].precincts[0].cblks.dec[0];
    cblk.decoded_data =
        static_cast<OPJ_INT32*>(opj_aligned_malloc(16 * sizeof(OPJ_INT32)));
    ASSERT_NE(nullptr, cblk.decoded_data);
    cblk.segs[0].len = 42;
    cblk.segs[0].numpasses = 3;

    harness.image->x1 = 32;
    harness.image->y1 = 32;
    harness.tccps[0].numresolutions = 3;
    harness.tccps[0].cblkw = 2;
    harness.tccps[0].cblkh = 2;
    opj_dwt_calc_explicit_stepsizes(&harness.tccps[0], 8);
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    EXPECT_EQ(nullptr, tc.data_win);
    EXPECT_EQ(
        nullptr,
        tc.resolutions[0].bands[0].precincts[0].cblks.dec[0].decoded_data);
    EXPECT_EQ(0u,
              tc.resolutions[0].bands[0].precincts[0].cblks.dec[0].segs[0].len);
  }
}

TEST(TcdTest, CopyAndUpdateTileDataAllPrecisionsSignsAndStride) {
  struct PrecisionDataCase {
    OPJ_UINT32 prec;
    OPJ_UINT32 sgnd;
    OPJ_UINT32 expected_bytes_per_sample;
  };
  constexpr auto kDataCases = std::to_array<PrecisionDataCase>({
      {.prec = 5, .sgnd = 0, .expected_bytes_per_sample = 1},
      {.prec = 8, .sgnd = 1, .expected_bytes_per_sample = 1},
      {.prec = 12, .sgnd = 0, .expected_bytes_per_sample = 2},
      {.prec = 16, .sgnd = 1, .expected_bytes_per_sample = 2},
      {.prec = 20, .sgnd = 0, .expected_bytes_per_sample = 4},
      {.prec = 24, .sgnd = 1, .expected_bytes_per_sample = 4},
      {.prec = 28, .sgnd = 0, .expected_bytes_per_sample = 4},
      {.prec = 32, .sgnd = 1, .expected_bytes_per_sample = 4},
  });

  // Keep tile sample count (4x4 = 16) divisible by 4 so every packed buffer
  // preserves 4-byte alignment across all component precisions.
  constexpr OPJ_UINT32 kW = 4;
  constexpr OPJ_UINT32 kH = 4;
  constexpr size_t kNumPixels = kW * kH;

  for (const auto& tc : kDataCases) {
    SCOPED_TRACE(tc.prec * 10u + tc.sgnd);
    // 1. Copy input tile bytes -> tilec->data across every precision and sign.
    DirectTcdHarness enc;
    ASSERT_TRUE(enc.Init({.is_decoder = OPJ_FALSE,
                          .width = kW,
                          .height = kH,
                          .numcomps = 1,
                          .prec = tc.prec,
                          .sgnd = tc.sgnd,
                          .tdx = kW,
                          .tdy = kH,
                          .numresolutions = 2,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_encode_tile(enc.tcd.get(), 0, &enc.events.manager));
    opj_tcd_tilecomp_t& enc_tilec = enc.tcd->tcd_image->tiles->comps[0];
    ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&enc_tilec));

    const OPJ_SIZE_T expected_bytes =
        opj_tcd_get_encoder_input_buffer_size(enc.tcd.get());
    EXPECT_EQ(kNumPixels * tc.expected_bytes_per_sample, expected_bytes);

    std::vector<OPJ_BYTE> src_bytes(expected_bytes + 4, kByteSentinel);
    // Use 4-byte aligned backing storage for typed writes on 2-byte/4-byte
    // paths.
    alignas(4) std::array<OPJ_BYTE, kNumPixels * 4> aligned_src{};
    std::vector<OPJ_INT32> expected_samples(kNumPixels);
    for (size_t i = 0; i < kNumPixels; ++i) {
      if (tc.expected_bytes_per_sample == 1) {
        const uint8_t raw_byte = (i % 2 == 0) ? static_cast<uint8_t>(10u + i)
                                              : static_cast<uint8_t>(0xF6u - i);
        aligned_src[i] = raw_byte;
        // Upstream tcd.c casts via OPJ_CHAR (unsigned char under
        // -funsigned-char, signed char otherwise) when sgnd == 1.
        expected_samples[i] =
            tc.sgnd ? static_cast<OPJ_INT32>(static_cast<OPJ_CHAR>(raw_byte))
                    : static_cast<OPJ_INT32>(raw_byte);
      } else if (tc.expected_bytes_per_sample == 2) {
        const OPJ_INT16 val16 = (i % 2 == 0) ? static_cast<OPJ_INT16>(300 + i)
                                             : static_cast<OPJ_INT16>(-250 - i);
        std::memcpy(aligned_src.data() + i * 2, &val16, sizeof(val16));
        expected_samples[i] = tc.sgnd
                                  ? static_cast<OPJ_INT32>(val16)
                                  : (static_cast<OPJ_INT32>(val16) & 0xFFFF);
      } else {
        const OPJ_INT32 val32 = (i % 2 == 0)
                                    ? (500000 + static_cast<OPJ_INT32>(i))
                                    : (-500000 - static_cast<OPJ_INT32>(i));
        std::memcpy(aligned_src.data() + i * 4, &val32, sizeof(val32));
        expected_samples[i] = val32;
      }
    }

    // Mismatched input length fails without corrupting destination.
    EXPECT_EQ(OPJ_FALSE, opj_tcd_copy_tile_data(enc.tcd.get(), src_bytes.data(),
                                                expected_bytes - 1));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_copy_tile_data(
                            enc.tcd.get(), aligned_src.data(), expected_bytes));
    EXPECT_THAT(ExtractTileSamples(enc_tilec),
                ElementsAreArray(expected_samples));

    // 2. Update decoded output bytes from tilec->data across all prec/sign
    // cases.
    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = kW,
                          .height = kH,
                          .numcomps = 1,
                          .prec = tc.prec,
                          .sgnd = tc.sgnd,
                          .tdx = kW,
                          .tdy = kH,
                          .numresolutions = 2,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    dec.tcd->whole_tile_decoding = OPJ_TRUE;
    dec.image->comps[0].resno_decoded = 1;
    opj_tcd_tilecomp_t& dec_tilec = dec.tcd->tcd_image->tiles->comps[0];
    dec_tilec.data_size_needed = kNumPixels * sizeof(OPJ_INT32);
    ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&dec_tilec));
    std::copy(expected_samples.begin(), expected_samples.end(), dec_tilec.data);

    const OPJ_UINT32 dec_bytes =
        opj_tcd_get_decoded_tile_size(dec.tcd.get(), OPJ_TRUE);
    ASSERT_EQ(expected_bytes, dec_bytes);
    alignas(4) std::array<OPJ_BYTE, kNumPixels * 4 + 4> out_buf{};
    out_buf.fill(kByteSentinel);

    // Truncated destination length fails cleanly.
    EXPECT_EQ(OPJ_FALSE, opj_tcd_update_tile_data(dec.tcd.get(), out_buf.data(),
                                                  dec_bytes - 1));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_update_tile_data(dec.tcd.get(), out_buf.data(),
                                                 dec_bytes));
    EXPECT_THAT(std::span<const OPJ_BYTE>(out_buf.data(), dec_bytes),
                ElementsAreArray(aligned_src.data(), dec_bytes));
    EXPECT_THAT(std::span<const OPJ_BYTE>(out_buf.data() + dec_bytes, 4),
                Each(kByteSentinel));
  }

  // 3. Stride skipping when resno_decoded < minimum_num_resolutions - 1 across
  // 1-byte, 2-byte, and 4-byte precisions, both signed and unsigned.
  for (const OPJ_UINT32 prec : {8u, 16u, 24u}) {
    for (const OPJ_UINT32 sgnd : {0u, 1u}) {
      DirectTcdHarness dec;
      ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                            .width = 8,
                            .height = 8,
                            .numcomps = 1,
                            .prec = prec,
                            .sgnd = sgnd,
                            .tdx = 8,
                            .tdy = 8,
                            .numresolutions = 3,
                            .allocate_image_data = false}));
      ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(dec.tcd.get(), 0,
                                                   &dec.events.manager));
      dec.tcd->whole_tile_decoding = OPJ_TRUE;
      // resno_decoded = 1 selects the 4x4 resolution while tilec->data stride
      // remains 8 from minimum_num_resolutions - 1 = 2 (l_stride = 4).
      dec.image->comps[0].resno_decoded = 1;
      opj_tcd_tilecomp_t& tilec = dec.tcd->tcd_image->tiles->comps[0];
      tilec.data_size_needed = 64u * sizeof(OPJ_INT32);
      ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tilec));
      std::fill_n(tilec.data, 64, kSampleSentinel);
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          tilec.data[y * 8 + x] = sgnd ? (-40 + y * 10 + x) : (20 + y * 10 + x);
        }
      }

      const OPJ_UINT32 bps = (prec == 8) ? 1u : (prec == 16 ? 2u : 4u);
      const OPJ_UINT32 full_len =
          opj_tcd_get_decoded_tile_size(dec.tcd.get(), OPJ_TRUE);
      ASSERT_EQ(64u * bps, full_len);
      std::vector<OPJ_BYTE> out(full_len + 4, kByteSentinel);
      ASSERT_EQ(OPJ_TRUE,
                opj_tcd_update_tile_data(dec.tcd.get(), out.data(), full_len));
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const OPJ_INT32 expected =
              sgnd ? (-40 + y * 10 + x) : (20 + y * 10 + x);
          const size_t idx = static_cast<size_t>(y * 4 + x);
          if (bps == 1) {
            const OPJ_CHAR actual = static_cast<OPJ_CHAR>(out[idx]);
            EXPECT_EQ(static_cast<OPJ_CHAR>(expected), actual);
          } else if (bps == 2) {
            OPJ_INT16 actual = 0;
            std::memcpy(&actual, out.data() + idx * 2, sizeof(actual));
            EXPECT_EQ(static_cast<OPJ_INT16>(expected), actual);
          } else {
            OPJ_INT32 actual = 0;
            std::memcpy(&actual, out.data() + idx * 4, sizeof(actual));
            EXPECT_EQ(expected, actual);
          }
        }
      }
      EXPECT_THAT(std::span<const OPJ_BYTE>(out.data() + 16u * bps,
                                            out.size() - 16u * bps),
                  Each(kByteSentinel));
    }
  }

  // 4. UINT_MAX decoded tile size fails opj_tcd_update_tile_data cleanly.
  {
    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 8,
                          .height = 8,
                          .numcomps = 1,
                          .prec = 8,
                          .tdx = 8,
                          .tdy = 8,
                          .numresolutions = 1,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    dec.tcd->whole_tile_decoding = OPJ_TRUE;
    opj_tcd_resolution_t& res =
        dec.tcd->tcd_image->tiles->comps[0].resolutions[0];
    res.x0 = 0;
    res.x1 = 65536;
    res.y0 = 0;
    res.y1 = 65536;
    OPJ_BYTE dummy = 0;
    EXPECT_EQ(OPJ_FALSE, opj_tcd_update_tile_data(dec.tcd.get(), &dummy, 1));
  }
}

TEST(TcdTest, MultiTileWriteReadAndPrecisionRoundTrips) {
  struct MultiTileCase {
    OPJ_UINT32 prec;
    OPJ_UINT32 sgnd;
    OPJ_UINT32 numcomps;
    OPJ_UINT32 dx;
    OPJ_UINT32 dy;
  };
  constexpr auto kMultiTileCases = std::to_array<MultiTileCase>({
      {.prec = 8, .sgnd = 0, .numcomps = 3, .dx = 2, .dy = 2},
      {.prec = 16, .sgnd = 1, .numcomps = 1, .dx = 1, .dy = 1},
      {.prec = 24, .sgnd = 0, .numcomps = 1, .dx = 1, .dy = 1},
  });

  constexpr OPJ_UINT32 kImgW = 32;
  constexpr OPJ_UINT32 kImgH = 32;
  constexpr OPJ_UINT32 kTileW = 16;
  constexpr OPJ_UINT32 kTileH = 16;

  // 1. Multi-tile opj_write_tile -> opj_decode_tile_data across subsampled
  // 8-bit, signed 16-bit, and 24-bit (3->4 byte) precisions.
  for (const auto& tc : kMultiTileCases) {
    SCOPED_TRACE(tc.prec * 10u + tc.sgnd);
    std::vector<opj_image_cmptparm_t> cmpts(tc.numcomps);
    std::vector<OPJ_UINT32> tile_cw(tc.numcomps);
    std::vector<OPJ_UINT32> tile_ch(tc.numcomps);
    for (OPJ_UINT32 c = 0; c < tc.numcomps; ++c) {
      const OPJ_UINT32 cdx = (c == 0) ? 1 : tc.dx;
      const OPJ_UINT32 cdy = (c == 0) ? 1 : tc.dy;
      cmpts[c] = opj_image_cmptparm_t{
          .dx = cdx,
          .dy = cdy,
          .w = (kImgW + cdx - 1) / cdx,
          .h = (kImgH + cdy - 1) / cdy,
          .x0 = 0,
          .y0 = 0,
          .prec = tc.prec,
          .sgnd = tc.sgnd,
      };
      tile_cw[c] = kTileW / cdx;
      tile_ch[c] = kTileH / cdy;
    }

    OpjImagePtr img(opj_image_tile_create(
        tc.numcomps, cmpts.data(),
        (tc.numcomps == 1) ? OPJ_CLRSPC_GRAY : OPJ_CLRSPC_SYCC));
    ASSERT_NE(nullptr, img);
    img->x0 = 0;
    img->y0 = 0;
    img->x1 = kImgW;
    img->y1 = kImgH;

    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.tile_size_on = OPJ_TRUE;
    cparams.cp_tx0 = 0;
    cparams.cp_ty0 = 0;
    cparams.cp_tdx = kTileW;
    cparams.cp_tdy = kTileH;
    cparams.tcp_numlayers = 1;
    cparams.tcp_rates[0] = 0;
    cparams.cp_disto_alloc = 1;
    cparams.numresolution = 3;
    cparams.tcp_mct = 0;

    OpjCodecPtr comp(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_NE(nullptr, comp);
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(comp.get(), &cparams, img.get()));

    MemWriteStream mem_out;
    OpjStreamPtr write_stream = mem_out.CreateStream();
    ASSERT_NE(nullptr, write_stream);
    ASSERT_EQ(OPJ_TRUE,
              opj_start_compress(comp.get(), img.get(), write_stream.get()));
    for (OPJ_UINT32 tile_idx = 0; tile_idx < 4; ++tile_idx) {
      std::vector<uint8_t> payload =
          PackTilePayload(tile_idx, tile_cw, tile_ch, tc.prec, tc.sgnd);
      ASSERT_EQ(OPJ_TRUE,
                opj_write_tile(comp.get(), tile_idx, payload.data(),
                               static_cast<OPJ_UINT32>(payload.size()),
                               write_stream.get()));
    }
    ASSERT_EQ(OPJ_TRUE, opj_end_compress(comp.get(), write_stream.get()));
    write_stream.reset();

    MemReadStream mem_in(mem_out.data);
    OpjStreamPtr read_stream = mem_in.CreateStream();
    ASSERT_NE(nullptr, read_stream);
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_NE(nullptr, decomp);
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_dec_img = nullptr;
    ASSERT_EQ(OPJ_TRUE,
              opj_read_header(read_stream.get(), decomp.get(), &raw_dec_img));
    OpjImagePtr dec_img(raw_dec_img);

    OPJ_UINT32 decoded_tiles = 0;
    for (;;) {
      OPJ_UINT32 tile_index = 0, data_size = 0, nb_comps = 0;
      OPJ_INT32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      OPJ_BOOL should_go_on = OPJ_FALSE;
      ASSERT_EQ(OPJ_TRUE,
                opj_read_tile_header(decomp.get(), read_stream.get(),
                                     &tile_index, &data_size, &x0, &y0, &x1,
                                     &y1, &nb_comps, &should_go_on));
      if (should_go_on == OPJ_FALSE) {
        break;
      }
      const std::vector<uint8_t> expected =
          PackTilePayload(tile_index, tile_cw, tile_ch, tc.prec, tc.sgnd);
      ASSERT_EQ(expected.size(), data_size);
      std::vector<OPJ_BYTE> actual(data_size + 4, kByteSentinel);
      ASSERT_EQ(OPJ_TRUE,
                opj_decode_tile_data(decomp.get(), tile_index, actual.data(),
                                     data_size, read_stream.get()));
      EXPECT_THAT(std::span<const OPJ_BYTE>(actual.data(), data_size),
                  ElementsAreArray(expected));
      EXPECT_THAT(std::span<const OPJ_BYTE>(actual.data() + data_size, 4),
                  Each(kByteSentinel));
      ++decoded_tiles;
    }
    EXPECT_EQ(4u, decoded_tiles);
    EXPECT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), read_stream.get()));
  }

  // 2. Single-tile signed 8-bit lossless round-trip preserves exact negative
  // pixels through opj_encode/opj_decode (avoids OPJ_CHAR byte packing).
  {
    OpjImagePtr src =
        CreateTestImage(16, 16, 1, /*prec=*/8, /*sgnd=*/1, OPJ_CLRSPC_GRAY);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());
    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_disto_alloc = 1;
    cparams.tcp_numlayers = 1;
    cparams.tcp_rates[0] = 0;
    cparams.numresolution = 3;
    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());

    MemReadStream mem_in(*encoded);
    OpjStreamPtr read_stream = mem_in.CreateStream();
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_img = nullptr;
    ASSERT_EQ(OPJ_TRUE,
              opj_read_header(read_stream.get(), decomp.get(), &raw_img));
    OpjImagePtr dec_img(raw_img);
    ASSERT_EQ(OPJ_TRUE,
              opj_decode(decomp.get(), read_stream.get(), dec_img.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), read_stream.get()));
    EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[0].data, 256),
                ElementsAreArray(expected[0]));
  }

  // 3. Direct 28-bit signed 4-byte lossless TCD encode -> decode round-trip
  // (keeps coefficient magnitude within t1.c fractional bit headroom and avoids
  // upstream shifts >= 31).
  {
    DirectTcdHarness enc;
    ASSERT_TRUE(enc.Init({.is_decoder = OPJ_FALSE,
                          .width = 16,
                          .height = 16,
                          .numcomps = 1,
                          .prec = 28,
                          .sgnd = 1,
                          .tdx = 16,
                          .tdy = 16,
                          .numresolutions = 2,
                          .qmfbid = 1,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_encode_tile(enc.tcd.get(), 0, &enc.events.manager));
    opj_tcd_tilecomp_t& enc_tilec = enc.tcd->tcd_image->tiles->comps[0];
    ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&enc_tilec));

    std::vector<OPJ_INT32> pixels(256);
    for (size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<OPJ_INT32>((i % 2 == 0 ? 1 : -1) *
                                         (100000 + static_cast<int>(i) * 123));
    }
    std::vector<OPJ_BYTE> raw_in(256 * sizeof(OPJ_INT32));
    std::memcpy(raw_in.data(), pixels.data(), raw_in.size());
    ASSERT_EQ(OPJ_TRUE, opj_tcd_copy_tile_data(enc.tcd.get(), raw_in.data(),
                                               raw_in.size()));

    std::vector<OPJ_BYTE> tile_bytes(8192, 0);
    OPJ_UINT32 bytes_written = 0;
    ASSERT_EQ(OPJ_TRUE, opj_tcd_encode_tile(
                            enc.tcd.get(), 0, tile_bytes.data(), &bytes_written,
                            static_cast<OPJ_UINT32>(tile_bytes.size()), nullptr,
                            nullptr, &enc.events.manager));
    ASSERT_GT(bytes_written, 0u);

    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 16,
                          .height = 16,
                          .numcomps = 1,
                          .prec = 28,
                          .sgnd = 1,
                          .tdx = 16,
                          .tdy = 16,
                          .numresolutions = 2,
                          .qmfbid = 1,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_decode_tile(dec.tcd.get(), 0, 0, 16, 16, 0, nullptr,
                                  tile_bytes.data(), bytes_written, 0, nullptr,
                                  &dec.events.manager));
    std::vector<OPJ_INT32> decoded(256, kSampleSentinel);
    std::vector<OPJ_BYTE> raw_out(256 * sizeof(OPJ_INT32), kByteSentinel);
    ASSERT_EQ(OPJ_TRUE, opj_tcd_update_tile_data(
                            dec.tcd.get(), raw_out.data(),
                            static_cast<OPJ_UINT32>(raw_out.size())));
    std::memcpy(decoded.data(), raw_out.data(), raw_out.size());
    EXPECT_THAT(decoded, ElementsAreArray(pixels));
  }
}

TEST(TcdTest, EncodeAndRateAllocateStrategies) {
  // 1. Multi-layer RATE_DISTORTION_RATIO (lossy + lossless, rev & irrev),
  // PLT marker output, and multi-tile-part progression (cur_tp_num > 0).
  for (const int irrev : {0, 1}) {
    SCOPED_TRACE(irrev);
    OpjImagePtr src =
        CreateTestImage(24, 24, 3, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);
    // Snapshot pixels before CompressImageToBytes because opj_start_compress
    // transfers ownership of src->comps[c].data and sets them to nullptr.
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_disto_alloc = 1;
    cparams.tcp_numlayers = 3;
    cparams.tcp_rates[0] = 20.0f;
    cparams.tcp_rates[1] = 8.0f;
    cparams.tcp_rates[2] = 0.0f;
    cparams.numresolution = 3;
    cparams.cblockw_init = 8;
    cparams.cblockh_init = 8;
    cparams.irreversible = irrev;
    cparams.tcp_mct = 1;
    cparams.tp_on = 1;
    cparams.tp_flag = 'C';
    cparams.prog_order = OPJ_CPRL;

    constexpr const char* kPltOpts[] = {"PLT=YES", nullptr};
    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams, kPltOpts);
    ASSERT_TRUE(encoded.has_value());

    MemReadStream mem_in(*encoded);
    OpjStreamPtr read_stream = mem_in.CreateStream();
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_img = nullptr;
    ASSERT_EQ(OPJ_TRUE,
              opj_read_header(read_stream.get(), decomp.get(), &raw_img));
    OpjImagePtr dec_img(raw_img);
    ASSERT_EQ(OPJ_TRUE,
              opj_decode(decomp.get(), read_stream.get(), dec_img.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), read_stream.get()));

    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      for (size_t i = 0; i < 24u * 24u; ++i) {
        const int diff = std::abs(expected[c][i] - dec_img->comps[c].data[i]);
        if (irrev == 0) {
          EXPECT_EQ(0, diff);
        } else {
          EXPECT_LE(diff, kIrreversibleMaxAbsError);
        }
      }
    }
  }

  // 2. Direct RATE_DISTORTION_RATIO edge cases:
  //    a) Tiny rate[0] forces thresh stabilization with stable_thresh == 0.
  //    b) p_cstr_info + p_marker_info index generation.
  //    c) Tight p_max_length fails opj_tcd_t2_encode cleanly.
  //    d) max_comp_size forces T2 component rate limit checks.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_FALSE,
                              .width = 24,
                              .height = 24,
                              .numcomps = 3,
                              .prec = 8,
                              .tdx = 24,
                              .tdy = 24,
                              .numresolutions = 3,
                              .cblkw = 3,
                              .cblkh = 3,
                              .qmfbid = 0,
                              .numlayers = 2,
                              .mct = 1,
                              .max_comp_size = 150,
                              .allocate_image_data = false}));
    harness.tcps[0].rates[0] = 1.0f;
    harness.tcps[0].rates[1] = 2000.0f;
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      opj_tcd_tilecomp_t& tc = harness.tcd->tcd_image->tiles->comps[c];
      ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tc));
      for (size_t i = 0; i < 24u * 24u; ++i) {
        tc.data[i] = static_cast<OPJ_INT32>((i * 23u + c * 71u) & 0xFFu);
      }
    }

    opj_tile_info_t tile_info{};
    opj_codestream_info_t cstr_info{};
    cstr_info.numcomps = 3;
    cstr_info.numlayers = 2;
    cstr_info.tile = &tile_info;
    absl::Cleanup free_cstr_tile_info = [&tile_info] {
      opj_free(tile_info.thresh);
      tile_info.thresh = nullptr;
      opj_free(tile_info.packet);
      tile_info.packet = nullptr;
    };

    OpjMarkerInfoPtr marker_info(opj_tcd_marker_info_create(OPJ_TRUE));
    ASSERT_NE(nullptr, marker_info);
    std::vector<OPJ_BYTE> dest(16384, 0);
    OPJ_UINT32 written = 0;
    ASSERT_EQ(OPJ_TRUE, opj_tcd_encode_tile(
                            harness.tcd.get(), 0, dest.data(), &written,
                            static_cast<OPJ_UINT32>(dest.size()), &cstr_info,
                            marker_info.get(), &harness.events.manager));
    EXPECT_GT(written, 0u);
    EXPECT_EQ(1, cstr_info.index_write);
    ASSERT_NE(nullptr, tile_info.thresh);
    EXPECT_NE(nullptr, tile_info.packet);
    EXPECT_GT(tile_info.numpix, 0);
    EXPECT_GT(marker_info->packet_count, 0u);
    EXPECT_NE(nullptr, marker_info->p_packet_size);

    // Re-encoding with cur_tp_num > 0 skips transform/ratealloc stages and
    // fails gracefully when p_max_length is too small for opj_tcd_t2_encode.
    harness.tcd->cur_tp_num = 1;
    harness.tcd->tp_num = 1;
    OPJ_UINT32 short_written = 0;
    std::array<OPJ_BYTE, 4> tiny_dest{};
    EXPECT_EQ(OPJ_FALSE,
              opj_tcd_encode_tile(harness.tcd.get(), 0, tiny_dest.data(),
                                  &short_written,
                                  static_cast<OPJ_UINT32>(tiny_dest.size()),
                                  nullptr, nullptr, &harness.events.manager));
  }

  // 3. FIXED_DISTORTION_RATIO: standard profile end-to-end and Cinema profile
  // direct rate-allocation path (distoratio + t2_encode_packets limit).
  {
    OpjImagePtr src =
        CreateTestImage(24, 24, 3, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);
    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_fixed_quality = 1;
    cparams.tcp_numlayers = 3;
    cparams.tcp_distoratio[0] = 22.0f;
    cparams.tcp_distoratio[1] = 35.0f;
    cparams.tcp_distoratio[2] = 0.0f;
    cparams.numresolution = 3;
    cparams.tcp_mct = 1;
    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_FALSE(encoded->empty());
    OpjImagePtr decoded = DecompressBytesToImage(*encoded, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, decoded);
    EXPECT_EQ(3u, decoded->numcomps);

    // Direct Cinema-2K profile with FIXED_DISTORTION_RATIO exercises
    // OPJ_IS_CINEMA rate-check warnings/retries and succeeds with final pass.
    DirectTcdHarness cinema;
    ASSERT_TRUE(cinema.Init({.is_decoder = OPJ_FALSE,
                             .width = 24,
                             .height = 24,
                             .numcomps = 3,
                             .prec = 8,
                             .tdx = 24,
                             .tdy = 24,
                             .numresolutions = 3,
                             .cblkw = 3,
                             .cblkh = 3,
                             .qmfbid = 1,
                             .numlayers = 2,
                             .mct = 1,
                             .rsiz = OPJ_PROFILE_CINEMA_2K,
                             .alloc_strategy = FIXED_DISTORTION_RATIO,
                             .allocate_image_data = false}));
    cinema.tcps[0].distoratio[0] = 20.0f;
    cinema.tcps[0].distoratio[1] = 45.0f;
    cinema.tcps[0].rates[0] = 8.0f;
    cinema.tcps[0].rates[1] = 8192.0f;
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(cinema.tcd.get(), 0,
                                                 &cinema.events.manager));
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      opj_tcd_tilecomp_t& tc = cinema.tcd->tcd_image->tiles->comps[c];
      ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tc));
      for (size_t i = 0; i < 24u * 24u; ++i) {
        tc.data[i] = static_cast<OPJ_INT32>((i * 19u + c * 41u) & 0xFFu);
      }
    }
    std::vector<OPJ_BYTE> dest(16384, 0);
    OPJ_UINT32 written = 0;
    EXPECT_EQ(OPJ_TRUE,
              opj_tcd_encode_tile(cinema.tcd.get(), 0, dest.data(), &written,
                                  static_cast<OPJ_UINT32>(dest.size()), nullptr,
                                  nullptr, &cinema.events.manager));
    EXPECT_THAT(cinema.events.capture.errors, IsEmpty());
    EXPECT_GT(written, 0u);
  }

  // 4. FIXED_LAYER (cp_fixed_alloc + cp_matrice) across 17x17 2x2 tiled
  // multi-magnitude 16-bit image: exercises empty edge tile bands, imsb vs
  // matrice branches (including negative clamped delta and zero passes), and
  // exact lossless recovery on final layer.
  {
    opj_image_cmptparm_t cmpt{.dx = 1,
                              .dy = 1,
                              .w = 17,
                              .h = 17,
                              .x0 = 0,
                              .y0 = 0,
                              .prec = 16,
                              .sgnd = 0};
    OpjImagePtr src(opj_image_create(1, &cmpt, OPJ_CLRSPC_GRAY));
    ASSERT_NE(nullptr, src);
    src->x0 = 0;
    src->y0 = 0;
    src->x1 = 17;
    src->y1 = 17;
    // Tile 0 ([0,16)x[0,16)) splits into 4x4 codeblocks of varying amplitude:
    // constant (numbps == 0), tiny (+/-10), medium (+/-400), and large
    // (+/-15000).
    for (int y = 0; y < 17; ++y) {
      for (int x = 0; x < 17; ++x) {
        int delta = 0;
        if (x < 8 && y < 8) {
          delta = 0;
        } else if (x >= 8 && y < 8) {
          delta = ((x ^ y) & 1) ? 10 : -10;
        } else if (x < 8 && y >= 8) {
          delta = ((x + y) & 1) ? 400 : -400;
        } else {
          delta = ((x ^ y) & 1) ? 15000 : -15000;
        }
        src->comps[0].data[y * 17 + x] = 32768 + delta;
      }
    }
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    // 3 layers x 3 resolutions x 3 bands = 27 entries; matrice <= prec (16) so
    // layer accumulation stays within totalpasses.
    // Non-const array because opj_cparameters_t::cp_matrice is int*.
    std::array<OPJ_INT32, 27> matrice = {
        4, 4, 4, 4, 4,  4,  4,  4,  4,  8,  8,  8,  8,  8,
        8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    };
    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.tile_size_on = OPJ_TRUE;
    cparams.cp_tx0 = 0;
    cparams.cp_ty0 = 0;
    cparams.cp_tdx = 16;
    cparams.cp_tdy = 16;
    cparams.cp_fixed_alloc = 1;
    cparams.cp_matrice = matrice.data();
    cparams.tcp_numlayers = 3;
    cparams.numresolution = 3;
    cparams.cblockw_init = 4;
    cparams.cblockh_init = 4;

    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());

    MemReadStream mem_in(*encoded);
    OpjStreamPtr read_stream = mem_in.CreateStream();
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_img = nullptr;
    ASSERT_EQ(OPJ_TRUE,
              opj_read_header(read_stream.get(), decomp.get(), &raw_img));
    OpjImagePtr dec_img(raw_img);
    ASSERT_EQ(OPJ_TRUE,
              opj_decode(decomp.get(), read_stream.get(), dec_img.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), read_stream.get()));
    EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[0].data, 17u * 17u),
                ElementsAreArray(expected[0]));
  }
}

TEST(TcdTest, MctZeroOneTwoAndNullCustomMatrices) {
  // 1. Standard MCT == 1 with reversible (qmfbid=1) & irreversible (qmfbid=0),
  // tested on both whole-tile and partial windowed decode.
  for (const int irrev : {0, 1}) {
    SCOPED_TRACE(irrev);
    OpjImagePtr src =
        CreateTestImage(24, 24, 3, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_disto_alloc = 1;
    cparams.tcp_numlayers = 1;
    cparams.tcp_rates[0] = 0;
    cparams.numresolution = 3;
    cparams.irreversible = irrev;
    cparams.tcp_mct = 1;

    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());

    MemReadStream mem_in(*encoded);
    OpjStreamPtr stream = mem_in.CreateStream();
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_img = nullptr;
    ASSERT_EQ(OPJ_TRUE, opj_read_header(stream.get(), decomp.get(), &raw_img));
    OpjImagePtr dec_img(raw_img);
    // Subwindow [4, 20) x [4, 20) exercises mct_decode partial path.
    ASSERT_EQ(OPJ_TRUE,
              opj_set_decode_area(decomp.get(), dec_img.get(), 4, 4, 20, 20));
    ASSERT_EQ(OPJ_TRUE, opj_decode(decomp.get(), stream.get(), dec_img.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), stream.get()));

    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
          const OPJ_INT32 ref = expected[c][(y + 4) * 24 + (x + 4)];
          const OPJ_INT32 got = dec_img->comps[c].data[y * 16 + x];
          if (irrev == 0) {
            EXPECT_EQ(ref, got);
          } else {
            EXPECT_LE(std::abs(ref - got), 3);
          }
        }
      }
    }
  }

  // 2. Direct MCT == 2 with null coding & decoding matrices takes early
  // OPJ_TRUE return paths cleanly.
  {
    DirectTcdHarness enc;
    ASSERT_TRUE(enc.Init({.is_decoder = OPJ_FALSE,
                          .width = 16,
                          .height = 16,
                          .numcomps = 3,
                          .prec = 8,
                          .tdx = 16,
                          .tdy = 16,
                          .numresolutions = 2,
                          .qmfbid = 1,
                          .mct = 2,
                          .allocate_image_data = false}));
    enc.SetCustomMct(/*encode_matrix=*/{}, /*decode_matrix=*/{}, {});
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_encode_tile(enc.tcd.get(), 0, &enc.events.manager));
    std::vector<OPJ_INT32> comp_ref(256);
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      opj_tcd_tilecomp_t& tc = enc.tcd->tcd_image->tiles->comps[c];
      ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tc));
      for (size_t i = 0; i < 256; ++i) {
        tc.data[i] = static_cast<OPJ_INT32>((i + c * 31u) & 0xFFu);
        if (c == 0) {
          comp_ref[i] = tc.data[i];
        }
      }
    }
    std::vector<OPJ_BYTE> bytes(8192, 0);
    OPJ_UINT32 written = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_encode_tile(enc.tcd.get(), 0, bytes.data(), &written,
                                  static_cast<OPJ_UINT32>(bytes.size()),
                                  nullptr, nullptr, &enc.events.manager));

    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 16,
                          .height = 16,
                          .numcomps = 3,
                          .prec = 8,
                          .tdx = 16,
                          .tdy = 16,
                          .numresolutions = 2,
                          .qmfbid = 1,
                          .mct = 2,
                          .allocate_image_data = false}));
    dec.SetCustomMct(/*encode_matrix=*/{}, /*decode_matrix=*/{}, {});
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_decode_tile(dec.tcd.get(), 0, 0, 16, 16, 0,
                                            nullptr, bytes.data(), written, 0,
                                            nullptr, &dec.events.manager));
    EXPECT_THAT(ExtractTileSamples(dec.tcd->tcd_image->tiles->comps[0]),
                ElementsAreArray(comp_ref));
  }

  // 3. Direct custom MCT == 2 with 0/1 permutation matrix (preserves float
  // bit patterns in opj_mct_encode_custom without signed int overflow) and
  // asymmetric dc_shifts across whole-tile and partial windowed decoding.
  {
    constexpr std::array<OPJ_FLOAT32, 9> kPerm = {
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    };
    constexpr std::array<OPJ_INT32, 3> kDcShift = {100, 128, 150};

    DirectTcdHarness enc;
    ASSERT_TRUE(enc.Init({.is_decoder = OPJ_FALSE,
                          .width = 24,
                          .height = 24,
                          .numcomps = 3,
                          .prec = 8,
                          .tdx = 24,
                          .tdy = 24,
                          .numresolutions = 3,
                          .qmfbid = 0,
                          .mct = 2,
                          .allocate_image_data = false}));
    enc.SetCustomMct(kPerm, kPerm, kDcShift);
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_encode_tile(enc.tcd.get(), 0, &enc.events.manager));

    std::array<std::vector<OPJ_INT32>, 3> input;
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      input[c].resize(24u * 24u);
      opj_tcd_tilecomp_t& tc = enc.tcd->tcd_image->tiles->comps[c];
      ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tc));
      for (size_t i = 0; i < input[c].size(); ++i) {
        input[c][i] = static_cast<OPJ_INT32>((i * 7u + c * 61u) & 0xFFu);
        tc.data[i] = input[c][i];
      }
    }
    std::vector<OPJ_BYTE> encoded(32768, 0);
    OPJ_UINT32 written = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_encode_tile(enc.tcd.get(), 0, encoded.data(), &written,
                                  static_cast<OPJ_UINT32>(encoded.size()),
                                  nullptr, nullptr, &enc.events.manager));

    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 24,
                          .height = 24,
                          .numcomps = 3,
                          .prec = 8,
                          .tdx = 24,
                          .tdy = 24,
                          .numresolutions = 3,
                          .qmfbid = 0,
                          .mct = 2,
                          .allocate_image_data = false}));
    dec.SetCustomMct(kPerm, kPerm, kDcShift);
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));

    // Whole-tile decode with mct == 2.
    ASSERT_EQ(OPJ_TRUE, opj_tcd_decode_tile(dec.tcd.get(), 0, 0, 24, 24, 0,
                                            nullptr, encoded.data(), written, 0,
                                            nullptr, &dec.events.manager));
    EXPECT_EQ(OPJ_TRUE, dec.tcd->whole_tile_decoding);
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      const opj_tcd_tilecomp_t& tc = dec.tcd->tcd_image->tiles->comps[c];
      for (size_t i = 0; i < 24u * 24u; ++i) {
        EXPECT_LE(std::abs(input[c][i] - tc.data[i]), 4);
      }
    }

    // Partial windowed decode [4, 20) x [4, 20) with mct == 2.
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_decode_tile(dec.tcd.get(), 4, 4, 20, 20, 0,
                                            nullptr, encoded.data(), written, 0,
                                            nullptr, &dec.events.manager));
    EXPECT_EQ(OPJ_FALSE, dec.tcd->whole_tile_decoding);
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      const opj_tcd_tilecomp_t& tc = dec.tcd->tcd_image->tiles->comps[c];
      ASSERT_NE(nullptr, tc.data_win);
      for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
          EXPECT_LE(std::abs(input[c][(y + 4) * 24 + (x + 4)] -
                             tc.data_win[y * 16 + x]),
                    kIrreversibleMaxAbsError);
        }
      }
    }
  }
}

TEST(TcdTest, RoiShiftAndPtermModes) {
  // 1. ROI upshift on component 0 preserves exact ROI pixels while component 1
  // encodes normally.
  {
    OpjImagePtr src = CreateTestImage(16, 16, 2, /*prec=*/8, /*sgnd=*/0,
                                      OPJ_CLRSPC_UNSPECIFIED);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_disto_alloc = 1;
    cparams.tcp_numlayers = 1;
    cparams.tcp_rates[0] = 0;
    cparams.numresolution = 3;
    cparams.roi_compno = 0;
    cparams.roi_shift = 5;

    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());

    MemReadStream mem_in(*encoded);
    OpjStreamPtr stream = mem_in.CreateStream();
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_img = nullptr;
    ASSERT_EQ(OPJ_TRUE, opj_read_header(stream.get(), decomp.get(), &raw_img));
    OpjImagePtr dec_img(raw_img);
    ASSERT_EQ(OPJ_TRUE, opj_decode(decomp.get(), stream.get(), dec_img.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), stream.get()));
    EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[0].data, 256),
                ElementsAreArray(expected[0]));
    EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[1].data, 256),
                ElementsAreArray(expected[1]));
  }

  // 2. J2K_CCP_CBLKSTY_PTERM across multi-layer encode: decode all layers
  // (check_pterm == OPJ_TRUE) vs partial layers (check_pterm == OPJ_FALSE).
  {
    OpjImagePtr src =
        CreateTestImage(24, 24, 1, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_disto_alloc = 1;
    cparams.tcp_numlayers = 2;
    cparams.tcp_rates[0] = 10.0f;
    cparams.tcp_rates[1] = 0.0f;
    cparams.numresolution = 3;
    cparams.mode = J2K_CCP_CBLKSTY_PTERM;

    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());

    for (const OPJ_UINT32 layer_limit : {0u, 1u}) {
      SCOPED_TRACE(layer_limit);
      MemReadStream mem_in(*encoded);
      OpjStreamPtr stream = mem_in.CreateStream();
      OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
      opj_dparameters_t dparams;
      opj_set_default_decoder_parameters(&dparams);
      dparams.cp_layer = layer_limit;
      ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
      opj_image_t* raw_img = nullptr;
      ASSERT_EQ(OPJ_TRUE,
                opj_read_header(stream.get(), decomp.get(), &raw_img));
      OpjImagePtr dec_img(raw_img);
      ASSERT_EQ(OPJ_TRUE,
                opj_decode(decomp.get(), stream.get(), dec_img.get()));
      ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), stream.get()));
      if (layer_limit == 0) {
        EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[0].data, 576),
                    ElementsAreArray(expected[0]));
      } else {
        // Partial-layer decode primarily targets the check_pterm == OPJ_FALSE
        // early-stop path; assert output stays within 8-bit unsigned range.
        for (int i = 0; i < 576; ++i) {
          EXPECT_GE(dec_img->comps[0].data[i], 0);
          EXPECT_LE(dec_img->comps[0].data[i], 255);
        }
      }
    }
  }
}

TEST(TcdTest, SubbandAreaOfInterestAndPartialWindowDecode) {
  // 1. Direct checks on opj_tcd_is_subband_area_of_interest covering qmfbid
  // filter margins (2 vs 3), resno 0 vs > 0, nb == 0 vs > 0, bandno 0..3,
  // ceildivpow2 vs clamped zero branches, and intersecting vs disjoint bounds.
  {
    DirectTcdHarness harness;
    ASSERT_TRUE(harness.Init({.is_decoder = OPJ_TRUE,
                              .width = 64,
                              .height = 64,
                              .numcomps = 2,
                              .dx = 2,
                              .dy = 2,
                              .tdx = 64,
                              .tdy = 64,
                              .numresolutions = 4,
                              .qmfbid = 1,
                              .allocate_image_data = false}));
    harness.tccps[1].qmfbid = 0;  // filter_margin = 3 on comp 1 vs 2 on comp 0
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(harness.tcd.get(), 0,
                                                 &harness.events.manager));
    harness.tcd->tcp = &harness.cp.tcps[0];
    harness.tcd->win_x0 = 40;
    harness.tcd->win_y0 = 40;
    harness.tcd->win_x1 = 56;
    harness.tcd->win_y1 = 56;

    // Comp 0 (qmfbid=1, margin=2): resno=0 (nb=3) maps [40,56) -> [5,7) with
    // margin -> [3,9). [3,9) intersects [4,8) but is disjoint from [0,3).
    EXPECT_EQ(OPJ_TRUE, opj_tcd_is_subband_area_of_interest(
                            harness.tcd.get(), 0, 0, 0, 4, 4, 8, 8));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_is_subband_area_of_interest(
                             harness.tcd.get(), 0, 0, 0, 0, 0, 3, 3));
    // bandno = 1 (HL), 2 (LH), 3 (HH) at resno = 3 (nb = 1, (1<<0)=1).
    EXPECT_EQ(OPJ_TRUE, opj_tcd_is_subband_area_of_interest(
                            harness.tcd.get(), 0, 3, 1, 18, 18, 25, 25));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_is_subband_area_of_interest(
                             harness.tcd.get(), 0, 3, 2, 0, 0, 4, 4));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_is_subband_area_of_interest(
                             harness.tcd.get(), 0, 3, 3, 32, 32, 40, 40));

    // Near tile origin: tcx0 <= (1<<(nb-1))*x0b and tbx0 < filter_margin -> 0.
    harness.tcd->win_x0 = 1;
    harness.tcd->win_y0 = 1;
    harness.tcd->win_x1 = 4;
    harness.tcd->win_y1 = 4;
    EXPECT_EQ(OPJ_TRUE, opj_tcd_is_subband_area_of_interest(
                            harness.tcd.get(), 0, 3, 3, 0, 0, 2, 2));

    // Subsampled comp 1 (dx=2, dy=2, qmfbid=0, margin=3): [40,56) -> [20,28).
    harness.tcd->win_x0 = 40;
    harness.tcd->win_y0 = 40;
    harness.tcd->win_x1 = 56;
    harness.tcd->win_y1 = 56;
    // At resno = 2 (nb = 2): tbx0=ceil(20/4)=5 (clamped margin->2),
    // tbx1=7+3=10.
    EXPECT_EQ(OPJ_TRUE, opj_tcd_is_subband_area_of_interest(
                            harness.tcd.get(), 1, 2, 1, 2, 2, 6, 6));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_is_subband_area_of_interest(
                             harness.tcd.get(), 1, 2, 1, 10, 10, 14, 14));

    // nb == 0 path (numresolutions == 1, resno == 0).
    DirectTcdHarness one_res;
    ASSERT_TRUE(one_res.Init({.is_decoder = OPJ_TRUE,
                              .width = 16,
                              .height = 16,
                              .numresolutions = 1,
                              .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(one_res.tcd.get(), 0,
                                                 &one_res.events.manager));
    one_res.tcd->tcp = &one_res.cp.tcps[0];
    one_res.tcd->win_x0 = 6;
    one_res.tcd->win_y0 = 6;
    one_res.tcd->win_x1 = 10;
    one_res.tcd->win_y1 = 10;
    EXPECT_EQ(OPJ_TRUE, opj_tcd_is_subband_area_of_interest(
                            one_res.tcd.get(), 0, 0, 0, 4, 4, 8, 8));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_is_subband_area_of_interest(
                             one_res.tcd.get(), 0, 0, 0, 13, 13, 16, 16));
  }

  // 2. Partial window decode with precincts/codeblocks, subsampling, cp_reduce,
  // component subsetting (used_component != nullptr), and zero-area crop.
  {
    OpjImagePtr src = CreateTestImage(32, 32, 3, /*prec=*/8, /*sgnd=*/0,
                                      OPJ_CLRSPC_SYCC, /*dx=*/2, /*dy=*/2);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams;
    opj_set_default_encoder_parameters(&cparams);
    cparams.cp_disto_alloc = 1;
    cparams.tcp_numlayers = 1;
    cparams.tcp_rates[0] = 0;
    cparams.numresolution = 3;
    cparams.cblockw_init = 8;
    cparams.cblockh_init = 8;
    cparams.res_spec = 3;
    for (int r = 0; r < 3; ++r) {
      cparams.prcw_init[r] = 16;
      cparams.prch_init[r] = 16;
    }
    cparams.tcp_mct = 0;

    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(encoded.has_value());

    // Partial window [8, 24) x [8, 24) selecting components {2, 0}.
    {
      MemReadStream mem_in(*encoded);
      OpjStreamPtr stream = mem_in.CreateStream();
      OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
      opj_dparameters_t dparams;
      opj_set_default_decoder_parameters(&dparams);
      ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
      opj_image_t* raw_img = nullptr;
      ASSERT_EQ(OPJ_TRUE,
                opj_read_header(stream.get(), decomp.get(), &raw_img));
      OpjImagePtr dec_img(raw_img);
      constexpr std::array<OPJ_UINT32, 2> kComps = {2, 0};
      ASSERT_EQ(OPJ_TRUE, opj_set_decoded_components(decomp.get(), 2,
                                                     kComps.data(), OPJ_FALSE));
      ASSERT_EQ(OPJ_TRUE,
                opj_set_decode_area(decomp.get(), dec_img.get(), 8, 8, 24, 24));
      ASSERT_EQ(OPJ_TRUE,
                opj_decode(decomp.get(), stream.get(), dec_img.get()));
      ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), stream.get()));

      // Subset index 0 maps to codestream component 2 (16x16, win
      // [4,12)x[4,12)).
      ASSERT_EQ(8u, dec_img->comps[0].w);
      ASSERT_EQ(8u, dec_img->comps[0].h);
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          EXPECT_EQ(expected[2][(y + 4) * 16 + (x + 4)],
                    dec_img->comps[0].data[y * 8 + x]);
        }
      }
      // Subset index 1 maps to codestream component 0 (32x32, win
      // [8,24)x[8,24)).
      ASSERT_EQ(16u, dec_img->comps[1].w);
      ASSERT_EQ(16u, dec_img->comps[1].h);
      for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
          EXPECT_EQ(expected[0][(y + 8) * 32 + (x + 8)],
                    dec_img->comps[1].data[y * 16 + x]);
        }
      }
    }

    // Whole-tile decode selecting only components {2, 0} skips allocation for
    // unused component 1 while taking the whole_tile_decoding path.
    {
      MemReadStream mem_in(*encoded);
      OpjStreamPtr stream = mem_in.CreateStream();
      OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
      opj_dparameters_t dparams;
      opj_set_default_decoder_parameters(&dparams);
      ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
      opj_image_t* raw_img = nullptr;
      ASSERT_EQ(OPJ_TRUE,
                opj_read_header(stream.get(), decomp.get(), &raw_img));
      OpjImagePtr dec_img(raw_img);
      constexpr std::array<OPJ_UINT32, 2> kComps = {2, 0};
      ASSERT_EQ(OPJ_TRUE, opj_set_decoded_components(decomp.get(), 2,
                                                     kComps.data(), OPJ_FALSE));
      ASSERT_EQ(OPJ_TRUE,
                opj_decode(decomp.get(), stream.get(), dec_img.get()));
      ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), stream.get()));
      EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[0].data, 16u * 16u),
                  ElementsAreArray(expected[2]));
      EXPECT_THAT(std::span<const OPJ_INT32>(dec_img->comps[1].data, 32u * 32u),
                  ElementsAreArray(expected[0]));
    }

    // cp_reduce = 1 whole-tile margin tolerance ([1, 31) fits within 2^1
    // border margin -> whole_tile_decoding == OPJ_TRUE).
    {
      MemReadStream mem_in(*encoded);
      OpjStreamPtr stream = mem_in.CreateStream();
      OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
      opj_dparameters_t dparams;
      opj_set_default_decoder_parameters(&dparams);
      dparams.cp_reduce = 1;
      ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
      opj_image_t* raw_img = nullptr;
      ASSERT_EQ(OPJ_TRUE,
                opj_read_header(stream.get(), decomp.get(), &raw_img));
      OpjImagePtr dec_img(raw_img);
      ASSERT_EQ(OPJ_TRUE,
                opj_set_decode_area(decomp.get(), dec_img.get(), 1, 1, 31, 31));
      ASSERT_EQ(OPJ_TRUE,
                opj_decode(decomp.get(), stream.get(), dec_img.get()));
      ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), stream.get()));
      EXPECT_EQ(15u, dec_img->comps[0].w);
      EXPECT_EQ(15u, dec_img->comps[0].h);
    }
  }

  // 3. Direct partial window decode + update_tile_data (!whole_tile_decoding)
  // plus empty crop window (win_x0 == win_x1 -> data_win == nullptr,
  // l_width == 0 in dc_level_shift_decode) and shift >= 32 guard.
  {
    OpjImagePtr src =
        CreateTestImage(16, 16, 1, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
    ASSERT_NE(nullptr, src);

    DirectTcdHarness enc;
    ASSERT_TRUE(enc.Init({.is_decoder = OPJ_FALSE,
                          .width = 16,
                          .height = 16,
                          .numcomps = 1,
                          .prec = 8,
                          .tdx = 16,
                          .tdy = 16,
                          .numresolutions = 3,
                          .qmfbid = 1,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_encode_tile(enc.tcd.get(), 0, &enc.events.manager));
    opj_tcd_tilecomp_t& enc_tc = enc.tcd->tcd_image->tiles->comps[0];
    ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&enc_tc));
    std::copy_n(src->comps[0].data, 256, enc_tc.data);
    std::vector<OPJ_BYTE> tile_bytes(4096, 0);
    OPJ_UINT32 written = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_encode_tile(enc.tcd.get(), 0, tile_bytes.data(), &written,
                                  static_cast<OPJ_UINT32>(tile_bytes.size()),
                                  nullptr, nullptr, &enc.events.manager));

    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 16,
                          .height = 16,
                          .numcomps = 1,
                          .prec = 8,
                          .tdx = 16,
                          .tdy = 16,
                          .numresolutions = 3,
                          .qmfbid = 1,
                          .allocate_image_data = false}));
    // Non-empty partial window [4, 12) x [4, 12) -> update_tile_data from
    // data_win.
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_decode_tile(dec.tcd.get(), 4, 4, 12, 12, 0,
                                            nullptr, tile_bytes.data(), written,
                                            0, nullptr, &dec.events.manager));
    EXPECT_EQ(OPJ_FALSE, dec.tcd->whole_tile_decoding);
    const OPJ_UINT32 win_bytes =
        opj_tcd_get_decoded_tile_size(dec.tcd.get(), OPJ_TRUE);
    ASSERT_EQ(64u, win_bytes);
    std::vector<OPJ_BYTE> out_win(win_bytes, kByteSentinel);
    ASSERT_EQ(OPJ_TRUE, opj_tcd_update_tile_data(dec.tcd.get(), out_win.data(),
                                                 win_bytes));
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        EXPECT_EQ(src->comps[0].data[(y + 4) * 16 + (x + 4)],
                  static_cast<OPJ_INT32>(out_win[y * 8 + x]));
      }
    }

    // Zero-area window [4, 4) x [4, 4) skips data_win allocation and
    // dc_level_shift_decode loop cleanly.
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_decode_tile(dec.tcd.get(), 4, 4, 4, 4, 0,
                                            nullptr, tile_bytes.data(), written,
                                            0, nullptr, &dec.events.manager));
    EXPECT_EQ(nullptr, dec.tcd->tcd_image->tiles->comps[0].data_win);
    OPJ_BYTE empty_byte = kByteSentinel;
    EXPECT_EQ(OPJ_TRUE,
              opj_tcd_update_tile_data(dec.tcd.get(), &empty_byte, 0));

    // shift >= 32 (numresolutions = 33, m_reduce = 32 -> shift = 32) avoids
    // right-shift >= 32 UB in opj_tcd_is_whole_tilecomp_decoding.
    static_assert(OPJ_J2K_MAXRLVLS == 33);
    DirectTcdHarness shift32;
    ASSERT_TRUE(shift32.Init({.is_decoder = OPJ_TRUE,
                              .width = 1,
                              .height = 1,
                              .numcomps = 1,
                              .prec = 8,
                              .tdx = 1,
                              .tdy = 1,
                              .numresolutions = OPJ_J2K_MAXRLVLS,
                              .m_reduce = OPJ_J2K_MAXRLVLS - 1,
                              .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(shift32.tcd.get(), 0,
                                                 &shift32.events.manager));
    OPJ_BYTE trunc_byte = 0;
    EXPECT_EQ(OPJ_TRUE, opj_tcd_decode_tile(shift32.tcd.get(), 0, 0, 1, 1, 0,
                                            nullptr, &trunc_byte, 0, 0, nullptr,
                                            &shift32.events.manager));
    EXPECT_EQ(OPJ_TRUE, shift32.tcd->whole_tile_decoding);
  }
}

TEST(TcdTest, DecodeErrorPathsAndFloatClamping) {
  // Build a reusable valid single-component 8x8 lossless tile payload.
  DirectTcdHarness enc_ref;
  ASSERT_TRUE(enc_ref.Init({.is_decoder = OPJ_FALSE,
                            .width = 8,
                            .height = 8,
                            .numcomps = 1,
                            .prec = 8,
                            .tdx = 8,
                            .tdy = 8,
                            .numresolutions = 2,
                            .allocate_image_data = false}));
  ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(enc_ref.tcd.get(), 0,
                                               &enc_ref.events.manager));
  opj_tcd_tilecomp_t& ref_tc = enc_ref.tcd->tcd_image->tiles->comps[0];
  ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&ref_tc));
  for (size_t i = 0; i < 64; ++i) {
    ref_tc.data[i] = static_cast<OPJ_INT32>((i * 11u + 3u) & 0xFFu);
  }
  std::vector<OPJ_BYTE> valid_bytes(2048, 0);
  OPJ_UINT32 valid_len = 0;
  ASSERT_EQ(OPJ_TRUE, opj_tcd_encode_tile(
                          enc_ref.tcd.get(), 0, valid_bytes.data(), &valid_len,
                          static_cast<OPJ_UINT32>(valid_bytes.size()), nullptr,
                          nullptr, &enc_ref.events.manager));
  // opj_tcd_decode_tile zero-copies single-segment codeblocks from src and
  // opj_mqc_init_dec_common reads OPJ_COMMON_CBLK_DATA_EXTRA trailing bytes.
  valid_bytes.resize(valid_len + OPJ_COMMON_CBLK_DATA_EXTRA, 0);

  // 1. Disjoint decode window (win_x0 > tile_x1 -> tilec->win_x1 < win_x0).
  {
    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 8,
                          .height = 8,
                          .tdx = 8,
                          .tdy = 8,
                          .numresolutions = 2,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    EXPECT_EQ(OPJ_FALSE,
              opj_tcd_decode_tile(dec.tcd.get(), 16, 0, 24, 8, 0, nullptr,
                                  valid_bytes.data(), valid_len, 0, nullptr,
                                  &dec.events.manager));
    EXPECT_THAT(dec.events.capture.errors,
                ElementsAre(HasSubstr("Invalid tilec->win_xxx")));
  }

  // 2. Truncated tile packet buffer fails opj_tcd_t2_decode.
  {
    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 8,
                          .height = 8,
                          .tdx = 8,
                          .tdy = 8,
                          .numresolutions = 2,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    dec.cp.strict = OPJ_TRUE;
    ASSERT_GT(valid_len, 2u);
    EXPECT_EQ(OPJ_FALSE,
              opj_tcd_decode_tile(dec.tcd.get(), 0, 0, 8, 8, 0, nullptr,
                                  valid_bytes.data(), valid_len - 1u, 0,
                                  nullptr, &dec.events.manager));
  }

  // 3. roishift + numbps >= 31 fails opj_tcd_t1_decode cleanly.
  {
    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 8,
                          .height = 8,
                          .tdx = 8,
                          .tdy = 8,
                          .numresolutions = 2,
                          .roishift = 30,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    EXPECT_EQ(OPJ_FALSE,
              opj_tcd_decode_tile(dec.tcd.get(), 0, 0, 8, 8, 0, nullptr,
                                  valid_bytes.data(), valid_len, 0, nullptr,
                                  &dec.events.manager));
    EXPECT_THAT(dec.events.capture.warnings,
                ::testing::Contains(HasSubstr("unsupported bpno_plus_one")));
  }

  // 4. MCT decode errors:
  //    a) numcomps < 3 with mct == 1 emits error and continues.
  {
    DirectTcdHarness dec;
    ASSERT_TRUE(dec.Init({.is_decoder = OPJ_TRUE,
                          .width = 8,
                          .height = 8,
                          .tdx = 8,
                          .tdy = 8,
                          .numresolutions = 2,
                          .mct = 1,
                          .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_init_decode_tile(dec.tcd.get(), 0, &dec.events.manager));
    EXPECT_EQ(OPJ_TRUE,
              opj_tcd_decode_tile(dec.tcd.get(), 0, 0, 8, 8, 0, nullptr,
                                  valid_bytes.data(), valid_len, 0, nullptr,
                                  &dec.events.manager));
    EXPECT_THAT(dec.events.capture.errors,
                ElementsAre(HasSubstr("inconsistent with a MCT")));
  }

  //    b) Encode a 3-component subsampled (dx=2, dy=2 on comp 1 & 2) tile with
  //    mct=0, then decode with mct=1 to trigger dimension mismatch on both
  //    whole-tile and partial-window decode, plus mismatched
  //    minimum_num_resolutions.
  {
    DirectTcdHarness enc3;
    ASSERT_TRUE(enc3.Init({.is_decoder = OPJ_FALSE,
                           .width = 16,
                           .height = 16,
                           .numcomps = 3,
                           .dx = 2,
                           .dy = 2,
                           .tdx = 16,
                           .tdy = 16,
                           .numresolutions = 2,
                           .mct = 0,
                           .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(enc3.tcd.get(), 0,
                                                 &enc3.events.manager));
    for (OPJ_UINT32 c = 0; c < 3; ++c) {
      opj_tcd_tilecomp_t& tc = enc3.tcd->tcd_image->tiles->comps[c];
      ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tc));
      std::fill_n(tc.data, (c == 0) ? 256 : 64, 128);
    }
    std::vector<OPJ_BYTE> bytes3(4096, 0);
    OPJ_UINT32 len3 = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_encode_tile(enc3.tcd.get(), 0, bytes3.data(), &len3,
                                  static_cast<OPJ_UINT32>(bytes3.size()),
                                  nullptr, nullptr, &enc3.events.manager));

    // Whole-tile minimum_num_resolutions mismatch.
    DirectTcdHarness dec_minres;
    ASSERT_TRUE(dec_minres.Init({.is_decoder = OPJ_TRUE,
                                 .width = 16,
                                 .height = 16,
                                 .numcomps = 3,
                                 .tdx = 16,
                                 .tdy = 16,
                                 .numresolutions = 2,
                                 .mct = 1,
                                 .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(dec_minres.tcd.get(), 0,
                                                 &dec_minres.events.manager));
    dec_minres.tcd->tcd_image->tiles->comps[1].minimum_num_resolutions = 1;
    EXPECT_EQ(OPJ_FALSE,
              opj_tcd_decode_tile(dec_minres.tcd.get(), 0, 0, 16, 16, 0,
                                  nullptr, bytes3.data(), len3, 0, nullptr,
                                  &dec_minres.events.manager));
    EXPECT_THAT(dec_minres.events.capture.errors,
                ElementsAre(HasSubstr("same dimension")));

    // Whole-tile and partial-window dimension mismatches when dx=2, dy=2.
    DirectTcdHarness dec_dim;
    ASSERT_TRUE(dec_dim.Init({.is_decoder = OPJ_TRUE,
                              .width = 16,
                              .height = 16,
                              .numcomps = 3,
                              .dx = 2,
                              .dy = 2,
                              .tdx = 16,
                              .tdy = 16,
                              .numresolutions = 2,
                              .mct = 1,
                              .allocate_image_data = false}));
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(dec_dim.tcd.get(), 0,
                                                 &dec_dim.events.manager));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_decode_tile(dec_dim.tcd.get(), 0, 0, 16, 16, 0,
                                             nullptr, bytes3.data(), len3, 0,
                                             nullptr, &dec_dim.events.manager));
    EXPECT_THAT(dec_dim.events.capture.errors,
                ElementsAre(HasSubstr("same dimension")));

    dec_dim.events.capture.Clear();
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(dec_dim.tcd.get(), 0,
                                                 &dec_dim.events.manager));
    EXPECT_EQ(OPJ_FALSE, opj_tcd_decode_tile(dec_dim.tcd.get(), 2, 2, 12, 12, 0,
                                             nullptr, bytes3.data(), len3, 0,
                                             nullptr, &dec_dim.events.manager));
    EXPECT_THAT(dec_dim.events.capture.errors,
                ElementsAre(HasSubstr("same dimension")));
  }

  // 5. Float saturation (> INT_MAX and < INT_MIN) in
  // opj_tcd_dc_level_shift_decode: encode 1-resolution 28-bit signed pixels at
  // +/-100000 with stepsize=1.0 (expn=28, numgbits=2 -> Mb=29), then decode
  // with matching Mb=29 via expn=0, numgbits=30 so decoder stepsize = 2^28
  // scales coefficients to +/-2.68e13f and clamps both l_max and l_min cleanly.
  {
    DirectTcdHarness enc_clamp;
    ASSERT_TRUE(enc_clamp.Init({.is_decoder = OPJ_FALSE,
                                .width = 4,
                                .height = 4,
                                .numcomps = 1,
                                .prec = 28,
                                .sgnd = 1,
                                .tdx = 4,
                                .tdy = 4,
                                .numresolutions = 1,
                                .qmfbid = 0,
                                .allocate_image_data = false}));
    enc_clamp.tccps[0].stepsizes[0].expn = 28;
    enc_clamp.tccps[0].stepsizes[0].mant = 0;
    enc_clamp.tccps[0].numgbits = 2;
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_encode_tile(enc_clamp.tcd.get(), 0,
                                                 &enc_clamp.events.manager));
    opj_tcd_tilecomp_t& tc_enc = enc_clamp.tcd->tcd_image->tiles->comps[0];
    ASSERT_EQ(OPJ_TRUE, opj_alloc_tile_component_data(&tc_enc));
    for (int i = 0; i < 16; ++i) {
      tc_enc.data[i] = (i < 8) ? 100000 : -100000;
    }
    std::vector<OPJ_BYTE> bytes_clamp(2048, 0);
    OPJ_UINT32 len_clamp = 0;
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_encode_tile(enc_clamp.tcd.get(), 0, bytes_clamp.data(),
                                  &len_clamp,
                                  static_cast<OPJ_UINT32>(bytes_clamp.size()),
                                  nullptr, nullptr, &enc_clamp.events.manager));

    DirectTcdHarness dec_clamp;
    ASSERT_TRUE(dec_clamp.Init({.is_decoder = OPJ_TRUE,
                                .width = 4,
                                .height = 4,
                                .numcomps = 1,
                                .prec = 28,
                                .sgnd = 1,
                                .tdx = 4,
                                .tdy = 4,
                                .numresolutions = 1,
                                .qmfbid = 0,
                                .allocate_image_data = false}));
    dec_clamp.tccps[0].stepsizes[0].expn = 0;
    dec_clamp.tccps[0].stepsizes[0].mant = 0;
    dec_clamp.tccps[0].numgbits = 30;
    ASSERT_EQ(OPJ_TRUE, opj_tcd_init_decode_tile(dec_clamp.tcd.get(), 0,
                                                 &dec_clamp.events.manager));
    ASSERT_EQ(OPJ_TRUE,
              opj_tcd_decode_tile(dec_clamp.tcd.get(), 0, 0, 4, 4, 0, nullptr,
                                  bytes_clamp.data(), len_clamp, 0, nullptr,
                                  &dec_clamp.events.manager));

    constexpr OPJ_INT32 kMax28Signed = (1 << 27) - 1;
    constexpr OPJ_INT32 kMin28Signed = -(1 << 27);
    const opj_tcd_tilecomp_t& tc_dec =
        dec_clamp.tcd->tcd_image->tiles->comps[0];
    for (int i = 0; i < 8; ++i) {
      EXPECT_EQ(kMax28Signed, tc_dec.data[i]);
    }
    for (int i = 8; i < 16; ++i) {
      EXPECT_EQ(kMin28Signed, tc_dec.data[i]);
    }
  }
}

TEST(TcdTest, MultiThreadedEncodeAndDecode) {
  // 1. Create a 3-component image and lossless-ready parameters.
  OpjImagePtr src =
      CreateTestImage(32, 32, 3, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
  ASSERT_NE(nullptr, src);
  const std::vector<std::vector<OPJ_INT32>> expected =
      SnapshotImagePixels(src.get());

  // 2. Encode with 2 worker threads.
  opj_cparameters_t cparams = MakeLosslessParams(3);
  cparams.cblockw_init = 8;
  cparams.cblockh_init = 8;
  std::optional<std::vector<uint8_t>> encoded = CompressImageToBytes(
      src.get(), OPJ_CODEC_J2K, &cparams, nullptr, nullptr, /*num_threads=*/2);
  ASSERT_TRUE(encoded.has_value());

  // 3. Decode with 2 worker threads and verify lossless pixel recovery.
  OpjImagePtr dec_img =
      DecompressBytesToImage(*encoded, OPJ_CODEC_J2K, {.threads = 2});
  ASSERT_NE(nullptr, dec_img);
  ExpectImagePixelsEqual(expected, dec_img.get());
}

TEST(TcdTest, OddTileOriginAndSequentialTileSizeGrowth) {
  // 1. Non-zero image origin (13, 13) to (33, 33) with tx0=0, ty0=0, tdx=16,
  // tdy=16 generates 3x3 grid tiles from 3x3 -> 16x3 -> 16x16 -> 1x1,
  // exercising odd-parity DWT (cas=1), opj_alloc_tile_component_data growth
  // across sequential tiles, empty edge-tile subbands, and partial crop on odd
  // origin.
  OpjImagePtr src = CreateTestImage(/*width=*/20, /*height=*/20, /*numcomps=*/1,
                                    /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_GRAY,
                                    /*dx=*/1, /*dy=*/1, /*x0=*/13, /*y0=*/13);
  ASSERT_NE(nullptr, src);
  const std::vector<std::vector<OPJ_INT32>> expected =
      SnapshotImagePixels(src.get());

  // 2. Encode the tiled stream.
  opj_cparameters_t cparams =
      MakeLosslessParams(3, TileGeometry{.tdx = 16, .tdy = 16});
  std::optional<std::vector<uint8_t>> encoded =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(encoded.has_value());

  // 3. Decode and verify origin coordinates and exact lossless pixels.
  OpjImagePtr dec_img = DecompressBytesToImage(*encoded, OPJ_CODEC_J2K);
  ASSERT_NE(nullptr, dec_img);
  EXPECT_EQ(13u, dec_img->x0);
  EXPECT_EQ(13u, dec_img->y0);
  EXPECT_EQ(33u, dec_img->x1);
  EXPECT_EQ(33u, dec_img->y1);
  ExpectImagePixelsEqual(expected, dec_img.get());
}

// TODO(schwehr): Cover OOM allocation failure branches in opj_tcd_create,
// opj_tcd_init, opj_alloc_tile_component_data, opj_tcd_init_tile,
// opj_tcd_rateallocate, opj_tcd_decode_tile, opj_tcd_t2_decode,
// opj_tcd_mct_encode, opj_tcd_mct_decode, and opj_tcd_t2_encode via
// :openjpeg2_internal_no_malloc and ScopedOpjAllocFailCountdown, plus
// SIZE_MAX overflow branches unreachable on 64-bit LP64.

}  // namespace
}  // namespace openjpeg2
