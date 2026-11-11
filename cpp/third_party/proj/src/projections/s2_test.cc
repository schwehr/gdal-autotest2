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
// Unit tests for the S2 spherical/ellipsoidal cube-face projection
// (proj/src/projections/s2.cpp). Hermetic: uses inline PROJ strings only
// without external grids, database queries, or network access.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/s2.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(S2Test, InitializationDefaultsAndInvalidParameters) {
  // Force s2_test.o (the first object on the link line, compiled with
  // -fexceptions) to emit the weak COMDAT definition of
  // std::__throw_out_of_range(const char*) before -fno-exceptions dependencies
  // such as gunit_main.
  void (*volatile ensure_throw_out_of_range)(const char*) =
      &std::__throw_out_of_range;  // NOLINT(build/internal)
  ASSERT_NE(nullptr, ensure_throw_out_of_range);

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Default projection omits +UVtoST and selects quadratic UV<->ST mapping.
  ScopedPj default_s2(proj_create(ctx.get(), "+proj=s2 +R=6371000"));
  ASSERT_NE(nullptr, default_s2.get());
  EXPECT_EQ(1, pj_has_inverse(default_s2.get()));
  EXPECT_EQ(1, proj_angular_input(default_s2.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(default_s2.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(default_s2.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(default_s2.get(), PJ_INV));

  const PJ_PROJ_INFO info = proj_pj_info(default_s2.get());
  EXPECT_STREQ("s2", info.id);
  EXPECT_EQ(1, info.has_inverse);

  ScopedPj explicit_quad(
      proj_create(ctx.get(), "+proj=s2 +UVtoST=quadratic +R=6371000"));
  ASSERT_NE(nullptr, explicit_quad.get());
  const PJ_COORD sample = proj_coord(DegToRad(20.0), DegToRad(15.0), 0.0, 0.0);
  const PJ_COORD default_xy = proj_trans(default_s2.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(default_s2.get()));
  const PJ_COORD quad_xy = proj_trans(explicit_quad.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(explicit_quad.get()));
  EXPECT_NEAR(quad_xy.xy.x, default_xy.xy.x, 1e-15);
  EXPECT_NEAR(quad_xy.xy.y, default_xy.xy.y, 1e-15);

  // 2. Verify proj_create_argv with mutable char buffers.
  char arg_proj[] = "proj=s2";
  char arg_uv[] = "UVtoST=tangent";
  char arg_rad[] = "R=6371000";
  char* argv[] = {arg_proj, arg_uv, arg_rad};
  ScopedPj argv_s2(proj_create_argv(ctx.get(), 3, argv));
  ASSERT_NE(nullptr, argv_s2.get());
  EXPECT_EQ(1, pj_has_inverse(argv_s2.get()));

  // 3. Invalid +UVtoST values throw std::out_of_range internally and fail with
  // PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE plus an explicit error log.
  constexpr const char* kInvalidDefs[] = {
      "+proj=s2 +UVtoST=invalid",
      "+proj=s2 +UVtoST=cubic",
      "+proj=s2 +UVtoST=",
      "+proj=s2 +UVtoST",
  };
  for (const char* def : kInvalidDefs) {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    ScopedPj bad(proj_create(ctx.get(), def));
    EXPECT_EQ(nullptr, bad.get()) << def;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << def;
    EXPECT_TRUE(capture.ContainsMessage("Invalid value for s2 parameter"))
        << def;
  }

  // 4. Empty/null definitions and invalid ellipsoid fail cleanly.
  ScopedLogSuppressor suppress(ctx.get());
  EXPECT_EQ(nullptr, proj_create(ctx.get(), nullptr));
  EXPECT_EQ(nullptr, proj_create(ctx.get(), ""));
  EXPECT_EQ(nullptr, proj_create(ctx.get(), "+proj=s2 +ellps=nonexistent"));
}

TEST(S2Test, ForwardAndInverseAllUVtoSTModesSphereAndEllipsoid) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ModeCase {
    const char* def;
    double lon_deg;
    double lat_deg;
    double expected_s;
    double expected_t;
  };
  constexpr ModeCase kCases[] = {
      // Sphere (+R=6371000): S2 uses PJ_IO_UNITS_PROJECTED, so outputs remain
      // unitless [0, 1] cube-face ST coordinates ([-1, 1] for UVtoST=none).
      {
          .def = "+proj=s2 +UVtoST=linear +R=6371000",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_s = 0.5,
          .expected_t = 0.5,
      },
      {
          .def = "+proj=s2 +UVtoST=linear +R=6371000",
          .lon_deg = 45.0,
          .lat_deg = 0.0,
          .expected_s = 1.0,
          .expected_t = 0.5,
      },
      {
          .def = "+proj=s2 +UVtoST=linear +R=6371000",
          .lon_deg = -20.0,
          .lat_deg = -15.0,
          .expected_s = 0.31801488286689883,
          .expected_t = 0.35742721263096416,
      },
      // Quadratic covers both u >= 0 / s >= 0.5 and u < 0 / s < 0.5 branches.
      {
          .def = "+proj=s2 +UVtoST=quadratic +R=6371000",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_s = 0.5,
          .expected_t = 0.5,
      },
      {
          .def = "+proj=s2 +UVtoST=quadratic +R=6371000",
          .lon_deg = 45.0,
          .lat_deg = 0.0,
          .expected_s = 1.0,
          .expected_t = 0.5,
      },
      {
          .def = "+proj=s2 +UVtoST=quadratic +R=6371000",
          .lon_deg = -20.0,
          .lat_deg = -15.0,
          .expected_s = 0.27682804555233764,
          .expected_t = 0.3189279178724518,
      },
      {
          .def = "+proj=s2 +UVtoST=tangent +R=6371000",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_s = 0.5,
          .expected_t = 0.5,
      },
      {
          .def = "+proj=s2 +UVtoST=tangent +R=6371000",
          .lon_deg = -20.0,
          .lat_deg = -15.0,
          .expected_s = 0.2777777777777778,
          .expected_t = 0.3231637126193204,
      },
      {
          .def = "+proj=s2 +UVtoST=none +R=6371000",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_s = 0.0,
          .expected_t = 0.0,
      },
      {
          .def = "+proj=s2 +UVtoST=none +R=6371000",
          .lon_deg = -20.0,
          .lat_deg = -15.0,
          .expected_s = -0.36397023426620234,
          .expected_t = -0.28514557473807167,
      },
      // Ellipsoid (+ellps=WGS84): exercises [LK12] geodetic <-> geocentric
      // latitude conversion, including negative latitude (invert_sign == 1) and
      // positive latitude (invert_sign == 0).
      {
          .def = "+proj=s2 +UVtoST=linear +ellps=WGS84",
          .lon_deg = 20.0,
          .lat_deg = 15.0,
          .expected_s = 0.6819851171331012,
          .expected_t = 0.6416183509541339,
      },
      {
          .def = "+proj=s2 +UVtoST=quadratic +ellps=WGS84",
          .lon_deg = 20.0,
          .lat_deg = 15.0,
          .expected_s = 0.7231719544476622,
          .expected_t = 0.6800202397217312,
      },
      {
          .def = "+proj=s2 +UVtoST=quadratic +ellps=WGS84",
          .lon_deg = -20.0,
          .lat_deg = -15.0,
          .expected_s = 0.2768280455523378,
          .expected_t = 0.3199797602782688,
      },
      {
          .def = "+proj=s2 +UVtoST=tangent +ellps=WGS84",
          .lon_deg = 20.0,
          .lat_deg = 15.0,
          .expected_s = 0.7222222222222223,
          .expected_t = 0.675711874216581,
      },
      {
          .def = "+proj=s2 +UVtoST=none +ellps=WGS84",
          .lon_deg = 20.0,
          .lat_deg = 15.0,
          .expected_s = 0.3639702342662023,
          .expected_t = 0.2832367019082678,
      },
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_s, fwd.xy.x, 1e-12) << tc.def;
    EXPECT_NEAR(tc.expected_t, fwd.xy.y, 1e-12) << tc.def;

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-12) << tc.def;
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-12) << tc.def;
  }

  // False easting/northing shifts the projected [0, 1] ST plane directly.
  ScopedPj shifted(
      proj_create(ctx.get(), "+proj=s2 +R=6371000 +x_0=10.0 +y_0=20.0"));
  ASSERT_NE(nullptr, shifted.get());
  const PJ_COORD center_fwd =
      proj_trans(shifted.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(shifted.get()));
  EXPECT_NEAR(10.5, center_fwd.xy.x, 1e-12);
  EXPECT_NEAR(20.5, center_fwd.xy.y, 1e-12);
  const PJ_COORD center_inv = proj_trans(shifted.get(), PJ_INV, center_fwd);
  EXPECT_EQ(0, proj_errno(shifted.get()));
  EXPECT_NEAR(0.0, center_inv.lp.lam, 1e-12);
  EXPECT_NEAR(0.0, center_inv.lp.phi, 1e-12);
}

TEST(S2Test, AllSixCubeFacesAndCenterThresholdsRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct FaceCase {
    const char* center_args;
    double lon_deg;
    double lat_deg;
    bool is_pole;
    bool check_center_st;
  };
  constexpr FaceCase kCases[] = {
      // FACE_FRONT (|lam0| <= 45 deg, |phi0| < 67.5 deg)
      {
          .center_args = "+lat_0=0 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .is_pole = false,
          .check_center_st = true,
      },
      {
          .center_args = "+lat_0=67 +lon_0=44",
          .lon_deg = -15.0,
          .lat_deg = -12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      // FACE_RIGHT (45 < lam0 <= 135 deg)
      {
          .center_args = "+lat_0=0 +lon_0=90",
          .lon_deg = 90.0,
          .lat_deg = 0.0,
          .is_pole = false,
          .check_center_st = true,
      },
      {
          .center_args = "+lat_0=0 +lon_0=46",
          .lon_deg = 105.0,
          .lat_deg = 12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      {
          .center_args = "+lat_0=0 +lon_0=134",
          .lon_deg = 75.0,
          .lat_deg = -12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      // FACE_TOP (phi0 >= 67.5 deg)
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = 90.0,
          .is_pole = true,
          .check_center_st = true,
      },
      {
          .center_args = "+lat_0=68 +lon_0=0",
          .lon_deg = 45.0,
          .lat_deg = 55.0,
          .is_pole = false,
          .check_center_st = false,
      },
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = -135.0,
          .lat_deg = 55.0,
          .is_pole = false,
          .check_center_st = false,
      },
      // FACE_BACK (|lam0| > 135 deg)
      {
          .center_args = "+lat_0=0 +lon_0=180",
          .lon_deg = 180.0,
          .lat_deg = 0.0,
          .is_pole = false,
          .check_center_st = true,
      },
      {
          .center_args = "+lat_0=0 +lon_0=136",
          .lon_deg = 165.0,
          .lat_deg = 12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      {
          .center_args = "+lat_0=0 +lon_0=-136",
          .lon_deg = -165.0,
          .lat_deg = -12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      // FACE_LEFT (-135 <= lam0 < -45 deg)
      {
          .center_args = "+lat_0=0 +lon_0=-90",
          .lon_deg = -90.0,
          .lat_deg = 0.0,
          .is_pole = false,
          .check_center_st = true,
      },
      {
          .center_args = "+lat_0=0 +lon_0=-46",
          .lon_deg = -75.0,
          .lat_deg = 12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      {
          .center_args = "+lat_0=0 +lon_0=-134",
          .lon_deg = -105.0,
          .lat_deg = -12.0,
          .is_pole = false,
          .check_center_st = false,
      },
      // FACE_BOTTOM (phi0 <= -67.5 deg)
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = -90.0,
          .is_pole = true,
          .check_center_st = true,
      },
      {
          .center_args = "+lat_0=-68 +lon_0=0",
          .lon_deg = 45.0,
          .lat_deg = -55.0,
          .is_pole = false,
          .check_center_st = false,
      },
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = -135.0,
          .lat_deg = -55.0,
          .is_pole = false,
          .check_center_st = false,
      },
  };

  constexpr const char* kEllipsoids[] = {"+R=6371000", "+ellps=WGS84"};
  for (const char* ellps : kEllipsoids) {
    for (const auto& tc : kCases) {
      const std::string def =
          std::string("+proj=s2 ") + tc.center_args + " " + ellps;
      ScopedPj pj(proj_create(ctx.get(), def.c_str()));
      ASSERT_NE(nullptr, pj.get()) << def;

      const double lon_rad = DegToRad(tc.lon_deg);
      const double lat_rad = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_FALSE(std::isnan(fwd.xy.x) || std::isinf(fwd.xy.x)) << def;
      EXPECT_FALSE(std::isnan(fwd.xy.y) || std::isinf(fwd.xy.y)) << def;

      if (tc.check_center_st) {
        EXPECT_NEAR(0.5, fwd.xy.x, 1e-12) << def;
        EXPECT_NEAR(0.5, fwd.xy.y, 1e-12) << def;
      }

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11) << def;
      if (!tc.is_pole) {
        // Wrap difference across the +-180 degree antimeridian on FACE_BACK.
        EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon_rad), 1e-11) << def;
      }
    }
  }
}

TEST(S2Test, DomainErrorsAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=s2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Latitude beyond +-pi/2 fails in fwd_prepare.
  const PJ_COORD bad_lat_pos =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(91.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat_pos.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD bad_lat_neg =
      proj_trans(pj.get(), PJ_FWD, proj_coord(0.0, DegToRad(-91.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat_neg.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. Longitude beyond 10 rad fails in fwd_prepare.
  const PJ_COORD bad_lon =
      proj_trans(pj.get(), PJ_FWD, proj_coord(11.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lon.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 3. HUGE_VAL inputs fail cleanly in forward and inverse.
  const PJ_COORD inf_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inf_fwd.xyzt.x);

  const PJ_COORD inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 0.5, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inf_inv.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 4. NaN propagates through all four lanes without setting errno.
  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_out =
      proj_trans(pj.get(), PJ_FWD, proj_coord(qnan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.x));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.y));
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 5. PJ_IDENT and nullptr PJ* edge cases.
  const PJ_COORD ident_in = proj_coord(0.25, -0.5, 10.0, 2026.0);
  const PJ_COORD ident_out = proj_trans(pj.get(), PJ_IDENT, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.v[0], ident_out.v[0]);
  EXPECT_DOUBLE_EQ(ident_in.v[1], ident_out.v[1]);
  EXPECT_DOUBLE_EQ(ident_in.v[2], ident_out.v[2]);
  EXPECT_DOUBLE_EQ(ident_in.v[3], ident_out.v[3]);

  const PJ_COORD null_pj_out = proj_trans(nullptr, PJ_FWD, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.v[0], null_pj_out.v[0]);
  const PJ_FACTORS null_fac = proj_factors(nullptr, ident_in);
  EXPECT_DOUBLE_EQ(0.0, null_fac.meridional_scale);
  PJ_COORD rt_copy = ident_in;
  EXPECT_EQ(HUGE_VAL, proj_roundtrip(nullptr, PJ_FWD, 1, &rt_copy));
}

TEST(S2Test, Transforms3D4DArrayGenericAndClassicEntryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=s2 +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t through forward and inverse.
  const PJ_COORD in4d =
      proj_coord(DegToRad(12.0), DegToRad(22.0), 345.67, 2026.5);
  const PJ_COORD fwd4d = proj_trans(pj.get(), PJ_FWD, in4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(345.67, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.5, fwd4d.xyzt.t);

  const PJ_COORD inv4d = proj_trans(pj.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in4d.lpzt.lam, inv4d.lpzt.lam, 1e-12);
  EXPECT_NEAR(in4d.lpzt.phi, inv4d.lpzt.phi, 1e-12);
  EXPECT_DOUBLE_EQ(345.67, inv4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2026.5, inv4d.lpzt.t);

  // 2. Classic 2D/3D internal entry points (pj_fwd/pj_inv, pj_fwd3d/pj_inv3d).
  const PJ_LP lp_in = {.lam = DegToRad(-14.0), .phi = DegToRad(-18.0)};
  const PJ_XY xy_2d = pj_fwd(lp_in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP lp_2d = pj_inv(xy_2d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp_in.lam, lp_2d.lam, 1e-12);
  EXPECT_NEAR(lp_in.phi, lp_2d.phi, 1e-12);

  const PJ_LPZ lpz_in = {
      .lam = DegToRad(18.0), .phi = DegToRad(-10.0), .z = 88.5};
  const PJ_XYZ xyz_3d = pj_fwd3d(lpz_in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(88.5, xyz_3d.z);
  const PJ_LPZ lpz_3d = pj_inv3d(xyz_3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lpz_in.lam, lpz_3d.lam, 1e-12);
  EXPECT_NEAR(lpz_in.phi, lpz_3d.phi, 1e-12);
  EXPECT_DOUBLE_EQ(88.5, lpz_3d.z);

  // 3. Batch transformation via proj_trans_array.
  std::array<PJ_COORD, 3> coords = {
      proj_coord(DegToRad(-25.0), DegToRad(-15.0), 10.0, 0.0),
      proj_coord(0.0, 0.0, 20.0, 0.0),
      proj_coord(DegToRad(25.0), DegToRad(15.0), 30.0, 0.0),
  };
  const auto orig = coords;
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (std::size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(orig[i].lpz.lam, coords[i].lpz.lam, 1e-12);
    EXPECT_NEAR(orig[i].lpz.phi, coords[i].lpz.phi, 1e-12);
    EXPECT_DOUBLE_EQ(orig[i].lpz.z, coords[i].lpz.z);
  }

  ScopedLogSuppressor suppress(ctx.get());
  std::array<PJ_COORD, 2> mixed = {
      proj_coord(DegToRad(10.0), DegToRad(10.0), 0.0, 0.0),
      proj_coord(0.0, DegToRad(95.0), 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
            proj_trans_array(pj.get(), PJ_FWD, mixed.size(), mixed.data()));
  EXPECT_FALSE(std::isinf(mixed[0].xy.x));
  EXPECT_EQ(HUGE_VAL, mixed[1].xy.x);
  proj_errno_reset(pj.get());

  // 4. Strided transformation via proj_trans_generic.
  std::array<double, 2> lons = {DegToRad(-10.0), DegToRad(10.0)};
  std::array<double, 2> lats = {DegToRad(5.0), DegToRad(-5.0)};
  double z_const = 50.0;
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_FWD, lons.data(),
                                   sizeof(double), lons.size(), lats.data(),
                                   sizeof(double), lats.size(), &z_const,
                                   sizeof(double), 1, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(2u, proj_trans_generic(pj.get(), PJ_INV, lons.data(),
                                   sizeof(double), lons.size(), lats.data(),
                                   sizeof(double), lats.size(), &z_const,
                                   sizeof(double), 1, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(DegToRad(-10.0), lons[0], 1e-12);
  EXPECT_NEAR(DegToRad(5.0), lats[0], 1e-12);
  EXPECT_NEAR(DegToRad(10.0), lons[1], 1e-12);
  EXPECT_NEAR(DegToRad(-5.0), lats[1], 1e-12);

  // 5. Multi-iteration proj_roundtrip in both directions.
  PJ_COORD rt_fwd = proj_coord(DegToRad(15.0), DegToRad(-20.0), 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(pj.get(), PJ_FWD, 5, &rt_fwd), 1e-6);
  PJ_COORD rt_inv = proj_coord(0.3, 0.7, 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(pj.get(), PJ_INV, 5, &rt_inv), 1e-12);
}

TEST(S2Test, ProjFactorsAcrossUVtoSTModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct FactorCase {
    const char* def;
    double lon_deg;
    double lat_deg;
    double expected_h;
    double expected_k;
    double expected_s;
  };
  constexpr FactorCase kCases[] = {
      // At (0, 0) on FACE_FRONT: conformal and isotropic (omega = 0).
      // Quadratic has a second-derivative jump across u=0 so central difference
      // with h=1e-5 yields ~0.7499944 instead of exact 0.75.
      {
          .def = "+proj=s2 +UVtoST=linear +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_h = 0.5,
          .expected_k = 0.5,
          .expected_s = 0.25,
      },
      {
          .def = "+proj=s2 +UVtoST=quadratic +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_h = 0.7499943751487769,
          .expected_k = 0.7499943751154702,
          .expected_s = 0.5624915627298245,
      },
      {
          .def = "+proj=s2 +UVtoST=tangent +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_h = 2.0 / kPi,
          .expected_k = 2.0 / kPi,
          .expected_s = 4.0 / (kPi * kPi),
      },
      {
          .def = "+proj=s2 +UVtoST=none +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_h = 1.0,
          .expected_k = 1.0,
          .expected_s = 1.0,
      },
      // Off-center on sphere and WGS84 ellipsoid.
      {
          .def = "+proj=s2 +UVtoST=quadratic +R=1",
          .lon_deg = 15.0,
          .lat_deg = 20.0,
          .expected_h = 0.6024379269359281,
          .expected_k = 0.6393128941729854,
          .expected_s = 0.38370749060857867,
      },
      {
          .def = "+proj=s2 +UVtoST=quadratic +ellps=WGS84",
          .lon_deg = 15.0,
          .lat_deg = 20.0,
          .expected_h = 0.6028019798732343,
          .expected_k = 0.6390391069101357,
          .expected_s = 0.3837890051654409,
      },
  };

  for (const auto& tc : kCases) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;

    const PJ_FACTORS fac = proj_factors(
        pj.get(),
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_h, fac.meridional_scale, 1e-6) << tc.def;
    EXPECT_NEAR(tc.expected_k, fac.parallel_scale, 1e-6) << tc.def;
    EXPECT_NEAR(tc.expected_s, fac.areal_scale, 1e-6) << tc.def;
  }
}

// TODO(schwehr): In projections/s2.cpp, the OOM guard in PJ_PROJECTION(s2) is
// unreachable without allocation fault injection, and unused internal helpers
// (Abs, LargestAbsComponent, FaceUVtoXYZ, GetFace, XYZtoFaceUV, FaceXYZtoUV)
// are not called by the forward/inverse projection entry points.

}  // namespace
}  // namespace proj
