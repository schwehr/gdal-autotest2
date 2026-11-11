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
// Unit tests for Space Oblique Mercator projections (som, misrsom, lsat)
// in proj/src/projections/som.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/som.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(SomTest, ParameterValidationErrorsAndBoundaryAcceptance) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  struct InvalidCase {
    const char* proj_str;
    const char* expected_log;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.proj_str = "+proj=som +asc_lon=361 +inc_angle=98.2 +ps_rev=0.0686695",
       .expected_log = "ascending longitude"},
      {.proj_str = "+proj=som +asc_lon=-361 +inc_angle=98.2 +ps_rev=0.0686695",
       .expected_log = "ascending longitude"},
      {.proj_str = "+proj=som +asc_lon=0 +inc_angle=-1 +ps_rev=0.0686695",
       .expected_log = "inclination angle"},
      {.proj_str = "+proj=som +asc_lon=0 +inc_angle=181 +ps_rev=0.0686695",
       .expected_log = "inclination angle"},
      {.proj_str = "+proj=som +asc_lon=0 +inc_angle=98.2 +ps_rev=-0.1",
       .expected_log = "Number of days per rotation"},
      {.proj_str = "+proj=misrsom", .expected_log = "[1, 233]"},
      {.proj_str = "+proj=misrsom +path=0", .expected_log = "[1, 233]"},
      {.proj_str = "+proj=misrsom +path=-1", .expected_log = "[1, 233]"},
      {.proj_str = "+proj=misrsom +path=234", .expected_log = "[1, 233]"},
      {.proj_str = "+proj=lsat +path=1", .expected_log = "[1, 5]"},
      {.proj_str = "+proj=lsat +lsat=0 +path=1", .expected_log = "[1, 5]"},
      {.proj_str = "+proj=lsat +lsat=6 +path=1", .expected_log = "[1, 5]"},
      {.proj_str = "+proj=lsat +lsat=3 +path=0", .expected_log = "[1, 251]"},
      {.proj_str = "+proj=lsat +lsat=3 +path=252", .expected_log = "[1, 251]"},
      {.proj_str = "+proj=lsat +lsat=4 +path=0", .expected_log = "[1, 233]"},
      {.proj_str = "+proj=lsat +lsat=5 +path=234", .expected_log = "[1, 233]"},
  };

  for (const auto& tc : kInvalidCases) {
    logs.Clear();
    ScopedPj bad(proj_create(ctx.get(), tc.proj_str));
    EXPECT_EQ(nullptr, bad.get()) << tc.proj_str;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << tc.proj_str;
    EXPECT_TRUE(logs.ContainsMessage(tc.expected_log)) << tc.proj_str;
  }

  struct ValidBoundaryCase {
    const char* proj_str;
    const char* expected_id;
  };
  constexpr ValidBoundaryCase kValidCases[] = {
      {.proj_str =
           "+proj=som +asc_lon=-359 +inc_angle=0 +ps_rev=0 +ellps=WGS84",
       .expected_id = "som"},
      {.proj_str = "+proj=som +asc_lon=359 +inc_angle=180 +ps_rev=0.0686695 "
                   "+ellps=WGS84",
       .expected_id = "som"},
      {.proj_str = "+proj=misrsom +path=1", .expected_id = "misrsom"},
      {.proj_str = "+proj=misrsom +path=233", .expected_id = "misrsom"},
      {.proj_str = "+proj=lsat +lsat=1 +path=1", .expected_id = "lsat"},
      {.proj_str = "+proj=lsat +lsat=3 +path=251", .expected_id = "lsat"},
      {.proj_str = "+proj=lsat +lsat=4 +path=1", .expected_id = "lsat"},
      {.proj_str = "+proj=lsat +lsat=5 +path=233", .expected_id = "lsat"},
  };

  for (const auto& tc : kValidCases) {
    ScopedPj ok(proj_create(ctx.get(), tc.proj_str));
    ASSERT_NE(nullptr, ok.get()) << tc.proj_str;
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << tc.proj_str;
    const PJ_PROJ_INFO info = proj_pj_info(ok.get());
    EXPECT_STREQ(tc.expected_id, info.id);
    EXPECT_EQ(1, info.has_inverse);
    EXPECT_EQ(1, pj_has_inverse(ok.get()));
    EXPECT_EQ(1, proj_angular_input(ok.get(), PJ_FWD));
    EXPECT_EQ(0, proj_angular_output(ok.get(), PJ_FWD));
  }
}

TEST(SomTest, SomForwardReferencePolesAndRoundTripWgs84) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=som +inc_angle=98.2 +ps_rev=0.0686695 "
                          "+asc_lon=129.3 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct RefCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
    double inv_tol_rad;
  };
  constexpr RefCase kCases[] = {
      {.lon_deg = 129.3,
       .lat_deg = 0.0,
       .expected_x = 40035871.018649,
       .expected_y = -2828007.908832,
       .inv_tol_rad = 1e-7},
      {.lon_deg = 120.0,
       .lat_deg = 35.0,
       .expected_x = 3946967.409780,
       .expected_y = 608629.239240,
       .inv_tol_rad = 1e-7},
      {.lon_deg = -75.0,
       .lat_deg = 40.0,
       .expected_x = 15652848.669714,
       .expected_y = -1298131.447840,
       .inv_tol_rad = 1e-7},
      {.lon_deg = 10.0,
       .lat_deg = -30.0,
       .expected_x = 27472327.257122,
       .expected_y = 8885690.794766,
       .inv_tol_rad = 1e-7},
      {.lon_deg = 129.3,
       .lat_deg = 90.0,
       .expected_x = 10064637.304286,
       .expected_y = -35528.352210,
       .inv_tol_rad = 1e-6},
      {.lon_deg = 129.3,
       .lat_deg = -90.0,
       .expected_x = 30193911.912859,
       .expected_y = 35528.352210,
       .inv_tol_rad = 1e-6},
  };

  for (const auto& tc : kCases) {
    const double lon = DegToRad(tc.lon_deg);
    const double lat = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 123.45, 2026.5));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, 1e-3);
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, 1e-3);
    EXPECT_DOUBLE_EQ(123.45, fwd.xyzt.z);
    EXPECT_DOUBLE_EQ(2026.5, fwd.xyzt.t);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lat, inv.lpzt.phi, tc.inv_tol_rad);
    if (std::abs(tc.lat_deg) < 89.9) {
      EXPECT_NEAR(0.0, adjlon(inv.lpzt.lam - lon), tc.inv_tol_rad);
    }
    EXPECT_DOUBLE_EQ(123.45, inv.lpzt.z);
    EXPECT_DOUBLE_EQ(2026.5, inv.lpzt.t);
  }
}

TEST(SomTest, MisrAndLandsatForwardReferenceAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct SatelliteCase {
    const char* proj_str;
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr SatelliteCase kCases[] = {
      {.proj_str = "+proj=misrsom +path=100 +ellps=WGS84",
       .lon_deg = -25.0,
       .lat_deg = 38.0,
       .expected_x = 4210558.451809,
       .expected_y = -275265.278636},
      {.proj_str = "+proj=lsat +lsat=2 +path=15 +ellps=WGS84",
       .lon_deg = 100.0,
       .lat_deg = 30.0,
       .expected_x = 43666517.316406,
       .expected_y = -2096758.235372},
      {.proj_str = "+proj=lsat +lsat=5 +path=190 +ellps=WGS84",
       .lon_deg = -122.0,
       .lat_deg = 37.5,
       .expected_x = 47496776.655979,
       .expected_y = -6815513.213233},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.proj_str));
    ASSERT_NE(nullptr, pj.get()) << tc.proj_str;

    const double lon = DegToRad(tc.lon_deg);
    const double lat = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.proj_str;
    EXPECT_NEAR(tc.expected_x, fwd.xy.x, 1e-3);
    EXPECT_NEAR(tc.expected_y, fwd.xy.y, 1e-3);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.proj_str;
    EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon), 1e-7);
    EXPECT_NEAR(lat, inv.lp.phi, 1e-7);
  }
}

TEST(SomTest, SphericalVsDefaultGrs80VsWgs84Ellipsoids) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj sphere(proj_create(ctx.get(),
                              "+proj=som +inc_angle=98.2 +ps_rev=0.0686695 "
                              "+asc_lon=0 +R=6371000"));
  ASSERT_NE(nullptr, sphere.get());
  const double lon_sph = DegToRad(10.0);
  const double lat_sph = DegToRad(45.0);
  const PJ_COORD fwd_sph =
      proj_trans(sphere.get(), PJ_FWD, proj_coord(lon_sph, lat_sph, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sphere.get()));
  EXPECT_NEAR(5051046.540695, fwd_sph.xy.x, 1e-3);
  EXPECT_NEAR(-1077410.356348, fwd_sph.xy.y, 1e-3);

  const PJ_COORD inv_sph = proj_trans(sphere.get(), PJ_INV, fwd_sph);
  EXPECT_EQ(0, proj_errno(sphere.get()));
  EXPECT_NEAR(lon_sph, inv_sph.lp.lam, 1e-7);
  EXPECT_NEAR(lat_sph, inv_sph.lp.phi, 1e-7);

  // Default ellipsoid (GRS80 when +ellps is omitted) vs WGS84 at (120E, 35N).
  ScopedPj grs80(proj_create(
      ctx.get(), "+proj=som +inc_angle=98.2 +ps_rev=0.0686695 +asc_lon=129.3"));
  ScopedPj wgs84(proj_create(ctx.get(),
                             "+proj=som +inc_angle=98.2 +ps_rev=0.0686695 "
                             "+asc_lon=129.3 +ellps=WGS84"));
  ASSERT_NE(nullptr, grs80.get());
  ASSERT_NE(nullptr, wgs84.get());
  const PJ_COORD pt_deg = proj_coord(DegToRad(120.0), DegToRad(35.0), 0.0, 0.0);
  const PJ_COORD fwd_grs = proj_trans(grs80.get(), PJ_FWD, pt_deg);
  const PJ_COORD fwd_wgs = proj_trans(wgs84.get(), PJ_FWD, pt_deg);
  EXPECT_EQ(0, proj_errno(grs80.get()));
  EXPECT_EQ(0, proj_errno(wgs84.get()));
  EXPECT_NEAR(3946967.409671, fwd_grs.xy.x, 1e-6);
  EXPECT_NEAR(608629.239277, fwd_grs.xy.y, 1e-6);

  // WGS84 has flattening 1/298.257223563 vs GRS80 1/298.257222101.
  EXPECT_GT(std::abs(fwd_grs.xy.x - fwd_wgs.xy.x), 5e-5);
}

TEST(SomTest, LsatGlobalGridCoversQuadrantsAndRlmWrapBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=lsat +lsat=5 +path=1 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  constexpr double kLatDegs[] = {-75.0, -45.0, -15.0, -0.01, 0.0,
                                 0.01,  15.0,  45.0,  75.0};
  for (int lon_deg = -180; lon_deg <= 165; lon_deg += 15) {
    for (const double lat_deg : kLatDegs) {
      const double lon = DegToRad(static_cast<double>(lon_deg));
      const double lat = DegToRad(lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 0.0, 0.0));
      ASSERT_EQ(0, proj_errno(pj.get()))
          << "lon=" << lon_deg << " lat=" << lat_deg;
      ASSERT_TRUE(std::isfinite(fwd.xy.x));
      ASSERT_TRUE(std::isfinite(fwd.xy.y));

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      ASSERT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon), 1e-6)
          << "lon=" << lon_deg << " lat=" << lat_deg;
      EXPECT_NEAR(lat, inv.lp.phi, 1e-6)
          << "lon=" << lon_deg << " lat=" << lat_deg;
    }
  }

  // Trigger lamdp >= Q->rlm2 wrap branch in som_e_forward.
  {
    const double lon = adjlon(pj->lam0 + 1.292);
    const double lat = 0.5;
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_TRUE(std::isfinite(fwd.xy.x));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon), 1e-6);
    EXPECT_NEAR(lat, inv.lp.phi, 1e-6);
  }

  // Trigger fabs(cos(lamt)) < TOL adjustment in som_e_forward inner loop.
  {
    constexpr double kP22Lsat5 = 98.8841202 / 1440.0;
    const double lon = adjlon(pj->lam0 + (1.0 - kP22Lsat5) * kHalfPi);
    const double lat = 0.5;
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_TRUE(std::isfinite(fwd.xy.x));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon), 1e-6);
    EXPECT_NEAR(lat, inv.lp.phi, 1e-6);
  }
}

TEST(SomTest, SpecialInclinationSetupAndInverseBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Polar orbit (inc_angle = 90 deg) triggers fabs(Q->ca) < 1e-9 clamp.
  {
    ScopedPj polar(proj_create(ctx.get(),
                               "+proj=som +inc_angle=90 +ps_rev=0.0686695 "
                               "+asc_lon=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, polar.get());

    const PJ_COORD fwd1 =
        proj_trans(polar.get(), PJ_FWD,
                   proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(polar.get()));
    EXPECT_NEAR(5089990.789905, fwd1.xy.x, 1e-3);
    EXPECT_NEAR(-403864.745670, fwd1.xy.y, 1e-3);
    const PJ_COORD inv1 = proj_trans(polar.get(), PJ_INV, fwd1);
    EXPECT_EQ(0, proj_errno(polar.get()));
    EXPECT_NEAR(DegToRad(10.0), inv1.lp.lam, 1e-7);
    EXPECT_NEAR(DegToRad(45.0), inv1.lp.phi, 1e-7);

    const PJ_COORD fwd2 =
        proj_trans(polar.get(), PJ_FWD,
                   proj_coord(DegToRad(-40.0), DegToRad(-25.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(polar.get()));
    EXPECT_NEAR(36896088.834881, fwd2.xy.x, 1e-3);
    EXPECT_NEAR(1349830.396302, fwd2.xy.y, 1e-3);
    const PJ_COORD inv2 = proj_trans(polar.get(), PJ_INV, fwd2);
    EXPECT_EQ(0, proj_errno(polar.get()));
    EXPECT_NEAR(DegToRad(-40.0), inv2.lp.lam, 1e-7);
    EXPECT_NEAR(DegToRad(-25.0), inv2.lp.phi, 1e-7);
  }

  // 2. Equatorial orbit (inc_angle = 0 deg) triggers fabs(Q->sa) < TOL in inv.
  {
    ScopedPj eq_wgs(proj_create(
        ctx.get(), "+proj=som +inc_angle=0 +ps_rev=0 +asc_lon=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, eq_wgs.get());
    const PJ_COORD fwd =
        proj_trans(eq_wgs.get(), PJ_FWD,
                   proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(eq_wgs.get()));
    EXPECT_NEAR(1669792.361899, fwd.xy.x, 1e-3);
    EXPECT_NEAR(3478671.976783, fwd.xy.y, 1e-3);

    const PJ_COORD inv = proj_trans(eq_wgs.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(eq_wgs.get()));
    EXPECT_NEAR(DegToRad(15.0), inv.lp.lam, 1e-7);
    EXPECT_NEAR(DegToRad(30.0), inv.lp.phi, 1e-7);
  }

  // 3. Spherical inc_angle = 0 with x = (pi/2)*R triggers cos(lamdp) < TOL
  // clamp.
  {
    constexpr double kRadius = 6371000.0;
    ScopedPj eq_sph(proj_create(
        ctx.get(), "+proj=som +inc_angle=0 +ps_rev=0 +asc_lon=0 +R=6371000"));
    ASSERT_NE(nullptr, eq_sph.get());
    const PJ_COORD inv = proj_trans(
        eq_sph.get(), PJ_INV, proj_coord(kHalfPi * kRadius, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(eq_sph.get()));
    EXPECT_NEAR(kHalfPi - 1e-7, inv.lp.lam, 1e-12);
    EXPECT_NEAR(0.0, inv.lp.phi, 1e-12);
  }

  // 4. Retrograde equatorial orbit (inc_angle = 180 deg) exercises ca = -1.
  {
    ScopedPj retro(proj_create(
        ctx.get(),
        "+proj=som +inc_angle=180 +ps_rev=0 +asc_lon=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, retro.get());
    const PJ_COORD fwd =
        proj_trans(retro.get(), PJ_FWD,
                   proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(retro.get()));
    EXPECT_NEAR(38405224.323679, fwd.xy.x, 1e-3);
    EXPECT_NEAR(-3478671.976783, fwd.xy.y, 1e-3);

    const PJ_COORD eq_pt = proj_coord(DegToRad(15.0), 0.0, 0.0, 0.0);
    const PJ_COORD fwd_eq = proj_trans(retro.get(), PJ_FWD, eq_pt);
    const PJ_COORD inv_eq = proj_trans(retro.get(), PJ_INV, fwd_eq);
    EXPECT_EQ(0, proj_errno(retro.get()));
    EXPECT_NEAR(eq_pt.lp.lam, inv_eq.lp.lam, 1e-7);
    EXPECT_NEAR(0.0, inv_eq.lp.phi, 1e-7);
  }
}

TEST(SomTest, ForwardClampingNonConvergenceAndInverseSingularity) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Direct pj->fwd exercises internal latitude overshoot clamping.
  {
    ScopedPj pj(proj_create(ctx.get(),
                            "+proj=som +inc_angle=98.2 +ps_rev=0.0686695 "
                            "+asc_lon=129.3 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    ASSERT_NE(nullptr, pj->fwd);

    const PJ_XY at_north = pj->fwd({.lam = 0.0, .phi = kHalfPi}, pj.get());
    const PJ_XY over_north =
        pj->fwd({.lam = 0.0, .phi = kHalfPi + 1e-4}, pj.get());
    EXPECT_DOUBLE_EQ(at_north.x, over_north.x);
    EXPECT_DOUBLE_EQ(at_north.y, over_north.y);

    const PJ_XY at_south = pj->fwd({.lam = 0.0, .phi = -kHalfPi}, pj.get());
    const PJ_XY under_south =
        pj->fwd({.lam = 0.0, .phi = -kHalfPi - 1e-4}, pj.get());
    EXPECT_DOUBLE_EQ(at_south.x, under_south.x);
    EXPECT_DOUBLE_EQ(at_south.y, under_south.y);
  }

  // 2. Non-converging ps_rev=0.735 at (0, 0) exhausts iteration limit (l == 0)
  //    and sets xy.x = xy.y = HUGE_VAL without altering proj_errno.
  {
    ScopedPj nonconv(proj_create(ctx.get(),
                                 "+proj=som +inc_angle=0 +ps_rev=0.735 "
                                 "+asc_lon=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, nonconv.get());
    const PJ_COORD res =
        proj_trans(nonconv.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, res.xyzt.x);
    EXPECT_EQ(HUGE_VAL, res.xyzt.y);
    EXPECT_EQ(0, proj_errno(nonconv.get()));
  }

  // 3. Inverse denom == 0.0 singularity on sphere for extreme |y| sets domain
  //    error and returns HUGE_VAL; proj_errno_reset recovers normal operation.
  {
    ScopedPj sph(proj_create(ctx.get(),
                             "+proj=som +R=6371000 +inc_angle=98.2 "
                             "+ps_rev=0.0686695 +asc_lon=0"));
    ASSERT_NE(nullptr, sph.get());
    const PJ_COORD bad_inv =
        proj_trans(sph.get(), PJ_INV, proj_coord(0.0, 5e8, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad_inv.lpzt.lam);
    EXPECT_EQ(HUGE_VAL, bad_inv.lpzt.phi);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(sph.get()));

    proj_errno_reset(sph.get());
    EXPECT_EQ(0, proj_errno(sph.get()));

    const PJ_COORD valid_fwd =
        proj_trans(sph.get(), PJ_FWD,
                   proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(sph.get()));
    EXPECT_TRUE(std::isfinite(valid_fwd.xy.x));
  }

  // 4. General invalid inputs (NaN, HUGE_VAL, |phi| > pi/2).
  {
    ScopedLogSuppressor suppress_logs(ctx.get());
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=lsat +lsat=5 +path=1 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD nan_coord = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isnan(nan_coord.xyzt.x));

    const PJ_COORD inf_coord =
        proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inf_coord.xyzt.x);

    const PJ_COORD over_lat =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, over_lat.xyzt.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
    proj_errno_reset(pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }
}

TEST(SomTest, FactorsBatchArraysClassicEntryPointsAndRoundTripApi) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=som +inc_angle=98.2 +ps_rev=0.0686695 "
                          "+asc_lon=129.3 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Scale and distortion factors at (120E, 35N).
  const PJ_COORD pt = proj_coord(DegToRad(120.0), DegToRad(35.0), 0.0, 0.0);
  const PJ_FACTORS fac = proj_factors(pj.get(), pt);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.00013756, fac.meridional_scale, 1e-6);
  EXPECT_NEAR(1.00012244, fac.parallel_scale, 1e-6);
  EXPECT_NEAR(1.00026002, fac.areal_scale, 1e-6);
  EXPECT_NEAR(-1.700218, fac.meridian_convergence, 1e-5);
  EXPECT_NEAR(5.4459e-5, fac.angular_distortion, 1e-7);

  // 2. Legacy 2D entry points.
  const PJ_XY xy = pj_fwd(pt.lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(pt.lp.lam, lp.lam, 1e-7);
  EXPECT_NEAR(pt.lp.phi, lp.phi, 1e-7);

  // 3. Batch transformation via proj_trans_array.
  std::array<PJ_COORD, 2> pts = {
      proj_coord(DegToRad(120.0), DegToRad(35.0), 0.0, 0.0),
      proj_coord(DegToRad(-75.0), DegToRad(40.0), 50.0, 1.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, pts.size(), pts.data()));
  EXPECT_NEAR(3946967.409780, pts[0].xy.x, 1e-3);
  EXPECT_NEAR(15652848.669714, pts[1].xy.x, 1e-3);
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, pts.size(), pts.data()));
  EXPECT_NEAR(0.0, adjlon(pts[0].lp.lam - DegToRad(120.0)), 1e-7);
  EXPECT_NEAR(DegToRad(40.0), pts[1].lp.phi, 1e-7);
  EXPECT_DOUBLE_EQ(50.0, pts[1].lpz.z);

  {
    ScopedLogSuppressor suppress_logs(ctx.get());
    std::array<PJ_COORD, 2> mixed = {
        proj_coord(DegToRad(120.0), DegToRad(35.0), 0.0, 0.0),
        proj_coord(0.0, kHalfPi + 0.1, 0.0, 0.0),
    };
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
              proj_trans_array(pj.get(), PJ_FWD, mixed.size(), mixed.data()));
    EXPECT_NEAR(3946967.409780, mixed[0].xy.x, 1e-3);
    EXPECT_EQ(HUGE_VAL, mixed[1].xy.x);
    proj_errno_reset(pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
  }

  // 4. Generic strided batch transformation.
  std::array<double, 2> x = {DegToRad(120.0), DegToRad(10.0)};
  std::array<double, 2> y = {DegToRad(35.0), DegToRad(-30.0)};
  const std::size_t done_fwd = proj_trans_generic(
      pj.get(), PJ_FWD, x.data(), sizeof(double), x.size(), y.data(),
      sizeof(double), y.size(), nullptr, 0, 0, nullptr, 0, 0);
  EXPECT_EQ(2U, done_fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(27472327.257122, x[1], 1e-3);

  // 5. proj_roundtrip surface error should stay within ~1m given TOL=1e-7.
  PJ_COORD rt_pt = proj_coord(DegToRad(120.0), DegToRad(35.0), 0.0, 0.0);
  const double rt_dist_m = proj_roundtrip(pj.get(), PJ_FWD, 3, &rt_pt);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_LT(rt_dist_m, 2.0);
}

}  // namespace
}  // namespace proj
