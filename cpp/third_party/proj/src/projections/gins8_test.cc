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
// Unit tests for the Ginsburg VIII (TsNIIGAiK) pseudocylindrical projection
// (proj/src/projections/gins8.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/gins8.cpp

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
constexpr double kGins8Cl = 0.000952426;
constexpr double kGins8Cp = 0.162388;
constexpr double kGins8C12 = 0.08333333333333333;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

constexpr PJ_XY ExpectedGins8UnitSphere(double lam, double phi) {
  const double phi2 = phi * phi;
  const double lam2 = lam * lam;
  return {
      .x = lam * (1.0 - kGins8Cp * phi2) * (0.87 - kGins8Cl * lam2 * lam2),
      .y = phi * (1.0 + phi2 * kGins8C12),
  };
}

C_NAMESPACE PJ* pj_gins8(PJ*);

TEST(Gins8Test, MetadataAndConstructorForceSphericalAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=gins8 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("gins8", info.id);
  EXPECT_THAT(std::string_view(pj->descr),
              HasSubstr("Ginsburg VIII (TsNIIGAiK)"));
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  // Exercise the PJ_PROJECTION(gins8) constructor entry point directly.
  ScopedPj raw(pj_gins8(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("gins8", raw->short_name);
  EXPECT_EQ(1, raw->need_ellps);
  EXPECT_EQ(raw.get(), pj_gins8(raw.get()));
  EXPECT_EQ(nullptr, raw->inv);
  EXPECT_NE(nullptr, raw->fwd);
  EXPECT_EQ(0.0, raw->es);
}

TEST(Gins8Test, ForwardPolynomialFormulaPseudocylindricalParallelsAndSymmetry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj unit_pj(proj_create(ctx.get(), "+proj=gins8 +R=1"));
  ASSERT_NE(nullptr, unit_pj.get());

  // 1. Origin (0, 0) -> (0, 0).
  const PJ_COORD origin =
      proj_trans(unit_pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(unit_pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-15);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-15);

  // 2. Pseudocylindrical property: northing y depends only on latitude phi,
  //    not on longitude lam.
  const double phi = DegToRad(40.0);
  const double expected_y = phi * (1.0 + phi * phi * kGins8C12);
  for (const double lon_deg : {-180.0, -90.0, -30.0, 0.0, 45.0, 120.0, 180.0}) {
    const double lam = DegToRad(lon_deg);
    const PJ_XY expected = ExpectedGins8UnitSphere(lam, phi);
    const PJ_XY actual = pj_fwd({.lam = lam, .phi = phi}, unit_pj.get());
    EXPECT_EQ(0, proj_errno(unit_pj.get()));
    EXPECT_NEAR(expected.x, actual.x, 1e-12);
    EXPECT_NEAR(expected_y, actual.y, 1e-12);
  }

  // 3. Quadrant symmetry across (+-lam, +-phi) including poles.
  for (const double lat_deg : {25.0, 60.0, 90.0}) {
    const double test_lam = DegToRad(75.0);
    const double test_phi = DegToRad(lat_deg);
    const PJ_XY expected = ExpectedGins8UnitSphere(test_lam, test_phi);

    const PJ_XY q1 = pj_fwd({.lam = test_lam, .phi = test_phi}, unit_pj.get());
    const PJ_XY q2 = pj_fwd({.lam = -test_lam, .phi = test_phi}, unit_pj.get());
    const PJ_XY q3 =
        pj_fwd({.lam = -test_lam, .phi = -test_phi}, unit_pj.get());
    const PJ_XY q4 = pj_fwd({.lam = test_lam, .phi = -test_phi}, unit_pj.get());

    EXPECT_NEAR(expected.x, q1.x, 1e-12);
    EXPECT_NEAR(expected.y, q1.y, 1e-12);
    EXPECT_NEAR(-expected.x, q2.x, 1e-12);
    EXPECT_NEAR(expected.y, q2.y, 1e-12);
    EXPECT_NEAR(-expected.x, q3.x, 1e-12);
    EXPECT_NEAR(-expected.y, q3.y, 1e-12);
    EXPECT_NEAR(expected.x, q4.x, 1e-12);
    EXPECT_NEAR(-expected.y, q4.y, 1e-12);
  }
}

TEST(Gins8Test, EllipsoidMajorAxisScalingScaleFactorsAndInverseUnavailable) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj unit_pj(proj_create(ctx.get(), "+proj=gins8 +R=1"));
  ASSERT_NE(nullptr, unit_pj.get());
  ScopedPj wgs84_pj(
      proj_create(ctx.get(), "+proj=gins8 +ellps=WGS84 +x_0=10000 +y_0=-5000"));
  ASSERT_NE(nullptr, wgs84_pj.get());

  // 1. WGS84 major axis scaling and 4D preservation.
  constexpr double kWgs84A = 6378137.0;
  const double lam = DegToRad(50.0);
  const double phi = DegToRad(-35.0);
  const PJ_XY expected = ExpectedGins8UnitSphere(lam, phi);
  const PJ_COORD wgs84_out =
      proj_trans(wgs84_pj.get(), PJ_FWD, proj_coord(lam, phi, 250.0, 1999.5));
  EXPECT_EQ(0, proj_errno(wgs84_pj.get()));
  EXPECT_NEAR(expected.x * kWgs84A + 10000.0, wgs84_out.xyzt.x, 1e-6);
  EXPECT_NEAR(expected.y * kWgs84A - 5000.0, wgs84_out.xyzt.y, 1e-6);
  EXPECT_NEAR(250.0, wgs84_out.xyzt.z, 1e-12);
  EXPECT_NEAR(1999.5, wgs84_out.xyzt.t, 1e-12);

  // 2. Scale factors at the origin: dx/dlam = 0.87, dy/dphi = 1.0.
  const PJ_FACTORS origin_factors =
      proj_factors(unit_pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(unit_pj.get()));
  EXPECT_NEAR(1.0, origin_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(0.87, origin_factors.parallel_scale, 1e-6);
  EXPECT_NEAR(0.87, origin_factors.areal_scale, 1e-6);
  EXPECT_NEAR(kHalfPi, origin_factors.meridian_parallel_angle, 1e-6);

  // 3. Inverse is unavailable and sets PROJ_ERR_OTHER_NO_INVERSE_OP.
  const PJ_COORD inv =
      proj_trans(unit_pj.get(), PJ_INV, proj_coord(0.4, 0.4, 0.0, 0.0));
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
