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
// Unit tests for meta Interrupted projection
// (proj/src/projections/interrupted.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/interrupted.cpp

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

// Upstream interrupted_destructor (proj/src/projections/interrupted.cpp)
// frees P->opaque via free() without invoking the destructors of the four
// std::vector<double> members in pj_interrupted_data. Destroy those vectors
// before calling proj_destroy(pj), and instantiate via pj_create_internal() so
// PROJStringParser does not create and destroy an uncleaned temporary PJ*.
struct InterruptedOpaqueMirror {
  PJ* base_pj;
  std::vector<double> lam_borders;
  std::vector<double> lam_centers;
  std::vector<double> x_borders;
  std::vector<double> x_centers;
};

struct InterruptedPjDeleter {
  void operator()(PJ* pj) const noexcept {
    if (pj != nullptr && pj->opaque != nullptr) {
      auto* q = static_cast<InterruptedOpaqueMirror*>(pj->opaque);
      std::destroy_at(&q->lam_borders);
      std::destroy_at(&q->lam_centers);
      std::destroy_at(&q->x_borders);
      std::destroy_at(&q->x_centers);
    }
    proj_destroy(pj);
  }
};

using ScopedInterruptedPj = std::unique_ptr<PJ, InterruptedPjDeleter>;

TEST(InterruptedTest, ParameterValidationRejectsInvalidBaseAndGores) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing or unsupported +base parameter.
  for (const char* def :
       {"+proj=interrupted", "+proj=interrupted +base=merc"}) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedInterruptedPj pj(pj_create_internal(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
  }
  EXPECT_TRUE(capture.ContainsMessage("Invalid value for base parameter"));

  // 2. Non-numeric +gores token fails c_locale_stod.
  {
    proj_context_errno_set(ctx.get(), 0);
    ScopedInterruptedPj pj(pj_create_internal(
        ctx.get(), "+proj=interrupted +base=sinu +gores=1,bad,2"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("Invalid value for gores parameter"));
  }

  // 3. Zero or empty gores (+gores=0 or +gores=,) produces an empty sizes
  // vector and fails with "Invalid 0 gores".
  for (const char* def : {"+proj=interrupted +base=sinu +gores=0",
                          "+proj=interrupted +base=sinu +gores=,"}) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedInterruptedPj pj(pj_create_internal(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
  }
  EXPECT_TRUE(capture.ContainsMessage("Invalid 0 gores"));
}

TEST(InterruptedTest, AllFourBaseProjectionsAndGoreConfigurationsRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      // Default gores (3 equal gores) across all 4 supported base projections.
      "+proj=interrupted +base=cass +ellps=WGS84",
      "+proj=interrupted +base=poly +ellps=WGS84",
      "+proj=interrupted +base=sinu +ellps=WGS84",
      "+proj=interrupted +base=tmerc +ellps=WGS84",
      // Explicit single gore count and custom comma-separated gore weights
      // (including an empty token between commas to cover gore.empty()).
      "+proj=interrupted +base=sinu +gores=4 +R=6371000",
      "+proj=interrupted +base=sinu +gores=1,,2,1 +R=6371000",
  };

  // Test points strictly inside the westernmost, central, and easternmost
  // gores (well away from gore boundaries at -90, -60, 0, +60, +90 deg).
  constexpr double kLonDegs[] = {-125.0, 10.0, 125.0};

  for (const char* def : kDefs) {
    ScopedInterruptedPj pj(pj_create_internal(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    for (const double lon_deg : kLonDegs) {
      // 1. Forward interrupted projection.
      const PJ_COORD in =
          proj_coord(DegToRad(lon_deg), DegToRad(25.0), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get())) << def << " lon=" << lon_deg;
      EXPECT_FALSE(std::isinf(fwd.xy.x)) << def << " lon=" << lon_deg;
      EXPECT_FALSE(std::isinf(fwd.xy.y)) << def << " lon=" << lon_deg;

      // 2. Inverse interrupted projection recovers (lon, lat).
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def << " lon=" << lon_deg;
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-9) << def << " lon=" << lon_deg;
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-9) << def << " lon=" << lon_deg;
    }
  }
}

TEST(InterruptedTest, InverseInInterGoreGapSetsOutsideProjectionDomainErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  // With 3 equal sinusoidal gores (-180..-60, -60..+60, +60..+180) on a unit
  // sphere, each gore has equatorial half-width pi/3 (~1.0472), while at
  // latitude 60 deg (y = pi/3) the sinusoidal gore half-width shrinks by
  // cos(60 deg) = 0.5 to pi/6 (~0.5236).
  ScopedInterruptedPj pj(
      pj_create_internal(ctx.get(), "+proj=interrupted +base=sinu +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Point near the eastern edge of the central gore at lat=60 deg has
  // inverted lam > end_lam (+pi/3).
  const PJ_COORD gap_east =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.95, kPi / 3.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, gap_east.lp.lam);
  EXPECT_EQ(HUGE_VAL, gap_east.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. Point near the western edge of the central gore at lat=60 deg has
  // inverted lam < start_lam (-pi/3).
  const PJ_COORD gap_west =
      proj_trans(pj.get(), PJ_INV, proj_coord(-0.95, kPi / 3.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, gap_west.lp.lam);
  EXPECT_EQ(HUGE_VAL, gap_west.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(InterruptedTest, DestructorHandlesNullPointerAndNullOpaqueCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedInterruptedPj pj(
      pj_create_internal(ctx.get(), "+proj=interrupted +base=sinu"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_DESTRUCTOR dtor = pj->destructor;
  ASSERT_NE(nullptr, dtor);
  EXPECT_EQ(nullptr, dtor(nullptr, 0));

  PJ* bare_pj = pj_new();
  ASSERT_NE(nullptr, bare_pj);
  bare_pj->ctx = ctx.get();
  EXPECT_EQ(nullptr, dtor(bare_pj, 0));
}

}  // namespace
}  // namespace proj
