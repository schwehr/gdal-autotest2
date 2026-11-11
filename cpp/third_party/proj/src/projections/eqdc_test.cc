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
// Unit tests for the Equidistant Conic projection
// (proj/src/projections/eqdc.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/eqdc.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

C_NAMESPACE PJ* pj_eqdc(PJ*);

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(EqdcTest, InvalidParametersFailWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. |lat_1| > 90 deg fails validation.
  ScopedPj bad_lat1(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=95 +lat_2=30 +ellps=WGS84"));
  EXPECT_EQ(nullptr, bad_lat1.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_1"));

  // 2. |lat_2| > 90 deg fails validation.
  proj_context_errno_set(ctx.get(), 0);
  logs.Clear();
  ScopedPj bad_lat2(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=30 +lat_2=-95 +ellps=WGS84"));
  EXPECT_EQ(nullptr, bad_lat2.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_2"));

  // 3. |lat_1 + lat_2| < EPS10 (1e-10) fails validation (symmetric parallels).
  proj_context_errno_set(ctx.get(), 0);
  logs.Clear();
  ScopedPj sum_zero(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=30 +lat_2=-30 +ellps=WGS84"));
  EXPECT_EQ(nullptr, sum_zero.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("|lat_1 + lat_2| should be > 0"));

  // 4. Ellipsoidal secant cone with phi1 = ~2.09e-10 rad (> EPS10) and phi2 = 0
  // has m1 == m2 == 1.0 in IEEE-754 double precision while ml1 != ml2, hitting
  // the `if (Q->n == 0)` error branch in the ellipsoidal setup.
  proj_context_errno_set(ctx.get(), 0);
  logs.Clear();
  ScopedPj ellips_n_zero(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=1.2e-8 +lat_2=0 +ellps=WGS84"));
  EXPECT_EQ(nullptr, ellips_n_zero.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for eccentricity"));

  // 5. Spherical secant cone with phi1 = ~2.09e-10 rad and phi2 = 0 has
  // cos(phi1) == cos(phi2) == 1.0 in IEEE-754 double precision, hitting the
  // `if (Q->n == 0)` error branch in the spherical setup.
  proj_context_errno_set(ctx.get(), 0);
  logs.Clear();
  ScopedPj sphere_n_zero(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=1.2e-8 +lat_2=0 +R=6371000"));
  EXPECT_EQ(nullptr, sphere_n_zero.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage("lat_1 + lat_2 should be > 0"));
}

TEST(EqdcTest, EllipsoidalSecantAndTangentForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Secant cone in Northern Hemisphere (Q->n > 0).
  ScopedPj pj_north(proj_create(
      ctx.get(),
      "+proj=eqdc +lat_1=30 +lat_2=50 +lat_0=40 +lon_0=-96 +x_0=100000 "
      "+y_0=200000 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_north.get());
  EXPECT_EQ(1, pj_has_inverse(pj_north.get()));
  EXPECT_STREQ("eqdc", proj_pj_info(pj_north.get()).id);

  const PJ_COORD origin = proj_coord(DegToRad(-96.0), DegToRad(40.0), 0.0, 0.0);
  const PJ_COORD origin_fwd = proj_trans(pj_north.get(), PJ_FWD, origin);
  EXPECT_EQ(0, proj_errno(pj_north.get()));
  EXPECT_NEAR(100000.0, origin_fwd.xy.x, 1e-6);
  EXPECT_NEAR(200000.0, origin_fwd.xy.y, 1e-6);

  // Meridional scale is 1 everywhere on Equidistant Conic, and parallel scale
  // is 1 on both standard parallels (30 N and 50 N).
  for (const double std_lat : {30.0, 50.0}) {
    const PJ_FACTORS f =
        proj_factors(pj_north.get(),
                     proj_coord(DegToRad(-90.0), DegToRad(std_lat), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_north.get()));
    EXPECT_NEAR(1.0, f.meridional_scale, 1e-5);
    EXPECT_NEAR(1.0, f.parallel_scale, 1e-5);
  }

  // 2. Secant cone in Southern Hemisphere (Q->n < 0), exercising the
  // `if (Q->n < 0.)` branch in eqdc_e_inverse.
  ScopedPj pj_south(proj_create(
      ctx.get(), "+proj=eqdc +lat_1=-20 +lat_2=-40 +lon_0=135 +ellps=GRS80"));
  ASSERT_NE(nullptr, pj_south.get());

  // 3. Tangent cone on ellipsoid (lat_1 == lat_2 -> secant == 0).
  ScopedPj pj_tangent(proj_create(
      ctx.get(), "+proj=eqdc +lat_1=45 +lat_2=45 +lon_0=10 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_tangent.get());

  for (PJ* p : {pj_north.get(), pj_south.get(), pj_tangent.get()}) {
    const PJ_LP in_lp = {
        .lam = DegToRad(5.0),
        .phi = (p == pj_south.get()) ? DegToRad(-30.0) : DegToRad(42.0)};
    const PJ_XY xy = pj_fwd(in_lp, p);
    EXPECT_EQ(0, proj_errno(p));
    const PJ_LP out_lp = pj_inv(xy, p);
    EXPECT_EQ(0, proj_errno(p));
    EXPECT_NEAR(in_lp.lam, out_lp.lam, 1e-11);
    EXPECT_NEAR(in_lp.phi, out_lp.phi, 1e-11);
  }
}

TEST(EqdcTest, SphericalSecantTangentAndConeApexInverseZeroRhoBranch) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical secant and tangent round-trip.
  for (const char* def : {"+proj=eqdc +lat_1=25 +lat_2=55 +R=6371000",
                          "+proj=eqdc +lat_1=30 +lat_2=30 +R=1",
                          "+proj=eqdc +lat_1=-30 +lat_2=-30 +R=1"}) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get());
    PJ_COORD coords[2] = {
        proj_coord(DegToRad(15.0), DegToRad(35.0), 0.0, 0.0),
        proj_coord(DegToRad(-25.0), DegToRad(-20.0), 0.0, 0.0),
    };
    const PJ_COORD orig[2] = {coords[0], coords[1]};
    ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, 2, coords));
    ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, 2, coords));
    for (int i = 0; i < 2; ++i) {
      EXPECT_NEAR(orig[i].lp.lam, coords[i].lp.lam, 1e-11);
      EXPECT_NEAR(orig[i].lp.phi, coords[i].lp.phi, 1e-11);
    }
  }

  // 2. Inverse at the exact cone apex (xy.x = 0, xy.y = Q->rho0 -> Q->rho == 0)
  // for both Q->n > 0 (+lat_1=30 +lat_2=30) and Q->n < 0 (+lat_1=-30
  // +lat_2=-30). Calling pj->fwd directly at lp = (0, Q->c) sets Q->rho = Q->c
  // - lp.phi = 0 and returns xy = (0, Q->rho0) exactly.
  ScopedPj pj_pos_n(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=30 +lat_2=30 +R=1"));
  ASSERT_NE(nullptr, pj_pos_n.get());
  const double phi1_pos = pj_param(ctx.get(), pj_pos_n->params, "rlat_1").f;
  const double c_pos = phi1_pos + std::cos(phi1_pos) / std::sin(phi1_pos);
  const PJ_XY apex_xy_pos =
      pj_pos_n->fwd({.lam = 0.0, .phi = c_pos}, pj_pos_n.get());
  const PJ_LP apex_pos = pj_inv(apex_xy_pos, pj_pos_n.get());
  EXPECT_EQ(0, proj_errno(pj_pos_n.get()));
  EXPECT_EQ(0.0, apex_pos.lam);
  EXPECT_NEAR(kHalfPi, apex_pos.phi, 1e-12);

  ScopedPj pj_neg_n(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=-30 +lat_2=-30 +R=1"));
  ASSERT_NE(nullptr, pj_neg_n.get());
  const double phi1_neg = pj_param(ctx.get(), pj_neg_n->params, "rlat_1").f;
  const double c_neg = phi1_neg + std::cos(phi1_neg) / std::sin(phi1_neg);
  const PJ_XY apex_xy_neg =
      pj_neg_n->fwd({.lam = 0.0, .phi = c_neg}, pj_neg_n.get());
  const PJ_LP apex_neg = pj_inv(apex_xy_neg, pj_neg_n.get());
  EXPECT_EQ(0, proj_errno(pj_neg_n.get()));
  EXPECT_EQ(0.0, apex_neg.lam);
  EXPECT_NEAR(-kHalfPi, apex_neg.phi, 1e-12);
}

TEST(EqdcTest, ConstructorAndDestructorEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(
      proj_create(ctx.get(), "+proj=eqdc +lat_1=30 +lat_2=50 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->destructor);

  // 1. Destructor with nullptr PJ* returns nullptr.
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  // 2. Direct pj_eqdc(nullptr) allocates a skeleton PJ with null opaque;
  // passing it to pj_eqdc_destructor exercises the nullptr == P->opaque branch.
  PJ* raw = pj_eqdc(nullptr);
  ASSERT_NE(nullptr, raw);
  EXPECT_STREQ("eqdc", raw->short_name);
  EXPECT_EQ(nullptr, raw->opaque);
  EXPECT_EQ(nullptr, pj->destructor(raw, PROJ_ERR_OTHER));
}

}  // namespace
}  // namespace proj
