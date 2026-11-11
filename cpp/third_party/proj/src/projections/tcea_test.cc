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
// Unit tests for the Transverse Cylindrical Equal Area projection
// (proj/src/projections/tcea.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/tcea.cpp

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

C_NAMESPACE PJ* pj_tcea(PJ*);

TEST(TceaTest, MetadataAndConstructorForceSphericalWithInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=tcea +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("tcea", info.id);
  EXPECT_THAT(std::string_view(pj->descr),
              HasSubstr("Transverse Cylindrical Equal Area"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(0.0, pj->es);

  // Exercise the PJ_PROJECTION(tcea) constructor entry point directly.
  ScopedPj raw(pj_tcea(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("tcea", raw->short_name);
  EXPECT_EQ(1, raw->need_ellps);
  EXPECT_EQ(raw.get(), pj_tcea(raw.get()));
  EXPECT_NE(nullptr, raw->fwd);
  EXPECT_NE(nullptr, raw->inv);
  EXPECT_EQ(0.0, raw->es);
}

TEST(TceaTest, ForwardMatchesClosedFormWithCustomScaleAndOrigin) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kK0 = 0.8;
  constexpr double kPhi0 = DegToRad(15.0);
  ScopedPj pj(proj_create(ctx.get(), "+proj=tcea +R=1 +k_0=0.8 +lat_0=15"));
  ASSERT_NE(nullptr, pj.get());

  struct AngleCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr AngleCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 15.0},   {.lon_deg = 25.0, .lat_deg = 40.0},
      {.lon_deg = -60.0, .lat_deg = 35.0}, {.lon_deg = -45.0, .lat_deg = -50.0},
      {.lon_deg = 80.0, .lat_deg = -20.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const double expected_x = std::cos(phi) * std::sin(lam) / kK0;
    const double expected_y =
        kK0 * (std::atan2(std::tan(phi), std::cos(lam)) - kPhi0);

    // 1. Forward check against analytical formula.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 50.0, 2026.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xyzt.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xyzt.y, 1e-12);
    EXPECT_NEAR(50.0, fwd.xyzt.z, 1e-12);
    EXPECT_NEAR(2026.0, fwd.xyzt.t, 1e-12);

    // 2. Inverse round-trip check.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lpzt.lam, 1e-12);
    EXPECT_NEAR(phi, inv.lpzt.phi, 1e-12);
    EXPECT_NEAR(50.0, inv.lpzt.z, 1e-12);
    EXPECT_NEAR(2026.0, inv.lpzt.t, 1e-12);
  }
}

TEST(TceaTest, Classic2DForwardAndInverseRoundTripAndBoundaryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(
      proj_create(ctx.get(),
                  "+proj=tcea +ellps=WGS84 +lon_0=10 +lat_0=-20 +k_0=0.9996 "
                  "+x_0=500000 +y_0=100000"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP in_lp = {.lam = DegToRad(32.0), .phi = DegToRad(-5.0)};
  const PJ_XY xy = pj_fwd(in_lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(xy.x));

  const PJ_LP out_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-12);
  EXPECT_NEAR(in_lp.phi, out_lp.phi, 1e-12);

  // Boundary at (lam = +-pi/2, phi = 0) on unit sphere: x = +-1, t = 0.
  ScopedPj unit_pj(proj_create(ctx.get(), "+proj=tcea +R=1"));
  ASSERT_NE(nullptr, unit_pj.get());
  const PJ_XY east_horizon =
      pj_fwd({.lam = kHalfPi, .phi = 0.0}, unit_pj.get());
  EXPECT_NEAR(1.0, east_horizon.x, 1e-12);
  EXPECT_NEAR(0.0, east_horizon.y, 1e-12);
  const PJ_LP inv_horizon = pj_inv(east_horizon, unit_pj.get());
  EXPECT_NEAR(kHalfPi, inv_horizon.lam, 1e-12);
  EXPECT_NEAR(0.0, inv_horizon.phi, 1e-12);
}

TEST(TceaTest, EqualAreaScaleFactorsBatchTransformAndInvalidCoords) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=tcea +R=1 +k_0=0.95"));
  ASSERT_NE(nullptr, pj.get());

  // 1. On the central meridian (lam = 0), meridional_scale = k0 and
  //    parallel_scale = 1 / k0; areal_scale = 1 everywhere on the sphere.
  const PJ_FACTORS cm_factors =
      proj_factors(pj.get(), proj_coord(0.0, DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.95, cm_factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0 / 0.95, cm_factors.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, cm_factors.areal_scale, 1e-6);

  const PJ_FACTORS off_factors = proj_factors(
      pj.get(), proj_coord(DegToRad(35.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, off_factors.areal_scale, 1e-5);

  // 2. Batch forward + inverse round-trip via proj_trans_array.
  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(-20.0), DegToRad(15.0), 0.0, 0.0),
      proj_coord(DegToRad(40.0), DegToRad(-30.0), 0.0, 0.0),
  });
  const auto expected = coords;
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(expected[i].lp.lam, coords[i].lp.lam, 1e-12);
    EXPECT_NEAR(expected[i].lp.phi, coords[i].lp.phi, 1e-12);
  }

  // 3. Invalid latitude (> pi/2) fails cleanly.
  ScopedLogSuppressor suppress(ctx.get());
  const PJ_COORD bad_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_fwd.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
