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
// Unit tests for the `+proj=noop` no-operation conversion
// (proj/src/conversions/noop.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/conversions/noop.cpp

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

TEST(NoopTest, PreservesAll4DCoordinatesAcrossDirectionsAndEntryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // need_ellps = 0, so "+proj=noop" succeeds with no ellipsoid parameters.
  ScopedPj pj(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, pj.get());

  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_left(pj.get()));
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_right(pj.get()));
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // 1. PJ_FWD, PJ_INV, and PJ_IDENT preserve arbitrary values and -0.0 sign.
  constexpr std::array<std::array<double, 4>, 3> kCases = {{
      {123.456, -789.012, 42.5, 2026.75},
      {-0.0, 0.0, -0.0, 0.0},
      {1e9, -1e9, 1e-12, -2000.0},
  }};

  for (const auto& vals : kCases) {
    const PJ_COORD input = proj_coord(vals[0], vals[1], vals[2], vals[3]);
    for (const PJ_DIRECTION dir : {PJ_FWD, PJ_INV, PJ_IDENT}) {
      const PJ_COORD out = proj_trans(pj.get(), dir, input);
      EXPECT_EQ(0, proj_errno(pj.get()));
      for (int i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(input.v[i], out.v[i]);
        EXPECT_EQ(std::signbit(input.v[i]), std::signbit(out.v[i]));
      }
    }
  }

  // 2. Classic 2D and 3D internal entry points dispatch through fwd4d / inv4d.
  const PJ_XY xy = pj_fwd({.lam = 3.25, .phi = -4.5}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(3.25, xy.x);
  EXPECT_DOUBLE_EQ(-4.5, xy.y);

  const PJ_LP lp = pj_inv(xy, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(3.25, lp.lam);
  EXPECT_DOUBLE_EQ(-4.5, lp.phi);

  const PJ_XYZ xyz = pj_fwd3d({.lam = 10.0, .phi = 20.0, .z = -30.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(10.0, xyz.x);
  EXPECT_DOUBLE_EQ(20.0, xyz.y);
  EXPECT_DOUBLE_EQ(-30.0, xyz.z);

  const PJ_LPZ lpz = pj_inv3d(xyz, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(10.0, lpz.lam);
  EXPECT_DOUBLE_EQ(20.0, lpz.phi);
  EXPECT_DOUBLE_EQ(-30.0, lpz.z);
}

TEST(NoopTest, BatchGenericAndPipelineIntegration) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, pj.get());

  // 1. proj_trans_array in forward and inverse directions.
  std::array<PJ_COORD, 2> coords = {
      proj_coord(1.5, -2.5, 3.5, 2021.0),
      proj_coord(-4.5, 5.5, -6.5, 2025.0),
  };
  const auto original = coords;
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    for (int d = 0; d < 4; ++d) {
      EXPECT_DOUBLE_EQ(original[i].v[d], coords[i].v[d]);
    }
  }

  // 2. proj_trans_generic preserves all strided coordinates.
  std::array<double, 2> x = {11.0, 12.0};
  std::array<double, 2> y = {21.0, 22.0};
  std::array<double, 2> z = {31.0, 32.0};
  std::array<double, 2> t = {41.0, 42.0};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, x.data(), sizeof(double),
                                   x.size(), y.data(), sizeof(double), y.size(),
                                   z.data(), sizeof(double), z.size(), t.data(),
                                   sizeof(double), t.size()));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(11.0, x[0]);
  EXPECT_DOUBLE_EQ(22.0, y[1]);
  EXPECT_DOUBLE_EQ(31.0, z[0]);
  EXPECT_DOUBLE_EQ(42.0, t[1]);

  // 3. Leading and trailing noop steps in a pipeline inherit adjacent units.
  ScopedPj pipe(proj_create(
      ctx.get(),
      "+proj=pipeline +step +proj=noop +step +proj=merc +ellps=WGS84 "
      "+step +proj=noop"));
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, pipe.get());
  ASSERT_NE(nullptr, merc.get());

  const PJ_COORD pt =
      proj_coord(12.0 * (std::numbers::pi / 180.0),
                 55.0 * (std::numbers::pi / 180.0), 100.0, 2024.0);
  const PJ_COORD pipe_fwd = proj_trans(pipe.get(), PJ_FWD, pt);
  const PJ_COORD merc_fwd = proj_trans(merc.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pipe.get()));
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(merc_fwd.xyz.x, pipe_fwd.xyz.x, 1e-8);
  EXPECT_NEAR(merc_fwd.xyz.y, pipe_fwd.xyz.y, 1e-8);
  EXPECT_DOUBLE_EQ(100.0, pipe_fwd.xyz.z);
  EXPECT_DOUBLE_EQ(2024.0, pipe_fwd.xyzt.t);

  const PJ_COORD pipe_inv = proj_trans(pipe.get(), PJ_INV, pipe_fwd);
  EXPECT_EQ(0, proj_errno(pipe.get()));
  EXPECT_NEAR(pt.lpzt.lam, pipe_inv.lpzt.lam, 1e-12);
  EXPECT_NEAR(pt.lpzt.phi, pipe_inv.lpzt.phi, 1e-12);
}

TEST(NoopTest, HugeValNanAndProjFactorsEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, pj.get());

  // 1. HUGE_VAL only in t passes through untouched without error, whereas
  //    HUGE_VAL in v[0..2] is caught by fwd_prepare / inv_prepare.
  const PJ_COORD t_huge =
      proj_trans(pj.get(), PJ_FWD, proj_coord(1.0, 2.0, 3.0, HUGE_VAL));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(1.0, t_huge.xyzt.x);
  EXPECT_EQ(HUGE_VAL, t_huge.xyzt.t);

  const PJ_COORD x_huge_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 2.0, 3.0, 0.0));
  EXPECT_TRUE(std::isinf(x_huge_fwd.xyzt.x));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD x_huge_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 2.0, 3.0, 0.0));
  EXPECT_TRUE(std::isinf(x_huge_inv.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. NaN propagates cleanly to all components without setting errno.
  const PJ_COORD nan_out = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(1.0, std::numeric_limits<double>::quiet_NaN(), 3.0, 4.0));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.y));
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 3. proj_factors rejects fwd4d-only conversions.
  const PJ_FACTORS fac = proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, fac.meridional_scale);
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
