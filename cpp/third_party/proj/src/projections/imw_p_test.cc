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
// Unit tests for the International Map of the World Polyconic (imw_p)
// projection (proj/src/projections/imw_p.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/imw_p.cpp

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
constexpr double kHalfPi = kPi / 2.0;
constexpr double kWgs84MajorM = 6378137.0;
constexpr double kRoundTripTolRad = 1e-9;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(ImwPTest, InitializationValidationDescendingParallelsAndMetadata) {
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
      {.def = "+proj=imw_p +lat_2=36 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Missing parameter: lat_1"},
      {.def = "+proj=imw_p +lat_1=32 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Missing parameter: lat_2"},
      // |lat_1 - lat_2| < EPS.
      {.def = "+proj=imw_p +lat_1=32 +lat_2=32 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Illegal value for lat_1 and lat_2"},
      // |lat_1 + lat_2| < EPS.
      {.def = "+proj=imw_p +lat_1=-32 +lat_2=32 +ellps=WGS84",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Illegal value for lat_1 and lat_2"},
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
  char arg0[] = "proj=imw_p";
  char arg1[] = "lat_1=32";
  char arg2[] = "lat_2=36";
  char arg3[] = "ellps=WGS84";
  std::array<char*, 4> argv = {arg0, arg1, arg2, arg3};
  ScopedPj pj_asc(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pj_asc.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj_asc.get());
  EXPECT_STREQ("imw_p", info.id);
  EXPECT_THAT(pj_asc->descr,
              HasSubstr("International Map of the World Polyconic"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_asc.get()));
  EXPECT_EQ(1, proj_angular_input(pj_asc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj_asc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj_asc.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj_asc.get(), PJ_INV));
  EXPECT_EQ(nullptr, pj_asc->destructor(nullptr, 0));

  // Descending lat_1 > lat_2 swaps internally to match ascending order.
  ScopedPj pj_desc(
      proj_create(ctx.get(), "+proj=imw_p +lat_1=36 +lat_2=32 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_desc.get());
  const PJ_COORD pt = proj_coord(DegToRad(1.5), DegToRad(34.0), 0.0, 0.0);
  const PJ_COORD out_asc = proj_trans(pj_asc.get(), PJ_FWD, pt);
  const PJ_COORD out_desc = proj_trans(pj_desc.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_asc.get()));
  EXPECT_EQ(0, proj_errno(pj_desc.get()));
  EXPECT_DOUBLE_EQ(out_asc.xy.x, out_desc.xy.x);
  EXPECT_DOUBLE_EQ(out_asc.xy.y, out_desc.xy.y);
}

TEST(ImwPTest, AutomaticAndExplicitLon1LatitudeBandDefaults) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Omitted lon_1 picks 2 deg (|sig| <= 60), 4 deg (60 < |sig| <= 76), or
  //    8 deg (|sig| > 76). Verify exact match with explicit +lon_1=.
  struct BandCase {
    const char* auto_def;
    const char* explicit_def;
    double test_lon_deg;
    double test_lat_deg;
  };
  constexpr BandCase kBands[] = {
      {.auto_def = "+proj=imw_p +lat_1=32 +lat_2=36 +ellps=WGS84",
       .explicit_def = "+proj=imw_p +lat_1=32 +lat_2=36 +lon_1=2 +ellps=WGS84",
       .test_lon_deg = 1.0,
       .test_lat_deg = 34.0},
      {.auto_def = "+proj=imw_p +lat_1=64 +lat_2=68 +ellps=WGS84",
       .explicit_def = "+proj=imw_p +lat_1=64 +lat_2=68 +lon_1=4 +ellps=WGS84",
       .test_lon_deg = 2.0,
       .test_lat_deg = 66.0},
      {.auto_def = "+proj=imw_p +lat_1=76 +lat_2=80 +ellps=WGS84",
       .explicit_def = "+proj=imw_p +lat_1=76 +lat_2=80 +lon_1=8 +ellps=WGS84",
       .test_lon_deg = 4.0,
       .test_lat_deg = 78.0},
      {.auto_def = "+proj=imw_p +lat_1=-68 +lat_2=-64 +ellps=WGS84",
       .explicit_def =
           "+proj=imw_p +lat_1=-68 +lat_2=-64 +lon_1=4 +ellps=WGS84",
       .test_lon_deg = -2.0,
       .test_lat_deg = -66.0},
  };

  for (const auto& tc : kBands) {
    ScopedPj pj_auto(proj_create(ctx.get(), tc.auto_def));
    ScopedPj pj_expl(proj_create(ctx.get(), tc.explicit_def));
    ASSERT_NE(nullptr, pj_auto.get()) << tc.auto_def;
    ASSERT_NE(nullptr, pj_expl.get()) << tc.explicit_def;

    const PJ_COORD in = proj_coord(DegToRad(tc.test_lon_deg),
                                   DegToRad(tc.test_lat_deg), 0.0, 0.0);
    const PJ_COORD fwd_auto = proj_trans(pj_auto.get(), PJ_FWD, in);
    const PJ_COORD fwd_expl = proj_trans(pj_expl.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj_auto.get()));
    EXPECT_EQ(0, proj_errno(pj_expl.get()));
    EXPECT_DOUBLE_EQ(fwd_expl.xy.x, fwd_auto.xy.x);
    EXPECT_DOUBLE_EQ(fwd_expl.xy.y, fwd_auto.xy.y);

    const PJ_COORD inv = proj_trans(pj_auto.get(), PJ_INV, fwd_auto);
    EXPECT_EQ(0, proj_errno(pj_auto.get()));
    EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad);
    EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad);
  }

  // 2. Custom +lon_1=4 differs from default 2 deg in the 32..36N band.
  ScopedPj pj_def2(
      proj_create(ctx.get(), "+proj=imw_p +lat_1=32 +lat_2=36 +ellps=WGS84"));
  ScopedPj pj_lon4(proj_create(
      ctx.get(), "+proj=imw_p +lat_1=32 +lat_2=36 +lon_1=4 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_def2.get());
  ASSERT_NE(nullptr, pj_lon4.get());
  const PJ_COORD pt = proj_coord(DegToRad(1.0), DegToRad(34.0), 0.0, 0.0);
  const PJ_COORD fwd2 = proj_trans(pj_def2.get(), PJ_FWD, pt);
  const PJ_COORD fwd4 = proj_trans(pj_lon4.get(), PJ_FWD, pt);
  EXPECT_NE(fwd2.xy.y, fwd4.xy.y);
  const PJ_COORD inv4 = proj_trans(pj_lon4.get(), PJ_INV, fwd4);
  EXPECT_EQ(0, proj_errno(pj_lon4.get()));
  EXPECT_NEAR(pt.lp.lam, inv4.lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(pt.lp.phi, inv4.lp.phi, kRoundTripTolRad);
}

TEST(ImwPTest, AllModesNorthernSouthernAndEquatorBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct LonLatDeg {
    double lon_deg;
    double lat_deg;
  };

  // 1. NONE_IS_ZERO (northern hemisphere) + equator lp.phi == 0 branch.
  {
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=imw_p +lat_1=32 +lat_2=36 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    // Central meridian at southerly bounding parallel lat_1 maps to (0, 0).
    const PJ_COORD lat1_origin =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(32.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, lat1_origin.xy.x, kMetricTolM);
    EXPECT_NEAR(0.0, lat1_origin.xy.y, kMetricTolM);

    // Equator point exercises `if (lp.phi == 0.0)` branch in loc_for().
    const PJ_COORD eq_fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(DegToRad(1.0), 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(kWgs84MajorM * DegToRad(1.0), eq_fwd.xy.x, kMetricTolM);
    EXPECT_DOUBLE_EQ(0.0, eq_fwd.xy.y);

    constexpr LonLatDeg kNorthCases[] = {
        {.lon_deg = 0.0, .lat_deg = 34.0},
        {.lon_deg = 1.0, .lat_deg = 34.0},
        {.lon_deg = -2.0, .lat_deg = 35.0},
        {.lon_deg = 1.5, .lat_deg = 20.0},
    };
    for (const auto& tc : kNorthCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad);
    }
  }

  // 2. NONE_IS_ZERO (southern hemisphere) exercises negative-latitude branches.
  {
    ScopedPj pj(proj_create(ctx.get(),
                            "+proj=imw_p +lat_1=-36 +lat_2=-32 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    constexpr LonLatDeg kSouthCases[] = {
        {.lon_deg = 0.0, .lat_deg = -34.0},
        {.lon_deg = 1.5, .lat_deg = -34.0},
        {.lon_deg = -1.5, .lat_deg = -33.0},
        {.lon_deg = 2.0, .lat_deg = -35.0},
    };
    for (const auto& tc : kSouthCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad);
    }
  }

  // 3. PHI_1_IS_ZERO (lat_1=0, lat_2=4).
  {
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=imw_p +lat_1=0 +lat_2=4 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    constexpr LonLatDeg kPhi1ZeroCases[] = {
        {.lon_deg = 0.0, .lat_deg = 0.0}, {.lon_deg = 1.0, .lat_deg = 0.0},
        {.lon_deg = 1.0, .lat_deg = 2.0}, {.lon_deg = -1.5, .lat_deg = 3.0},
        {.lon_deg = 2.0, .lat_deg = 4.0},
    };
    for (const auto& tc : kPhi1ZeroCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad);
    }
  }

  // 4. PHI_2_IS_ZERO (lat_1=-4, lat_2=0): forward works everywhere; inverse
  //    starts at phi_2 == 0, so y == 0 converges while y != 0 triggers
  //    denom == 0 domain error in imw_p_e_inverse().
  {
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=imw_p +lat_1=-4 +lat_2=0 +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(1.0), DegToRad(-2.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_GT(fwd.xy.x, 0.0);
    EXPECT_GT(fwd.xy.y, 0.0);

    // Equator point y == 0 satisfies fabs(t.y - xy.y) <= TOL at i=0.
    const PJ_COORD eq_inv =
        proj_trans(pj.get(), PJ_INV,
                   proj_coord(kWgs84MajorM * DegToRad(1.0), 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(DegToRad(1.0), eq_inv.lp.lam, kRoundTripTolRad);
    EXPECT_NEAR(0.0, eq_inv.lp.phi, kRoundTripTolRad);

    // Non-zero y hits denom == t.y - yc == 0 on first iteration.
    const PJ_COORD denom_err =
        proj_trans(pj.get(), PJ_INV, proj_coord(0.0, 100000.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(denom_err.lp.lam));
    EXPECT_TRUE(std::isinf(denom_err.lp.phi));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }
}

TEST(ImwPTest, Transforms3D4DBatchGenericFactorsAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=imw_p +lat_1=32 +lat_2=36 +lon_0=15 +x_0=500000 +y_0=100000 "
      "+ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t; PJ_IDENT is no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(16.0), DegToRad(34.0), 310.0, 2025.0);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(310.0, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2025.0, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(310.0, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2025.0, inv_4d.lpzt.t);

  // 2. Classic 2D and 3D helpers agree with proj_trans.
  const PJ_XY xy2d = pj_fwd(in_4d.lp, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd_4d.xy.x, xy2d.x, 1e-6);
  EXPECT_NEAR(fwd_4d.xy.y, xy2d.y, 1e-6);
  const PJ_LP lp2d = pj_inv(xy2d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lp.lam, lp2d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lp.phi, lp2d.phi, kRoundTripTolRad);

  const PJ_XYZ xyz3d = pj_fwd3d(in_4d.lpz, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(310.0, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(310.0, lpz3d.z);

  // 3. Batch proj_trans_array (valid + mixed inverse failure via
  // PHI_2_IS_ZERO).
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(DegToRad(15.0), DegToRad(32.0), 0.0, 0.0),
      proj_coord(DegToRad(16.5), DegToRad(35.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(DegToRad(15.0), valid_batch[0].lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(DegToRad(32.0), valid_batch[0].lp.phi, kRoundTripTolRad);

  ScopedPj pj_phi2_zero(
      proj_create(ctx.get(), "+proj=imw_p +lat_1=-4 +lat_2=0 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_phi2_zero.get());
  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(100000.0, 0.0, 0.0, 0.0),
      proj_coord(0.0, 100000.0, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj_phi2_zero.get(), PJ_INV, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].lp.lam));
  EXPECT_TRUE(std::isinf(mixed_batch[1].lp.lam));
  proj_errno_reset(pj_phi2_zero.get());

  // 4. proj_trans_generic with strided arrays.
  std::array<double, 2> xs = {DegToRad(15.0), DegToRad(16.0)};
  std::array<double, 2> ys = {DegToRad(32.0), DegToRad(35.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(500000.0, xs[0], kMetricTolM);
  EXPECT_NEAR(100000.0, ys[0], kMetricTolM);

  // 5. Scale factors inside sheet and nullptr PJ guard.
  const PJ_FACTORS fac = proj_factors(pj.get(), in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, fac.meridional_scale, 0.01);
  EXPECT_NEAR(1.0, fac.parallel_scale, 0.01);
  EXPECT_NEAR(1.0, fac.areal_scale, 0.01);
  EXPECT_NEAR(kHalfPi, fac.meridian_parallel_angle, 0.01);

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
  EXPECT_TRUE(std::isinf(inf_fwd.xy.x));

  const PJ_COORD bad_lat =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 2.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(bad_lat.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
}

// TODO(schwehr): OOM guards (calloc and pj_enfn), the P->opaque == nullptr
// destructor branch, and the N_MAX_ITER=1000 fixed-point divergence guard in
// imw_p_e_inverse remain uncovered without fault injection.

}  // namespace
}  // namespace proj
