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
// Unit tests for projection scale and Tissot distortion factors
// (proj/src/factors.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/factors.cpp

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

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

void ExpectFactorsZeroed(const PJ_FACTORS& fac) {
  EXPECT_DOUBLE_EQ(0.0, fac.meridional_scale);
  EXPECT_DOUBLE_EQ(0.0, fac.parallel_scale);
  EXPECT_DOUBLE_EQ(0.0, fac.areal_scale);
  EXPECT_DOUBLE_EQ(0.0, fac.angular_distortion);
  EXPECT_DOUBLE_EQ(0.0, fac.meridian_parallel_angle);
  EXPECT_DOUBLE_EQ(0.0, fac.meridian_convergence);
  EXPECT_DOUBLE_EQ(0.0, fac.tissot_semimajor);
  EXPECT_DOUBLE_EQ(0.0, fac.tissot_semiminor);
  EXPECT_DOUBLE_EQ(0.0, fac.dx_dlam);
  EXPECT_DOUBLE_EQ(0.0, fac.dx_dphi);
  EXPECT_DOUBLE_EQ(0.0, fac.dy_dlam);
  EXPECT_DOUBLE_EQ(0.0, fac.dy_dphi);
}

TEST(FactorsTest, NullHandleAndCoordinateValidationBranches) {
  // 1. nullptr PJ returns zeroed PJ_FACTORS without crashing.
  ExpectFactorsZeroed(proj_factors(nullptr, proj_coord(0.0, 0.0, 0.0, 0.0)));

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // 2. HUGE_VAL / +inf longitude exits early cleanly without setting errno.
  ExpectFactorsZeroed(
      proj_factors(pj.get(), proj_coord(HUGE_VAL, 0.0, 0.0, 0.0)));
  EXPECT_EQ(0, proj_errno(pj.get()));

  ExpectFactorsZeroed(proj_factors(
      pj.get(),
      proj_coord(std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0)));
  EXPECT_EQ(0, proj_errno(pj.get()));

  // 3. Out-of-range latitude and longitude (including -inf) set INVALID_COORD.
  struct InvalidCoord {
    double lon_rad;
    double lat_rad;
  };
  constexpr std::array<InvalidCoord, 5> kInvalidCoords = {{
      {.lon_rad = 0.0, .lat_rad = kHalfPi + 1e-6},
      {.lon_rad = 0.0, .lat_rad = -kHalfPi - 1e-6},
      {.lon_rad = 10.1, .lat_rad = 0.0},
      {.lon_rad = -10.1, .lat_rad = 0.0},
      {.lon_rad = -std::numeric_limits<double>::infinity(), .lat_rad = 0.0},
  }};

  for (const auto& tc : kInvalidCoords) {
    proj_errno_reset(pj.get());
    ExpectFactorsZeroed(
        proj_factors(pj.get(), proj_coord(tc.lon_rad, tc.lat_rad, 0.0, 0.0)));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(pj.get()));
    proj_errno_reset(pj.get());
  }

  // 4. Valid call preserves pre-existing non-zero errno across pj_factors.
  proj_errno_set(pj.get(), PROJ_ERR_OTHER_API_MISUSE);
  const PJ_FACTORS valid =
      proj_factors(pj.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_NEAR(1.0, valid.meridional_scale, 1e-6);
  EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(pj.get()));
  proj_errno_reset(pj.get());
}

TEST(FactorsTest, SphericalAndEllipsoidalMercatorScaleAndDerivatives) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical Mercator (es == 0 branch): at phi = 60 deg, h = k = 1/cos(60)
  // = 2.0, s = 4.0, omega = 0, thetap = pi/2.
  ScopedPj sph(proj_create(ctx.get(), "+proj=merc +R=6371000"));
  ASSERT_NE(nullptr, sph.get());

  const PJ_FACTORS sph_60 =
      proj_factors(sph.get(), proj_coord(DegToRad(15.0), DegToRad(60.0), 0, 0));
  EXPECT_EQ(0, proj_errno(sph.get()));
  EXPECT_NEAR(2.0, sph_60.meridional_scale, 1e-6);
  EXPECT_NEAR(2.0, sph_60.parallel_scale, 1e-6);
  EXPECT_NEAR(4.0, sph_60.areal_scale, 1e-6);
  EXPECT_NEAR(0.0, sph_60.angular_distortion, 1e-5);
  EXPECT_NEAR(kHalfPi, sph_60.meridian_parallel_angle, 1e-5);
  EXPECT_NEAR(0.0, sph_60.meridian_convergence, 1e-7);
  EXPECT_NEAR(2.0, sph_60.tissot_semimajor, 1e-6);
  EXPECT_NEAR(2.0, sph_60.tissot_semiminor, 1e-6);
  EXPECT_NEAR(1.0, sph_60.dx_dlam, 1e-7);
  EXPECT_NEAR(0.0, sph_60.dx_dphi, 1e-7);
  EXPECT_NEAR(0.0, sph_60.dy_dlam, 1e-7);
  EXPECT_NEAR(2.0, sph_60.dy_dphi, 1e-6);

  // 2. Ellipsoidal Mercator (es != 0 branch): conformal (h == k) and
  // at lat = 45 deg, k = sqrt(1 - e^2 * sin^2(lat)) / cos(lat).
  ScopedPj ellps(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, ellps.get());

  const PJ_FACTORS ellps_0 =
      proj_factors(ellps.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ellps.get()));
  EXPECT_NEAR(1.0, ellps_0.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, ellps_0.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, ellps_0.dx_dlam, 1e-7);
  EXPECT_NEAR(1.0 - ellps->es, ellps_0.dy_dphi, 1e-6);

  const double lat45 = DegToRad(45.0);
  const double expected_k45 =
      std::sqrt(1.0 - ellps->es * std::sin(lat45) * std::sin(lat45)) /
      std::cos(lat45);
  const PJ_FACTORS ellps_45 =
      proj_factors(ellps.get(), proj_coord(0.0, lat45, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(ellps.get()));
  EXPECT_NEAR(expected_k45, ellps_45.meridional_scale, 1e-5);
  EXPECT_NEAR(expected_k45, ellps_45.parallel_scale, 1e-5);
  EXPECT_NEAR(expected_k45 * expected_k45, ellps_45.areal_scale, 1e-5);
  EXPECT_NEAR(0.0, ellps_45.angular_distortion, 1e-5);
}

TEST(FactorsTest, NonConformalTissotSinusoidalAndOverWrappingBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Non-conformal equal-area projection exercises t > 0 sqrt branch in
  // Tissot ellipse and angular distortion omega > 0, thetap < pi/2.
  ScopedPj sinu(proj_create(ctx.get(), "+proj=sinu +lon_0=0 +R=6371000"));
  ASSERT_NE(nullptr, sinu.get());

  const PJ_FACTORS non_cm = proj_factors(
      sinu.get(), proj_coord(DegToRad(30.0), DegToRad(45.0), 0, 0));
  EXPECT_EQ(0, proj_errno(sinu.get()));
  EXPECT_NEAR(1.0, non_cm.parallel_scale, 1e-6);
  EXPECT_NEAR(1.0, non_cm.areal_scale, 1e-6);
  EXPECT_GT(non_cm.meridional_scale, 1.0);
  EXPECT_GT(non_cm.angular_distortion, 0.0);
  EXPECT_LT(non_cm.meridian_parallel_angle, kHalfPi);
  EXPECT_GT(non_cm.tissot_semimajor, non_cm.tissot_semiminor);
  EXPECT_NEAR(1.0, non_cm.tissot_semimajor * non_cm.tissot_semiminor, 1e-5);

  // 2. Without +over, lon = 2*pi wraps via adjlon() to central meridian (0),
  // yielding zero distortion and h = 1. With +over, lon = 2*pi is NOT wrapped.
  const PJ_FACTORS wrapped =
      proj_factors(sinu.get(), proj_coord(2.0 * kPi, DegToRad(45.0), 0, 0));
  EXPECT_EQ(0, proj_errno(sinu.get()));
  EXPECT_NEAR(1.0, wrapped.meridional_scale, 1e-6);
  EXPECT_NEAR(0.0, wrapped.angular_distortion, 1e-5);

  ScopedPj sinu_over(
      proj_create(ctx.get(), "+proj=sinu +lon_0=0 +over +R=6371000"));
  ASSERT_NE(nullptr, sinu_over.get());
  const PJ_FACTORS unwrapped = proj_factors(
      sinu_over.get(), proj_coord(2.0 * kPi, DegToRad(45.0), 0, 0));
  EXPECT_EQ(0, proj_errno(sinu_over.get()));
  EXPECT_GT(unwrapped.meridional_scale, 2.0);
  EXPECT_GT(unwrapped.angular_distortion, 0.5);
}

TEST(FactorsTest, GeocentricLatitudeAndNearPoleClamping) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. +geoc converts input geocentric latitude to geodetic via INV before
  // computing factors.
  ScopedPj merc_geod(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ScopedPj merc_geoc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +geoc"));
  ASSERT_NE(nullptr, merc_geod.get());
  ASSERT_NE(nullptr, merc_geoc.get());

  const double lat_geocentric = DegToRad(45.0);
  const double lat_geodetic =
      std::atan(std::tan(lat_geocentric) / (1.0 - merc_geod->es));

  const PJ_FACTORS f_geoc =
      proj_factors(merc_geoc.get(), proj_coord(0.0, lat_geocentric, 0.0, 0.0));
  const PJ_FACTORS f_geod =
      proj_factors(merc_geod.get(), proj_coord(0.0, lat_geodetic, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_geoc.get()));
  EXPECT_EQ(0, proj_errno(merc_geod.get()));
  EXPECT_NEAR(f_geod.parallel_scale, f_geoc.parallel_scale, 1e-6);
  EXPECT_NEAR(f_geod.meridional_scale, f_geoc.meridional_scale, 1e-6);

  // 2. Exact north and south poles trigger the near-pole clamping branch
  // (fabs(lp.phi) > M_HALFPI - h) in both signs.
  ScopedPj north_stere(
      proj_create(ctx.get(), "+proj=stere +lat_0=90 +R=6371000"));
  ScopedPj south_stere(
      proj_create(ctx.get(), "+proj=stere +lat_0=-90 +R=6371000"));
  ASSERT_NE(nullptr, north_stere.get());
  ASSERT_NE(nullptr, south_stere.get());

  const PJ_FACTORS f_north =
      proj_factors(north_stere.get(), proj_coord(0.0, kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(north_stere.get()));
  EXPECT_NEAR(1.0, f_north.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, f_north.parallel_scale, 1e-6);

  const PJ_FACTORS f_south =
      proj_factors(south_stere.get(), proj_coord(0.0, -kHalfPi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(south_stere.get()));
  EXPECT_NEAR(1.0, f_south.meridional_scale, 1e-6);
  EXPECT_NEAR(1.0, f_south.parallel_scale, 1e-6);
}

TEST(FactorsTest, DerivFailureOnNoopAndOrthoAntipode) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. +proj=noop only defines fwd4d (fwd == nullptr), so pj_deriv() returns 1.
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  ExpectFactorsZeroed(proj_factors(noop.get(), proj_coord(0.0, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(noop.get()));

  // 2. Orthographic at antipode falls outside projection horizon (t.x ==
  // HUGE_VAL inside pj_deriv).
  ScopedPj ortho(proj_create(ctx.get(), "+proj=ortho +lat_0=0 +lon_0=0 +R=1"));
  ASSERT_NE(nullptr, ortho.get());
  ExpectFactorsZeroed(
      proj_factors(ortho.get(), proj_coord(kPi, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(ortho.get()));
}

TEST(FactorsTest, ProjectedCrsDatumEnsembleSingleDatumAndPrimeMeridian) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. EPSG:32632 uses WGS 84 datum ensemble (datum == nullptr,
  // datum_ensemble != nullptr). Second call exercises
  // cached_op_for_proj_factors.
  ScopedPj utm32n(proj_create(ctx.get(), "EPSG:32632"));
  ASSERT_NE(nullptr, utm32n.get());
  EXPECT_EQ(PJ_TYPE_PROJECTED_CRS, proj_get_type(utm32n.get()));
  EXPECT_EQ(nullptr, utm32n->cached_op_for_proj_factors);

  const PJ_FACTORS f_cm =
      proj_factors(utm32n.get(), proj_coord(DegToRad(9.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(utm32n.get()));
  EXPECT_NE(nullptr, utm32n->cached_op_for_proj_factors);
  EXPECT_NEAR(0.9996, f_cm.meridional_scale, 1e-6);
  EXPECT_NEAR(0.9996, f_cm.parallel_scale, 1e-6);

  const PJ_FACTORS f_off = proj_factors(
      utm32n.get(), proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(utm32n.get()));
  EXPECT_GT(f_off.parallel_scale, 0.9996);

  // 2. ProjectedCRS with explicit single DATUM (datum != nullptr, pm == 0)
  // and reversed Lat/Lon axes in base CRS.
  const char* single_datum_wkt =
      "PROJCRS[\"Single Datum Mercator\","
      "BASEGEOGCRS[\"GRS80 Geog\","
      "DATUM[\"GRS80 Datum\","
      "ELLIPSOID[\"GRS 1980\",6378137,298.257222101,"
      "LENGTHUNIT[\"metre\",1]]]],"
      "CONVERSION[\"Mercator\","
      "METHOD[\"Mercator (variant A)\",ID[\"EPSG\",9804]],"
      "PARAMETER[\"Latitude of natural origin\",0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "PARAMETER[\"Longitude of natural origin\",0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "PARAMETER[\"Scale factor at natural origin\",1,"
      "SCALEUNIT[\"unity\",1]],"
      "PARAMETER[\"False easting\",0,LENGTHUNIT[\"metre\",1]],"
      "PARAMETER[\"False northing\",0,LENGTHUNIT[\"metre\",1]]],"
      "CS[Cartesian,2],"
      "AXIS[\"Easting\",east,ORDER[1]],"
      "AXIS[\"Northing\",north,ORDER[2]],"
      "LENGTHUNIT[\"metre\",1]]";
  ScopedPj single_datum_crs(proj_create_from_wkt(ctx.get(), single_datum_wkt,
                                                 nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, single_datum_crs.get());
  constexpr double kGrs80InvF = 298.257222101;
  constexpr double kGrs80F = 1.0 / kGrs80InvF;
  constexpr double kGrs80Es = kGrs80F * (2.0 - kGrs80F);
  const double lat60 = DegToRad(60.0);
  const double expected_k60 =
      std::sqrt(1.0 - kGrs80Es * std::sin(lat60) * std::sin(lat60)) /
      std::cos(lat60);
  const PJ_FACTORS f_single =
      proj_factors(single_datum_crs.get(), proj_coord(0.0, lat60, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(single_datum_crs.get()));
  EXPECT_NEAR(expected_k60, f_single.meridional_scale, 1e-5);
  EXPECT_NEAR(expected_k60, f_single.parallel_scale, 1e-5);

  // 3. ProjectedCRS with non-Greenwich Prime Meridian (pm_longitude != 0).
  const char* paris_pm_wkt =
      "PROJCRS[\"Paris PM Mercator\","
      "BASEGEOGCRS[\"NTF (Paris)\","
      "DATUM[\"Nouvelle Triangulation Francaise (Paris)\","
      "ELLIPSOID[\"Clarke 1880 (IGN)\",6378249.2,293.466021293627,"
      "LENGTHUNIT[\"metre\",1]]],"
      "PRIMEM[\"Paris\",2.5969213,"
      "ANGLEUNIT[\"grad\",0.015707963267948967]]],"
      "CONVERSION[\"Mercator\","
      "METHOD[\"Mercator (variant A)\",ID[\"EPSG\",9804]],"
      "PARAMETER[\"Latitude of natural origin\",0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "PARAMETER[\"Longitude of natural origin\",0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "PARAMETER[\"Scale factor at natural origin\",1,"
      "SCALEUNIT[\"unity\",1]],"
      "PARAMETER[\"False easting\",0,LENGTHUNIT[\"metre\",1]],"
      "PARAMETER[\"False northing\",0,LENGTHUNIT[\"metre\",1]]],"
      "CS[Cartesian,2],"
      "AXIS[\"Easting\",east,ORDER[1]],"
      "AXIS[\"Northing\",north,ORDER[2]],"
      "LENGTHUNIT[\"metre\",1]]";
  ScopedPj paris_crs(
      proj_create_from_wkt(ctx.get(), paris_pm_wkt, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, paris_crs.get());
  const PJ_FACTORS f_paris =
      proj_factors(paris_crs.get(), proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(paris_crs.get()));
  EXPECT_NEAR(1.0, f_paris.meridional_scale, 1e-5);
  EXPECT_NEAR(1.0, f_paris.parallel_scale, 1e-5);
}

TEST(FactorsTest, DerivedProjectedCrsWithAndWithoutUnitConvert) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. DERIVED_PROJECTED_CRS via C API (EPSG:9624 affine parametric
  // conversion in metres, resized == false): 2x isotropic scaling on UTM 32N.
  ScopedPj utm32n(proj_create(ctx.get(), "EPSG:32632"));
  ASSERT_NE(nullptr, utm32n.get());
  ScopedPj affine_conv(proj_create_linear_affine_parametric_conversion(
      ctx.get(), "Scale2x", 100.0, "metre", 1.0, 2.0, "unity", 1.0, 0.0,
      "unity", 1.0, 200.0, "metre", 1.0, 0.0, "unity", 1.0, 2.0, "unity", 1.0));
  ASSERT_NE(nullptr, affine_conv.get());
  ScopedPj cs_metre(proj_create_cartesian_2D_cs(
      ctx.get(), PJ_CART2D_EASTING_NORTHING, "metre", 1.0));
  ASSERT_NE(nullptr, cs_metre.get());
  ScopedPj derived_m(proj_create_derived_projected_crs(
      ctx.get(), "UTM 32N Scaled 2x", utm32n.get(), affine_conv.get(),
      cs_metre.get()));
  ASSERT_NE(nullptr, derived_m.get());
  EXPECT_EQ(PJ_TYPE_DERIVED_PROJECTED_CRS, proj_get_type(derived_m.get()));

  const PJ_FACTORS d1 =
      proj_factors(derived_m.get(), proj_coord(DegToRad(9.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(derived_m.get()));
  EXPECT_NEAR(2.0 * 0.9996, d1.meridional_scale, 1e-5);
  EXPECT_NEAR(2.0 * 0.9996, d1.parallel_scale, 1e-5);
  EXPECT_NEAR(4.0 * 0.9996 * 0.9996, d1.areal_scale, 1e-5);

  // Second call exercises cached_op_for_proj_factors.
  const PJ_FACTORS d2 = proj_factors(
      derived_m.get(), proj_coord(DegToRad(9.0), DegToRad(45.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(derived_m.get()));
  EXPECT_NEAR(2.0 * 0.9996, d2.meridional_scale, 1e-5);

  // 2. DERIVED_PROJECTED_CRS using METHOD["PROJ affine"] + LENGTHUNIT["foot"]
  // emits trailing " +step +proj=unitconvert ...", triggering the resized=true
  // pipeline rebuild and newOp->fr_meter = 0.3048 in proj_factors.
  // Note: upstream factors.cpp also checks for " +step +proj=axiswap" (one 's'
  // typo vs "+proj=axisswap").
  const char* derived_foot_wkt =
      "DERIVEDPROJCRS[\"UTM 32N Shifted Foot\","
      "BASEPROJCRS[\"WGS 84 / UTM zone 32N\","
      "BASEGEOGCRS[\"WGS 84\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
      "LENGTHUNIT[\"metre\",1]]]],"
      "CONVERSION[\"UTM zone 32N\","
      "METHOD[\"Transverse Mercator\",ID[\"EPSG\",9807]],"
      "PARAMETER[\"Latitude of natural origin\",0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "PARAMETER[\"Longitude of natural origin\",9,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "PARAMETER[\"Scale factor at natural origin\",0.9996,"
      "SCALEUNIT[\"unity\",1]],"
      "PARAMETER[\"False easting\",500000,LENGTHUNIT[\"metre\",1]],"
      "PARAMETER[\"False northing\",0,LENGTHUNIT[\"metre\",1]]]],"
      "DERIVINGCONVERSION[\"PROJ Shift\","
      "METHOD[\"PROJ affine\"],"
      "PARAMETER[\"xoff\",100,LENGTHUNIT[\"metre\",1]],"
      "PARAMETER[\"yoff\",200,LENGTHUNIT[\"metre\",1]]],"
      "CS[Cartesian,2],"
      "AXIS[\"X\",east,ORDER[1]],"
      "AXIS[\"Y\",north,ORDER[2]],"
      "LENGTHUNIT[\"foot\",0.3048]]";
  ScopedPj derived_ft(proj_create_from_wkt(ctx.get(), derived_foot_wkt, nullptr,
                                           nullptr, nullptr));
  ASSERT_NE(nullptr, derived_ft.get());
  EXPECT_EQ(PJ_TYPE_DERIVED_PROJECTED_CRS, proj_get_type(derived_ft.get()));

  const PJ_FACTORS f_ft =
      proj_factors(derived_ft.get(), proj_coord(DegToRad(9.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(derived_ft.get()));
  EXPECT_NEAR(0.9996 * 0.3048, f_ft.meridional_scale, 1e-5);
  EXPECT_NEAR(0.9996 * 0.3048, f_ft.parallel_scale, 1e-5);
}

TEST(FactorsTest, CompoundCrsValidAndInvalidObjectTypeBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Valid COMPOUND_CRS whose sub-CRS 0 is PROJECTED_CRS (EPSG:32632).
  ScopedPj horiz_proj(proj_create(ctx.get(), "EPSG:32632"));
  ScopedPj vert(proj_create(ctx.get(), "EPSG:5703"));
  ASSERT_NE(nullptr, horiz_proj.get());
  ASSERT_NE(nullptr, vert.get());

  ScopedPj valid_compound(proj_create_compound_crs(
      ctx.get(), "UTM32N + NAVD88", horiz_proj.get(), vert.get()));
  ASSERT_NE(nullptr, valid_compound.get());
  EXPECT_EQ(PJ_TYPE_COMPOUND_CRS, proj_get_type(valid_compound.get()));

  const PJ_FACTORS f_comp = proj_factors(
      valid_compound.get(), proj_coord(DegToRad(9.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(valid_compound.get()));
  EXPECT_NEAR(0.9996, f_comp.meridional_scale, 1e-6);

  const PJ_FACTORS f_comp_cached = proj_factors(
      valid_compound.get(), proj_coord(DegToRad(9.0), DegToRad(30.0), 0, 0));
  EXPECT_EQ(0, proj_errno(valid_compound.get()));
  EXPECT_NEAR(0.9996, f_comp_cached.meridional_scale, 1e-5);

  // 2. COMPOUND_CRS whose sub-CRS 0 is Geographic2D fails type check and cleans
  // up the extracted horiz sub-CRS handle.
  ScopedPj horiz_geog(proj_create(ctx.get(), "EPSG:4326"));
  ASSERT_NE(nullptr, horiz_geog.get());
  ScopedPj invalid_compound(proj_create_compound_crs(
      ctx.get(), "WGS84 + NAVD88", horiz_geog.get(), vert.get()));
  ASSERT_NE(nullptr, invalid_compound.get());
  ExpectFactorsZeroed(
      proj_factors(invalid_compound.get(), proj_coord(0.0, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_errno(invalid_compound.get()));

  // 3. Unsupported PJ types (Geographic, Geocentric, Vertical, Ellipsoid)
  // reject with PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE.
  ExpectFactorsZeroed(
      proj_factors(horiz_geog.get(), proj_coord(0.0, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_errno(horiz_geog.get()));

  ScopedPj geocent(proj_create(ctx.get(), "EPSG:4978"));
  ASSERT_NE(nullptr, geocent.get());
  ExpectFactorsZeroed(
      proj_factors(geocent.get(), proj_coord(0.0, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(geocent.get()));

  ExpectFactorsZeroed(proj_factors(vert.get(), proj_coord(0.0, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(vert.get()));

  ScopedPj ellps(proj_get_ellipsoid(ctx.get(), horiz_geog.get()));
  ASSERT_NE(nullptr, ellps.get());
  ExpectFactorsZeroed(
      proj_factors(ellps.get(), proj_coord(0.0, 0.0, 0.0, 0.0)));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(ellps.get()));
}

}  // namespace
}  // namespace proj
