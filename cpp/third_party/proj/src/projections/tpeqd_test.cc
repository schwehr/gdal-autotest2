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
// Unit tests for the Two Point Equidistant (tpeqd) projection
// (proj/src/projections/tpeqd.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/tpeqd.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(TpeqdTest, RejectsIdenticalControlPointsWithIllegalArgErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  // 1. Default parameters (all 0) have phi_1 == phi_2 && lam_1 == lam_2.
  ScopedPj default_pj(proj_create(ctx.get(), "+proj=tpeqd"));
  EXPECT_EQ(nullptr, default_pj.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(capture.ContainsMessage("the 2 points should be distinct"));

  // 2. Non-zero identical control points also fail with the same error.
  capture.Clear();
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj identical_pj(proj_create(
      ctx.get(), "+proj=tpeqd +lat_1=35 +lon_1=-100 +lat_2=35 +lon_2=-100"));
  EXPECT_EQ(nullptr, identical_pj.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(capture.ContainsMessage("the 2 points should be distinct"));
}

TEST(TpeqdTest, RejectsZeroCentralAngleZ02AtPolesOrWrappedLongitude) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  // When lat_1 == lat_2 and lon_1 != lon_2 differs by a tiny sub-underflow
  // angle (or a full 2*pi revolution), the initial equality check
  // (phi_1 == phi_2 && lam_1 == lam_2) is false, but the great-circle central
  // angle Q->z02 evaluates to 0.0, triggering the Q->z02 == 0.0 error branch.
  ScopedPj zero_z02_pj(proj_create(
      ctx.get(), "+proj=tpeqd +lat_1=90 +lon_1=0 +lat_2=90 +lon_2=1e-200"));
  EXPECT_EQ(nullptr, zero_z02_pj.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(capture.ContainsMessage("their absolute value should be < 90"));
}

TEST(TpeqdTest, ControlPointsMapToSymmetricLocationsOnXAxis) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Unit sphere with P1 = (0N, -30E) and P2 = (0N, +30E).
  // The central distance z02 is 60 deg = pi/3, so P1 maps to (-z02/2, 0) =
  // (-pi/6, 0) and P2 maps to (+pi/6, 0).
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=tpeqd +lat_1=0 +lon_1=-30 +lat_2=0 +lon_2=30 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, pj->es);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("tpeqd", info.id);
  EXPECT_NE(std::string_view(pj->descr).find("Two Point Equidistant"),
            std::string_view::npos);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  const PJ_COORD p1_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(DegToRad(-30.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(-kPi / 6.0, p1_fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.0, p1_fwd.xy.y, 1e-12);

  const PJ_COORD p2_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(DegToRad(30.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kPi / 6.0, p2_fwd.xy.x, 1e-12);
  EXPECT_NEAR(0.0, p2_fwd.xy.y, 1e-12);
}

TEST(TpeqdTest, ForwardAndInverseRoundTripBothSidesOfBaseGreatCircle) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Oblique two-point equidistant between New York (40.7128N, 74.0060W) and
  // London (51.5074N, 0.1278W) on WGS84 (forced spherical by tpeqd).
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=tpeqd +lat_1=40.7128 +lon_1=-74.0060 "
                          "+lat_2=51.5074 +lon_2=-0.1278 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  struct PointCase {
    double lon_deg;
    double lat_deg;
    bool expect_negative_y;
  };
  // Points north of the New York -> London great circle have y > 0; points
  // south of it have y < 0, exercising both branches of the y-sign flip in
  // tpeqd_s_forward and tpeqd_s_inverse.
  constexpr PointCase kCases[] = {
      {.lon_deg = -40.0, .lat_deg = 65.0, .expect_negative_y = false},
      {.lon_deg = -20.0, .lat_deg = 60.0, .expect_negative_y = false},
      {.lon_deg = -40.0, .lat_deg = 25.0, .expect_negative_y = true},
      {.lon_deg = -15.0, .lat_deg = 30.0, .expect_negative_y = true},
      {.lon_deg = -74.0060, .lat_deg = 40.7128, .expect_negative_y = false},
      {.lon_deg = -0.1278, .lat_deg = 51.5074, .expect_negative_y = false},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    // 1. Forward projection.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_TRUE(std::isfinite(fwd.xy.x));
    EXPECT_TRUE(std::isfinite(fwd.xy.y));
    if (std::abs(fwd.xy.y) > 1.0) {
      if (tc.expect_negative_y) {
        EXPECT_LT(fwd.xy.y, 0.0);
      } else {
        EXPECT_GT(fwd.xy.y, 0.0);
      }
    }

    // 2. Inverse projection round-trip.
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-10);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-10);
  }
}

TEST(TpeqdTest, Classic2dEntryPointsArrayTransformAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(), "+proj=tpeqd +lat_1=10 +lon_1=10 +lat_2=40 +lon_2=50 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(25.0), .phi = DegToRad(30.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP inv_lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp.lam, inv_lp.lam, 1e-11);
  EXPECT_NEAR(lp.phi, inv_lp.phi, 1e-11);

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(20.0), DegToRad(35.0), 0.0, 0.0),
      proj_coord(DegToRad(35.0), DegToRad(15.0), 0.0, 0.0),
  });
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(20.0), coords[0].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(35.0), coords[0].lp.phi, 1e-11);
  EXPECT_NEAR(DegToRad(35.0), coords[1].lp.lam, 1e-11);
  EXPECT_NEAR(DegToRad(15.0), coords[1].lp.phi, 1e-11);

  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(30.0), DegToRad(25.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(factors.meridional_scale, 0.5);
  EXPECT_GT(factors.parallel_scale, 0.5);
  EXPECT_GT(factors.areal_scale, 0.5);
}

}  // namespace
}  // namespace proj
