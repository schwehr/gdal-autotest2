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
// Unit tests for the Central Conic (ccon) projection
// (proj/src/projections/ccon.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/ccon.cpp

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

TEST(CconTest, MissingOrZeroLat1FailsWithIllegalArgValue) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  for (const char* defn :
       {"+proj=ccon", "+proj=ccon +lat_1=0", "+proj=ccon +lat_1=1e-12"}) {
    ScopedPj pj(proj_create(ctx.get(), defn));
    EXPECT_EQ(nullptr, pj.get()) << defn;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for lat_1"));
}

TEST(CconTest, ForwardAndInverseFormulaRoundTripAndFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kLat1Deg = 45.0;
  const double phi1 = DegToRad(kLat1Deg);
  const double sinphi1 = std::sin(phi1);
  const double ctgphi1 = std::cos(phi1) / sinphi1;

  ScopedPj pj(proj_create(ctx.get(), "+proj=ccon +lat_1=45 +R=1"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  struct CoordCase {
    double lon_deg;
    double lat_deg;
  };
  constexpr CoordCase kCases[] = {
      // On the standard parallel phi == phi1: r == ctgphi1.
      {.lon_deg = 0.0, .lat_deg = 45.0},   {.lon_deg = 20.0, .lat_deg = 45.0},
      {.lon_deg = -35.0, .lat_deg = 30.0}, {.lon_deg = 50.0, .lat_deg = 60.0},
      {.lon_deg = -80.0, .lat_deg = 15.0},
  };

  for (const auto& tc : kCases) {
    const double lam = DegToRad(tc.lon_deg);
    const double phi = DegToRad(tc.lat_deg);

    // 1. Forward projection matches analytic Central Conic equations.
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double r = ctgphi1 - std::tan(phi - phi1);
    const double expected_x = r * std::sin(lam * sinphi1);
    const double expected_y = ctgphi1 - r * std::cos(lam * sinphi1);
    EXPECT_NEAR(expected_x, fwd.xy.x, 1e-12);
    EXPECT_NEAR(expected_y, fwd.xy.y, 1e-12);

    // 2. Inverse projection recovers (lam, phi).
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lam, inv.lp.lam, 1e-12);
    EXPECT_NEAR(phi, inv.lp.phi, 1e-12);
  }

  // 3. On the standard parallel lat_1 = 45 deg, both meridional and parallel
  // scale factors equal 1.0.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, factors.parallel_scale, 1e-6);
}

TEST(CconTest, EllipsoidalArrayTransformAndDestructorNullGuards) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=ccon +lat_1=30 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(10.0), DegToRad(30.0), 100.0, 2026.0),
      proj_coord(DegToRad(-25.0), DegToRad(40.0), 200.0, 2026.5),
  });
  const auto orig = coords;

  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(orig[i].lpz.lam, coords[i].lpz.lam, 1e-11);
    EXPECT_NEAR(orig[i].lpz.phi, coords[i].lpz.phi, 1e-11);
    EXPECT_NEAR(orig[i].lpz.z, coords[i].lpz.z, 1e-9);
    EXPECT_DOUBLE_EQ(orig[i].xyzt.t, coords[i].xyzt.t);
  }

  // Exercise pj_ccon_destructor defensive guards for nullptr and null opaque.
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  PJ* raw = pj_new();
  ASSERT_NE(nullptr, raw);
  raw->ctx = ctx.get();
  EXPECT_EQ(nullptr, raw->opaque);
  EXPECT_EQ(nullptr, pj->destructor(raw, 0));
}

}  // namespace
}  // namespace proj
