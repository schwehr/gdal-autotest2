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
// Unit tests for the Equidistant Cylindrical / Plate Carree projection
// (proj/src/projections/eqc.cpp, EPSG:1028 ellipsoidal and EPSG:1029
// spherical).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eqc.cpp

#include <cmath>
#include <cstdlib>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(EqcTest, InvalidLatTsGreaterThan90DegreesRejectsCreation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  for (const char* proj_str : {
           "+proj=eqc +lat_ts=95 +ellps=WGS84",
           "+proj=eqc +lat_ts=-100 +R=6371000",
       }) {
    ScopedPj pj(proj_create(ctx.get(), proj_str));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_ts"));
}

TEST(EqcTest, SphericalPlateCarreeAndShiftedOriginForwardInverseExactFormulas) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kRadius = 6371000.0;
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=eqc +R=6371000 +lat_ts=60 +lat_0=10 +lon_0=5"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // Spherical formulas (EPSG:1029):
  //   E = R * cos(lat_ts) * (lon - lon_0)
  //   N = R * (lat - lat_0)
  const double lon_rad = DegToRad(25.0);
  const double lat_rad = DegToRad(40.0);
  const double expected_x =
      kRadius * std::cos(DegToRad(60.0)) * (lon_rad - DegToRad(5.0));
  const double expected_y = kRadius * (lat_rad - DegToRad(10.0));

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 50.0, 2026.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_x, fwd.xy.x, 1e-6);
  EXPECT_NEAR(expected_y, fwd.xy.y, 1e-6);
  EXPECT_DOUBLE_EQ(50.0, fwd.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.0, fwd.xyzt.t);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-12);
  EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-12);
}

TEST(EqcTest, EllipsoidalForwardAndInverseRoundTripAndReferenceValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(), "+proj=eqc +ellps=WGS84 +lat_ts=30 +lat_0=15 +lon_0=-10"));
  ASSERT_NE(nullptr, pj.get());

  // 1. At (lon_0, lat_0), projected coordinates are (0, 0).
  const PJ_COORD origin_fwd = proj_trans(
      pj.get(), PJ_FWD, proj_coord(DegToRad(-10.0), DegToRad(15.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin_fwd.xy.x, 1e-9);
  EXPECT_NEAR(0.0, origin_fwd.xy.y, 1e-9);

  // 2. Round-trip across northern/southern latitudes and eastern/western
  // longitudes via proj_trans_array.
  PJ_COORD coords[] = {
      proj_coord(DegToRad(-10.0), DegToRad(15.0), 0.0, 0.0),
      proj_coord(DegToRad(20.0), DegToRad(45.0), 10.0, 0.0),
      proj_coord(DegToRad(-75.0), DegToRad(-30.0), 20.0, 0.0),
      proj_coord(DegToRad(140.0), DegToRad(80.0), 30.0, 0.0),
  };
  const PJ_COORD originals[4] = {coords[0], coords[1], coords[2], coords[3]};

  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 4, coords));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 4, coords));

  for (int i = 0; i < 4; ++i) {
    EXPECT_NEAR(originals[i].lp.lam, coords[i].lp.lam, 1e-11);
    EXPECT_NEAR(originals[i].lp.phi, coords[i].lp.phi, 1e-11);
    EXPECT_DOUBLE_EQ(originals[i].lpz.z, coords[i].lpz.z);
  }
}

TEST(EqcTest, FactorsOnStandardParallelAndDestructorNullBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=eqc +R=6371000 +lat_ts=45"));
  ASSERT_NE(nullptr, pj.get());

  // On a sphere at lat = lat_ts = 45 deg, parallel scale and meridional scale
  // are both 1.0.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-7);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-7);
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-7);
  EXPECT_NEAR(0.0, factors.meridian_convergence, 1e-7);

  // Exercise destructor null-pointer and null-opaque branches.
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
  std::free(pj->opaque);
  pj->opaque = nullptr;
}

}  // namespace
}  // namespace proj
