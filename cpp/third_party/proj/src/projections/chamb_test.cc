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
// Unit tests for the Chamberlin Trimetric projection
// (proj/src/projections/chamb.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/chamb.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

C_NAMESPACE PJ* pj_chamb(PJ*);

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(ChambTest, ColocatedControlPointsFailWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. Default control points are all (0, 0), so v.r == 0 (within TOL = 1e-9).
  ScopedPj all_zero(proj_create(ctx.get(), "+proj=chamb +R=6371000"));
  EXPECT_EQ(nullptr, all_zero.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(logs.ContainsMessage(
      "Invalid value for control points: they should be distinct"));

  // 2. Control points 2 and 3 identical (c[1].v.r == 0).
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj dup_23(proj_create(
      ctx.get(),
      "+proj=chamb +lat_1=10 +lon_1=10 +lat_2=20 +lon_2=30 +lat_3=20 +lon_3=30 "
      "+R=6371000"));
  EXPECT_EQ(nullptr, dup_23.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));

  // 3. Control points 3 and 1 identical (c[2].v.r == 0).
  proj_context_errno_set(ctx.get(), 0);
  ScopedPj dup_31(proj_create(
      ctx.get(),
      "+proj=chamb +lat_1=10 +lon_1=10 +lat_2=20 +lon_2=30 +lat_3=10 +lon_3=10 "
      "+R=6371000"));
  EXPECT_EQ(nullptr, dup_31.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(ChambTest, ForwardAtControlPointsAndGeneralPointsSmallAndLargeArcs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Small control-point separation (|dphi| <= 1 rad and |dlam| <= 1 rad),
  // exercising the Haversine `2 * aasin(...)` branch of `vect`. Control points
  // are ordered clockwise (1: NW, 2: NE, 3: S) matching chamb's planar
  // control-triangle orientation.
  ScopedPj pj_small(proj_create(
      ctx.get(),
      "+proj=chamb +lat_1=30 +lon_1=10 +lat_2=30 +lon_2=30 +lat_3=10 +lon_3=20 "
      "+ellps=WGS84"));
  ASSERT_NE(nullptr, pj_small.get());
  // Chamberlin Trimetric is spherical-only (`P->es = 0`) and has no inverse.
  EXPECT_EQ(0.0, pj_small->es);
  EXPECT_EQ(0, pj_has_inverse(pj_small.get()));
  EXPECT_STREQ("chamb", proj_pj_info(pj_small.get()).id);

  // Evaluating at each of the three control points hits `v[i].r == 0.0`
  // (`break`) and `if (i < 3) xy = Q->c[i].p` for i = 0, 1, 2.
  const PJ_COORD ctl_small[3] = {
      proj_coord(DegToRad(10.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(30.0), DegToRad(30.0), 0.0, 0.0),
      proj_coord(DegToRad(20.0), DegToRad(10.0), 0.0, 0.0),
  };
  for (int i = 0; i < 3; ++i) {
    const PJ_COORD fwd_ctl = proj_trans(pj_small.get(), PJ_FWD, ctl_small[i]);
    EXPECT_EQ(0, proj_errno(pj_small.get()));
    EXPECT_TRUE(std::isfinite(fwd_ctl.xy.x));
    EXPECT_TRUE(std::isfinite(fwd_ctl.xy.y));

    // A point slightly perturbed from control point i should project very close
    // to Q->c[i].p via the general intercept-mean branch (`i == 3`).
    const PJ_COORD near_ctl = proj_coord(ctl_small[i].lp.lam + 1e-5,
                                         ctl_small[i].lp.phi + 1e-5, 0.0, 0.0);
    const PJ_COORD fwd_near = proj_trans(pj_small.get(), PJ_FWD, near_ctl);
    EXPECT_EQ(0, proj_errno(pj_small.get()));
    EXPECT_NEAR(fwd_ctl.xy.x, fwd_near.xy.x, 200.0);
    EXPECT_NEAR(fwd_ctl.xy.y, fwd_near.xy.y, 200.0);
  }

  // 2. Large control-point separation (|dphi| > 1 rad or |dlam| > 1 rad),
  // exercising the spherical law of cosines `aacos(...)` branch of `vect`, and
  // testing points on both sides of the control triangle (`v[i].Az < 0` and
  // `v[i].Az >= 0`).
  ScopedPj pj_large(proj_create(
      ctx.get(),
      "+proj=chamb +lat_1=-30 +lon_1=-70 +lat_2=40 +lon_2=60 +lat_3=55 "
      "+lon_3=-10 +R=6371000"));
  ASSERT_NE(nullptr, pj_large.get());

  PJ_COORD test_pts[5] = {
      proj_coord(DegToRad(0.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(-80.0), DegToRad(-40.0), 0.0, 0.0),
      proj_coord(DegToRad(80.0), DegToRad(10.0), 0.0, 0.0),
      proj_coord(DegToRad(-20.0), DegToRad(70.0), 0.0, 0.0),
      proj_coord(DegToRad(10.0), DegToRad(-50.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj_large.get(), PJ_FWD, 5, test_pts));
  for (const auto& pt : test_pts) {
    EXPECT_TRUE(std::isfinite(pt.xy.x));
    EXPECT_TRUE(std::isfinite(pt.xy.y));
  }

  const PJ_FACTORS factors = proj_factors(
      pj_large.get(), proj_coord(DegToRad(0.0), DegToRad(20.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_large.get()));
  EXPECT_GT(factors.areal_scale, 0.5);

  // 3. Direct call to pj_chamb(nullptr) exercises OPERATION macro allocation.
  ScopedPj raw(pj_chamb(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("chamb", raw->short_name);
}

}  // namespace
}  // namespace proj
