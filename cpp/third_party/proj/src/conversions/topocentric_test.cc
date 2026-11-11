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
// Unit tests for geocentric <-> topocentric (ENU) coordinate conversions
// (proj/src/conversions/topocentric.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/topocentric.cpp

#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <numbers>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;

constexpr double DegToRad(double deg) { return deg * kDegToRad; }

TEST(TopocentricTest, MissingOrMutuallyExclusiveOriginArgsFailInitialization) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  struct InvalidCase {
    const char* proj_str;
    int expected_errno;
    const char* expected_msg;
  };
  constexpr InvalidCase kCases[] = {
      {
          .proj_str = "+proj=topocentric +ellps=WGS84",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing X_0 or lon_0",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +lat_0=55",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing X_0 or lon_0",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +Y_0=100 +Z_0=200",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing X_0 or lon_0",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +h_0=50",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing X_0 or lon_0",
      },
      {
          .proj_str =
              "+proj=topocentric +ellps=WGS84 +X_0=1 +Y_0=2 +Z_0=3 +lon_0=0 "
              "+lat_0=0",
          .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
          .expected_msg =
              "(X_0,Y_0,Z_0) and (lon_0,lat_0,h_0) are mutually exclusive",
      },
      {
          .proj_str =
              "+proj=topocentric +ellps=WGS84 +X_0=1 +Y_0=2 +Z_0=3 +h_0=10",
          .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
          .expected_msg =
              "(X_0,Y_0,Z_0) and (lon_0,lat_0,h_0) are mutually exclusive",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +Y_0=2 +lon_0=0 +lat_0=0",
          .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
          .expected_msg =
              "(X_0,Y_0,Z_0) and (lon_0,lat_0,h_0) are mutually exclusive",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +Z_0=3 +lon_0=0 +lat_0=0",
          .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
          .expected_msg =
              "(X_0,Y_0,Z_0) and (lon_0,lat_0,h_0) are mutually exclusive",
      },
      {
          .proj_str =
              "+proj=topocentric +ellps=WGS84 +X_0=1 +Y_0=2 +Z_0=3 +lat_0=0",
          .expected_errno = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
          .expected_msg =
              "(X_0,Y_0,Z_0) and (lon_0,lat_0,h_0) are mutually exclusive",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +X_0=100 +Z_0=200",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing Y_0 and/or Z_0",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +X_0=100 +Y_0=200",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing Y_0 and/or Z_0",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +X_0=100",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing Y_0 and/or Z_0",
      },
      {
          .proj_str = "+proj=topocentric +ellps=WGS84 +lon_0=10",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
          .expected_msg = "missing lat_0",
      },
  };

  for (const auto& tc : kCases) {
    capture.Clear();
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), tc.proj_str));
    EXPECT_EQ(nullptr, pj.get()) << tc.proj_str;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.proj_str;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_msg)) << tc.proj_str;
  }
}

TEST(TopocentricTest, CardinalAndPolarOriginsMatchExactEnuAxes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Equator / Prime Meridian on WGS84 (a = 6378137 m):
  //    e_hat = (0, 1, 0), n_hat = (0, 0, 1), u_hat = (1, 0, 0).
  {
    ScopedPj pj(proj_create(
        ctx.get(), "+proj=topocentric +ellps=WGS84 +lon_0=0 +lat_0=0 +h_0=0"));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, pj_left(pj.get()));
    EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, pj_right(pj.get()));
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(6378137.0 + 100.0, 250.0, -400.0, 2026.5));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(250.0, fwd.xyz.x, 1e-9);
    EXPECT_NEAR(-400.0, fwd.xyz.y, 1e-9);
    EXPECT_NEAR(100.0, fwd.xyz.z, 1e-9);
    EXPECT_DOUBLE_EQ(2026.5, fwd.xyzt.t);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(6378237.0, inv.xyz.x, 1e-7);
    EXPECT_NEAR(250.0, inv.xyz.y, 1e-8);
    EXPECT_NEAR(-400.0, inv.xyz.z, 1e-8);
    EXPECT_DOUBLE_EQ(2026.5, inv.xyzt.t);
  }

  // 2. Sphere at 90E, equator, h_0 = 100 m:
  //    X_0 = 0, Y_0 = R + 100, Z_0 = 0;
  //    e = (-1, 0, 0), n = (0, 0, 1), u = (0, 1, 0).
  {
    ScopedPj pj(proj_create(
        ctx.get(), "+proj=topocentric +R=6371000 +lon_0=90 +lat_0=0 +h_0=100"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(-150.0, 6371100.0 + 200.0, 500.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(150.0, fwd.xyz.x, 1e-8);
    EXPECT_NEAR(500.0, fwd.xyz.y, 1e-8);
    EXPECT_NEAR(200.0, fwd.xyz.z, 1e-8);

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(-150.0, inv.xyz.x, 1e-8);
    EXPECT_NEAR(6371300.0, inv.xyz.y, 1e-7);
    EXPECT_NEAR(500.0, inv.xyz.z, 1e-8);
  }

  // 3. North and South poles on sphere:
  //    North pole: e = (0, 1, 0), n = (-1, 0, 0), u = (0, 0, 1).
  //    South pole: e = (0, 1, 0), n = (1, 0, 0), u = (0, 0, -1).
  {
    ScopedPj north(proj_create(
        ctx.get(), "+proj=topocentric +R=6371000 +lon_0=0 +lat_0=90"));
    ASSERT_NE(nullptr, north.get());
    const PJ_COORD north_fwd = proj_trans(
        north.get(), PJ_FWD, proj_coord(100.0, 250.0, 6371000.0 + 50.0, 0.0));
    EXPECT_EQ(0, proj_errno(north.get()));
    EXPECT_NEAR(250.0, north_fwd.xyz.x, 1e-9);
    EXPECT_NEAR(-100.0, north_fwd.xyz.y, 1e-9);
    EXPECT_NEAR(50.0, north_fwd.xyz.z, 1e-9);

    ScopedPj south(proj_create(
        ctx.get(), "+proj=topocentric +R=6371000 +lon_0=0 +lat_0=-90 +h_0=10"));
    ASSERT_NE(nullptr, south.get());
    const PJ_COORD south_fwd = proj_trans(
        south.get(), PJ_FWD, proj_coord(100.0, 250.0, -6371010.0 + 50.0, 0.0));
    EXPECT_EQ(0, proj_errno(south.get()));
    EXPECT_NEAR(250.0, south_fwd.xyz.x, 1e-9);
    EXPECT_NEAR(100.0, south_fwd.xyz.y, 1e-9);
    EXPECT_NEAR(-50.0, south_fwd.xyz.z, 1e-9);
  }
}

TEST(TopocentricTest,
     GeocentricAndGeographicOriginsProduceEquivalentTransforms) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kLon0Deg = 5.0;
  constexpr double kLat0Deg = 55.0;
  constexpr double kH0 = 200.0;

  // 1. Compute the geocentric origin (X_0, Y_0, Z_0) via +proj=cart.
  ScopedPj cart(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, cart.get());
  const PJ_COORD origin_xyz =
      proj_trans(cart.get(), PJ_FWD,
                 proj_coord(DegToRad(kLon0Deg), DegToRad(kLat0Deg), kH0, 0.0));
  ASSERT_EQ(0, proj_errno(cart.get()));

  // 2. Construct both geographic-origin and geocentric-origin conversions.
  ScopedPj topo_geog(proj_create(
      ctx.get(), "+proj=topocentric +ellps=WGS84 +lon_0=5 +lat_0=55 +h_0=200"));
  ASSERT_NE(nullptr, topo_geog.get());

  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss << std::setprecision(17) << "+proj=topocentric +ellps=WGS84"
      << " +X_0=" << origin_xyz.xyz.x << " +Y_0=" << origin_xyz.xyz.y
      << " +Z_0=" << origin_xyz.xyz.z;
  const std::string geoc_def = oss.str();
  ScopedPj topo_geoc(proj_create(ctx.get(), geoc_def.c_str()));
  ASSERT_NE(nullptr, topo_geoc.get());

  // 3. Moving purely along the ellipsoidal normal at (lon_0, lat_0) yields
  //    exact (0, 0, dh) in ENU.
  const PJ_COORD up_xyz = proj_trans(
      cart.get(), PJ_FWD,
      proj_coord(DegToRad(kLon0Deg), DegToRad(kLat0Deg), kH0 + 150.0, 0.0));
  ASSERT_EQ(0, proj_errno(cart.get()));

  const PJ_COORD up_from_geog = proj_trans(topo_geog.get(), PJ_FWD, up_xyz);
  const PJ_COORD up_from_geoc = proj_trans(topo_geoc.get(), PJ_FWD, up_xyz);
  EXPECT_EQ(0, proj_errno(topo_geog.get()));
  EXPECT_EQ(0, proj_errno(topo_geoc.get()));
  EXPECT_NEAR(0.0, up_from_geog.xyz.x, 1e-8);
  EXPECT_NEAR(0.0, up_from_geog.xyz.y, 1e-8);
  EXPECT_NEAR(150.0, up_from_geog.xyz.z, 1e-8);
  EXPECT_NEAR(up_from_geog.xyz.x, up_from_geoc.xyz.x, 1e-7);
  EXPECT_NEAR(up_from_geog.xyz.y, up_from_geoc.xyz.y, 1e-7);
  EXPECT_NEAR(up_from_geog.xyz.z, up_from_geoc.xyz.z, 1e-7);

  // 4. General offset point verifies exact 3D ENU rotation and round-trip.
  const PJ_COORD target_xyz =
      proj_trans(cart.get(), PJ_FWD,
                 proj_coord(DegToRad(6.25), DegToRad(55.75), 350.0, 2024.25));
  ASSERT_EQ(0, proj_errno(cart.get()));

  const PJ_COORD enu_geog = proj_trans(topo_geog.get(), PJ_FWD, target_xyz);
  const PJ_COORD enu_geoc = proj_trans(topo_geoc.get(), PJ_FWD, target_xyz);
  EXPECT_EQ(0, proj_errno(topo_geog.get()));
  EXPECT_EQ(0, proj_errno(topo_geoc.get()));
  EXPECT_NEAR(enu_geog.xyz.x, enu_geoc.xyz.x, 1e-6);
  EXPECT_NEAR(enu_geog.xyz.y, enu_geoc.xyz.y, 1e-6);
  EXPECT_NEAR(enu_geog.xyz.z, enu_geoc.xyz.z, 1e-6);
  EXPECT_DOUBLE_EQ(2024.25, enu_geog.xyzt.t);

  const PJ_COORD back_geog = proj_trans(topo_geog.get(), PJ_INV, enu_geog);
  const PJ_COORD back_geoc = proj_trans(topo_geoc.get(), PJ_INV, enu_geoc);
  EXPECT_EQ(0, proj_errno(topo_geog.get()));
  EXPECT_EQ(0, proj_errno(topo_geoc.get()));
  EXPECT_NEAR(target_xyz.xyz.x, back_geog.xyz.x, 1e-7);
  EXPECT_NEAR(target_xyz.xyz.y, back_geog.xyz.y, 1e-7);
  EXPECT_NEAR(target_xyz.xyz.z, back_geog.xyz.z, 1e-7);
  EXPECT_NEAR(target_xyz.xyz.x, back_geoc.xyz.x, 1e-7);
  EXPECT_NEAR(target_xyz.xyz.y, back_geoc.xyz.y, 1e-7);
  EXPECT_NEAR(target_xyz.xyz.z, back_geoc.xyz.z, 1e-7);
}

TEST(TopocentricTest, OmittedH0DefaultsToZeroAndEllipsoidMatters) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Omitting +h_0 defaults origin ellipsoidal height to 0.
  ScopedPj without_h0(proj_create(
      ctx.get(), "+proj=topocentric +ellps=WGS84 +lon_0=-122 +lat_0=-33.5"));
  ScopedPj with_h0_zero(proj_create(
      ctx.get(),
      "+proj=topocentric +ellps=WGS84 +lon_0=-122 +lat_0=-33.5 +h_0=0"));
  ASSERT_NE(nullptr, without_h0.get());
  ASSERT_NE(nullptr, with_h0_zero.get());

  const PJ_COORD sample_xyz =
      proj_coord(-2800000.0, -4500000.0, -3500000.0, 0.0);
  const PJ_COORD enu_a = proj_trans(without_h0.get(), PJ_FWD, sample_xyz);
  const PJ_COORD enu_b = proj_trans(with_h0_zero.get(), PJ_FWD, sample_xyz);
  EXPECT_EQ(0, proj_errno(without_h0.get()));
  EXPECT_EQ(0, proj_errno(with_h0_zero.get()));
  EXPECT_DOUBLE_EQ(enu_b.xyz.x, enu_a.xyz.x);
  EXPECT_DOUBLE_EQ(enu_b.xyz.y, enu_a.xyz.y);
  EXPECT_DOUBLE_EQ(enu_b.xyz.z, enu_a.xyz.z);

  // 2. Sphere vs WGS84 with identical (X_0, Y_0, Z_0) compute different
  //    geodetic phi0 via pj_inv3d, rotating N and U by ~0.19 deg at 45N.
  ScopedPj topo_wgs84(proj_create(
      ctx.get(),
      "+proj=topocentric +ellps=WGS84 +X_0=4500000 +Y_0=0 +Z_0=4500000"));
  ScopedPj topo_sphere(proj_create(
      ctx.get(),
      "+proj=topocentric +R=6371000 +X_0=4500000 +Y_0=0 +Z_0=4500000"));
  ASSERT_NE(nullptr, topo_wgs84.get());
  ASSERT_NE(nullptr, topo_sphere.get());

  const PJ_COORD probe_xyz = proj_coord(4500000.0, 0.0, 5500000.0, 0.0);
  const PJ_COORD enu_wgs84 = proj_trans(topo_wgs84.get(), PJ_FWD, probe_xyz);
  const PJ_COORD enu_sphere = proj_trans(topo_sphere.get(), PJ_FWD, probe_xyz);
  EXPECT_EQ(0, proj_errno(topo_wgs84.get()));
  EXPECT_EQ(0, proj_errno(topo_sphere.get()));
  // At 1000 km along +Z, ~0.192 deg rotation shifts N and U by > 2000 m.
  EXPECT_GT(std::abs(enu_wgs84.xyz.y - enu_sphere.xyz.y), 2000.0);
  EXPECT_GT(std::abs(enu_wgs84.xyz.z - enu_sphere.xyz.z), 2000.0);
}

TEST(TopocentricTest, PipelineBatchAndLegacyEntryPointsRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Pipeline from geographic (lon/lat rad, h m) -> topocentric (E, N, U m).
  ScopedPj pipe(proj_create(
      ctx.get(),
      "+proj=pipeline +step +proj=cart +ellps=WGS84 "
      "+step +proj=topocentric +ellps=WGS84 +lon_0=10 +lat_0=48 +h_0=500"));
  ASSERT_NE(nullptr, pipe.get());

  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(10.0), DegToRad(48.0), 500.0, 2020.0),
      proj_coord(DegToRad(10.5), DegToRad(48.25), 750.0, 2025.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pipe.get(), PJ_FWD, coords.size(), coords.data()));
  EXPECT_NEAR(0.0, coords[0].xyz.x, 1e-8);
  EXPECT_NEAR(0.0, coords[0].xyz.y, 1e-8);
  EXPECT_NEAR(0.0, coords[0].xyz.z, 1e-8);
  EXPECT_GT(coords[1].xyz.x, 30000.0);
  EXPECT_GT(coords[1].xyz.y, 20000.0);

  ASSERT_EQ(0,
            proj_trans_array(pipe.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(10.0), coords[0].lpz.lam, 1e-12);
  EXPECT_NEAR(DegToRad(48.0), coords[0].lpz.phi, 1e-12);
  EXPECT_NEAR(500.0, coords[0].lpz.z, 1e-5);
  EXPECT_NEAR(DegToRad(10.5), coords[1].lpz.lam, 1e-12);
  EXPECT_NEAR(DegToRad(48.25), coords[1].lpz.phi, 1e-12);
  EXPECT_NEAR(750.0, coords[1].lpz.z, 1e-5);

  // 2. Standalone proj_trans_generic and pj_fwd3d/pj_inv3d.
  ScopedPj topo(proj_create(
      ctx.get(), "+proj=topocentric +R=6371000 +lon_0=0 +lat_0=0 +h_0=0"));
  ASSERT_NE(nullptr, topo.get());

  std::array<double, 2> x = {6371010.0, 6371020.0};
  std::array<double, 2> y = {100.0, -200.0};
  std::array<double, 2> z = {300.0, -400.0};
  EXPECT_EQ(2u, proj_trans_generic(topo.get(), PJ_FWD, x.data(), sizeof(double),
                                   x.size(), y.data(), sizeof(double), y.size(),
                                   z.data(), sizeof(double), z.size(), nullptr,
                                   0, 0));
  EXPECT_EQ(0, proj_errno(topo.get()));
  EXPECT_NEAR(100.0, x[0], 1e-9);
  EXPECT_NEAR(300.0, y[0], 1e-9);
  EXPECT_NEAR(10.0, z[0], 1e-9);
  EXPECT_NEAR(-200.0, x[1], 1e-9);
  EXPECT_NEAR(-400.0, y[1], 1e-9);
  EXPECT_NEAR(20.0, z[1], 1e-9);

  const PJ_XYZ enu3d =
      pj_fwd3d({.lam = 6371050.0, .phi = 15.0, .z = -25.0}, topo.get());
  EXPECT_EQ(0, proj_errno(topo.get()));
  EXPECT_NEAR(15.0, enu3d.x, 1e-9);
  EXPECT_NEAR(-25.0, enu3d.y, 1e-9);
  EXPECT_NEAR(50.0, enu3d.z, 1e-9);

  const PJ_LPZ xyz3d = pj_inv3d(enu3d, topo.get());
  EXPECT_EQ(0, proj_errno(topo.get()));
  EXPECT_NEAR(6371050.0, xyz3d.lam, 1e-7);
  EXPECT_NEAR(15.0, xyz3d.phi, 1e-8);
  EXPECT_NEAR(-25.0, xyz3d.z, 1e-8);
}

TEST(TopocentricTest, EdgeCoordinatesAndProjFactorsErrorBehavior) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(), "+proj=topocentric +ellps=WGS84 +lon_0=0 +lat_0=0 +h_0=0"));
  ASSERT_NE(nullptr, pj.get());

  // 1. PJ_IDENT leaves coordinates untouched.
  const PJ_COORD ident =
      proj_trans(pj.get(), PJ_IDENT, proj_coord(1.0, 2.0, 3.0, 4.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(1.0, ident.xyzt.x);
  EXPECT_DOUBLE_EQ(2.0, ident.xyzt.y);
  EXPECT_DOUBLE_EQ(3.0, ident.xyzt.z);
  EXPECT_DOUBLE_EQ(4.0, ident.xyzt.t);

  // 2. HUGE_VAL in forward cartesian input returns proj_coord_error() without
  //    setting errno (fwd_prepare); inverse sets OUTSIDE_PROJECTION_DOMAIN.
  const PJ_COORD huge_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(huge_fwd.xyzt.x));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD huge_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.0, HUGE_VAL, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(huge_inv.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 3. NaN propagates cleanly to all components without setting errno.
  const PJ_COORD nan_out = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.x));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.y));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.z));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.t));
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 4. proj_factors fails because topocentric defines fwd4d rather than 2D fwd.
  const PJ_FACTORS fac = proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, fac.meridional_scale);
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
