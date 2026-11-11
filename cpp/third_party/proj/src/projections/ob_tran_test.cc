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
// Unit tests for the General Oblique Transformation projection
// (proj/src/projections/ob_tran.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/ob_tran.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numbers>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTolRad = 1e-10;
constexpr double kTolMeters = 1e-3;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

void ExpectRoundTrip(PJ* pj, double lon_deg, double lat_deg, double tol_rad) {
  const PJ_COORD input =
      proj_coord(DegToRad(lon_deg), DegToRad(lat_deg), 0.0, 0.0);
  // 1. Forward: geographic (lon, lat in radians) -> rotated projected (x, y).
  const PJ_COORD fwd = proj_trans(pj, PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pj)) << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_FALSE(std::isinf(fwd.xy.x)) << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_FALSE(std::isinf(fwd.xy.y)) << "lon=" << lon_deg << " lat=" << lat_deg;

  // 2. Inverse: rotated projected (x, y) -> geographic (lon, lat in radians).
  const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj)) << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_NEAR(input.lp.lam, inv.lp.lam, tol_rad)
      << "lon=" << lon_deg << " lat=" << lat_deg;
  EXPECT_NEAR(input.lp.phi, inv.lp.phi, tol_rad)
      << "lon=" << lon_deg << " lat=" << lat_deg;
}

TEST(ObTranTest, CreateValidationErrorsAndDestructorNullBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  struct ErrorCase {
    const char* name;
    const char* proj_def;
    int expected_errno;
  };
  constexpr ErrorCase kInvalidDefs[] = {
      // Missing o_proj= triggers missing-arg failure before target creation.
      {
          .name = "missing_o_proj",
          .proj_def = "+proj=ob_tran +o_lat_p=45 +o_lon_p=10 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
      },
      // Self-recursion guard (o_proj=ob_tran) prevents unbounded recursion.
      {
          .name = "recursive_o_proj_ob_tran",
          .proj_def = "+proj=ob_tran +o_proj=ob_tran +o_lat_p=45 +o_lon_p=10 "
                      "+R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
      },
      // Unknown, empty, value-less, or failing target init gives R == nullptr.
      {
          .name = "unknown_o_proj",
          .proj_def = "+proj=ob_tran +o_proj=nonexistent_proj +o_lat_p=45 "
                      "+R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "empty_o_proj",
          .proj_def = "+proj=ob_tran +o_proj= +o_lat_p=45 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "valueless_o_proj",
          .proj_def = "+proj=ob_tran +o_proj +o_lat_p=45 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          // utm rejects spherical Earth (es == 0), failing target setup.
          .name = "target_init_failure_utm_sphere",
          .proj_def = "+proj=ob_tran +o_proj=utm +o_lat_p=45 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      // Azimuth mode rejects |o_lat_c| == 90 deg (with already-linked R freed).
      {
          .name = "alpha_lat_c_plus_90",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_alpha=30 +o_lat_c=90 +o_lon_c=10 "
              "+R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "alpha_lat_c_minus_90",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_alpha=30 +o_lat_c=-90 +o_lon_c=10 "
              "+R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      // Two-point mode validation errors (with Q->link freed on cleanup).
      {
          .name = "two_point_lat_1_plus_90",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=90 +o_lon_2=45 "
              "+o_lat_2=30 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "two_point_lat_1_minus_90",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=-90 +o_lon_2=45 "
              "+o_lat_2=30 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "two_point_lat_2_plus_90",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=30 +o_lon_2=45 "
              "+o_lat_2=90 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "two_point_lat_2_minus_90",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=30 +o_lon_2=45 "
              "+o_lat_2=-90 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "two_point_equal_lats",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=30 +o_lon_2=45 "
              "+o_lat_2=30 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
      {
          .name = "two_point_lat_1_zero",
          .proj_def =
              "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=0 +o_lon_2=45 "
              "+o_lat_2=30 +R=6371000",
          .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
      },
  };

  for (const auto& tc : kInvalidDefs) {
    ScopedPj bad(proj_create(ctx.get(), tc.proj_def));
    EXPECT_EQ(nullptr, bad.get()) << tc.name;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.name;
  }

  // Exercise the nullptr == P guard in ob_tran's custom destructor.
  ScopedPj valid(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lat_p=45 +o_lon_p=10 +R=6371000"));
  ASSERT_NE(nullptr, valid.get());
  ASSERT_NE(nullptr, valid->destructor);
  EXPECT_EQ(nullptr, valid->destructor(nullptr, 0));
}

TEST(ObTranTest, TargetParamsSkipInvAndPropagateUsedFlagsFromTarget) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Place o_proj=stere after other parameters using mutable argv buffers
  //    to exercise non-o_proj iteration in ob_tran_target_params() and transfer
  //    of target used-flags (+lat_ts=60) back onto P->params.
  char arg_proj[] = "proj=ob_tran";
  char arg_latp[] = "o_lat_p=45";
  char arg_lonp[] = "o_lon_p=10";
  char arg_oproj[] = "o_proj=merc";
  char arg_lat_ts[] = "lat_ts=60";
  char arg_unused[] = "unused_custom_param=99";
  char arg_radius[] = "R=6371000";
  char* argv[] = {arg_proj,   arg_latp,   arg_lonp,  arg_oproj,
                  arg_lat_ts, arg_unused, arg_radius};
  ScopedPj stere(
      proj_create_argv(ctx.get(), static_cast<int>(std::size(argv)), argv));
  ASSERT_NE(nullptr, stere.get());

  const PJ_PROJ_INFO info = proj_pj_info(stere.get());
  ASSERT_NE(nullptr, info.definition);
  const std::string_view def(info.definition);
  EXPECT_NE(std::string_view::npos, def.find("lat_ts=60")) << def;
  EXPECT_EQ(std::string_view::npos, def.find("unused_custom_param")) << def;

  // 2. Verify +inv on an ob_tran pipeline step is skipped cleanly by
  //    ob_tran_target_params() rather than failing target setup.
  ScopedPj pipeline(proj_create(
      ctx.get(),
      "+proj=pipeline "
      "+step +proj=ob_tran +o_proj=merc +o_lat_p=45 +o_lon_p=10 +R=6371000 "
      "+step +inv +proj=ob_tran +o_proj=merc +o_lat_p=45 +o_lon_p=10 "
      "+R=6371000"));
  ASSERT_NE(nullptr, pipeline.get());

  const PJ_COORD input = proj_coord(DegToRad(15.0), DegToRad(48.0), 0.0, 0.0);
  const PJ_COORD fwd = proj_trans(pipeline.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(pipeline.get()));
  EXPECT_NEAR(input.lp.lam, fwd.lp.lam, kTolRad);
  EXPECT_NEAR(input.lp.phi, fwd.lp.phi, kTolRad);

  const PJ_COORD inv = proj_trans(pipeline.get(), PJ_INV, input);
  EXPECT_EQ(0, proj_errno(pipeline.get()));
  EXPECT_NEAR(input.lp.lam, inv.lp.lam, kTolRad);
  EXPECT_NEAR(input.lp.phi, inv.lp.phi, kTolRad);
}

TEST(ObTranTest, ObliquePoleModeMatchesReferenceAndEllipsoidalRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Pole at (90N, 0E) acts as identity rotation matching unrotated Mercator.
  ScopedPj identity(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lat_p=90 +o_lon_p=0 +R=6371000"));
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +R=6371000"));
  ASSERT_NE(nullptr, identity.get());
  ASSERT_NE(nullptr, merc.get());

  const PJ_COORD sample =
      proj_coord(DegToRad(22.5), DegToRad(-31.25), 0.0, 0.0);
  const PJ_COORD rot_id = proj_trans(identity.get(), PJ_FWD, sample);
  const PJ_COORD ref_merc = proj_trans(merc.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(identity.get()));
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(ref_merc.xy.x, rot_id.xy.x, 1e-6);
  EXPECT_NEAR(ref_merc.xy.y, rot_id.xy.y, 1e-6);

  // 2. Rotated pole on sphere: compare against analytical spherical Mercator
  //    value (10E, 50N -> rotated (-155.5912553804 deg, -0.3166954876 deg)).
  ScopedPj oblique(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lat_p=39.25 +o_lon_p=-162 +R=6371000"));
  ASSERT_NE(nullptr, oblique.get());
  EXPECT_EQ(1, proj_pj_info(oblique.get()).has_inverse);

  const PJ_COORD fwd =
      proj_trans(oblique.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(50.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(oblique.get()));
  EXPECT_NEAR(-17300958.228557, fwd.xy.x, kTolMeters);
  EXPECT_NEAR(-35215.110825, fwd.xy.y, kTolMeters);

  // Classic 2D pj_fwd / pj_inv entry points.
  const PJ_XY xy2d =
      pj_fwd({.lam = DegToRad(10.0), .phi = DegToRad(50.0)}, oblique.get());
  EXPECT_EQ(0, proj_errno(oblique.get()));
  EXPECT_NEAR(fwd.xy.x, xy2d.x, 1e-6);
  EXPECT_NEAR(fwd.xy.y, xy2d.y, 1e-6);
  const PJ_LP lp2d = pj_inv(xy2d, oblique.get());
  EXPECT_EQ(0, proj_errno(oblique.get()));
  EXPECT_NEAR(DegToRad(10.0), lp2d.lam, kTolRad);
  EXPECT_NEAR(DegToRad(50.0), lp2d.phi, kTolRad);

  // 3. Round-trip across hemispheres and ellipsoidal WGS84 target.
  ScopedPj wgs84_oblique(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lat_p=40 +o_lon_p=-100 +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84_oblique.get());
  struct LonLat {
    double lon_deg;
    double lat_deg;
  };
  constexpr LonLat kPoints[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 10.0, .lat_deg = 50.0},
      {.lon_deg = -75.0, .lat_deg = -25.0},
      {.lon_deg = 175.0, .lat_deg = 20.0},
  };
  for (const auto& pt : kPoints) {
    ExpectRoundTrip(oblique.get(), pt.lon_deg, pt.lat_deg, kTolRad);
    ExpectRoundTrip(wgs84_oblique.get(), pt.lon_deg, pt.lat_deg, kTolRad);
  }
}

TEST(ObTranTest, AlphaModeObliqueAndTransverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Oblique azimuth mode (|phip| > TOL -> o_forward / o_inverse).
  ScopedPj oblique(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_alpha=30 +o_lat_c=40 +o_lon_c=10 "
      "+R=6371000"));
  ASSERT_NE(nullptr, oblique.get());

  const PJ_COORD fwd_ob =
      proj_trans(oblique.get(), PJ_FWD,
                 proj_coord(DegToRad(15.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(oblique.get()));
  EXPECT_NEAR(-9901615.005256, fwd_ob.xy.x, kTolMeters);
  EXPECT_NEAR(-2401783.845304, fwd_ob.xy.y, kTolMeters);
  ExpectRoundTrip(oblique.get(), 15.0, 45.0, kTolRad);
  ExpectRoundTrip(oblique.get(), -30.0, -15.0, kTolRad);

  // 2. Transverse via o_alpha=0 (phip == 0 -> t_forward / t_inverse).
  ScopedPj transverse(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_alpha=0 +o_lat_c=40 +o_lon_c=10 "
      "+R=6371000"));
  ASSERT_NE(nullptr, transverse.get());

  const PJ_COORD fwd_tr =
      proj_trans(transverse.get(), PJ_FWD,
                 proj_coord(DegToRad(15.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(transverse.get()));
  EXPECT_NEAR(-7282064.710769, fwd_tr.xy.x, kTolMeters);
  EXPECT_NEAR(-5318125.473388, fwd_tr.xy.y, kTolMeters);
  ExpectRoundTrip(transverse.get(), 15.0, 45.0, kTolRad);
  ExpectRoundTrip(transverse.get(), -40.0, 20.0, kTolRad);
}

TEST(ObTranTest, TwoPointModeObliqueAndTransverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Oblique two-point rotation.
  ScopedPj oblique(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lon_1=-45 +o_lat_1=30 +o_lon_2=45 "
      "+o_lat_2=60 +R=6371000"));
  ASSERT_NE(nullptr, oblique.get());

  const PJ_COORD fwd =
      proj_trans(oblique.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(oblique.get()));
  EXPECT_NEAR(5540355.753738, fwd.xy.x, kTolMeters);
  EXPECT_NEAR(-11698818.073758, fwd.xy.y, kTolMeters);
  ExpectRoundTrip(oblique.get(), 10.0, 45.0, kTolRad);
  ExpectRoundTrip(oblique.get(), -45.0, 30.0, kTolRad);

  // 2. Equal defining longitudes give |phip| <= TOL (transverse).
  ScopedPj transverse(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lon_1=0 +o_lat_1=30 +o_lon_2=0 "
      "+o_lat_2=60 +R=6371000"));
  ASSERT_NE(nullptr, transverse.get());
  ExpectRoundTrip(transverse.get(), 20.0, 40.0, kTolRad);
  ExpectRoundTrip(transverse.get(), -35.0, -25.0, kTolRad);
}

TEST(ObTranTest, TransversePoleModeRoundTripsAndDomainSingularity) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // +o_lat_p=0 +o_lon_p=0 enters t_forward and t_inverse.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lat_p=0 +o_lon_p=0 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  ExpectRoundTrip(pj.get(), 25.0, 30.0, kTolRad);
  ExpectRoundTrip(pj.get(), -60.0, -45.0, kTolRad);

  // At (0, 0), t_forward maps phi_rot = aasin(-1) = -pi/2, where tan(-pi/2)
  // in IEEE-754 double produces a large negative y (~-2.42e8 m) and x = 0.
  const PJ_COORD singular =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_DOUBLE_EQ(0.0, singular.xy.x);
  EXPECT_LT(singular.xy.y, -1e8);
  EXPECT_EQ(0, proj_errno(pj.get()));

  proj_errno_reset(pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  ExpectRoundTrip(pj.get(), 12.0, -34.0, kTolRad);
}

TEST(ObTranTest, TargetInverseDomainFailurePropagatesHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // Bounded target projection (orthographic) returns HUGE_VAL on inverse
  // inputs outside the disk radius, exercising lp.lam == HUGE_VAL in both
  // o_inverse and t_inverse.
  constexpr const char* kDefs[] = {
      "+proj=ob_tran +o_proj=ortho +o_lat_p=45 +o_lon_p=10 +R=6371000",
      "+proj=ob_tran +o_proj=ortho +o_lat_p=0 +o_lon_p=0 +R=6371000",
  };
  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    const PJ_COORD bad_inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(1e8, 1e8, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad_inv.lp.lam) << def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << def;
    proj_errno_reset(pj.get());
  }
}

TEST(ObTranTest, NullTargetFwdOrInvDisablesCorrespondingDirection) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Target without inverse (august): fwd works, inv is disabled.
  constexpr const char* kNoInvDefs[] = {
      "+proj=ob_tran +o_proj=august +o_lat_p=45 +o_lon_p=10 +R=6371000",
      "+proj=ob_tran +o_proj=august +o_lat_p=0 +o_lon_p=0 +R=6371000",
  };
  for (const char* def : kNoInvDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(0, proj_pj_info(pj.get()).has_inverse) << def;

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_FALSE(std::isinf(fwd.xy.x)) << def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(HUGE_VAL, inv.lp.lam) << def;
    EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get())) << def;
    proj_errno_reset(pj.get());
  }

  // 2. Target with neither 2D fwd nor inv (set only provides fwd4d/inv4d):
  //    ob_tran sees null Q->link->fwd and Q->link->inv in both oblique and
  //    transverse modes.
  constexpr const char* kNo2DOpsDefs[] = {
      "+proj=ob_tran +o_proj=set +v_1=1.0 +o_lat_p=45 +o_lon_p=10 +R=6371000",
      "+proj=ob_tran +o_proj=set +v_1=1.0 +o_lat_p=0 +o_lon_p=0 +R=6371000",
  };
  for (const char* def : kNo2DOpsDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(0, proj_pj_info(pj.get()).has_inverse) << def;

    const PJ_COORD fwd = proj_trans(
        pj.get(), PJ_FWD, proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, fwd.xy.x) << def;
    EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get())) << def;
    proj_errno_reset(pj.get());

    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(1.0, 2.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inv.lp.lam) << def;
    EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(pj.get())) << def;
    proj_errno_reset(pj.get());
  }
}

TEST(ObTranTest, LonglatTargetSetsWhateverRightUnitsWithoutLinearScaling) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kLatLongDefs[] = {
      "+proj=ob_tran +o_proj=longlat +o_lat_p=45 +o_lon_p=10 +R=6371000",
      "+proj=ob_tran +o_proj=latlong +o_lat_p=45 +o_lon_p=10 +R=6371000",
  };
  for (const char* def : kLatLongDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    // ob_tran sets P->right = PJ_IO_UNITS_WHATEVER when target outputs radians
    // so fwd_finalize/inv_prepare do not scale by P->a / P->ra.
    EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj->right) << def;
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD)) << def;
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD)) << def;

    const PJ_COORD input = proj_coord(DegToRad(20.0), DegToRad(30.0), 0.0, 0.0);
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, input);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_NEAR(0.483180615523, fwd.xy.x, kTolRad) << def;
    EXPECT_NEAR(-0.223750792648, fwd.xy.y, kTolRad) << def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_NEAR(input.lp.lam, inv.lp.lam, kTolRad) << def;
    EXPECT_NEAR(input.lp.phi, inv.lp.phi, kTolRad) << def;
  }
}

TEST(ObTranTest, BatchArrayGeneric4DFactorsAndEdgeCoords) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=ob_tran +o_proj=merc +o_lat_p=39.25 +o_lon_p=-162 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Batch transform via proj_trans_array.
  std::array<PJ_COORD, 3> coords = {
      proj_coord(DegToRad(-20.0), DegToRad(40.0), 100.0, 2020.0),
      proj_coord(DegToRad(10.0), DegToRad(50.0), 250.0, 2021.0),
      proj_coord(DegToRad(45.0), DegToRad(-15.0), -50.0, 2022.0),
  };
  const std::array<PJ_COORD, 3> originals = coords;
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (std::size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(originals[i].xyzt.x, coords[i].xyzt.x, kTolRad) << "idx=" << i;
    EXPECT_NEAR(originals[i].xyzt.y, coords[i].xyzt.y, kTolRad) << "idx=" << i;
    EXPECT_DOUBLE_EQ(originals[i].xyzt.z, coords[i].xyzt.z) << "idx=" << i;
    EXPECT_DOUBLE_EQ(originals[i].xyzt.t, coords[i].xyzt.t) << "idx=" << i;
  }

  // 2. Strided proj_trans_generic preserving z and t across 4D round-trip.
  double x = DegToRad(15.0);
  double y = DegToRad(35.0);
  double z = 321.5;
  double t = 2026.25;
  EXPECT_EQ(
      1u, proj_trans_generic(pj.get(), PJ_FWD, &x, sizeof(x), 1, &y, sizeof(y),
                             1, &z, sizeof(z), 1, &t, sizeof(t), 1));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(
      1u, proj_trans_generic(pj.get(), PJ_INV, &x, sizeof(x), 1, &y, sizeof(y),
                             1, &z, sizeof(z), 1, &t, sizeof(t), 1));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(DegToRad(15.0), x, kTolRad);
  EXPECT_NEAR(DegToRad(35.0), y, kTolRad);
  EXPECT_DOUBLE_EQ(321.5, z);
  EXPECT_DOUBLE_EQ(2026.25, t);

  // 3. Conformality via proj_factors on spherical rotated Mercator.
  const PJ_FACTORS fac = proj_factors(
      pj.get(), proj_coord(DegToRad(10.0), DegToRad(50.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_GT(fac.meridional_scale, 0.9);
  EXPECT_NEAR(fac.meridional_scale, fac.parallel_scale, 1e-6);
  EXPECT_NEAR(0.0, fac.angular_distortion, 1e-6);

  // 4. PJ_IDENT, nullptr PJ*, NaN, +Inf, and out-of-range latitude.
  const PJ_COORD ident_in = proj_coord(0.25, 0.5, 1.0, 2.0);
  const PJ_COORD ident_out = proj_trans(pj.get(), PJ_IDENT, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.xyzt.x, ident_out.xyzt.x);
  EXPECT_DOUBLE_EQ(ident_in.xyzt.y, ident_out.xyzt.y);
  const PJ_COORD null_out = proj_trans(nullptr, PJ_FWD, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.xyzt.x, null_out.xyzt.x);

  const PJ_COORD nan_out = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xy.x));

  const PJ_COORD inf_out = proj_trans(
      pj.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inf_out.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD bad_lat =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(95.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
}

// TODO(schwehr): Cover remaining defensive OOM/unreachable paths in
// proj/src/projections/ob_tran.cpp if fault injection is added:
// - destructor() lines 99-100 (P->opaque == nullptr)
// - ob_tran_target_params() line 144 (argc < 2; proj_create needs proj=ob_tran)
// - ob_tran_target_params() line 149 & PJ_PROJECTION(ob_tran) line 184 (OOM)

}  // namespace
}  // namespace proj
