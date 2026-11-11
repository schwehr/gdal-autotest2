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
// Unit tests for the Laborde Oblique Mercator projection for Madagascar
// (proj/src/projections/labrd.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/labrd.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(LabrdTest, InvalidLat0ZeroOrMissingRejectsCreation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. Missing +lat_0 defaults to 0.0, which is rejected.
  ScopedPj missing_lat0(proj_create(ctx.get(), "+proj=labrd +ellps=intl"));
  EXPECT_EQ(nullptr, missing_lat0.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_0"));

  // 2. Explicit +lat_0=0 is also rejected.
  ScopedPj zero_lat0(
      proj_create(ctx.get(), "+proj=labrd +lat_0=0 +azi=18.9 +ellps=intl"));
  EXPECT_EQ(nullptr, zero_lat0.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(LabrdTest, MadagascarGridForwardAndInverseRoundTripAndFalseOrigin) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Tananarive (Paris) / Laborde Grid parameters for Madagascar.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=labrd +ellps=intl +lat_0=-18.9 +lon_0=46.4372291667 +azi=18.9 "
      "+k_0=0.9995 +x_0=400000 +y_0=800000"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. Projection center (lon_0, lat_0) maps to false origin (400000, 800000).
  const PJ_COORD center_in =
      proj_coord(DegToRad(46.4372291667), DegToRad(-18.9), 25.0, 2026.0);
  const PJ_COORD center_fwd = proj_trans(pj.get(), PJ_FWD, center_in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(400000.0, center_fwd.xy.x, 1e-4);
  EXPECT_NEAR(800000.0, center_fwd.xy.y, 1e-4);
  EXPECT_DOUBLE_EQ(25.0, center_fwd.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.0, center_fwd.xyzt.t);

  // 2. Points across Madagascar round-trip through forward and iterative
  // inverse.
  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 46.4372291667, .lat_deg = -18.9},
      {.lon_deg = 47.52, .lat_deg = -18.91},  // Antananarivo
      {.lon_deg = 49.40, .lat_deg = -18.15},  // Toamasina
      {.lon_deg = 43.68, .lat_deg = -23.35},  // Toliara
      {.lon_deg = 49.29, .lat_deg = -12.27},  // Antsiranana
  };

  for (const auto& tc : kCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(fwd.xy.x));
    EXPECT_FALSE(std::isinf(fwd.xy.y));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    // labrd_e_inverse uses a 5th-order polynomial series reversion (V1..V4,
    // I7..I11) and an iterative latitude loop with EPS = 1e-10, yielding
    // ~1.2e-9 rad (~8 mm) truncation error several degrees from (lon_0, lat_0).
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 5e-9);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 5e-9);
  }
}

TEST(LabrdTest, NorthernHemisphereAndSphericalConfigsAndConformalFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Northern hemisphere ellipsoidal and spherical Laborde configurations.
  for (const char* proj_str : {
           "+proj=labrd +ellps=WGS84 +lat_0=35 +azi=25 +k_0=0.999",
           "+proj=labrd +R=6371000 +lat_0=35 +azi=0",
       }) {
    ScopedPj pj(proj_create(ctx.get(), proj_str));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD in = proj_coord(DegToRad(2.5), DegToRad(36.5), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, 5e-9);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, 5e-9);
  }

  // 2. Laborde is conformal (meridional_scale == parallel_scale,
  // angular_distortion == 0 at the projection center with k_0=0.9995).
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=labrd +ellps=intl +lat_0=-18.9 +azi=18.9 +k_0=0.9995"));
  ASSERT_NE(nullptr, pj.get());
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(-18.9), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.9995, factors.meridional_scale, 1e-6);
  EXPECT_NEAR(0.9995, factors.parallel_scale, 1e-6);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-6);
}

}  // namespace
}  // namespace proj
