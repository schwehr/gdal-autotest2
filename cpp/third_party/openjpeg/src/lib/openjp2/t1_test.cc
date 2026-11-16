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
// Unit tests for OpenJPEG Tier-1 entropy coding/decoding (EBCOT) APIs
// (src/lib/openjp2/t1.c and t1.h).
//
// Caveats for google3:
// - Compiled on little-endian Linux with -DMUTEX_pthread=1 and
//   -funsigned-char; Win32, OPJ_BIG_ENDIAN, and debug-print #ifdefs are
//   compiled out.
// - SIMD blocks in opj_t1_clbl_decode_processor (__SSE2__) and
//   opj_t1_cblk_encode_processor (__AVX2__ / __AVX512F__) depend on target
//   compiler flags (--cpu=arm exercises the scalar paths).
// - Inspects internal opj_cp_t / opj_tcd_t / opj_tcd_cblk_* / opj_t1_t
//   fields, uses opj_t1_decode_cblks()'s volatile OPJ_BOOL* status parameter,
//   pins upstream warning substrings, and uses empirical irreversible 9-7 + ICT
//   tolerances (0.5f * stepsize + 1e-3f, <= 4 for 8-bit full decode across x86
//   SIMD and --cpu=arm scalar paths).
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/t1.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/t1.h

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_t1_t) > 0);
static_assert(T1_NMSEDEC_FRACBITS == 6);
static_assert(OPJ_COMMON_CBLK_DATA_EXTRA == 2);

constexpr OPJ_INT32 kDecodeSentinel = -999999;

struct TcdConfig {
  OPJ_UINT32 width = 16;
  OPJ_UINT32 height = 16;
  OPJ_UINT32 numcomps = 1;
  OPJ_UINT32 prec = 12;
  OPJ_UINT32 sgnd = 1;
  OPJ_UINT32 numresolutions = 1;
  OPJ_UINT32 cblkw = 64;
  OPJ_UINT32 cblkh = 64;
  OPJ_UINT32 cblksty = 0;
  OPJ_UINT32 qmfbid = 1;
  OPJ_INT32 roishift = 0;
  OPJ_UINT32 prcw_exp = 15;
  OPJ_UINT32 prch_exp = 15;
  OPJ_UINT32 mct = 0;
  OPJ_UINT32 cp_reduce = 0;
};

OPJ_UINT32 FloorLog2U32(OPJ_UINT32 value) {
  return value > 1U ? static_cast<OPJ_UINT32>(std::bit_width(value) - 1) : 0U;
}

// Owns value-initialized OpenJPEG image, cp/tcp/tccp structs, event capture,
// mutex, and TCD handle for direct opj_t1_encode_cblks / opj_t1_decode_cblks
// testing. Must be destroyed after any external chunk buffers it references
// are no longer used by active thread-pool jobs.
class TcdTileHarness {
 public:
  TcdTileHarness() = default;
  TcdTileHarness(const TcdTileHarness&) = delete;
  TcdTileHarness& operator=(const TcdTileHarness&) = delete;

  bool Init(bool is_decoder, const TcdConfig& cfg, opj_thread_pool_t* tp,
            bool allocate_tilec_data = true, bool create_mutex = true) {
    is_decoder_ = is_decoder;
    events_.Clear();
    manager_ = MakeEventManager(&events_);

    if (create_mutex) {
      manager_mutex_.reset(opj_mutex_create());
      if (!manager_mutex_) {
        return false;
      }
    } else {
      manager_mutex_.reset();
    }

    image_ = {};
    image_.x0 = 0;
    image_.y0 = 0;
    image_.x1 = cfg.width;
    image_.y1 = cfg.height;
    image_.numcomps = cfg.numcomps;
    image_.color_space =
        (cfg.numcomps >= 3U) ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
    comps_.assign(cfg.numcomps, opj_image_comp_t{});
    for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
      comps_[c].dx = 1;
      comps_[c].dy = 1;
      comps_[c].w = cfg.width;
      comps_[c].h = cfg.height;
      comps_[c].prec = cfg.prec;
      comps_[c].sgnd = cfg.sgnd;
    }
    image_.comps = comps_.empty() ? nullptr : comps_.data();

    cp_ = {};
    cp_.tx0 = 0;
    cp_.ty0 = 0;
    cp_.tdx = cfg.width;
    cp_.tdy = cfg.height;
    cp_.tw = 1;
    cp_.th = 1;
    cp_.m_specific_param.m_dec.m_reduce = cfg.cp_reduce;

    tcp_ = {};
    tcp_.numlayers = 1;
    tcp_.num_layers_to_decode = 1;
    tcp_.mct = cfg.mct;

    tccps_.assign(cfg.numcomps, opj_tccp_t{});
    const OPJ_UINT32 cblkw_exp = FloorLog2U32(cfg.cblkw);
    const OPJ_UINT32 cblkh_exp = FloorLog2U32(cfg.cblkh);
    const bool custom_precincts = (cfg.prcw_exp < 15U || cfg.prch_exp < 15U);
    for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
      opj_tccp_t& tccp = tccps_[c];
      tccp.csty = custom_precincts ? J2K_CCP_CSTY_PRT : 0U;
      tccp.numresolutions = cfg.numresolutions;
      tccp.cblkw = cblkw_exp;
      tccp.cblkh = cblkh_exp;
      tccp.cblksty = cfg.cblksty;
      tccp.qmfbid = cfg.qmfbid;
      tccp.qntsty =
          (cfg.qmfbid == 1U) ? J2K_CCP_QNTSTY_NOQNT : J2K_CCP_QNTSTY_SEQNT;
      tccp.numgbits = 2;
      tccp.roishift = cfg.roishift;
      for (OPJ_UINT32 r = 0; r < cfg.numresolutions; ++r) {
        tccp.prcw[r] = cfg.prcw_exp;
        tccp.prch[r] = cfg.prch_exp;
      }
      opj_dwt_calc_explicit_stepsizes(&tccp, cfg.prec);
    }
    tcp_.tccps = tccps_.empty() ? nullptr : tccps_.data();
    cp_.tcps = &tcp_;

    tcd_.reset(opj_tcd_create(is_decoder ? OPJ_TRUE : OPJ_FALSE));
    if (!tcd_) {
      return false;
    }
    if (!opj_tcd_init(tcd_.get(), &image_, &cp_, tp)) {
      return false;
    }
    tcd_->tcd_tileno = 0;
    tcd_->tcp = &tcp_;
    tcd_->whole_tile_decoding = OPJ_TRUE;
    tcd_->win_x0 = 0;
    tcd_->win_y0 = 0;
    tcd_->win_x1 = cfg.width;
    tcd_->win_y1 = cfg.height;

    const OPJ_BOOL tile_ok =
        is_decoder ? opj_tcd_init_decode_tile(tcd_.get(), 0, &manager_)
                   : opj_tcd_init_encode_tile(tcd_.get(), 0, &manager_);
    if (!tile_ok) {
      return false;
    }

    if (allocate_tilec_data) {
      for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
        if (!AllocateTileCompData(c)) {
          return false;
        }
      }
    }
    return true;
  }

  bool AllocateTileCompData(OPJ_UINT32 compno, OPJ_INT32 fill_value = 0) {
    opj_tcd_tilecomp_t* tc = &tile()->comps[compno];
    if (is_decoder_) {
      const opj_tcd_resolution_t* res =
          &tc->resolutions[tc->minimum_num_resolutions - 1U];
      const OPJ_SIZE_T w = static_cast<OPJ_SIZE_T>(res->x1 - res->x0);
      const OPJ_SIZE_T h = static_cast<OPJ_SIZE_T>(res->y1 - res->y0);
      tc->data_size_needed = w * h * sizeof(OPJ_UINT32);
    }
    if (!opj_alloc_tile_component_data(tc)) {
      return false;
    }
    const std::size_t count = tc->data_size_needed / sizeof(OPJ_INT32);
    if (tc->data != nullptr && count > 0) {
      std::fill_n(tc->data, count, fill_value);
    }
    return true;
  }

  opj_tcd_t* tcd() { return tcd_.get(); }
  opj_tcd_tile_t* tile() { return tcd_->tcd_image->tiles; }
  opj_tcd_tilecomp_t* tilecomp(OPJ_UINT32 c) { return &tile()->comps[c]; }
  opj_tcp_t* tcp() { return &tcp_; }
  opj_tccp_t* tccp(OPJ_UINT32 c) { return &tccps_[c]; }
  opj_event_mgr_t* manager() { return &manager_; }
  opj_mutex_t* manager_mutex() { return manager_mutex_.get(); }
  const EventCapture& events() const { return events_; }
  void ClearEvents() { events_.Clear(); }

 private:
  bool is_decoder_ = false;
  EventCapture events_;
  opj_event_mgr_t manager_{};
  opj_image_t image_{};
  std::vector<opj_image_comp_t> comps_;
  opj_cp_t cp_{};
  opj_tcp_t tcp_{};
  std::vector<opj_tccp_t> tccps_;
  OpjMutexPtr manager_mutex_;
  OpjTcdPtr tcd_;
};

// Pointer-stable backing store for encoded codeblock byte chunks supplied to
// opj_tcd_cblk_dec_t::chunks[]. Each buffer carries OPJ_COMMON_CBLK_DATA_EXTRA
// writable sentinel bytes at the end for opj_mqc_init_dec /
// opj_mqc_raw_init_dec.
struct ChunkStorage {
  std::deque<std::vector<OPJ_BYTE>> buffers;

  OPJ_BYTE* AddBuffer(std::span<const OPJ_BYTE> payload,
                      std::size_t extra_zero_padding = 0) {
    auto& dst = buffers.emplace_back();
    dst.reserve(payload.size() + extra_zero_padding +
                OPJ_COMMON_CBLK_DATA_EXTRA);
    dst.insert(dst.end(), payload.begin(), payload.end());
    dst.insert(dst.end(), extra_zero_padding, 0x00);
    dst.insert(dst.end(), OPJ_COMMON_CBLK_DATA_EXTRA, 0xA5);
    return dst.data();
  }
};

// Populates a decoder codeblock's segments and chunks from an encoder
// codeblock. Allocates dec->chunks with opj_calloc so opj_tcd_destroy frees it.
void CopyEncodedCodeblock(const opj_tcd_cblk_enc_t& enc,
                          opj_tcd_cblk_dec_t* dec, ChunkStorage* storage,
                          OPJ_UINT32 max_chunks = 1,
                          std::size_t extra_trailing_bytes = 0) {
  ASSERT_NE(nullptr, dec);
  ASSERT_NE(nullptr, storage);
  EXPECT_EQ(enc.x0, dec->x0);
  EXPECT_EQ(enc.y0, dec->y0);
  EXPECT_EQ(enc.x1, dec->x1);
  EXPECT_EQ(enc.y1, dec->y1);

  dec->numbps = enc.numbps;
  dec->corrupted = OPJ_FALSE;

  if (enc.totalpasses == 0 || enc.numbps == 0) {
    dec->numsegs = 0;
    dec->real_num_segs = 0;
    dec->numchunks = 0;
    return;
  }

  // Group consecutive passes into segments terminated wherever pass.term == 1.
  std::vector<opj_tcd_seg_t> segs;
  OPJ_UINT32 seg_start_rate = 0;
  OPJ_UINT32 seg_passes = 0;
  for (OPJ_UINT32 p = 0; p < enc.totalpasses; ++p) {
    const opj_tcd_pass_t& pass = enc.passes[p];
    ++seg_passes;
    if (pass.term != 0 || p + 1U == enc.totalpasses) {
      opj_tcd_seg_t seg{};
      seg.len = pass.rate - seg_start_rate;
      seg.numpasses = seg_passes;
      seg.real_num_passes = seg_passes;
      seg.maxpasses = seg_passes;
      segs.push_back(seg);
      seg_start_rate = pass.rate;
      seg_passes = 0;
    }
  }
  ASSERT_FALSE(segs.empty());
  if (extra_trailing_bytes > 0) {
    segs.back().len += static_cast<OPJ_UINT32>(extra_trailing_bytes);
  }

  if (segs.size() > dec->m_current_max_segs) {
    auto* new_segs = static_cast<opj_tcd_seg_t*>(
        opj_realloc(dec->segs, segs.size() * sizeof(opj_tcd_seg_t)));
    ASSERT_NE(nullptr, new_segs);
    dec->segs = new_segs;
    dec->m_current_max_segs = static_cast<OPJ_UINT32>(segs.size());
  }
  std::memcpy(dec->segs, segs.data(), segs.size() * sizeof(opj_tcd_seg_t));
  dec->numsegs = static_cast<OPJ_UINT32>(segs.size());
  dec->real_num_segs = static_cast<OPJ_UINT32>(segs.size());

  const OPJ_UINT32 total_bytes = enc.passes[enc.totalpasses - 1U].rate;
  const OPJ_UINT32 chunk_count =
      std::min(std::max(1U, max_chunks), std::max(1U, total_bytes));
  if (dec->chunks != nullptr) {
    opj_free(dec->chunks);
    dec->chunks = nullptr;
  }
  dec->chunks = static_cast<opj_tcd_seg_data_chunk_t*>(
      opj_calloc(chunk_count, sizeof(opj_tcd_seg_data_chunk_t)));
  ASSERT_NE(nullptr, dec->chunks);
  dec->numchunks = chunk_count;
  dec->numchunksalloc = chunk_count;

  const std::span<const OPJ_BYTE> full_span(enc.data, total_bytes);
  OPJ_UINT32 offset = 0;
  for (OPJ_UINT32 c = 0; c < chunk_count; ++c) {
    const OPJ_UINT32 remaining = total_bytes - offset;
    const OPJ_UINT32 chunks_left_after = chunk_count - c - 1U;
    const OPJ_UINT32 len = (chunks_left_after == 0U)
                               ? remaining
                               : std::min(std::max(1U, remaining / 2U),
                                          remaining - chunks_left_after);
    const std::size_t pad =
        (chunks_left_after == 0U) ? extra_trailing_bytes : 0U;
    dec->chunks[c].len = len + static_cast<OPJ_UINT32>(pad);
    dec->chunks[c].data =
        storage->AddBuffer(full_span.subspan(offset, len), pad);
    offset += len;
  }
}

void CopyEncodedTile(TcdTileHarness& enc, TcdTileHarness& dec,
                     ChunkStorage* storage, OPJ_UINT32 max_chunks = 1) {
  ASSERT_EQ(enc.tile()->numcomps, dec.tile()->numcomps);
  for (OPJ_UINT32 compno = 0; compno < enc.tile()->numcomps; ++compno) {
    const opj_tcd_tilecomp_t* etc = enc.tilecomp(compno);
    opj_tcd_tilecomp_t* dtc = dec.tilecomp(compno);
    ASSERT_EQ(etc->numresolutions, dtc->numresolutions);
    for (OPJ_UINT32 resno = 0; resno < etc->numresolutions; ++resno) {
      const opj_tcd_resolution_t* eres = &etc->resolutions[resno];
      opj_tcd_resolution_t* dres = &dtc->resolutions[resno];
      ASSERT_EQ(eres->numbands, dres->numbands);
      for (OPJ_UINT32 bandno = 0; bandno < eres->numbands; ++bandno) {
        const opj_tcd_band_t* eband = &eres->bands[bandno];
        opj_tcd_band_t* dband = &dres->bands[bandno];
        if (opj_tcd_is_band_empty(const_cast<opj_tcd_band_t*>(eband))) {
          continue;
        }
        for (OPJ_UINT32 precno = 0; precno < eres->pw * eres->ph; ++precno) {
          const opj_tcd_precinct_t* eprc = &eband->precincts[precno];
          opj_tcd_precinct_t* dprc = &dband->precincts[precno];
          ASSERT_EQ(eprc->cw * eprc->ch, dprc->cw * dprc->ch);
          for (OPJ_UINT32 cblkno = 0; cblkno < eprc->cw * eprc->ch; ++cblkno) {
            ASSERT_NO_FATAL_FAILURE(CopyEncodedCodeblock(
                eprc->cblks.enc[cblkno], &dprc->cblks.dec[cblkno], storage,
                max_chunks));
          }
        }
      }
    }
  }
}

// Fills a (w x h) codeblock region inside `data` (row stride `stride`) with a
// deterministic signed pattern that explicitly exercises:
// - all-zero stripes (agg == 1, runlen == 4 -> !v),
// - runlen == 0, 1, 2, 3 in cleanup pass when w >= 12 && h >= 5,
// - 4-sample columns becoming significant together in sigpass (setting all 4
//   T1_PI and T1_SIGMA bits for refpass/clnpass edge paths when w >= 17 && h >=
//   5),
// - large (+/-255..1023), small (+/-1..15 for RAW/LAZY and bpno == 0), and 0.
void FillRichInt32Block(OPJ_INT32* data, OPJ_UINT32 w, OPJ_UINT32 h,
                        OPJ_UINT32 stride, uint32_t seed = 0x13579BDFU,
                        OPJ_INT32 scale = 1) {
  for (OPJ_UINT32 y = 0; y < h; ++y) {
    for (OPJ_UINT32 x = 0; x < w; ++x) {
      data[y * stride + x] = 0;
    }
  }

  bool any_nonzero = false;
  OPJ_UINT32 start_y = 0;
  if (w >= 17U && h >= 5U) {
    // Top stripe (y = 0..3) and guard row (y = 4) left isolated so pass 0
    // (bpno = 10 for scale = 1) hits all-zero !v and runlen 0..3 in clnpass.
    data[0 * stride + 2] = 1200 * scale;    // runlen == 0
    data[1 * stride + 5] = -1180 * scale;   // runlen == 1
    data[2 * stride + 8] = 1150 * scale;    // runlen == 2
    data[3 * stride + 11] = -1130 * scale;  // runlen == 3

    // Adjacent columns (x = 15, 16) trigger all-4-PI + all-4-SIGMA in bpno 9.
    for (OPJ_UINT32 r = 0; r < 4U; ++r) {
      data[r * stride + 15] = 1200 * scale;
      data[r * stride + 16] = 600 * scale;
    }
    any_nonzero = true;
    start_y = 5U;
  }

  uint32_t rng = seed | 1U;
  for (OPJ_UINT32 y = start_y; y < h; ++y) {
    for (OPJ_UINT32 x = 0; x < w; ++x) {
      rng = rng * 1664525U + 1013904223U;
      const uint32_t bucket = (rng >> 28) & 0xFU;
      OPJ_INT32 mag = 0;
      if (bucket < 4U) {
        mag = 0;
      } else if (bucket < 8U) {
        // Small magnitudes (< 16) stay insignificant until LAZY RAW passes.
        mag = static_cast<OPJ_INT32>((rng & 0xEU) >> 1U) + 1;
      } else if (bucket < 12U) {
        mag = static_cast<OPJ_INT32>((rng >> 16) & 0xFFU) + 16;
      } else {
        mag = static_cast<OPJ_INT32>((rng >> 12) & 0x1FFU) + 128;
      }
      if (mag != 0) {
        any_nonzero = true;
        mag = mag * scale + static_cast<OPJ_INT32>(rng & 0x3U);
        if ((rng & 0x80000000U) != 0U) {
          mag = -mag;
        }
      }
      data[y * stride + x] = mag;
    }
  }

  if (!any_nonzero && w > 0U && h > 0U) {
    data[0] = 42 * scale;
  }
}

void FillRichFloatBlockAsBits(OPJ_INT32* data, OPJ_UINT32 w, OPJ_UINT32 h,
                              OPJ_UINT32 stride, uint32_t seed = 0x2468ACE1U) {
  std::vector<OPJ_INT32> ints(static_cast<std::size_t>(w) * h, 0);
  FillRichInt32Block(ints.data(), w, h, w, seed, /*scale=*/1);
  for (OPJ_UINT32 y = 0; y < h; ++y) {
    for (OPJ_UINT32 x = 0; x < w; ++x) {
      const float f = static_cast<float>(ints[y * w + x]);
      data[y * stride + x] = std::bit_cast<OPJ_INT32>(f);
    }
  }
}

void VerifyTileLosslessRoundTrip(const TcdConfig& cfg, opj_thread_pool_t* tp,
                                 uint32_t seed, OPJ_INT32 scale = 1,
                                 OPJ_UINT32 max_chunks = 1) {
  TcdTileHarness enc;
  ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp));

  std::vector<std::vector<OPJ_INT32>> expected(cfg.numcomps);
  const std::size_t count = static_cast<std::size_t>(cfg.width) * cfg.height;
  for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
    expected[c].resize(count);
    FillRichInt32Block(expected[c].data(), cfg.width, cfg.height, cfg.width,
                       seed + c * 97U, scale);
    std::copy_n(expected[c].data(), count, enc.tilecomp(c)->data);
  }

  ASSERT_EQ(OPJ_TRUE,
            opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(), nullptr, 0));
  EXPECT_GT(enc.tile()->distotile, 0.0);

  TcdTileHarness dec;
  ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp));
  for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
    std::fill_n(dec.tilecomp(c)->data, count, kDecodeSentinel);
  }

  ChunkStorage storage;
  ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage, max_chunks));

  volatile OPJ_BOOL ret = OPJ_TRUE;
  const OPJ_BOOL check_pterm =
      (cfg.cblksty & J2K_CCP_CBLKSTY_PTERM) != 0 ? OPJ_TRUE : OPJ_FALSE;
  {
    absl::Cleanup wait_tp = [tp] { opj_thread_pool_wait_completion(tp, 0); };
    for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(c), dec.tccp(c),
                          dec.manager(), dec.manager_mutex(), check_pterm);
    }
  }
  ASSERT_EQ(OPJ_TRUE, ret);
  EXPECT_THAT(dec.events().errors, IsEmpty());
  EXPECT_THAT(dec.events().warnings, IsEmpty());

  for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
    EXPECT_THAT(std::span<const OPJ_INT32>(dec.tilecomp(c)->data, count),
                ElementsAreArray(expected[c]))
        << "component " << c;
  }
}

TEST(T1Test, CreateAndDestroyHandlesNullAndEncoderFlag) {
  // Null-safe destroy.
  opj_t1_destroy(nullptr);

  for (OPJ_BOOL is_encoder : {OPJ_FALSE, OPJ_TRUE}) {
    SCOPED_TRACE(is_encoder);
    OpjT1Ptr t1(opj_t1_create(is_encoder));
    ASSERT_NE(nullptr, t1);
    EXPECT_EQ(is_encoder, t1->encoder);
    EXPECT_EQ(nullptr, t1->data);
    EXPECT_EQ(nullptr, t1->flags);
    EXPECT_EQ(nullptr, t1->cblkdatabuffer);
    EXPECT_EQ(0U, t1->w);
    EXPECT_EQ(0U, t1->h);
    EXPECT_EQ(0U, t1->datasize);
    EXPECT_EQ(0U, t1->flagssize);
    EXPECT_EQ(0U, t1->cblkdatabuffersize);
    EXPECT_EQ(OPJ_FALSE, t1->mustuse_cblkdatabuffer);
  }
}

TEST(T1Test, EncodeDecodeReversibleAllSubbandsAndHeightsMod4) {
  // Reuse one single-threaded pool across increasing and decreasing codeblock
  // sizes to exercise opj_t1_allocate_buffers reallocation and buffer reuse.
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  struct SizeCase {
    std::string_view name;
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    OPJ_UINT32 numresolutions;
    OPJ_UINT32 cblkw;
    OPJ_UINT32 cblkh;
  };
  // Covers h % 4 == 0, 1, 2, 3, width > 16 scalar tails, 1x1 empty subbands,
  // and multi-resolution subband orientations 0..3.
  constexpr SizeCase kCases[] = {
      {.name = "1x1_2res_empty_bands",
       .width = 1,
       .height = 1,
       .numresolutions = 2,
       .cblkw = 4,
       .cblkh = 4},
      {.name = "5x5_h_mod4_1",
       .width = 5,
       .height = 5,
       .numresolutions = 1,
       .cblkw = 16,
       .cblkh = 16},
      {.name = "6x6_h_mod4_2",
       .width = 6,
       .height = 6,
       .numresolutions = 1,
       .cblkw = 16,
       .cblkh = 16},
      {.name = "7x7_h_mod4_3",
       .width = 7,
       .height = 7,
       .numresolutions = 1,
       .cblkw = 16,
       .cblkh = 16},
      {.name = "17x11_generic_h_mod4_3",
       .width = 17,
       .height = 11,
       .numresolutions = 1,
       .cblkw = 32,
       .cblkh = 32},
      // Scalar tail under --cpu=arm when AVX2 is disabled.
      {.name = "25x17_avx_tail_h_mod4_1",
       .width = 25,
       .height = 17,
       .numresolutions = 1,
       .cblkw = 32,
       .cblkh = 32},
      {.name = "26x22_2res_all_orientations",
       .width = 26,
       .height = 22,
       .numresolutions = 2,
       .cblkw = 16,
       .cblkh = 16},
      {.name = "4x4_smaller_reuse",
       .width = 4,
       .height = 4,
       .numresolutions = 1,
       .cblkw = 4,
       .cblkh = 4},
  };

  for (std::size_t idx = 0; idx < std::size(kCases); ++idx) {
    const SizeCase& tc = kCases[idx];
    SCOPED_TRACE(std::string(tc.name));
    const TcdConfig cfg{
        .width = tc.width,
        .height = tc.height,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = tc.numresolutions,
        .cblkw = tc.cblkw,
        .cblkh = tc.cblkh,
        .cblksty = 0,
        .qmfbid = 1,
    };
    ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
        cfg, tp.get(), 0x10203040U + static_cast<uint32_t>(idx)));
  }
}

TEST(T1Test, EncodeDecode64x64FastPathWithAndWithoutVsc) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // Use 14-bit range (scale = 8) on 64x64 so pass bytes regularly include 0xFF
  // and exercise non-terminated pass clamping and trailing 0xFF adjustment.
  for (const OPJ_UINT32 cblksty :
       {OPJ_UINT32{0}, OPJ_UINT32{J2K_CCP_CBLKSTY_VSC},
        OPJ_UINT32{J2K_CCP_CBLKSTY_LAZY | J2K_CCP_CBLKSTY_PTERM |
                   J2K_CCP_CBLKSTY_SEGSYM}}) {
    SCOPED_TRACE(cblksty);
    const TcdConfig cfg{
        .width = 64,
        .height = 64,
        .numcomps = 1,
        .prec = 16,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 64,
        .cblkh = 64,
        .cblksty = cblksty,
        .qmfbid = 1,
    };
    ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
        cfg, tp.get(), 0x55AA1234U + cblksty, /*scale=*/8));
  }
}

TEST(T1Test, EncodeDecodeAllCblkstyModesAndCombinations) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  struct StyleCase {
    std::string_view name;
    OPJ_UINT32 cblksty;
  };
  constexpr StyleCase kStyles[] = {
      {.name = "LAZY", .cblksty = J2K_CCP_CBLKSTY_LAZY},
      {.name = "RESET", .cblksty = J2K_CCP_CBLKSTY_RESET},
      {.name = "TERMALL", .cblksty = J2K_CCP_CBLKSTY_TERMALL},
      {.name = "VSC", .cblksty = J2K_CCP_CBLKSTY_VSC},
      {.name = "PTERM", .cblksty = J2K_CCP_CBLKSTY_PTERM},
      {.name = "SEGSYM", .cblksty = J2K_CCP_CBLKSTY_SEGSYM},
      {.name = "LAZY_PTERM",
       .cblksty = J2K_CCP_CBLKSTY_LAZY | J2K_CCP_CBLKSTY_PTERM},
      {.name = "LAZY_RESET_TERMALL",
       .cblksty = J2K_CCP_CBLKSTY_LAZY | J2K_CCP_CBLKSTY_RESET |
                  J2K_CCP_CBLKSTY_TERMALL},
      {.name = "VSC_SEGSYM_PTERM_RESET",
       .cblksty = J2K_CCP_CBLKSTY_VSC | J2K_CCP_CBLKSTY_SEGSYM |
                  J2K_CCP_CBLKSTY_PTERM | J2K_CCP_CBLKSTY_RESET},
      {.name = "ALL_STANDARD_FLAGS",
       .cblksty = J2K_CCP_CBLKSTY_LAZY | J2K_CCP_CBLKSTY_RESET |
                  J2K_CCP_CBLKSTY_TERMALL | J2K_CCP_CBLKSTY_VSC |
                  J2K_CCP_CBLKSTY_PTERM | J2K_CCP_CBLKSTY_SEGSYM},
  };

  struct Dim {
    OPJ_UINT32 w;
    OPJ_UINT32 h;
  };
  constexpr Dim kDims[] = {{16U, 16U}, {17U, 11U}};

  for (const Dim& dim : kDims) {
    for (const StyleCase& tc : kStyles) {
      SCOPED_TRACE(std::string(tc.name));
      const TcdConfig cfg{
          .width = dim.w,
          .height = dim.h,
          .numcomps = 1,
          .prec = 12,
          .sgnd = 1,
          .numresolutions = 1,
          .cblkw = 32,
          .cblkh = 32,
          .cblksty = tc.cblksty,
          .qmfbid = 1,
      };
      ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
          cfg, tp.get(), 0xCAFE0001U + dim.h + tc.cblksty));
    }
  }
}

TEST(T1Test, EncodeDecodeIrreversibleAndMctNormsAllOrientations) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. Direct qmfbid == 0 encode/decode on non-multiple-of-4 height (17x11)
  // covers irreversible sample conversion and tail loops.
  {
    const TcdConfig cfg{
        .width = 17,
        .height = 11,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 32,
        .cblkh = 32,
        .cblksty = J2K_CCP_CBLKSTY_SEGSYM,
        .qmfbid = 0,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    const std::size_t count = static_cast<std::size_t>(cfg.width) * cfg.height;
    FillRichFloatBlockAsBits(enc.tilecomp(0)->data, cfg.width, cfg.height,
                             cfg.width);
    const std::vector<OPJ_INT32> input_bits(enc.tilecomp(0)->data,
                                            enc.tilecomp(0)->data + count);

    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));
    EXPECT_GT(enc.tile()->distotile, 0.0);

    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get()));
    ChunkStorage storage;
    ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage));

    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    ASSERT_EQ(OPJ_TRUE, ret);

    const float stepsize = dec.tilecomp(0)->resolutions[0].bands[0].stepsize;
    const float enc_stepsize =
        enc.tilecomp(0)->resolutions[0].bands[0].stepsize;
    EXPECT_FLOAT_EQ(enc_stepsize, stepsize);
    for (std::size_t i = 0; i < count; ++i) {
      const float orig = std::bit_cast<float>(input_bits[i]);
      const float actual = std::bit_cast<float>(dec.tilecomp(0)->data[i]);
      EXPECT_NEAR(orig, actual, 0.5f * stepsize + 1e-3f) << "index " << i;
    }
  }

  // 2. Multi-component, multi-resolution (orient = 0, 1, 2, 3) irreversible T1
  // encode with partial mct_norms (mct_numcomps = 2 < numcomps = 3) so both
  // (compno < mct_numcomps) and fallback (w1 = 1) branches run in getwmsedec.
  {
    const TcdConfig cfg{
        .width = 20,
        .height = 18,
        .numcomps = 3,
        .prec = 10,
        .sgnd = 1,
        .numresolutions = 2,
        .cblkw = 16,
        .cblkh = 16,
        .cblksty = 0,
        .qmfbid = 0,
        .mct = 1,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    const std::size_t count = static_cast<std::size_t>(cfg.width) * cfg.height;
    std::vector<std::vector<OPJ_INT32>> input_bits(cfg.numcomps);
    for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
      FillRichFloatBlockAsBits(enc.tilecomp(c)->data, cfg.width, cfg.height,
                               cfg.width, 0x77770001U + c * 31U);
      input_bits[c].assign(enc.tilecomp(c)->data,
                           enc.tilecomp(c)->data + count);
    }
    constexpr std::array<OPJ_FLOAT64, 2> kPartialMctNorms = {1.732, 0.707};
    ASSERT_EQ(OPJ_TRUE,
              opj_t1_encode_cblks(
                  enc.tcd(), enc.tile(), enc.tcp(), kPartialMctNorms.data(),
                  static_cast<OPJ_UINT32>(kPartialMctNorms.size())));
    EXPECT_GT(enc.tile()->distotile, 0.0);

    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get()));
    ChunkStorage storage;
    ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage));

    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
        opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(c), dec.tccp(c),
                            dec.manager(), dec.manager_mutex(), OPJ_FALSE);
      }
    }
    ASSERT_EQ(OPJ_TRUE, ret);
    for (OPJ_UINT32 c = 0; c < cfg.numcomps; ++c) {
      const auto& enc_comp = enc.tilecomp(c);
      const auto& dec_comp = dec.tilecomp(c);
      const OPJ_INT32 r0_w =
          dec_comp->resolutions[0].x1 - dec_comp->resolutions[0].x0;
      const OPJ_INT32 r0_h =
          dec_comp->resolutions[0].y1 - dec_comp->resolutions[0].y0;
      const float ll_enc_step = enc_comp->resolutions[0].bands[0].stepsize;
      const float ll_dec_step = dec_comp->resolutions[0].bands[0].stepsize;
      for (OPJ_UINT32 y = 0; y < cfg.height; ++y) {
        for (OPJ_UINT32 x = 0; x < cfg.width; ++x) {
          const bool in_low_x = static_cast<OPJ_INT32>(x) < r0_w;
          const bool in_low_y = static_cast<OPJ_INT32>(y) < r0_h;
          const OPJ_UINT32 r = (in_low_x && in_low_y) ? 0U : 1U;
          const OPJ_UINT32 b = (in_low_x && in_low_y)    ? 0U
                               : (!in_low_x && in_low_y) ? 0U
                               : (in_low_x && !in_low_y) ? 1U
                                                         : 2U;
          // Without DWT running, dec_step omits the encoder's 1 << log2_gain
          // factor (tcd.c Table E-1 BUG_WEIRD_TWO_INVK), scaling HL/LH by 1/2
          // and HH by 1/4 relative to enc_step.
          const float enc_step =
              (r == 0U) ? ll_enc_step
                        : enc_comp->resolutions[1].bands[b].stepsize;
          const float dec_step =
              (r == 0U) ? ll_dec_step
                        : dec_comp->resolutions[1].bands[b].stepsize;
          const std::size_t idx = static_cast<std::size_t>(y) * cfg.width + x;
          const float expected_val =
              std::bit_cast<float>(input_bits[c][idx]) * (dec_step / enc_step);
          const float actual_val = std::bit_cast<float>(dec_comp->data[idx]);
          // JPEG 2000 deadzone quantization truncates toward zero, bounding
          // reconstruction error by one decoder step size plus FP rounding.
          EXPECT_NEAR(expected_val, actual_val, dec_step + 1e-3f)
              << "comp=" << c << " x=" << x << " y=" << y;
        }
      }
    }
  }

  // 3. Full J2K irreversible 9-7 + ICT round-trip across 3 resolution levels.
  {
    OpjImagePtr src =
        CreateTestImage(32, 32, 3, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams = MakeLosslessParams(3);
    cparams.irreversible = 1;
    cparams.tcp_mct = 1;

    const std::optional<std::vector<uint8_t>> j2k_bytes =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(j2k_bytes.has_value());

    OpjImagePtr decoded = DecompressBytesToImage(*j2k_bytes, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, decoded);
    ASSERT_EQ(3U, decoded->numcomps);
    // Empirical max abs error <= 4 on 8-bit 9-7 + ICT across SSE2 and
    // --cpu=arm.
    for (OPJ_UINT32 c = 0; c < 3U; ++c) {
      for (std::size_t i = 0; i < 32U * 32U; ++i) {
        EXPECT_LE(std::abs(expected[c][i] - decoded->comps[c].data[i]), 4)
            << "c=" << c << " i=" << i;
      }
    }
  }
}

TEST(T1Test, EncodeHandlesZeroBlockAndIntMinEdgeCases) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. All-zero codeblock produces numbps == 0, totalpasses == 0, disto == 0.0,
  // and decodes with numchunks == 0 cleanly.
  {
    const TcdConfig cfg{
        .width = 8,
        .height = 8,
        .numcomps = 1,
        .prec = 8,
        .sgnd = 0,
        .numresolutions = 1,
        .cblkw = 8,
        .cblkh = 8,
        .qmfbid = 1,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));
    const opj_tcd_cblk_enc_t& enc_cblk =
        enc.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.enc[0];
    EXPECT_EQ(0U, enc_cblk.numbps);
    EXPECT_EQ(0U, enc_cblk.totalpasses);
    EXPECT_DOUBLE_EQ(0.0, enc.tile()->distotile);

    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get()));
    std::fill_n(dec.tilecomp(0)->data, 64, kDecodeSentinel);
    ChunkStorage storage;
    ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage));

    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    EXPECT_EQ(OPJ_TRUE, ret);
    EXPECT_THAT(std::span<const OPJ_INT32>(dec.tilecomp(0)->data, 64), Each(0));
  }

  // 2. Sample (1 << 25) shifts left by T1_NMSEDEC_FRACBITS (6) in unsigned
  // arithmetic to 0x80000000 (INT_MIN), exercising the INT_MIN clamp in
  // opj_t1_encode_cblk without signed overflow UB.
  {
    const TcdConfig cfg{
        .width = 4,
        .height = 4,
        .numcomps = 1,
        .prec = 26,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 4,
        .cblkh = 4,
        .qmfbid = 1,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    enc.tilecomp(0)->data[0] =
        static_cast<OPJ_INT32>(1U << (31U - T1_NMSEDEC_FRACBITS));
    enc.tilecomp(0)->data[1] = -7;

    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));
    const opj_tcd_cblk_enc_t& enc_cblk =
        enc.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.enc[0];
    EXPECT_EQ(25U, enc_cblk.numbps);
    EXPECT_GT(enc_cblk.totalpasses, 0U);
    EXPECT_GT(enc.tile()->distotile, 0.0);
  }
}

TEST(T1Test, DecodeMultiChunkAndBufferGrowthRealloc) {
  for (int num_threads : {0, 2}) {
    SCOPED_TRACE(num_threads);
    OpjThreadPoolPtr tp(opj_thread_pool_create(num_threads));
    ASSERT_NE(nullptr, tp);

    // 1. Small 8x8 codeblocks (2 blocks) split into 3 chunks (initial
    // cblkdatabuffer alloc).
    {
      const TcdConfig small_cfg{
          .width = 16,
          .height = 8,
          .numcomps = 1,
          .prec = 12,
          .sgnd = 1,
          .numresolutions = 1,
          .cblkw = 8,
          .cblkh = 8,
          .qmfbid = 1,
      };
      ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
          small_cfg, tp.get(), 0x11112222U, /*scale=*/1, /*max_chunks=*/3));
    }

    // 2. Larger 64x64 codeblocks (2 blocks) on the same thread pool grow
    // cblkdatabuffer via opj_realloc when cblk_len exceeds the previous
    // cblkdatabuffersize.
    {
      const TcdConfig large_cfg{
          .width = 128,
          .height = 64,
          .numcomps = 1,
          .prec = 16,
          .sgnd = 1,
          .numresolutions = 1,
          .cblkw = 64,
          .cblkh = 64,
          .qmfbid = 1,
      };
      ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
          large_cfg, tp.get(), 0x33334444U, /*scale=*/8, /*max_chunks=*/4));
    }

    // 3. Another pair of small 8x8 codeblocks reuses the larger cblkdatabuffer
    // without realloc.
    {
      const TcdConfig small_cfg{
          .width = 16,
          .height = 8,
          .numcomps = 1,
          .prec = 12,
          .sgnd = 1,
          .numresolutions = 1,
          .cblkw = 8,
          .cblkh = 8,
          .qmfbid = 1,
      };
      ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
          small_cfg, tp.get(), 0x55556666U, /*scale=*/1, /*max_chunks=*/2));
    }
  }
}

TEST(T1Test, DecodeRoiShiftShiftsDownNonZeroCoefficients) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. Direct opj_t1_decode_cblks with roishift = 4 covering positive,
  // negative, and zero coefficients (exercising mag >= thresh and mag <
  // thresh).
  {
    constexpr OPJ_INT32 kRoiShift = 4;
    const TcdConfig cfg{
        .width = 8,
        .height = 8,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 8,
        .cblkh = 8,
        .qmfbid = 1,
        .roishift = kRoiShift,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    std::vector<OPJ_INT32> expected(64, 0);
    expected[0] = 24;
    expected[1] = -40;
    expected[9] = 3;
    expected[18] = -5;
    expected[63] = 255;
    std::copy(expected.begin(), expected.end(), enc.tilecomp(0)->data);
    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));

    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get()));
    std::fill_n(dec.tilecomp(0)->data, 64, kDecodeSentinel);
    ChunkStorage storage;
    ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage));

    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    ASSERT_EQ(OPJ_TRUE, ret);
    EXPECT_THAT(
        std::span<const OPJ_INT32>(dec.tilecomp(0)->data, expected.size()),
        ElementsAreArray(expected));
  }

  // 2. End-to-end J2K round-trip with RGN marker (roi_compno = 0, roi_shift =
  // 4).
  {
    OpjImagePtr src =
        CreateTestImage(16, 16, 1, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_GRAY);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams = MakeLosslessParams(/*numresolution=*/2);
    cparams.roi_compno = 0;
    cparams.roi_shift = 4;

    const std::optional<std::vector<uint8_t>> j2k_bytes =
        CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
    ASSERT_TRUE(j2k_bytes.has_value());

    OpjImagePtr decoded = DecompressBytesToImage(*j2k_bytes, OPJ_CODEC_J2K);
    ASSERT_NE(nullptr, decoded);
    ASSERT_EQ(1U, decoded->numcomps);
    ASSERT_EQ(
        expected[0].size(),
        static_cast<std::size_t>(decoded->comps[0].w) * decoded->comps[0].h);
    EXPECT_THAT(
        std::span<const OPJ_INT32>(decoded->comps[0].data, expected[0].size()),
        ElementsAreArray(expected[0]));
  }
}

TEST(T1Test, DecodeErrorAndWarningPaths) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. bpno_plus_one >= 31 triggers warning and OPJ_FALSE, both with and
  // without p_manager_mutex, and aborts remaining codeblocks in
  // opj_t1_decode_cblks.
  for (bool use_mutex : {true, false}) {
    SCOPED_TRACE(use_mutex);
    const TcdConfig cfg{
        .width = 16,
        .height = 8,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 8,
        .cblkh = 8,
        .qmfbid = 1,
        .roishift = 3,
    };
    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get(),
                         /*allocate_tilec_data=*/true,
                         /*create_mutex=*/use_mutex));
    opj_tcd_precinct_t* prc =
        &dec.tilecomp(0)->resolutions[0].bands[0].precincts[0];
    ASSERT_EQ(2U, prc->cw * prc->ch);
    prc->cblks.dec[0].numbps = 28;  // 3 + 28 == 31 >= 31
    prc->cblks.dec[1].numbps = 5;

    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    EXPECT_EQ(OPJ_FALSE, ret);
    EXPECT_THAT(dec.events().warnings,
                ElementsAre(HasSubstr("unsupported bpno_plus_one = 31 >= 31")));
  }

  // 2. Starting opj_t1_decode_cblks with *pret == OPJ_FALSE hits early abort
  // inside opj_t1_clbl_decode_processor and opj_t1_decode_cblks.
  {
    const TcdConfig cfg{
        .width = 8,
        .height = 8,
        .numcomps = 1,
        .prec = 8,
        .sgnd = 0,
        .numresolutions = 1,
        .cblkw = 8,
        .cblkh = 8,
        .qmfbid = 1,
    };
    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get()));
    volatile OPJ_BOOL ret = OPJ_FALSE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    EXPECT_EQ(OPJ_FALSE, ret);
  }

  // 3. Corrupted codeblock flag (numchunks == 0) exits cleanly as OPJ_TRUE.
  {
    const TcdConfig cfg{
        .width = 8,
        .height = 8,
        .numcomps = 1,
        .prec = 8,
        .sgnd = 0,
        .numresolutions = 1,
        .cblkw = 8,
        .cblkh = 8,
        .qmfbid = 1,
    };
    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get()));
    std::fill_n(dec.tilecomp(0)->data, 64, kDecodeSentinel);
    opj_tcd_cblk_dec_t& cblk =
        dec.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.dec[0];
    cblk.corrupted = OPJ_TRUE;
    cblk.numchunks = 0;
    cblk.numbps = 4;

    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    EXPECT_EQ(OPJ_TRUE, ret);
    EXPECT_THAT(std::span<const OPJ_INT32>(dec.tilecomp(0)->data, 64), Each(0));
  }

  // 4. PTERM check failure paths (extra trailing bytes & truncated stream)
  // with and without p_manager_mutex, plus bpno_plus_one >= 1 loop clamp.
  {
    const TcdConfig cfg{
        .width = 16,
        .height = 16,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 16,
        .cblkh = 16,
        .cblksty = J2K_CCP_CBLKSTY_PTERM,
        .qmfbid = 1,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    FillRichInt32Block(enc.tilecomp(0)->data, 16, 16, 16);
    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));
    const opj_tcd_cblk_enc_t& enc_cblk =
        enc.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.enc[0];
    ASSERT_GT(enc_cblk.totalpasses, 0U);

    for (bool use_mutex : {true, false}) {
      SCOPED_TRACE(use_mutex);
      // 4a. Extra trailing bytes: mqc->bp + 2 < mqc->end warning.
      {
        TcdTileHarness dec;
        ASSERT_TRUE(
            dec.Init(/*is_decoder=*/true, cfg, tp.get(), true, use_mutex));
        ChunkStorage storage;
        opj_tcd_cblk_dec_t* dec_cblk =
            &dec.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.dec[0];
        ASSERT_NO_FATAL_FAILURE(
            CopyEncodedCodeblock(enc_cblk, dec_cblk, &storage, /*max_chunks=*/1,
                                 /*extra_trailing_bytes=*/16));

        volatile OPJ_BOOL ret = OPJ_TRUE;
        {
          absl::Cleanup wait_tp = [&tp] {
            opj_thread_pool_wait_completion(tp.get(), 0);
          };
          opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                              dec.manager(), dec.manager_mutex(), OPJ_TRUE);
        }
        EXPECT_EQ(OPJ_TRUE, ret);
        EXPECT_THAT(dec.events().warnings,
                    ElementsAre(HasSubstr("remaining bytes in code block")));
      }

      // 4b. Truncated stream: end_of_byte_stream_counter > 2 warning, plus
      // extra real_num_passes stopped by (bpno_plus_one >= 1).
      {
        TcdTileHarness dec;
        ASSERT_TRUE(
            dec.Init(/*is_decoder=*/true, cfg, tp.get(), true, use_mutex));
        ChunkStorage storage;
        opj_tcd_cblk_dec_t* dec_cblk =
            &dec.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.dec[0];
        ASSERT_NO_FATAL_FAILURE(
            CopyEncodedCodeblock(enc_cblk, dec_cblk, &storage));
        ASSERT_GE(dec_cblk->numsegs, 1U);
        dec_cblk->segs[0].len = 1U;
        dec_cblk->chunks[0].len = 1U;
        dec_cblk->segs[0].real_num_passes += 12U;

        volatile OPJ_BOOL ret = OPJ_TRUE;
        {
          absl::Cleanup wait_tp = [&tp] {
            opj_thread_pool_wait_completion(tp.get(), 0);
          };
          opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                              dec.manager(), dec.manager_mutex(), OPJ_TRUE);
        }
        EXPECT_EQ(OPJ_TRUE, ret);
        EXPECT_THAT(dec.events().warnings,
                    ElementsAre(HasSubstr("synthesized 0xFF markers read")));
      }
    }
  }

  // 5. HTJ2K dispatch (J2K_CCP_CBLKSTY_HT): both empty-block OPJ_TRUE and
  // invalid-ROI OPJ_FALSE paths inside opj_t1_clbl_decode_processor.
  {
    const TcdConfig ht_ok_cfg{
        .width = 4,
        .height = 4,
        .numcomps = 1,
        .prec = 8,
        .sgnd = 0,
        .numresolutions = 1,
        .cblkw = 4,
        .cblkh = 4,
        .cblksty = J2K_CCP_CBLKSTY_HT,
        .qmfbid = 1,
        .roishift = 0,
    };
    TcdTileHarness dec_ok;
    ASSERT_TRUE(dec_ok.Init(/*is_decoder=*/true, ht_ok_cfg, tp.get()));
    std::fill_n(dec_ok.tilecomp(0)->data, 16, kDecodeSentinel);
    opj_tcd_cblk_dec_t& ok_cblk =
        dec_ok.tilecomp(0)->resolutions[0].bands[0].precincts[0].cblks.dec[0];
    ok_cblk.Mb = 0;
    ok_cblk.numchunks = 0;

    volatile OPJ_BOOL ret_ok = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec_ok.tcd(), &ret_ok, dec_ok.tilecomp(0),
                          dec_ok.tccp(0), dec_ok.manager(),
                          dec_ok.manager_mutex(), OPJ_FALSE);
    }
    EXPECT_EQ(OPJ_TRUE, ret_ok);
    EXPECT_THAT(std::span<const OPJ_INT32>(dec_ok.tilecomp(0)->data, 16),
                Each(0));

    TcdConfig ht_err_cfg = ht_ok_cfg;
    ht_err_cfg.roishift = 1;  // HT decoder rejects roishift != 0
    TcdTileHarness dec_err;
    ASSERT_TRUE(dec_err.Init(/*is_decoder=*/true, ht_err_cfg, tp.get()));
    volatile OPJ_BOOL ret_err = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec_err.tcd(), &ret_err, dec_err.tilecomp(0),
                          dec_err.tccp(0), dec_err.manager(),
                          dec_err.manager_mutex(), OPJ_FALSE);
    }
    EXPECT_EQ(OPJ_FALSE, ret_err);
    EXPECT_THAT(dec_err.events().errors, ::testing::Contains(HasSubstr("ROI")));
  }
}

TEST(T1Test, DecodeSubtilePartialAreaReusesAndDiscardsCodeblocks) {
  OpjThreadPoolPtr tp(opj_thread_pool_create(0));
  ASSERT_NE(nullptr, tp);

  // 1. Reversible subtile decoding across a 64x64 tile with 32x32 precincts
  // (2x2 precincts) and 8x8 codeblocks (4x4 codeblocks per precinct).
  {
    const TcdConfig cfg{
        .width = 64,
        .height = 64,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 8,
        .cblkh = 8,
        .qmfbid = 1,
        .prcw_exp = 5,
        .prch_exp = 5,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    FillRichInt32Block(enc.tilecomp(0)->data, 64, 64, 64, 0x76543210U);
    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));

    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get(),
                         /*allocate_tilec_data=*/false));
    ChunkStorage storage;
    ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage));

    opj_tcd_band_t* band = &dec.tilecomp(0)->resolutions[0].bands[0];
    ASSERT_EQ(4U, dec.tilecomp(0)->resolutions[0].ph *
                      dec.tilecomp(0)->resolutions[0].pw);
    opj_tcd_precinct_t* prc0 = &band->precincts[0];
    ASSERT_EQ(16U, prc0->cw * prc0->ch);

    auto run_subtile = [&](OPJ_UINT32 x0, OPJ_UINT32 y0, OPJ_UINT32 x1,
                           OPJ_UINT32 y1) {
      dec.tcd()->whole_tile_decoding = OPJ_FALSE;
      dec.tcd()->win_x0 = x0;
      dec.tcd()->win_y0 = y0;
      dec.tcd()->win_x1 = x1;
      dec.tcd()->win_y1 = y1;
      volatile OPJ_BOOL ret = OPJ_TRUE;
      {
        absl::Cleanup wait_tp = [&tp] {
          opj_thread_pool_wait_completion(tp.get(), 0);
        };
        opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                            dec.manager(), dec.manager_mutex(), OPJ_FALSE);
      }
      ASSERT_EQ(OPJ_TRUE, ret);
    };

    // Step 1: Decode [0, 4) x [0, 4) -> only cblk(0,0) in precinct 0
    // intersects.
    ASSERT_NO_FATAL_FAILURE(run_subtile(0, 0, 4, 4));
    ASSERT_NE(nullptr, prc0->cblks.dec[0].decoded_data);
    EXPECT_EQ(nullptr, prc0->cblks.dec[1].decoded_data);
    EXPECT_EQ(nullptr, band->precincts[3].cblks.dec[0].decoded_data);
    for (OPJ_UINT32 y = 0; y < 8U; ++y) {
      for (OPJ_UINT32 x = 0; x < 8U; ++x) {
        EXPECT_EQ(enc.tilecomp(0)->data[y * 64U + x],
                  prc0->cblks.dec[0].decoded_data[y * 8U + x]);
      }
    }

    // Step 2: Decode [0, 12) x [0, 12) -> cblk(0,0) is reused (already
    // non-null), while cblk(1,0) and cblk(0,1) are newly decoded.
    OPJ_INT32* const cached_ptr = prc0->cblks.dec[0].decoded_data;
    ASSERT_NO_FATAL_FAILURE(run_subtile(0, 0, 12, 12));
    EXPECT_EQ(cached_ptr, prc0->cblks.dec[0].decoded_data);
    EXPECT_NE(nullptr, prc0->cblks.dec[1].decoded_data);

    // Step 3: Decode [19, 20) x [0, 12) -> still intersects precinct 0, so
    // cblk(0,0) and cblk(1,0) are discarded at the codeblock level, cblk(2,0)
    // is decoded, and cblk(3,0) stays untouched (nullptr).
    ASSERT_NO_FATAL_FAILURE(run_subtile(19, 0, 20, 12));
    EXPECT_EQ(nullptr, prc0->cblks.dec[0].decoded_data);
    EXPECT_EQ(nullptr, prc0->cblks.dec[1].decoded_data);
    EXPECT_NE(nullptr, prc0->cblks.dec[2].decoded_data);
    EXPECT_EQ(nullptr, prc0->cblks.dec[3].decoded_data);

    // Step 4: Temporarily make an in-AOI codeblock zero-width to exercise the
    // cblk_w == 0 || cblk_h == 0 skip guard in subtile decoding.
    const OPJ_INT32 saved_x1 = prc0->cblks.dec[3].x1;
    prc0->cblks.dec[3].x1 = prc0->cblks.dec[3].x0;
    ASSERT_NO_FATAL_FAILURE(run_subtile(22, 0, 28, 12));
    EXPECT_EQ(nullptr, prc0->cblks.dec[3].decoded_data);
    prc0->cblks.dec[3].x1 = saved_x1;

    // Step 5: Decode [40, 60) x [40, 60) -> precinct 0 is now completely
    // outside AOI and cblk(2,0) is discarded at the precinct level; precinct 3
    // is decoded.
    ASSERT_NO_FATAL_FAILURE(run_subtile(40, 40, 60, 60));
    EXPECT_EQ(nullptr, prc0->cblks.dec[2].decoded_data);
    EXPECT_NE(nullptr, band->precincts[3].cblks.dec[0].decoded_data);

    // Step 6: Switch back to whole-tile decoding while precinct 3 still has
    // decoded_data != nullptr; opj_t1_clbl_decode_processor frees stale
    // decoded_data and writes the full tile into tilec->data.
    ASSERT_TRUE(dec.AllocateTileCompData(0, kDecodeSentinel));
    dec.tcd()->whole_tile_decoding = OPJ_TRUE;
    dec.tcd()->win_x0 = 0;
    dec.tcd()->win_y0 = 0;
    dec.tcd()->win_x1 = 64;
    dec.tcd()->win_y1 = 64;
    volatile OPJ_BOOL full_ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &full_ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    ASSERT_EQ(OPJ_TRUE, full_ret);
    EXPECT_EQ(nullptr, band->precincts[3].cblks.dec[0].decoded_data);
    EXPECT_THAT(std::span<const OPJ_INT32>(dec.tilecomp(0)->data, 64U * 64U),
                ElementsAreArray(enc.tilecomp(0)->data, 64U * 64U));
  }

  // 2. Irreversible (qmfbid == 0) subtile decoding on 37x37 with 32x32
  // codeblocks exercises both the 16-element SSE2 loop (on 32x32 and 5x5) and
  // the scalar tail loop (9 remaining samples of 5x5 = 25).
  {
    const TcdConfig cfg{
        .width = 37,
        .height = 37,
        .numcomps = 1,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 1,
        .cblkw = 32,
        .cblkh = 32,
        .qmfbid = 0,
    };
    TcdTileHarness enc;
    ASSERT_TRUE(enc.Init(/*is_decoder=*/false, cfg, tp.get()));
    FillRichFloatBlockAsBits(enc.tilecomp(0)->data, 37, 37, 37);
    ASSERT_EQ(OPJ_TRUE, opj_t1_encode_cblks(enc.tcd(), enc.tile(), enc.tcp(),
                                            nullptr, 0));

    TcdTileHarness dec;
    ASSERT_TRUE(dec.Init(/*is_decoder=*/true, cfg, tp.get(),
                         /*allocate_tilec_data=*/false));
    ChunkStorage storage;
    ASSERT_NO_FATAL_FAILURE(CopyEncodedTile(enc, dec, &storage));

    dec.tcd()->whole_tile_decoding = OPJ_FALSE;
    dec.tcd()->win_x0 = 0;
    dec.tcd()->win_y0 = 0;
    dec.tcd()->win_x1 = 37;
    dec.tcd()->win_y1 = 37;
    volatile OPJ_BOOL ret = OPJ_TRUE;
    {
      absl::Cleanup wait_tp = [&tp] {
        opj_thread_pool_wait_completion(tp.get(), 0);
      };
      opj_t1_decode_cblks(dec.tcd(), &ret, dec.tilecomp(0), dec.tccp(0),
                          dec.manager(), dec.manager_mutex(), OPJ_FALSE);
    }
    ASSERT_EQ(OPJ_TRUE, ret);

    const opj_tcd_band_t* band = &dec.tilecomp(0)->resolutions[0].bands[0];
    const opj_tcd_precinct_t* prc = &band->precincts[0];
    ASSERT_EQ(4U, prc->cw * prc->ch);
    const float stepsize = band->stepsize;
    for (OPJ_UINT32 cblkno = 0; cblkno < 4U; ++cblkno) {
      const opj_tcd_cblk_dec_t& cblk = prc->cblks.dec[cblkno];
      ASSERT_NE(nullptr, cblk.decoded_data) << "cblkno " << cblkno;
      const OPJ_UINT32 cw = static_cast<OPJ_UINT32>(cblk.x1 - cblk.x0);
      const OPJ_UINT32 ch = static_cast<OPJ_UINT32>(cblk.y1 - cblk.y0);
      for (OPJ_UINT32 y = 0; y < ch; ++y) {
        for (OPJ_UINT32 x = 0; x < cw; ++x) {
          const OPJ_INT32 orig_bits =
              enc.tilecomp(0)
                  ->data[(static_cast<std::size_t>(cblk.y0) + y) * 37U +
                         (static_cast<std::size_t>(cblk.x0) + x)];
          const float orig = std::bit_cast<float>(orig_bits);
          const float actual =
              std::bit_cast<float>(cblk.decoded_data[y * cw + x]);
          EXPECT_NEAR(orig, actual, 0.5f * stepsize + 1e-3f);
        }
      }
    }
  }
}

TEST(T1Test, MultiThreadedEncodeAndDecodeWithRealThreadPool) {
  // 1. Direct multi-threaded T1 encode/decode across 3 components and multiple
  // codeblocks with a 2-worker opj_thread_pool_t (exercising TLS T1 handles,
  // mutex locking, and mustuse_cblkdatabuffer == OPJ_TRUE).
  {
    OpjThreadPoolPtr tp(opj_thread_pool_create(2));
    ASSERT_NE(nullptr, tp);
    const TcdConfig cfg{
        .width = 64,
        .height = 64,
        .numcomps = 3,
        .prec = 12,
        .sgnd = 1,
        .numresolutions = 2,
        .cblkw = 16,
        .cblkh = 16,
        .cblksty = J2K_CCP_CBLKSTY_LAZY | J2K_CCP_CBLKSTY_SEGSYM |
                   J2K_CCP_CBLKSTY_PTERM,
        .qmfbid = 1,
    };
    ASSERT_NO_FATAL_FAILURE(VerifyTileLosslessRoundTrip(
        cfg, tp.get(), 0x89ABCDEFU, /*scale=*/1, /*max_chunks=*/2));
  }

  // 2. End-to-end 2-thread J2K encode/decode with 2 quality layers (generating
  // multi-chunk codeblocks via T2), custom codeblock size, and PTERM checks.
  {
    OpjImagePtr src =
        CreateTestImage(48, 40, 3, /*prec=*/8, /*sgnd=*/0, OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams = MakeLosslessParams(3);
    cparams.tcp_mct = 1;
    cparams.tcp_numlayers = 2;
    cparams.tcp_rates[0] = 20.0f;
    cparams.tcp_rates[1] = 0.0f;
    cparams.cblockw_init = 16;
    cparams.cblockh_init = 16;
    cparams.mode =
        J2K_CCP_CBLKSTY_PTERM | J2K_CCP_CBLKSTY_SEGSYM | J2K_CCP_CBLKSTY_RESET;

    EventCapture enc_events;
    std::optional<std::vector<uint8_t>> encoded = CompressImageToBytes(
        src.get(), OPJ_CODEC_J2K, &cparams, nullptr, &enc_events,
        /*num_threads=*/2);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_THAT(enc_events.errors, IsEmpty());

    EventCapture dec_events;
    OpjImagePtr decoded = DecompressBytesToImage(
        *encoded, OPJ_CODEC_J2K, {.events = &dec_events, .threads = 2});
    ASSERT_NE(nullptr, decoded);
    EXPECT_THAT(dec_events.errors, IsEmpty());
    EXPECT_THAT(dec_events.warnings, IsEmpty());
    ExpectImagePixelsEqual(expected, decoded.get());
  }
}

// TODO(schwehr): Cover remaining OOM allocation failure guards
// (opj_t1_create/allocate_buffers/encode/decode), unreachable defensive
// checks (roishift >= 31 after opj_t1_decode_cblk rejects >= 31, runlen != 0
// when all 4 SIGMA+PI bits are set), and target-gated AVX2/AVX512 SIMD loops
// in t1.c when compiled without AVX2/AVX512.

}  // namespace
}  // namespace openjpeg2
