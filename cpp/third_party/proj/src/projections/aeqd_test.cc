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
// Unit tests for the Azimuthal Equidistant (aeqd) projection including
// spherical, ellipsoidal, and Guam variants (proj/src/projections/aeqd.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/aeqd.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/geodesic.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kEarthRadiusM = 6371000.0;
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84F = 1.0 / 298.257223563;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(AeqdTest, MetadataAndNullDestructorGuard) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ellps(proj_create(ctx.get(), "+proj=aeqd +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ellps.get());
  const PJ_PROJ_INFO info = proj_pj_info(pj_ellps.get());
  EXPECT_STREQ("aeqd", info.id);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, proj_angular_input(pj_ellps.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj_ellps.get(), PJ_FWD));
  EXPECT_EQ(nullptr, pj_ellps->destructor(nullptr, 0));

  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=aeqd +R=6371000"));
  ASSERT_NE(nullptr, pj_sphere.get());
  EXPECT_EQ(0.0, pj_sphere->es);
  EXPECT_EQ(nullptr, pj_sphere->destructor(nullptr, 0));
}

TEST(AeqdTest, SphericalAllFourModesForwardInverseAndOrigins) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Equatorial mode (omitted +lat_0 defaults to 0 -> EQUIT).
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=aeqd +R=6371000"));
    ASSERT_NE(nullptr, pj.get());

    // Exact origin hits sinc == 0 -> k = 1.0 in forward and c_rh < EPS10 in
    // inverse.
    const PJ_COORD fwd_origin =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, fwd_origin.xy.x);
    EXPECT_DOUBLE_EQ(0.0, fwd_origin.xy.y);

    const PJ_COORD inv_tiny =
        proj_trans(pj.get(), PJ_INV, proj_coord(1e-4, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, inv_tiny.lp.lam, 1e-12);
    EXPECT_NEAR(0.0, inv_tiny.lp.phi, 1e-12);

    struct EquitCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr EquitCase kCases[] = {
        {.lon_deg = 30.0, .lat_deg = 20.0},
        {.lon_deg = -45.0, .lat_deg = -15.0},
        {.lon_deg = 120.0, .lat_deg = 50.0},
        {.lon_deg = 0.0, .lat_deg = 90.0},
    };
    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));

      // Radial distance on sphere equals R * great-circle angle c.
      const double expected_dist =
          kEarthRadiusM * std::acos(std::cos(phi) * std::cos(lam));
      EXPECT_NEAR(expected_dist, std::hypot(fwd.xy.x, fwd.xy.y), 1e-6);

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
      if (std::abs(std::abs(phi) - kHalfPi) > 1e-9) {
        EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
      }
    }
  }

  // 2. Oblique mode (positive and negative +lat_0).
  {
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=45 +lon_0=10"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD fwd_origin = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, fwd_origin.xy.x, 1e-9);
    EXPECT_NEAR(0.0, fwd_origin.xy.y, 1e-9);

    const PJ_COORD inv_tiny =
        proj_trans(pj.get(), PJ_INV, proj_coord(1e-4, -1e-4, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(10.0), inv_tiny.lp.lam, 1e-12);
    EXPECT_NEAR(DegToRad(45.0), inv_tiny.lp.phi, 1e-12);

    struct ObliqCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr ObliqCase kCases[] = {
        {.lon_deg = 25.0, .lat_deg = 60.0},
        {.lon_deg = -20.0, .lat_deg = 10.0},
        {.lon_deg = 160.0, .lat_deg = -30.0},
        // North pole maps straight north at distance R * (pi/2 - lat_0).
        {.lon_deg = 10.0, .lat_deg = 90.0},
    };
    for (const auto& tc : kCases) {
      const double lam = DegToRad(tc.lon_deg);
      const double phi = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
      if (std::abs(std::abs(phi) - kHalfPi) > 1e-9) {
        EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
      }
    }
  }

  // 3. North Pole mode (+lat_0=90).
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=90"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD fwd_pole =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, fwd_pole.xy.x, 1e-9);
    EXPECT_NEAR(0.0, fwd_pole.xy.y, 1e-9);
    const PJ_COORD inv_pole = proj_trans(pj.get(), PJ_INV, fwd_pole);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kHalfPi, inv_pole.lp.phi, 1e-12);

    // 60 deg N at 30 deg E is south (-y) and east (+x) from North Pole.
    const double rho = kEarthRadiusM * (kPi / 6.0);
    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(30.0), DegToRad(60.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(rho * std::sin(DegToRad(30.0)), fwd.xy.x, 1e-6);
    EXPECT_NEAR(-rho * std::cos(DegToRad(30.0)), fwd.xy.y, 1e-6);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(30.0), inv.lp.lam, 1e-12);
    EXPECT_NEAR(DegToRad(60.0), inv.lp.phi, 1e-12);
  }

  // 4. South Pole mode (+lat_0=-90).
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=-90"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD fwd_pole =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, fwd_pole.xy.x, 1e-9);
    EXPECT_NEAR(0.0, fwd_pole.xy.y, 1e-9);
    const PJ_COORD inv_pole = proj_trans(pj.get(), PJ_INV, fwd_pole);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-kHalfPi, inv_pole.lp.phi, 1e-12);

    // 60 deg S at 30 deg E is north (+y) and east (+x) from South Pole.
    const double rho = kEarthRadiusM * (kPi / 6.0);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(30.0), DegToRad(-60.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(rho * std::sin(DegToRad(30.0)), fwd.xy.x, 1e-6);
    EXPECT_NEAR(rho * std::cos(DegToRad(30.0)), fwd.xy.y, 1e-6);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(30.0), inv.lp.lam, 1e-12);
    EXPECT_NEAR(DegToRad(-60.0), inv.lp.phi, 1e-12);
  }
}

TEST(AeqdTest, SphericalDomainErrorsAndAntipodalClamping) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Equatorial antipode (180 deg, 0 deg) has cosc = -1 < TOL - 1 -> error.
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=0"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD antipode =
        proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(antipode.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());

    // Inverse beyond outer horizon c_rh - EPS10 > pi -> error.
    const PJ_COORD beyond = proj_trans(
        pj.get(), PJ_INV, proj_coord(kEarthRadiusM * 3.5, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(beyond.lp.lam));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());

    // Inverse inside tolerance band pi < c_rh <= pi + EPS10 clamps c_rh = pi.
    const PJ_COORD clamped =
        proj_trans(pj.get(), PJ_INV,
                   proj_coord(kEarthRadiusM * (kPi + 0.5e-10), 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kPi, std::abs(clamped.lp.lam), 1e-10);
    EXPECT_NEAR(0.0, clamped.lp.phi, 1e-10);
  }

  // 2. Oblique antipode (180 deg, -45 deg for lat_0=45 deg) -> error.
  {
    ScopedPj pj(proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=45"));
    ASSERT_NE(nullptr, pj.get());
    const PJ_COORD antipode = proj_trans(
        pj.get(), PJ_FWD, proj_coord(kPi, DegToRad(-45.0), 0.0, 0.0));
    EXPECT_TRUE(std::isinf(antipode.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
  }

  // 3. Polar antipodes: South Pole in N_POLE mode and North Pole in S_POLE mode
  // trigger fabs(lp.phi - M_HALFPI) < EPS10 -> error.
  {
    ScopedPj pj_n(proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=90"));
    ASSERT_NE(nullptr, pj_n.get());
    const PJ_COORD s_pole =
        proj_trans(pj_n.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(s_pole.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj_n.get()));

    ScopedPj pj_s(proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=-90"));
    ASSERT_NE(nullptr, pj_s.get());
    const PJ_COORD n_pole =
        proj_trans(pj_s.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(n_pole.xy.x));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj_s.get()));
  }
}

TEST(AeqdTest, EllipsoidalAllFourModesMatchesGeodesicAndRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct geod_geodesic wgs84_geod{};
  geod_init(&wgs84_geod, kWgs84A, kWgs84F);

  // 1. Equatorial ellipsoidal (+lat_0=0 +lon_0=0) and Oblique (+lat_0=52.5
  // +lon_0=13.4) modes cross-checked against geodesic.h (degrees, (lat, lon)).
  struct CenterConfig {
    const char* proj_str;
    double lon0_deg;
    double lat0_deg;
    double test_lon_deg;
    double test_lat_deg;
  };
  constexpr CenterConfig kCenterConfigs[] = {
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=0 +lon_0=0",
          .lon0_deg = 0.0,
          .lat0_deg = 0.0,
          .test_lon_deg = 12.0,
          .test_lat_deg = 55.0,
      },
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=52.5 +lon_0=13.4",
          .lon0_deg = 13.4,
          .lat0_deg = 52.5,
          .test_lon_deg = -74.0,
          .test_lat_deg = 40.7,
      },
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=-33.9 +lon_0=151.2",
          .lon0_deg = 151.2,
          .lat0_deg = -33.9,
          .test_lon_deg = 139.7,
          .test_lat_deg = 35.7,
      },
  };

  for (const auto& cfg : kCenterConfigs) {
    ScopedPj pj(proj_create(ctx.get(), cfg.proj_str));
    ASSERT_NE(nullptr, pj.get());

    // Exact origin and sub-EPS10 perturbation hit the zero-distance guard.
    const double lon0_rad = DegToRad(cfg.lon0_deg);
    const double lat0_rad = DegToRad(cfg.lat0_deg);
    const PJ_COORD fwd_near = proj_trans(
        pj.get(), PJ_FWD, proj_coord(lon0_rad + 1e-11, lat0_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_DOUBLE_EQ(0.0, fwd_near.xy.x);
    EXPECT_DOUBLE_EQ(0.0, fwd_near.xy.y);

    const PJ_COORD inv_near =
        proj_trans(pj.get(), PJ_INV, proj_coord(1e-4, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon0_rad, inv_near.lp.lam, 1e-12);
    EXPECT_NEAR(lat0_rad, inv_near.lp.phi, 1e-12);

    // Compare forward (x, y) against geod_inverse(lat1, lon1, lat2, lon2).
    double s12 = 0.0;
    double azi1 = 0.0;
    double azi2 = 0.0;
    geod_inverse(&wgs84_geod, cfg.lat0_deg, cfg.lon0_deg, cfg.test_lat_deg,
                 cfg.test_lon_deg, &s12, &azi1, &azi2);
    const double expected_x = s12 * std::sin(DegToRad(azi1));
    const double expected_y = s12 * std::cos(DegToRad(azi1));

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(cfg.test_lon_deg),
                              DegToRad(cfg.test_lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-6);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-6);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(cfg.test_lon_deg), inv.lp.lam, 1e-11);
    EXPECT_NEAR(DegToRad(cfg.test_lat_deg), inv.lp.phi, 1e-11);
  }

  // 2. North Pole (+lat_0=90) and South Pole (+lat_0=-90) ellipsoidal modes.
  struct PolarConfig {
    const char* proj_str;
    double pole_lat_deg;
    double test_lon_deg;
    double test_lat_deg;
  };
  constexpr PolarConfig kPolarConfigs[] = {
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=90",
          .pole_lat_deg = 90.0,
          .test_lon_deg = 45.0,
          .test_lat_deg = 60.0,
      },
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=90",
          .pole_lat_deg = 90.0,
          .test_lon_deg = -100.0,
          .test_lat_deg = -15.0,
      },
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=-90",
          .pole_lat_deg = -90.0,
          .test_lon_deg = 45.0,
          .test_lat_deg = -60.0,
      },
      {
          .proj_str = "+proj=aeqd +ellps=WGS84 +lat_0=-90",
          .pole_lat_deg = -90.0,
          .test_lon_deg = -100.0,
          .test_lat_deg = 15.0,
      },
  };

  for (const auto& cfg : kPolarConfigs) {
    ScopedPj pj(proj_create(ctx.get(), cfg.proj_str));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD fwd_pole =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(0.0, DegToRad(cfg.pole_lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, fwd_pole.xy.x, 1e-9);
    EXPECT_NEAR(0.0, fwd_pole.xy.y, 1e-9);

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(cfg.test_lon_deg),
                              DegToRad(cfg.test_lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    double s12 = 0.0;
    geod_inverse(&wgs84_geod, cfg.pole_lat_deg, 0.0, cfg.test_lat_deg,
                 cfg.test_lon_deg, &s12, nullptr, nullptr);
    EXPECT_NEAR(s12, std::hypot(fwd.xy.x, fwd.xy.y), 1e-6);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(cfg.test_lon_deg), inv.lp.lam, 1e-11);
    EXPECT_NEAR(DegToRad(cfg.test_lat_deg), inv.lp.phi, 1e-11);
  }
}

TEST(AeqdTest, GuamEllipticalForwardAndInverseRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Guam 1963 local Azimuthal Equidistant parameters on Clarke 1866.
  constexpr double kGuamLon0Deg = 144.7487507055556;
  constexpr double kGuamLat0Deg = 13.47246635277778;
  ScopedPj pj_guam(
      proj_create(ctx.get(),
                  "+proj=aeqd +ellps=clrk66 +lat_0=13.47246635277778 "
                  "+lon_0=144.7487507055556 +x_0=50000 +y_0=50000 +guam"));
  ASSERT_NE(nullptr, pj_guam.get());

  // 1. Origin maps to exact false easting/northing (50000, 50000) and back.
  const PJ_COORD fwd_origin = proj_trans(
      pj_guam.get(), PJ_FWD,
      proj_coord(DegToRad(kGuamLon0Deg), DegToRad(kGuamLat0Deg), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_guam.get()));
  EXPECT_NEAR(50000.0, fwd_origin.xy.x, 1e-9);
  EXPECT_NEAR(50000.0, fwd_origin.xy.y, 1e-9);

  const PJ_COORD inv_origin =
      proj_trans(pj_guam.get(), PJ_INV, proj_coord(50000.0, 50000.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_guam.get()));
  EXPECT_NEAR(DegToRad(kGuamLon0Deg), inv_origin.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(kGuamLat0Deg), inv_origin.lp.phi, 1e-12);

  // Verify +guam selects the Guam approximation rather than exact geodesic
  // Azimuthal Equidistant away from origin.
  ScopedPj pj_exact(
      proj_create(ctx.get(),
                  "+proj=aeqd +ellps=clrk66 +lat_0=13.47246635277778 "
                  "+lon_0=144.7487507055556 +x_0=50000 +y_0=50000"));
  ASSERT_NE(nullptr, pj_exact.get());
  const PJ_COORD off_center =
      proj_coord(DegToRad(145.5), DegToRad(14.0), 0.0, 0.0);
  const PJ_COORD guam_pt = proj_trans(pj_guam.get(), PJ_FWD, off_center);
  const PJ_COORD exact_pt = proj_trans(pj_exact.get(), PJ_FWD, off_center);
  EXPECT_EQ(0, proj_errno(pj_guam.get()));
  EXPECT_EQ(0, proj_errno(pj_exact.get()));
  EXPECT_GT(
      std::hypot(guam_pt.xy.x - exact_pt.xy.x, guam_pt.xy.y - exact_pt.xy.y),
      1e-2);

  // 2. Points across Guam round-trip cleanly via 3-iteration e_guam_inv.
  struct GuamCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr GuamCase kCases[] = {
      {.lon_deg = kGuamLon0Deg, .lat_deg = kGuamLat0Deg},
      {.lon_deg = 144.75, .lat_deg = 13.48},
      {.lon_deg = 144.85, .lat_deg = 13.55},
      {.lon_deg = 144.65, .lat_deg = 13.30},
  };
  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj_guam.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_guam.get()));

    const PJ_COORD inv = proj_trans(pj_guam.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj_guam.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-11);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-11);
  }
}

TEST(AeqdTest, FactorsLinearUnitsBatchAndSpecialValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. proj_factors on spherical and ellipsoidal oblique aeqd: near-unit scale
  // at center and growing azimuthal distortion away from center.
  ScopedPj pj_sphere(
      proj_create(ctx.get(), "+proj=aeqd +R=6371000 +lat_0=45 +lon_0=10"));
  ScopedPj pj_ellps(
      proj_create(ctx.get(), "+proj=aeqd +ellps=WGS84 +lat_0=45 +lon_0=10"));
  ASSERT_NE(nullptr, pj_sphere.get());
  ASSERT_NE(nullptr, pj_ellps.get());

  const PJ_FACTORS near_fac =
      proj_factors(pj_sphere.get(),
                   proj_coord(DegToRad(10.001), DegToRad(45.001), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(1.0, near_fac.meridional_scale, 1e-4);
  EXPECT_NEAR(1.0, near_fac.parallel_scale, 1e-4);
  EXPECT_NEAR(1.0, near_fac.areal_scale, 1e-4);

  const PJ_FACTORS far_fac = proj_factors(
      pj_ellps.get(), proj_coord(DegToRad(30.0), DegToRad(55.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_GT(far_fac.areal_scale, 1.0);
  EXPECT_GT(far_fac.angular_distortion, 0.0);

  // 2. False easting/northing, +units=km, and 4D (z, t) pass-through.
  ScopedPj pj_km(proj_create(
      ctx.get(),
      "+proj=aeqd +ellps=WGS84 +lat_0=45 +lon_0=10 +x_0=100000 +y_0=200000 "
      "+units=km"));
  ASSERT_NE(nullptr, pj_km.get());
  const PJ_COORD pt_4d =
      proj_coord(DegToRad(12.0), DegToRad(46.0), 345.6, 2026.5);
  const PJ_COORD fwd_km = proj_trans(pj_km.get(), PJ_FWD, pt_4d);
  EXPECT_EQ(0, proj_errno(pj_km.get()));
  EXPECT_NEAR(0.3456, fwd_km.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.5, fwd_km.xyzt.t, 1e-12);
  const PJ_COORD inv_km = proj_trans(pj_km.get(), PJ_INV, fwd_km);
  EXPECT_EQ(0, proj_errno(pj_km.get()));
  EXPECT_NEAR(pt_4d.lpzt.lam, inv_km.lpzt.lam, 1e-11);
  EXPECT_NEAR(pt_4d.lpzt.phi, inv_km.lpzt.phi, 1e-11);
  EXPECT_NEAR(345.6, inv_km.lpzt.z, 1e-12);
  EXPECT_NEAR(2026.5, inv_km.lpzt.t, 1e-12);

  // 3. Batch transform with proj_trans_generic.
  std::array<double, 2> x = {DegToRad(10.0), DegToRad(20.0)};
  std::array<double, 2> y = {DegToRad(45.0), DegToRad(50.0)};
  EXPECT_EQ(2u,
            proj_trans_generic(pj_ellps.get(), PJ_FWD, x.data(), sizeof(double),
                               x.size(), y.data(), sizeof(double), y.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_EQ(2u,
            proj_trans_generic(pj_ellps.get(), PJ_INV, x.data(), sizeof(double),
                               x.size(), y.data(), sizeof(double), y.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_ellps.get()));
  EXPECT_NEAR(DegToRad(10.0), x[0], 1e-11);
  EXPECT_NEAR(DegToRad(45.0), y[0], 1e-11);
  EXPECT_NEAR(DegToRad(20.0), x[1], 1e-11);
  EXPECT_NEAR(DegToRad(50.0), y[1], 1e-11);

  // 4. proj_trans_array on valid batch succeeds (0 == success), and mixed
  // valid + out-of-domain spherical inverse returns OUTSIDE_PROJECTION_DOMAIN
  // while transforming valid items.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0),
      proj_coord(DegToRad(25.0), DegToRad(60.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj_ellps.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj_ellps.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(10.0), valid_batch[0].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(45.0), valid_batch[0].lp.phi, 1e-11);
  EXPECT_NEAR(DegToRad(25.0), valid_batch[1].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(60.0), valid_batch[1].lp.phi, 1e-11);

  ScopedLogSuppressor suppress(ctx.get());
  std::array<PJ_COORD, 2> batch = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(kEarthRadiusM * 3.5, 0.0, 0.0, 0.0),
  };
  EXPECT_EQ(
      PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
      proj_trans_array(pj_sphere.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(DegToRad(10.0), batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(45.0), batch[0].lp.phi, 1e-12);
  EXPECT_TRUE(std::isinf(batch[1].lp.lam));
  proj_errno_reset(pj_sphere.get());

  // 5. NaN and HUGE_VAL edge inputs.
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_res =
      proj_trans(pj_ellps.get(), PJ_FWD, proj_coord(kNan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_res.xy.x));
  EXPECT_TRUE(std::isnan(nan_res.xy.y));

  const PJ_COORD inf_res =
      proj_trans(pj_ellps.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_res.xy.x));
}

// TODO(schwehr): Unreachable lines in proj/src/projections/aeqd.cpp without
// fault injection:
// - OOM null checks on calloc/pj_enfn in PJ_PROJECTION(aeqd) (lines 257, 281).
// - destructor() P->opaque == nullptr fallback (line 63).

}  // namespace
}  // namespace proj
