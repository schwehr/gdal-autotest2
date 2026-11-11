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
// Unit tests for the Lambert Azimuthal Equal Area projection
// (proj/src/projections/laea.cpp), covering parameter validation, all four
// modes (N_POLE, S_POLE, EQUIT, OBLIQ) in both spherical and ellipsoidal
// formulations, antipodal and outer-disk domain errors, batch transforms, and
// equal-area distortion factors.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/laea.cpp

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
constexpr double kSqrt2 = std::numbers::sqrt2;

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

TEST(LaeaTest, MetadataAndInitializationModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj_ell(proj_create(ctx.get(), "+proj=laea +ellps=WGS84"));
  ScopedPj pj_sph(proj_create(ctx.get(), "+proj=laea +R=1"));
  ASSERT_NE(nullptr, pj_ell.get());
  ASSERT_NE(nullptr, pj_sph.get());

  // 1. Metadata and angular/linear unit predicates.
  const PJ_PROJ_INFO info = proj_pj_info(pj_ell.get());
  EXPECT_STREQ("laea", info.id);
  EXPECT_STREQ("PROJ-based coordinate operation", info.description);
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj_ell.get()));
  EXPECT_STREQ("laea", pj_ell->short_name);
  EXPECT_THAT(std::string(pj_ell->descr),
              HasSubstr("Lambert Azimuthal Equal Area"));

  const std::array<PJ*, 2> pjs = {pj_ell.get(), pj_sph.get()};
  for (PJ* pj : pjs) {
    EXPECT_EQ(1, proj_angular_input(pj, PJ_FWD));
    EXPECT_EQ(0, proj_angular_output(pj, PJ_FWD));
    EXPECT_EQ(0, proj_angular_input(pj, PJ_INV));
    EXPECT_EQ(1, proj_angular_output(pj, PJ_INV));
  }

  const char* proj_str =
      proj_as_proj_string(ctx.get(), pj_ell.get(), PJ_PROJ_5, nullptr);
  ASSERT_NE(nullptr, proj_str);
  EXPECT_THAT(std::string(proj_str), HasSubstr("+proj=laea"));

  // 2. Boundary lat_0 values within EPS10 of +/-pi/2 and 0 succeed.
  constexpr const char* kValidBoundaryDefs[] = {
      "+proj=laea +lat_0=89.999999999 +ellps=WGS84",
      "+proj=laea +lat_0=-89.999999999 +ellps=WGS84",
      "+proj=laea +lat_0=0.000000001 +R=6371000",
      "+proj=laea +lat_0=89.999999999 +R=6371000",
      "+proj=laea +lat_0=-89.999999999 +R=6371000",
  };
  for (const char* def : kValidBoundaryDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    EXPECT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << def;
  }

  // 3. Mutable char[] buffers required by proj_create_argv.
  char arg0[] = "proj=laea";
  char arg1[] = "lat_0=52";
  char arg2[] = "lon_0=10";
  char arg3[] = "ellps=WGS84";
  char* argv[] = {arg0, arg1, arg2, arg3};
  ScopedPj pj_argv(proj_create_argv(ctx.get(), 4, argv));
  ASSERT_NE(nullptr, pj_argv.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

TEST(LaeaTest, InvalidLat0ParameterFailsWithErrorLog) {
  constexpr const char* kInvalidDefs[] = {
      "+proj=laea +lat_0=91 +ellps=WGS84",
      "+proj=laea +lat_0=-91 +ellps=WGS84",
      "+proj=laea +lat_0=90.001 +R=6371000",
      "+proj=laea +lat_0=-90.001 +R=6371000",
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
    EXPECT_TRUE(capture.ContainsMessage("Invalid value for lat_0")) << def;
  }
}

TEST(LaeaTest, SphericalClosedFormAndAllFourModesRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Verify exact spherical closed-form values on unit sphere (+R=1).
  struct ClosedFormCase {
    const char* def;
    double lon_deg;
    double lat_deg;
    double expected_x;
    double expected_y;
  };
  const ClosedFormCase kClosedForm[] = {
      // N_POLE: rho = 2*sin(pi/4 - phi/2), x = rho*sin(lam), y = -rho*cos(lam).
      {
          .def = "+proj=laea +lat_0=90 +R=1",
          .lon_deg = 0.0,
          .lat_deg = 90.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=90 +R=1",
          .lon_deg = 0.0,
          .lat_deg = 30.0,
          .expected_x = 0.0,
          .expected_y = -1.0,
      },
      {
          .def = "+proj=laea +lat_0=90 +R=1",
          .lon_deg = 90.0,
          .lat_deg = 30.0,
          .expected_x = 1.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=90 +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_x = 0.0,
          .expected_y = -kSqrt2,
      },
      // S_POLE: rho = 2*cos(pi/4 - phi/2), x = rho*sin(lam), y = rho*cos(lam).
      {
          .def = "+proj=laea +lat_0=-90 +R=1",
          .lon_deg = 0.0,
          .lat_deg = -90.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=-90 +R=1",
          .lon_deg = 0.0,
          .lat_deg = -30.0,
          .expected_x = 0.0,
          .expected_y = 1.0,
      },
      {
          .def = "+proj=laea +lat_0=-90 +R=1",
          .lon_deg = -90.0,
          .lat_deg = -30.0,
          .expected_x = -1.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=-90 +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_x = 0.0,
          .expected_y = kSqrt2,
      },
      // EQUIT: at (60, 0), k'=sqrt(2/1.5), x = k'*sin(60) = 1.0.
      {
          .def = "+proj=laea +lat_0=0 +R=1",
          .lon_deg = 0.0,
          .lat_deg = 0.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=0 +R=1",
          .lon_deg = 60.0,
          .lat_deg = 0.0,
          .expected_x = 1.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=0 +R=1",
          .lon_deg = 90.0,
          .lat_deg = 0.0,
          .expected_x = kSqrt2,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=0 +R=1",
          .lon_deg = 0.0,
          .lat_deg = 90.0,
          .expected_x = 0.0,
          .expected_y = kSqrt2,
      },
      // OBLIQ (+lat_0=45, +lon_0=10): north pole at distance 45 deg gives
      // y = 2 * sin(22.5 deg) = sqrt(2 - sqrt(2)).
      {
          .def = "+proj=laea +lat_0=45 +lon_0=10 +R=1",
          .lon_deg = 10.0,
          .lat_deg = 45.0,
          .expected_x = 0.0,
          .expected_y = 0.0,
      },
      {
          .def = "+proj=laea +lat_0=45 +lon_0=10 +R=1",
          .lon_deg = 10.0,
          .lat_deg = 90.0,
          .expected_x = 0.0,
          .expected_y = std::sqrt(2.0 - kSqrt2),
      },
  };

  for (const auto& tc : kClosedForm) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;
    const PJ_COORD out = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << tc.def;
    EXPECT_NEAR(tc.expected_x, out.xy.x, 1e-12) << tc.def;
    EXPECT_NEAR(tc.expected_y, out.xy.y, 1e-12) << tc.def;
  }

  // 2. Round-trip all 4 spherical modes with R=6371000, non-zero lon_0, and
  // false easting/northing, including exact center (0, 0) inverse.
  constexpr LonLatCase kNorthPoleCases[] = {
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 90.0, .lat_deg = 30.0},
      {.lon_deg = -90.0, .lat_deg = 30.0},
      {.lon_deg = 180.0, .lat_deg = 0.0},
      {.lon_deg = -45.0, .lat_deg = -45.0},
      {.lon_deg = 135.0, .lat_deg = -85.0},
  };
  constexpr LonLatCase kSouthPoleCases[] = {
      {.lon_deg = 0.0, .lat_deg = -90.0},  {.lon_deg = -90.0, .lat_deg = -30.0},
      {.lon_deg = 90.0, .lat_deg = -30.0}, {.lon_deg = 180.0, .lat_deg = 0.0},
      {.lon_deg = 45.0, .lat_deg = 45.0},  {.lon_deg = -135.0, .lat_deg = 85.0},
  };
  constexpr LonLatCase kEquitCases[] = {
      {.lon_deg = 15.0, .lat_deg = 0.0},
      {.lon_deg = 75.0, .lat_deg = 0.0},
      {.lon_deg = -45.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 90.0},
      {.lon_deg = 15.0, .lat_deg = -90.0},
      {.lon_deg = 110.0, .lat_deg = 65.0},
      {.lon_deg = -110.0, .lat_deg = -65.0},
  };
  constexpr LonLatCase kObliqCases[] = {
      {.lon_deg = 10.0, .lat_deg = 45.0},  {.lon_deg = 10.0, .lat_deg = 89.9},
      {.lon_deg = 10.0, .lat_deg = -89.9}, {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = -80.0, .lat_deg = 30.0}, {.lon_deg = 120.0, .lat_deg = -20.0},
      {.lon_deg = 180.0, .lat_deg = 15.0},
  };

  ScopedPj sph_np(proj_create(ctx.get(), "+proj=laea +lat_0=90 +R=6371000"));
  ScopedPj sph_sp(proj_create(ctx.get(), "+proj=laea +lat_0=-90 +R=6371000"));
  ScopedPj sph_eq(proj_create(
      ctx.get(),
      "+proj=laea +lat_0=0 +lon_0=15 +x_0=100000 +y_0=-50000 +R=6371000"));
  ScopedPj sph_ob(proj_create(
      ctx.get(),
      "+proj=laea +lat_0=45 +lon_0=10 +x_0=500000 +y_0=250000 +R=6371000"));
  ASSERT_NE(nullptr, sph_np.get());
  ASSERT_NE(nullptr, sph_sp.get());
  ASSERT_NE(nullptr, sph_eq.get());
  ASSERT_NE(nullptr, sph_ob.get());

  AssertRoundTripCases(sph_np.get(), kNorthPoleCases,
                       std::size(kNorthPoleCases), 1e-11);
  AssertRoundTripCases(sph_sp.get(), kSouthPoleCases,
                       std::size(kSouthPoleCases), 1e-11);
  AssertRoundTripCases(sph_eq.get(), kEquitCases, std::size(kEquitCases),
                       1e-11);
  AssertRoundTripCases(sph_ob.get(), kObliqCases, std::size(kObliqCases),
                       1e-11);

  // 3. Explicit xy.y == 0.0 inverse paths in spherical N_POLE/S_POLE (taking
  // atan2 rather than returning 0) and EQUIT/OBLIQ center (rh <= EPS10).
  const PJ_COORD np_east =
      proj_trans(sph_np.get(), PJ_INV, proj_coord(6371000.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph_np.get()));
  EXPECT_NEAR(kHalfPi, np_east.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(30.0), np_east.lp.phi, 1e-12);

  const PJ_COORD sp_west =
      proj_trans(sph_sp.get(), PJ_INV, proj_coord(-6371000.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(sph_sp.get()));
  EXPECT_NEAR(-kHalfPi, sp_west.lp.lam, 1e-12);
  EXPECT_NEAR(DegToRad(-30.0), sp_west.lp.phi, 1e-12);
}

TEST(LaeaTest, EllipsoidalAllFourModesAndKnownValuesRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj ell_np(proj_create(ctx.get(), "+proj=laea +lat_0=90 +ellps=WGS84"));
  ScopedPj ell_sp(proj_create(ctx.get(), "+proj=laea +lat_0=-90 +ellps=WGS84"));
  ScopedPj ell_eq(proj_create(ctx.get(), "+proj=laea +lat_0=0 +ellps=WGS84"));
  ScopedPj ell_etrs(
      proj_create(ctx.get(),
                  "+proj=laea +lat_0=52 +lon_0=10 +x_0=4321000 +y_0=3210000 "
                  "+ellps=GRS80"));
  ScopedPj ell_south_ob(
      proj_create(ctx.get(), "+proj=laea +lat_0=-35 +lon_0=-60 +ellps=WGS84"));
  ScopedPj ell_high_flat(proj_create(
      ctx.get(), "+proj=laea +lat_0=52 +lon_0=10 +a=6378137 +rf=30"));
  ASSERT_NE(nullptr, ell_np.get());
  ASSERT_NE(nullptr, ell_sp.get());
  ASSERT_NE(nullptr, ell_eq.get());
  ASSERT_NE(nullptr, ell_etrs.get());
  ASSERT_NE(nullptr, ell_south_ob.get());
  ASSERT_NE(nullptr, ell_high_flat.get());

  // 1. Exact WGS84 authalic polar and equatorial values:
  // qp = 1 + ((1 - e^2) / e) * atanh(e).
  constexpr double kWgs84A = 6378137.0;
  constexpr double kWgs84F = 1.0 / 298.257223563;
  constexpr double kWgs84Es = 2.0 * kWgs84F - kWgs84F * kWgs84F;
  const double e = std::sqrt(kWgs84Es);
  const double qp = 1.0 + ((1.0 - kWgs84Es) / e) * std::atanh(e);

  // N_POLE at exact pole (q < 1e-15 -> (0, 0)) and equator (y = -a*sqrt(qp)).
  const PJ_COORD np_pole =
      proj_trans(ell_np.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_np.get()));
  EXPECT_NEAR(0.0, np_pole.xy.x, 1e-9);
  EXPECT_NEAR(0.0, np_pole.xy.y, 1e-9);

  const PJ_COORD np_eq =
      proj_trans(ell_np.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_np.get()));
  EXPECT_NEAR(0.0, np_eq.xy.x, 1e-6);
  EXPECT_NEAR(-kWgs84A * std::sqrt(qp), np_eq.xy.y, 1e-6);

  // S_POLE at exact pole (q < 1e-15 -> (0, 0)) and equator (y = +a*sqrt(qp)).
  const PJ_COORD sp_pole =
      proj_trans(ell_sp.get(), PJ_FWD, proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_sp.get()));
  EXPECT_NEAR(0.0, sp_pole.xy.x, 1e-9);
  EXPECT_NEAR(0.0, sp_pole.xy.y, 1e-9);

  const PJ_COORD sp_eq =
      proj_trans(ell_sp.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_sp.get()));
  EXPECT_NEAR(0.0, sp_eq.xy.x, 1e-6);
  EXPECT_NEAR(kWgs84A * std::sqrt(qp), sp_eq.xy.y, 1e-6);

  // EQUIT at poles: y = +/- a * qp / sqrt(2).
  const PJ_COORD eq_np =
      proj_trans(ell_eq.get(), PJ_FWD, proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_eq.get()));
  EXPECT_NEAR(0.0, eq_np.xy.x, 1e-6);
  EXPECT_NEAR(kWgs84A * qp / kSqrt2, eq_np.xy.y, 1e-6);

  // ETRS89-LAEA center (10 E, 52 N) maps exactly to false origin.
  const PJ_COORD etrs_center =
      proj_trans(ell_etrs.get(), PJ_FWD,
                 proj_coord(DegToRad(10.0), DegToRad(52.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_etrs.get()));
  EXPECT_NEAR(4321000.0, etrs_center.xy.x, 1e-6);
  EXPECT_NEAR(3210000.0, etrs_center.xy.y, 1e-6);

  // 2. Round-trip all 4 ellipsoidal modes including exact center inverse
  // (rho < EPS10 / q == 0.0) and high-latitude / multi-quadrant points.
  constexpr LonLatCase kNorthCases[] = {
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 20.0, .lat_deg = 89.9},
      {.lon_deg = -45.0, .lat_deg = 60.0},
      {.lon_deg = 120.0, .lat_deg = 0.0},
      {.lon_deg = -150.0, .lat_deg = -35.0},
      {.lon_deg = 40.0, .lat_deg = -85.0},
  };
  constexpr LonLatCase kSouthCases[] = {
      {.lon_deg = 0.0, .lat_deg = -90.0},  {.lon_deg = -20.0, .lat_deg = -89.9},
      {.lon_deg = 45.0, .lat_deg = -60.0}, {.lon_deg = -120.0, .lat_deg = 0.0},
      {.lon_deg = 150.0, .lat_deg = 35.0}, {.lon_deg = -40.0, .lat_deg = 85.0},
  };
  constexpr LonLatCase kEquitCases[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 55.0, .lat_deg = 0.0},
      {.lon_deg = -75.0, .lat_deg = 0.0},
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
      {.lon_deg = 115.0, .lat_deg = 52.5},
      {.lon_deg = -115.0, .lat_deg = -52.5},
  };
  constexpr LonLatCase kEtrsCases[] = {
      {.lon_deg = 10.0, .lat_deg = 52.0},  {.lon_deg = -10.0, .lat_deg = 35.0},
      {.lon_deg = 30.0, .lat_deg = 70.0},  {.lon_deg = -80.0, .lat_deg = -20.0},
      {.lon_deg = 10.0, .lat_deg = -89.9}, {.lon_deg = 160.0, .lat_deg = 25.0},
  };
  constexpr LonLatCase kSouthObliqCases[] = {
      {.lon_deg = -60.0, .lat_deg = -35.0},
      {.lon_deg = -120.0, .lat_deg = -60.0},
      {.lon_deg = 15.0, .lat_deg = 10.0},
      {.lon_deg = -60.0, .lat_deg = 89.9},
  };

  AssertRoundTripCases(ell_np.get(), kNorthCases, std::size(kNorthCases),
                       1e-11);
  AssertRoundTripCases(ell_sp.get(), kSouthCases, std::size(kSouthCases),
                       1e-11);
  AssertRoundTripCases(ell_eq.get(), kEquitCases, std::size(kEquitCases),
                       1e-11);
  AssertRoundTripCases(ell_etrs.get(), kEtrsCases, std::size(kEtrsCases),
                       1e-11);
  AssertRoundTripCases(ell_south_ob.get(), kSouthObliqCases,
                       std::size(kSouthObliqCases), 1e-11);
  AssertRoundTripCases(ell_high_flat.get(), kEtrsCases, std::size(kEtrsCases),
                       1e-10);
}

TEST(LaeaTest, DomainErrorsAndSingularities) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Antipodal forward singularities hit outside-domain error in all 4
  // spherical modes and all 4 ellipsoidal modes.
  struct AntipodeCase {
    const char* def;
    double lon_deg;
    double lat_deg;
  };
  constexpr AntipodeCase kAntipodes[] = {
      {.def = "+proj=laea +lat_0=90 +R=1", .lon_deg = 0.0, .lat_deg = -90.0},
      {.def = "+proj=laea +lat_0=-90 +R=1", .lon_deg = 0.0, .lat_deg = 90.0},
      {.def = "+proj=laea +lat_0=0 +R=1", .lon_deg = 180.0, .lat_deg = 0.0},
      {.def = "+proj=laea +lat_0=45 +lon_0=10 +R=1",
       .lon_deg = -170.0,
       .lat_deg = -45.0},
      {.def = "+proj=laea +lat_0=90 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = -90.0},
      {.def = "+proj=laea +lat_0=-90 +ellps=WGS84",
       .lon_deg = 0.0,
       .lat_deg = 90.0},
      {.def = "+proj=laea +lat_0=0 +ellps=WGS84",
       .lon_deg = 180.0,
       .lat_deg = 0.0},
      {.def = "+proj=laea +lat_0=52 +lon_0=10 +ellps=WGS84",
       .lon_deg = -170.0,
       .lat_deg = -52.0},
  };

  for (const auto& tc : kAntipodes) {
    ScopedPj pj(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pj.get()) << tc.def;
    const PJ_COORD bad = proj_trans(
        pj.get(), PJ_FWD,
        proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad.xy.x) << tc.def;
    EXPECT_EQ(HUGE_VAL, bad.xy.y) << tc.def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << tc.def;
    proj_errno_reset(pj.get());
  }

  // 2. Inverse outside projection disk: spherical rh > 2*R (all 4 modes) and
  // ellipsoidal EQUIT/OBLIQ rho > 2*rq*a.
  constexpr const char* kSphericalDefs[] = {
      "+proj=laea +lat_0=90 +R=1",
      "+proj=laea +lat_0=-90 +R=1",
      "+proj=laea +lat_0=0 +R=1",
      "+proj=laea +lat_0=45 +R=1",
  };
  for (const char* def : kSphericalDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    const PJ_COORD bad =
        proj_trans(pj.get(), PJ_INV, proj_coord(2.1, 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad.lp.lam) << def;
    EXPECT_EQ(HUGE_VAL, bad.lp.phi) << def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << def;
    proj_errno_reset(pj.get());
  }

  constexpr const char* kEllDiskDefs[] = {
      "+proj=laea +lat_0=0 +ellps=WGS84",
      "+proj=laea +lat_0=52 +ellps=WGS84",
  };
  for (const char* def : kEllDiskDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    const PJ_COORD bad =
        proj_trans(pj.get(), PJ_INV, proj_coord(2.0e7, 0.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, bad.lp.lam) << def;
    EXPECT_EQ(HUGE_VAL, bad.lp.phi) << def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << def;
    proj_errno_reset(pj.get());

    // Error reset recovers cleanly for subsequent transforms.
    const PJ_COORD ok =
        proj_trans(pj.get(), PJ_FWD, proj_coord(0.1, 0.2, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_FALSE(std::isinf(ok.xy.x));
  }

  // 3. Ellipsoidal N_POLE/S_POLE inverse with q > 2*qp feeds |ab| > 1 into
  // asin(ab), returning NaN latitude without setting proj_errno.
  constexpr const char* kEllPolarDefs[] = {
      "+proj=laea +lat_0=90 +ellps=WGS84",
      "+proj=laea +lat_0=-90 +ellps=WGS84",
  };
  for (const char* def : kEllPolarDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;
    const PJ_COORD out =
        proj_trans(pj.get(), PJ_INV, proj_coord(2.0e7, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isnan(out.lp.phi)) << def;
    EXPECT_FALSE(std::isnan(out.lp.lam)) << def;
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    proj_errno_reset(pj.get());
  }
}

TEST(LaeaTest, Batch4DFactorsAndEdgeInputs) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj ell_ob(
      proj_create(ctx.get(), "+proj=laea +lat_0=52 +lon_0=10 +ellps=WGS84"));
  ScopedPj sph_ob(
      proj_create(ctx.get(), "+proj=laea +lat_0=45 +lon_0=10 +R=6371000"));
  ASSERT_NE(nullptr, ell_ob.get());
  ASSERT_NE(nullptr, sph_ob.get());

  // 1. PJ_IDENT and 4D passthrough preserve z and t coordinates.
  const PJ_COORD orig4d =
      proj_coord(DegToRad(22.0), DegToRad(48.0), 345.25, 2026.5);
  const PJ_COORD ident = proj_trans(ell_ob.get(), PJ_IDENT, orig4d);
  EXPECT_EQ(0, proj_errno(ell_ob.get()));
  EXPECT_DOUBLE_EQ(orig4d.v[0], ident.v[0]);
  EXPECT_DOUBLE_EQ(orig4d.v[1], ident.v[1]);
  EXPECT_DOUBLE_EQ(orig4d.v[2], ident.v[2]);
  EXPECT_DOUBLE_EQ(orig4d.v[3], ident.v[3]);

  const PJ_COORD fwd4d = proj_trans(ell_ob.get(), PJ_FWD, orig4d);
  EXPECT_EQ(0, proj_errno(ell_ob.get()));
  EXPECT_DOUBLE_EQ(orig4d.xyzt.z, fwd4d.xyzt.z);
  EXPECT_DOUBLE_EQ(orig4d.xyzt.t, fwd4d.xyzt.t);
  const PJ_COORD inv4d = proj_trans(ell_ob.get(), PJ_INV, fwd4d);
  EXPECT_EQ(0, proj_errno(ell_ob.get()));
  EXPECT_NEAR(orig4d.lpzt.lam, inv4d.lpzt.lam, 1e-11);
  EXPECT_NEAR(orig4d.lpzt.phi, inv4d.lpzt.phi, 1e-11);
  EXPECT_DOUBLE_EQ(orig4d.lpzt.z, inv4d.lpzt.z);
  EXPECT_DOUBLE_EQ(orig4d.lpzt.t, inv4d.lpzt.t);

  // 2. proj_trans_array full round-trip and mixed-validity inverse batch.
  std::array<PJ_COORD, 3> batch = {
      proj_coord(DegToRad(10.0), DegToRad(52.0), 10.0, 2020.0),
      proj_coord(DegToRad(-5.0), DegToRad(40.0), 20.0, 2021.0),
      proj_coord(DegToRad(35.0), DegToRad(62.0), 30.0, 2022.0),
  };
  const std::array<PJ_COORD, 3> expected_batch = batch;
  ASSERT_EQ(0,
            proj_trans_array(ell_ob.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0,
            proj_trans_array(ell_ob.get(), PJ_INV, batch.size(), batch.data()));
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(expected_batch[i].lpzt.lam, batch[i].lpzt.lam, 1e-11);
    EXPECT_NEAR(expected_batch[i].lpzt.phi, batch[i].lpzt.phi, 1e-11);
    EXPECT_DOUBLE_EQ(expected_batch[i].lpzt.z, batch[i].lpzt.z);
    EXPECT_DOUBLE_EQ(expected_batch[i].lpzt.t, batch[i].lpzt.t);
  }

  std::array<PJ_COORD, 2> mixed_inv = {
      proj_trans(ell_ob.get(), PJ_FWD,
                 proj_coord(DegToRad(15.0), DegToRad(50.0), 0.0, 0.0)),
      proj_coord(2.0e7, 0.0, 0.0, 0.0),
  };
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_trans_array(ell_ob.get(), PJ_INV, mixed_inv.size(),
                             mixed_inv.data()));
  EXPECT_FALSE(std::isinf(mixed_inv[0].lp.lam));
  EXPECT_EQ(HUGE_VAL, mixed_inv[1].lp.lam);
  proj_errno_reset(ell_ob.get());

  // 3. proj_trans_generic strided round-trip.
  std::array<double, 2> lams = {DegToRad(5.0), DegToRad(25.0)};
  std::array<double, 2> phis = {DegToRad(46.0), DegToRad(58.0)};
  const std::array<double, 2> lams_orig = lams;
  const std::array<double, 2> phis_orig = phis;
  ASSERT_EQ(
      2u, proj_trans_generic(sph_ob.get(), PJ_FWD, lams.data(), sizeof(double),
                             lams.size(), phis.data(), sizeof(double),
                             phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(sph_ob.get()));
  ASSERT_EQ(
      2u, proj_trans_generic(sph_ob.get(), PJ_INV, lams.data(), sizeof(double),
                             lams.size(), phis.data(), sizeof(double),
                             phis.size(), nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(sph_ob.get()));
  for (size_t i = 0; i < lams.size(); ++i) {
    EXPECT_NEAR(lams_orig[i], lams[i], 1e-11);
    EXPECT_NEAR(phis_orig[i], phis[i], 1e-11);
  }

  // 4. proj_roundtrip and equal-area proj_factors verification across modes.
  PJ_COORD rt_ell = proj_coord(DegToRad(22.0), DegToRad(48.0), 0.0, 0.0);
  const double rt_ell_m = proj_roundtrip(ell_ob.get(), PJ_FWD, 3, &rt_ell);
  EXPECT_EQ(0, proj_errno(ell_ob.get()));
  EXPECT_GE(rt_ell_m, 0.0);
  EXPECT_LT(rt_ell_m, 1e-3);

  PJ_COORD rt_sph = proj_coord(DegToRad(22.0), DegToRad(48.0), 0.0, 0.0);
  const double rt_sph_m = proj_roundtrip(sph_ob.get(), PJ_FWD, 3, &rt_sph);
  EXPECT_EQ(0, proj_errno(sph_ob.get()));
  EXPECT_GE(rt_sph_m, 0.0);
  EXPECT_LT(rt_sph_m, 1e-3);

  // At the oblique projection center, distortion is zero (h = k = s = 1).
  const PJ_FACTORS fac_center = proj_factors(
      ell_ob.get(), proj_coord(DegToRad(10.0), DegToRad(52.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ell_ob.get()));
  EXPECT_NEAR(1.0, fac_center.meridional_scale, 1e-5);
  EXPECT_NEAR(1.0, fac_center.parallel_scale, 1e-5);
  EXPECT_NEAR(1.0, fac_center.areal_scale, 1e-5);
  EXPECT_NEAR(0.0, fac_center.angular_distortion, 1e-5);

  struct FactorProbe {
    const char* def;
    double lon_deg;
    double lat_deg;
  };
  constexpr FactorProbe kFactorProbes[] = {
      {.def = "+proj=laea +lat_0=52 +lon_0=10 +ellps=WGS84",
       .lon_deg = 30.0,
       .lat_deg = 35.0},
      {.def = "+proj=laea +lat_0=0 +ellps=WGS84",
       .lon_deg = 35.0,
       .lat_deg = 25.0},
      {.def = "+proj=laea +lat_0=90 +ellps=WGS84",
       .lon_deg = 45.0,
       .lat_deg = 60.0},
      {.def = "+proj=laea +lat_0=-90 +R=6371000",
       .lon_deg = -45.0,
       .lat_deg = -60.0},
      {.def = "+proj=laea +lat_0=45 +lon_0=10 +R=6371000",
       .lon_deg = 30.0,
       .lat_deg = 20.0},
  };
  for (const auto& probe : kFactorProbes) {
    ScopedPj pj(proj_create(ctx.get(), probe.def));
    ASSERT_NE(nullptr, pj.get()) << probe.def;
    const PJ_FACTORS fac = proj_factors(
        pj.get(),
        proj_coord(DegToRad(probe.lon_deg), DegToRad(probe.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << probe.def;
    EXPECT_NEAR(1.0, fac.areal_scale, 1e-5) << probe.def;
    EXPECT_GE(fac.tissot_semimajor, fac.tissot_semiminor) << probe.def;
    EXPECT_NEAR(1.0, fac.tissot_semimajor * fac.tissot_semiminor, 1e-5)
        << probe.def;
  }

  // 5. Invalid latitude, HUGE_VAL, NaN, and Inf edge inputs.
  const PJ_COORD bad_lat = proj_trans(
      ell_ob.get(), PJ_FWD, proj_coord(0.0, DegToRad(95.0), 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat.xy.x);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ell_ob.get()));
  proj_errno_reset(ell_ob.get());

  const PJ_COORD err_out = proj_trans(ell_ob.get(), PJ_FWD, proj_coord_error());
  EXPECT_TRUE(std::isinf(err_out.xy.x));
  proj_errno_reset(ell_ob.get());

  const PJ_COORD nan_out = proj_trans(
      ell_ob.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isnan(nan_out.xy.x));
  EXPECT_TRUE(std::isnan(nan_out.xy.y));
  EXPECT_EQ(0, proj_errno(ell_ob.get()));

  const PJ_COORD inf_lon = proj_trans(
      ell_ob.get(), PJ_FWD,
      proj_coord(std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_lon.xy.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ell_ob.get()));
  proj_errno_reset(ell_ob.get());

  const PJ_COORD neg_inf_lat = proj_trans(
      ell_ob.get(), PJ_FWD,
      proj_coord(0.0, -std::numeric_limits<double>::infinity(), 0.0, 0.0));
  EXPECT_TRUE(std::isinf(neg_inf_lat.xy.y));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ell_ob.get()));
  proj_errno_reset(ell_ob.get());
}

// TODO(schwehr): Uncovered lines in proj/src/projections/laea.cpp are limited
// to defensive null/OOM guards (pj_laea_destructor null checks, calloc/
// pj_authalic_lat_compute_coeffs allocation failures in PJ_PROJECTION(laea),
// and the init.cpp-shadowed |phi0| > M_HALFPI + EPS10 check).

}  // namespace
}  // namespace proj
