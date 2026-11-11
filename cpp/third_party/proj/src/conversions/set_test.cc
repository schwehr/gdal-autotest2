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
// Unit tests for the `+proj=set` coordinate component setter operation
// (proj/src/conversions/set.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/set.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(SetTest, ParameterSubsetsOverwriteOnlySpecifiedDimensionsSymmetrically) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct SetCase {
    const char* proj_str;
    std::array<double, 4> expected;
  };
  constexpr SetCase kCases[] = {
      // No parameters specified -> all 4 components preserved.
      {.proj_str = "+proj=set", .expected = {1.0, 2.0, 3.0, 4.0}},
      // Individual dimension flags.
      {.proj_str = "+proj=set +v_1=-10.5", .expected = {-10.5, 2.0, 3.0, 4.0}},
      {.proj_str = "+proj=set +v_2=22.25", .expected = {1.0, 22.25, 3.0, 4.0}},
      {.proj_str = "+proj=set +v_3=0", .expected = {1.0, 2.0, 0.0, 4.0}},
      {.proj_str = "+proj=set +v_4=2026.5",
       .expected = {1.0, 2.0, 3.0, 2026.5}},
      // Flag without '=val' triggers pj_param_exists with pj_param default 0.0.
      {.proj_str = "+proj=set +v_1", .expected = {0.0, 2.0, 3.0, 4.0}},
      // Multiple and all dimensions overwritten together.
      {.proj_str = "+proj=set +v_1=100 +v_3=-50",
       .expected = {100.0, 2.0, -50.0, 4.0}},
      {.proj_str = "+proj=set +v_1=11 +v_2=22 +v_3=33 +v_4=44",
       .expected = {11.0, 22.0, 33.0, 44.0}},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.proj_str));
    ASSERT_NE(nullptr, pj.get()) << tc.proj_str;
    EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_left(pj.get()));
    EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_right(pj.get()));
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    const PJ_COORD input = proj_coord(1.0, 2.0, 3.0, 4.0);
    // Both forward and inverse invoke set_fwd_inv.
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, input);
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, input);
    EXPECT_EQ(0, proj_errno(pj.get()));

    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(tc.expected[i], fwd.v[i]) << tc.proj_str << " i=" << i;
      EXPECT_DOUBLE_EQ(tc.expected[i], inv.v[i]) << tc.proj_str << " i=" << i;
    }

    // PJ_IDENT bypasses set_fwd_inv and leaves input untouched.
    const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, input);
    EXPECT_EQ(0, proj_errno(pj.get()));
    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(input.v[i], ident.v[i]);
    }
  }
}

TEST(SetTest, PipelineBatchAndLegacyEntryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Pipeline: force ellipsoidal height v_3 = 100 m before +proj=cart.
  ScopedPj pipe(proj_create(
      ctx.get(),
      "+proj=pipeline +step +proj=set +v_3=100 +step +proj=cart +ellps=WGS84"));
  ScopedPj cart(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, pipe.get());
  ASSERT_NE(nullptr, cart.get());

  const double lon = 12.0 * (std::numbers::pi / 180.0);
  const double lat = 55.0 * (std::numbers::pi / 180.0);
  const PJ_COORD from_pipe =
      proj_trans(pipe.get(), PJ_FWD, proj_coord(lon, lat, 999.0, 0.0));
  const PJ_COORD from_cart =
      proj_trans(cart.get(), PJ_FWD, proj_coord(lon, lat, 100.0, 0.0));
  EXPECT_EQ(0, proj_errno(pipe.get()));
  EXPECT_EQ(0, proj_errno(cart.get()));
  EXPECT_NEAR(from_cart.xyz.x, from_pipe.xyz.x, 1e-8);
  EXPECT_NEAR(from_cart.xyz.y, from_pipe.xyz.y, 1e-8);
  EXPECT_NEAR(from_cart.xyz.z, from_pipe.xyz.z, 1e-8);

  // 2. Batch proj_trans_array and proj_trans_generic.
  ScopedPj set_zt(proj_create(ctx.get(), "+proj=set +v_3=42.5 +v_4=2010.25"));
  ASSERT_NE(nullptr, set_zt.get());

  std::array<PJ_COORD, 2> coords = {
      proj_coord(1.0, 2.0, -5.0, 1990.0),
      proj_coord(-3.0, 4.0, 99.0, 2000.0),
  };
  ASSERT_EQ(
      0, proj_trans_array(set_zt.get(), PJ_FWD, coords.size(), coords.data()));
  for (const auto& c : coords) {
    EXPECT_DOUBLE_EQ(42.5, c.xyzt.z);
    EXPECT_DOUBLE_EQ(2010.25, c.xyzt.t);
  }
  EXPECT_DOUBLE_EQ(1.0, coords[0].xyzt.x);
  EXPECT_DOUBLE_EQ(4.0, coords[1].xyzt.y);

  std::array<double, 2> x = {7.0, 8.0};
  std::array<double, 2> y = {9.0, 10.0};
  std::array<double, 2> z = {11.0, 12.0};
  std::array<double, 2> t = {13.0, 14.0};
  EXPECT_EQ(2u,
            proj_trans_generic(set_zt.get(), PJ_INV, x.data(), sizeof(double),
                               x.size(), y.data(), sizeof(double), y.size(),
                               z.data(), sizeof(double), z.size(), t.data(),
                               sizeof(double), t.size()));
  EXPECT_EQ(0, proj_errno(set_zt.get()));
  EXPECT_DOUBLE_EQ(42.5, z[0]);
  EXPECT_DOUBLE_EQ(42.5, z[1]);
  EXPECT_DOUBLE_EQ(2010.25, t[0]);
  EXPECT_DOUBLE_EQ(2010.25, t[1]);

  // 3. Classic pj_fwd / pj_inv / pj_fwd3d / pj_inv3d dispatch via fwd4d/inv4d.
  ScopedPj set_xy(proj_create(ctx.get(), "+proj=set +v_1=5.5 +v_2=-6.5"));
  ASSERT_NE(nullptr, set_xy.get());
  const PJ_XY xy = pj_fwd({.lam = 1.0, .phi = 2.0}, set_xy.get());
  EXPECT_EQ(0, proj_errno(set_xy.get()));
  EXPECT_DOUBLE_EQ(5.5, xy.x);
  EXPECT_DOUBLE_EQ(-6.5, xy.y);

  const PJ_LP lp = pj_inv({.x = 9.0, .y = 8.0}, set_xy.get());
  EXPECT_EQ(0, proj_errno(set_xy.get()));
  EXPECT_DOUBLE_EQ(5.5, lp.lam);
  EXPECT_DOUBLE_EQ(-6.5, lp.phi);

  const PJ_XYZ xyz = pj_fwd3d({.lam = 1.0, .phi = 2.0, .z = 3.0}, set_zt.get());
  EXPECT_EQ(0, proj_errno(set_zt.get()));
  EXPECT_DOUBLE_EQ(42.5, xyz.z);
  const PJ_LPZ lpz = pj_inv3d({.x = 1.0, .y = 2.0, .z = 3.0}, set_zt.get());
  EXPECT_EQ(0, proj_errno(set_zt.get()));
  EXPECT_DOUBLE_EQ(42.5, lpz.z);
}

TEST(SetTest, HugeValNanAndProjFactorsEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj set_t(proj_create(ctx.get(), "+proj=set +v_4=2025.0"));
  ASSERT_NE(nullptr, set_t.get());

  // 1. fwd_prepare / inv_prepare check only v[0..2] for HUGE_VAL, so
  //    t == HUGE_VAL is cleanly overwritten by +v_4 in both directions.
  const PJ_COORD t_huge_fwd =
      proj_trans(set_t.get(), PJ_FWD, proj_coord(1.0, 2.0, 3.0, HUGE_VAL));
  EXPECT_EQ(0, proj_errno(set_t.get()));
  EXPECT_DOUBLE_EQ(2025.0, t_huge_fwd.xyzt.t);

  const PJ_COORD t_huge_inv =
      proj_trans(set_t.get(), PJ_INV, proj_coord(1.0, 2.0, 3.0, HUGE_VAL));
  EXPECT_EQ(0, proj_errno(set_t.get()));
  EXPECT_DOUBLE_EQ(2025.0, t_huge_inv.xyzt.t);

  // 2. HUGE_VAL in v[2] aborts in fwd_prepare (no errno) and inv_prepare
  //    (OUTSIDE_PROJECTION_DOMAIN).
  const PJ_COORD z_huge_fwd =
      proj_trans(set_t.get(), PJ_FWD, proj_coord(1.0, 2.0, HUGE_VAL, 0.0));
  EXPECT_TRUE(std::isinf(z_huge_fwd.xyzt.x));
  EXPECT_EQ(0, proj_errno(set_t.get()));

  const PJ_COORD z_huge_inv =
      proj_trans(set_t.get(), PJ_INV, proj_coord(1.0, 2.0, HUGE_VAL, 0.0));
  EXPECT_TRUE(std::isinf(z_huge_inv.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(set_t.get()));
  proj_errno_reset(set_t.get());

  // 3. NaN in any component causes proj_trans to return all-NaN cleanly.
  const PJ_COORD nan_out = proj_trans(
      set_t.get(), PJ_FWD,
      proj_coord(1.0, 2.0, 3.0, std::numeric_limits<double>::quiet_NaN()));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.t));
  EXPECT_EQ(0, proj_errno(set_t.get()));

  // 4. proj_factors rejects operations without a 2D P->fwd callback.
  const PJ_FACTORS fac =
      proj_factors(set_t.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(set_t.get()));
  EXPECT_DOUBLE_EQ(0.0, fac.meridional_scale);
  proj_errno_reset(set_t.get());
}

}  // namespace
}  // namespace proj
