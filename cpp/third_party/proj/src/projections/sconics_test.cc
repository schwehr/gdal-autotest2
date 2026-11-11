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
// Unit tests for simple conic projections: euler, murd1, murd2, murd3,
// pconic, tissot, and vitk1 (proj/src/projections/sconics.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/sconics.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

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
constexpr double kSphereRadiusM = 6371000.0;
constexpr double kRoundTripTolRad = 1e-11;
constexpr double kMetricTolM = 1e-4;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

struct ConicSpec {
  const char* id;
  const char* expected_desc;
  bool origin_at_zero_zero;
};

constexpr ConicSpec kAllConics[] = {
    {.id = "euler", .expected_desc = "Euler", .origin_at_zero_zero = true},
    {.id = "murd1", .expected_desc = "Murdoch I", .origin_at_zero_zero = true},
    {.id = "murd2", .expected_desc = "Murdoch II", .origin_at_zero_zero = true},
    {.id = "murd3",
     .expected_desc = "Murdoch III",
     .origin_at_zero_zero = true},
    {.id = "pconic",
     .expected_desc = "Perspective Conic",
     .origin_at_zero_zero = true},
    // Tissot sets rho_0 = sqrt((rho_c - 2*sin(phi0))/n) while forward uses
    // rho = rho_c - phi, so (lon_0, lat_0) has a small non-zero y offset.
    {.id = "tissot", .expected_desc = "Tissot", .origin_at_zero_zero = false},
    {.id = "vitk1",
     .expected_desc = "Vitkovsky I",
     .origin_at_zero_zero = true},
};

TEST(SconicsTest, InitializationValidationAndMetadataAllSevenConics) {
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
      {.def = "+proj=euler +lat_2=50 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Missing parameter: lat_1"},
      {.def = "+proj=euler +lat_1=30 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Missing parameter: lat_2"},
      {.def = "+proj=murd1 +lat_1=40 +lat_2=40 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Illegal value for lat_1 and lat_2"},
      {.def = "+proj=vitk1 +lat_1=-40 +lat_2=40 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Illegal value for lat_1 and lat_2"},
      // pconic enforces |lat_0 - 0.5 * (lat_1 + lat_2)| < 90 deg.
      {.def = "+proj=pconic +lat_1=45 +lat_2=55 +lat_0=-45 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lat_0/lat_1/lat_2"},
      {.def = "+proj=pconic +lat_1=30 +lat_2=60 +lat_0=-50 +R=6371000",
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for lat_0/lat_1/lat_2"},
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

  for (const auto& spec : kAllConics) {
    // Pass +ellps=WGS84 to verify pj_sconics_setup forces spherical P->es = 0.
    const std::string def =
        std::string("+proj=") + spec.id + " +lat_1=30 +lat_2=50 +ellps=WGS84";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << spec.id;
    EXPECT_EQ(0.0, pj->es) << spec.id;

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ(spec.id, info.id);
    EXPECT_THAT(pj->descr, HasSubstr(spec.expected_desc));
    EXPECT_EQ(1, info.has_inverse);
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
    EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_INV));
    EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_INV));
    EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));
  }

  // Mutable buffers for proj_create_argv (never const_cast string literals).
  char arg0[] = "proj=pconic";
  char arg1[] = "lat_1=30";
  char arg2[] = "lat_2=50";
  char arg3[] = "lat_0=40";
  char arg4[] = "R=6371000";
  std::array<char*, 5> argv = {arg0, arg1, arg2, arg3, arg4};
  ScopedPj pj_argv(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, pj_argv.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

TEST(SconicsTest, NorthernAndSouthernRoundTripsAllSevenConics) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct LonLatDeg {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLatDeg kNorthCases[] = {
      {.lon_deg = 10.0, .lat_deg = 40.0},  {.lon_deg = 25.0, .lat_deg = 45.0},
      {.lon_deg = -10.0, .lat_deg = 30.0}, {.lon_deg = 5.0, .lat_deg = 55.0},
      {.lon_deg = 40.0, .lat_deg = 20.0},
  };
  constexpr LonLatDeg kSouthCases[] = {
      {.lon_deg = 135.0, .lat_deg = -40.0},
      {.lon_deg = 150.0, .lat_deg = -35.0},
      {.lon_deg = 120.0, .lat_deg = -55.0},
      {.lon_deg = 145.0, .lat_deg = -25.0},
  };

  for (const auto& spec : kAllConics) {
    // 1. Northern hemisphere cone (Q->n > 0).
    const std::string north_def = std::string("+proj=") + spec.id +
                                  " +lat_1=30 +lat_2=50 +lat_0=40 +lon_0=10 "
                                  "+R=6371000";
    ScopedPj pj_n(proj_create(ctx.get(), north_def.c_str()));
    ASSERT_NE(nullptr, pj_n.get()) << spec.id;

    if (spec.origin_at_zero_zero) {
      const PJ_COORD origin =
          proj_trans(pj_n.get(), PJ_FWD,
                     proj_coord(DegToRad(10.0), DegToRad(40.0), 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj_n.get()));
      EXPECT_NEAR(0.0, origin.xy.x, kMetricTolM) << spec.id;
      EXPECT_NEAR(0.0, origin.xy.y, kMetricTolM) << spec.id;
    }

    for (const auto& tc : kNorthCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj_n.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj_n.get())) << spec.id;
      const PJ_COORD inv = proj_trans(pj_n.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj_n.get())) << spec.id;
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad) << spec.id;
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad) << spec.id;
    }

    // 2. Southern hemisphere cone exercises Q->n < 0 branch in
    //    sconics_s_inverse() across default, MURD2, and PCONIC switches.
    const std::string south_def = std::string("+proj=") + spec.id +
                                  " +lat_1=-50 +lat_2=-30 +lat_0=-40 "
                                  "+lon_0=135 +R=6371000";
    ScopedPj pj_s(proj_create(ctx.get(), south_def.c_str()));
    ASSERT_NE(nullptr, pj_s.get()) << spec.id;

    for (const auto& tc : kSouthCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj_s.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj_s.get())) << spec.id;
      const PJ_COORD inv = proj_trans(pj_s.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj_s.get())) << spec.id;
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, kRoundTripTolRad) << spec.id;
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, kRoundTripTolRad) << spec.id;
    }
  }
}

TEST(SconicsTest, AnalyticalFormulasAndScaleFactors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Analytical central-meridian and off-meridian checks for Euler, Murdoch
  //    II, and Perspective Conic.
  ScopedPj euler(proj_create(
      ctx.get(), "+proj=euler +lat_1=30 +lat_2=50 +lat_0=40 +R=6371000"));
  ScopedPj murd2(proj_create(
      ctx.get(), "+proj=murd2 +lat_1=30 +lat_2=50 +lat_0=40 +R=6371000"));
  ScopedPj pconic(proj_create(
      ctx.get(), "+proj=pconic +lat_1=30 +lat_2=50 +lat_0=40 +R=6371000"));
  ASSERT_NE(nullptr, euler.get());
  ASSERT_NE(nullptr, murd2.get());
  ASSERT_NE(nullptr, pconic.get());

  const PJ_COORD lat50 = proj_coord(0.0, DegToRad(50.0), 0.0, 0.0);
  const PJ_COORD euler_50 = proj_trans(euler.get(), PJ_FWD, lat50);
  const PJ_COORD murd2_50 = proj_trans(murd2.get(), PJ_FWD, lat50);
  const PJ_COORD pconic_50 = proj_trans(pconic.get(), PJ_FWD, lat50);
  EXPECT_NEAR(kSphereRadiusM * DegToRad(10.0), euler_50.xy.y, kMetricTolM);
  EXPECT_NEAR(kSphereRadiusM * std::tan(DegToRad(10.0)), murd2_50.xy.y,
              kMetricTolM);
  EXPECT_NEAR(
      kSphereRadiusM * std::cos(DegToRad(10.0)) * std::tan(DegToRad(10.0)),
      pconic_50.xy.y, kMetricTolM);

  // Off-meridian Euler formula.
  const double sig = DegToRad(40.0);
  const double del = DegToRad(10.0);
  const double half_del = DegToRad(5.0);
  const double euler_n = std::sin(sig) * std::sin(del) / del;
  const double euler_rho_c =
      half_del / (std::tan(half_del) * std::tan(sig)) + sig;
  const double euler_rho_0 = euler_rho_c - DegToRad(40.0);
  const double euler_rho = euler_rho_c - DegToRad(50.0);
  const double theta = euler_n * DegToRad(15.0);
  const PJ_COORD euler_off =
      proj_trans(euler.get(), PJ_FWD,
                 proj_coord(DegToRad(15.0), DegToRad(50.0), 0.0, 0.0));
  EXPECT_NEAR(kSphereRadiusM * euler_rho * std::sin(theta), euler_off.xy.x,
              kMetricTolM);
  EXPECT_NEAR(kSphereRadiusM * (euler_rho_0 - euler_rho * std::cos(theta)),
              euler_off.xy.y, kMetricTolM);

  // 2. All seven conics have orthogonal meridians/parallels; simple equidistant
  //    conics (euler, murd1, murd3, vitk1, tissot) maintain meridional_scale ==
  //    1.0 everywhere.
  for (const auto& spec : kAllConics) {
    const std::string def = std::string("+proj=") + spec.id +
                            " +lat_1=30 +lat_2=50 +lat_0=40 +R=6371000";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << spec.id;
    const PJ_FACTORS fac = proj_factors(
        pj.get(), proj_coord(DegToRad(12.0), DegToRad(42.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << spec.id;
    EXPECT_GT(fac.meridional_scale, 0.0) << spec.id;
    EXPECT_GT(fac.parallel_scale, 0.0) << spec.id;
    EXPECT_GT(fac.areal_scale, 0.0) << spec.id;
    EXPECT_NEAR(kHalfPi, fac.meridian_parallel_angle, 1e-7) << spec.id;
    if (std::string(spec.id) != "murd2" && std::string(spec.id) != "pconic") {
      EXPECT_NEAR(1.0, fac.meridional_scale, 1e-7) << spec.id;
    }
  }

  const PJ_FACTORS null_fac =
      proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);
}

TEST(SconicsTest, Transforms3D4DBatchGenericAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=euler +lat_1=30 +lat_2=50 +lat_0=40 +x_0=200000 +y_0=-100000 "
      "+R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t; PJ_IDENT is no-op.
  const PJ_COORD in_4d =
      proj_coord(DegToRad(8.0), DegToRad(44.0), 150.5, 2025.5);
  const PJ_COORD ident = proj_trans(pj.get(), PJ_IDENT, in_4d);
  EXPECT_DOUBLE_EQ(in_4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(in_4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(in_4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(in_4d.v[3], ident.v[3]);

  const PJ_COORD fwd_4d = proj_trans(pj.get(), PJ_FWD, in_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(150.5, fwd_4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2025.5, fwd_4d.xyzt.t);

  const PJ_COORD inv_4d = proj_trans(pj.get(), PJ_INV, fwd_4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpzt.lam, inv_4d.lpzt.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpzt.phi, inv_4d.lpzt.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(150.5, inv_4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2025.5, inv_4d.lpzt.t);

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
  EXPECT_DOUBLE_EQ(150.5, xyz3d.z);
  const PJ_LPZ lpz3d = pj_inv3d(xyz3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in_4d.lpz.lam, lpz3d.lam, kRoundTripTolRad);
  EXPECT_NEAR(in_4d.lpz.phi, lpz3d.phi, kRoundTripTolRad);
  EXPECT_DOUBLE_EQ(150.5, lpz3d.z);

  // 3. Batch proj_trans_array and strided proj_trans_generic.
  std::array<PJ_COORD, 2> valid_batch = {
      proj_coord(0.0, DegToRad(40.0), 0.0, 0.0),
      proj_coord(DegToRad(15.0), DegToRad(45.0), 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_FWD, valid_batch.size(),
                                valid_batch.data()));
  ASSERT_EQ(0, proj_trans_array(pj.get(), PJ_INV, valid_batch.size(),
                                valid_batch.data()));
  EXPECT_NEAR(0.0, valid_batch[0].lp.lam, kRoundTripTolRad);
  EXPECT_NEAR(DegToRad(40.0), valid_batch[0].lp.phi, kRoundTripTolRad);

  std::array<PJ_COORD, 2> mixed_batch = {
      proj_coord(0.0, DegToRad(40.0), 0.0, 0.0),
      proj_coord(0.0, 2.0, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
            proj_trans_array(pj.get(), PJ_FWD, mixed_batch.size(),
                             mixed_batch.data()));
  EXPECT_FALSE(std::isinf(mixed_batch[0].xy.x));
  EXPECT_TRUE(std::isinf(mixed_batch[1].xy.x));
  proj_errno_reset(pj.get());

  std::array<double, 2> xs = {0.0, DegToRad(10.0)};
  std::array<double, 2> ys = {DegToRad(40.0), DegToRad(48.0)};
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, xs.data(), sizeof(double),
                                   xs.size(), ys.data(), sizeof(double),
                                   ys.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(200000.0, xs[0], kMetricTolM);
  EXPECT_NEAR(-100000.0, ys[0], kMetricTolM);

  // 4. Edge inputs: nullptr PJ, NaN, Inf.
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

  const PJ_COORD inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_inv.lp.lam));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
}

// TODO(schwehr): Only the calloc OOM guard in pj_sconics_setup remains
// uncovered without allocator fault injection.

}  // namespace
}  // namespace proj
