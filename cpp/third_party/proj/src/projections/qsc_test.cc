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
// Unit tests for the Quadrilateralized Spherical Cube (QSC) projection
// (proj/src/projections/qsc.cpp). Hermetic: uses inline PROJ strings only
// without external grids, database queries, or network access.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/qsc.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kSphereRadius = 6371000.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(QscTest, InitializationMetadataAndInvalidDefinitions) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Verify default and ellipsoidal initialization metadata.
  ScopedPj sphere_qsc(proj_create(ctx.get(), "+proj=qsc +R=6371000"));
  ASSERT_NE(nullptr, sphere_qsc.get());
  EXPECT_EQ(1, pj_has_inverse(sphere_qsc.get()));
  EXPECT_EQ(1, proj_angular_input(sphere_qsc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(sphere_qsc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(sphere_qsc.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(sphere_qsc.get(), PJ_INV));

  const PJ_PROJ_INFO info = proj_pj_info(sphere_qsc.get());
  EXPECT_STREQ("qsc", info.id);
  EXPECT_EQ(1, info.has_inverse);

  ScopedPj wgs84_qsc(proj_create(ctx.get(), "+proj=qsc +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84_qsc.get());
  EXPECT_EQ(1, pj_has_inverse(wgs84_qsc.get()));

  // 2. Verify proj_create_argv with mutable char buffers.
  char arg_proj[] = "proj=qsc";
  char arg_lon0[] = "lon_0=90";
  char arg_rad[] = "R=6371000";
  char* argv[] = {arg_proj, arg_lon0, arg_rad};
  ScopedPj argv_qsc(proj_create_argv(ctx.get(), 3, argv));
  ASSERT_NE(nullptr, argv_qsc.get());
  EXPECT_EQ(1, pj_has_inverse(argv_qsc.get()));

  // 3. Invalid/empty/null definitions fail cleanly.
  ScopedLogSuppressor suppress(ctx.get());
  EXPECT_EQ(nullptr, proj_create(ctx.get(), nullptr));
  EXPECT_EQ(nullptr, proj_create(ctx.get(), ""));
  EXPECT_EQ(nullptr, proj_create(ctx.get(), "+proj=qsc +ellps=nonexistent"));
  EXPECT_EQ(nullptr, proj_create(ctx.get(), "+proj=qsc +R=0"));
}

TEST(QscTest, EquatorialFrontFaceAllAreasCenterAndExactEdgeCoordinates) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=qsc +lat_0=0 +lon_0=0 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  struct FrontCase {
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
    bool check_xy;
  };
  // QSC uses PJ_IO_UNITS_CLASSIC, so cube face half-width equals R (6371000 m).
  constexpr FrontCase kCases[] = {
      // Exact face center triggers phi < EPS10 in qsc_fwd_equat_face_theta.
      {
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
          .check_xy = true,
      },
      // Cube face edge midpoints across AREA_0..AREA_3.
      {
          .lon_deg = 45.0,
          .lat_deg = 0.0,
          .expected_x = kSphereRadius,
          .expected_y = 0.0,
          .check_xy = true,
      },
      {
          .lon_deg = 0.0,
          .lat_deg = 45.0,
          .expected_x = 0.0,
          .expected_y = kSphereRadius,
          .check_xy = true,
      },
      {
          .lon_deg = -45.0,
          .lat_deg = 0.0,
          .expected_x = -kSphereRadius,
          .expected_y = 0.0,
          .check_xy = true,
      },
      {
          .lon_deg = 0.0,
          .lat_deg = -45.0,
          .expected_x = 0.0,
          .expected_y = -kSphereRadius,
          .check_xy = true,
      },
      // Interior AREA_0 reference point.
      {
          .lon_deg = 20.0,
          .lat_deg = 15.0,
          .expected_x = 3134337.71710217,
          .expected_y = 2553909.042820623,
          .check_xy = true,
      },
      // AREA_1, AREA_2 (both theta > 0 and theta < 0), and AREA_3 interiors.
      {
          .lon_deg = 5.0,
          .lat_deg = 15.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
          .check_xy = false,
      },
      {
          .lon_deg = -15.0,
          .lat_deg = 5.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
          .check_xy = false,
      },
      {
          .lon_deg = -15.0,
          .lat_deg = -5.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
          .check_xy = false,
      },
      {
          .lon_deg = 5.0,
          .lat_deg = -15.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
          .check_xy = false,
      },
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);
    const PJ_COORD fwd =
        proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    if (tc.check_xy) {
      EXPECT_NEAR(tc.expected_x, fwd.xy.x, 1e-4);
      EXPECT_NEAR(tc.expected_y, fwd.xy.y, 1e-4);
    }

    const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11);
  }

  // Verify false easting/northing and linear unit scaling (CLASSIC units).
  ScopedPj km_pj(proj_create(
      ctx.get(), "+proj=qsc +R=6371000 +x_0=100000 +y_0=200000 +units=km"));
  ASSERT_NE(nullptr, km_pj.get());
  const PJ_COORD km_fwd = proj_trans(km_pj.get(), PJ_FWD,
                                     proj_coord(DegToRad(45.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(km_pj.get()));
  EXPECT_NEAR(6471.0, km_fwd.xy.x, 1e-7);
  EXPECT_NEAR(200.0, km_fwd.xy.y, 1e-7);
  const PJ_COORD km_inv = proj_trans(km_pj.get(), PJ_INV, km_fwd);
  EXPECT_EQ(0, proj_errno(km_pj.get()));
  EXPECT_NEAR(DegToRad(45.0), km_inv.lp.lam, 1e-11);
  EXPECT_NEAR(0.0, km_inv.lp.phi, 1e-11);
}

TEST(QscTest, InverseAllAreasAndCosphiClampingBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=qsc +lat_0=0 +lon_0=0 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Inverse followed by forward across AREA_0, AREA_1, AREA_2 (mu >= 0 and
  // mu < 0), and AREA_3.
  struct InvCase {
    double x;
    double y;
  };
  constexpr InvCase kCases[] = {
      {.x = 0.0, .y = 0.0},
      {.x = 2000000.0, .y = 500000.0},
      {.x = 500000.0, .y = 2000000.0},
      {.x = -2000000.0, .y = 500000.0},
      {.x = -2000000.0, .y = -500000.0},
      {.x = 500000.0, .y = -2000000.0},
  };
  for (const auto& tc : kCases) {
    const PJ_COORD inv =
        proj_trans(pj.get(), PJ_INV, proj_coord(tc.x, tc.y, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, inv);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(tc.x, fwd.xy.x, 1e-5);
    EXPECT_NEAR(tc.y, fwd.xy.y, 1e-5);
  }

  // 2. Far outside the cube face triggers cosphi < -1.0 clamping to -1.0 and
  // t >= 1.0 clamping for s and r, yielding the antipodal point (+-pi, 0).
  const PJ_COORD clamped = proj_trans(
      pj.get(), PJ_INV, proj_coord(10.0 * kSphereRadius, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(kPi, std::abs(clamped.lp.lam), 1e-12);
  EXPECT_NEAR(0.0, clamped.lp.phi, 1e-12);
}

TEST(QscTest, TopAndBottomFacesAllAreasRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct PolarFaceCase {
    const char* center_args;
    double lon_deg;
    double lat_deg;
    bool is_pole;
  };
  constexpr PolarFaceCase kCases[] = {
      // FACE_TOP (phi0 >= 67.5 deg): covers all 4 longitude sectors, including
      // AREA_1 with both lon > 0 (+150) and lon <= 0 (-150), plus North Pole.
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = 90.0,
          .lat_deg = 60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = 150.0,
          .lat_deg = 60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=68 +lon_0=0",
          .lon_deg = -150.0,
          .lat_deg = 60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = -90.0,
          .lat_deg = 60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = 60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=90 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = 90.0,
          .is_pole = true,
      },
      // FACE_BOTTOM (phi0 <= -67.5 deg): covers all 4 longitude sectors,
      // including AREA_3 with both lon > 0 (+150) and lon < 0 (-150), plus
      // South Pole.
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = 90.0,
          .lat_deg = -60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = -60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=-68 +lon_0=0",
          .lon_deg = -90.0,
          .lat_deg = -60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = 150.0,
          .lat_deg = -60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = -150.0,
          .lat_deg = -60.0,
          .is_pole = false,
      },
      {
          .center_args = "+lat_0=-90 +lon_0=0",
          .lon_deg = 0.0,
          .lat_deg = -90.0,
          .is_pole = true,
      },
  };

  constexpr const char* kEllipsoids[] = {"+R=6371000", "+ellps=WGS84"};
  for (const char* ellps : kEllipsoids) {
    for (const auto& tc : kCases) {
      const std::string def =
          std::string("+proj=qsc ") + tc.center_args + " " + ellps;
      ScopedPj pj(proj_create(ctx.get(), def.c_str()));
      ASSERT_NE(nullptr, pj.get()) << def;

      const double lon_rad = DegToRad(tc.lon_deg);
      const double lat_rad = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      if (tc.is_pole) {
        EXPECT_NEAR(0.0, fwd.xy.x, 1e-6) << def;
        EXPECT_NEAR(0.0, fwd.xy.y, 1e-6) << def;
      }

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11) << def;
      if (!tc.is_pole) {
        EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon_rad), 1e-11) << def;
      }
    }
  }
}

TEST(QscTest, AllEquatorialFacesAndLongitudeOriginWrappingRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct EquatFaceCase {
    const char* center_args;
    double lon_deg;
    double lat_deg;
  };
  constexpr EquatFaceCase kCases[] = {
      // FACE_FRONT (|lam0| <= 45 deg)
      {.center_args = "+lat_0=67 +lon_0=44", .lon_deg = 12.0, .lat_deg = 18.0},
      {.center_args = "+lat_0=0 +lon_0=0", .lon_deg = -12.0, .lat_deg = -18.0},
      // FACE_RIGHT (45 < lam0 <= 135 deg); (-150, 10) wraps via
      // qsc_shift_longitude_origin (slon > +M_PI in fwd, slon < -M_PI in inv).
      {.center_args = "+lat_0=0 +lon_0=90", .lon_deg = 90.0, .lat_deg = 0.0},
      {.center_args = "+lat_0=0 +lon_0=46", .lon_deg = 105.0, .lat_deg = -15.0},
      {.center_args = "+lat_0=0 +lon_0=134", .lon_deg = 75.0, .lat_deg = 15.0},
      {.center_args = "+lat_0=0 +lon_0=90", .lon_deg = -150.0, .lat_deg = 10.0},
      // FACE_BACK (|lam0| > 135 deg)
      {.center_args = "+lat_0=0 +lon_0=180", .lon_deg = 180.0, .lat_deg = 0.0},
      {.center_args = "+lat_0=0 +lon_0=136",
       .lon_deg = 165.0,
       .lat_deg = -10.0},
      {.center_args = "+lat_0=0 +lon_0=-136",
       .lon_deg = -165.0,
       .lat_deg = 10.0},
      // FACE_LEFT (-135 <= lam0 < -45 deg); (150, -10) wraps via
      // qsc_shift_longitude_origin (slon < -M_PI in fwd, slon > +M_PI in inv).
      {.center_args = "+lat_0=0 +lon_0=-90", .lon_deg = -90.0, .lat_deg = 0.0},
      {.center_args = "+lat_0=0 +lon_0=-46",
       .lon_deg = -105.0,
       .lat_deg = 15.0},
      {.center_args = "+lat_0=0 +lon_0=-134",
       .lon_deg = -75.0,
       .lat_deg = -15.0},
      {.center_args = "+lat_0=0 +lon_0=-90",
       .lon_deg = 150.0,
       .lat_deg = -10.0},
  };

  constexpr const char* kEllipsoids[] = {"+R=6371000", "+ellps=WGS84"};
  for (const char* ellps : kEllipsoids) {
    for (const auto& tc : kCases) {
      const std::string def =
          std::string("+proj=qsc ") + tc.center_args + " " + ellps;
      ScopedPj pj(proj_create(ctx.get(), def.c_str()));
      ASSERT_NE(nullptr, pj.get()) << def;

      const double lon_rad = DegToRad(tc.lon_deg);
      const double lat_rad = DegToRad(tc.lat_deg);
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_FALSE(std::isnan(fwd.xy.x) || std::isinf(fwd.xy.x)) << def;
      EXPECT_FALSE(std::isnan(fwd.xy.y) || std::isinf(fwd.xy.y)) << def;

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(0.0, adjlon(inv.lp.lam - lon_rad), 1e-11) << def;
      EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11) << def;
    }
  }
}

TEST(QscTest, DomainErrorsAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=qsc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Out-of-range latitude and longitude trigger fwd_prepare errors.
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

  const PJ_COORD bad_lon =
      proj_trans(pj.get(), PJ_FWD, proj_coord(11.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lon.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 2. HUGE_VAL inputs fail cleanly in forward and inverse.
  const PJ_COORD inf_fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inf_fwd.xyzt.x);

  const PJ_COORD inf_inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, inf_inv.xyzt.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 3. NaN propagates cleanly without setting errno.
  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const PJ_COORD nan_out =
      proj_trans(pj.get(), PJ_FWD, proj_coord(qnan, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.x));
  EXPECT_TRUE(std::isnan(nan_out.xyzt.y));
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 4. PJ_IDENT and nullptr PJ* edge cases.
  const PJ_COORD ident_in = proj_coord(0.2, -0.3, 42.0, 2026.0);
  const PJ_COORD ident_out = proj_trans(pj.get(), PJ_IDENT, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.v[0], ident_out.v[0]);
  EXPECT_DOUBLE_EQ(ident_in.v[1], ident_out.v[1]);
  EXPECT_DOUBLE_EQ(ident_in.v[2], ident_out.v[2]);
  EXPECT_DOUBLE_EQ(ident_in.v[3], ident_out.v[3]);

  const PJ_COORD null_out = proj_trans(nullptr, PJ_FWD, ident_in);
  EXPECT_DOUBLE_EQ(ident_in.v[0], null_out.v[0]);
  const PJ_FACTORS null_fac = proj_factors(nullptr, ident_in);
  EXPECT_DOUBLE_EQ(0.0, null_fac.areal_scale);
  PJ_COORD rt_copy = ident_in;
  EXPECT_EQ(HUGE_VAL, proj_roundtrip(nullptr, PJ_FWD, 1, &rt_copy));
}

TEST(QscTest, Transforms3D4DArrayGenericAndClassicEntryPoints) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(ctx.get(), "+proj=qsc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 1. 4D proj_trans preserves z and t through forward and inverse.
  const PJ_COORD in4d =
      proj_coord(DegToRad(15.0), DegToRad(20.0), 42.5, 2026.25);
  const PJ_COORD fwd4d = proj_trans(pj.get(), PJ_FWD, in4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(42.5, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.25, fwd4d.xyzt.t);

  const PJ_COORD inv4d = proj_trans(pj.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in4d.lpzt.lam, inv4d.lpzt.lam, 1e-12);
  EXPECT_NEAR(in4d.lpzt.phi, inv4d.lpzt.phi, 1e-12);
  EXPECT_DOUBLE_EQ(42.5, inv4d.lpzt.z);
  EXPECT_DOUBLE_EQ(2026.25, inv4d.lpzt.t);

  // 2. Classic 2D/3D internal entry points (pj_fwd/pj_inv, pj_fwd3d/pj_inv3d).
  const PJ_LP lp_in = {.lam = DegToRad(-18.0), .phi = DegToRad(-12.0)};
  const PJ_XY xy_2d = pj_fwd(lp_in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  const PJ_LP lp_2d = pj_inv(xy_2d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lp_in.lam, lp_2d.lam, 1e-12);
  EXPECT_NEAR(lp_in.phi, lp_2d.phi, 1e-12);

  const PJ_LPZ lpz_in = {
      .lam = DegToRad(14.0), .phi = DegToRad(-22.0), .z = 75.0};
  const PJ_XYZ xyz_3d = pj_fwd3d(lpz_in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(75.0, xyz_3d.z);
  const PJ_LPZ lpz_3d = pj_inv3d(xyz_3d, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lpz_in.lam, lpz_3d.lam, 1e-12);
  EXPECT_NEAR(lpz_in.phi, lpz_3d.phi, 1e-12);
  EXPECT_DOUBLE_EQ(75.0, lpz_3d.z);

  // 3. Batch transformation via proj_trans_array (success and partial failure).
  std::array<PJ_COORD, 3> coords = {
      proj_coord(DegToRad(-20.0), DegToRad(-10.0), 1.0, 0.0),
      proj_coord(0.0, 0.0, 2.0, 0.0),
      proj_coord(DegToRad(20.0), DegToRad(10.0), 3.0, 0.0),
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
  std::array<double, 2> lons = {DegToRad(-12.0), DegToRad(12.0)};
  std::array<double, 2> lats = {DegToRad(8.0), DegToRad(-8.0)};
  double z_const = 25.0;
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
  EXPECT_NEAR(DegToRad(-12.0), lons[0], 1e-12);
  EXPECT_NEAR(DegToRad(8.0), lats[0], 1e-12);
  EXPECT_NEAR(DegToRad(12.0), lons[1], 1e-12);
  EXPECT_NEAR(DegToRad(-8.0), lats[1], 1e-12);

  // 5. Multi-iteration proj_roundtrip in both directions.
  PJ_COORD rt_fwd = proj_coord(DegToRad(15.0), DegToRad(-20.0), 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(pj.get(), PJ_FWD, 5, &rt_fwd), 1e-6);
  PJ_COORD rt_inv = proj_coord(1500000.0, -1000000.0, 0.0, 0.0);
  EXPECT_LT(proj_roundtrip(pj.get(), PJ_INV, 5, &rt_inv), 1e-6);
}

TEST(QscTest, ProjFactorsVerifyEqualAreaOnSphereAndEllipsoidalDeviation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. On the sphere, each cube face has area (2*R)^2 and total cube area
  // 24*R^2 vs sphere area 4*pi*R^2, giving constant areal scale 6/pi away
  // from the area diagonal seams |theta| = pi/4.
  ScopedPj sphere_front(proj_create(ctx.get(), "+proj=qsc +R=6371000"));
  ASSERT_NE(nullptr, sphere_front.get());

  constexpr double kEqualAreaScale = 6.0 / kPi;
  struct Pt {
    double lon_deg;
    double lat_deg;
  };
  constexpr Pt kSpherePoints[] = {
      {.lon_deg = 15.0, .lat_deg = 5.0},  {.lon_deg = 5.0, .lat_deg = 15.0},
      {.lon_deg = -15.0, .lat_deg = 5.0}, {.lon_deg = -5.0, .lat_deg = -15.0},
      {.lon_deg = 20.0, .lat_deg = 0.0},  {.lon_deg = 0.0, .lat_deg = 20.0},
  };
  for (const auto& pt : kSpherePoints) {
    const PJ_FACTORS fac = proj_factors(
        sphere_front.get(),
        proj_coord(DegToRad(pt.lon_deg), DegToRad(pt.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(sphere_front.get()));
    EXPECT_GT(fac.meridional_scale, 0.0);
    EXPECT_GT(fac.parallel_scale, 0.0);
    EXPECT_NEAR(kEqualAreaScale, fac.areal_scale, 1e-6);
  }

  ScopedPj sphere_right(
      proj_create(ctx.get(), "+proj=qsc +lon_0=90 +R=6371000"));
  ASSERT_NE(nullptr, sphere_right.get());
  const PJ_FACTORS right_fac = proj_factors(
      sphere_right.get(), proj_coord(DegToRad(105.0), DegToRad(5.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sphere_right.get()));
  EXPECT_NEAR(kEqualAreaScale, right_fac.areal_scale, 1e-6);

  // 2. On WGS84, the [LK12] geocentric shift alters areal scale slightly away
  // from 6/pi.
  ScopedPj wgs84_front(proj_create(ctx.get(), "+proj=qsc +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84_front.get());
  const PJ_FACTORS wgs_fac = proj_factors(
      wgs84_front.get(), proj_coord(DegToRad(15.0), DegToRad(5.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(wgs84_front.get()));
  EXPECT_NEAR(1.5116500702642595, wgs_fac.meridional_scale, 1e-6);
  EXPECT_NEAR(1.2698025801883066, wgs_fac.parallel_scale, 1e-6);
  EXPECT_NEAR(1.9099554727926453, wgs_fac.areal_scale, 1e-6);
}

// TODO(schwehr): In projections/qsc.cpp, the OOM guard in PJ_PROJECTION(qsc),
// the unreachable fallback else branch in qsc_e_forward, and the defensive
// cosphi > +1.0 clamp in qsc_e_inverse cannot be triggered via normal inputs.

}  // namespace
}  // namespace proj
