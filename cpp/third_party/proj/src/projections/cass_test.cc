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
// Unit tests for the Cassini-Soldner and Hyperbolic Cassini-Soldner
// projections (proj/src/projections/cass.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/cass.cpp

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

TEST(CassTest, SphericalForwardAndInverseExactFormulasAndRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kRadius = 6371000.0;
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=cass +R=6371000 +lat_0=10 +lon_0=5"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // Spherical formulas:
  //   x = R * asin(cos(phi) * sin(lam))
  //   y = R * (atan2(tan(phi), cos(lam)) - phi0)
  const double lon_rad = DegToRad(12.0);
  const double lat_rad = DegToRad(35.0);
  const double dlam = lon_rad - DegToRad(5.0);
  const double expected_x =
      kRadius * std::asin(std::cos(lat_rad) * std::sin(dlam));
  const double expected_y =
      kRadius *
      (std::atan2(std::tan(lat_rad), std::cos(dlam)) - DegToRad(10.0));

  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 40.0, 2026.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(expected_x, fwd.xy.x, 1e-5);
  EXPECT_NEAR(expected_y, fwd.xy.y, 1e-5);
  EXPECT_DOUBLE_EQ(40.0, fwd.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.0, fwd.xyzt.t);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-12);
  EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-12);
}

TEST(CassTest, EllipsoidalStandardAndHyperbolicForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Standard ellipsoidal Cassini-Soldner and 2. Hyperbolic Cassini-Soldner
  // (+hyperbolic, used for Vanua Levu Grid).
  ScopedPj standard_pj(proj_create(
      ctx.get(), "+proj=cass +ellps=WGS84 +lat_0=-16.25 +lon_0=179.3333333"));
  ASSERT_NE(nullptr, standard_pj.get());

  ScopedPj hyp_pj(
      proj_create(ctx.get(),
                  "+proj=cass +ellps=WGS84 +lat_0=-16.25 +lon_0=179.3333333 "
                  "+hyperbolic"));
  ASSERT_NE(nullptr, hyp_pj.get());

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      {.lon_deg = 179.3333333, .lat_deg = -16.25},
      {.lon_deg = 179.5, .lat_deg = -16.0},
      {.lon_deg = 178.8, .lat_deg = -17.0},
      {.lon_deg = 180.0, .lat_deg = -15.5},
  };

  for (const auto& tc : kCases) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);

    const PJ_COORD std_fwd = proj_trans(standard_pj.get(), PJ_FWD, in);
    const PJ_COORD hyp_fwd = proj_trans(hyp_pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(standard_pj.get()));
    EXPECT_EQ(0, proj_errno(hyp_pj.get()));
    // Eastings are identical; hyperbolic northing subtracts y^3 / (6*rho*nu)
    // when y != 0.
    EXPECT_DOUBLE_EQ(std_fwd.xy.x, hyp_fwd.xy.x);
    if (std::abs(std_fwd.xy.y) > 1.0) {
      EXPECT_NE(std_fwd.xy.y, hyp_fwd.xy.y);
    }

    const PJ_COORD std_inv = proj_trans(standard_pj.get(), PJ_INV, std_fwd);
    const PJ_COORD hyp_inv = proj_trans(hyp_pj.get(), PJ_INV, hyp_fwd);
    EXPECT_EQ(0, proj_errno(standard_pj.get()));
    EXPECT_EQ(0, proj_errno(hyp_pj.get()));
    EXPECT_NEAR(in.lp.lam, std_inv.lp.lam, 1e-11);
    EXPECT_NEAR(in.lp.phi, std_inv.lp.phi, 1e-11);
    EXPECT_NEAR(in.lp.lam, hyp_inv.lp.lam, 1e-11);
    EXPECT_NEAR(in.lp.phi, hyp_inv.lp.phi, 1e-11);
  }
}

TEST(CassTest, CentralMeridianFactorsAndDestructorNullBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=cass +ellps=WGS84 +lat_0=52 +lon_0=10"));
  ASSERT_NE(nullptr, pj.get());

  // Along the central meridian (lon == lon_0), Cassini-Soldner has unit
  // meridional and parallel scale.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(10.0), DegToRad(53.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);

  // Exercise destructor null-pointer and null-opaque branches without leaking
  // Q->en.
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  struct CassOpaqueLayout {
    double* en;
    double m0;
    bool hyperbolic;
  };
  auto* q = static_cast<CassOpaqueLayout*>(pj->opaque);
  ASSERT_NE(nullptr, q);
  std::free(q->en);
  std::free(q);
  pj->opaque = nullptr;
}

}  // namespace
}  // namespace proj
