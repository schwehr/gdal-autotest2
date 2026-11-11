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
// Unit tests for the Interrupted Goode Homolosine (igh) projection
// (proj/src/projections/igh.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/igh.cpp

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

// Transition latitude between Sinusoidal and Mollweide: 40deg 44' 11.8".
constexpr double kPhiBoundaryDeg = 40.0 + 44.0 / 60.0 + 11.8 / 3600.0;
constexpr double kPhiBoundaryRad = kPhiBoundaryDeg * (kPi / 180.0);

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(IghTest, MetadataForcedSphericalAndNullDestructorGuard) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_default(proj_create(ctx.get(), "+proj=igh"));
  ASSERT_NE(nullptr, pj_default.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj_default.get());
  EXPECT_STREQ("igh", info.id);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, proj_angular_input(pj_default.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj_default.get(), PJ_FWD));
  // igh is strictly spherical and forces P->es = 0 during setup.
  EXPECT_EQ(0.0, pj_default->es);
  EXPECT_EQ(nullptr, pj_default->destructor(nullptr, 0));

  // Ellipsoid parameters are forced spherical (es == 0) while preserving `a`.
  ScopedPj pj_wgs84(proj_create(ctx.get(), "+proj=igh +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_wgs84.get());
  EXPECT_EQ(0.0, pj_wgs84->es);

  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=igh +R=6378137"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_COORD input = proj_coord(DegToRad(-100.0), DegToRad(55.0), 0.0, 0.0);
  const PJ_COORD out_wgs84 = proj_trans(pj_wgs84.get(), PJ_FWD, input);
  const PJ_COORD out_sphere = proj_trans(pj_sphere.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj_wgs84.get()));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(out_sphere.xy.x, out_wgs84.xy.x, 1e-6);
  EXPECT_NEAR(out_sphere.xy.y, out_wgs84.xy.y, 1e-6);
}

TEST(IghTest, ForwardAndInverseRoundTripsAllTwelveZones) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=igh +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct ZoneCase {
    double lon_deg;
    double lat_deg;
    double tol_rad;
  };
  constexpr ZoneCase kCases[] = {
      // Zone 1: phi >= boundary, lam <= -40 deg (Mollweide, lon_0=-100).
      {.lon_deg = -100.0, .lat_deg = 55.0, .tol_rad = 1e-9},
      {.lon_deg = -40.0, .lat_deg = 55.0, .tol_rad = 1e-9},
      {.lon_deg = -175.0, .lat_deg = 75.0, .tol_rad = 1e-9},
      // Zone 2: phi >= boundary, lam > -40 deg (Mollweide, lon_0=+30).
      {.lon_deg = 30.0, .lat_deg = 55.0, .tol_rad = 1e-9},
      {.lon_deg = 175.0, .lat_deg = 75.0, .tol_rad = 1e-9},
      // Zone 3: 0 <= phi < boundary, lam <= -40 deg (Sinusoidal, lon_0=-100).
      {.lon_deg = -100.0, .lat_deg = 20.0, .tol_rad = 1e-12},
      {.lon_deg = -40.0, .lat_deg = 0.0, .tol_rad = 1e-12},
      // Zone 4: 0 <= phi < boundary, lam > -40 deg (Sinusoidal, lon_0=+30).
      {.lon_deg = 30.0, .lat_deg = 20.0, .tol_rad = 1e-12},
      {.lon_deg = 0.0, .lat_deg = 0.0, .tol_rad = 1e-12},
      // Zone 5: -boundary <= phi < 0, lam <= -100 deg (Sinusoidal, lon_0=-160).
      {.lon_deg = -160.0, .lat_deg = -20.0, .tol_rad = 1e-12},
      {.lon_deg = -100.0, .lat_deg = -20.0, .tol_rad = 1e-12},
      // Zone 6: -boundary <= phi < 0, -100 < lam <= -20 deg (Sinu, lon_0=-60).
      {.lon_deg = -60.0, .lat_deg = -20.0, .tol_rad = 1e-12},
      {.lon_deg = -20.0, .lat_deg = -35.0, .tol_rad = 1e-12},
      // Zone 7: -boundary <= phi < 0, -20 < lam <= 80 deg (Sinu, lon_0=+20).
      {.lon_deg = 20.0, .lat_deg = -20.0, .tol_rad = 1e-12},
      {.lon_deg = 80.0, .lat_deg = -20.0, .tol_rad = 1e-12},
      // Zone 8: -boundary <= phi < 0, lam > 80 deg (Sinusoidal, lon_0=+140).
      {.lon_deg = 140.0, .lat_deg = -20.0, .tol_rad = 1e-12},
      {.lon_deg = 175.0, .lat_deg = -35.0, .tol_rad = 1e-12},
      // Zone 9: phi < -boundary, lam <= -100 deg (Mollweide, lon_0=-160).
      {.lon_deg = -160.0, .lat_deg = -55.0, .tol_rad = 1e-9},
      {.lon_deg = -100.0, .lat_deg = -55.0, .tol_rad = 1e-9},
      {.lon_deg = -175.0, .lat_deg = -85.0, .tol_rad = 1e-8},
      // Zone 10: phi < -boundary, -100 < lam <= -20 deg (Moll, lon_0=-60).
      {.lon_deg = -60.0, .lat_deg = -55.0, .tol_rad = 1e-9},
      {.lon_deg = -20.0, .lat_deg = -85.0, .tol_rad = 1e-8},
      // Zone 11: phi < -boundary, -20 < lam <= 80 deg (Mollweide, lon_0=+20).
      {.lon_deg = 20.0, .lat_deg = -55.0, .tol_rad = 1e-9},
      {.lon_deg = 80.0, .lat_deg = -85.0, .tol_rad = 1e-8},
      // Zone 12: phi < -boundary, lam > 80 deg (Mollweide, lon_0=+140).
      {.lon_deg = 140.0, .lat_deg = -55.0, .tol_rad = 1e-9},
      {.lon_deg = 175.0, .lat_deg = -85.0, .tol_rad = 1e-8},
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

  // Exact North Pole along zone 1 and 2 central meridians lies at y = +y90 and
  // inverse-maps back to phi = +pi/2.
  constexpr double kNorthCentralMeridiansDeg[] = {-100.0, 30.0};
  for (const double lon0_deg : kNorthCentralMeridiansDeg) {
    const PJ_COORD fwd_pole = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(lon0_deg), kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv_pole = proj_trans(pj.get(), PJ_INV, fwd_pole);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kHalfPi, inv_pole.lp.phi, 1e-9);
  }
}

TEST(IghTest, BoundaryContinuityAndVerticalRangeErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  // Unit radius R=1 exposes internal projected coordinates directly.
  ScopedPj pj(proj_create(ctx.get(), "+proj=igh +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Verify dy0 keeps y continuous across +/-igh_phi_boundary along central
  // meridians (Mollweide matches Sinusoidal at 40deg 44' 11.8").
  const PJ_COORD north_moll = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(DegToRad(-100.0), kPhiBoundaryRad + 1e-9, 0.0, 0.0));
  const PJ_COORD north_sinu = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(DegToRad(-100.0), kPhiBoundaryRad - 1e-9, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kPhiBoundaryRad, north_moll.xy.y, 1e-6);
  EXPECT_NEAR(north_sinu.xy.y, north_moll.xy.y, 1e-6);

  const PJ_COORD south_moll =
      proj_trans(pj.get(), PJ_FWD,
                 proj_coord(DegToRad(20.0), -kPhiBoundaryRad - 1e-9, 0.0, 0.0));
  const PJ_COORD south_sinu =
      proj_trans(pj.get(), PJ_FWD,
                 proj_coord(DegToRad(20.0), -kPhiBoundaryRad + 1e-9, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kPhiBoundaryRad, south_moll.xy.y, 1e-6);
  EXPECT_NEAR(south_sinu.xy.y, south_moll.xy.y, 1e-6);

  // 2. Inverse y > y90 + EPSLN and y < -y90 + EPSLN both return HUGE_VAL.
  const PJ_COORD above_north_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(above_north_pole.lp.lam));
  EXPECT_TRUE(std::isinf(above_north_pole.lp.phi));
  proj_errno_reset(pj.get());

  const PJ_COORD below_south_pole =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, -2.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(below_south_pole.lp.lam));
  EXPECT_TRUE(std::isinf(below_south_pole.lp.phi));
  proj_errno_reset(pj.get());

  // 3. Exact South Pole produces y = -y90, which triggers the upstream
  // `xy.y < -y90 + EPSLN` guard on inverse and returns HUGE_VAL.
  const PJ_COORD fwd_south_pole = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(20.0), -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_COORD inv_south_pole = proj_trans(pj.get(), PJ_INV, fwd_south_pole);
  EXPECT_TRUE(std::isinf(inv_south_pole.lp.lam));
  EXPECT_TRUE(std::isinf(inv_south_pole.lp.phi));
}

TEST(IghTest, InverseInterruptionGapsRejectAllTwelveZones) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=igh +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // Each (x, y) falls inside the corresponding zone's (x, y) bounding box on
  // the unit sphere, but maps to a longitude outside that zone's valid range
  // (the interruption wedges between lobes).
  struct GapCase {
    int zone;
    double x;
    double y;
  };
  constexpr GapCase kGapCases[] = {
      {.zone = 1, .x = DegToRad(-42.0), .y = 0.75},
      {.zone = 2, .x = DegToRad(-38.0), .y = 0.75},
      {.zone = 3, .x = DegToRad(-42.0), .y = DegToRad(20.0)},
      {.zone = 4, .x = DegToRad(-38.0), .y = DegToRad(20.0)},
      {.zone = 5, .x = DegToRad(-102.0), .y = DegToRad(-20.0)},
      {.zone = 6, .x = DegToRad(-99.0), .y = DegToRad(-20.0)},
      {.zone = 7, .x = DegToRad(-19.0), .y = DegToRad(-20.0)},
      {.zone = 8, .x = DegToRad(81.0), .y = DegToRad(-20.0)},
      {.zone = 9, .x = DegToRad(-102.0), .y = -1.0},
      {.zone = 10, .x = DegToRad(-99.0), .y = -1.0},
      {.zone = 11, .x = DegToRad(-19.0), .y = -1.0},
      {.zone = 12, .x = DegToRad(81.0), .y = -1.0},
  };

  for (const auto& tc : kGapCases) {
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(tc.x, tc.y, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(inv.lp.lam)) << "zone=" << tc.zone;
    EXPECT_TRUE(std::isinf(inv.lp.phi)) << "zone=" << tc.zone;
    proj_errno_reset(pj.get());
  }
}

TEST(IghTest, InverseZone1AndZone2ExtensionLobesAcceptValidPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj igh(proj_create(ctx.get(), "+proj=igh +R=1"));
  ScopedPj moll_z1(proj_create(ctx.get(), "+proj=moll +R=1 +lon_0=-100"));
  ScopedPj moll_z2(proj_create(ctx.get(), "+proj=moll +R=1 +lon_0=30"));
  ASSERT_NE(nullptr, igh.get());
  ASSERT_NE(nullptr, moll_z1.get());
  ASSERT_NE(nullptr, moll_z2.get());

  // 1. Zone 1 extension lobe accepts lon in [-40, -10] deg when lat >= 60 deg
  // (Greenland). Construct Zone 1 Mollweide coordinates for (-20 deg, 80 deg),
  // whose x <= -40 deg routes inverse into case 1.
  const PJ_COORD y_ref = proj_trans(
      igh.get(), PJ_FWD, proj_coord(DegToRad(30.0), DegToRad(80.0), 0.0, 0.0));
  ASSERT_EQ(0, proj_errno(igh.get()));

  const PJ_COORD moll1_offset =
      proj_trans(moll_z1.get(), PJ_FWD,
                 proj_coord(DegToRad(-20.0), DegToRad(80.0), 0.0, 0.0));
  ASSERT_EQ(0, proj_errno(moll_z1.get()));
  const PJ_COORD z1_lobe_xy =
      proj_coord(DegToRad(-100.0) + moll1_offset.xy.x, y_ref.xy.y, 0.0, 0.0);
  ASSERT_LE(z1_lobe_xy.xy.x, DegToRad(-40.0));
  ASSERT_GE(z1_lobe_xy.xy.y, kPhiBoundaryRad);

  const PJ_COORD z1_inv = proj_trans(igh.get(), PJ_INV, z1_lobe_xy);
  EXPECT_EQ(0, proj_errno(igh.get()));
  EXPECT_NEAR(DegToRad(-20.0), z1_inv.lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(80.0), z1_inv.lp.phi, 1e-9);

  // 2. Zone 2 extension lobe accepts lon in [-50, -40] deg when lat >= 60 deg.
  // Construct Zone 2 Mollweide coordinates for (-45 deg, 80 deg), whose
  // x > -40 deg routes inverse into case 2.
  const PJ_COORD moll2_offset =
      proj_trans(moll_z2.get(), PJ_FWD,
                 proj_coord(DegToRad(-45.0), DegToRad(80.0), 0.0, 0.0));
  ASSERT_EQ(0, proj_errno(moll_z2.get()));
  const PJ_COORD z2_lobe_xy =
      proj_coord(DegToRad(30.0) + moll2_offset.xy.x, y_ref.xy.y, 0.0, 0.0);
  ASSERT_GT(z2_lobe_xy.xy.x, DegToRad(-40.0));
  ASSERT_GE(z2_lobe_xy.xy.y, kPhiBoundaryRad);

  const PJ_COORD z2_inv = proj_trans(igh.get(), PJ_INV, z2_lobe_xy);
  EXPECT_EQ(0, proj_errno(igh.get()));
  EXPECT_NEAR(DegToRad(-45.0), z2_inv.lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(80.0), z2_inv.lp.phi, 1e-9);
}

TEST(IghTest, FactorsLinearUnitsBatchAndSpecialValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. proj_factors in Sinusoidal (zone 4) and Mollweide (zone 2) regions
  // preserves equal-area property (areal_scale == 1.0).
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=igh +R=6371000"));
  ASSERT_NE(nullptr, pj_sphere.get());

  const PJ_FACTORS sinu_fac = proj_factors(
      pj_sphere.get(), proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_NEAR(1.0, sinu_fac.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, sinu_fac.areal_scale, 1e-6);

  const PJ_FACTORS moll_fac = proj_factors(
      pj_sphere.get(), proj_coord(DegToRad(10.0), DegToRad(60.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_GT(moll_fac.meridional_scale, 0.0);
  EXPECT_GT(moll_fac.parallel_scale, 0.0);
  EXPECT_NEAR(1.0, moll_fac.areal_scale, 1e-5);

  // 2. False easting/northing, +lon_0, +units=km, and 4D (z, t) pass-through.
  ScopedPj pj_km(proj_create(
      ctx.get(),
      "+proj=igh +R=6371000 +lon_0=15 +x_0=500000 +y_0=1000000 +units=km"));
  ASSERT_NE(nullptr, pj_km.get());
  const PJ_COORD pt_4d =
      proj_coord(DegToRad(5.0), DegToRad(10.0), 123.5, 2026.25);
  const PJ_COORD fwd_km = proj_trans(pj_km.get(), PJ_FWD, pt_4d);
  EXPECT_EQ(0, proj_errno(pj_km.get()));
  EXPECT_NEAR(0.1235, fwd_km.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.25, fwd_km.xyzt.t, 1e-12);
  const PJ_COORD inv_km = proj_trans(pj_km.get(), PJ_INV, fwd_km);
  EXPECT_EQ(0, proj_errno(pj_km.get()));
  EXPECT_NEAR(pt_4d.lpzt.lam, inv_km.lpzt.lam, 1e-12);
  EXPECT_NEAR(pt_4d.lpzt.phi, inv_km.lpzt.phi, 1e-12);
  EXPECT_NEAR(123.5, inv_km.lpzt.z, 1e-12);
  EXPECT_NEAR(2026.25, inv_km.lpzt.t, 1e-12);

  // 3. Batch transform with proj_trans_generic (forward + inverse).
  std::array<double, 2> x = {DegToRad(-100.0), DegToRad(140.0)};
  std::array<double, 2> y = {DegToRad(50.0), DegToRad(-30.0)};
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
  EXPECT_NEAR(DegToRad(50.0), y[0], 1e-9);
  EXPECT_NEAR(DegToRad(140.0), x[1], 1e-12);
  EXPECT_NEAR(DegToRad(-30.0), y[1], 1e-12);

  // 4. proj_trans_array on valid batch (0 == success) and mixed batch.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(-100.0), DegToRad(55.0), 0.0, 0.0),
      proj_coord(DegToRad(20.0), DegToRad(-20.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj_sphere.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj_sphere.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(-100.0), valid_batch[0].lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(55.0), valid_batch[0].lp.phi, 1e-9);
  EXPECT_NEAR(DegToRad(20.0), valid_batch[1].lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-20.0), valid_batch[1].lp.phi, 1e-12);

  ScopedLogSuppressor suppress(ctx.get());
  std::array<PJ_COORD, 2> batch = {
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(0.0, 2.0e7, 0.0, 0.0),
  };
  EXPECT_EQ(
      0, proj_trans_array(pj_sphere.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(0.0, batch[0].lp.lam, 1e-12);
  EXPECT_NEAR(0.0, batch[0].lp.phi, 1e-12);
  EXPECT_TRUE(std::isinf(batch[1].lp.lam));
  proj_errno_reset(pj_sphere.get());

  // 5. NaN and HUGE_VAL edge inputs.
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_res =
      proj_trans(pj_sphere.get(), PJ_FWD, proj_coord(kNan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_res.xy.x));
  EXPECT_TRUE(std::isnan(nan_res.xy.y));

  const PJ_COORD inf_res =
      proj_trans(pj_sphere.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_res.xy.x));
}

// TODO(schwehr): Unreachable lines in proj/src/projections/igh.cpp without
// fault injection or upstream code changes:
// - OOM null checks on calloc/pj_sinu/pj_moll in pj_igh_setup_zone() and
//   PJ_PROJECTION(igh) (lines 238-241, 256-257, 267, 272, 289).
// - pj_igh_data_destructor() P->opaque == nullptr fallback (lines 202-203).
// - Extension-lobe longitude range in case 2 (lp.lam in [-180, -160] deg with
//   lam0 = +30 deg), which exceeds Mollweide's |lam - lam0| < pi inverse
//   domain.

}  // namespace
}  // namespace proj
