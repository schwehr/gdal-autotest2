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
// Unit tests for the Interrupted Mollweide (imoll) projection
// (proj/src/projections/imoll.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/imoll.cpp

#include <array>
#include <cmath>
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

TEST(ImollTest, MetadataForcedSphericalAndNullDestructorGuard) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_default(proj_create(ctx.get(), "+proj=imoll"));
  ASSERT_NE(nullptr, pj_default.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj_default.get());
  EXPECT_STREQ("imoll", info.id);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, proj_angular_input(pj_default.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj_default.get(), PJ_FWD));
  // imoll is strictly spherical and forces P->es = 0 during setup.
  EXPECT_EQ(0.0, pj_default->es);
  EXPECT_EQ(nullptr, pj_default->destructor(nullptr, 0));

  // Passing an ellipsoid forces es == 0 while preserving semimajor axis `a`.
  ScopedPj pj_wgs84(proj_create(ctx.get(), "+proj=imoll +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_wgs84.get());
  EXPECT_EQ(0.0, pj_wgs84->es);

  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=imoll +R=6378137"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD input = proj_coord(DegToRad(20.0), DegToRad(-30.0), 0.0, 0.0);
  const PJ_COORD out_wgs84 = proj_trans(pj_wgs84.get(), PJ_FWD, input);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj_wgs84.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_wgs84.xy.x, 1e-6);
  EXPECT_NEAR(out_sphere.xy.y, out_wgs84.xy.y, 1e-6);
}

TEST(ImollTest, ForwardAndInverseRoundTripsAllSixZones) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=imoll +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct ZoneCase {
    double lon_deg;
    double lat_deg;
    double tol_rad;
  };
  constexpr ZoneCase kCases[] = {
      // Zone 1: phi >= 0, lam <= -40 deg (lon_0=-100).
      {.lon_deg = -100.0, .lat_deg = 30.0, .tol_rad = 1e-9},
      {.lon_deg = -40.0, .lat_deg = 0.0, .tol_rad = 1e-9},
      {.lon_deg = -175.0, .lat_deg = 70.0, .tol_rad = 1e-9},
      // Zone 2: phi >= 0, lam > -40 deg (lon_0=+30).
      {.lon_deg = 30.0, .lat_deg = 30.0, .tol_rad = 1e-9},
      {.lon_deg = 0.0, .lat_deg = 0.0, .tol_rad = 1e-9},
      {.lon_deg = 175.0, .lat_deg = 70.0, .tol_rad = 1e-9},
      // Zone 3: phi < 0, lam <= -100 deg (lon_0=-160).
      {.lon_deg = -160.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = -100.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = -175.0, .lat_deg = -80.0, .tol_rad = 1e-8},
      // Zone 4: phi < 0, -100 < lam <= -20 deg (lon_0=-60).
      {.lon_deg = -60.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = -20.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = -50.0, .lat_deg = -80.0, .tol_rad = 1e-8},
      // Zone 5: phi < 0, -20 < lam <= 80 deg (lon_0=+20).
      {.lon_deg = 20.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = 80.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = 40.0, .lat_deg = -80.0, .tol_rad = 1e-8},
      // Zone 6: phi < 0, lam > 80 deg (lon_0=+140).
      {.lon_deg = 140.0, .lat_deg = -30.0, .tol_rad = 1e-9},
      {.lon_deg = 175.0, .lat_deg = -80.0, .tol_rad = 1e-8},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    // 1. Forward projection to projected meters.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()))
        << "lon=" << tc.lon_deg << " lat=" << tc.lat_deg;
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    // 2. Inverse projection back to (lon, lat) in radians.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()))
        << "lon=" << tc.lon_deg << " lat=" << tc.lat_deg;
    EXPECT_NEAR(lon_rad, inv.lp.lam, tc.tol_rad)
        << "lon=" << tc.lon_deg << " lat=" << tc.lat_deg;
    EXPECT_NEAR(lat_rad, inv.lp.phi, tc.tol_rad)
        << "lon=" << tc.lon_deg << " lat=" << tc.lat_deg;
  }

  // Exact North Pole along zone 1 and 2 central meridians maps to y = sqrt(2)*R
  // and inverse-maps back to phi = +pi/2.
  constexpr double kNorthCentralMeridiansDeg[] = {-100.0, 30.0};
  for (const double lon0_deg : kNorthCentralMeridiansDeg) {
    const PJ_COORD fwd_pole = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(lon0_deg), kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(6371000.0 * std::sqrt(2.0), fwd_pole.xy.y, 1e-3);
    const PJ_COORD inv_pole = proj_trans(pj.get(), PJ_INV, fwd_pole);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kHalfPi, inv_pole.lp.phi, 1e-9);
  }
}

TEST(ImollTest, EquatorSeamContinuityAndVerticalRangeErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=imoll +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Verify compute_zone_offset aligns all zone seams continuously along
  // the equator (both across north/south and left/right zone boundaries).
  constexpr double kEquatorialSeamLonsDeg[] = {-160.0, -100.0, -40.0, -20.0,
                                               80.0};
  for (const double seam_lon_deg : kEquatorialSeamLonsDeg) {
    const double lam = DegToRad(seam_lon_deg);
    const PJ_COORD north_west =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam - 1e-9, 1e-9, 0.0, 0.0));
    const PJ_COORD north_east =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam + 1e-9, 1e-9, 0.0, 0.0));
    const PJ_COORD south_west =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam - 1e-9, -1e-9, 0.0, 0.0));
    const PJ_COORD south_east =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam + 1e-9, -1e-9, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(north_west.xy.x, north_east.xy.x, 1e-6)
        << "seam=" << seam_lon_deg;
    EXPECT_NEAR(north_west.xy.x, south_west.xy.x, 1e-6)
        << "seam=" << seam_lon_deg;
    EXPECT_NEAR(south_west.xy.x, south_east.xy.x, 1e-6)
        << "seam=" << seam_lon_deg;
  }

  // 2. Inverse y > sqrt(2) + EPSLN and y < -sqrt(2) + EPSLN both return
  // HUGE_VAL.
  const PJ_COORD above_north_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 1.5, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(above_north_pole.lp.lam));
  EXPECT_TRUE(std::isinf(above_north_pole.lp.phi));
  proj_errno_reset(pj.get());

  const PJ_COORD below_south_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -1.5, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(below_south_pole.lp.lam));
  EXPECT_TRUE(std::isinf(below_south_pole.lp.phi));
  proj_errno_reset(pj.get());

  // 3. Exact South Pole produces y = -sqrt(2), triggering the upstream
  // `xy.y < -y90 + EPSLN` guard on inverse.
  const PJ_COORD fwd_south_pole = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(20.0), -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD inv_south_pole = proj_trans(pj.get(), PJ_INV, fwd_south_pole);
  EXPECT_TRUE(std::isinf(inv_south_pole.lp.lam));
  EXPECT_TRUE(std::isinf(inv_south_pole.lp.phi));
}

TEST(ImollTest, InverseInterruptionGapsRejectAllSixZones) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=imoll +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Seam x boundaries on unit sphere:
  // boundary12 ~= -45.981 deg, boundary34 == -100 deg,
  // boundary45 ~= -27.975 deg, boundary56 ~= +62.057 deg.
  // Each (x, y) routes into its zone's Mollweide inverse, whose decoded
  // longitude falls outside that zone's interruption boundary.
  struct GapCase {
    int zone;
    double x;
    double y;
  };
  constexpr GapCase kGapCases[] = {
      {.zone = 1, .x = DegToRad(-47.0), .y = 1.0},
      {.zone = 2, .x = DegToRad(-44.0), .y = 1.0},
      {.zone = 3, .x = DegToRad(-102.0), .y = -1.0},
      {.zone = 4, .x = DegToRad(-98.0), .y = -1.0},
      {.zone = 5, .x = DegToRad(-26.0), .y = -1.0},
      {.zone = 6, .x = DegToRad(64.0), .y = -1.0},
  };

  for (const auto& tc : kGapCases) {
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(tc.x, tc.y, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(inv.lp.lam)) << "zone=" << tc.zone;
    EXPECT_TRUE(std::isinf(inv.lp.phi)) << "zone=" << tc.zone;
    proj_errno_reset(pj.get());
  }
}

TEST(ImollTest, FactorsLinearUnitsBatchAndSpecialValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. proj_factors confirms equal-area property (areal_scale == 1.0) in both
  // northern and southern zones.
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=imoll +R=6371000"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_FACTORS north_fac = proj_factors(
      pj_sphere.get(), proj_coord(DegToRad(-100.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_GT(north_fac.meridional_scale, 0.0);
  EXPECT_GT(north_fac.parallel_scale, 0.0);
  EXPECT_NEAR(1.0, north_fac.areal_scale, 1e-5);

  const PJ_FACTORS south_fac = proj_factors(
      pj_sphere.get(), proj_coord(DegToRad(20.0), DegToRad(-45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(1.0, south_fac.areal_scale, 1e-5);

  // 2. False easting/northing, +lon_0, +units=km, and 4D (z, t) pass-through.
  ScopedPj pj_km(proj_create(
      ctx.get(),
      "+proj=imoll +R=6371000 +lon_0=-10 +x_0=100000 +y_0=200000 +units=km"));
  ASSERT_NE(nullptr, pj_km.get());
  const PJ_COORD pt_4d =
      proj_coord(DegToRad(20.0), DegToRad(25.0), -42.5, 2026.5);
  const PJ_COORD fwd_km = proj_trans(pj_km.get(), PJ_FWD, pt_4d);
  EXPECT_EQ(0, proj_errno(pj_km.get()));
  EXPECT_NEAR(-0.0425, fwd_km.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.5, fwd_km.xyzt.t, 1e-12);
  const PJ_COORD inv_km = proj_trans(pj_km.get(), PJ_INV, fwd_km);
  EXPECT_EQ(0, proj_errno(pj_km.get()));
  EXPECT_NEAR(pt_4d.lpzt.lam, inv_km.lpzt.lam, 1e-9);
  EXPECT_NEAR(pt_4d.lpzt.phi, inv_km.lpzt.phi, 1e-9);
  EXPECT_NEAR(-42.5, inv_km.lpzt.z, 1e-12);
  EXPECT_NEAR(2026.5, inv_km.lpzt.t, 1e-12);

  // 3. Batch transform via proj_trans_generic.
  std::array<double, 2> x = {DegToRad(-100.0), DegToRad(140.0)};
  std::array<double, 2> y = {DegToRad(40.0), DegToRad(-40.0)};
  EXPECT_EQ(
      2u, proj_trans_generic(pj_sphere.get(), PJ_FWD, x.data(), sizeof(double),
                             x.size(), y.data(), sizeof(double), y.size(),
                             nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_EQ(
      2u, proj_trans_generic(pj_sphere.get(), PJ_INV, x.data(), sizeof(double),
                             x.size(), y.data(), sizeof(double), y.size(),
                             nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(DegToRad(-100.0), x[0], 1e-9);
  EXPECT_NEAR(DegToRad(40.0), y[0], 1e-9);
  EXPECT_NEAR(DegToRad(140.0), x[1], 1e-9);
  EXPECT_NEAR(DegToRad(-40.0), y[1], 1e-9);

  // 4. proj_trans_array valid batch (0 == success) and mixed out-of-domain.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(30.0), DegToRad(50.0), 0.0, 0.0),
      proj_coord(DegToRad(-60.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj_sphere.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj_sphere.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(30.0), valid_batch[0].lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(50.0), valid_batch[0].lp.phi, 1e-9);
  EXPECT_NEAR(DegToRad(-60.0), valid_batch[1].lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(-50.0), valid_batch[1].lp.phi, 1e-9);

  ScopedLogSuppressor suppress(ctx.get());
  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(0.0, 2.0e7, 0.0, 0.0),
  };
  EXPECT_EQ(0, proj_trans_array(pj_sphere.get(), PJ_INV, mixed_batch.size(),
                                mixed_batch.data()));
  EXPECT_TRUE(std::isfinite(mixed_batch[0].lp.lam));
  EXPECT_NEAR(0.0, mixed_batch[0].lp.phi, 1e-12);
  EXPECT_TRUE(std::isinf(mixed_batch[1].lp.lam));
  proj_errno_reset(pj_sphere.get());

  // 5. NaN and HUGE_VAL inputs.
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_res =
      proj_trans(pj_sphere.get(), PJ_FWD, proj_coord(kNan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_res.xy.x));
  EXPECT_TRUE(std::isnan(nan_res.xy.y));

  const PJ_COORD inf_res =
      proj_trans(pj_sphere.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_res.xy.x));
}

// TODO(schwehr): Unreachable lines in proj/src/projections/imoll.cpp without
// fault injection or upstream code changes:
// - OOM null checks on calloc/pj_moll in setup_zone() and PJ_PROJECTION(imoll)
//   (lines 200-203, 244-245, 255).
// - pj_imoll_destructor() P->opaque == nullptr fallback (lines 164-165).

}  // namespace
}  // namespace proj
