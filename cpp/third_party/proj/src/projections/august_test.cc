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
// Unit tests for the August Epicycloidal projection
// (proj/src/projections/august.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/august.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

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
constexpr double kAugustM = 1.333333333333333;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

PJ_XY ExpectedAugustUnitSphere(double lam, double phi) {
  const double t = std::tan(0.5 * phi);
  const double c1 = std::sqrt(1.0 - t * t);
  const double half_lam = 0.5 * lam;
  const double c = 1.0 + c1 * std::cos(half_lam);
  const double x1 = std::sin(half_lam) * c1 / c;
  const double y1 = t / c;
  const double x12 = x1 * x1;
  const double y12 = y1 * y1;
  return {
      .x = kAugustM * x1 * (3.0 + x12 - 3.0 * y12),
      .y = kAugustM * y1 * (3.0 + 3.0 * x12 - y12),
  };
}

C_NAMESPACE PJ* pj_august(PJ*);

TEST(AugustTest, MetadataAndConstructorForceSphericalAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=august +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("august", info.id);
  EXPECT_THAT(std::string_view(pj->descr), HasSubstr("August Epicycloidal"));
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  // Exercise the PJ_PROJECTION(august) constructor entry point directly.
  ScopedPj raw(pj_august(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("august", raw->short_name);
  EXPECT_EQ(1, raw->need_ellps);
  EXPECT_EQ(raw.get(), pj_august(raw.get()));
  EXPECT_EQ(nullptr, raw->inv);
  EXPECT_NE(nullptr, raw->fwd);
  EXPECT_EQ(0.0, raw->es);
}

TEST(AugustTest, ForwardClosedFormAtOriginEquatorPrimeMeridianAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=august +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Origin (0, 0) maps to (0, 0).
  const PJ_COORD origin =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-15);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-15);

  // 2. North and south poles (lam = 0, phi = +-pi/2): t = +-1, c1 = 0, y1 =
  // +-1,
  //    so x = 0 and y = +-2 * M.
  const PJ_COORD north_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, north_pole.xy.x, 1e-12);
  EXPECT_NEAR(2.0 * kAugustM, north_pole.xy.y, 1e-12);

  const PJ_COORD south_pole =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, south_pole.xy.x, 1e-12);
  EXPECT_NEAR(-2.0 * kAugustM, south_pole.xy.y, 1e-12);

  // 3. Equator at lam = +-pi (antimeridian): x1 = +-1, y1 = 0 -> x = +-4 * M.
  const PJ_COORD antimeridian_east =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kPi, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(4.0 * kAugustM, antimeridian_east.xy.x, 1e-12);
  EXPECT_NEAR(0.0, antimeridian_east.xy.y, 1e-12);

  // 4. Prime meridian at phi = 60 deg.
  const PJ_XY expected_pm = ExpectedAugustUnitSphere(0.0, DegToRad(60.0));
  const PJ_COORD pm =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(60.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_pm.x, pm.xy.x, 1e-12);
  EXPECT_NEAR(expected_pm.y, pm.xy.y, 1e-12);
}

TEST(AugustTest, ForwardQuadrantSymmetryAndEllipsoidScaling) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj unit_pj(proj_create(ctx.get(), "+proj=august +R=1"));
  ASSERT_NE(nullptr, unit_pj.get());
  ScopedPj wgs84_pj(
      proj_create(ctx.get(), "+proj=august +ellps=WGS84 +x_0=1000 +y_0=-2000"));
  ASSERT_NE(nullptr, wgs84_pj.get());

  struct AnglePair {
    double lon_deg;
    double lat_deg;
  };
  constexpr AnglePair kAngles[] = {
      {.lon_deg = 30.0, .lat_deg = 25.0},
      {.lon_deg = 75.0, .lat_deg = 50.0},
      {.lon_deg = 135.0, .lat_deg = 70.0},
  };

  constexpr double kWgs84A = 6378137.0;
  for (const auto& tc : kAngles) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const PJ_XY expected = ExpectedAugustUnitSphere(lam, phi);

    const PJ_XY q1 = pj_fwd({.lam = lam, .phi = phi}, unit_pj.get());
    const PJ_XY q2 = pj_fwd({.lam = -lam, .phi = phi}, unit_pj.get());
    const PJ_XY q3 = pj_fwd({.lam = -lam, .phi = -phi}, unit_pj.get());
    const PJ_XY q4 = pj_fwd({.lam = lam, .phi = -phi}, unit_pj.get());
    EXPECT_EQ(0, proj_errno(unit_pj.get()));

    EXPECT_NEAR(expected.x, q1.x, 1e-12);
    EXPECT_NEAR(expected.y, q1.y, 1e-12);
    EXPECT_NEAR(-q1.x, q2.x, 1e-12);
    EXPECT_NEAR(q1.y, q2.y, 1e-12);
    EXPECT_NEAR(-q1.x, q3.x, 1e-12);
    EXPECT_NEAR(-q1.y, q3.y, 1e-12);
    EXPECT_NEAR(q1.x, q4.x, 1e-12);
    EXPECT_NEAR(-q1.y, q4.y, 1e-12);

    const PJ_COORD wgs84_out =
        proj_trans(wgs84_pj.get(), PJ_FWD, proj_coord(lam, phi, 123.0, 456.0));
    EXPECT_EQ(0, proj_errno(wgs84_pj.get()));
    EXPECT_NEAR(expected.x * kWgs84A + 1000.0, wgs84_out.xyzt.x, 1e-6);
    EXPECT_NEAR(expected.y * kWgs84A - 2000.0, wgs84_out.xyzt.y, 1e-6);
    EXPECT_NEAR(123.0, wgs84_out.xyzt.z, 1e-12);
    EXPECT_NEAR(456.0, wgs84_out.xyzt.t, 1e-12);
  }
}

TEST(AugustTest, ConformalScaleFactorsInverseUnavailableAndBatchTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=august +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. August Epicycloidal is conformal on the sphere: meridional and parallel
  //    scales match and angular distortion vanishes.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(25.0), DegToRad(35.0), 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(factors.meridional_scale, factors.parallel_scale, 1e-5);
  EXPECT_NEAR(factors.meridional_scale * factors.parallel_scale,
              factors.areal_scale, 1e-5);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-5);
  EXPECT_NEAR(kHalfPi, factors.meridian_parallel_angle, 1e-5);

  // 2. Inverse is not implemented and sets PROJ_ERR_OTHER_NO_INVERSE_OP.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.5, 0.5, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.lp.lam);
  EXPECT_EQ(HUGE_VAL, inv.lp.phi);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_LP inv_lp = pj_inv({.x = 0.5, .y = 0.5}, pj.get());
  EXPECT_EQ(HUGE_VAL, inv_lp.lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 3. Batch forward transform via proj_trans_array.
  auto coords = std::to_array<PJ_COORD>({
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(DegToRad(45.0), DegToRad(30.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  const PJ_XY expected =
      ExpectedAugustUnitSphere(DegToRad(45.0), DegToRad(30.0));
  EXPECT_NEAR(0.0, coords[0].xy.x, 1e-12);
  EXPECT_NEAR(expected.x, coords[1].xy.x, 1e-12);
  EXPECT_NEAR(expected.y, coords[1].xy.y, 1e-12);
}

}  // namespace
}  // namespace proj
