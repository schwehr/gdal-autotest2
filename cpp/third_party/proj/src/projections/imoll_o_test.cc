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
// Unit tests for the Interrupted Mollweide Oceanic View projection
// (proj/src/projections/imoll_o.cpp), covering all 6 sub-projection zones,
// equator seam matching, inverse gore/limb domain checks, batch transforms,
// and equal-area distortion factors.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/imoll_o.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numbers>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

double WrapAngleDiff(double a, double b) {
  double diff = std::fmod(a - b + kPi, 2.0 * kPi);
  if (diff < 0.0) {
    diff += 2.0 * kPi;
  }
  return diff - kPi;
}

struct LonLatCase {
  double lon_deg;
  double lat_deg;
};

// Covers interior points and seam boundaries across all 6 sub-projection zones:
//   North (phi >= 0): zone 1 (-180..-90), zone 2 (-90..60), zone 3 (60..180)
//   South (phi < 0):  zone 4 (-180..-60), zone 5 (-60..90), zone 6 (90..180)
constexpr LonLatCase kZoneAndSeamCases[] = {
    // Zone 1 interior and boundaries (-180, -90).
    {.lon_deg = -180.0, .lat_deg = 30.0},
    {.lon_deg = -170.0, .lat_deg = 45.0},
    {.lon_deg = -140.0, .lat_deg = 0.0},
    {.lon_deg = -140.0, .lat_deg = 75.0},
    {.lon_deg = -140.0, .lat_deg = 89.9},
    {.lon_deg = -95.0, .lat_deg = 15.0},
    {.lon_deg = -90.0, .lat_deg = 45.0},
    // Zone 2 interior and equatorial seam.
    {.lon_deg = -85.0, .lat_deg = 20.0},
    {.lon_deg = -10.0, .lat_deg = 0.0},
    {.lon_deg = -10.0, .lat_deg = 60.0},
    {.lon_deg = -10.0, .lat_deg = 89.9},
    {.lon_deg = 0.0, .lat_deg = 35.0},
    {.lon_deg = 55.0, .lat_deg = 10.0},
    // Zone 3 interior and boundaries (60, 180).
    {.lon_deg = 60.0, .lat_deg = 45.0},
    {.lon_deg = 65.0, .lat_deg = 25.0},
    {.lon_deg = 130.0, .lat_deg = 0.0},
    {.lon_deg = 130.0, .lat_deg = 70.0},
    {.lon_deg = 130.0, .lat_deg = 89.9},
    {.lon_deg = 175.0, .lat_deg = 40.0},
    {.lon_deg = 180.0, .lat_deg = 30.0},
    // Zone 4 interior and boundaries (-180, -60).
    {.lon_deg = -180.0, .lat_deg = -30.0},
    {.lon_deg = -175.0, .lat_deg = -35.0},
    {.lon_deg = -110.0, .lat_deg = -1e-4},
    {.lon_deg = -110.0, .lat_deg = -70.0},
    {.lon_deg = -110.0, .lat_deg = -89.9},
    {.lon_deg = -65.0, .lat_deg = -20.0},
    {.lon_deg = -60.0, .lat_deg = -45.0},
    // Zone 5 interior.
    {.lon_deg = -55.0, .lat_deg = -25.0},
    {.lon_deg = 20.0, .lat_deg = -1e-4},
    {.lon_deg = 20.0, .lat_deg = -65.0},
    {.lon_deg = 20.0, .lat_deg = -89.9},
    {.lon_deg = 85.0, .lat_deg = -30.0},
    // Zone 6 interior and boundaries (90, 180).
    {.lon_deg = 90.0, .lat_deg = -45.0},
    {.lon_deg = 95.0, .lat_deg = -25.0},
    {.lon_deg = 150.0, .lat_deg = -1e-4},
    {.lon_deg = 150.0, .lat_deg = -70.0},
    {.lon_deg = 150.0, .lat_deg = -89.9},
    {.lon_deg = 175.0, .lat_deg = -45.0},
    {.lon_deg = 180.0, .lat_deg = -30.0},
};

void AssertRoundTripAll(PJ* pj, const LonLatCase* cases, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const double lon_rad = DegToRad(cases[i].lon_deg);
    const double lat_rad = DegToRad(cases[i].lat_deg);
    const double tol_rad = std::abs(cases[i].lat_deg) > 85.0 ? 1e-7 : 1e-10;

    // 1. Forward: geodetic (lon, lat) in radians -> projected (x, y) in meters.
    const PJ_COORD fwd =
        proj_trans(pj, PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    ASSERT_EQ(0, proj_errno(pj)) << "Forward failed at (" << cases[i].lon_deg
                                 << ", " << cases[i].lat_deg << ")";
    ASSERT_FALSE(std::isinf(fwd.xy.x) || std::isnan(fwd.xy.x));
    ASSERT_FALSE(std::isinf(fwd.xy.y) || std::isnan(fwd.xy.y));

    // 2. Inverse: projected (x, y) in meters -> geodetic (lon, lat) in radians.
    const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
    ASSERT_EQ(0, proj_errno(pj)) << "Inverse failed at (" << cases[i].lon_deg
                                 << ", " << cases[i].lat_deg << ")";
    EXPECT_NEAR(lat_rad, inv.lp.phi, tol_rad)
        << "Latitude mismatch at (" << cases[i].lon_deg << ", "
        << cases[i].lat_deg << ")";
    EXPECT_NEAR(0.0, WrapAngleDiff(inv.lp.lam, lon_rad), tol_rad)
        << "Longitude mismatch at (" << cases[i].lon_deg << ", "
        << cases[i].lat_deg << ")";
  }
}

TEST(ImollOTest, MetadataAndInitializationOptions) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_unit(proj_create(ctx.get(), "+proj=imoll_o +R=1"));
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=imoll_o +R=6371000"));
  ScopedPj pj_ell(proj_create(ctx.get(), "+proj=imoll_o +ellps=WGS84"));
  ScopedPj pj_wgs84_sphere(proj_create(ctx.get(), "+proj=imoll_o +R=6378137"));
  ASSERT_NE(nullptr, pj_unit.get());
  ASSERT_NE(nullptr, pj_sphere.get());
  ASSERT_NE(nullptr, pj_ell.get());
  ASSERT_NE(nullptr, pj_wgs84_sphere.get());

  // 1. Metadata and angular/linear unit predicates.
  const PJ_PROJ_INFO info = proj_pj_info(pj_unit.get());
  EXPECT_STREQ("imoll_o", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_unit.get()));
  EXPECT_STREQ("imoll_o", pj_unit->short_name);
  EXPECT_THAT(std::string(pj_unit->descr),
              HasSubstr("Interrupted Mollweide Oceanic View"));

  EXPECT_EQ(1, proj_angular_input(pj_unit.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj_unit.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj_unit.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj_unit.get(), PJ_INV));

  const char* proj_str =
      proj_as_proj_string(ctx.get(), pj_unit.get(), PJ_PROJ_5, nullptr);
  ASSERT_NE(nullptr, proj_str);
  EXPECT_THAT(std::string(proj_str), HasSubstr("+proj=imoll_o"));

  // 2. Constructor resets P->es = 0.0 (purely spherical using semimajor a).
  EXPECT_DOUBLE_EQ(0.0, pj_ell->es);
  const PJ_COORD sample = proj_coord(DegToRad(35.0), DegToRad(-25.0), 0.0, 0.0);
  const PJ_COORD out_ell = proj_trans(pj_ell.get(), PJ_FWD, sample);
  const PJ_COORD out_sph = proj_trans(pj_wgs84_sphere.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(pj_ell.get()));
  EXPECT_EQ(0, proj_errno(pj_wgs84_sphere.get()));
  EXPECT_NEAR(out_sph.xy.x, out_ell.xy.x, 1e-9);
  EXPECT_NEAR(out_sph.xy.y, out_ell.xy.y, 1e-9);

  // 3. Mutable char[] buffers required by proj_create_argv.
  char arg0[] = "proj=imoll_o";
  char arg1[] = "R=6371000";
  char arg2[] = "units=km";
  char* argv[] = {arg0, arg1, arg2};
  ScopedPj pj_argv(proj_create_argv(ctx.get(), 3, argv));
  ASSERT_NE(nullptr, pj_argv.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_COORD out_m = proj_trans(pj_sphere.get(), PJ_FWD, sample);
  const PJ_COORD out_km = proj_trans(pj_argv.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(pj_sphere.get()));
  EXPECT_EQ(0, proj_errno(pj_argv.get()));
  EXPECT_NEAR(out_m.xy.x / 1000.0, out_km.xy.x, 1e-9);
  EXPECT_NEAR(out_m.xy.y / 1000.0, out_km.xy.y, 1e-9);
}

TEST(ImollOTest, KnownClosedFormEquatorAndMollweidePoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_unit(proj_create(ctx.get(), "+proj=imoll_o +R=1"));
  ScopedPj pj_scaled(proj_create(ctx.get(), "+proj=imoll_o +R=6371000"));
  ASSERT_NE(nullptr, pj_unit.get());
  ASSERT_NE(nullptr, pj_scaled.get());

  // 1. Along the equator (phi == +0.0 and -0.0 use north zones; small negative
  // phi uses south zones), zone offsets keep x strictly continuous:
  // x(lam, 0) = -d140 + (2 * sqrt(2) / pi) * (lam + d140).
  const double d140_rad = DegToRad(140.0);
  const double cx = 2.0 * std::numbers::sqrt2 / kPi;
  constexpr double kEquatorLonsDeg[] = {-175.0, -140.0, -90.0, -60.0, -10.0,
                                        0.0,    60.0,   90.0,  130.0, 175.0};
  constexpr std::array<double, 2> kEquatorPhis = {0.0, -0.0};
  for (double lon_deg : kEquatorLonsDeg) {
    const double lam = DegToRad(lon_deg);
    const double expected_x = -d140_rad + cx * (lam + d140_rad);

    for (double phi : kEquatorPhis) {
      const PJ_COORD out_eq =
          proj_trans(pj_unit.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj_unit.get()));
      EXPECT_NEAR(expected_x, out_eq.xy.x, 1e-12) << lon_deg;
      EXPECT_NEAR(0.0, out_eq.xy.y, 1e-12) << lon_deg;
    }

    const PJ_COORD out_south =
        proj_trans(pj_unit.get(), PJ_FWD, proj_coord(lam, -1e-7, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    EXPECT_NEAR(expected_x, out_south.xy.x, 1e-8) << lon_deg;
    EXPECT_LT(out_south.xy.y, 0.0);
  }

  // 2. Closed-form Mollweide parallel: when
  // 2*theta + sin(2*theta) = pi*sin(phi) has 2*theta = pi/2 (i.e. sin(phi) =
  // 0.5 + 1/pi), y = sqrt(2)*sin(pi/4) = 1. In zone 1 (lam0 = -140 deg, x0 =
  // -d140):
  //   lam = -140 deg -> x = -d140, y = 1.0
  //   lam = -110 deg (dlon = pi/6) ->
  //       x = -d140 + (2*sqrt(2)/pi)*(pi/6)*(sqrt(2)/2) = -d140 + 1/3, y = 1.0.
  const double phi_y1 = std::asin(0.5 + 1.0 / kPi);
  const PJ_COORD z1_center = proj_trans(
      pj_unit.get(), PJ_FWD, proj_coord(-d140_rad, phi_y1, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_unit.get()));
  EXPECT_NEAR(-d140_rad, z1_center.xy.x, 1e-10);
  EXPECT_NEAR(1.0, z1_center.xy.y, 1e-10);

  const PJ_COORD z1_east = proj_trans(
      pj_unit.get(), PJ_FWD, proj_coord(DegToRad(-110.0), phi_y1, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_unit.get()));
  EXPECT_NEAR(-d140_rad + (1.0 / 3.0), z1_east.xy.x, 1e-10);
  EXPECT_NEAR(1.0, z1_east.xy.y, 1e-10);

  // Classic unit scaling multiplies unit-sphere coordinates by R.
  constexpr double kRadiusM = 6371000.0;
  const PJ_COORD z1_scaled = proj_trans(
      pj_scaled.get(), PJ_FWD, proj_coord(DegToRad(-110.0), phi_y1, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_scaled.get()));
  EXPECT_NEAR(z1_east.xy.x * kRadiusM, z1_scaled.xy.x, 1e-4);
  EXPECT_NEAR(z1_east.xy.y * kRadiusM, z1_scaled.xy.y, 1e-4);
}

TEST(ImollOTest, AllSixZonesSeamsAndPoleBoundaryRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj_unit(proj_create(ctx.get(), "+proj=imoll_o +R=1"));
  ScopedPj pj_sphere(proj_create(ctx.get(), "+proj=imoll_o +R=6371000"));
  ScopedPj pj_offset(proj_create(
      ctx.get(),
      "+proj=imoll_o +R=6371000 +lon_0=25 +x_0=500000 +y_0=-100000"));
  ASSERT_NE(nullptr, pj_unit.get());
  ASSERT_NE(nullptr, pj_sphere.get());
  ASSERT_NE(nullptr, pj_offset.get());

  // 1. Round-trip all 6 zones, boundaries, and near-pole latitudes.
  AssertRoundTripAll(pj_sphere.get(), kZoneAndSeamCases,
                     std::size(kZoneAndSeamCases));
  AssertRoundTripAll(pj_offset.get(), kZoneAndSeamCases,
                     std::size(kZoneAndSeamCases));

  // 2. Exact North Pole along each northern zone central meridian (-140, -10,
  // 130 deg) projects to y = sqrt(2) and inverts back to +pi/2.
  constexpr std::array<double, 3> kNorthPoleLonsDeg = {-140.0, -10.0, 130.0};
  for (double lon_deg : kNorthPoleLonsDeg) {
    const PJ_COORD np_fwd =
        proj_trans(pj_unit.get(), PJ_FWD,
                   proj_coord(DegToRad(lon_deg), kHalfPi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    EXPECT_NEAR(std::numbers::sqrt2, np_fwd.xy.y, 1e-10);

    const PJ_COORD np_inv = proj_trans(pj_unit.get(), PJ_INV, np_fwd);
    EXPECT_EQ(0, proj_errno(pj_unit.get()));
    EXPECT_NEAR(kHalfPi, np_inv.lp.phi, 1e-6);
  }

  // 3. Exact South Pole forwards to y = -sqrt(2). Because imoll_o_s_inverse()
  // checks `xy.y < -y90 + EPSLN` (imoll_o.cpp:105), y = -sqrt(2) falls below
  // -y90 + 1e-10 and returns HUGE_VAL with OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD sp_fwd = proj_trans(
      pj_unit.get(), PJ_FWD, proj_coord(DegToRad(-110.0), -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_unit.get()));
  EXPECT_NEAR(-std::numbers::sqrt2, sp_fwd.xy.y, 1e-10);

  const PJ_COORD sp_inv = proj_trans(pj_unit.get(), PJ_INV, sp_fwd);
  EXPECT_EQ(HUGE_VAL, sp_inv.lp.lam);
  EXPECT_EQ(HUGE_VAL, sp_inv.lp.phi);
  proj_errno_reset(pj_unit.get());
}

TEST(ImollOTest, InterruptedGoresAndOutOfDomainInverseRejectWithDomainError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=imoll_o +R=1"));
  ASSERT_NE(nullptr, pj.get());

  auto expect_inv_outside = [&](double x, double y, const char* label) {
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(x, y, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inv.lp.lam) << label;
    EXPECT_EQ(HUGE_VAL, inv.lp.phi) << label;
    proj_errno_reset(pj.get());
  };

  // 1. Out-of-y bounds (north of +y90 + EPSLN and south of -y90 + EPSLN).
  expect_inv_outside(0.0, std::numbers::sqrt2 + 1e-4, "north_of_pole");
  expect_inv_outside(0.0, -std::numbers::sqrt2 - 1e-4, "south_of_pole");

  // 2. Outside the outer +/-180 deg Mollweide limbs (moll_s_inverse returns
  // HUGE_VAL, failing switch zone range checks in cases 1, 3, 4, 6).
  expect_inv_outside(-10.0, 0.5, "west_of_zone_1");
  expect_inv_outside(10.0, 0.5, "east_of_zone_3");
  expect_inv_outside(-10.0, -0.5, "west_of_zone_4");
  expect_inv_outside(10.0, -0.5, "east_of_zone_6");

  // 3. Points inside each half-gore between adjacent interrupted lobes at +/-60
  // deg latitude (moll_s_inverse returns a finite longitude outside that
  // zone's permitted [lam_min, lam_max] range, hitting `ok = false` in all 6
  // switch cases).
  struct SeamGoreProbe {
    double seam_lon_deg;
    double gore_lat_deg;
    double eq_probe_lat_deg;
    const char* left_label;
    const char* right_label;
  };
  constexpr SeamGoreProbe kSeams[] = {
      {.seam_lon_deg = -90.0,
       .gore_lat_deg = 60.0,
       .eq_probe_lat_deg = 1e-5,
       .left_label = "gore_zone_1_case1",
       .right_label = "gore_zone_2_case2"},
      {.seam_lon_deg = 60.0,
       .gore_lat_deg = 60.0,
       .eq_probe_lat_deg = 1e-5,
       .left_label = "gore_zone_2_right_case2",
       .right_label = "gore_zone_3_case3"},
      {.seam_lon_deg = -60.0,
       .gore_lat_deg = -60.0,
       .eq_probe_lat_deg = -1e-5,
       .left_label = "gore_zone_4_case4",
       .right_label = "gore_zone_5_case5"},
      {.seam_lon_deg = 90.0,
       .gore_lat_deg = -60.0,
       .eq_probe_lat_deg = -1e-5,
       .left_label = "gore_zone_5_right_case5",
       .right_label = "gore_zone_6_case6"},
  };

  for (const auto& s : kSeams) {
    const double b_x =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(s.seam_lon_deg),
                              DegToRad(s.eq_probe_lat_deg), 0.0, 0.0))
            .xy.x;
    const PJ_COORD left_edge =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(s.seam_lon_deg - 1e-4),
                              DegToRad(s.gore_lat_deg), 0.0, 0.0));
    const PJ_COORD right_edge =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(s.seam_lon_deg + 1e-4),
                              DegToRad(s.gore_lat_deg), 0.0, 0.0));
    ASSERT_LT(left_edge.xy.x, b_x) << s.left_label;
    ASSERT_GT(right_edge.xy.x, b_x) << s.right_label;

    expect_inv_outside(0.5 * (left_edge.xy.x + b_x), left_edge.xy.y,
                       s.left_label);
    expect_inv_outside(0.5 * (b_x + right_edge.xy.x), right_edge.xy.y,
                       s.right_label);
  }

  // 4. Error reset recovers cleanly for subsequent transforms.
  const PJ_COORD valid = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(-10.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(valid.xy.x));
}

TEST(ImollOTest, Batch4DFactorsAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=imoll_o +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. PJ_IDENT and 4D passthrough preserve z and t coordinates.
  const PJ_COORD orig4d =
      proj_coord(DegToRad(20.0), DegToRad(-30.0), 125.5, 2026.25);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, orig4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(orig4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(orig4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(orig4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(orig4d.v[3], ident.v[3]);

  const PJ_COORD fwd4d = proj_trans(pj.get(), PJ_FWD, orig4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(orig4d.xyzt.z, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(orig4d.xyzt.t, fwd4d.xyzt.t);
  const PJ_COORD inv4d = proj_trans(pj.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(orig4d.lpzt.lam, inv4d.lpzt.lam, 1e-10);
  EXPECT_NEAR(orig4d.lpzt.phi, inv4d.lpzt.phi, 1e-10);
  EXPECT_DOUBLE_EQ(orig4d.lpzt.z, inv4d.lpzt.z);
  EXPECT_DOUBLE_EQ(orig4d.lpzt.t, inv4d.lpzt.t);

  // 2. proj_trans_array round-trips across all 6 zones.
  std::array<PJ_COORD, 6> batch = {
      proj_coord(DegToRad(-140.0), DegToRad(30.0), 1.0, 2000.0),
      proj_coord(DegToRad(-10.0), DegToRad(30.0), 2.0, 2001.0),
      proj_coord(DegToRad(130.0), DegToRad(30.0), 3.0, 2002.0),
      proj_coord(DegToRad(-110.0), DegToRad(-30.0), 4.0, 2003.0),
      proj_coord(DegToRad(20.0), DegToRad(-30.0), 5.0, 2004.0),
      proj_coord(DegToRad(150.0), DegToRad(-30.0), 6.0, 2005.0),
  };
  const std::array<PJ_COORD, 6> expected_batch = batch;
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, batch.size(), batch.data()));
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(expected_batch[i].lpzt.lam, batch[i].lpzt.lam, 1e-10);
    EXPECT_NEAR(expected_batch[i].lpzt.phi, batch[i].lpzt.phi, 1e-10);
    EXPECT_DOUBLE_EQ(expected_batch[i].lpzt.z, batch[i].lpzt.z);
    EXPECT_DOUBLE_EQ(expected_batch[i].lpzt.t, batch[i].lpzt.t);
  }

  // imoll_o_s_inverse returns {HUGE_VAL, HUGE_VAL} for out-of-domain y without
  // calling proj_errno_set(), so pj_inv4d replaces the coordinate with
  // proj_coord_error() while leaving last_errno == 0.
  std::array<PJ_COORD, 2> mixed_inv = {
      proj_trans(pj.get(), PJ_FWD,
                 proj_coord(DegToRad(-10.0), DegToRad(30.0), 0.0, 0.0)),
      proj_coord(0.0, 1.0e8, 0.0, 0.0),
  };
  EXPECT_EQ(0, proj_trans_array(pj.get(), PJ_INV, mixed_inv.size(),
                                mixed_inv.data()));
  EXPECT_FALSE(std::isinf(mixed_inv[0].lp.lam));
  EXPECT_EQ(HUGE_VAL, mixed_inv[1].lp.lam);
  EXPECT_EQ(HUGE_VAL, mixed_inv[1].lp.phi);
  proj_errno_reset(pj.get());

  // 3. proj_trans_generic on separate strided arrays.
  std::array<double, 2> lams = {DegToRad(-135.0), DegToRad(145.0)};
  std::array<double, 2> phis = {DegToRad(40.0), DegToRad(-40.0)};
  const std::array<double, 2> lams_orig = lams;
  const std::array<double, 2> phis_orig = phis;
  ASSERT_EQ(2u,
            proj_trans_generic(pj.get(), PJ_FWD, lams.data(), sizeof(double),
                               lams.size(), phis.data(), sizeof(double),
                               phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  ASSERT_EQ(2u,
            proj_trans_generic(pj.get(), PJ_INV, lams.data(), sizeof(double),
                               lams.size(), phis.data(), sizeof(double),
                               phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  for (size_t i = 0; i < lams.size(); ++i) {
    EXPECT_NEAR(lams_orig[i], lams[i], 1e-10);
    EXPECT_NEAR(phis_orig[i], phis[i], 1e-10);
  }

  // 4. proj_roundtrip and proj_factors equal-area check across all 6 zones.
  for (const PJ_COORD& pt : expected_batch) {
    PJ_COORD rt = pt;
    const double rt_m = proj_roundtrip(pj.get(), PJ_FWD, 3, &rt);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_GE(rt_m, 0.0);
    EXPECT_LT(rt_m, 1e-3);

    const PJ_FACTORS fac = proj_factors(pj.get(), pt);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(1.0, fac.areal_scale, 1e-5);
    EXPECT_GT(fac.meridional_scale, 0.0);
    EXPECT_GT(fac.parallel_scale, 0.0);
    EXPECT_GE(fac.tissot_semimajor, fac.tissot_semiminor);
    EXPECT_FALSE(std::isnan(fac.meridian_parallel_angle));
  }

  // 5. Invalid latitude, HUGE_VAL, NaN, and Inf inputs.
  const PJ_COORD bad_lat =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(95.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD err_out = proj_trans(pj.get(), PJ_FWD, proj_coord_error());
  EXPECT_TRUE(std::isinf(err_out.xy.x));
  proj_errno_reset(pj.get());

  const PJ_COORD nan_out = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xy.x));
  EXPECT_TRUE(std::isnan(nan_out.xy.y));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD inf_lon = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_lon.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD neg_inf_lat = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(0.0, -std::numeric_limits<double>::infinity(), 0.0, 0.0));
  EXPECT_TRUE(std::isinf(neg_inf_lat.xy.y));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

// TODO(schwehr): Uncovered lines in proj/src/projections/imoll_o.cpp are
// limited to defensive null/OOM guards (pj_imoll_o_destructor null checks and
// calloc/pj_moll allocation failures in pj_imoll_o_setup_zone /
// PJ_PROJECTION(imoll_o)).

}  // namespace
}  // namespace proj
