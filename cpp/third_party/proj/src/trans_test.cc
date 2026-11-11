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
// Unit tests for PROJ 4D coordinate transformation dispatch, multi-operation
// selection/retry/fallback, batch/generic/roundtrip APIs (proj/src/trans.cpp).
// Note: in google3 builds, network access is disabled and grids under /mem/
// are backed by InMemoryFileSystem rather than disk files.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/trans.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::IsNull;
using ::testing::NotNull;

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;

constexpr double DegToRad(double deg) { return deg * kDegToRad; }

// Builds a working gridless Longitude Rotation CoordinateOperation in degrees.
ScopedPj MakeLonRotOp(PJ_CONTEXT* ctx, const char* name, double offset_deg,
                      double accuracy, bool src_lat_lon = false,
                      bool dst_lat_lon = false) {
  const auto axis_type = [](bool lat_lon) {
    return lat_lon ? PJ_ELLPS2D_LATITUDE_LONGITUDE
                   : PJ_ELLPS2D_LONGITUDE_LATITUDE;
  };
  ScopedPj src_cs(
      proj_create_ellipsoidal_2D_cs(ctx, axis_type(src_lat_lon), nullptr, 0.0));
  ScopedPj dst_cs(
      proj_create_ellipsoidal_2D_cs(ctx, axis_type(dst_lat_lon), nullptr, 0.0));
  if (!src_cs || !dst_cs) {
    return nullptr;
  }
  ScopedPj src_crs(proj_create_geographic_crs(
      ctx, "Synthetic WGS84 Src", "World Geodetic System 1984", "WGS 84",
      6378137.0, 298.257223563, "Greenwich", 0.0, nullptr, 0.0, src_cs.get()));
  ScopedPj dst_crs(proj_create_geographic_crs(
      ctx, "Synthetic WGS84 Dst", "World Geodetic System 1984", "WGS 84",
      6378137.0, 298.257223563, "Greenwich", 0.0, nullptr, 0.0, dst_cs.get()));
  if (!src_crs || !dst_crs) {
    return nullptr;
  }
  const PJ_PARAM_DESCRIPTION param = {
      .name = "Longitude offset",
      .auth_name = "EPSG",
      .code = "8602",
      .value = offset_deg,
      .unit_name = "degree",
      .unit_conv_factor = 0.017453292519943295,
      .unit_type = PJ_UT_ANGULAR,
  };
  return ScopedPj(proj_create_transformation(
      ctx, name, nullptr, nullptr, src_crs.get(), dst_crs.get(), nullptr,
      "Longitude rotation", "EPSG", "9601", 1, &param, accuracy));
}

// Builds a non-instantiable NTv2 CoordinateOperation referencing a missing
// grid (and optionally an implicit "null" available grid so both availability
// branches in pj_warn_about_missing_grid() can be exercised).
ScopedPj MakeMissingNtv2Op(PJ_CONTEXT* ctx,
                           bool include_available_grid = false) {
  static constexpr char kWktMissingOnly[] =
      "COORDINATEOPERATION[\"Synthetic NTv2 missing grid\","
      "SOURCECRS[GEOGCRS[\"Synthetic LonLat\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"geodetic longitude (Lon)\",east,ORDER[1],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]],"
      "AXIS[\"geodetic latitude (Lat)\",north,ORDER[2],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]]]],"
      "TARGETCRS[GEOGCRS[\"Synthetic LonLat\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"geodetic longitude (Lon)\",east,ORDER[1],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]],"
      "AXIS[\"geodetic latitude (Lat)\",north,ORDER[2],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]]]],"
      "METHOD[\"NTv2\",ID[\"EPSG\",9615]],"
      "PARAMETERFILE[\"Latitude and longitude difference file\","
      "\"/mem/missing_test_grid.gsb\",ID[\"EPSG\",8656]],"
      "OPERATIONACCURACY[0.5]]";
  static constexpr char kWktMixedGrids[] =
      "COORDINATEOPERATION[\"Synthetic NTv2 mixed grid\","
      "SOURCECRS[GEOGCRS[\"Synthetic LonLat\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"geodetic longitude (Lon)\",east,ORDER[1],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]],"
      "AXIS[\"geodetic latitude (Lat)\",north,ORDER[2],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]]]],"
      "TARGETCRS[GEOGCRS[\"Synthetic LonLat\","
      "DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"geodetic longitude (Lon)\",east,ORDER[1],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]],"
      "AXIS[\"geodetic latitude (Lat)\",north,ORDER[2],"
      "ANGLEUNIT[\"degree\",0.017453292519943295]]]],"
      "METHOD[\"NTv2\",ID[\"EPSG\",9615]],"
      "PARAMETERFILE[\"Latitude and longitude difference file\","
      "\"null,/mem/missing_test_grid.gsb\",ID[\"EPSG\",8656]],"
      "OPERATIONACCURACY[0.5]]";

  ScopedLogSuppressor suppress(ctx);
  ScopedPj op(proj_create(
      ctx, include_available_grid ? kWktMixedGrids : kWktMissingOnly));
  if (op) {
    proj_errno_reset(op.get());
  }
  return op;
}

// Builds a geocentric XYZ (metres) -> geographic lon, lat (degrees) operation
// mirroring crs_to_crs.cpp's geocentric area-of-use helper.
ScopedPj MakeGeocentricToLonLatOp(PJ_CONTEXT* ctx) {
  ScopedPj geoc_crs(proj_create_geocentric_crs(
      ctx, "Synthetic WGS84 Geoc", "Synthetic Datum", "WGS 84", 6378137.0,
      298.257223563, "Greenwich", 0.0, nullptr, 0.0, nullptr, 0.0));
  ScopedPj cs(proj_create_ellipsoidal_2D_cs(ctx, PJ_ELLPS2D_LONGITUDE_LATITUDE,
                                            nullptr, 0.0));
  if (!geoc_crs || !cs) {
    return nullptr;
  }
  ScopedPj lonlat_crs(proj_create_geographic_crs(
      ctx, "Synthetic WGS84 LonLat", "Synthetic Datum", "WGS 84", 6378137.0,
      298.257223563, "Greenwich", 0.0, nullptr, 0.0, cs.get()));
  ScopedOpFactoryContext fctx(
      proj_create_operation_factory_context(ctx, nullptr));
  if (!lonlat_crs || !fctx) {
    return nullptr;
  }
  proj_operation_factory_context_set_spatial_criterion(
      ctx, fctx.get(), PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION);
  proj_operation_factory_context_set_grid_availability_use(
      ctx, fctx.get(),
      PROJ_GRID_AVAILABILITY_DISCARD_OPERATION_IF_MISSING_GRID);
  ScopedObjList list(proj_create_operations(ctx, geoc_crs.get(),
                                            lonlat_crs.get(), fctx.get()));
  if (!list || proj_list_get_count(list.get()) < 1) {
    return nullptr;
  }
  return ScopedPj(proj_list_get(ctx, list.get(), 0));
}

void FailOutsideGridOp(PJ_COORD& coo, PJ* P) {
  proj_errno_set(P, PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID);
  coo = proj_coord_error();
}

void FailNetworkErrorOp(PJ_COORD& coo, PJ* P) {
  proj_errno_set(P, PROJ_ERR_OTHER_NETWORK_ERROR);
  coo = proj_coord_error();
}

// Creates a minimal instantiable synthetic PJ* whose 4D forward/inverse
// callbacks fail with a caller-supplied error handler.
ScopedPj MakeFailingSyntheticOp(PJ_CONTEXT* ctx, PJ_OPERATOR fn) {
  ScopedPj op(pj_new());
  if (!op) {
    return nullptr;
  }
  op->ctx = ctx;
  op->skip_fwd_prepare = 1;
  op->skip_fwd_finalize = 1;
  op->skip_inv_prepare = 1;
  op->skip_inv_finalize = 1;
  op->fwd4d = fn;
  op->inv4d = fn;
  return op;
}

void AppendCoordOp(std::vector<PJCoordOperation>& out, int idx, double minx_src,
                   double miny_src, double maxx_src, double maxy_src,
                   double minx_dst, double miny_dst, double maxx_dst,
                   double maxy_dst, ScopedPj op, const char* name,
                   double accuracy, double pseudo_area, const char* area_name,
                   const PJ* src_geoc = nullptr, const PJ* dst_geoc = nullptr) {
  PJ_CONTEXT* ctx = op ? op->ctx : nullptr;
  ScopedLogSuppressor suppress(ctx);
  out.emplace_back(idx, minx_src, miny_src, maxx_src, maxy_src, minx_dst,
                   miny_dst, maxx_dst, maxy_dst, op.release(), name, accuracy,
                   pseudo_area, area_name, src_geoc, dst_geoc);
  if (ctx != nullptr) {
    proj_context_errno_set(ctx, 0);
  }
}

TEST(TransTest, TransBasicHandlesNullIdentInvertedAndNonOperationIsoObj) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  const PJ_COORD input = proj_coord(12.5, 55.25, 10.0, 2020.0);

  // 1. nullptr PJ returns input verbatim without touching state.
  const PJ_COORD from_null = proj_trans(nullptr, PJ_FWD, input);
  EXPECT_EQ(12.5, from_null.xyzt.x);
  EXPECT_EQ(55.25, from_null.xyzt.y);
  EXPECT_EQ(10.0, from_null.xyzt.z);
  EXPECT_EQ(2020.0, from_null.xyzt.t);

  // 2. PJ_IDENT returns input unmodified; P->inverted flips PJ_FWD <-> PJ_INV.
  ScopedPj helmert(proj_create(ctx.get(), "+proj=helmert +x=10 +y=-5 +z=2"));
  ASSERT_THAT(helmert.get(), NotNull());

  const PJ_COORD ident = proj_trans(helmert.get(), PJ_IDENT, input);
  EXPECT_EQ(0, proj_errno(helmert.get()));
  EXPECT_DOUBLE_EQ(input.xyzt.x, ident.xyzt.x);
  EXPECT_DOUBLE_EQ(input.xyzt.y, ident.xyzt.y);
  EXPECT_DOUBLE_EQ(input.xyzt.z, ident.xyzt.z);

  helmert->inverted = 1;
  const PJ_COORD inverted_fwd = proj_trans(helmert.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(helmert.get()));
  EXPECT_DOUBLE_EQ(2.5, inverted_fwd.xyzt.x);
  EXPECT_DOUBLE_EQ(60.25, inverted_fwd.xyzt.y);
  EXPECT_DOUBLE_EQ(8.0, inverted_fwd.xyzt.z);

  const PJ_COORD inverted_inv = proj_trans(helmert.get(), PJ_INV, inverted_fwd);
  EXPECT_EQ(0, proj_errno(helmert.get()));
  EXPECT_DOUBLE_EQ(input.xyzt.x, inverted_inv.xyzt.x);
  EXPECT_DOUBLE_EQ(input.xyzt.y, inverted_inv.xyzt.y);
  EXPECT_DOUBLE_EQ(input.xyzt.z, inverted_inv.xyzt.z);

  // 3. Passing an ISO-19111 CRS (iso_obj != nullptr and
  // !iso_obj_is_coordinate_operation) must log an error and fail with
  // PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE.
  ScopedPj crs(proj_create(ctx.get(), "EPSG:4326"));
  ASSERT_THAT(crs.get(), NotNull());
  ASSERT_NE(nullptr, crs->iso_obj.get());
  ASSERT_FALSE(crs->iso_obj_is_coordinate_operation);

  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
  const PJ_COORD crs_res = proj_trans(crs.get(), PJ_FWD, input);
  EXPECT_EQ(HUGE_VAL, crs_res.xyzt.x);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(crs.get()));
  EXPECT_TRUE(capture.ContainsMessage("Object is not a coordinate operation"));
  proj_errno_reset(crs.get());
}

TEST(TransTest, TransSingleOpCoversEpochsNaNsSphericalAndEllipsoidal) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // 1. Single-op P->hasCoordinateEpoch overrides input time in forward/inverse.
  ScopedPj timed_helmert(
      proj_create(ctx.get(), "+proj=helmert +x=10 +dx=2 +t_epoch=2020.0"));
  ASSERT_THAT(timed_helmert.get(), NotNull());
  timed_helmert->hasCoordinateEpoch = true;
  timed_helmert->coordinateEpoch = 2022.0;

  const PJ_COORD epoch_fwd = proj_trans(
      timed_helmert.get(), PJ_FWD, proj_coord(100.0, 200.0, 300.0, 2020.0));
  EXPECT_EQ(0, proj_errno(timed_helmert.get()));
  EXPECT_DOUBLE_EQ(114.0, epoch_fwd.xyzt.x);
  EXPECT_DOUBLE_EQ(2022.0, epoch_fwd.xyzt.t);

  const PJ_COORD epoch_inv = proj_trans(
      timed_helmert.get(), PJ_INV, proj_coord(114.0, 200.0, 300.0, 2020.0));
  EXPECT_EQ(0, proj_errno(timed_helmert.get()));
  EXPECT_DOUBLE_EQ(100.0, epoch_inv.xyzt.x);
  EXPECT_DOUBLE_EQ(2022.0, epoch_inv.xyzt.t);
  timed_helmert->hasCoordinateEpoch = false;

  // 2. Any NaN component propagates to all 4 output components
  // (pj_coord_has_nans).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::array<PJ_COORD, 4> nan_inputs = {
      proj_coord(nan, 0.0, 0.0, 0.0),
      proj_coord(0.0, nan, 0.0, 0.0),
      proj_coord(0.0, 0.0, nan, 0.0),
      proj_coord(0.0, 0.0, 0.0, nan),
  };
  for (size_t i = 0; i < nan_inputs.size(); ++i) {
    const PJ_COORD res = proj_trans(timed_helmert.get(), PJ_FWD, nan_inputs[i]);
    EXPECT_EQ(0, proj_errno(timed_helmert.get())) << "component " << i;
    for (int c = 0; c < 4; ++c) {
      EXPECT_TRUE(std::isnan(res.v[c])) << "nan input " << i << " comp " << c;
    }
  }

  // 3. Spherical vs ellipsoidal Mercator forward/inverse, proj_factors, and
  // domain singularities.
  ScopedPj merc_ellps(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ScopedPj merc_sphere(proj_create(ctx.get(), "+proj=merc +R=6371000"));
  ASSERT_THAT(merc_ellps.get(), NotNull());
  ASSERT_THAT(merc_sphere.get(), NotNull());

  const PJ_COORD lp = proj_coord(DegToRad(12.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_COORD xy_ellps = proj_trans(merc_ellps.get(), PJ_FWD, lp);
  const PJ_COORD xy_sphere = proj_trans(merc_sphere.get(), PJ_FWD, lp);
  EXPECT_EQ(0, proj_errno(merc_ellps.get()));
  EXPECT_EQ(0, proj_errno(merc_sphere.get()));
  EXPECT_GT(std::abs(xy_ellps.xy.y - xy_sphere.xy.y), 10000.0);

  const PJ_COORD back_ellps = proj_trans(merc_ellps.get(), PJ_INV, xy_ellps);
  const PJ_COORD back_sphere = proj_trans(merc_sphere.get(), PJ_INV, xy_sphere);
  EXPECT_EQ(0, proj_errno(merc_ellps.get()));
  EXPECT_EQ(0, proj_errno(merc_sphere.get()));
  EXPECT_NEAR(lp.lp.lam, back_ellps.lp.lam, 1e-12);
  EXPECT_NEAR(lp.lp.phi, back_ellps.lp.phi, 1e-12);
  EXPECT_NEAR(lp.lp.lam, back_sphere.lp.lam, 1e-12);
  EXPECT_NEAR(lp.lp.phi, back_sphere.lp.phi, 1e-12);

  const PJ_FACTORS factors = proj_factors(merc_ellps.get(), lp);
  EXPECT_EQ(0, proj_errno(merc_ellps.get()));
  EXPECT_NEAR(factors.meridional_scale, factors.parallel_scale, 1e-9);
  EXPECT_NEAR(0.0, factors.angular_distortion, 1e-9);
  EXPECT_GT(factors.areal_scale, 1.9);

  {
    ScopedLogSuppressor suppress(ctx.get());
    const PJ_COORD invalid_lat =
        proj_trans(merc_ellps.get(), PJ_FWD, proj_coord(0.0, 2.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, invalid_lat.xy.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
              proj_errno(merc_ellps.get()));
    proj_errno_reset(merc_ellps.get());
  }

  // 4. Kinematic deformation grid via InMemoryFileSystem covering missing-time
  // (t == HUGE_VAL -> PROJ_ERR_COORD_TRANSFM_MISSING_TIME) and 4D shift.
  InMemoryFileSystem fs;
  // CTable2 velocities (E=100 mm/yr, N=200 mm/yr) over [-5..5] deg.
  const std::vector<float> vel_xy = {
      100.0f, 200.0f, 100.0f, 200.0f, 100.0f, 200.0f, 100.0f, 200.0f, 100.0f,
      200.0f, 100.0f, 200.0f, 100.0f, 200.0f, 100.0f, 200.0f, 100.0f, 200.0f,
  };
  const std::vector<float> vel_z(9, 300.0f);  // 300 mm/yr Up.
  fs.AddFile("/mem/trans_vel.ct2",
             BuildCTable2Grid(DegToRad(-5.0), DegToRad(-5.0), DegToRad(5.0),
                              DegToRad(5.0), 3, 3, vel_xy));
  fs.AddFile("/mem/trans_vvel.gtx",
             BuildGtxGrid(-5.0, -5.0, 5.0, 5.0, 3, 3, vel_z));
  ASSERT_TRUE(fs.Install(ctx.get()));

  ScopedPj def(
      proj_create(ctx.get(),
                  "+proj=deformation +ellps=WGS84 +xy_grids=/mem/trans_vel.ct2 "
                  "+z_grids=/mem/trans_vvel.gtx +t_epoch=2020.0"));
  ASSERT_THAT(def.get(), NotNull());

  {
    ScopedLogSuppressor suppress(ctx.get());
    const PJ_COORD missing_t = proj_trans(
        def.get(), PJ_FWD, proj_coord(6378137.0, 0.0, 0.0, HUGE_VAL));
    EXPECT_EQ(HUGE_VAL, missing_t.xyzt.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_MISSING_TIME, proj_errno(def.get()));
    proj_errno_reset(def.get());
  }

  // At (lon=0, lat=0), ENU -> XYZ maps U->X (0.3m/yr), E->Y (0.1m/yr),
  // N->Z (0.2m/yr) over dt = 2.0 years.
  const PJ_COORD def_fwd =
      proj_trans(def.get(), PJ_FWD, proj_coord(6378137.0, 0.0, 0.0, 2022.0));
  EXPECT_EQ(0, proj_errno(def.get()));
  EXPECT_NEAR(6378137.6, def_fwd.xyzt.x, 1e-4);
  EXPECT_NEAR(0.2, def_fwd.xyzt.y, 1e-4);
  EXPECT_NEAR(0.4, def_fwd.xyzt.z, 1e-4);

  const PJ_COORD def_inv = proj_trans(def.get(), PJ_INV, def_fwd);
  EXPECT_EQ(0, proj_errno(def.get()));
  EXPECT_NEAR(6378137.0, def_inv.xyzt.x, 1e-4);
  EXPECT_NEAR(0.0, def_inv.xyzt.y, 1e-4);
  EXPECT_NEAR(0.0, def_inv.xyzt.z, 1e-4);
}

TEST(TransTest,
     GetSuggestedOperationCoversDirectionsWrappingGeocentricAndTieBreaking) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  const int no_excl[2] = {-1, -1};

  // 1. Empty opList and fully-excluded opList return -1.
  std::vector<PJCoordOperation> empty_ops;
  EXPECT_EQ(-1, pj_get_suggested_operation(ctx.get(), empty_ops, no_excl,
                                           /*skipNonInstantiable=*/true, PJ_FWD,
                                           proj_coord(0.0, 0.0, 0.0, 0.0)));

  // 2. Longitude normalization across [-180, 180] and > +/-360 wrapping on both
  // LonLat and LatLon CRSs in forward and inverse directions.
  {
    std::vector<PJCoordOperation> ops;
    ScopedPj lonlat_op = MakeLonRotOp(ctx.get(), "LonLat wrap", 1.0, 1.0,
                                      /*src_lat_lon=*/false,
                                      /*dst_lat_lon=*/false);
    ASSERT_THAT(lonlat_op.get(), NotNull());
    // src box [-180,-165] x [-20,20]; dst box [165,180] x [-20,20]
    AppendCoordOp(ops, 0, -180.0, -20.0, -165.0, 20.0, 165.0, -20.0, 180.0,
                  20.0, std::move(lonlat_op), "LonLat wrap", 1.0, 100.0,
                  "Wrap");

    // Excluded index returns -1 even when spatial criteria match.
    const int excl_zero[2] = {0, -1};
    EXPECT_EQ(
        -1, pj_get_suggested_operation(ctx.get(), ops, excl_zero, true, PJ_FWD,
                                       proj_coord(-170.0, 0.0, 0.0, 0.0)));
    const int excl_second[2] = {-1, 0};
    EXPECT_EQ(-1, pj_get_suggested_operation(
                      ctx.get(), ops, excl_second, true, PJ_FWD,
                      proj_coord(-170.0, 0.0, 0.0, 0.0)));

    // Direct match and wrapped >180 (single -360, multi -360 via fmod, <-180).
    EXPECT_EQ(0,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_FWD,
                                         proj_coord(-170.0, 0.0, 0.0, 0.0)));
    EXPECT_EQ(0,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_FWD,
                                         proj_coord(190.0, 0.0, 0.0, 0.0)));
    EXPECT_EQ(0,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_FWD,
                                         proj_coord(550.0, 0.0, 0.0, 0.0)));
    EXPECT_EQ(0,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_FWD,
                                         proj_coord(-900.0, 0.0, 0.0, 0.0)));
    // x = -190 normalizes to +170 (hits x < -180 branch without inner fmod)
    // and x = 20 hits the unchanged [-180, 180] return x branch, both missing
    // [-180, -165].
    EXPECT_EQ(-1,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_FWD,
                                         proj_coord(-190.0, 0.0, 0.0, 0.0)));
    EXPECT_EQ(-1,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_FWD,
                                         proj_coord(20.0, 0.0, 0.0, 0.0)));

    // PJ_INV against [165, 180]: -190 wraps to +170 (hits x < -180).
    EXPECT_EQ(0,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_INV,
                                         proj_coord(170.0, 0.0, 0.0, 0.0)));
    EXPECT_EQ(0,
              pj_get_suggested_operation(ctx.get(), ops, no_excl, true, PJ_INV,
                                         proj_coord(-190.0, 0.0, 0.0, 0.0)));

    // LatLon ordering (y is longitude): test dst/src LatLon wrap paths too.
    ScopedPj latlon_op = MakeLonRotOp(ctx.get(), "LatLon wrap", 1.0, 1.0,
                                      /*src_lat_lon=*/true,
                                      /*dst_lat_lon=*/true);
    ASSERT_THAT(latlon_op.get(), NotNull());
    std::vector<PJCoordOperation> latlon_ops;
    AppendCoordOp(latlon_ops, 0, -20.0, -180.0, 20.0, -165.0, -20.0, -180.0,
                  20.0, -165.0, std::move(latlon_op), "LatLon wrap", 1.0, 100.0,
                  "Wrap");
    EXPECT_EQ(0, pj_get_suggested_operation(ctx.get(), latlon_ops, no_excl,
                                            true, PJ_FWD,
                                            proj_coord(0.0, 550.0, 0.0, 0.0)));
    EXPECT_EQ(0, pj_get_suggested_operation(ctx.get(), latlon_ops, no_excl,
                                            true, PJ_INV,
                                            proj_coord(0.0, 550.0, 0.0, 0.0)));
    EXPECT_EQ(0, pj_get_suggested_operation(ctx.get(), latlon_ops, no_excl,
                                            true, PJ_INV,
                                            proj_coord(0.0, -900.0, 0.0, 0.0)));
  }

  // 3. Geocentric bbox checks (both global fast path and pj_fwd4d regional).
  {
    ScopedPj geoc_to_ll = MakeGeocentricToLonLatOp(ctx.get());
    ASSERT_THAT(geoc_to_ll.get(), NotNull());
    ScopedPj op_global = MakeLonRotOp(ctx.get(), "Global geoc", 1.0, 2.0);
    ScopedPj op_regional = MakeLonRotOp(ctx.get(), "East geoc", 1.0, 1.0);
    ASSERT_THAT(op_global.get(), NotNull());
    ASSERT_THAT(op_regional.get(), NotNull());

    std::vector<PJCoordOperation> ops;
    AppendCoordOp(ops, 0, -180.0, -90.0, 180.0, 90.0, -180.0, -90.0, 180.0,
                  90.0, std::move(op_global), "Global geoc", 2.0, 1000.0,
                  "Global", geoc_to_ll.get(), geoc_to_ll.get());
    AppendCoordOp(ops, 1, 80.0, -10.0, 100.0, 10.0, 80.0, -10.0, 100.0, 10.0,
                  std::move(op_regional), "East geoc", 1.0, 100.0, "East",
                  geoc_to_ll.get(), geoc_to_ll.get());

    // (0, 6378137, 0) corresponds to lon=90, lat=0 -> regional (idx 1) wins.
    const PJ_COORD lon90_xyz = proj_coord(0.0, 6378137.0, 0.0, 0.0);
    EXPECT_EQ(1, pj_get_suggested_operation(ctx.get(), ops, no_excl, true,
                                            PJ_FWD, lon90_xyz));
    EXPECT_EQ(1, pj_get_suggested_operation(ctx.get(), ops, no_excl, true,
                                            PJ_INV, lon90_xyz));

    // (6378137, 0, 0) corresponds to lon=0, lat=0 -> misses idx 1, picks global
    // fast path idx 0.
    const PJ_COORD lon0_xyz = proj_coord(6378137.0, 0.0, 0.0, 0.0);
    EXPECT_EQ(0, pj_get_suggested_operation(ctx.get(), ops, no_excl, true,
                                            PJ_FWD, lon0_xyz));
    EXPECT_EQ(0, pj_get_suggested_operation(ctx.get(), ops, no_excl, true,
                                            PJ_INV, lon0_xyz));
  }

  // 4. Tie-breaking: known accuracy > unknown accuracy (-1), better accuracy,
  // smaller pseudoArea, !isOffshore, !isUnknownAreaName, isPriorityOp, and
  // skipNonInstantiable. Notice coarse_onshore (acc 5.0) precedes unknown_acc
  // (acc -1.0) so bestAccuracy is valid (>= 0) and unknown_acc cannot win.
  {
    std::vector<PJCoordOperation> ops;
    ScopedPj coarse_onshore = MakeLonRotOp(ctx.get(), "Coarse", 1.0, 5.0);
    ScopedPj unknown_acc = MakeLonRotOp(ctx.get(), "Unknown acc", 1.0, -1.0);
    ScopedPj fine_large = MakeLonRotOp(ctx.get(), "Fine large", 1.0, 1.0);
    ScopedPj fine_offshore = MakeLonRotOp(ctx.get(), "Fine offshore", 1.0, 1.0);
    ScopedPj fine_unknown_area =
        MakeLonRotOp(ctx.get(), "Fine unknown area", 1.0, 1.0);
    ScopedPj fine_small = MakeLonRotOp(ctx.get(), "Fine small", 1.0, 1.0);
    ScopedPj missing_best = MakeMissingNtv2Op(ctx.get());
    ASSERT_THAT(coarse_onshore.get(), NotNull());
    ASSERT_THAT(unknown_acc.get(), NotNull());
    ASSERT_THAT(fine_large.get(), NotNull());
    ASSERT_THAT(fine_offshore.get(), NotNull());
    ASSERT_THAT(fine_unknown_area.get(), NotNull());
    ASSERT_THAT(fine_small.get(), NotNull());
    ASSERT_THAT(missing_best.get(), NotNull());

    AppendCoordOp(ops, 0, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(coarse_onshore), "Coarse", 5.0, 500.0, "Onshore");
    AppendCoordOp(ops, 1, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(unknown_acc), "Unknown acc", -1.0, 50.0, "Onshore");
    AppendCoordOp(ops, 2, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(fine_large), "Fine large", 1.0, 400.0,
                  "Named area");
    // Offshore and unknown-area candidates must NOT beat idx 2 despite smaller
    // pseudoArea, whereas named onshore idx 5 DOES beat idx 2.
    AppendCoordOp(ops, 3, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(fine_offshore), "Fine offshore", 1.0, 10.0,
                  "Region - offshore");
    AppendCoordOp(ops, 4, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(fine_unknown_area), "Fine unknown area", 1.0, 10.0,
                  "unknown");
    AppendCoordOp(ops, 5, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(fine_small), "Fine small", 1.0, 200.0,
                  "Smaller named");
    AppendCoordOp(ops, 6, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0, 10.0,
                  std::move(missing_best), "Missing best", 0.5, 500.0,
                  "Onshore");

    const PJ_COORD pt = proj_coord(0.0, 0.0, 0.0, 0.0);
    EXPECT_EQ(5, pj_get_suggested_operation(ctx.get(), ops, no_excl,
                                            /*skipNonInstantiable=*/true,
                                            PJ_FWD, pt));
    EXPECT_EQ(6, pj_get_suggested_operation(ctx.get(), ops, no_excl,
                                            /*skipNonInstantiable=*/false,
                                            PJ_FWD, pt));

    // isPriorityOp ("GDA94 to WGS 84 (1)") resists a smaller-area equal-acc op.
    std::vector<PJCoordOperation> priority_ops;
    ScopedPj priority =
        MakeLonRotOp(ctx.get(), "GDA94 to WGS 84 (1)", 1.0, 1.0);
    ScopedPj non_priority =
        MakeLonRotOp(ctx.get(), "Smaller non-prio", 1.0, 1.0);
    ASSERT_THAT(priority.get(), NotNull());
    ASSERT_THAT(non_priority.get(), NotNull());
    AppendCoordOp(priority_ops, 0, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0,
                  10.0, std::move(priority), "GDA94 to WGS 84 (1)", 1.0, 500.0,
                  "Australia");
    AppendCoordOp(priority_ops, 1, -10.0, -10.0, 10.0, 10.0, -10.0, -10.0, 10.0,
                  10.0, std::move(non_priority), "Smaller non-prio", 1.0, 50.0,
                  "Subset");
    EXPECT_EQ(0, pj_get_suggested_operation(ctx.get(), priority_ops, no_excl,
                                            true, PJ_FWD, pt));
  }
}

TEST(TransTest, WarnAboutMissingGridCoversDebugWarningAndErrorModes) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // 1. Mixed grids ("null,/mem/missing_test_grid.gsb") exercises both the
  // !available (missing grid) and available ("null") branches inside the loop.
  ScopedPj missing_op =
      MakeMissingNtv2Op(ctx.get(), /*include_available_grid=*/true);
  ASSERT_THAT(missing_op.get(), NotNull());
  ASSERT_EQ(
      2, proj_coordoperation_get_grid_used_count(ctx.get(), missing_op.get()));

  missing_op->errorIfBestTransformationNotAvailable = false;
  missing_op->warnIfBestTransformationNotAvailable = true;

  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    pj_warn_about_missing_grid(missing_op.get());
    EXPECT_FALSE(missing_op->warnIfBestTransformationNotAvailable);
    EXPECT_TRUE(capture.ContainsMessage(
        "Grid /mem/missing_test_grid.gsb is not available."));
    EXPECT_TRUE(
        capture.ContainsMessage("This might become an error in a future"));
    ASSERT_FALSE(capture.entries().empty());
    EXPECT_EQ(PJ_LOG_DEBUG, capture.entries().back().level);

    // Subsequent warning call no longer appends the once-per-instance notice.
    capture.Clear();
    pj_warn_about_missing_grid(missing_op.get());
    EXPECT_TRUE(capture.ContainsMessage(
        "Grid /mem/missing_test_grid.gsb is not available."));
    EXPECT_FALSE(
        capture.ContainsMessage("This might become an error in a future"));
    ASSERT_FALSE(capture.entries().empty());
    EXPECT_EQ(PJ_LOG_DEBUG, capture.entries().back().level);
  }

  // 2. errorIfBestTransformationNotAvailable logs at PJ_LOG_ERROR without
  // appending the migration warning.
  missing_op->errorIfBestTransformationNotAvailable = true;
  missing_op->warnIfBestTransformationNotAvailable = true;
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    pj_warn_about_missing_grid(missing_op.get());
    EXPECT_TRUE(capture.ContainsMessage(
        "Grid /mem/missing_test_grid.gsb is not available."));
    EXPECT_FALSE(
        capture.ContainsMessage("This might become an error in a future"));
    ASSERT_FALSE(capture.entries().empty());
    EXPECT_EQ(PJ_LOG_ERROR, capture.entries().back().level);
  }

  // 3. Operation with zero referenced grids executes zero loop iterations.
  ScopedPj no_grid_op = MakeLonRotOp(ctx.get(), "No grid op", 1.0, 1.0);
  ASSERT_THAT(no_grid_op.get(), NotNull());
  no_grid_op->errorIfBestTransformationNotAvailable = false;
  no_grid_op->warnIfBestTransformationNotAvailable = false;
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    pj_warn_about_missing_grid(no_grid_op.get());
    EXPECT_TRUE(capture.ContainsMessage("No grid op failed."));
    ASSERT_FALSE(capture.entries().empty());
    EXPECT_EQ(PJ_LOG_DEBUG, capture.entries().back().level);
  }
}

TEST(TransTest, TransMultiOpCoversSelectionRetryNetworkErrorAndFallback) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ASSERT_THAT(ctx->get_cpp_context(), NotNull());

  // 1. Warn-then-retry path: idx 0 missing grid warns, flips flags, and
  // retries idx 1 (with hasCoordinateEpoch = true) in PJ_FWD and PJ_INV.
  {
    ScopedPj parent(proj_create(ctx.get(), "+proj=noop"));
    ASSERT_THAT(parent.get(), NotNull());
    parent->warnIfBestTransformationNotAvailable = true;
    parent->errorIfBestTransformationNotAvailable = false;
    parent->alternativeCoordinateOperations.reserve(2);

    ScopedPj missing = MakeMissingNtv2Op(ctx.get());
    ScopedPj backup = MakeLonRotOp(ctx.get(), "Backup +2 deg", 2.0, 2.0);
    ASSERT_THAT(missing.get(), NotNull());
    ASSERT_THAT(backup.get(), NotNull());
    backup->hasCoordinateEpoch = true;
    backup->coordinateEpoch = 2024.5;

    AppendCoordOp(parent->alternativeCoordinateOperations, 0, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(missing),
                  "Missing grid op", 0.5, 100.0, "Domain");
    AppendCoordOp(parent->alternativeCoordinateOperations, 1, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(backup),
                  "Backup +2 deg", 2.0, 100.0, "Domain");

    ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
    const PJ_COORD fwd =
        proj_trans(parent.get(), PJ_FWD, proj_coord(5.0, 5.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(parent.get()));
    EXPECT_NEAR(7.0, fwd.xyzt.x, 1e-12);
    EXPECT_NEAR(5.0, fwd.xyzt.y, 1e-12);
    EXPECT_EQ(1, parent->iCurCoordOp);
    EXPECT_FALSE(parent->warnIfBestTransformationNotAvailable);
    EXPECT_TRUE(capture.ContainsMessage("Grid /mem/missing_test_grid.gsb"));
    EXPECT_TRUE(capture.ContainsMessage("Attempting a retry"));
    EXPECT_TRUE(
        capture.ContainsMessage("Using coordinate operation Backup +2 deg"));

    // Subsequent call with iCurCoordOp == 1 skips debug operation-change log.
    capture.Clear();
    const PJ_COORD inv = proj_trans(parent.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(parent.get()));
    EXPECT_NEAR(5.0, inv.xyzt.x, 1e-12);
    EXPECT_NEAR(5.0, inv.xyzt.y, 1e-12);
    EXPECT_FALSE(capture.ContainsMessage("Using coordinate operation"));
  }

  // 2. errorIfBestTransformationNotAvailable = true stops without retrying.
  {
    ScopedPj parent(proj_create(ctx.get(), "+proj=noop"));
    ASSERT_THAT(parent.get(), NotNull());
    parent->errorIfBestTransformationNotAvailable = true;
    parent->warnIfBestTransformationNotAvailable = false;
    parent->alternativeCoordinateOperations.reserve(2);

    ScopedPj missing = MakeMissingNtv2Op(ctx.get());
    ScopedPj backup = MakeLonRotOp(ctx.get(), "Unused backup", 2.0, 2.0);
    ASSERT_THAT(missing.get(), NotNull());
    ASSERT_THAT(backup.get(), NotNull());

    AppendCoordOp(parent->alternativeCoordinateOperations, 0, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(missing),
                  "Missing grid op", 0.5, 100.0, "Domain");
    AppendCoordOp(parent->alternativeCoordinateOperations, 1, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(backup),
                  "Unused backup", 2.0, 100.0, "Domain");

    ScopedLogSuppressor suppress(ctx.get());
    const PJ_COORD err =
        proj_trans(parent.get(), PJ_FWD, proj_coord(5.0, 5.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, err.xyzt.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_NO_OPERATION, proj_errno(parent.get()));
    EXPECT_EQ(0, parent->iCurCoordOp);
    proj_errno_reset(parent.get());
  }

  // 3. PROJ_ERR_OTHER_NETWORK_ERROR returns immediately without retry/fallback.
  {
    ScopedPj parent(proj_create(ctx.get(), "+proj=noop"));
    ASSERT_THAT(parent.get(), NotNull());
    parent->warnIfBestTransformationNotAvailable = false;
    parent->skipNonInstantiable = false;
    parent->alternativeCoordinateOperations.reserve(2);

    ScopedPj net_fail = MakeFailingSyntheticOp(ctx.get(), &FailNetworkErrorOp);
    ScopedPj backup = MakeLonRotOp(ctx.get(), "Backup after net", 2.0, 5.0);
    ASSERT_THAT(net_fail.get(), NotNull());
    ASSERT_THAT(backup.get(), NotNull());

    AppendCoordOp(parent->alternativeCoordinateOperations, 0, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(net_fail),
                  "Network error op", 0.1, 100.0, "Domain");
    AppendCoordOp(parent->alternativeCoordinateOperations, 1, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(backup),
                  "Backup after net", 5.0, 100.0, "Domain");

    ScopedLogSuppressor suppress(ctx.get());
    const PJ_COORD err =
        proj_trans(parent.get(), PJ_FWD, proj_coord(5.0, 5.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, err.xyzt.x);
    EXPECT_EQ(PROJ_ERR_OTHER_NETWORK_ERROR, proj_errno(parent.get()));
    EXPECT_EQ(0, parent->iCurCoordOp);
    proj_errno_reset(parent.get());
  }

  // 4. Exhaust N_MAX_RETRY (3 failing ops) -> falls through to first gridless
  // CoordinateOperation in both PJ_FWD and PJ_INV; also verify non-DEBUG path.
  {
    ScopedPj parent(proj_create(ctx.get(), "+proj=noop"));
    ASSERT_THAT(parent.get(), NotNull());
    parent->warnIfBestTransformationNotAvailable = false;
    parent->skipNonInstantiable = false;
    parent->alternativeCoordinateOperations.reserve(5);

    for (int i = 0; i < 3; ++i) {
      ScopedPj fail_op = MakeFailingSyntheticOp(ctx.get(), &FailOutsideGridOp);
      ASSERT_THAT(fail_op.get(), NotNull());
      AppendCoordOp(parent->alternativeCoordinateOperations, i, -10.0, -10.0,
                    10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(fail_op),
                    "Synthetic fail", 0.1 * (i + 1), 100.0, "Domain");
    }
    ScopedPj missing_fallback = MakeMissingNtv2Op(ctx.get());
    ScopedPj gridless_fallback =
        MakeLonRotOp(ctx.get(), "Gridless fallback +3 deg", 3.0, 10.0);
    ASSERT_THAT(missing_fallback.get(), NotNull());
    ASSERT_THAT(gridless_fallback.get(), NotNull());
    // Out-of-point-bbox for retry so N_MAX_RETRY exhausts on ops 0..2, then
    // fallback skips 0..2 (no iso_obj) and 3 (requires grid), picking idx 4.
    AppendCoordOp(parent->alternativeCoordinateOperations, 3, 200.0, 0.0, 210.0,
                  10.0, 200.0, 0.0, 210.0, 10.0, std::move(missing_fallback),
                  "Missing grid fallback candidate", 1.0, 100.0, "Elsewhere");
    AppendCoordOp(parent->alternativeCoordinateOperations, 4, 200.0, 0.0, 210.0,
                  10.0, 200.0, 0.0, 210.0, 10.0, std::move(gridless_fallback),
                  "Gridless fallback +3 deg", 10.0, 100.0, "Elsewhere");

    {
      ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);
      const PJ_COORD fallback_fwd =
          proj_trans(parent.get(), PJ_FWD, proj_coord(5.0, 5.0, 0.0, 0.0));
      EXPECT_NEAR(8.0, fallback_fwd.xyzt.x, 1e-12);
      EXPECT_EQ(4, parent->iCurCoordOp);
      EXPECT_TRUE(capture.ContainsMessage(
          "as a fallback due to lack of more appropriate"));
      proj_errno_reset(parent.get());

      // Repeating fallback when iCurCoordOp is already 4 (point outside all
      // bboxes so iBest < 0 immediately) skips the duplicate fallback log.
      capture.Clear();
      const PJ_COORD repeat_fallback =
          proj_trans(parent.get(), PJ_FWD, proj_coord(50.0, 5.0, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(parent.get()));
      EXPECT_NEAR(53.0, repeat_fallback.xyzt.x, 1e-12);
      EXPECT_EQ(4, parent->iCurCoordOp);
      EXPECT_FALSE(capture.ContainsMessage(
          "as a fallback due to lack of more appropriate"));
    }

    // Exercise retry and fallback paths with log level < PJ_LOG_DEBUG and
    // PJ_INV.
    ScopedLogSuppressor suppress(ctx.get());
    const PJ_COORD fallback_inv =
        proj_trans(parent.get(), PJ_INV, proj_coord(8.0, 5.0, 0.0, 0.0));
    EXPECT_NEAR(5.0, fallback_inv.xyzt.x, 1e-12);
    proj_errno_reset(parent.get());
  }

  // 5. Out-of-area with zero gridless CoordinateOperations sets
  // PROJ_ERR_COORD_TRANSFM_NO_OPERATION and returns proj_coord_error().
  {
    ScopedPj parent(proj_create(ctx.get(), "+proj=noop"));
    ASSERT_THAT(parent.get(), NotNull());
    parent->warnIfBestTransformationNotAvailable = false;
    parent->alternativeCoordinateOperations.reserve(1);

    ScopedPj missing = MakeMissingNtv2Op(ctx.get());
    ASSERT_THAT(missing.get(), NotNull());
    AppendCoordOp(parent->alternativeCoordinateOperations, 0, -10.0, -10.0,
                  10.0, 10.0, -10.0, -10.0, 10.0, 10.0, std::move(missing),
                  "Only missing", 1.0, 100.0, "Elsewhere");

    ScopedLogSuppressor suppress(ctx.get());
    const PJ_COORD no_op =
        proj_trans(parent.get(), PJ_FWD, proj_coord(50.0, 50.0, 0.0, 0.0));
    EXPECT_EQ(HUGE_VAL, no_op.xyzt.x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_NO_OPERATION, proj_errno(parent.get()));
    proj_errno_reset(parent.get());
  }
}

TEST(TransTest, TransGetLastUsedOperationCoversNullSingleAndMultiOp) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // 1. nullptr PJ and multi-op PJ prior to any proj_trans() call return
  // nullptr.
  EXPECT_THAT(proj_trans_get_last_used_operation(nullptr), IsNull());

  ScopedPj parent(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_THAT(parent.get(), NotNull());
  parent->alternativeCoordinateOperations.reserve(2);
  ScopedPj op0 = MakeLonRotOp(ctx.get(), "OpZero", 1.0, 5.0);
  ScopedPj op1 = MakeLonRotOp(ctx.get(), "OpOne", 2.0, 1.0);
  ASSERT_THAT(op0.get(), NotNull());
  ASSERT_THAT(op1.get(), NotNull());
  AppendCoordOp(parent->alternativeCoordinateOperations, 0, -10.0, -10.0, 10.0,
                10.0, -10.0, -10.0, 10.0, 10.0, std::move(op0), "OpZero", 5.0,
                100.0, "Domain");
  AppendCoordOp(parent->alternativeCoordinateOperations, 1, -10.0, -10.0, 10.0,
                10.0, -10.0, -10.0, 10.0, 10.0, std::move(op1), "OpOne", 1.0,
                100.0, "Domain");

  ASSERT_EQ(-1, parent->iCurCoordOp);
  EXPECT_THAT(proj_trans_get_last_used_operation(parent.get()), IsNull());

  // 2. Multi-op returns clone of selected alternative operation.
  const PJ_COORD shifted =
      proj_trans(parent.get(), PJ_FWD, proj_coord(1.0, 2.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(parent.get()));
  EXPECT_NEAR(3.0, shifted.xyzt.x, 1e-12);
  ScopedPj last_multi(proj_trans_get_last_used_operation(parent.get()));
  ASSERT_THAT(last_multi.get(), NotNull());
  EXPECT_STREQ("OpOne", proj_get_name(last_multi.get()));

  // 3. Single-op returns a working clone of P itself.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(merc.get(), NotNull());
  const PJ_COORD pt =
      proj_trans(merc.get(), PJ_FWD, proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc.get()));
  ScopedPj last_single(proj_trans_get_last_used_operation(merc.get()));
  ASSERT_THAT(last_single.get(), NotNull());
  EXPECT_NE(merc.get(), last_single.get());
  const PJ_COORD pt_clone =
      proj_trans(last_single.get(), PJ_FWD, proj_coord(0.1, 0.2, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(last_single.get()));
  EXPECT_DOUBLE_EQ(pt.xy.x, pt_clone.xy.x);
  EXPECT_DOUBLE_EQ(pt.xy.y, pt_clone.xy.y);
}

TEST(TransTest, TransArrayHandlesZeroValidUniformFailureAndMixedFailure) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  // GTX over [0..3] deg lon x [0..1] deg lat: cols 0-1 valid (+2m), cols 2-3
  // nodata (-88.88880f). vgridshift subtracts the interpolated offset in
  // PJ_FWD.
  constexpr float kNodata = -88.88880f;
  const std::vector<float> shifts = {
      2.0f, 2.0f, kNodata, kNodata, 2.0f, 2.0f, kNodata, kNodata,
  };
  InMemoryFileSystem fs;
  fs.AddFile("/mem/trans_mixed.gtx",
             BuildGtxGrid(0.0, 0.0, 1.0, 1.0, 2, 4, shifts));
  ASSERT_TRUE(fs.Install(ctx.get()));

  ScopedPj vgrid(
      proj_create(ctx.get(), "+proj=vgridshift +grids=/mem/trans_mixed.gtx"));
  ASSERT_THAT(vgrid.get(), NotNull());

  // 1. n == 0 succeeds immediately.
  ASSERT_EQ(0, proj_trans_array(vgrid.get(), PJ_FWD, 0, nullptr));
  EXPECT_EQ(0, proj_errno(vgrid.get()));

  // 2. All valid points return 0 and preserve/shift z.
  std::array<PJ_COORD, 2> valid_pts = {
      proj_coord(DegToRad(0.5), DegToRad(0.5), 100.0, 0.0),
      proj_coord(DegToRad(0.25), DegToRad(0.25), 50.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(vgrid.get(), PJ_FWD, valid_pts.size(),
                                valid_pts.data()));
  EXPECT_EQ(0, proj_errno(vgrid.get()));
  EXPECT_NEAR(98.0, valid_pts[0].lpz.z, 1e-6);
  EXPECT_NEAR(48.0, valid_pts[1].lpz.z, 1e-6);

  // 3. Uniform outside-grid failures across multiple points preserve the
  // precise error code (PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID).
  {
    ScopedLogSuppressor suppress(ctx.get());
    std::array<PJ_COORD, 3> uniform_fail = {
        proj_coord(DegToRad(0.5), DegToRad(0.5), 100.0, 0.0),
        proj_coord(DegToRad(10.0), DegToRad(0.5), 100.0, 0.0),
        proj_coord(DegToRad(12.0), DegToRad(0.5), 100.0, 0.0),
    };
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID,
              proj_trans_array(vgrid.get(), PJ_FWD, uniform_fail.size(),
                               uniform_fail.data()));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_errno(vgrid.get()));
    EXPECT_NEAR(98.0, uniform_fail[0].lpz.z, 1e-6);
    EXPECT_EQ(HUGE_VAL, uniform_fail[1].lpz.lam);
    EXPECT_EQ(HUGE_VAL, uniform_fail[2].lpz.lam);
    proj_errno_reset(vgrid.get());

    // 4. Distinct failure causes in the same batch (GRID_AT_NODATA and
    // OUTSIDE_GRID) collapse to generic PROJ_ERR_COORD_TRANSFM while valid
    // coordinates still succeed.
    std::array<PJ_COORD, 4> mixed_fail = {
        proj_coord(DegToRad(0.5), DegToRad(0.5), 100.0, 0.0),
        proj_coord(DegToRad(2.5), DegToRad(0.5), 100.0, 0.0),
        proj_coord(DegToRad(10.0), DegToRad(0.5), 100.0, 0.0),
        proj_coord(DegToRad(2.6), DegToRad(0.5), 100.0, 0.0),
    };
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM,
              proj_trans_array(vgrid.get(), PJ_FWD, mixed_fail.size(),
                               mixed_fail.data()));
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM, proj_errno(vgrid.get()));
    EXPECT_NEAR(98.0, mixed_fail[0].lpz.z, 1e-6);
    EXPECT_EQ(HUGE_VAL, mixed_fail[1].lpz.lam);
    EXPECT_EQ(HUGE_VAL, mixed_fail[2].lpz.lam);
    EXPECT_EQ(HUGE_VAL, mixed_fail[3].lpz.lam);
    proj_errno_reset(vgrid.get());
  }
}

TEST(TransTest,
     TransGenericHandlesNullZeroIdentInvertedStridesAndBroadcasting) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  ScopedPj helmert(proj_create(
      ctx.get(), "+proj=helmert +x=10 +y=20 +z=30 +dx=2 +t_epoch=2020.0"));
  ASSERT_THAT(helmert.get(), NotNull());

  // 1. nullptr P and empty/null array configurations return 0.
  EXPECT_EQ(size_t{0},
            proj_trans_generic(nullptr, PJ_FWD, nullptr, 0, 0, nullptr, 0, 0,
                               nullptr, 0, 0, nullptr, 0, 0));
  double dummy = 5.0;
  EXPECT_EQ(size_t{0}, proj_trans_generic(helmert.get(), PJ_FWD, nullptr, 0, 4,
                                          &dummy, sizeof(double), 0, nullptr, 0,
                                          2, nullptr, 0, 0));

  // 2. PJ_IDENT returns nmin without mutating inputs.
  std::array<double, 3> ident_x = {1.0, 2.0, 3.0};
  EXPECT_EQ(size_t{3},
            proj_trans_generic(helmert.get(), PJ_IDENT, ident_x.data(),
                               sizeof(double), ident_x.size(), nullptr, 0, 0,
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_DOUBLE_EQ(1.0, ident_x[0]);
  EXPECT_DOUBLE_EQ(3.0, ident_x[2]);

  // 3. Strided struct arrays + length-1 scalar broadcast (altered in-place).
  struct PackedPoint {
    double x;
    int tag;
    double y;
  };
  std::array<PackedPoint, 3> pts = {{
      {.x = 100.0, .tag = 1, .y = 200.0},
      {.x = 300.0, .tag = 2, .y = 400.0},
      {.x = 500.0, .tag = 3, .y = 600.0},
  }};
  double const_z = 5.0;
  double const_t = 2021.0;  // +dx=2 * dt=1 adds +2 to x (total +12).
  EXPECT_EQ(
      size_t{3},
      proj_trans_generic(helmert.get(), PJ_FWD, &pts[0].x, sizeof(PackedPoint),
                         pts.size(), &pts[0].y, sizeof(PackedPoint), pts.size(),
                         &const_z, 0, 1, &const_t, 0, 1));
  EXPECT_EQ(0, proj_errno(helmert.get()));
  EXPECT_DOUBLE_EQ(112.0, pts[0].x);
  EXPECT_DOUBLE_EQ(220.0, pts[0].y);
  EXPECT_DOUBLE_EQ(512.0, pts[2].x);
  EXPECT_DOUBLE_EQ(620.0, pts[2].y);
  // Length-1 arrays are updated in place by the final coordinate.
  EXPECT_DOUBLE_EQ(35.0, const_z);
  EXPECT_DOUBLE_EQ(2021.0, const_t);

  // 4. nmin chooses shortest count across nx, ny, nz, nt > 1, and nx == 1.
  std::array<double, 4> xs = {1.0, 2.0, 3.0, 4.0};
  std::array<double, 3> ys = {10.0, 20.0, 30.0};
  std::array<double, 2> zs = {100.0, 200.0};
  std::array<double, 2> ts = {2020.0, 2020.0};
  EXPECT_EQ(size_t{2},
            proj_trans_generic(helmert.get(), PJ_FWD, xs.data(), sizeof(double),
                               xs.size(), ys.data(), sizeof(double), ys.size(),
                               zs.data(), sizeof(double), zs.size(), ts.data(),
                               sizeof(double), ts.size()));
  EXPECT_DOUBLE_EQ(11.0, xs[0]);
  EXPECT_DOUBLE_EQ(12.0, xs[1]);
  EXPECT_DOUBLE_EQ(3.0, xs[2]);  // Beyond nmin=2, untouched.
  EXPECT_DOUBLE_EQ(30.0, ys[2]);

  // Verify ny shortest (ny=2 < nx=4) and nt shortest (nt=2 < nx=4, ny=3, nz=3).
  std::array<double, 4> xs2 = {1.0, 2.0, 3.0, 4.0};
  std::array<double, 3> ys3 = {10.0, 20.0, 30.0};
  std::array<double, 3> zs3 = {100.0, 200.0, 300.0};
  EXPECT_EQ(size_t{2}, proj_trans_generic(
                           helmert.get(), PJ_FWD, xs2.data(), sizeof(double),
                           xs2.size(), ys3.data(), sizeof(double), ys3.size(),
                           zs3.data(), sizeof(double), zs3.size(), ts.data(),
                           sizeof(double), ts.size()));
  double single_x = 10.0;
  std::array<double, 2> ys2 = {20.0, 30.0};
  EXPECT_EQ(size_t{2},
            proj_trans_generic(helmert.get(), PJ_FWD, &single_x, 0, 1,
                               ys2.data(), sizeof(double), ys2.size(), nullptr,
                               0, 0, nullptr, 0, 0));
  EXPECT_DOUBLE_EQ(20.0, single_x);

  // Cover nmin initialization branches (nz > 1) and (nt > 1) when nx, ny <= 1.
  std::array<double, 2> only_z = {100.0, 200.0};
  EXPECT_EQ(size_t{2},
            proj_trans_generic(helmert.get(), PJ_FWD, nullptr, 0, 0, nullptr, 0,
                               0, only_z.data(), sizeof(double), only_z.size(),
                               nullptr, 0, 0));
  EXPECT_DOUBLE_EQ(130.0, only_z[0]);
  EXPECT_DOUBLE_EQ(230.0, only_z[1]);

  std::array<double, 2> only_t = {2020.0, 2021.0};
  EXPECT_EQ(size_t{2},
            proj_trans_generic(helmert.get(), PJ_FWD, nullptr, 0, 0, nullptr, 0,
                               0, nullptr, 0, 0, only_t.data(), sizeof(double),
                               only_t.size()));
  EXPECT_DOUBLE_EQ(2020.0, only_t[0]);
  EXPECT_DOUBLE_EQ(2021.0, only_t[1]);

  // 5. Inverted flag flips direction in proj_trans_generic (and proj_trans
  // flips it back, yielding double-inversion).
  helmert->inverted = 1;
  std::array<double, 2> inv_x = {20.0, 30.0};
  EXPECT_EQ(size_t{2}, proj_trans_generic(helmert.get(), PJ_FWD, inv_x.data(),
                                          sizeof(double), inv_x.size(), nullptr,
                                          0, 0, nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_DOUBLE_EQ(30.0, inv_x[0]);
  EXPECT_DOUBLE_EQ(40.0, inv_x[1]);

  // 6. Null t / nt == 0 broadcasts invalid_time = HUGE_VAL -> triggers
  // PROJ_ERR_COORD_TRANSFM_MISSING_TIME on +proj=deformation.
  InMemoryFileSystem fs;
  const std::vector<float> zero_xy(18, 0.0f);
  const std::vector<float> zero_z(9, 0.0f);
  fs.AddFile("/mem/gen_vel.ct2",
             BuildCTable2Grid(DegToRad(-5.0), DegToRad(-5.0), DegToRad(5.0),
                              DegToRad(5.0), 3, 3, zero_xy));
  fs.AddFile("/mem/gen_vvel.gtx",
             BuildGtxGrid(-5.0, -5.0, 5.0, 5.0, 3, 3, zero_z));
  ASSERT_TRUE(fs.Install(ctx.get()));

  ScopedPj def(
      proj_create(ctx.get(),
                  "+proj=deformation +ellps=WGS84 +xy_grids=/mem/gen_vel.ct2 "
                  "+z_grids=/mem/gen_vvel.gtx +t_epoch=2020.0"));
  ASSERT_THAT(def.get(), NotNull());
  {
    ScopedLogSuppressor suppress(ctx.get());
    double def_x = 6378137.0;
    EXPECT_EQ(size_t{1},
              proj_trans_generic(def.get(), PJ_FWD, &def_x, 0, 1, nullptr, 0, 0,
                                 nullptr, 0, 0, nullptr, 0, 0));
    EXPECT_EQ(HUGE_VAL, def_x);
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_MISSING_TIME, proj_errno(def.get()));
    proj_errno_reset(def.get());
  }
}

TEST(TransTest, RoundtripCoversNullInvalidCountsAngularLinearAndNaN) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());

  PJ_COORD c = proj_coord(DegToRad(10.0), DegToRad(50.0), 12.0, 0.0);

  // 1. nullptr PJ returns HUGE_VAL.
  EXPECT_EQ(HUGE_VAL, proj_roundtrip(nullptr, PJ_FWD, 1, &c));

  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_THAT(merc.get(), NotNull());

  // 2. n < 1 sets PROJ_ERR_OTHER_API_MISUSE and returns HUGE_VAL.
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    EXPECT_EQ(HUGE_VAL, proj_roundtrip(merc.get(), PJ_FWD, 0, &c));
    EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(merc.get()));
    EXPECT_TRUE(capture.ContainsMessage("n should be >= 1"));
    proj_errno_reset(merc.get());

    EXPECT_EQ(HUGE_VAL, proj_roundtrip(merc.get(), PJ_FWD, -3, &c));
    EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_errno(merc.get()));
    proj_errno_reset(merc.get());
  }

  // 3. Angular input roundtrip (n == 1 and n > 1) uses proj_lpz_dist().
  PJ_COORD lpz_1 = proj_coord(DegToRad(10.0), DegToRad(50.0), 15.0, 0.0);
  const double err_fwd_1 = proj_roundtrip(merc.get(), PJ_FWD, 1, &lpz_1);
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_LT(err_fwd_1, 1e-6);

  PJ_COORD lpz_5 = proj_coord(DegToRad(-122.0), DegToRad(37.5), 0.0, 0.0);
  const double err_fwd_5 = proj_roundtrip(merc.get(), PJ_FWD, 5, &lpz_5);
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_LT(err_fwd_5, 1e-5);

  // 4. Linear input roundtrip (PJ_INV on merc + Helmert) uses proj_xyz_dist().
  const PJ_COORD projected = proj_trans(
      merc.get(), PJ_FWD, proj_coord(DegToRad(10.0), DegToRad(50.0), 0.0, 0.0));
  PJ_COORD xy_round = projected;
  const double err_inv = proj_roundtrip(merc.get(), PJ_INV, 4, &xy_round);
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_LT(err_inv, 1e-5);

  ScopedPj helmert(proj_create(ctx.get(), "+proj=helmert +x=100 +y=-50 +z=25"));
  ASSERT_THAT(helmert.get(), NotNull());
  PJ_COORD xyz = proj_coord(1000.0, 2000.0, 3000.0, 0.0);
  const double err_helmert = proj_roundtrip(helmert.get(), PJ_FWD, 3, &xyz);
  EXPECT_EQ(0, proj_errno(helmert.get()));
  EXPECT_NEAR(0.0, err_helmert, 1e-12);

  // 5. Starting with NaN outputs all-NaN and returns 0.0.
  PJ_COORD nan_coord =
      proj_coord(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0);
  EXPECT_DOUBLE_EQ(0.0, proj_roundtrip(merc.get(), PJ_FWD, 2, &nan_coord));
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(std::isnan(nan_coord.v[i])) << "comp " << i;
  }
}

}  // namespace
}  // namespace proj
