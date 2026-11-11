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
// Unit tests for the Larrivee projection (proj/src/projections/larr.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/larr.cpp

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
constexpr double kSixth = 0.16666666666666666;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

PJ_XY ExpectedLarrUnitSphere(double lam, double phi) {
  return {
      .x = 0.5 * lam * (1.0 + std::sqrt(std::cos(phi))),
      .y = phi / (std::cos(0.5 * phi) * std::cos(kSixth * lam)),
  };
}

C_NAMESPACE PJ* pj_larr(PJ*);

TEST(LarrTest, MetadataAndConstructorForceSphericalAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=larr +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("larr", info.id);
  EXPECT_THAT(std::string_view(pj->descr), HasSubstr("Larrivee"));
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  // Exercise the PJ_PROJECTION(larr) constructor entry point directly.
  ScopedPj raw(pj_larr(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("larr", raw->short_name);
  EXPECT_EQ(1, raw->need_ellps);
  EXPECT_EQ(raw.get(), pj_larr(raw.get()));
  EXPECT_EQ(nullptr, raw->inv);
  EXPECT_NE(nullptr, raw->fwd);
  EXPECT_EQ(0.0, raw->es);
}

TEST(LarrTest, ForwardClosedFormAtOriginEquatorPrimeMeridianAndPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=larr +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Origin (0, 0) -> (0, 0).
  const PJ_COORD origin =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-15);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-15);

  // 2. Equator (lam, 0) -> (lam, 0).
  for (const double lon_deg : {-180.0, -90.0, 30.0, 120.0, 180.0}) {
    const double lam = DegToRad(lon_deg);
    const PJ_XY xy = pj_fwd({.lam = lam, .phi = 0.0}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, xy.x, 1e-12);
    EXPECT_NEAR(0.0, xy.y, 1e-12);
  }

  // 3. Prime meridian (0, phi) -> (0, phi / cos(phi / 2)).
  for (const double lat_deg : {-90.0, -60.0, 45.0, 90.0}) {
    const double phi = DegToRad(lat_deg);
    const PJ_XY xy = pj_fwd({.lam = 0.0, .phi = phi}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, xy.x, 1e-12);
    EXPECT_NEAR(phi / std::cos(0.5 * phi), xy.y, 1e-12);
  }

  // 4. North pole at antimeridian (lam = pi, phi = pi/2):
  //    In IEEE-754 double precision, cos(pi/2) ~ 6.12e-17, so sqrt(cos(pi/2))
  //    is ~7.83e-9 and x is within ~1.23e-8 of pi/2.
  const PJ_XY pole_corner = pj_fwd({.lam = kPi, .phi = kHalfPi}, pj.get());
  const PJ_XY expected_corner = ExpectedLarrUnitSphere(kPi, kHalfPi);
  EXPECT_NEAR(expected_corner.x, pole_corner.x, 1e-12);
  EXPECT_NEAR(0.5 * kPi, pole_corner.x, 1e-7);
  EXPECT_NEAR(expected_corner.y, pole_corner.y, 1e-12);
}

TEST(LarrTest, QuadrantSymmetryEllipsoidScalingScaleFactorsAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj unit_pj(proj_create(ctx.get(), "+proj=larr +R=1"));
  ASSERT_NE(nullptr, unit_pj.get());
  ScopedPj wgs84_pj(
      proj_create(ctx.get(), "+proj=larr +ellps=WGS84 +x_0=3000 +y_0=-4000"));
  ASSERT_NE(nullptr, wgs84_pj.get());

  // 1. Quadrant symmetry and WGS84 major-axis scaling.
  constexpr double kWgs84A = 6378137.0;
  const double lam = DegToRad(55.0);
  const double phi = DegToRad(40.0);
  const PJ_XY expected = ExpectedLarrUnitSphere(lam, phi);

  const PJ_XY q1 = pj_fwd({.lam = lam, .phi = phi}, unit_pj.get());
  const PJ_XY q2 = pj_fwd({.lam = -lam, .phi = phi}, unit_pj.get());
  const PJ_XY q3 = pj_fwd({.lam = -lam, .phi = -phi}, unit_pj.get());
  const PJ_XY q4 = pj_fwd({.lam = lam, .phi = -phi}, unit_pj.get());
  EXPECT_NEAR(expected.x, q1.x, 1e-12);
  EXPECT_NEAR(expected.y, q1.y, 1e-12);
  EXPECT_NEAR(-expected.x, q2.x, 1e-12);
  EXPECT_NEAR(expected.y, q2.y, 1e-12);
  EXPECT_NEAR(-expected.x, q3.x, 1e-12);
  EXPECT_NEAR(-expected.y, q3.y, 1e-12);
  EXPECT_NEAR(expected.x, q4.x, 1e-12);
  EXPECT_NEAR(-expected.y, q4.y, 1e-12);

  const PJ_COORD wgs84_out =
      proj_trans(wgs84_pj.get(), PJ_FWD, proj_coord(lam, phi, 88.0, 2026.0));
  EXPECT_EQ(0, proj_errno(wgs84_pj.get()));
  EXPECT_NEAR(expected.x * kWgs84A + 3000.0, wgs84_out.xyzt.x, 1e-6);
  EXPECT_NEAR(expected.y * kWgs84A - 4000.0, wgs84_out.xyzt.y, 1e-6);
  EXPECT_NEAR(88.0, wgs84_out.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.0, wgs84_out.xyzt.t, 1e-12);

  // 2. At the origin (0, 0), Larrivee is locally conformal and isometric:
  //    dx/dlam = 1, dy/dphi = 1.
  const PJ_FACTORS origin_factors =
      proj_factors(unit_pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(unit_pj.get()));
  EXPECT_NEAR(1.0, origin_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, origin_factors.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, origin_factors.areal_scale, 1e-6);
  EXPECT_NEAR(0.0, origin_factors.angular_distortion, 1e-6);
  EXPECT_NEAR(kHalfPi, origin_factors.meridian_parallel_angle, 1e-6);

  // 3. Inverse is unavailable and sets PROJ_ERR_OTHER_NO_INVERSE_OP.
  const PJ_COORD inv =
      proj_trans(unit_pj.get(), PJ_INV, proj_coord(0.5, 0.5, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.lp.lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(unit_pj.get()));
  proj_errno_reset(unit_pj.get());

  // 4. Batch transform via proj_trans_array.
  auto coords = std::to_array<PJ_COORD>({
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(lam, phi, 0.0, 0.0),
  });
  ASSERT_EQ(
      0, proj_trans_array(unit_pj.get(), PJ_FWD, coords.size(), coords.data()));
  EXPECT_NEAR(expected.x, coords[1].xy.x, 1e-12);
  EXPECT_NEAR(expected.y, coords[1].xy.y, 1e-12);
}

}  // namespace
}  // namespace proj
