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
// Unit tests for Near-sided perspective (nsper) and Tilted perspective (tpers)
// projections (proj/src/projections/nsper.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/nsper.cpp

#include <cmath>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(NsperTest, InvalidHeightParameterFailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  constexpr const char* kInvalidDefs[] = {
      // Missing +h defaults to height = 0 -> pn1 = 0 <= 0.
      "+proj=nsper +R=6371000",
      // Explicit zero height -> pn1 == 0.
      "+proj=nsper +R=6371000 +h=0",
      // Negative height -> pn1 < 0.
      "+proj=nsper +R=6371000 +h=-1000",
      // Normalized height pn1 = h / a > 1e10.
      "+proj=nsper +R=1 +h=2e10",
      // Same validation path reached via tpers.
      "+proj=tpers +R=6371000 +h=-500 +tilt=10 +azi=20",
  };

  for (const char* def : kInvalidDefs) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, pj.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
  }
  EXPECT_TRUE(capture.ContainsMessage("Invalid value for h"));
}

TEST(NsperTest, ForwardAndInverseRoundTripAllFourAspectModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ModeCase {
    const char* name;
    const char* proj_def;
    double lat0_deg;
    double sample_lon_deg;
    double sample_lat_deg;
  };
  constexpr ModeCase kModes[] = {
      {
          .name = "Equatorial",
          .proj_def = "+proj=nsper +R=6371000 +h=10000000 +lat_0=0",
          .lat0_deg = 0.0,
          .sample_lon_deg = 15.0,
          .sample_lat_deg = 20.0,
      },
      {
          .name = "Oblique",
          .proj_def = "+proj=nsper +R=6371000 +h=10000000 +lat_0=45",
          .lat0_deg = 45.0,
          .sample_lon_deg = 12.0,
          .sample_lat_deg = 50.0,
      },
      {
          .name = "NorthPole",
          .proj_def = "+proj=nsper +R=6371000 +h=10000000 +lat_0=90",
          .lat0_deg = 90.0,
          .sample_lon_deg = 30.0,
          .sample_lat_deg = 70.0,
      },
      {
          .name = "SouthPole",
          .proj_def = "+proj=nsper +R=6371000 +h=10000000 +lat_0=-90",
          .lat0_deg = -90.0,
          .sample_lon_deg = -40.0,
          .sample_lat_deg = -65.0,
      },
  };

  for (const auto& mode : kModes) {
    ScopedPj pj(proj_create(ctx.get(), mode.proj_def));
    ASSERT_NE(nullptr, pj.get()) << mode.name;
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    // 1. Projection center projects to (0, 0) and exercises rh <= EPS10 in
    // nsper_s_inverse.
    const PJ_COORD center_in =
        proj_coord(0.0, DegToRad(mode.lat0_deg), 0.0, 0.0);
    const PJ_COORD center_fwd = proj_trans(pj.get(), PJ_FWD, center_in);
    EXPECT_EQ(0, proj_errno(pj.get())) << mode.name;
    EXPECT_NEAR(0.0, center_fwd.xy.x, 1e-6) << mode.name;
    EXPECT_NEAR(0.0, center_fwd.xy.y, 1e-6) << mode.name;

    const PJ_COORD center_inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << mode.name;
    EXPECT_NEAR(0.0, center_inv.lp.lam, 1e-12) << mode.name;
    EXPECT_NEAR(DegToRad(mode.lat0_deg), center_inv.lp.phi, 1e-12) << mode.name;

    // 2. Non-origin point exercises the rh > EPS10 inverse branch for each
    // mode.
    const PJ_COORD pt_in = proj_coord(DegToRad(mode.sample_lon_deg),
                                      DegToRad(mode.sample_lat_deg), 0.0, 0.0);
    const PJ_COORD pt_fwd = proj_trans(pj.get(), PJ_FWD, pt_in);
    EXPECT_EQ(0, proj_errno(pj.get())) << mode.name;
    EXPECT_FALSE(std::isinf(pt_fwd.xy.x)) << mode.name;
    EXPECT_FALSE(std::isinf(pt_fwd.xy.y)) << mode.name;

    const PJ_COORD pt_inv = proj_trans(pj.get(), PJ_INV, pt_fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << mode.name;
    EXPECT_NEAR(pt_in.lp.lam, pt_inv.lp.lam, 1e-10) << mode.name;
    EXPECT_NEAR(pt_in.lp.phi, pt_inv.lp.phi, 1e-10) << mode.name;
  }
}

TEST(NsperTest, TiltedPerspectiveForwardAndInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=tpers +R=6371000 +h=8000000 +lat_0=35 "
                          "+lon_0=-100 +tilt=15 +azi=25"));
  ASSERT_NE(nullptr, pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = -100.0, .lat_deg = 35.0},
      {.lon_deg = -92.0, .lat_deg = 40.0},
      {.lon_deg = -108.0, .lat_deg = 28.0},
      {.lon_deg = -95.0, .lat_deg = 30.0},
  };

  for (const auto& tc : kCases) {
    // 1. Forward tilted perspective transformation.
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 50.0, 2026.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse tilted perspective transformation recovers (lon, lat, z, t).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-10);
    EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-10);
    EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-12);
    EXPECT_NEAR(in.lpzt.t, inv.lpzt.t, 1e-12);
  }
}

TEST(NsperTest, ForwardAndInverseOutsideHorizonDomainSetErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppressor(ctx.get());

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=nsper +R=6371000 +h=5000000 +lat_0=0"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Point on the opposite side of the globe (lon = 180 deg) has xy.y = -1 <
  // rp.
  const PJ_COORD far_side =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, far_side.xy.x);
  EXPECT_EQ(HUGE_VAL, far_side.xy.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. Projected point far outside the visible horizon disk makes sinz < 0.
  const PJ_COORD outside_disk =
      proj_trans(pj.get(), PJ_INV, proj_coord(2.0e7, 2.0e7, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, outside_disk.lp.lam);
  EXPECT_EQ(HUGE_VAL, outside_disk.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(NsperTest, FactorsAndTransArraySucceedWithinVisibleDisk) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=nsper +R=6371000 +h=10000000 +lat_0=20"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(5.0), DegToRad(25.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.0);
  EXPECT_GT(factors.parallel_scale, 0.0);
  EXPECT_GT(factors.areal_scale, 0.0);

  PJ_COORD coords[2] = {
      proj_coord(DegToRad(0.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(10.0), DegToRad(30.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
  EXPECT_NEAR(0.0, coords[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(10.0), coords[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(30.0), coords[1].lp.phi, 1e-10);
}

}  // namespace
}  // namespace proj
