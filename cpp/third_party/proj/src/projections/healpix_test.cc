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
// Unit tests for the HEALPix and rHEALPix equal-area projections
// (proj/src/projections/healpix.cpp), covering rot_xy, north_square and
// south_square parameter validation, all polar caps and equatorial bands,
// spherical and ellipsoidal transforms, domain checks, and distortion factors.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/healpix.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numbers>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;
constexpr double kQuarterPi = kPi / 4.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

double WrapAngleDiff(double a, double b) {
  double diff = std::fmod(a - b + kPi, 2.0 * kPi);
  if (diff < 0.0) {
    diff += 2.0 * kPi;
  }
  return diff - kPi;
}

struct LonLatCase {
  double lon_deg;
  double lat_deg;
};

// Covers the equatorial belt (|phi| <= asin(2/3) ~ 41.8103 deg), transition
// vicinity (41.0 and 42.5 deg), all 4 northern and southern polar quadrants,
// and the +180 deg antimeridian polar clamping path (cn >= 4).
constexpr LonLatCase kRoundTripCases[] = {
    {.lon_deg = -150.0, .lat_deg = -35.0},
    {.lon_deg = -45.0, .lat_deg = 0.0},
    {.lon_deg = 0.0, .lat_deg = 0.0},
    {.lon_deg = 45.0, .lat_deg = 30.0},
    {.lon_deg = 135.0, .lat_deg = -20.0},
    {.lon_deg = 30.0, .lat_deg = 41.0},
    {.lon_deg = 30.0, .lat_deg = 42.5},
    {.lon_deg = -30.0, .lat_deg = -41.0},
    {.lon_deg = -30.0, .lat_deg = -42.5},
    {.lon_deg = -135.0, .lat_deg = 65.0},
    {.lon_deg = -45.0, .lat_deg = 65.0},
    {.lon_deg = 45.0, .lat_deg = 65.0},
    {.lon_deg = 135.0, .lat_deg = 65.0},
    {.lon_deg = -135.0, .lat_deg = -65.0},
    {.lon_deg = -45.0, .lat_deg = -65.0},
    {.lon_deg = 45.0, .lat_deg = -65.0},
    {.lon_deg = 135.0, .lat_deg = -65.0},
    {.lon_deg = 179.9, .lat_deg = 60.0},
    {.lon_deg = -179.9, .lat_deg = -60.0},
    {.lon_deg = 180.0, .lat_deg = 60.0},
    {.lon_deg = 180.0, .lat_deg = -60.0},
    {.lon_deg = 20.0, .lat_deg = 89.0},
    {.lon_deg = -20.0, .lat_deg = -89.0},
    {.lon_deg = 0.0, .lat_deg = 90.0},
    {.lon_deg = 0.0, .lat_deg = -90.0},
};

void AssertRoundTripCases(PJ* pj, const LonLatCase* cases, size_t count,
                          double tol_rad) {
  for (size_t i = 0; i < count; ++i) {
    const double lon_rad = DegToRad(cases[i].lon_deg);
    const double lat_rad = DegToRad(cases[i].lat_deg);

    // 1. Forward: geodetic (lon, lat) in radians -> projected (x, y) in meters.
    const PJ_COORD fwd =
        proj_trans(pj, PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    ASSERT_EQ(0, proj_errno(pj)) << "Forward failed at (" << cases[i].lon_deg
                                 << ", " << cases[i].lat_deg << ")";
    ASSERT_FALSE(std::isinf(fwd.xy.x) || std::isnan(fwd.xy.x));
    ASSERT_FALSE(std::isinf(fwd.xy.y) || std::isnan(fwd.xy.y));

    // 2. Inverse: projected (x, y) in meters -> geodetic (lon, lat) in radians.
    const PJ_COORD inv = proj_trans(pj, PJ_INV, fwd);
    ASSERT_EQ(0, proj_errno(pj)) << "Inverse failed at (" << cases[i].lon_deg
                                 << ", " << cases[i].lat_deg << ")";
    EXPECT_NEAR(lat_rad, inv.lp.phi, tol_rad)
        << "Latitude mismatch at (" << cases[i].lon_deg << ", "
        << cases[i].lat_deg << ")";
    if (std::abs(std::abs(lat_rad) - kHalfPi) > 1e-9) {
      EXPECT_NEAR(0.0, WrapAngleDiff(inv.lp.lam, lon_rad), tol_rad)
          << "Longitude mismatch at (" << cases[i].lon_deg << ", "
          << cases[i].lat_deg << ")";
    }
  }
}

TEST(HealpixTest, MetadataAndAngularConvention) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj healpix(proj_create(ctx.get(), "+proj=healpix +R=1"));
  ScopedPj rhealpix(proj_create(ctx.get(), "+proj=rhealpix +R=1"));
  ASSERT_NE(nullptr, healpix.get());
  ASSERT_NE(nullptr, rhealpix.get());

  const PJ_PROJ_INFO h_info = proj_pj_info(healpix.get());
  EXPECT_STREQ("healpix", h_info.id);
  EXPECT_NE(nullptr, h_info.description);
  EXPECT_EQ(1, h_info.has_inverse);

  const PJ_PROJ_INFO r_info = proj_pj_info(rhealpix.get());
  EXPECT_STREQ("rhealpix", r_info.id);
  EXPECT_NE(nullptr, r_info.description);
  EXPECT_EQ(1, r_info.has_inverse);

  for (PJ* pj : {healpix.get(), rhealpix.get()}) {
    EXPECT_EQ(1, proj_angular_input(pj, PJ_FWD));
    EXPECT_EQ(0, proj_angular_output(pj, PJ_FWD));
    EXPECT_EQ(0, proj_angular_input(pj, PJ_INV));
    EXPECT_EQ(1, proj_angular_output(pj, PJ_INV));
  }

  const char* r_str =
      proj_as_proj_string(ctx.get(), rhealpix.get(), PJ_PROJ_5, nullptr);
  ASSERT_NE(nullptr, r_str);
  EXPECT_THAT(std::string(r_str), HasSubstr("+proj=rhealpix"));
}

TEST(HealpixTest, ParameterAndEllipsoidInitialization) {
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());

    ScopedPj h_sph(proj_create(ctx.get(), "+proj=healpix +R=1"));
    ScopedPj h_rot(proj_create(ctx.get(), "+proj=healpix +R=1 +rot_xy=45"));
    ScopedPj h_ell(
        proj_create(ctx.get(), "+proj=healpix +ellps=WGS84 +rot_xy=-30"));
    ScopedPj r_sph(proj_create(ctx.get(), "+proj=rhealpix +R=1"));
    ScopedPj r_ell(proj_create(ctx.get(),
                               "+proj=rhealpix +ellps=WGS84 +north_square=2 "
                               "+south_square=3"));
    EXPECT_NE(nullptr, h_sph.get());
    EXPECT_NE(nullptr, h_rot.get());
    EXPECT_NE(nullptr, h_ell.get());
    EXPECT_NE(nullptr, r_sph.get());
    EXPECT_NE(nullptr, r_ell.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));

    // Mutable buffers required by proj_create_argv.
    char a0[] = "proj=rhealpix";
    char a1[] = "north_square=2";
    char a2[] = "south_square=3";
    char a3[] = "R=1";
    char* argv[] = {a0, a1, a2, a3};
    ScopedPj r_argv(proj_create_argv(ctx.get(), 4, argv));
    EXPECT_NE(nullptr, r_argv.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
  }

  struct InvalidRhealpixCase {
    const char* def;
    const char* expected_msg;
  };
  constexpr InvalidRhealpixCase kInvalidCases[] = {
      {
          .def = "+proj=rhealpix +R=1 +north_square=4",
          .expected_msg = "Invalid value for north_square",
      },
      {
          .def = "+proj=rhealpix +R=1 +south_square=4",
          .expected_msg = "Invalid value for south_square",
      },
      {
          .def = "+proj=rhealpix +R=1 +north_square=-1",
          .expected_msg = nullptr,
      },
      {
          .def = "+proj=rhealpix +R=1 +south_square=-1",
          .expected_msg = nullptr,
      },
      {
          .def = "+proj=rhealpix +R=1 +north_square=invalid",
          .expected_msg = nullptr,
      },
  };
  for (const auto& tc : kInvalidCases) {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    ScopedPj invalid(proj_create(ctx.get(), tc.def));
    EXPECT_EQ(nullptr, invalid.get()) << tc.def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << tc.def;
    if (tc.expected_msg != nullptr) {
      EXPECT_TRUE(capture.ContainsMessage(tc.expected_msg)) << tc.def;
    }
  }
}

TEST(HealpixTest, HealpixSphericalKnownValuesAndRotXy) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=healpix +R=1"));
  ScopedPj pj_rot(proj_create(ctx.get(), "+proj=healpix +R=1 +rot_xy=45"));
  ScopedPj pj_scaled(proj_create(ctx.get(), "+proj=healpix +R=6371000"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj_rot.get());
  ASSERT_NE(nullptr, pj_scaled.get());

  struct KnownCase {
    double lon_deg;
    double lat_rad;
    double expected_x;
    double expected_y;
  };
  const double phi0 = std::asin(2.0 / 3.0);
  const KnownCase kCases[] = {
      {
          .lon_deg = 0.0,
          .lat_rad = 0.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
      },
      {
          .lon_deg = 45.0,
          .lat_rad = DegToRad(30.0),
          .expected_x = kQuarterPi,
          .expected_y = (3.0 * kPi / 8.0) * 0.5,
      },
      {
          // Exact equatorial/polar transition latitude phi0 = asin(2/3).
          .lon_deg = 0.0,
          .lat_rad = phi0,
          .expected_x = 0.0,
          .expected_y = kQuarterPi,
      },
      {
          .lon_deg = 60.0,
          .lat_rad = DegToRad(60.0),
          .expected_x = 0.9513723245668952,
          .expected_y = 1.0728738432865557,
      },
      {
          .lon_deg = -120.0,
          .lat_rad = DegToRad(-60.0),
          .expected_x = -2.190220329022898,
          .expected_y = -1.0728738432865557,
      },
      {
          // lon = +180 deg triggers cn >= 4 clamping to cap 3.
          .lon_deg = 180.0,
          .lat_rad = DegToRad(60.0),
          .expected_x = 2.8541169737006857,
          .expected_y = 1.0728738432865557,
      },
      {
          .lon_deg = 0.0,
          .lat_rad = kHalfPi,
          .expected_x = kQuarterPi,
          .expected_y = kHalfPi,
      },
      {
          .lon_deg = 0.0,
          .lat_rad = -kHalfPi,
          .expected_x = kQuarterPi,
          .expected_y = -kHalfPi,
      },
  };

  for (const auto& tc : kCases) {
    const PJ_COORD out =
        proj_trans(pj.get(), PJ_FWD,
                   proj_coord(DegToRad(tc.lon_deg), tc.lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(tc.expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(tc.expected_y, out.xy.y, 1e-12);
  }

  // Verify +rot_xy=45 rotates unit-sphere coordinates by -45 deg in forward
  // and +45 deg back in inverse.
  const PJ_COORD rot_in = proj_coord(DegToRad(45.0), DegToRad(30.0), 0.0, 0.0);
  const PJ_COORD rot_fwd = proj_trans(pj_rot.get(), PJ_FWD, rot_in);
  EXPECT_EQ(0, proj_errno(pj_rot.get()));
  EXPECT_NEAR(0.9718806427221425, rot_fwd.xy.x, 1e-12);
  EXPECT_NEAR(-0.138840091817449, rot_fwd.xy.y, 1e-12);

  const PJ_COORD rot_inv = proj_trans(pj_rot.get(), PJ_INV, rot_fwd);
  EXPECT_EQ(0, proj_errno(pj_rot.get()));
  EXPECT_NEAR(rot_in.lp.lam, rot_inv.lp.lam, 1e-12);
  EXPECT_NEAR(rot_in.lp.phi, rot_inv.lp.phi, 1e-12);

  // Verify linear scaling by sphere radius.
  const PJ_COORD scaled_fwd = proj_trans(pj_scaled.get(), PJ_FWD, rot_in);
  EXPECT_EQ(0, proj_errno(pj_scaled.get()));
  EXPECT_NEAR(6371000.0 * kQuarterPi, scaled_fwd.xy.x, 1e-6);
  EXPECT_NEAR(6371000.0 * (3.0 * kPi / 8.0) * 0.5, scaled_fwd.xy.y, 1e-6);
}

TEST(HealpixTest, HealpixSphericalAndEllipsoidalRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Covers sphere, rotated sphere, WGS84, rotated WGS84, and high-flattening
  // ellipsoid (|n| > 0.01 exercises Newton iteration in authalic inverse).
  constexpr const char* kDefs[] = {
      "+proj=healpix +R=6371000",
      "+proj=healpix +R=6371000 +rot_xy=35",
      "+proj=healpix +ellps=WGS84",
      "+proj=healpix +ellps=WGS84 +rot_xy=-60",
      "+proj=healpix +a=6378137 +rf=20",
  };
  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    AssertRoundTripCases(pj.get(), kRoundTripCases, std::size(kRoundTripCases),
                         1e-11);
  }

  // Near-pole coordinates lose precision in sqrt(1 - sin(phi)).
  constexpr LonLatCase kNearPole[] = {
      {.lon_deg = 20.0, .lat_deg = 89.999},
      {.lon_deg = -20.0, .lat_deg = -89.999},
  };
  ScopedPj pj_ell(proj_create(ctx.get(), "+proj=healpix +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_ell.get());
  AssertRoundTripCases(pj_ell.get(), kNearPole, std::size(kNearPole), 1e-8);
}

TEST(HealpixTest, HealpixInverseBoundaryVerticesAndOutOfDomain) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_sph(proj_create(ctx.get(), "+proj=healpix +R=1"));
  ScopedPj pj_rot(proj_create(ctx.get(), "+proj=healpix +R=1 +rot_xy=45"));
  ScopedPj pj_ell(proj_create(ctx.get(), "+proj=healpix +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_sph.get());
  ASSERT_NE(nullptr, pj_rot.get());
  ASSERT_NE(nullptr, pj_ell.get());

  // 1. Exact vertex of healpixVertsJit (0, pi/4 + 1e-15) exercises pnpoly's
  // vertex equality early return.
  const PJ_COORD vertex_inv = proj_trans(
      pj_sph.get(), PJ_INV, proj_coord(0.0, kQuarterPi + 1e-15, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_NEAR(0.0, vertex_inv.lp.lam, 1e-12);
  EXPECT_NEAR(std::asin(2.0 / 3.0), vertex_inv.lp.phi, 1e-12);

  // 2. Polar seam point (pi, pi/4 + 2e-16) stays inside jittered boundary while
  // exercising cn >= 4 clamping in healpix_spherhealpix_e_inverse().
  const PJ_COORD seam_inv = proj_trans(
      pj_sph.get(), PJ_INV, proj_coord(kPi, kQuarterPi + 2e-16, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_NEAR(0.0, WrapAngleDiff(seam_inv.lp.lam, kPi), 1e-12);
  EXPECT_NEAR(std::asin(2.0 / 3.0), seam_inv.lp.phi, 1e-12);

  // 3. Exact north and south pole coordinates (|y| == pi/2) exercise the
  // fabs(y) >= M_HALFPI branch in healpix_spherhealpix_e_inverse().
  const PJ_COORD npole_inv = proj_trans(
      pj_sph.get(), PJ_INV, proj_coord(kQuarterPi, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_NEAR(-kPi, npole_inv.lp.lam, 1e-12);
  EXPECT_NEAR(kHalfPi, npole_inv.lp.phi, 1e-12);

  const PJ_COORD spole_inv = proj_trans(
      pj_sph.get(), PJ_INV, proj_coord(-kQuarterPi, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_NEAR(-kPi, spole_inv.lp.lam, 1e-12);
  EXPECT_NEAR(-kHalfPi, spole_inv.lp.phi, 1e-12);

  // 4. Out-of-image inverse coordinates fail with OUTSIDE_PROJECTION_DOMAIN
  // on spherical, rotated, and ellipsoidal projections.
  constexpr std::array<PJ_XY, 4> kOutsideSph = {{
      {.x = 0.0, .y = 1.2},   // notch above equatorial band at lam=0
      {.x = 0.0, .y = -1.2},  // notch below equatorial band at lam=0
      {.x = 4.0, .y = 0.0},   // east of +pi
      {.x = -4.0, .y = 0.0},  // west of -pi
  }};
  for (const PJ_XY& pt : kOutsideSph) {
    const PJ_COORD inv =
        proj_trans(pj_sph.get(), PJ_INV, proj_coord(pt.x, pt.y, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inv.lp.lam);
    EXPECT_EQ(HUGE_VAL, inv.lp.phi);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj_sph.get()));
    proj_errno_reset(pj_sph.get());
  }

  // Verify rotated inverse rejects unrotated outside points after rotation.
  const PJ_COORD rot_out =
      proj_trans(pj_rot.get(), PJ_INV, proj_coord(5.0, 5.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, rot_out.lp.lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_rot.get()));
  proj_errno_reset(pj_rot.get());

  // Ellipsoidal HEALPix uses authalic radius ~6.371e6 m, so (0, 8e6) lies in
  // the polar notch outside the sawtooth domain.
  const PJ_COORD ell_out =
      proj_trans(pj_ell.get(), PJ_INV, proj_coord(0.0, 8.0e6, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, ell_out.lp.lam);
  EXPECT_EQ(HUGE_VAL, ell_out.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_ell.get()));
  proj_errno_reset(pj_ell.get());
}

TEST(HealpixTest, RhealpixKnownValuesAllCapsAndSubTriangles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Verify exact forward coordinates for shifted polar squares.
  ScopedPj pj_12(proj_create(
      ctx.get(), "+proj=rhealpix +R=1 +north_square=1 +south_square=2"));
  ASSERT_NE(nullptr, pj_12.get());

  struct KnownRhealpixCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  constexpr KnownRhealpixCase kKnown12[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0, .expected_x = 0.0, .expected_y = 0.0},
      {.lon_deg = -135.0,
       .lat_deg = 65.0,
       .expected_x = -1.2017900485423683,
       .expected_y = kHalfPi},
      {.lon_deg = -45.0,
       .lat_deg = 65.0,
       .expected_x = -kQuarterPi,
       .expected_y = 1.1544044416499766},
      {.lon_deg = 45.0,
       .lat_deg = 65.0,
       .expected_x = -0.3690062782525283,
       .expected_y = kHalfPi},
      {.lon_deg = 135.0,
       .lat_deg = 65.0,
       .expected_x = -kQuarterPi,
       .expected_y = 1.9871882119398165},
      {.lon_deg = -135.0,
       .lat_deg = -65.0,
       .expected_x = kQuarterPi,
       .expected_y = -1.9871882119398165},
      {.lon_deg = -45.0,
       .lat_deg = -65.0,
       .expected_x = 0.3690062782525283,
       .expected_y = -kHalfPi},
      {.lon_deg = 45.0,
       .lat_deg = -65.0,
       .expected_x = kQuarterPi,
       .expected_y = -1.1544044416499766},
      {.lon_deg = 135.0,
       .lat_deg = -65.0,
       .expected_x = 1.2017900485423683,
       .expected_y = -kHalfPi},
  };
  for (const auto& tc : kKnown12) {
    const PJ_COORD out = proj_trans(
        pj_12.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj_12.get()));
    EXPECT_NEAR(tc.expected_x, out.xy.x, 1e-12);
    EXPECT_NEAR(tc.expected_y, out.xy.y, 1e-12);
  }

  // 2. Sweep all 16 (north_square, south_square) combinations for both
  // spherical and ellipsoidal rHEALPix so every get_rotate_index() case
  // (-3..3) and every polar cap sub-triangle branch is exercised.
  for (int ns = 0; ns < 4; ++ns) {
    for (int ss = 0; ss < 4; ++ss) {
      const std::string sph_def =
          "+proj=rhealpix +R=6371000 +north_square=" + std::to_string(ns) +
          " +south_square=" + std::to_string(ss);
      const std::string ell_def =
          "+proj=rhealpix +ellps=WGS84 +north_square=" + std::to_string(ns) +
          " +south_square=" + std::to_string(ss);

      ScopedPj pj_sph(proj_create(ctx.get(), sph_def.c_str()));
      ScopedPj pj_ell(proj_create(ctx.get(), ell_def.c_str()));
      ASSERT_NE(nullptr, pj_sph.get()) << sph_def;
      ASSERT_NE(nullptr, pj_ell.get()) << ell_def;

      AssertRoundTripCases(pj_sph.get(), kRoundTripCases,
                           std::size(kRoundTripCases), 1e-11);
      AssertRoundTripCases(pj_ell.get(), kRoundTripCases,
                           std::size(kRoundTripCases), 1e-11);
    }
  }
}

TEST(HealpixTest, RhealpixOutOfDomainAndVertexBoundary) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_sph(proj_create(
      ctx.get(), "+proj=rhealpix +R=1 +north_square=1 +south_square=2"));
  ScopedPj pj_ell(proj_create(ctx.get(),
                              "+proj=rhealpix +ellps=WGS84 +north_square=1 "
                              "+south_square=2"));
  ASSERT_NE(nullptr, pj_sph.get());
  ASSERT_NE(nullptr, pj_ell.get());

  // 1. Exact vertex of rhealpixVertsJit (-pi - 1e-15, pi/4 + 1e-15) matches in
  // pnpoly() and inverts without error.
  const PJ_COORD vert_inv =
      proj_trans(pj_sph.get(), PJ_INV,
                 proj_coord(-kPi - 1e-15, kQuarterPi + 1e-15, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_FALSE(std::isinf(vert_inv.lp.lam));
  EXPECT_FALSE(std::isinf(vert_inv.lp.phi));

  // 2. Points outside polar square columns (north_square=1 occupies [-pi/2, 0];
  // south_square=2 occupies [0, pi/2]) fail inverse domain check.
  constexpr std::array<PJ_XY, 4> kOutsideRhealpixSph = {{
      {.x = -3.0 * kQuarterPi, .y = kHalfPi},   // empty north column 0
      {.x = kQuarterPi, .y = kHalfPi},          // empty north column 2
      {.x = -3.0 * kQuarterPi, .y = -kHalfPi},  // empty south column 0
      {.x = -kQuarterPi, .y = -kHalfPi},        // empty south column 1
  }};
  for (const PJ_XY& pt : kOutsideRhealpixSph) {
    const PJ_COORD inv =
        proj_trans(pj_sph.get(), PJ_INV, proj_coord(pt.x, pt.y, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, inv.lp.lam);
    EXPECT_EQ(HUGE_VAL, inv.lp.phi);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj_sph.get()));
    proj_errno_reset(pj_sph.get());
  }

  // Ellipsoidal out-of-domain inverse check and recovery after reset.
  const PJ_COORD ell_out =
      proj_trans(pj_ell.get(), PJ_INV, proj_coord(1.0e8, 1.0e8, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, ell_out.lp.lam);
  EXPECT_EQ(HUGE_VAL, ell_out.lp.phi);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_ell.get()));
  proj_errno_reset(pj_ell.get());

  const PJ_COORD valid =
      proj_trans(pj_ell.get(), PJ_FWD, proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_ell.get()));
  EXPECT_FALSE(std::isinf(valid.xy.x));
}

TEST(HealpixTest, BatchTransformRoundtripFactorsAndInvalidCoord) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj h_sph(proj_create(ctx.get(), "+proj=healpix +R=6371000"));
  ScopedPj h_ell(proj_create(ctx.get(), "+proj=healpix +ellps=WGS84"));
  ScopedPj r_sph(proj_create(
      ctx.get(), "+proj=rhealpix +R=6371000 +north_square=1 +south_square=2"));
  ScopedPj r_ell(proj_create(ctx.get(),
                             "+proj=rhealpix +ellps=WGS84 +north_square=1 "
                             "+south_square=2"));
  ASSERT_NE(nullptr, h_sph.get());
  ASSERT_NE(nullptr, h_ell.get());
  ASSERT_NE(nullptr, r_sph.get());
  ASSERT_NE(nullptr, r_ell.get());

  // 1. PJ_IDENT preserves input coordinate unchanged.
  const PJ_COORD orig = proj_coord(0.3, -0.4, 15.0, 2026.0);
  const PJ_COORD ident = proj_trans(r_sph.get(), PJ_IDENT, orig);
  EXPECT_EQ(0, proj_errno(r_sph.get()));
  EXPECT_DOUBLE_EQ(orig.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(orig.v[1], ident.v[1]);

  // 2. proj_trans_array forward/inverse round-trip on ellipsoidal rHEALPix.
  std::array<PJ_COORD, 3> pts = {
      proj_coord(DegToRad(15.0), DegToRad(20.0), 0.0, 0.0),
      proj_coord(DegToRad(-135.0), DegToRad(65.0), 0.0, 0.0),
      proj_coord(DegToRad(45.0), DegToRad(-65.0), 0.0, 0.0),
  };
  const std::array<PJ_COORD, 3> expected_pts = pts;
  ASSERT_EQ(0, proj_trans_array(r_ell.get(), PJ_FWD, pts.size(), pts.data()));
  ASSERT_EQ(0, proj_trans_array(r_ell.get(), PJ_INV, pts.size(), pts.data()));
  for (size_t i = 0; i < pts.size(); ++i) {
    EXPECT_NEAR(expected_pts[i].lp.lam, pts[i].lp.lam, 1e-11);
    EXPECT_NEAR(expected_pts[i].lp.phi, pts[i].lp.phi, 1e-11);
  }

  // 3. proj_trans_generic on ellipsoidal HEALPix.
  std::array<double, 2> lams = {DegToRad(30.0), DegToRad(-120.0)};
  std::array<double, 2> phis = {DegToRad(25.0), DegToRad(-60.0)};
  const std::array<double, 2> lams_orig = lams;
  const std::array<double, 2> phis_orig = phis;
  ASSERT_EQ(2u,
            proj_trans_generic(h_ell.get(), PJ_FWD, lams.data(), sizeof(double),
                               lams.size(), phis.data(), sizeof(double),
                               phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(h_ell.get()));
  ASSERT_EQ(2u,
            proj_trans_generic(h_ell.get(), PJ_INV, lams.data(), sizeof(double),
                               lams.size(), phis.data(), sizeof(double),
                               phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(h_ell.get()));
  for (size_t i = 0; i < lams.size(); ++i) {
    EXPECT_NEAR(lams_orig[i], lams[i], 1e-11);
    EXPECT_NEAR(phis_orig[i], phis[i], 1e-11);
  }

  // 4. proj_roundtrip checks.
  PJ_COORD rt_h = proj_coord(DegToRad(60.0), DegToRad(60.0), 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(h_ell.get(), PJ_FWD, 3, &rt_h), 1e-5);
  PJ_COORD rt_r = proj_coord(DegToRad(-45.0), DegToRad(65.0), 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(r_ell.get(), PJ_FWD, 3, &rt_r), 1e-5);

  // 5. Spherical HEALPix and rHEALPix have constant areal scale 3*pi/8 across
  // smooth equatorial and polar interiors.
  constexpr double kExpectedSphArealScale = 3.0 * kPi / 8.0;
  for (PJ* pj : {h_sph.get(), r_sph.get()}) {
    const PJ_FACTORS f_eq = proj_factors(pj, proj_coord(0.2, 0.2, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj));
    EXPECT_NEAR(kExpectedSphArealScale, f_eq.areal_scale, 1e-5);

    const PJ_FACTORS f_pol = proj_factors(
        pj, proj_coord(DegToRad(-135.0), DegToRad(65.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj));
    EXPECT_NEAR(kExpectedSphArealScale, f_pol.areal_scale, 1e-5);
    EXPECT_GE(f_pol.tissot_semimajor, f_pol.tissot_semiminor);
  }

  // Ellipsoidal forward uses unscaled authalic sphere formulas divided by
  // ellipsoid differential factor r/cosphi, yielding areal scale 3*pi/(4*qp).
  constexpr double kExpectedWgs84EllArealScale = 1.180735547017127;
  for (PJ* pj : {h_ell.get(), r_ell.get()}) {
    const PJ_FACTORS f_ell = proj_factors(pj, proj_coord(0.2, 0.2, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj));
    EXPECT_NEAR(kExpectedWgs84EllArealScale, f_ell.areal_scale, 1e-5);
    EXPECT_GT(f_ell.meridional_scale, 0.5);
    EXPECT_GT(f_ell.parallel_scale, 0.5);
  }

  // 6. Out-of-range latitude and NaN coordinate handling.
  const PJ_COORD bad_lat = proj_trans(
      h_sph.get(), PJ_FWD, proj_coord(0.0, DegToRad(95.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(h_sph.get()));
  proj_errno_reset(h_sph.get());

  const PJ_COORD nan_coord = proj_trans(
      r_sph.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_coord.xy.x));
  EXPECT_TRUE(std::isnan(nan_coord.xy.y));
  EXPECT_EQ(0, proj_errno(r_sph.get()));
}

}  // namespace
}  // namespace proj
