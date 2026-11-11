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
// Unit tests for the Transverse Central Cylindrical projection
// (proj/src/projections/tcc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/tcc.cpp

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

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

PJ_XY ExpectedTccUnitSphere(double lam, double phi) {
  const double b = std::cos(phi) * std::sin(lam);
  const double bt = 1.0 - b * b;
  return {
      .x = b / std::sqrt(bt),
      .y = std::atan2(std::tan(phi), std::cos(lam)),
  };
}

C_NAMESPACE PJ* pj_tcc(PJ*);

TEST(TccTest, MetadataAndConstructorForceSphericalAndNoInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=tcc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("tcc", info.id);
  EXPECT_THAT(std::string_view(pj->descr),
              HasSubstr("Transverse Central Cylindrical"));
  EXPECT_EQ(0, info.has_inverse);
  EXPECT_EQ(0, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  // Exercise the PJ_PROJECTION(tcc) constructor entry point directly.
  ScopedPj raw(pj_tcc(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("tcc", raw->short_name);
  EXPECT_EQ(1, raw->need_ellps);
  EXPECT_EQ(raw.get(), pj_tcc(raw.get()));
  EXPECT_EQ(nullptr, raw->inv);
  EXPECT_NE(nullptr, raw->fwd);
  EXPECT_EQ(0.0, raw->es);
}

TEST(TccTest, ForwardClosedFormAndQuadrantSymmetry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj unit_pj(proj_create(ctx.get(), "+proj=tcc +R=1"));
  ASSERT_NE(nullptr, unit_pj.get());
  ScopedPj wgs84_pj(
      proj_create(ctx.get(), "+proj=tcc +ellps=WGS84 +x_0=2500 +y_0=-1500"));
  ASSERT_NE(nullptr, wgs84_pj.get());

  // 1. Origin (0, 0) -> (0, 0).
  const PJ_COORD origin =
      proj_trans(unit_pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(unit_pj.get()));
  EXPECT_NEAR(0.0, origin.xy.x, 1e-15);
  EXPECT_NEAR(0.0, origin.xy.y, 1e-15);

  // 2. Central meridian (lam = 0, phi) -> (0, phi), including poles.
  for (const double lat_deg : {-90.0, -45.0, 30.0, 90.0}) {
    const double phi = DegToRad(lat_deg);
    const PJ_XY xy = pj_fwd({.lam = 0.0, .phi = phi}, unit_pj.get());
    EXPECT_EQ(0, proj_errno(unit_pj.get()));
    EXPECT_NEAR(0.0, xy.x, 1e-12);
    EXPECT_NEAR(phi, xy.y, 1e-12);
  }

  // 3. Equator (lam, 0) -> (tan(lam), 0) for |lam| < pi/2.
  for (const double lon_deg : {-60.0, -30.0, 20.0, 45.0}) {
    const double lam = DegToRad(lon_deg);
    const PJ_XY xy = pj_fwd({.lam = lam, .phi = 0.0}, unit_pj.get());
    EXPECT_EQ(0, proj_errno(unit_pj.get()));
    EXPECT_NEAR(std::tan(lam), xy.x, 1e-12);
    EXPECT_NEAR(0.0, xy.y, 1e-12);
  }

  // 4. General points across all 4 quadrants and WGS84 scaling.
  constexpr double kWgs84A = 6378137.0;
  const double lam = DegToRad(35.0);
  const double phi = DegToRad(40.0);
  const PJ_XY expected = ExpectedTccUnitSphere(lam, phi);

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
      proj_trans(wgs84_pj.get(), PJ_FWD, proj_coord(lam, phi, 10.0, 20.0));
  EXPECT_EQ(0, proj_errno(wgs84_pj.get()));
  EXPECT_NEAR(expected.x * kWgs84A + 2500.0, wgs84_out.xyzt.x, 1e-6);
  EXPECT_NEAR(expected.y * kWgs84A - 1500.0, wgs84_out.xyzt.y, 1e-6);
  EXPECT_NEAR(10.0, wgs84_out.xyzt.z, 1e-12);
  EXPECT_NEAR(20.0, wgs84_out.xyzt.t, 1e-12);
}

TEST(TccTest, SingularityWhenBtLessThanEps10RejectsPointOutsideDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=tcc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Exact horizon on the equator (lam = +-pi/2, phi = 0) gives b = +-1,
  //    bt = 1 - b^2 = 0 < EPS10 (1e-10).
  for (const double singular_lam :
       {kHalfPi, -kHalfPi, kHalfPi - 1e-6, -kHalfPi + 1e-6}) {
    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD, proj_coord(singular_lam, 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, out.xy.x);
    EXPECT_EQ(HUGE_VAL, out.xy.y);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());

    const PJ_XY xy = pj_fwd({.lam = singular_lam, .phi = 0.0}, pj.get());
    EXPECT_EQ(HUGE_VAL, xy.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // 2. Just inside the domain (lam = pi/2 - 1e-4, phi = 0 -> bt ≈ 1e-8 > 1e-10)
  //    succeeds with a large finite x coordinate.
  const PJ_COORD near_horizon =
      proj_trans(pj.get(), PJ_FWD, proj_coord(kHalfPi - 1e-4, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(near_horizon.xy.x));
  EXPECT_GT(near_horizon.xy.x, 9999.0);
  EXPECT_NEAR(0.0, near_horizon.xy.y, 1e-12);
}

TEST(TccTest, ScaleFactorsInverseUnavailableAndBatchTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=tcc +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Along the central meridian (lam = 0), TCC is tangent to the sphere:
  //    both meridional and parallel scales are 1.0.
  const PJ_FACTORS cm_factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, cm_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, cm_factors.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, cm_factors.areal_scale, 1e-6);

  // 2. Inverse is unavailable and sets PROJ_ERR_OTHER_NO_INVERSE_OP.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(0.2, 0.3, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inv.lp.lam);
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 3. Batch forward transform via proj_trans_array.
  auto coords = std::to_array<PJ_COORD>({
      proj_coord(0.0, 0.0, 0.0, 0.0),
      proj_coord(DegToRad(25.0), DegToRad(35.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  const PJ_XY expected = ExpectedTccUnitSphere(DegToRad(25.0), DegToRad(35.0));
  EXPECT_NEAR(expected.x, coords[1].xy.x, 1e-12);
  EXPECT_NEAR(expected.y, coords[1].xy.y, 1e-12);
}

}  // namespace
}  // namespace proj
