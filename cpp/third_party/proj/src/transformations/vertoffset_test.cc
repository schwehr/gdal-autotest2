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
// Unit tests for Vertical Offset and Slope transformations (+proj=vertoffset,
// EPSG method 1046) in proj/src/transformations/vertoffset.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/vertoffset.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kArcSecToRad = kDegToRad / 3600.0;

constexpr double DegToRad(double deg) { return deg * kDegToRad; }

TEST(VertoffsetTest, DefaultZeroParametersActAsIdentity) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=vertoffset +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->left);
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->right);

  const PJ_COORD in = proj_coord(DegToRad(15.0), DegToRad(48.0), 250.5, 2026.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lpzt.lam, fwd.lpzt.lam, 1e-15);
  EXPECT_NEAR(in.lpzt.phi, fwd.lpzt.phi, 1e-15);
  EXPECT_DOUBLE_EQ(in.lpzt.z, fwd.lpzt.z);
  EXPECT_DOUBLE_EQ(in.lpzt.t, fwd.lpzt.t);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-15);
  EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-15);
  EXPECT_DOUBLE_EQ(in.lpzt.z, inv.lpzt.z);
  EXPECT_DOUBLE_EQ(in.lpzt.t, inv.lpzt.t);
}

TEST(VertoffsetTest,
     EllipsoidalOffsetAndSlopesMatchEpsg1046FormulaAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=vertoffset +ellps=GRS80 +lat_0=50 +lon_0=10 "
                          "+dh=-0.255 +slope_lat=-0.012 +slope_lon=0.025"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At the evaluation origin (lon = lon_0, lat = lat_0), both slope terms
  //    vanish and only +dh is applied while lon_0 is preserved on X.
  const PJ_COORD origin_in =
      proj_coord(DegToRad(10.0), DegToRad(50.0), 100.0, 0.0);
  const PJ_COORD origin_fwd = proj_trans(pj.get(), PJ_FWD, origin_in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(DegToRad(10.0), origin_fwd.lpz.lam, 1e-15);
  EXPECT_NEAR(DegToRad(50.0), origin_fwd.lpz.phi, 1e-15);
  EXPECT_NEAR(100.0 - 0.255, origin_fwd.lpz.z, 1e-12);

  // 2. Off-origin point (lon = 12 deg, lat = 52 deg, z = 100 m):
  //    Verify exact EPSG:1046 formula using P->a and P->es from GRS80.
  const double phi0 = DegToRad(50.0);
  const double lam0 = DegToRad(10.0);
  const double sin_phi0 = std::sin(phi0);
  const double w2 = 1.0 - pj->es * sin_phi0 * sin_phi0;
  const double rho0 = pj->a * (1.0 - pj->es) / (w2 * std::sqrt(w2));
  const double nu0 = pj->a / std::sqrt(w2);

  const double lon = DegToRad(12.0);
  const double lat = DegToRad(52.0);
  const double z0 = 100.0;
  const double expected_dz =
      -0.255 + (-0.012 * kArcSecToRad) * rho0 * (lat - phi0) +
      (0.025 * kArcSecToRad) * nu0 * (lon - lam0) * std::cos(lat);

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, z0, 2025.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon, fwd.lpzt.lam, 1e-14);
  EXPECT_NEAR(lat, fwd.lpzt.phi, 1e-14);
  EXPECT_NEAR(z0 + expected_dz, fwd.lpzt.z, 1e-11);
  EXPECT_DOUBLE_EQ(2025.0, fwd.lpzt.t);

  // 3. Inverse 4D round-trip recovers original (lon, lat, z, t).
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon, inv.lpzt.lam, 1e-14);
  EXPECT_NEAR(lat, inv.lpzt.phi, 1e-14);
  EXPECT_NEAR(z0, inv.lpzt.z, 1e-11);
  EXPECT_DOUBLE_EQ(2025.0, inv.lpzt.t);
}

TEST(VertoffsetTest, SphericalRadiusAndDirect3DEntryPointsRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  constexpr double kRadius = 6371000.0;
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=vertoffset +R=6371000 +lat_0=45 +lon_0=-75 "
                          "+dh=1.5 +slope_lat=0.1 +slope_lon=-0.2"));
  ASSERT_NE(nullptr, pj.get());

  // On a sphere (es == 0), rho0 == nu0 == R.
  const double lon = DegToRad(-73.0);
  const double lat = DegToRad(46.0);
  const double z0 = 50.0;
  const double expected_dz =
      1.5 + (0.1 * kArcSecToRad) * kRadius * (lat - DegToRad(45.0)) +
      (-0.2 * kArcSecToRad) * kRadius * (lon - DegToRad(-75.0)) * std::cos(lat);

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lon, lat, z0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon, fwd.lpz.lam, 1e-14);
  EXPECT_NEAR(lat, fwd.lpz.phi, 1e-14);
  EXPECT_NEAR(z0 + expected_dz, fwd.lpz.z, 1e-11);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon, inv.lpz.lam, 1e-14);
  EXPECT_NEAR(lat, inv.lpz.phi, 1e-14);
  EXPECT_NEAR(z0, inv.lpz.z, 1e-11);

  // Direct pj_fwd3d / pj_inv3d entry points.
  const PJ_LPZ in3d = {.lam = lon, .phi = lat, .z = z0};
  const PJ_XYZ out3d = pj_fwd3d(in3d, pj.get());
  EXPECT_NEAR(lon, out3d.x, 1e-14);
  EXPECT_NEAR(lat, out3d.y, 1e-14);
  EXPECT_NEAR(z0 + expected_dz, out3d.z, 1e-11);

  const PJ_LPZ back3d = pj_inv3d(out3d, pj.get());
  EXPECT_NEAR(lon, back3d.lam, 1e-14);
  EXPECT_NEAR(lat, back3d.phi, 1e-14);
  EXPECT_NEAR(z0, back3d.z, 1e-11);
}

}  // namespace
}  // namespace proj
