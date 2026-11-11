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
// Unit tests for the Urmaev V pseudocylindrical projection
// (proj/src/projections/urm5.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/urm5.cpp

#include <array>
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

TEST(Urm5Test, ParameterValidationRejectsMissingOrOutOfRangeNAndAlpha) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get(), PJ_LOG_ERROR);

  // 1. Missing required +n parameter.
  ScopedPj missing_n(proj_create(ctx.get(), "+proj=urm5 +q=0.4 +alpha=10"));
  EXPECT_EQ(nullptr, missing_n.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Missing parameter n."));
  logs.Clear();

  // 2. Invalid +n <= 0 or +n > 1 (must be in ]0, 1]).
  for (const char* def : {
           "+proj=urm5 +n=0",
           "+proj=urm5 +n=-0.5",
           "+proj=urm5 +n=1.0001",
       }) {
    ScopedPj bad_n(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, bad_n.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage(
        "Invalid value for n: it should be in ]0,1] range."));
    logs.Clear();
  }

  // 3. Invalid combination +n=1 +alpha=+-90 where n * sin(|alpha|) == 1, making
  //    denom = sqrt(1 - t^2) == 0.
  for (const char* def : {
           "+proj=urm5 +n=1 +alpha=90",
           "+proj=urm5 +n=1 +alpha=-90",
       }) {
    ScopedPj zero_denom(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, zero_denom.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(logs.ContainsMessage(
        "Invalid value for n / alpha: n * sin(|alpha|) should be < 1."));
    logs.Clear();
  }
}

TEST(Urm5Test, MetadataAndForwardMatchesClosedFormFormula) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kN = 0.8;
  constexpr double kQ = 0.414524;
  constexpr double kAlphaDeg = 30.0;
  constexpr double kAlphaRad = DegToRad(kAlphaDeg);

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=urm5 +n=0.8 +q=0.414524 +alpha=30 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0.0, pj->es);
  EXPECT_EQ(nullptr, pj->inv);

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("urm5", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_STREQ("Urmaev V\n\tPCyl, Sph, no inv\n\tn= q= alpha=", pj->descr);
  EXPECT_EQ(0, info.has_inverse);

  // Verify closed-form forward equations across multiple points:
  //   t = n * sin(alpha), m = cos(alpha) / sqrt(1 - t^2), rmn = 1 / (m * n)
  //   psi = asin(n * sin(phi)), x = m * lam * cos(psi),
  //   y = psi * (1 + psi^2 * (q / 3)) * rmn.
  const double t_alpha = kN * std::sin(kAlphaRad);
  const double m = std::cos(kAlphaRad) / std::sqrt(1.0 - t_alpha * t_alpha);
  const double rmn = 1.0 / (m * kN);
  const double q3 = kQ / 3.0;

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},   {.lon_deg = 45.0, .lat_deg = 0.0},
      {.lon_deg = 30.0, .lat_deg = 60.0}, {.lon_deg = -120.0, .lat_deg = -45.0},
      {.lon_deg = 90.0, .lat_deg = 90.0}, {.lon_deg = -90.0, .lat_deg = -90.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);
    const double psi = std::asin(kN * std::sin(phi));
    const double expected_x = m * lam * std::cos(psi);
    const double expected_y = psi * (1.0 + psi * psi * q3) * rmn;

    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);
  }
}

TEST(Urm5Test, BoundaryNOneDefaultQAndAlphaClassic2DFwdAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Boundary n = 1.0 with default q = 0 and alpha = 0 reduces to m = 1, rmn =
  // 1, x = lam * cos(phi), y = phi.
  ScopedPj pj(proj_create(ctx.get(), "+proj=urm5 +n=1 +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP lp = {.lam = DegToRad(60.0), .phi = DegToRad(60.0)};
  const PJ_XY xy = pj_fwd(lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.5 * lp.lam, xy.x, 1e-12);
  EXPECT_NEAR(lp.phi, xy.y, 1e-12);

  std::array<PJ_COORD, 2> coords = {
      proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(-45.0), DegToRad(-60.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(15.0) * std::cos(DegToRad(30.0)), coords[0].xy.x, 1e-12);
  EXPECT_NEAR(DegToRad(30.0), coords[0].xy.y, 1e-12);

  const double lam_f = DegToRad(10.0);
  const double phi_f = DegToRad(20.0);
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(lam_f, phi_f, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  // For x = lam * cos(phi), y = phi (Sinusoidal), dx/dphi = -lam * sin(phi) and
  // dy/dphi = 1, so meridional scale h = hypot(1, lam * sin(phi)), while
  // parallel scale k = 1 and areal scale s = 1.
  const double expected_h = std::hypot(1.0, lam_f * std::sin(phi_f));
  EXPECT_NEAR(expected_h, factors.meridional_scale, 1e-5);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-5);
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
