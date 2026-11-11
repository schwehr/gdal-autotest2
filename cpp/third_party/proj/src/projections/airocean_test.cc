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
// Unit tests for the Fuller Airocean (Dymaxion) icosahedral projection
// (proj/src/projections/airocean.cpp), covering vertical/horizontal
// orientations, all 23 faces/subfaces, spherical and ellipsoidal formulas,
// out-of-domain guards, batch transforms, and distortion factors.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/airocean.cpp

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
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

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

// Interior points for all 23 faces and subfaces of the modified icosahedron
// (faces 0..17 cover the standard faces and split Australia; faces 18..22
// cover the remaining split subfaces around Australia and Japan), plus
// equator, antimeridian, and poles.
constexpr LonLatCase kAllFaceAndBoundaryCases[] = {
    {.lon_deg = 23.032, .lat_deg = 28.173},     // face 0
    {.lon_deg = 71.528, .lat_deg = 46.042},     // face 1
    {.lon_deg = 145.356, .lat_deg = 74.928},    // face 2
    {.lon_deg = -77.207, .lat_deg = 60.433},    // face 3
    {.lon_deg = -25.818, .lat_deg = 34.388},    // face 4
    {.lon_deg = 29.020, .lat_deg = -13.233},    // face 5
    {.lon_deg = 71.775, .lat_deg = -24.487},    // face 6
    {.lon_deg = 96.151, .lat_deg = 9.898},      // face 7
    {.lon_deg = 169.247, .lat_deg = 34.717},    // face 8
    {.lon_deg = -150.980, .lat_deg = 13.233},   // face 9
    {.lon_deg = -108.225, .lat_deg = 24.487},   // face 10
    {.lon_deg = -83.849, .lat_deg = -9.898},    // face 11
    {.lon_deg = -42.022, .lat_deg = -4.530},    // face 12
    {.lon_deg = -10.753, .lat_deg = -34.717},   // face 13
    {.lon_deg = 154.182, .lat_deg = -34.388},   // face 14
    {.lon_deg = -156.968, .lat_deg = -28.173},  // face 15
    {.lon_deg = -108.472, .lat_deg = -46.042},  // face 16
    {.lon_deg = -34.644, .lat_deg = -74.928},   // face 17
    {.lon_deg = 98.080, .lat_deg = -48.295},    // face 18
    {.lon_deg = 113.096, .lat_deg = -72.205},   // face 19
    {.lon_deg = 142.927, .lat_deg = -8.877},    // face 20
    {.lon_deg = 123.802, .lat_deg = 6.798},     // face 21
    {.lon_deg = 147.406, .lat_deg = 15.400},    // face 22
    {.lon_deg = 0.0, .lat_deg = 0.0},
    {.lon_deg = 90.0, .lat_deg = 0.0},
    {.lon_deg = -90.0, .lat_deg = 0.0},
    {.lon_deg = 180.0, .lat_deg = 0.0},
    {.lon_deg = -180.0, .lat_deg = 0.0},
    {.lon_deg = 0.0, .lat_deg = 90.0},
    {.lon_deg = 0.0, .lat_deg = -90.0},
};

void AssertRoundTripAllCases(PJ* pj, const LonLatCase* cases, size_t count,
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

TEST(AiroceanTest, MetadataAndAngularConvention) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=airocean +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("airocean", info.id);
  EXPECT_NE(nullptr, info.description);
  EXPECT_EQ(1, info.has_inverse);

  EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(pj.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_INV));

  const char* proj_str =
      proj_as_proj_string(ctx.get(), pj.get(), PJ_PROJ_5, nullptr);
  ASSERT_NE(nullptr, proj_str);
  EXPECT_THAT(std::string(proj_str), HasSubstr("+proj=airocean"));
}

TEST(AiroceanTest, OrientParameterValidation) {
  {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedPj pj_default(proj_create(ctx.get(), "+proj=airocean +R=1"));
    EXPECT_NE(nullptr, pj_default.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));

    ScopedPj pj_vertical(
        proj_create(ctx.get(), "+proj=airocean +orient=vertical +R=1"));
    EXPECT_NE(nullptr, pj_vertical.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));

    ScopedPj pj_horizontal(
        proj_create(ctx.get(), "+proj=airocean +orient=horizontal +R=1"));
    EXPECT_NE(nullptr, pj_horizontal.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));

    // Mutable buffers required by proj_create_argv.
    char arg0[] = "proj=airocean";
    char arg1[] = "orient=horizontal";
    char arg2[] = "R=1";
    char* argv[] = {arg0, arg1, arg2};
    ScopedPj pj_argv(proj_create_argv(ctx.get(), 3, argv));
    EXPECT_NE(nullptr, pj_argv.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
  }

  constexpr const char* kInvalidDefs[] = {
      "+proj=airocean +orient=diagonal +R=1",
      "+proj=airocean +orient= +R=1",
      "+proj=airocean +orient +R=1",
  };
  for (const char* def : kInvalidDefs) {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    ScopedPj invalid(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, invalid.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
    EXPECT_TRUE(capture.ContainsMessage("Invalid value for orient")) << def;
  }
}

TEST(AiroceanTest, KnownCoordinatesVerticalAndHorizontalOrientations) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_default(proj_create(ctx.get(), "+proj=airocean +R=1"));
  ScopedPj pj_vertical(
      proj_create(ctx.get(), "+proj=airocean +orient=vertical +R=1"));
  ScopedPj pj_horizontal(
      proj_create(ctx.get(), "+proj=airocean +orient=horizontal +R=1"));
  ScopedPj pj_scaled(proj_create(ctx.get(), "+proj=airocean +R=6371000"));
  ASSERT_NE(nullptr, pj_default.get());
  ASSERT_NE(nullptr, pj_vertical.get());
  ASSERT_NE(nullptr, pj_horizontal.get());
  ASSERT_NE(nullptr, pj_scaled.get());

  struct KnownCase {
    double lon_deg;
    double lat_deg;
    double expected_x_vert;
    double expected_y_vert;
  };
  constexpr KnownCase kKnown[] = {
      {
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_x_vert = 2.670600194055548,
          .expected_y_vert = 3.780079365687846,
      },
      {
          .lon_deg = 23.032,
          .lat_deg = 28.173,
          .expected_x_vert = 2.124721061560691,
          .expected_y_vert = 3.680118409676239,
      },
  };
  // Translation constant in orient_horizontal_trans from airocean.cpp.
  constexpr double kHorizOffset = 5.78304223331047;
  constexpr double kRadiusM = 6371000.0;

  for (const auto& tc : kKnown) {
    const PJ_COORD in =
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);

    const PJ_COORD out_def = proj_trans(pj_default.get(), PJ_FWD, in);
    const PJ_COORD out_vert = proj_trans(pj_vertical.get(), PJ_FWD, in);
    const PJ_COORD out_horiz = proj_trans(pj_horizontal.get(), PJ_FWD, in);
    const PJ_COORD out_scaled = proj_trans(pj_scaled.get(), PJ_FWD, in);

    EXPECT_EQ(0, proj_errno(pj_default.get()));
    EXPECT_EQ(0, proj_errno(pj_vertical.get()));
    EXPECT_EQ(0, proj_errno(pj_horizontal.get()));
    EXPECT_EQ(0, proj_errno(pj_scaled.get()));

    EXPECT_NEAR(tc.expected_x_vert, out_def.xy.x, 1e-12);
    EXPECT_NEAR(tc.expected_y_vert, out_def.xy.y, 1e-12);
    EXPECT_NEAR(out_def.xy.x, out_vert.xy.x, 1e-14);
    EXPECT_NEAR(out_def.xy.y, out_vert.xy.y, 1e-14);

    // orient_horizontal_trans rotates 90 deg CW in (x, y) and shifts x.
    EXPECT_NEAR(-tc.expected_y_vert + kHorizOffset, out_horiz.xy.x, 1e-12);
    EXPECT_NEAR(tc.expected_x_vert, out_horiz.xy.y, 1e-12);

    // Classic unit scaling multiplies unit-sphere coordinates by R.
    EXPECT_NEAR(tc.expected_x_vert * kRadiusM, out_scaled.xy.x, 1e-6);
    EXPECT_NEAR(tc.expected_y_vert * kRadiusM, out_scaled.xy.y, 1e-6);
  }
}

TEST(AiroceanTest, All23FacesRoundTripSphericalVerticalAndHorizontal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      "+proj=airocean +R=6371000",
      "+proj=airocean +R=6371000 +orient=vertical",
      "+proj=airocean +R=6371000 +orient=horizontal",
  };
  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    AssertRoundTripAllCases(pj.get(), kAllFaceAndBoundaryCases,
                            std::size(kAllFaceAndBoundaryCases), 1e-12);
  }
}

TEST(AiroceanTest, All23FacesRoundTripEllipsoidalVerticalAndHorizontal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Near-pole latitudes verify ellipsoidal conversion away from exact poles.
  constexpr LonLatCase kNearPoleCases[] = {
      {.lon_deg = 45.0, .lat_deg = 89.9999},
      {.lon_deg = -45.0, .lat_deg = -89.9999},
  };

  constexpr const char* kDefs[] = {
      "+proj=airocean +ellps=WGS84",
      "+proj=airocean +ellps=WGS84 +orient=horizontal",
  };
  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    AssertRoundTripAllCases(pj.get(), kAllFaceAndBoundaryCases,
                            std::size(kAllFaceAndBoundaryCases), 1e-12);
    // Near-pole longitude and acos(-s) amplify ULP differences by ~1/cos(phi).
    AssertRoundTripAllCases(pj.get(), kNearPoleCases, std::size(kNearPoleCases),
                            1e-8);
  }

  // Verify geodetic->geocentric shift alters mid-latitude output compared to
  // a sphere with the same semimajor axis, while matching on the equator.
  ScopedPj pj_sph(proj_create(ctx.get(), "+proj=airocean +R=6378137"));
  ScopedPj pj_ell(proj_create(ctx.get(), "+proj=airocean +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_sph.get());
  ASSERT_NE(nullptr, pj_ell.get());

  const PJ_COORD mid = proj_coord(DegToRad(23.032), DegToRad(28.173), 0.0, 0.0);
  const PJ_COORD mid_sph = proj_trans(pj_sph.get(), PJ_FWD, mid);
  const PJ_COORD mid_ell = proj_trans(pj_ell.get(), PJ_FWD, mid);
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_EQ(0, proj_errno(pj_ell.get()));
  EXPECT_GT(
      std::hypot(mid_sph.xy.x - mid_ell.xy.x, mid_sph.xy.y - mid_ell.xy.y),
      1e3);

  const PJ_COORD eq = proj_coord(0.0, 0.0, 0.0, 0.0);
  const PJ_COORD eq_sph = proj_trans(pj_sph.get(), PJ_FWD, eq);
  const PJ_COORD eq_ell = proj_trans(pj_ell.get(), PJ_FWD, eq);
  EXPECT_NEAR(eq_sph.xy.x, eq_ell.xy.x, 1e-9);
  EXPECT_NEAR(eq_sph.xy.y, eq_ell.xy.y, 1e-9);
}

TEST(AiroceanTest, InverseOutsideNetAndForwardDomainErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  constexpr const char* kDefs[] = {
      "+proj=airocean +R=1",
      "+proj=airocean +R=1 +orient=horizontal",
      "+proj=airocean +ellps=WGS84",
      "+proj=airocean +ellps=WGS84 +orient=horizontal",
  };
  constexpr std::array<PJ_XY, 3> kOutsideNet = {{
      {.x = 0.0, .y = 0.0},
      {.x = -1.0, .y = -1.0},
      {.x = 1e8, .y = 1e8},
  }};

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    for (const PJ_XY& pt : kOutsideNet) {
      const PJ_COORD inv =
          proj_trans(pj.get(), PJ_INV, proj_coord(pt.x, pt.y, 0.0, 0.0));
      EXPECT_EQ(HUGE_VAL, inv.lp.lam) << def;
      EXPECT_EQ(HUGE_VAL, inv.lp.phi) << def;
      EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
                proj_errno(pj.get()))
          << def;
      proj_errno_reset(pj.get());
    }

    // Verify recovery after error reset on a valid coordinate.
    const PJ_COORD valid =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(valid.xy.x));

    // Out-of-range latitude triggers PROJ_ERR_COORD_TRANSFM_INVALID_COORD.
    const PJ_COORD bad_lat =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(95.0), 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad_lat.xy.x);
    EXPECT_EQ(HUGE_VAL, bad_lat.xy.y);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
    proj_errno_reset(pj.get());

    // proj_trans propagates NaN coordinates cleanly without setting errno.
    const double nan_val = std::numeric_limits<double>::quiet_NaN();
    const PJ_COORD nan_res =
        proj_trans(pj.get(), PJ_FWD, proj_coord(nan_val, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isnan(nan_res.xy.x));
    EXPECT_TRUE(std::isnan(nan_res.xy.y));
    EXPECT_EQ(0, proj_errno(pj.get()));

    // Direct pj_fwd with NaN bypasses proj_trans's NaN short-circuit and
    // exercises get_ico_face_index() == 23 inside airocean_forward().
    const PJ_XY nan_fwd = pj_fwd(PJ_LP{.lam = nan_val, .phi = 0.0}, pj.get());
    EXPECT_EQ(HUGE_VAL, nan_fwd.x);
    EXPECT_EQ(HUGE_VAL, nan_fwd.y);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }
}

TEST(AiroceanTest, BatchArrayGenericRoundtripFactorsAndParameters) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_sph(proj_create(ctx.get(), "+proj=airocean +R=6371000"));
  ScopedPj pj_ell(proj_create(ctx.get(), "+proj=airocean +ellps=WGS84"));
  ScopedPj pj_offset(proj_create(
      ctx.get(),
      "+proj=airocean +R=6371000 +lon_0=30 +x_0=1000 +y_0=-2000 +units=km"));
  ASSERT_NE(nullptr, pj_sph.get());
  ASSERT_NE(nullptr, pj_ell.get());
  ASSERT_NE(nullptr, pj_offset.get());

  // 1. PJ_IDENT leaves input untouched.
  const PJ_COORD orig = proj_coord(0.4, 0.5, 12.0, 2026.0);
  const PJ_COORD ident = proj_trans(pj_sph.get(), PJ_IDENT, orig);
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_DOUBLE_EQ(orig.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(orig.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(orig.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(orig.v[3], ident.v[3]);

  // 2. proj_trans_array forward and inverse batch round-trip.
  std::array<PJ_COORD, 3> batch = {
      proj_coord(DegToRad(23.032), DegToRad(28.173), 0.0, 0.0),
      proj_coord(DegToRad(-77.207), DegToRad(60.433), 0.0, 0.0),
      proj_coord(DegToRad(98.080), DegToRad(-48.295), 0.0, 0.0),
  };
  const std::array<PJ_COORD, 3> expected_batch = batch;
  ASSERT_EQ(0,
            proj_trans_array(pj_ell.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj_ell.get(), PJ_INV, batch.size(), batch.data()));
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(expected_batch[i].lp.lam, batch[i].lp.lam, 1e-12);
    EXPECT_NEAR(expected_batch[i].lp.phi, batch[i].lp.phi, 1e-12);
  }

  // Partial failure in inverse array sets failed element to HUGE_VAL and
  // returns PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  std::array<PJ_COORD, 2> mixed_inv = {
      proj_trans(pj_sph.get(), PJ_FWD,
                 proj_coord(DegToRad(23.032), DegToRad(28.173), 0.0, 0.0)),
      proj_coord(-1e7, -1e7, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(pj_sph.get(), PJ_INV, mixed_inv.size(),
                             mixed_inv.data()));
  EXPECT_FALSE(std::isinf(mixed_inv[0].lp.lam));
  EXPECT_EQ(HUGE_VAL, mixed_inv[1].lp.lam);
  proj_errno_reset(pj_sph.get());

  // 3. proj_trans_generic strided forward/inverse round-trip.
  std::array<double, 2> xs = {DegToRad(29.020), DegToRad(-108.225)};
  std::array<double, 2> ys = {DegToRad(-13.233), DegToRad(24.487)};
  const std::array<double, 2> xs_orig = xs;
  const std::array<double, 2> ys_orig = ys;
  ASSERT_EQ(2u,
            proj_trans_generic(pj_sph.get(), PJ_FWD, xs.data(), sizeof(double),
                               xs.size(), ys.data(), sizeof(double), ys.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  ASSERT_EQ(2u,
            proj_trans_generic(pj_sph.get(), PJ_INV, xs.data(), sizeof(double),
                               xs.size(), ys.data(), sizeof(double), ys.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  for (size_t i = 0; i < xs.size(); ++i) {
    EXPECT_NEAR(xs_orig[i], xs[i], 1e-12);
    EXPECT_NEAR(ys_orig[i], ys[i], 1e-12);
  }

  // 4. proj_roundtrip and proj_factors inside face 0.
  PJ_COORD rt_pt = proj_coord(DegToRad(23.032), DegToRad(28.173), 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(pj_ell.get(), PJ_FWD, 3, &rt_pt), 1e-6);

  const PJ_FACTORS fac_sph = proj_factors(
      pj_sph.get(), proj_coord(DegToRad(23.032), DegToRad(28.173), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_sph.get()));
  EXPECT_GT(fac_sph.meridional_scale, 0.5);
  EXPECT_LT(fac_sph.meridional_scale, 1.5);
  EXPECT_GT(fac_sph.parallel_scale, 0.5);
  EXPECT_LT(fac_sph.parallel_scale, 1.5);
  EXPECT_GT(fac_sph.areal_scale, 0.4);
  EXPECT_GE(fac_sph.tissot_semimajor, fac_sph.tissot_semiminor);

  const PJ_FACTORS fac_ell = proj_factors(
      pj_ell.get(), proj_coord(DegToRad(23.032), DegToRad(28.173), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_ell.get()));
  EXPECT_GT(fac_ell.areal_scale, 0.4);

  // 5. Central meridian, false easting/northing, and linear unit options.
  const PJ_COORD off_in =
      proj_coord(DegToRad(53.032), DegToRad(28.173), 0.0, 0.0);
  const PJ_COORD off_fwd = proj_trans(pj_offset.get(), PJ_FWD, off_in);
  EXPECT_EQ(0, proj_errno(pj_offset.get()));
  const PJ_COORD off_inv = proj_trans(pj_offset.get(), PJ_INV, off_fwd);
  EXPECT_EQ(0, proj_errno(pj_offset.get()));
  EXPECT_NEAR(off_in.lp.lam, off_inv.lp.lam, 1e-12);
  EXPECT_NEAR(off_in.lp.phi, off_inv.lp.phi, 1e-12);
}

}  // namespace
}  // namespace proj
