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
// Unit tests for the OpenJPEG packet iterator (src/lib/openjp2/pi.c).
//
// Caveats:
// - Tests link against openjpeg2_internal to exercise internal PI APIs
//   (opj_pi_initialise_encode, opj_pi_update_encoding_parameters,
//   opj_pi_create_encode, opj_pi_create_decode, opj_pi_destroy, opj_pi_next,
//   opj_get_encoding_packet_count).
// - Inspects internal opj_pi_iterator_t / opj_cp_t / opj_tcp_t / opj_poc_t
//   fields, relies on opj_pi_destroy() freeing opj_calloc'd iterator members,
//   and pins upstream error substrings.
// - In openjpeg.h, opj_poc_t::{tx0,tx1,ty0,ty1} default to OPJ_INT32 unless
//   OPJ_UINT32_SEMANTICALLY_BUT_INT32 is overridden as in pi.c. Keep test
//   tile/image coordinates non-negative and within INT32_MAX.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/pi.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/pi.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);

struct PacketCoord {
  OPJ_UINT32 layno = 0;
  OPJ_UINT32 resno = 0;
  OPJ_UINT32 compno = 0;
  OPJ_UINT32 precno = 0;

  auto operator<=>(const PacketCoord&) const = default;
};

std::ostream& operator<<(std::ostream& os, const PacketCoord& p) {
  return os << "{L=" << p.layno << ", R=" << p.resno << ", C=" << p.compno
            << ", P=" << p.precno << "}";
}

// RAII wrapper for an opj_pi_iterator_t array of `num_elements` entries.
class ScopedPiIterator {
 public:
  ScopedPiIterator() = default;
  ScopedPiIterator(opj_pi_iterator_t* ptr, OPJ_UINT32 num_elements)
      : ptr_(ptr), num_elements_(num_elements) {}
  ~ScopedPiIterator() { reset(); }

  ScopedPiIterator(const ScopedPiIterator&) = delete;
  ScopedPiIterator& operator=(const ScopedPiIterator&) = delete;

  ScopedPiIterator(ScopedPiIterator&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)),
        num_elements_(std::exchange(other.num_elements_, 0)) {}

  ScopedPiIterator& operator=(ScopedPiIterator&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = std::exchange(other.ptr_, nullptr);
      num_elements_ = std::exchange(other.num_elements_, 0);
    }
    return *this;
  }

  void reset(opj_pi_iterator_t* ptr = nullptr, OPJ_UINT32 num_elements = 0) {
    if (ptr_ != nullptr) {
      opj_pi_destroy(ptr_, num_elements_);
    }
    ptr_ = ptr;
    num_elements_ = num_elements;
  }

  opj_pi_iterator_t* get() const { return ptr_; }
  opj_pi_iterator_t* operator->() const { return ptr_; }
  opj_pi_iterator_t& operator[](std::size_t i) const { return ptr_[i]; }
  explicit operator bool() const { return ptr_ != nullptr; }
  OPJ_UINT32 num_elements() const { return num_elements_; }

 private:
  opj_pi_iterator_t* ptr_ = nullptr;
  OPJ_UINT32 num_elements_ = 0;
};

struct CompSpec {
  OPJ_UINT32 dx = 1;
  OPJ_UINT32 dy = 1;
  OPJ_UINT32 numresolutions = 1;
  std::array<OPJ_UINT32, OPJ_J2K_MAXRLVLS> prcw{};
  std::array<OPJ_UINT32, OPJ_J2K_MAXRLVLS> prch{};
};

struct PocSpec {
  OPJ_UINT32 resno0 = 0;
  OPJ_UINT32 compno0 = 0;
  OPJ_UINT32 layno1 = 1;
  OPJ_UINT32 resno1 = 1;
  OPJ_UINT32 compno1 = 1;
  OPJ_PROG_ORDER prg = OPJ_LRCP;
};

struct TileGridSpec {
  OPJ_UINT32 x0 = 0;
  OPJ_UINT32 y0 = 0;
  OPJ_UINT32 x1 = 8;
  OPJ_UINT32 y1 = 8;
  OPJ_UINT32 tx0 = 0;
  OPJ_UINT32 ty0 = 0;
  OPJ_UINT32 tdx = 8;
  OPJ_UINT32 tdy = 8;
  OPJ_UINT32 tw = 1;
  OPJ_UINT32 th = 1;
  OPJ_UINT32 numlayers = 1;
  OPJ_PROG_ORDER prg = OPJ_LRCP;
  OPJ_UINT16 rsiz = OPJ_PROFILE_NONE;
  bool tp_on = false;
};

// Owns value-initialized image, coding parameters, tile/component configs,
// and an EventCapture-wired opj_event_mgr_t without allocating pixel buffers.
class PiTestModel {
 public:
  PiTestModel() = default;
  PiTestModel(const PiTestModel&) = delete;
  PiTestModel& operator=(const PiTestModel&) = delete;

  void Init(const TileGridSpec& grid, std::span<const CompSpec> comp_specs,
            std::span<const PocSpec> poc_specs = {}) {
    events_.Clear();
    manager_ = MakeEventManager(&events_);

    const OPJ_UINT32 numcomps = static_cast<OPJ_UINT32>(comp_specs.size());
    image_ = {};
    image_.x0 = grid.x0;
    image_.y0 = grid.y0;
    image_.x1 = grid.x1;
    image_.y1 = grid.y1;
    image_.numcomps = numcomps;
    comps_.assign(numcomps, opj_image_comp_t{});
    for (OPJ_UINT32 c = 0; c < numcomps; ++c) {
      comps_[c].dx = comp_specs[c].dx;
      comps_[c].dy = comp_specs[c].dy;
    }
    image_.comps = comps_.empty() ? nullptr : comps_.data();

    const OPJ_UINT32 num_tiles = grid.tw * grid.th;
    cp_ = {};
    cp_.rsiz = grid.rsiz;
    cp_.tx0 = grid.tx0;
    cp_.ty0 = grid.ty0;
    cp_.tdx = grid.tdx;
    cp_.tdy = grid.tdy;
    cp_.tw = grid.tw;
    cp_.th = grid.th;
    cp_.m_specific_param.m_enc = {};
    cp_.m_specific_param.m_enc.m_tp_on = grid.tp_on ? 1 : 0;

    tcps_.assign(num_tiles, opj_tcp_t{});
    tile_tccps_.assign(num_tiles, std::vector<opj_tccp_t>(numcomps));
    for (OPJ_UINT32 t = 0; t < num_tiles; ++t) {
      opj_tcp_t& tcp = tcps_[t];
      tcp.prg = grid.prg;
      tcp.numlayers = grid.numlayers;
      if (!poc_specs.empty()) {
        tcp.POC = 1;
        tcp.numpocs = static_cast<OPJ_UINT32>(poc_specs.size() - 1);
        for (std::size_t p = 0; p < poc_specs.size(); ++p) {
          opj_poc_t& poc = tcp.pocs[p];
          poc.resno0 = poc_specs[p].resno0;
          poc.compno0 = poc_specs[p].compno0;
          poc.layno1 = poc_specs[p].layno1;
          poc.resno1 = poc_specs[p].resno1;
          poc.compno1 = poc_specs[p].compno1;
          poc.prg1 = poc_specs[p].prg;
          poc.prg = poc_specs[p].prg;
          poc.tile = t + 1;
        }
      }
      for (OPJ_UINT32 c = 0; c < numcomps; ++c) {
        opj_tccp_t& tccp = tile_tccps_[t][c];
        tccp.numresolutions = comp_specs[c].numresolutions;
        for (OPJ_UINT32 r = 0; r < OPJ_J2K_MAXRLVLS; ++r) {
          tccp.prcw[r] = comp_specs[c].prcw[r];
          tccp.prch[r] = comp_specs[c].prch[r];
        }
      }
      tcp.tccps = tile_tccps_[t].empty() ? nullptr : tile_tccps_[t].data();
    }
    cp_.tcps = tcps_.empty() ? nullptr : tcps_.data();
  }

  opj_image_t* image() { return &image_; }
  const opj_image_t* image() const { return &image_; }
  opj_cp_t* cp() { return &cp_; }
  const opj_cp_t* cp() const { return &cp_; }
  opj_tcp_t& tcp(OPJ_UINT32 tileno) { return tcps_.at(tileno); }
  opj_event_mgr_t* manager() { return &manager_; }
  const EventCapture& events() const { return events_; }
  void ClearEvents() { events_.Clear(); }

  OPJ_UINT32 PocCount(OPJ_UINT32 tileno) const {
    return tcps_.at(tileno).numpocs + 1;
  }

 private:
  opj_image_t image_{};
  std::vector<opj_image_comp_t> comps_;
  opj_cp_t cp_{};
  std::vector<opj_tcp_t> tcps_;
  std::vector<std::vector<opj_tccp_t>> tile_tccps_;
  EventCapture events_;
  opj_event_mgr_t manager_{};
};

CompSpec MakeUniformComp(OPJ_UINT32 dx, OPJ_UINT32 dy,
                         OPJ_UINT32 numresolutions, OPJ_UINT32 pdx,
                         OPJ_UINT32 pdy) {
  CompSpec spec;
  spec.dx = dx;
  spec.dy = dy;
  spec.numresolutions = numresolutions;
  for (OPJ_UINT32 r = 0; r < numresolutions; ++r) {
    spec.prcw[r] = pdx;
    spec.prch[r] = pdy;
  }
  return spec;
}

std::vector<PacketCoord> DrainIterator(opj_pi_iterator_t* pi,
                                       std::size_t max_packets = 4096) {
  std::vector<PacketCoord> packets;
  while (opj_pi_next(pi)) {
    packets.push_back(PacketCoord{
        .layno = pi->layno,
        .resno = pi->resno,
        .compno = pi->compno,
        .precno = pi->precno,
    });
    if (packets.size() >= max_packets) {
      ADD_FAILURE() << "Exceeded packet iteration cap of " << max_packets;
      break;
    }
  }
  return packets;
}

std::vector<PacketCoord> RunDecodePackets(PiTestModel& model,
                                          OPJ_UINT32 tileno) {
  const OPJ_UINT32 num_pocs = model.PocCount(tileno);
  ScopedPiIterator pi(
      opj_pi_create_decode(model.image(), model.cp(), tileno, model.manager()),
      num_pocs);
  if (!pi) {
    ADD_FAILURE() << "opj_pi_create_decode returned nullptr";
    return {};
  }
  std::vector<PacketCoord> packets;
  for (OPJ_UINT32 pino = 0; pino < num_pocs; ++pino) {
    std::vector<PacketCoord> part = DrainIterator(&pi[pino]);
    packets.insert(packets.end(), part.begin(), part.end());
  }
  return packets;
}

std::vector<PacketCoord> RunEncodePackets(PiTestModel& model, OPJ_UINT32 tileno,
                                          J2K_T2_MODE t2_mode) {
  const OPJ_UINT32 num_pocs = model.PocCount(tileno);
  ScopedPiIterator pi(
      opj_pi_initialise_encode(model.image(), model.cp(), tileno, t2_mode,
                               model.manager()),
      num_pocs);
  if (!pi) {
    ADD_FAILURE() << "opj_pi_initialise_encode returned nullptr";
    return {};
  }
  std::vector<PacketCoord> packets;
  for (OPJ_UINT32 pino = 0; pino < num_pocs; ++pino) {
    opj_pi_create_encode(pi.get(), model.cp(), tileno, pino, /*tpnum=*/0,
                         /*tppos=*/0, t2_mode);
    std::vector<PacketCoord> part = DrainIterator(&pi[pino]);
    packets.insert(packets.end(), part.begin(), part.end());
  }
  return packets;
}

// Geometry for exact progression-order verification:
// 8x4 tile, 2 layers, 2 resolutions, comp 0 (1x1), comp 1 (2x2), 4x4 precincts.
// Comp 0 has 1 precinct at res 0 and 2 at res 1; comp 1 has 1 at res 0 and 1
// at res 1 (5 spatial-resolution packets * 2 layers = 10 total).
// {layno, resno, compno, precno}
constexpr auto kOrderTestExpectedLrcp = std::to_array<PacketCoord>({
    {0, 0, 0, 0},
    {0, 0, 1, 0},
    {0, 1, 0, 0},
    {0, 1, 0, 1},
    {0, 1, 1, 0},
    {1, 0, 0, 0},
    {1, 0, 1, 0},
    {1, 1, 0, 0},
    {1, 1, 0, 1},
    {1, 1, 1, 0},
});
constexpr auto kOrderTestExpectedRlcp = std::to_array<PacketCoord>({
    {0, 0, 0, 0},
    {0, 0, 1, 0},
    {1, 0, 0, 0},
    {1, 0, 1, 0},
    {0, 1, 0, 0},
    {0, 1, 0, 1},
    {0, 1, 1, 0},
    {1, 1, 0, 0},
    {1, 1, 0, 1},
    {1, 1, 1, 0},
});
constexpr auto kOrderTestExpectedRpcl = std::to_array<PacketCoord>({
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {0, 0, 1, 0},
    {1, 0, 1, 0},
    {0, 1, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 1, 0},
    {1, 1, 1, 0},
    {0, 1, 0, 1},
    {1, 1, 0, 1},
});
constexpr auto kOrderTestExpectedPcrl = std::to_array<PacketCoord>({
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {0, 1, 0, 0},
    {1, 1, 0, 0},
    {0, 0, 1, 0},
    {1, 0, 1, 0},
    {0, 1, 1, 0},
    {1, 1, 1, 0},
    {0, 1, 0, 1},
    {1, 1, 0, 1},
});
constexpr auto kOrderTestExpectedCprl = std::to_array<PacketCoord>({
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {0, 1, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 1},
    {1, 1, 0, 1},
    {0, 0, 1, 0},
    {1, 0, 1, 0},
    {0, 1, 1, 0},
    {1, 1, 1, 0},
});

TEST(PiTest, DestroyHandlesNullAndPartiallyInitializedIterators) {
  opj_pi_destroy(nullptr, 0);
  opj_pi_destroy(nullptr, 4);

  // Build a bare 2-element iterator via opj_calloc with partial sub-arrays to
  // exercise null checks for include, comps, and resolutions in opj_pi_destroy.
  auto* raw =
      static_cast<opj_pi_iterator_t*>(opj_calloc(2, sizeof(opj_pi_iterator_t)));
  ASSERT_NE(nullptr, raw);
  ScopedPiIterator pi(raw, 2);

  // pi[0]: include == nullptr, comps == nullptr.
  // pi[1]: comps != nullptr with 2 comps, comp 0 has resolutions, comp 1 null.
  pi[1].numcomps = 2;
  pi[1].comps =
      static_cast<opj_pi_comp_t*>(opj_calloc(2, sizeof(opj_pi_comp_t)));
  ASSERT_NE(nullptr, pi[1].comps);
  pi[1].comps[0].numresolutions = 1;
  pi[1].comps[0].resolutions = static_cast<opj_pi_resolution_t*>(
      opj_calloc(1, sizeof(opj_pi_resolution_t)));
  ASSERT_NE(nullptr, pi[1].comps[0].resolutions);

  pi.reset();
}

TEST(PiTest, AllFiveProgressionOrdersMatchExactOrderInEncodeAndDecode) {
  struct OrderCase {
    OPJ_PROG_ORDER prg;
    std::string_view name;
    std::span<const PacketCoord> expected;
  };
  constexpr OrderCase kOrderCases[] = {
      {.prg = OPJ_LRCP, .name = "LRCP", .expected = kOrderTestExpectedLrcp},
      {.prg = OPJ_RLCP, .name = "RLCP", .expected = kOrderTestExpectedRlcp},
      {.prg = OPJ_RPCL, .name = "RPCL", .expected = kOrderTestExpectedRpcl},
      {.prg = OPJ_PCRL, .name = "PCRL", .expected = kOrderTestExpectedPcrl},
      {.prg = OPJ_CPRL, .name = "CPRL", .expected = kOrderTestExpectedCprl},
  };

  const std::array<CompSpec, 2> comps = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/2,
                      /*pdy=*/2),
      MakeUniformComp(/*dx=*/2, /*dy=*/2, /*numresolutions=*/2, /*pdx=*/2,
                      /*pdy=*/2),
  };

  for (const OrderCase& tc : kOrderCases) {
    SCOPED_TRACE(tc.name);
    PiTestModel model;
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 8,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 8,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 2,
                            .prg = tc.prg},
               comps);

    // Upper-bound packet count: numlayers(2) * max_prec(2) * numcomps(2) *
    // max_res(2) = 16.
    EXPECT_EQ(16u, opj_get_encoding_packet_count(model.image(), model.cp(), 0));

    // Verify decode iterator metadata and exact packet sequence.
    ScopedPiIterator dec_pi(
        opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
    ASSERT_NE(nullptr, dec_pi.get());
    EXPECT_EQ(0u, dec_pi->tx0);
    EXPECT_EQ(0u, dec_pi->ty0);
    EXPECT_EQ(8u, dec_pi->tx1);
    EXPECT_EQ(4u, dec_pi->ty1);
    EXPECT_EQ(1u, dec_pi->step_p);
    EXPECT_EQ(2u, dec_pi->step_c);
    EXPECT_EQ(4u, dec_pi->step_r);
    EXPECT_EQ(8u, dec_pi->step_l);
    EXPECT_EQ(1u, dec_pi->comps[0].resolutions[0].pw);
    EXPECT_EQ(1u, dec_pi->comps[0].resolutions[0].ph);
    EXPECT_EQ(2u, dec_pi->comps[0].resolutions[1].pw);
    EXPECT_EQ(1u, dec_pi->comps[0].resolutions[1].ph);
    EXPECT_EQ(1u, dec_pi->comps[1].resolutions[0].pw);
    EXPECT_EQ(1u, dec_pi->comps[1].resolutions[1].pw);

    EXPECT_THAT(DrainIterator(dec_pi.get()), ElementsAreArray(tc.expected));
    EXPECT_THAT(RunEncodePackets(model, 0, THRESH_CALC),
                ElementsAreArray(tc.expected));
    EXPECT_THAT(RunEncodePackets(model, 0, FINAL_PASS),
                ElementsAreArray(tc.expected));
    EXPECT_THAT(model.events().errors, IsEmpty());
  }
}

TEST(PiTest, AllOrdersEmitSamePacketSetAndEncodeMatchesDecode) {
  // Image [3,15) x [5,17) on a 2x2 tile grid starting at (1,2) with 8x8 tiles:
  // - Tile 0 covers [3,9) x [5,10) (clipped on left/top by image->x0/y0).
  // - Tile 3 covers [9,15) x [10,17) (clipped on right/bottom by image->x1/y1).
  // Comp 0 has 3 resolutions (dx=1, dy=1); comp 1 has 2 resolutions (dx=2,
  // dy=2). Unaligned tile origins exercise the (x == tx0) and (y == ty0)
  // precinct-start checks in RPCL/PCRL/CPRL as well as prcj > 0 and
  // resno >= comp->numresolutions skipping.
  const std::array<CompSpec, 2> comps = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/3, /*pdx=*/1,
                      /*pdy=*/1),
      MakeUniformComp(/*dx=*/2, /*dy=*/2, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
  };

  struct TileExpectation {
    OPJ_UINT32 tileno;
    OPJ_UINT32 expected_packet_count_bound;
    std::size_t expected_emitted_packets;
  };
  constexpr TileExpectation kTileCases[] = {
      // Tile 0: comp0 has (2*1 + 2*2 + 4*3) = 18 precincts (max_prec=12),
      // comp1 has (2*1 + 2*2) = 6 precincts; 24 * 2 layers = 48 packets.
      {.tileno = 0,
       .expected_packet_count_bound = 2u * 12u * 2u * 3u,
       .expected_emitted_packets = 48},
      // Tile 3: comp0 has (1*2 + 2*3 + 4*4) = 24 precincts (max_prec=16),
      // comp1 has (1*2 + 2*3) = 8 precincts; 32 * 2 layers = 64 packets.
      {.tileno = 3,
       .expected_packet_count_bound = 2u * 16u * 2u * 3u,
       .expected_emitted_packets = 64},
  };

  constexpr std::array<OPJ_PROG_ORDER, 5> kOrders = {
      OPJ_LRCP, OPJ_RLCP, OPJ_RPCL, OPJ_PCRL, OPJ_CPRL,
  };

  for (const TileExpectation& tile_tc : kTileCases) {
    SCOPED_TRACE(tile_tc.tileno);
    std::vector<PacketCoord> canonical_sorted;

    for (OPJ_PROG_ORDER prg : kOrders) {
      SCOPED_TRACE(static_cast<int>(prg));
      PiTestModel model;
      model.Init(TileGridSpec{.x0 = 3,
                              .y0 = 5,
                              .x1 = 15,
                              .y1 = 17,
                              .tx0 = 1,
                              .ty0 = 2,
                              .tdx = 8,
                              .tdy = 8,
                              .tw = 2,
                              .th = 2,
                              .numlayers = 2,
                              .prg = prg},
                 comps);

      EXPECT_EQ(tile_tc.expected_packet_count_bound,
                opj_get_encoding_packet_count(model.image(), model.cp(),
                                              tile_tc.tileno));

      std::vector<PacketCoord> dec = RunDecodePackets(model, tile_tc.tileno);
      std::vector<PacketCoord> enc =
          RunEncodePackets(model, tile_tc.tileno, FINAL_PASS);
      EXPECT_EQ(dec, enc);
      ASSERT_EQ(tile_tc.expected_emitted_packets, dec.size());

      for (std::size_t i = 0; i < dec.size(); ++i) {
        EXPECT_LT(dec[i].layno, 2u);
        EXPECT_LT(dec[i].compno, 2u);
        EXPECT_LT(dec[i].resno, comps[dec[i].compno].numresolutions);
        if (i > 0) {
          if (prg == OPJ_LRCP) {
            EXPECT_LT(dec[i - 1], dec[i]);
          } else if (prg == OPJ_RLCP) {
            EXPECT_LE(dec[i - 1].resno, dec[i].resno);
            if (dec[i - 1].resno == dec[i].resno) {
              EXPECT_LE(dec[i - 1].layno, dec[i].layno);
            }
          } else if (prg == OPJ_RPCL) {
            EXPECT_LE(dec[i - 1].resno, dec[i].resno);
          } else if (prg == OPJ_CPRL) {
            EXPECT_LE(dec[i - 1].compno, dec[i].compno);
          }
        }
      }

      std::vector<PacketCoord> sorted = dec;
      std::ranges::sort(sorted);
      ASSERT_EQ(sorted.end(), std::ranges::adjacent_find(sorted))
          << "duplicate packet";
      if (canonical_sorted.empty()) {
        canonical_sorted = std::move(sorted);
      } else {
        EXPECT_EQ(canonical_sorted, sorted);
      }
      EXPECT_THAT(model.events().errors, IsEmpty());
    }
  }
}

TEST(PiTest,
     MultiPocSwitchesProgressionOrdersAndSkipsOverlapInEncodeAndDecode) {
  // 8x4 tile, 2 layers, 2 comps, 3 resolutions (res 0: 1 prec, res 1: 2 precs,
  // res 2: 8 precs -> 11 * 2 * 2 = 44 total packets).
  const std::array<CompSpec, 2> comps = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/3, /*pdx=*/1,
                      /*pdy=*/1),
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/3, /*pdx=*/1,
                      /*pdy=*/1),
  };

  // Overlapping POCs test !pi->include[index] == false skipping in every order.
  const std::array<PocSpec, 3> pocs_lrcp_rpcl_cprl = {{
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 2,
       .resno1 = 1,
       .compno1 = 2,
       .prg = OPJ_LRCP},
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 2,
       .resno1 = 2,
       .compno1 = 2,
       .prg = OPJ_RPCL},
      {.resno0 = 1,
       .compno0 = 0,
       .layno1 = 2,
       .resno1 = 3,
       .compno1 = 2,
       .prg = OPJ_CPRL},
  }};

  PiTestModel model;
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 4,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 4,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 2,
                          .prg = OPJ_LRCP},
             comps, pocs_lrcp_rpcl_cprl);

  std::vector<PacketCoord> dec = RunDecodePackets(model, 0);
  std::vector<PacketCoord> enc = RunEncodePackets(model, 0, FINAL_PASS);
  EXPECT_EQ(dec, enc);
  ASSERT_EQ(44u, dec.size());
  // First 4 packets come from POC 0 (res 0 in LRCP).
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(0u, dec[i].resno);
  }
  // Next 8 packets come from POC 1 (res 1 only, res 0 skipped, in RPCL).
  for (std::size_t i = 4; i < 12; ++i) {
    EXPECT_EQ(1u, dec[i].resno);
  }
  // Remaining 32 packets come from POC 2 (res 2 only, res 1 skipped, in CPRL).
  for (std::size_t i = 12; i < 28; ++i) {
    EXPECT_EQ(2u, dec[i].resno);
    EXPECT_EQ(0u, dec[i].compno);
  }
  for (std::size_t i = 28; i < 44; ++i) {
    EXPECT_EQ(2u, dec[i].resno);
    EXPECT_EQ(1u, dec[i].compno);
  }

  // In THRESH_CALC with non-cinema rsiz, opj_pi_initialise_encode calls
  // opj_pi_update_encode_not_poc; with OPJ_PROFILE_CINEMA_4K it calls
  // opj_pi_update_encode_poc_and_final.
  std::vector<PacketCoord> thresh_non_cinema =
      RunEncodePackets(model, 0, THRESH_CALC);
  // opj_pi_update_encode_not_poc overwrites every pocs[pino] with tcp.prg and
  // full bounds, so pino=0 emits all 44 packets in LRCP and pino=1,2 emit none.
  ASSERT_EQ(44u, thresh_non_cinema.size());
  for (std::size_t i = 1; i < thresh_non_cinema.size(); ++i) {
    EXPECT_TRUE(thresh_non_cinema[i - 1] < thresh_non_cinema[i]);
  }

  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 4,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 4,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 2,
                          .prg = OPJ_LRCP,
                          .rsiz = OPJ_PROFILE_CINEMA_4K},
             comps, pocs_lrcp_rpcl_cprl);
  EXPECT_EQ(dec, RunEncodePackets(model, 0, THRESH_CALC));

  // Verify decode clamps layno1 to tcp->numlayers and covers overlap skipping
  // for OPJ_RLCP, OPJ_PCRL, and OPJ_LRCP.
  const std::array<PocSpec, 4> pocs_all_overlap = {{
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 2,
       .resno1 = 1,
       .compno1 = 2,
       .prg = OPJ_RPCL},
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 2,
       .resno1 = 2,
       .compno1 = 2,
       .prg = OPJ_RLCP},
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 9,
       .resno1 = 2,
       .compno1 = 2,
       .prg = OPJ_LRCP},
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 9,
       .resno1 = 3,
       .compno1 = 2,
       .prg = OPJ_PCRL},
  }};
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 4,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 4,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 2,
                          .prg = OPJ_LRCP},
             comps, pocs_all_overlap);
  std::vector<PacketCoord> dec_all = RunDecodePackets(model, 0);
  ASSERT_EQ(44u, dec_all.size());
  for (const PacketCoord& p : dec_all) {
    EXPECT_LT(p.layno, 2u);
  }
  std::ranges::sort(dec_all);
  EXPECT_EQ(dec_all.end(), std::ranges::adjacent_find(dec_all));
  EXPECT_THAT(model.events().errors, IsEmpty());
}

TEST(PiTest, UpdateEncodingParametersHandlesPocAndNonPocTiles) {
  // 1. Non-POC multi-tile grid with differing numresolutions and subsampling.
  const std::array<CompSpec, 2> comps = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/3, /*pdx=*/1,
                      /*pdy=*/1),
      MakeUniformComp(/*dx=*/2, /*dy=*/2, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
  };
  PiTestModel model;
  model.Init(TileGridSpec{.x0 = 1,
                          .y0 = 1,
                          .x1 = 10,
                          .y1 = 10,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 5,
                          .tdy = 5,
                          .tw = 2,
                          .th = 2,
                          .numlayers = 2,
                          .prg = OPJ_RLCP},
             comps);

  opj_pi_update_encoding_parameters(model.image(), model.cp(), 0);
  const opj_poc_t& tile0_poc = model.tcp(0).pocs[0];
  EXPECT_EQ(0u, tile0_poc.compS);
  EXPECT_EQ(2u, tile0_poc.compE);
  EXPECT_EQ(0u, tile0_poc.resS);
  EXPECT_EQ(3u, tile0_poc.resE);
  EXPECT_EQ(0u, tile0_poc.layS);
  EXPECT_EQ(2u, tile0_poc.layE);
  EXPECT_EQ(OPJ_RLCP, tile0_poc.prg);
  EXPECT_EQ(0u, tile0_poc.prcS);
  EXPECT_EQ(1u, tile0_poc.txS);
  EXPECT_EQ(5u, tile0_poc.txE);
  EXPECT_EQ(1u, tile0_poc.tyS);
  EXPECT_EQ(5u, tile0_poc.tyE);
  EXPECT_EQ(2u, tile0_poc.dx);
  EXPECT_EQ(2u, tile0_poc.dy);

  opj_pi_update_encoding_parameters(model.image(), model.cp(), 3);
  const opj_poc_t& tile3_poc = model.tcp(3).pocs[0];
  EXPECT_EQ(5u, tile3_poc.txS);
  EXPECT_EQ(10u, tile3_poc.txE);
  EXPECT_EQ(5u, tile3_poc.tyS);
  EXPECT_EQ(10u, tile3_poc.tyE);

  // 2. Zero-width / zero-height resolution grids (l_rx0 == l_rx1 and
  // l_ry0 == l_ry1) and dx/dy > UINT_MAX clamping in
  // opj_get_encoding_parameters.
  CompSpec huge_dx_comp;
  huge_dx_comp.dx = 0x80000000U;
  huge_dx_comp.dy = 0x80000000U;
  huge_dx_comp.numresolutions = 2;
  huge_dx_comp.prcw[0] = 1;
  huge_dx_comp.prch[0] = 1;
  huge_dx_comp.prcw[1] = 0;
  huge_dx_comp.prch[1] = 0;
  const std::array<CompSpec, 1> single_huge = {huge_dx_comp};
  model.Init(TileGridSpec{.x0 = 1,
                          .y0 = 0,
                          .x1 = 2,
                          .y1 = 4,
                          .tx0 = 1,
                          .ty0 = 0,
                          .tdx = 1,
                          .tdy = 4,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_LRCP},
             single_huge);
  opj_pi_update_encoding_parameters(model.image(), model.cp(), 0);
  // 0x7fffffff is pi.c's initial p_dx_min/p_dy_min sentinel when no step fits.
  EXPECT_EQ(0x7fffffffU, model.tcp(0).pocs[0].dx);
  EXPECT_EQ(0x7fffffffU, model.tcp(0).pocs[0].dy);

  const std::array<CompSpec, 1> unit_comp = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
  };
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 1,
                          .x1 = 4,
                          .y1 = 2,
                          .tx0 = 0,
                          .ty0 = 1,
                          .tdx = 4,
                          .tdy = 1,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_LRCP},
             unit_comp);
  opj_pi_update_encoding_parameters(model.image(), model.cp(), 0);
  EXPECT_EQ(2u, model.tcp(0).pocs[0].prcE);

  // 3. POC mode: test both branches of layS computation on pino >= 1:
  // when layE <= prev->layE, layS == 0; when layE > prev->layE, upstream sets
  // layS = layE (pi.c opj_pi_update_encode_poc_and_final).
  const std::array<PocSpec, 3> poc_specs = {{
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 1,
       .resno1 = 1,
       .compno1 = 2,
       .prg = OPJ_LRCP},
      {.resno0 = 1,
       .compno0 = 0,
       .layno1 = 1,
       .resno1 = 2,
       .compno1 = 2,
       .prg = OPJ_RLCP},
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 2,
       .resno1 = 3,
       .compno1 = 2,
       .prg = OPJ_CPRL},
  }};
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 8,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 8,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 2,
                          .prg = OPJ_LRCP},
             comps, poc_specs);
  opj_pi_update_encoding_parameters(model.image(), model.cp(), 0);
  EXPECT_EQ(0u, model.tcp(0).pocs[0].layS);
  EXPECT_EQ(1u, model.tcp(0).pocs[0].layE);
  EXPECT_EQ(OPJ_LRCP, model.tcp(0).pocs[0].prg);
  EXPECT_EQ(0u, model.tcp(0).pocs[1].layS);
  EXPECT_EQ(1u, model.tcp(0).pocs[1].layE);
  EXPECT_EQ(OPJ_RLCP, model.tcp(0).pocs[1].prg);
  EXPECT_EQ(2u, model.tcp(0).pocs[2].layS);
  EXPECT_EQ(2u, model.tcp(0).pocs[2].layE);
  EXPECT_EQ(OPJ_CPRL, model.tcp(0).pocs[2].prg);
}

TEST(PiTest, TilePartEncodingCoversAllProgressionFlagsAndCheckNextLevel) {
  // 8x8 tile, 2 comps, 2 layers, 2 resolutions:
  // res 0 uses 2x2 precincts (pdx=1, pdy=1 -> 2x2=4 precincts),
  // res 1 uses 4x4 precincts (pdx=2, pdy=2 -> 2x2=4 precincts).
  // Every resolution has 4 valid precincts and dx_min=4, dy_min=4 (32 packets).
  CompSpec uniform_4prec;
  uniform_4prec.dx = 1;
  uniform_4prec.dy = 1;
  uniform_4prec.numresolutions = 2;
  uniform_4prec.prcw[0] = 1;
  uniform_4prec.prch[0] = 1;
  uniform_4prec.prcw[1] = 2;
  uniform_4prec.prch[1] = 2;
  const std::array<CompSpec, 2> comps = {uniform_4prec, uniform_4prec};

  struct TpCase {
    OPJ_PROG_ORDER prg;
    OPJ_INT32 tppos;
    OPJ_UINT32 num_tp;
    bool call_past_end;
    std::string_view label;
  };
  constexpr TpCase kTpCases[] = {
      {.prg = OPJ_LRCP,
       .tppos = 0,
       .num_tp = 2,
       .call_past_end = true,
       .label = "LRCP_L"},
      {.prg = OPJ_LRCP,
       .tppos = 1,
       .num_tp = 4,
       .call_past_end = true,
       .label = "LRCP_R"},
      {.prg = OPJ_LRCP,
       .tppos = 2,
       .num_tp = 8,
       .call_past_end = true,
       .label = "LRCP_C"},
      {.prg = OPJ_LRCP,
       .tppos = 3,
       .num_tp = 32,
       .call_past_end = true,
       .label = "LRCP_P"},
      {.prg = OPJ_RLCP,
       .tppos = 1,
       .num_tp = 4,
       .call_past_end = false,
       .label = "RLCP_L"},
      {.prg = OPJ_RLCP,
       .tppos = 3,
       .num_tp = 32,
       .call_past_end = false,
       .label = "RLCP_P"},
      {.prg = OPJ_RPCL,
       .tppos = 0,
       .num_tp = 2,
       .call_past_end = false,
       .label = "RPCL_R"},
      {.prg = OPJ_RPCL,
       .tppos = 1,
       .num_tp = 8,
       .call_past_end = true,
       .label = "RPCL_P"},
      {.prg = OPJ_RPCL,
       .tppos = 2,
       .num_tp = 16,
       .call_past_end = false,
       .label = "RPCL_C"},
      {.prg = OPJ_RPCL,
       .tppos = 3,
       .num_tp = 32,
       .call_past_end = true,
       .label = "RPCL_L"},
      {.prg = OPJ_PCRL,
       .tppos = 0,
       .num_tp = 4,
       .call_past_end = false,
       .label = "PCRL_P"},
      {.prg = OPJ_PCRL,
       .tppos = 1,
       .num_tp = 8,
       .call_past_end = false,
       .label = "PCRL_C"},
      {.prg = OPJ_CPRL,
       .tppos = 0,
       .num_tp = 2,
       .call_past_end = false,
       .label = "CPRL_C"},
      {.prg = OPJ_CPRL,
       .tppos = 1,
       .num_tp = 8,
       .call_past_end = false,
       .label = "CPRL_P"},
  };

  for (const TpCase& tc : kTpCases) {
    SCOPED_TRACE(tc.label);
    PiTestModel baseline_model;
    baseline_model.Init(TileGridSpec{.x0 = 0,
                                     .y0 = 0,
                                     .x1 = 8,
                                     .y1 = 8,
                                     .tx0 = 0,
                                     .ty0 = 0,
                                     .tdx = 8,
                                     .tdy = 8,
                                     .tw = 1,
                                     .th = 1,
                                     .numlayers = 2,
                                     .prg = tc.prg,
                                     .tp_on = false},
                        comps);
    std::vector<PacketCoord> expected =
        RunEncodePackets(baseline_model, 0, FINAL_PASS);
    ASSERT_EQ(32u, expected.size());

    PiTestModel tp_model;
    tp_model.Init(TileGridSpec{.x0 = 0,
                               .y0 = 0,
                               .x1 = 8,
                               .y1 = 8,
                               .tx0 = 0,
                               .ty0 = 0,
                               .tdx = 8,
                               .tdy = 8,
                               .tw = 1,
                               .th = 1,
                               .numlayers = 2,
                               .prg = tc.prg,
                               .tp_on = true},
                  comps);
    ScopedPiIterator pi(
        opj_pi_initialise_encode(tp_model.image(), tp_model.cp(), 0, FINAL_PASS,
                                 tp_model.manager()),
        1);
    ASSERT_NE(nullptr, pi.get());
    EXPECT_EQ(1, pi->tp_on);

    std::vector<PacketCoord> actual;
    const std::size_t expected_per_tp = 32u / tc.num_tp;
    for (OPJ_UINT32 tpnum = 0; tpnum < tc.num_tp; ++tpnum) {
      opj_pi_create_encode(pi.get(), tp_model.cp(), 0, 0, tpnum, tc.tppos,
                           FINAL_PASS);
      std::vector<PacketCoord> tp_packets = DrainIterator(pi.get());
      EXPECT_EQ(expected_per_tp, tp_packets.size()) << "tpnum=" << tpnum;
      actual.insert(actual.end(), tp_packets.begin(), tp_packets.end());
    }
    EXPECT_EQ(expected, actual);

    // Note: in opj_pi_check_next_level, case OPJ_LRCP/OPJ_RLCP under 'P' is
    // unreachable via opj_pi_create_encode because 'P' is at index 3 of
    // "LRCP"/"RLCP" and check_next_level is called with pos = i - 1 <= 2.
    if (tc.call_past_end) {
      // Calling one past the final tile-part exercises check_next_level ==
      // OPJ_FALSE and incr_top = 0 / resetX = 0 without overflowing.
      opj_pi_create_encode(pi.get(), tp_model.cp(), 0, 0, tc.num_tp, tc.tppos,
                           FINAL_PASS);
    }
    EXPECT_THAT(tp_model.events().errors, IsEmpty());
  }

  // Verify THRESH_CALC rsiz gating in opj_pi_create_encode:
  // non-cinema/non-IMF ignores tp_on during THRESH_CALC (emits all 32 packets
  // in tpnum=0), while Cinema and IMF enable tile-part splitting.
  constexpr OPJ_UINT16 kThreshProfiles[] = {
      OPJ_PROFILE_NONE, OPJ_PROFILE_CINEMA_2K, OPJ_PROFILE_IMF_2K};
  for (OPJ_UINT16 rsiz : kThreshProfiles) {
    SCOPED_TRACE(rsiz);
    PiTestModel model;
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 8,
                            .y1 = 8,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 8,
                            .tdy = 8,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 2,
                            .prg = OPJ_CPRL,
                            .rsiz = rsiz,
                            .tp_on = true},
               comps);
    ScopedPiIterator pi(opj_pi_initialise_encode(model.image(), model.cp(), 0,
                                                 THRESH_CALC, model.manager()),
                        1);
    ASSERT_NE(nullptr, pi.get());
    opj_pi_create_encode(pi.get(), model.cp(), 0, 0, /*tpnum=*/0, /*tppos=*/0,
                         THRESH_CALC);
    std::vector<PacketCoord> tp0 = DrainIterator(pi.get());
    if (rsiz == OPJ_PROFILE_NONE) {
      EXPECT_EQ(32u, tp0.size());
    } else {
      EXPECT_EQ(16u, tp0.size());
      opj_pi_create_encode(pi.get(), model.cp(), 0, 0, /*tpnum=*/1,
                           /*tppos=*/0, THRESH_CALC);
      EXPECT_EQ(16u, DrainIterator(pi.get()).size());
    }
  }

  // Cinema 4K style 2-POC tile-part encoding split on 'C' (tppos = 0).
  const std::array<PocSpec, 2> cinema_pocs = {{
      {.resno0 = 0,
       .compno0 = 0,
       .layno1 = 1,
       .resno1 = 1,
       .compno1 = 2,
       .prg = OPJ_CPRL},
      {.resno0 = 1,
       .compno0 = 0,
       .layno1 = 1,
       .resno1 = 2,
       .compno1 = 2,
       .prg = OPJ_CPRL},
  }};
  PiTestModel cinema_model;
  cinema_model.Init(TileGridSpec{.x0 = 0,
                                 .y0 = 0,
                                 .x1 = 8,
                                 .y1 = 8,
                                 .tx0 = 0,
                                 .ty0 = 0,
                                 .tdx = 8,
                                 .tdy = 8,
                                 .tw = 1,
                                 .th = 1,
                                 .numlayers = 1,
                                 .prg = OPJ_CPRL,
                                 .rsiz = OPJ_PROFILE_CINEMA_4K,
                                 .tp_on = true},
                    comps, cinema_pocs);
  ScopedPiIterator cinema_pi(
      opj_pi_initialise_encode(cinema_model.image(), cinema_model.cp(), 0,
                               FINAL_PASS, cinema_model.manager()),
      2);
  ASSERT_NE(nullptr, cinema_pi.get());
  std::vector<PacketCoord> cinema_packets;
  for (OPJ_UINT32 pino = 0; pino < 2; ++pino) {
    for (OPJ_UINT32 tpnum = 0; tpnum < 2; ++tpnum) {
      opj_pi_create_encode(cinema_pi.get(), cinema_model.cp(), 0, pino, tpnum,
                           /*tppos=*/0, FINAL_PASS);
      std::vector<PacketCoord> part = DrainIterator(&cinema_pi[pino]);
      EXPECT_EQ(4u, part.size());
      cinema_packets.insert(cinema_packets.end(), part.begin(), part.end());
    }
  }
  EXPECT_EQ(16u, cinema_packets.size());
  EXPECT_THAT(cinema_model.events().errors, IsEmpty());
}

TEST(PiTest, EmptyZeroAndOverflowGuardsReturnNullOrFalseCleanly) {
  PiTestModel model;

  // 1. numcomps == 0 fails l_tmp_data allocation in both decode and encode.
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 8,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 8,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_LRCP},
             {});
  EXPECT_EQ(nullptr, opj_pi_create_decode(model.image(), model.cp(), 0,
                                          model.manager()));
  EXPECT_EQ(nullptr, opj_pi_initialise_encode(model.image(), model.cp(), 0,
                                              FINAL_PASS, model.manager()));

  // 2. Partial numresolutions == 0 fails opj_pi_create after allocating comp 0.
  const std::array<CompSpec, 2> zero_res_comps = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/0, /*pdx=*/1,
                      /*pdy=*/1),
  };
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 8,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 8,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_LRCP},
             zero_res_comps);
  EXPECT_EQ(nullptr, opj_pi_create_decode(model.image(), model.cp(), 0,
                                          model.manager()));
  EXPECT_EQ(nullptr, opj_pi_initialise_encode(model.image(), model.cp(), 0,
                                              FINAL_PASS, model.manager()));

  // 3. numlayers == 0 produces include_size == 0 in opj_pi_initialise_encode.
  const std::array<CompSpec, 1> valid_comp = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
  };
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 8,
                          .y1 = 8,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 8,
                          .tdy = 8,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 0,
                          .prg = OPJ_LRCP},
             valid_comp);
  EXPECT_EQ(nullptr, opj_pi_initialise_encode(model.image(), model.cp(), 0,
                                              FINAL_PASS, model.manager()));

  // 4. max_prec == 0 (subsampled dx=2 on [1,2) gives l_rx0 == l_rx1) yields
  // include_size == 0 in opj_pi_create_decode.
  const std::array<CompSpec, 1> sub_comp = {
      MakeUniformComp(/*dx=*/2, /*dy=*/2, /*numresolutions=*/1, /*pdx=*/1,
                      /*pdy=*/1),
  };
  model.Init(TileGridSpec{.x0 = 1,
                          .y0 = 1,
                          .x1 = 2,
                          .y1 = 2,
                          .tx0 = 1,
                          .ty0 = 1,
                          .tdx = 1,
                          .tdy = 1,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_LRCP},
             sub_comp);
  EXPECT_EQ(nullptr, opj_pi_create_decode(model.image(), model.cp(), 0,
                                          model.manager()));

  // 5. include array integer overflow guard in opj_pi_create_decode:
  // 65536 x 32768 with 1x1 precincts gives l_step_l = 0x80000000 > UINT_MAX/2.
  const std::array<CompSpec, 1> one_by_one_prec = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/1, /*pdx=*/0,
                      /*pdy=*/0),
  };
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 65536,
                          .y1 = 32768,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 65536,
                          .tdy = 32768,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_LRCP},
             one_by_one_prec);
  EXPECT_EQ(nullptr, opj_pi_create_decode(model.image(), model.cp(), 0,
                                          model.manager()));

  // 6. OPJ_PROG_UNKNOWN hits the default progression-order branch -> OPJ_FALSE.
  model.Init(TileGridSpec{.x0 = 0,
                          .y0 = 0,
                          .x1 = 4,
                          .y1 = 4,
                          .tx0 = 0,
                          .ty0 = 0,
                          .tdx = 4,
                          .tdy = 4,
                          .tw = 1,
                          .th = 1,
                          .numlayers = 1,
                          .prg = OPJ_PROG_UNKNOWN},
             valid_comp);
  ScopedPiIterator unknown_pi(
      opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
  ASSERT_NE(nullptr, unknown_pi.get());
  EXPECT_EQ(OPJ_FALSE, opj_pi_next(unknown_pi.get()));
}

TEST(PiTest, InvalidComponentBoundsAndIncludeOverflowEmitErrorsAndStop) {
  constexpr std::array<OPJ_PROG_ORDER, 5> kOrders = {
      OPJ_LRCP, OPJ_RLCP, OPJ_RPCL, OPJ_PCRL, OPJ_CPRL,
  };
  const std::array<CompSpec, 2> comps = {
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
      MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/1,
                      /*pdy=*/1),
  };

  for (OPJ_PROG_ORDER prg : kOrders) {
    SCOPED_TRACE(static_cast<int>(prg));
    PiTestModel model;
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 4,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 4,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               comps);
    ScopedPiIterator dec_pi(
        opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
    ASSERT_NE(nullptr, dec_pi.get());

    // compno0 >= numcomps.
    dec_pi->poc.compno0 = 2;
    dec_pi->poc.compno1 = 2;
    EXPECT_EQ(OPJ_FALSE, opj_pi_next(dec_pi.get()));
    ASSERT_FALSE(model.events().errors.empty());
    EXPECT_THAT(model.events().errors.back(),
                HasSubstr("invalid compno0/compno1"));

    // compno1 >= numcomps + 1.
    model.ClearEvents();
    dec_pi->poc.compno0 = 0;
    dec_pi->poc.compno1 = 3;
    EXPECT_EQ(OPJ_FALSE, opj_pi_next(dec_pi.get()));
    ASSERT_FALSE(model.events().errors.empty());
    EXPECT_THAT(model.events().errors.back(),
                HasSubstr("invalid compno0/compno1"));

    // POC with layno1 > tcp->numlayers in FINAL_PASS hits index >=
    // include_size cleanly after layer 0 is emitted.
    const std::array<PocSpec, 1> over_layer_poc = {{
        {.resno0 = 0,
         .compno0 = 0,
         .layno1 = 2,
         .resno1 = 2,
         .compno1 = 2,
         .prg = prg},
    }};
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 4,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 4,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               comps, over_layer_poc);
    ScopedPiIterator enc_pi(
        opj_pi_initialise_encode(model.image(), model.cp(), 0, FINAL_PASS,
                                 model.manager()),
        1);
    ASSERT_NE(nullptr, enc_pi.get());
    opj_pi_create_encode(enc_pi.get(), model.cp(), 0, 0, 0, 0, FINAL_PASS);
    while (opj_pi_next(enc_pi.get())) {
    }
    ASSERT_FALSE(model.events().errors.empty());
    EXPECT_THAT(model.events().errors.back(),
                HasSubstr("Invalid access to pi->include"));
  }
}

TEST(PiTest, PositionOrderGridOverflowAndZeroExtentGuards) {
  constexpr std::array<OPJ_PROG_ORDER, 3> kPosOrders = {
      OPJ_RPCL,
      OPJ_PCRL,
      OPJ_CPRL,
  };

  for (OPJ_PROG_ORDER prg : kPosOrders) {
    SCOPED_TRACE(static_cast<int>(prg));
    PiTestModel model;

    // 1. pi->dx == 0 and pi->dy == 0 when all resolutions overflow 32-bit step.
    const std::array<CompSpec, 1> dx_overflow_comp = {
        MakeUniformComp(/*dx=*/4, /*dy=*/1, /*numresolutions=*/3, /*pdx=*/30,
                        /*pdy=*/1),
    };
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 4,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 4,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               dx_overflow_comp);
    ScopedPiIterator pi_dx0(
        opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
    ASSERT_NE(nullptr, pi_dx0.get());
    EXPECT_THAT(DrainIterator(pi_dx0.get()), IsEmpty());

    const std::array<CompSpec, 1> dy_overflow_comp = {
        MakeUniformComp(/*dx=*/1, /*dy=*/4, /*numresolutions=*/3, /*pdx=*/1,
                        /*pdy=*/30),
    };
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 4,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 4,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               dy_overflow_comp);
    ScopedPiIterator pi_dy0(
        opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
    ASSERT_NE(nullptr, pi_dy0.get());
    EXPECT_THAT(DrainIterator(pi_dy0.get()), IsEmpty());

    // 2. comp->dx << rpx and comp->dy << rpy overflow on res 0 (rpx = 33 < 64)
    // while res 3 (pdx=0, pdy=0, levelno=0) supplies valid pi->dx and pi->dy.
    CompSpec huge_x_comp;
    huge_x_comp.dx = 0x80000000U;
    huge_x_comp.dy = 1;
    huge_x_comp.numresolutions = 4;
    huge_x_comp.prcw[0] = 30;
    huge_x_comp.prch[0] = 0;
    huge_x_comp.prcw[1] = 1;
    huge_x_comp.prch[1] = 1;
    huge_x_comp.prcw[2] = 0;
    huge_x_comp.prch[2] = 0;
    huge_x_comp.prcw[3] = 0;
    huge_x_comp.prch[3] = 0;

    CompSpec huge_y_comp = huge_x_comp;
    huge_y_comp.dx = 1;
    huge_y_comp.dy = 0x80000000U;
    huge_y_comp.prcw[0] = 0;
    huge_y_comp.prch[0] = 30;

    const std::array<CompSpec, 3> mixed_huge = {
        MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/1, /*pdx=*/0,
                        /*pdy=*/0),
        huge_x_comp,
        huge_y_comp,
    };
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 4,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 4,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               mixed_huge);
    std::vector<PacketCoord> huge_packets = RunDecodePackets(model, 0);
    EXPECT_EQ(30u, huge_packets.size());
    EXPECT_THAT(model.events().errors, IsEmpty());

    // 3. res->pw == 0 (via [1,2) x [0,4)) and res->ph == 0 (via [0,4) x [1,2))
    // at res 0 while res 1 is non-empty.
    const std::array<CompSpec, 1> two_res = {
        MakeUniformComp(/*dx=*/1, /*dy=*/1, /*numresolutions=*/2, /*pdx=*/1,
                        /*pdy=*/1),
    };
    model.Init(TileGridSpec{.x0 = 1,
                            .y0 = 0,
                            .x1 = 2,
                            .y1 = 4,
                            .tx0 = 1,
                            .ty0 = 0,
                            .tdx = 1,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               two_res);
    std::vector<PacketCoord> pw0_packets = RunDecodePackets(model, 0);
    ASSERT_EQ(2u, pw0_packets.size());
    EXPECT_EQ(1u, pw0_packets[0].resno);
    EXPECT_EQ(1u, pw0_packets[1].resno);
    EXPECT_THAT(model.events().errors, IsEmpty());

    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 1,
                            .x1 = 4,
                            .y1 = 2,
                            .tx0 = 0,
                            .ty0 = 1,
                            .tdx = 4,
                            .tdy = 1,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               two_res);
    std::vector<PacketCoord> ph0_packets = RunDecodePackets(model, 0);
    ASSERT_EQ(2u, ph0_packets.size());
    EXPECT_EQ(1u, ph0_packets[0].resno);
    EXPECT_EQ(1u, ph0_packets[1].resno);
    EXPECT_THAT(model.events().errors, IsEmpty());

    // 4. trx0 == trx1 and try0 == try1 when res->pw > 0 && res->ph > 0.
    model.Init(TileGridSpec{.x0 = 0,
                            .y0 = 0,
                            .x1 = 4,
                            .y1 = 4,
                            .tx0 = 0,
                            .ty0 = 0,
                            .tdx = 4,
                            .tdy = 4,
                            .tw = 1,
                            .th = 1,
                            .numlayers = 1,
                            .prg = prg},
               two_res);
    ScopedPiIterator trx_pi(
        opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
    ASSERT_NE(nullptr, trx_pi.get());
    trx_pi->tx0 = 1;
    trx_pi->tx1 = 2;
    std::vector<PacketCoord> trx_packets = DrainIterator(trx_pi.get());
    ASSERT_EQ(2u, trx_packets.size());
    EXPECT_EQ(1u, trx_packets[0].resno);
    EXPECT_EQ(1u, trx_packets[1].resno);

    ScopedPiIterator try_pi(
        opj_pi_create_decode(model.image(), model.cp(), 0, model.manager()), 1);
    ASSERT_NE(nullptr, try_pi.get());
    try_pi->ty0 = 1;
    try_pi->ty1 = 2;
    std::vector<PacketCoord> try_packets = DrainIterator(try_pi.get());
    ASSERT_EQ(2u, try_packets.size());
    EXPECT_EQ(1u, try_packets[0].resno);
    EXPECT_EQ(1u, try_packets[1].resno);
    EXPECT_THAT(model.events().errors, IsEmpty());

    // 5. (comp->dx << levelno) and (comp->dy << levelno) overflow guards:
    // construct a 34-resolution 1x1-tile iterator directly (levelno = 33 < 64
    // at res 0). Deliberately exceeds OPJ_J2K_MAXRLVLS (pi.c has no bound
    // check).
    static_assert(OPJ_J2K_MAXRLVLS == 33);
    constexpr OPJ_UINT32 kNumRes = OPJ_J2K_MAXRLVLS + 1;
    auto* raw_pi = static_cast<opj_pi_iterator_t*>(
        opj_calloc(1, sizeof(opj_pi_iterator_t)));
    ASSERT_NE(nullptr, raw_pi);
    ScopedPiIterator lvl_pi(raw_pi, 1);
    lvl_pi->manager = model.manager();
    lvl_pi->numcomps = 2;
    lvl_pi->comps =
        static_cast<opj_pi_comp_t*>(opj_calloc(2, sizeof(opj_pi_comp_t)));
    ASSERT_NE(nullptr, lvl_pi->comps);
    for (OPJ_UINT32 c = 0; c < 2; ++c) {
      lvl_pi->comps[c].dx = (c == 0) ? 0x80000000U : 1u;
      lvl_pi->comps[c].dy = (c == 1) ? 0x80000000U : 1u;
      lvl_pi->comps[c].numresolutions = kNumRes;
      lvl_pi->comps[c].resolutions = static_cast<opj_pi_resolution_t*>(
          opj_calloc(kNumRes, sizeof(opj_pi_resolution_t)));
      ASSERT_NE(nullptr, lvl_pi->comps[c].resolutions);
      for (OPJ_UINT32 r = 0; r < kNumRes; ++r) {
        lvl_pi->comps[c].resolutions[r].pw = 1;
        lvl_pi->comps[c].resolutions[r].ph = 1;
      }
    }
    lvl_pi->tx0 = 0;
    lvl_pi->ty0 = 0;
    lvl_pi->tx1 = 1;
    lvl_pi->ty1 = 1;
    lvl_pi->step_p = 1;
    lvl_pi->step_c = 1;
    lvl_pi->step_r = 2;
    lvl_pi->step_l = 2 * kNumRes;
    lvl_pi->include_size = 2 * kNumRes;
    lvl_pi->include = static_cast<OPJ_INT16*>(
        opj_calloc(lvl_pi->include_size, sizeof(OPJ_INT16)));
    ASSERT_NE(nullptr, lvl_pi->include);
    lvl_pi->first = 1;
    lvl_pi->poc.prg = prg;
    lvl_pi->poc.resno1 = kNumRes;
    lvl_pi->poc.compno1 = 2;
    lvl_pi->poc.layno1 = 1;
    lvl_pi->poc.precno1 = 1;

    std::vector<PacketCoord> lvl_packets = DrainIterator(lvl_pi.get());
    EXPECT_EQ(66u, lvl_packets.size());
    for (const PacketCoord& p : lvl_packets) {
      EXPECT_GE(p.resno, 1u);
    }
    EXPECT_THAT(model.events().errors, IsEmpty());
  }
}

// TODO(schwehr): Cover remaining OOM and unreachable progression-order branches
// in pi.c.

}  // namespace
}  // namespace openjpeg2
