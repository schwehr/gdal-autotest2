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
// Unit tests for coordinate unit conversions across horizontal, vertical, and
// temporal dimensions (proj/src/conversions/unitconvert.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/unitconvert.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

TEST(UnitconvertTest, DefaultUnitconvertIsIdentityAndSupportsArgvAndInfo) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Default unitconvert without parameters acts as a 4D identity.
  ScopedPj pj(proj_create(ctx.get(), "+proj=unitconvert"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("unitconvert", info.id);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_output(pj.get(), PJ_FWD));

  const PJ_COORD input = proj_coord(12.5, -34.25, 100.5, 58849.25);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, input);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(input.xyzt.x, ident.xyzt.x, 1e-15);
  EXPECT_NEAR(input.xyzt.y, ident.xyzt.y, 1e-15);
  EXPECT_NEAR(input.xyzt.z, ident.xyzt.z, 1e-15);
  EXPECT_NEAR(input.xyzt.t, ident.xyzt.t, 1e-15);

  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(input.xyzt.x, fwd.xyzt.x, 1e-15);
  EXPECT_NEAR(input.xyzt.y, fwd.xyzt.y, 1e-15);
  EXPECT_NEAR(input.xyzt.z, fwd.xyzt.z, 1e-15);
  EXPECT_NEAR(input.xyzt.t, fwd.xyzt.t, 1e-15);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(input.xyzt.x, inv.xyzt.x, 1e-15);
  EXPECT_NEAR(input.xyzt.y, inv.xyzt.y, 1e-15);
  EXPECT_NEAR(input.xyzt.z, inv.xyzt.z, 1e-15);
  EXPECT_NEAR(input.xyzt.t, inv.xyzt.t, 1e-15);

  // 2. Verify proj_create_argv with mutable argument buffers.
  char arg0[] = "+proj=unitconvert";
  char arg1[] = "+xy_in=km";
  char arg2[] = "+xy_out=m";
  char* argv[] = {arg0, arg1, arg2};
  ScopedPj argv_pj(proj_create_argv(ctx.get(), 3, argv));
  ASSERT_NE(nullptr, argv_pj.get());
  const PJ_COORD argv_fwd =
      proj_trans(argv_pj.get(), PJ_FWD, proj_coord(2.5, -3.0, 42.0, 100.0));
  EXPECT_EQ(0, proj_errno(argv_pj.get()));
  EXPECT_NEAR(2500.0, argv_fwd.xyzt.x, 1e-12);
  EXPECT_NEAR(-3000.0, argv_fwd.xyzt.y, 1e-12);
  EXPECT_NEAR(42.0, argv_fwd.xyzt.z, 1e-12);
  EXPECT_NEAR(100.0, argv_fwd.xyzt.t, 1e-12);
}

TEST(UnitconvertTest, LinearHorizontalUnitConversionsRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct LinearCase {
    const char* def;
    double in_x;
    double in_y;
    double expected_x;
    double expected_y;
    double tol;
  };
  constexpr LinearCase kCases[] = {
      {.def = "+proj=unitconvert +xy_in=km +xy_out=m",
       .in_x = 1.25,
       .in_y = -0.5,
       .expected_x = 1250.0,
       .expected_y = -500.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=m +xy_out=cm",
       .in_x = 2.5,
       .in_y = -1.75,
       .expected_x = 250.0,
       .expected_y = -175.0,
       .tol = 1e-11},
      {.def = "+proj=unitconvert +xy_in=mm +xy_out=km",
       .in_x = 500000.0,
       .in_y = -250000.0,
       .expected_x = 0.5,
       .expected_y = -0.25,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=ft +xy_out=m",
       .in_x = 100.0,
       .in_y = 200.0,
       .expected_x = 30.48,
       .expected_y = 60.96,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=m +xy_out=us-ft",
       .in_x = 1200.0,
       .in_y = 2400.0,
       .expected_x = 3937.0,
       .expected_y = 7874.0,
       .tol = 1e-10},
      {.def = "+proj=unitconvert +xy_in=yd +xy_out=ft",
       .in_x = 4.0,
       .in_y = -9.0,
       .expected_x = 12.0,
       .expected_y = -27.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=mi +xy_out=km",
       .in_x = 1.0,
       .in_y = 2.0,
       .expected_x = 1.609344,
       .expected_y = 3.218688,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=kmi +xy_out=m",
       .in_x = 2.0,
       .in_y = 0.5,
       .expected_x = 3704.0,
       .expected_y = 926.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=in +xy_out=mm",
       .in_x = 10.0,
       .in_y = -4.0,
       .expected_x = 254.0,
       .expected_y = -101.6,
       .tol = 1e-11},
      {.def = "+proj=unitconvert +xy_in=fath +xy_out=yd",
       .in_x = 3.0,
       .in_y = 1.5,
       .expected_x = 6.0,
       .expected_y = 3.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=ch +xy_out=link",
       .in_x = 2.0,
       .in_y = -3.0,
       .expected_x = 200.0,
       .expected_y = -300.0,
       .tol = 1e-11},
      {.def = "+proj=unitconvert +xy_in=us-in +xy_out=us-yd",
       .in_x = 36.0,
       .in_y = 72.0,
       .expected_x = 1.0,
       .expected_y = 2.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=us-ch +xy_out=us-mi",
       .in_x = 80.0,
       .in_y = 160.0,
       .expected_x = 1.0,
       .expected_y = 2.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=ind-yd +xy_out=ind-ft",
       .in_x = 5.0,
       .in_y = -2.0,
       .expected_x = 15.0,
       .expected_y = -6.0,
       .tol = 1e-12},
      // Custom numeric multipliers are supported when not matching a keyword.
      {.def = "+proj=unitconvert +xy_in=2.5 +xy_out=0.5",
       .in_x = 4.0,
       .in_y = -6.0,
       .expected_x = 20.0,
       .expected_y = -30.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=-2.0 +xy_out=m",
       .in_x = 3.0,
       .in_y = -5.0,
       .expected_x = -6.0,
       .expected_y = 10.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_in=m +xy_out=100",
       .in_x = 250.0,
       .in_y = -50.0,
       .expected_x = 2.5,
       .expected_y = -0.5,
       .tol = 1e-12},
      // Single-sided xy_in / xy_out defaults the omitted side to meters (1.0).
      {.def = "+proj=unitconvert +xy_in=km",
       .in_x = 1.5,
       .in_y = -2.25,
       .expected_x = 1500.0,
       .expected_y = -2250.0,
       .tol = 1e-12},
      {.def = "+proj=unitconvert +xy_out=km",
       .in_x = 1500.0,
       .in_y = -2250.0,
       .expected_x = 1.5,
       .expected_y = -2.25,
       .tol = 1e-12},
  };

  constexpr double kZ = 17.25;
  constexpr double kT = 59000.5;
  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;
    EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_FWD)) << tc.def;
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD)) << tc.def;

    // 1. Forward conversion scales only x and y, leaving z and t untouched.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.in_x, tc.in_y, kZ, kT));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, tc.tol) << tc.def;
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, tc.tol) << tc.def;
    EXPECT_NEAR(kZ, fwd.xyzt.z, 1e-12) << tc.def;
    EXPECT_NEAR(kT, fwd.xyzt.t, 1e-12) << tc.def;

    // 2. Inverse conversion restores original x and y.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.in_x, inv.xyzt.x, tc.tol) << tc.def;
    EXPECT_NEAR(tc.in_y, inv.xyzt.y, tc.tol) << tc.def;
    EXPECT_NEAR(kZ, inv.xyzt.z, 1e-12) << tc.def;
    EXPECT_NEAR(kT, inv.xyzt.t, 1e-12) << tc.def;
  }
}

TEST(UnitconvertTest, AngularHorizontalUnitsSetIoUnitPredicatesAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct AngularCase {
    const char* def;
    double in_x;
    double in_y;
    double expected_x;
    double expected_y;
    int angular_in;
    int degree_in;
    int angular_out;
    int degree_out;
  };
  constexpr AngularCase kCases[] = {
      {.def = "+proj=unitconvert +xy_in=deg +xy_out=rad",
       .in_x = 180.0,
       .in_y = -45.0,
       .expected_x = kPi,
       .expected_y = -kPi / 4.0,
       .angular_in = 0,
       .degree_in = 1,
       .angular_out = 1,
       .degree_out = 0},
      {.def = "+proj=unitconvert +xy_in=rad +xy_out=deg",
       .in_x = kPi / 2.0,
       .in_y = -kPi / 6.0,
       .expected_x = 90.0,
       .expected_y = -30.0,
       .angular_in = 1,
       .degree_in = 0,
       .angular_out = 0,
       .degree_out = 1},
      // Grads leave left/right IO units as PJ_IO_UNITS_WHATEVER (neither
      // Radian nor Degree predicates match).
      {.def = "+proj=unitconvert +xy_in=grad +xy_out=deg",
       .in_x = 200.0,
       .in_y = -100.0,
       .expected_x = 180.0,
       .expected_y = -90.0,
       .angular_in = 0,
       .degree_in = 0,
       .angular_out = 0,
       .degree_out = 1},
      {.def = "+proj=unitconvert +xy_in=rad +xy_out=grad",
       .in_x = kPi / 2.0,
       .in_y = -kPi,
       .expected_x = 100.0,
       .expected_y = -200.0,
       .angular_in = 1,
       .degree_in = 0,
       .angular_out = 0,
       .degree_out = 0},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    EXPECT_EQ(tc.angular_in, proj_angular_input(pj.get(), PJ_FWD)) << tc.def;
    EXPECT_EQ(tc.degree_in, proj_degree_input(pj.get(), PJ_FWD)) << tc.def;
    EXPECT_EQ(tc.angular_out, proj_angular_output(pj.get(), PJ_FWD)) << tc.def;
    EXPECT_EQ(tc.degree_out, proj_degree_output(pj.get(), PJ_FWD)) << tc.def;

    EXPECT_EQ(tc.angular_out, proj_angular_input(pj.get(), PJ_INV)) << tc.def;
    EXPECT_EQ(tc.degree_out, proj_degree_input(pj.get(), PJ_INV)) << tc.def;
    EXPECT_EQ(tc.angular_in, proj_angular_output(pj.get(), PJ_INV)) << tc.def;
    EXPECT_EQ(tc.degree_in, proj_degree_output(pj.get(), PJ_INV)) << tc.def;

    // 1. Forward conversion.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(tc.in_x, tc.in_y, 50.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, fwd.xyzt.x, 1e-11) << tc.def;
    EXPECT_NEAR(tc.expected_y, fwd.xyzt.y, 1e-11) << tc.def;
    EXPECT_NEAR(50.0, fwd.xyzt.z, 1e-12) << tc.def;

    // 2. Inverse conversion.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.in_x, inv.xyzt.x, 1e-11) << tc.def;
    EXPECT_NEAR(tc.in_y, inv.xyzt.y, 1e-11) << tc.def;
  }
}

TEST(UnitconvertTest, VerticalUnitConversionsAndDirect2D3DEntryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct VerticalCase {
    const char* def;
    double in_z;
    double expected_z;
  };
  constexpr VerticalCase kCases[] = {
      {.def = "+proj=unitconvert +z_in=ft +z_out=m",
       .in_z = 100.0,
       .expected_z = 30.48},
      {.def = "+proj=unitconvert +z_in=m +z_out=cm",
       .in_z = 2.75,
       .expected_z = 275.0},
      {.def = "+proj=unitconvert +z_in=us-ft +z_out=m",
       .in_z = 3937.0,
       .expected_z = 1200.0},
      // Angular vertical units are accepted when both z_in and z_out are
      // angular.
      {.def = "+proj=unitconvert +z_in=deg +z_out=rad",
       .in_z = 90.0,
       .expected_z = kPi / 2.0},
      // Numeric custom multipliers and single-sided z_in / z_out (other side
      // defaults to 1.0).
      {.def = "+proj=unitconvert +z_in=0.25 +z_out=2.0",
       .in_z = 16.0,
       .expected_z = 2.0},
      {.def = "+proj=unitconvert +z_in=km", .in_z = 1.5, .expected_z = 1500.0},
      {.def = "+proj=unitconvert +z_out=km", .in_z = 1500.0, .expected_z = 1.5},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(5.0, -7.0, tc.in_z, 123.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(5.0, fwd.xyzt.x, 1e-12) << tc.def;
    EXPECT_NEAR(-7.0, fwd.xyzt.y, 1e-12) << tc.def;
    EXPECT_NEAR(tc.expected_z, fwd.xyzt.z, 1e-10) << tc.def;
    EXPECT_NEAR(123.0, fwd.xyzt.t, 1e-12) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.in_z, inv.xyzt.z, 1e-10) << tc.def;
  }

  // Exercise direct internal 2D and 3D entry points alongside combined xy+z.
  ScopedPj combined(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=km +xy_out=m +z_in=ft"));
  ASSERT_NE(nullptr, combined.get());

  const PJ_LP lp = {.lam = 2.0, .phi = -3.5};
  const PJ_XY xy = pj_fwd(lp, combined.get());
  EXPECT_EQ(0, proj_errno(combined.get()));
  EXPECT_NEAR(2000.0, xy.x, 1e-12);
  EXPECT_NEAR(-3500.0, xy.y, 1e-12);
  const PJ_LP lp_back = pj_inv(xy, combined.get());
  EXPECT_EQ(0, proj_errno(combined.get()));
  EXPECT_NEAR(lp.lam, lp_back.lam, 1e-12);
  EXPECT_NEAR(lp.phi, lp_back.phi, 1e-12);

  const PJ_LPZ lpz = {.lam = 1.5, .phi = 2.5, .z = 100.0};
  const PJ_XYZ xyz = pj_fwd3d(lpz, combined.get());
  EXPECT_EQ(0, proj_errno(combined.get()));
  EXPECT_NEAR(1500.0, xyz.x, 1e-12);
  EXPECT_NEAR(2500.0, xyz.y, 1e-12);
  EXPECT_NEAR(30.48, xyz.z, 1e-12);
  const PJ_LPZ lpz_back = pj_inv3d(xyz, combined.get());
  EXPECT_EQ(0, proj_errno(combined.get()));
  EXPECT_NEAR(lpz.lam, lpz_back.lam, 1e-12);
  EXPECT_NEAR(lpz.phi, lpz_back.phi, 1e-12);
  EXPECT_NEAR(lpz.z, lpz_back.z, 1e-12);
}

TEST(UnitconvertTest, TimeUnitConversionsCoverAllCalendarsLeapRulesAndBounds) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct TimeCase {
    const char* def;
    double in_t;
    double expected_t;
    double tol;
    bool test_roundtrip;
  };
  constexpr TimeCase kCases[] = {
      // mjd <-> mjd
      {.def = "+proj=unitconvert +t_in=mjd +t_out=mjd",
       .in_t = 58849.25,
       .expected_t = 58849.25,
       .tol = 1e-12,
       .test_roundtrip = true},
      // gps_week <-> mjd (epoch 1980-01-06 == MJD 44244.0)
      {.def = "+proj=unitconvert +t_in=gps_week +t_out=mjd",
       .in_t = 0.0,
       .expected_t = 44244.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=gps_week +t_out=mjd",
       .in_t = -1.0,
       .expected_t = 44237.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=gps_week",
       .in_t = 2086.5,
       .expected_t = 44244.0 + 2086.5 * 7.0,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_out=gps_week",
       .in_t = 44251.0,
       .expected_t = 1.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      // decimalyear <-> mjd across MJD start (1858/1859), century non-leap
      // (1900), quad-century leap (2000), and standard leap/non-leap years.
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 1859.0,
       .expected_t = 45.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 1858.5,
       .expected_t = -137.5,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 1900.25,
       .expected_t = 15111.25,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 2000.5,
       .expected_t = 51727.0,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 2016.0,
       .expected_t = 57388.0,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 2017.5,
       .expected_t = 57936.5,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear",
       .in_t = 2020.25,
       .expected_t = 58940.5,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_out=decimalyear",
       .in_t = 58849.0,
       .expected_t = 2020.0,
       .tol = 1e-10,
       .test_roundtrip = true},
      // Out-of-range decimalyear hits the [-10000, 10000] guard and returns 0.
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = 10001.0,
       .expected_t = 0.0,
       .tol = 1e-12,
       .test_roundtrip = false},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=mjd",
       .in_t = -10001.0,
       .expected_t = 0.0,
       .tol = 1e-12,
       .test_roundtrip = false},
      // yyyymmdd <-> mjd across 1859-01-01, 1900 non-leap, 2000 quad-century
      // leap, 2020 leap Feb 29, and Dec 31 end-of-year month traversal.
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 18590101.0,
       .expected_t = 45.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 19000228.0,
       .expected_t = 15078.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 19000301.0,
       .expected_t = 15079.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20000229.0,
       .expected_t = 51603.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20200229.0,
       .expected_t = 58908.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20201231.0,
       .expected_t = 59214.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=yyyymmdd",
       .in_t = 19800106.0,
       .expected_t = 44244.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_out=yyyymmdd",
       .in_t = 58909.0,
       .expected_t = 20200301.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      // Invalid month/day values in yyyymmdd are clamped by daynumber_in_year.
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20200015.0,  // month 0 -> Jan 15 (58849 + 14)
       .expected_t = 58863.0,
       .tol = 1e-12,
       .test_roundtrip = false},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20201301.0,  // month 13 -> Dec 01 (59214 - 30)
       .expected_t = 59184.0,
       .tol = 1e-12,
       .test_roundtrip = false},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20200231.0,  // day 31 in leap Feb -> Feb 29
       .expected_t = 58908.0,
       .tol = 1e-12,
       .test_roundtrip = false},
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=mjd",
       .in_t = 20190229.0,  // day 29 in non-leap Feb -> Feb 28 (58849 - 307)
       .expected_t = 58542.0,
       .tol = 1e-12,
       .test_roundtrip = false},
      // Cross-calendar direct pairs (non-MJD on both ends).
      {.def = "+proj=unitconvert +t_in=yyyymmdd +t_out=gps_week",
       .in_t = 19800106.0,
       .expected_t = 0.0,
       .tol = 1e-12,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=gps_week +t_out=decimalyear",
       .in_t = 0.0,
       .expected_t = 1980.0 + 5.0 / 366.0,
       .tol = 1e-10,
       .test_roundtrip = true},
      {.def = "+proj=unitconvert +t_in=decimalyear +t_out=yyyymmdd",
       .in_t = 2020.0,
       .expected_t = 20200101.0,
       .tol = 1e-12,
       .test_roundtrip = true},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, 2.0, 3.0, tc.in_t));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def << " t=" << tc.in_t;
    EXPECT_NEAR(tc.expected_t, fwd.xyzt.t, tc.tol)
        << tc.def << " t=" << tc.in_t;

    if (tc.test_roundtrip) {
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << tc.def << " t=" << tc.in_t;
      EXPECT_NEAR(tc.in_t, inv.xyzt.t, tc.tol) << tc.def << " t=" << tc.in_t;
    }
  }
}

TEST(UnitconvertTest, BatchTransformsRoundtripEllipsoidsAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Batch transformation via proj_trans_array.
  ScopedPj pj(
      proj_create(ctx.get(),
                  "+proj=unitconvert +xy_in=km +xy_out=m +z_in=ft +z_out=m "
                  "+t_in=gps_week +t_out=mjd"));
  ASSERT_NE(nullptr, pj.get());

  std::array<PJ_COORD, 3> coords = {
      proj_coord(1.0, 2.0, 100.0, 0.0),
      proj_coord(-3.5, 4.25, 0.0, 1.0),
      proj_coord(0.0, -0.5, -10.0, 2000.0),
  };
  const std::array<PJ_COORD, 3> orig = coords;
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  EXPECT_NEAR(1000.0, coords[0].xyzt.x, 1e-12);
  EXPECT_NEAR(30.48, coords[0].xyzt.z, 1e-12);
  EXPECT_NEAR(44244.0, coords[0].xyzt.t, 1e-12);
  EXPECT_NEAR(44251.0, coords[1].xyzt.t, 1e-12);
  EXPECT_NEAR(58244.0, coords[2].xyzt.t, 1e-12);

  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(orig[i].xyzt.x, coords[i].xyzt.x, 1e-10);
    EXPECT_NEAR(orig[i].xyzt.y, coords[i].xyzt.y, 1e-10);
    EXPECT_NEAR(orig[i].xyzt.z, coords[i].xyzt.z, 1e-10);
    EXPECT_NEAR(orig[i].xyzt.t, coords[i].xyzt.t, 1e-10);
  }

  // 2. Strided generic transformation (with null z/t broadcast on pure 2D op).
  ScopedPj pj_2d(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=km +xy_out=m"));
  ASSERT_NE(nullptr, pj_2d.get());
  struct StridedPoint {
    double x;
    double y;
    int tag;
  };
  std::array<StridedPoint, 2> pts = {
      StridedPoint{.x = 1.5, .y = -2.5, .tag = 7},
      StridedPoint{.x = -0.25, .y = 8.0, .tag = 9},
  };
  EXPECT_EQ(2u, proj_trans_generic(pj_2d.get(), PJ_FWD, &pts[0].x,
                                   sizeof(StridedPoint), pts.size(), &pts[0].y,
                                   sizeof(StridedPoint), pts.size(), nullptr, 0,
                                   0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_2d.get()));
  EXPECT_NEAR(1500.0, pts[0].x, 1e-12);
  EXPECT_NEAR(-2500.0, pts[0].y, 1e-12);
  EXPECT_NEAR(-250.0, pts[1].x, 1e-12);
  EXPECT_NEAR(8000.0, pts[1].y, 1e-12);
  EXPECT_EQ(7, pts[0].tag);
  EXPECT_EQ(9, pts[1].tag);

  // 3. Round-trip helper check.
  PJ_COORD rt_coord = proj_coord(12.34, -56.78, 90.0, 2000.0);
  EXPECT_LT(proj_roundtrip(pj.get(), PJ_FWD, 5, &rt_coord), 1e-9);
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 4. Spherical (+R=...) vs ellipsoidal (+ellps=WGS84) definitions and
  // pipelines produce identical unit conversion scaling and valid proj_factors.
  constexpr const char* kFactorDefs[] = {
      "+proj=unitconvert +xy_in=2.0 +xy_out=1.0 +R=6371000",
      "+proj=unitconvert +xy_in=2.0 +xy_out=1.0 +ellps=WGS84",
  };
  for (const char* def : kFactorDefs) {
    ScopedPj factor_pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, factor_pj.get()) << def;
    const PJ_FACTORS fac =
        proj_factors(factor_pj.get(), proj_coord(0.1, 0.2, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(factor_pj.get())) << def;
    EXPECT_NEAR(2.0, fac.dx_dlam, 1e-9) << def;
    EXPECT_NEAR(0.0, fac.dx_dphi, 1e-9) << def;
    EXPECT_NEAR(0.0, fac.dy_dlam, 1e-9) << def;
    EXPECT_NEAR(2.0, fac.dy_dphi, 1e-9) << def;
  }

  ScopedPj sphere_pipe(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=unitconvert +xy_in=deg "
                  "+xy_out=rad +step +proj=merc +R=6371000 "
                  "+step +proj=unitconvert +xy_in=m +xy_out=km"));
  ScopedPj ellps_pipe(
      proj_create(ctx.get(),
                  "+proj=pipeline +step +proj=unitconvert +xy_in=deg "
                  "+xy_out=rad +step +proj=merc +ellps=WGS84 "
                  "+step +proj=unitconvert +xy_in=m +xy_out=km"));
  ASSERT_NE(nullptr, sphere_pipe.get());
  ASSERT_NE(nullptr, ellps_pipe.get());
  const PJ_COORD sphere_out =
      proj_trans(sphere_pipe.get(), PJ_FWD, proj_coord(10.0, 45.0, 0.0, 0.0));
  const PJ_COORD ellps_out =
      proj_trans(ellps_pipe.get(), PJ_FWD, proj_coord(10.0, 45.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sphere_pipe.get()));
  EXPECT_EQ(0, proj_errno(ellps_pipe.get()));
  EXPECT_GT(std::abs(sphere_out.xyzt.y - ellps_out.xyzt.y), 10.0);
}

TEST(UnitconvertTest, ParameterValidationErrorsAndTraceLogging) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Verify trace logging emits normalized unit descriptions for all params.
  {
    ScopedLogCapture trace_capture(ctx.get(), PJ_LOG_TRACE);
    ScopedPj pj(
        proj_create(ctx.get(),
                    "+proj=unitconvert +xy_in=km +xy_out=m +z_in=ft +z_out=cm "
                    "+t_in=decimalyear +t_out=gps_week"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_TRUE(trace_capture.ContainsMessage("xy_in unit: Kilometer"));
    EXPECT_TRUE(trace_capture.ContainsMessage("xy_out unit: Meter"));
    EXPECT_TRUE(trace_capture.ContainsMessage("z_in unit: International Foot"));
    EXPECT_TRUE(trace_capture.ContainsMessage("z_out unit: Centimeter"));
    EXPECT_TRUE(trace_capture.ContainsMessage("t_in unit: Decimal year"));
    EXPECT_TRUE(trace_capture.ContainsMessage("t_out unit: GPS Week"));
  }

  // 2. Verify every invalid parameter and unit-type mismatch branch.
  struct ErrorCase {
    const char* def;
    const char* expected_log;
  };
  constexpr ErrorCase kErrorCases[] = {
      {.def = "+proj=unitconvert +xy_in=bogus_unit",
       .expected_log = "unknown xy_in unit"},
      {.def = "+proj=unitconvert +xy_in=0",
       .expected_log = "unknown xy_in unit"},
      {.def = "+proj=unitconvert +xy_in=inf",
       .expected_log = "unknown xy_in unit"},
      {.def = "+proj=unitconvert +xy_out=bogus_unit",
       .expected_log = "unknown xy_out unit"},
      {.def = "+proj=unitconvert +xy_out=0",
       .expected_log = "unknown xy_out unit"},
      {.def = "+proj=unitconvert +xy_out=inf",
       .expected_log = "unknown xy_out unit"},
      {.def = "+proj=unitconvert +xy_in=m +xy_out=deg",
       .expected_log = "inconsistent unit type between xy_in and xy_out"},
      {.def = "+proj=unitconvert +xy_in=rad +xy_out=km",
       .expected_log = "inconsistent unit type between xy_in and xy_out"},
      {.def = "+proj=unitconvert +z_in=bogus_unit",
       .expected_log = "unknown z_in unit"},
      {.def = "+proj=unitconvert +z_in=0", .expected_log = "unknown z_in unit"},
      {.def = "+proj=unitconvert +z_in=inf",
       .expected_log = "unknown z_in unit"},
      {.def = "+proj=unitconvert +z_out=bogus_unit",
       .expected_log = "unknown z_out unit"},
      {.def = "+proj=unitconvert +z_out=0",
       .expected_log = "unknown z_out unit"},
      {.def = "+proj=unitconvert +z_out=inf",
       .expected_log = "unknown z_out unit"},
      {.def = "+proj=unitconvert +z_in=m +z_out=rad",
       .expected_log = "inconsistent unit type between z_in and z_out"},
      {.def = "+proj=unitconvert +z_in=deg +z_out=ft",
       .expected_log = "inconsistent unit type between z_in and z_out"},
      {.def = "+proj=unitconvert +t_in=bogus_time",
       .expected_log = "unknown t_in unit"},
      {.def = "+proj=unitconvert +t_out=bogus_time",
       .expected_log = "unknown t_out unit"},
  };

  for (const auto& tc : kErrorCases) {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, bad.get()) << tc.def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << tc.def;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_log))
        << tc.def << " missing log: " << tc.expected_log;
  }
}

TEST(UnitconvertTest, EdgeInputsNullEmptyNanInfAndHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Null and empty definition strings fail cleanly.
  EXPECT_EQ(nullptr, proj_create(ctx.get(), nullptr));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  EXPECT_EQ(nullptr, proj_create(ctx.get(), ""));
  EXPECT_NE(0, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 2. HUGE_VAL and NaN coordinate handling in forward and inverse transforms.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=unitconvert +xy_in=km +xy_out=m "
                          "+t_in=decimalyear +t_out=mjd"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD huge_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 1.0, 2.0, 2020.0));
  EXPECT_TRUE(std::isinf(huge_fwd.xyzt.x));

  const PJ_COORD huge_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 1.0, 2.0, 58849.0));
  EXPECT_TRUE(std::isinf(huge_inv.xyzt.x));

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kNaN, 1.0, 2.0, 2020.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.x));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.y));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.z));
  EXPECT_TRUE(std::isnan(nan_fwd.xyzt.t));

  // 3. Infinity and NaN in t alone hit decimalyear_to_mjd's NaN/range guard.
  const PJ_COORD inf_t_fwd = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(1.0, 2.0, 3.0, std::numeric_limits<double>::infinity()));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, inf_t_fwd.xyzt.t, 1e-12);

  PJ_COORD direct_nan_t = proj_coord(1.0, 2.0, 3.0, kNaN);
  EXPECT_TRUE(pj_fwd4d(direct_nan_t, pj.get()));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, direct_nan_t.xyzt.t, 1e-12);
}

}  // namespace
}  // namespace proj
