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
// Unit tests for the 3D Spherical Cross-track Height (sch) radar-aligned
// coordinate system (proj/src/projections/sch.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/sch.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

constexpr double kPi = std::numbers::pi;
constexpr double kSphereRadiusM = 6371000.0;
constexpr double kRoundTripTolRad = 1e-11;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(SchTest, InitializationMissingArgsAndMetadata) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  struct InvalidCase {
    const char* def;
    int expected_errno;
    const char* expected_log;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.def = nullptr,
       .expected_errno = PROJ_ERR_OTHER_API_MISUSE,
       .expected_log = nullptr},
      {.def = "",
       .expected_errno = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
       .expected_log = nullptr},
      {.def = "+proj=sch +plon_0=-118 +phdg_0=45 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
       .expected_log = "Missing parameter plat_0"},
      {.def = "+proj=sch +plat_0=34 +phdg_0=45 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
       .expected_log = "Missing parameter plon_0"},
      {.def = "+proj=sch +plat_0=34 +plon_0=-118 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
       .expected_log = "Missing parameter phdg_0"},
  };

  for (const auto& tc : kInvalidCases) {
    capture.Clear();
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj bad(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, bad.get());
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get()));
    if (tc.expected_log != nullptr) {
      EXPECT_TRUE(capture.ContainsMessage(tc.expected_log)) << tc.def;
    }
  }

  // Mutable buffers for proj_create_argv (never const_cast string literals).
  char arg0[] = "proj=sch";
  char arg1[] = "plat_0=34";
  char arg2[] = "plon_0=-118";
  char arg3[] = "phdg_0=45";
  char arg4[] = "h_0=1000";
  char arg5[] = "ellps=WGS84";
  std::array<char*, 6> argv = {arg0, arg1, arg2, arg3, arg4, arg5};
  ScopedPj pj(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("sch", info.id);
  EXPECT_THAT(pj->descr, HasSubstr("Spherical Cross-track Height"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));
  EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_INV));

  // sch registers exclusively 3D entry points (fwd/inv remain nullptr).
  EXPECT_EQ(nullptr, pj->fwd);
  EXPECT_EQ(nullptr, pj->inv);
  EXPECT_NE(nullptr, pj->fwd3d);
  EXPECT_NE(nullptr, pj->inv3d);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
}

TEST(SchTest, EllipsoidalAndSpherical3DRoundTripsAndPegGeometry) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Ellipsoidal without +h_0 (defaults to 0.0): peg point maps to (0,0,0)
  //    and heights above ellipsoid map linearly at peg.
  {
    ScopedPj pj(proj_create(
        ctx.get(),
        "+proj=sch +plat_0=34 +plon_0=-118 +phdg_0=30 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD peg_zero =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(-118.0), DegToRad(34.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, peg_zero.xyz.x, 1e-7);
    EXPECT_NEAR(0.0, peg_zero.xyz.y, 1e-7);
    EXPECT_NEAR(0.0, peg_zero.xyz.z, 1e-7);

    const PJ_COORD peg_elev =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(-118.0), DegToRad(34.0), 500.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, peg_elev.xyz.x, 1e-7);
    EXPECT_NEAR(0.0, peg_elev.xyz.y, 1e-7);
    EXPECT_NEAR(500.0, peg_elev.xyz.z, 1e-7);

    struct Case3D {
      double lon_deg;
      double lat_deg;
      double h_m;
    };
    constexpr Case3D kCases[] = {
        {.lon_deg = -117.5, .lat_deg = 34.5, .h_m = 250.0},
        {.lon_deg = -118.5, .lat_deg = 33.5, .h_m = -50.0},
        {.lon_deg = -119.0, .lat_deg = 35.0, .h_m = 4000.0},
    };
    for (const auto& tc : kCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), tc.h_m, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lpz.lam, inv.lpz.lam, kRoundTripTolRad);
      EXPECT_NEAR(in.lpz.phi, inv.lpz.phi, kRoundTripTolRad);
      EXPECT_NEAR(tc.h_m, inv.lpz.z, kMetricTolM);
    }
  }

  // 2. Ellipsoidal southern/western peg with non-zero +h_0=1500 and
  // phdg_0=-120.
  {
    ScopedPj pj(
        proj_create(ctx.get(),
                    "+proj=sch +plat_0=-35 +plon_0=-70 +phdg_0=-120 +h_0=1500 "
                    "+ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    // Reference altitude h_0=1500 maps to SCH height h=0 at the peg point.
    const PJ_COORD peg_ref =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(-70.0), DegToRad(-35.0), 1500.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, peg_ref.xyz.x, 1e-7);
    EXPECT_NEAR(0.0, peg_ref.xyz.y, 1e-7);
    EXPECT_NEAR(0.0, peg_ref.xyz.z, 1e-7);

    const PJ_COORD off_peg =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(-69.2), DegToRad(-34.4), 2100.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, off_peg);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(-69.2), inv.lpz.lam, kRoundTripTolRad);
    EXPECT_NEAR(DegToRad(-34.4), inv.lpz.phi, kRoundTripTolRad);
    EXPECT_NEAR(2100.0, inv.lpz.z, kMetricTolM);
  }

  // 3. Spherical exact geometry at equatorial peg heading due north (phdg_0=0):
  //    local radius of curvature equals R + h_0, along-track s follows +lat,
  //    and cross-track c is -R_curv * lon (right-hand side of heading).
  {
    constexpr double kH0 = 500.0;
    ScopedPj sph(proj_create(
        ctx.get(),
        "+proj=sch +plat_0=0 +plon_0=0 +phdg_0=0 +h_0=500 +R=6371000"));
    ASSERT_NE(nullptr, sph.get());
    EXPECT_EQ(0.0, sph->es);

    const double expected_arc = (kSphereRadiusM + kH0) * DegToRad(1.0);
    const PJ_COORD north_1deg =
        proj_trans(sph.get(), PJ_FWD, proj_coord(0.0, DegToRad(1.0), kH0, 0.0));
    EXPECT_EQ(0, proj_errno(sph.get()));
    EXPECT_NEAR(expected_arc, north_1deg.xyz.x, kMetricTolM);
    EXPECT_NEAR(0.0, north_1deg.xyz.y, kMetricTolM);
    EXPECT_NEAR(0.0, north_1deg.xyz.z, kMetricTolM);

    const PJ_COORD east_1deg =
        proj_trans(sph.get(), PJ_FWD, proj_coord(DegToRad(1.0), 0.0, kH0, 0.0));
    EXPECT_EQ(0, proj_errno(sph.get()));
    EXPECT_NEAR(0.0, east_1deg.xyz.x, kMetricTolM);
    EXPECT_NEAR(-expected_arc, east_1deg.xyz.y, kMetricTolM);
    EXPECT_NEAR(0.0, east_1deg.xyz.z, kMetricTolM);

    const PJ_COORD inv_east = proj_trans(sph.get(), PJ_INV, east_1deg);
    EXPECT_EQ(0, proj_errno(sph.get()));
    EXPECT_NEAR(DegToRad(1.0), inv_east.lpz.lam, kRoundTripTolRad);
    EXPECT_NEAR(0.0, inv_east.lpz.phi, kRoundTripTolRad);
    EXPECT_NEAR(kH0, inv_east.lpz.z, kMetricTolM);
  }
}

TEST(SchTest, Transforms2D3D4DBatchGenericFactorsAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=sch +plat_0=34 +plon_0=-118 +phdg_0=45 +h_0=200 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves t while transforming 3D position; PJ_IDENT is
  //    a no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(-117.75), DegToRad(34.25), 350.0, 2026.25);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(2026.25, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.z, inv_4d.lpzt.z, kMetricTolM);
  EXPECT_DOUBLE_EQ(2026.25, inv_4d.lpzt.t);

  // 2. Classic 2D pj_fwd/pj_inv fall back to fwd3d/inv3d with z=0; 3D
  //    pj_fwd3d/pj_inv3d match proj_trans directly.
  const PJ_COORD fwd_z0 = proj_trans(
      pj.get(), PJ_FWD, proj_coord(in_4d.lp.lam, in_4d.lp.phi, 0.0, 0.0));
  const PJ_XY xy2d = pj_fwd(in_4d.lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd_z0.xy.x, xy2d.x, 1e-6);
  EXPECT_NEAR(fwd_z0.xy.y, xy2d.y, 1e-6);

  const PJ_COORD inv_z0 =
      proj_trans(pj.get(), PJ_INV, proj_coord(xy2d.x, xy2d.y, 0.0, 0.0));
  const PJ_LP lp2d = pj_inv(xy2d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(inv_z0.lp.lam, lp2d.lam, kRoundTripTolRad);
  EXPECT_NEAR(inv_z0.lp.phi, lp2d.phi, kRoundTripTolRad);

  const PJ_XYZ xyz3d = pj_fwd3d(in_4d.lpz, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd_4d.xyz.x, xyz3d.x, 1e-6);
  EXPECT_NEAR(fwd_4d.xyz.y, xyz3d.y, 1e-6);
  EXPECT_NEAR(fwd_4d.xyz.z, xyz3d.z, 1e-6);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.z, lpz3d.z, kMetricTolM);

  // 3. Batch proj_trans_array: valid round-trip and mixed invalid coord.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(-118.0), DegToRad(34.0), 200.0, 0.0),
      proj_coord(DegToRad(-117.5), DegToRad(34.5), 500.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(-118.0), valid_batch[0].lpz.lam, kRoundTripTolRad);
  EXPECT_NEAR(DegToRad(34.0), valid_batch[0].lpz.phi, kRoundTripTolRad);
  EXPECT_NEAR(200.0, valid_batch[0].lpz.z, kMetricTolM);

  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(DegToRad(-118.0), DegToRad(34.0), 200.0, 0.0),
      proj_coord(0.0, 2.0, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
            proj_trans_array(pj.get(), PJ_FWD, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].xyz.x));
  EXPECT_TRUE(std::isinf(mixed_batch[1].xyz.x));
  proj_errno_reset(pj.get());

  // 4. proj_trans_generic with 3D arrays.
  std::array<double, 2> xs = {DegToRad(-118.0), DegToRad(-117.8)};
  std::array<double, 2> ys = {DegToRad(34.0), DegToRad(34.2)};
  std::array<double, 2> zs = {200.0, 350.0};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), zs.data(), sizeof(double),
                                   zs.size(), nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.0, xs[0], kMetricTolM);
  EXPECT_NEAR(0.0, ys[0], kMetricTolM);
  EXPECT_NEAR(0.0, zs[0], kMetricTolM);

  // 5. proj_factors requires 2D P->fwd, so sch reports invalid coord and zero
  //    factors; nullptr PJ returns zero-initialized factors.
  const PJ_FACTORS fac = proj_factors(pj.get(), in_4d);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, fac.meridional_scale);
  EXPECT_DOUBLE_EQ(0.0, fac.areal_scale);
  proj_errno_reset(pj.get());

  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);

  // 6. Edge inputs: nullptr PJ, NaN, Inf, out-of-range latitude.
  const PJ_COORD null_out = proj_trans(nullptr, PJ_FWD, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], null_out.v[0]);

  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_out =
      proj_trans(pj.get(), PJ_FWD, proj_coord(qnan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.v[0]));
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_COORD inf_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, HUGE_VAL, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_fwd.xyz.x));

  const PJ_COORD inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_inv.lpz.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
}

// TODO(schwehr): Allocator OOM guards in PJ_PROJECTION(sch) and pj_sch_setup()
// remain uncovered without allocator fault injection.

}  // namespace
}  // namespace proj
