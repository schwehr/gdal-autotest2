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
// Unit tests for PJCoordOperation methods, priority op heuristics, area flags,
// instantiability caching, and axis/unit classification
// (proj/src/coord_operation.cpp). Note: in google3 default non-GCE builds,
// proj.db and select datum grids are embedded in memory.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/coord_operation.cpp

#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;

ScopedPj MakeFirstOperation(
    PJ_CONTEXT* ctx, const PJ* src_crs, const PJ* dst_crs,
    const PJ_OPERATION_FACTORY_CONTEXT* fctx = nullptr) {
  ScopedOpFactoryContext default_fctx(
      fctx == nullptr ? proj_create_operation_factory_context(ctx, nullptr)
                      : nullptr);
  const PJ_OPERATION_FACTORY_CONTEXT* effective_fctx =
      fctx != nullptr ? fctx : default_fctx.get();
  ScopedObjList ops(
      proj_create_operations(ctx, src_crs, dst_crs, effective_fctx));
  if (ops == nullptr || proj_list_get_count(ops.get()) <= 0) {
    return nullptr;
  }
  return ScopedPj(proj_list_get(ctx, ops.get(), 0));
}

TEST(CoordOperationTest, ConstructorSetsMetadataPriorityAndAreaFlags) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Priority op name cases + area name / offshore / unknown / nullptr cases.
  struct PriorityCase {
    const char* op_name;
    const char* area_name;
    bool expected_priority;
    bool expected_offshore;
    bool expected_unknown_area;
    const char* expected_area_str;
  };
  constexpr PriorityCase kCases[] = {
      {.op_name = "NAD83 to NAD83(HARN) (47)",
       .area_name = "USA - offshore Gulf",
       .expected_priority = true,
       .expected_offshore = true,
       .expected_unknown_area = false,
       .expected_area_str = "USA - offshore Gulf"},
      {.op_name = "NAD83 to NAD83(HARN) (48)",
       .area_name = "",
       .expected_priority = true,
       .expected_offshore = false,
       .expected_unknown_area = true,
       .expected_area_str = ""},
      {.op_name = "NAD83 to NAD83(HARN) (49)",
       .area_name = "unknown",
       .expected_priority = true,
       .expected_offshore = false,
       .expected_unknown_area = true,
       .expected_area_str = "unknown"},
      {.op_name = "NAD83 to NAD83(HARN) (50)",
       .area_name = nullptr,
       .expected_priority = true,
       .expected_offshore = false,
       .expected_unknown_area = true,
       .expected_area_str = ""},
      {.op_name = "GDA94 to WGS 84 (1)",
       .area_name = "Australia - onshore and offshore",
       .expected_priority = true,
       .expected_offshore = false,
       .expected_unknown_area = false,
       .expected_area_str = "Australia - onshore and offshore"},
      {.op_name = "GDA2020 to WGS 84 (2)",
       .area_name = "World - offshore marine",
       .expected_priority = true,
       .expected_offshore = true,
       .expected_unknown_area = false,
       .expected_area_str = "World - offshore marine"},
      {.op_name = "Generic Helmert transformation",
       .area_name = "World",
       .expected_priority = false,
       .expected_offshore = false,
       .expected_unknown_area = false,
       .expected_area_str = "World"},
  };

  int idx = 0;
  for (const auto& tc : kCases) {
    // Classic PROJ operation has null source_crs and target_crs.
    ScopedPj pj(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
    ASSERT_NE(nullptr, pj.get());
    PJCoordOperation op(idx, -10.0, 20.0, 30.0, 40.0, 1.0, 2.0, 3.0, 4.0,
                        pj.release(), tc.op_name, 0.5, 123.0, tc.area_name,
                        nullptr, nullptr);
    EXPECT_EQ(idx, op.idxInOriginalList);
    EXPECT_DOUBLE_EQ(-10.0, op.minxSrc);
    EXPECT_DOUBLE_EQ(20.0, op.minySrc);
    EXPECT_DOUBLE_EQ(30.0, op.maxxSrc);
    EXPECT_DOUBLE_EQ(40.0, op.maxySrc);
    EXPECT_DOUBLE_EQ(1.0, op.minxDst);
    EXPECT_DOUBLE_EQ(2.0, op.minyDst);
    EXPECT_DOUBLE_EQ(3.0, op.maxxDst);
    EXPECT_DOUBLE_EQ(4.0, op.maxyDst);
    EXPECT_DOUBLE_EQ(0.5, op.accuracy);
    EXPECT_DOUBLE_EQ(123.0, op.pseudoArea);
    EXPECT_EQ(tc.op_name, op.name);
    EXPECT_EQ(tc.expected_area_str, op.areaName);
    EXPECT_EQ(tc.expected_priority, op.isPriorityOp) << tc.op_name;
    EXPECT_EQ(tc.expected_offshore, op.isOffshore) << tc.op_name;
    EXPECT_EQ(tc.expected_unknown_area, op.isUnknownAreaName) << tc.op_name;
    EXPECT_EQ(nullptr, op.pjSrcGeocentricToLonLat);
    EXPECT_EQ(nullptr, op.pjDstGeocentricToLonLat);
    EXPECT_FALSE(op.srcIsLonLatDegree);
    EXPECT_FALSE(op.srcIsLatLonDegree);
    EXPECT_FALSE(op.dstIsLonLatDegree);
    EXPECT_FALSE(op.dstIsLatLonDegree);
    ++idx;
  }
}

TEST(CoordOperationTest, ClonesGeocentricConvertersAndSupportsMoveDestruction) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=noop"));
  ScopedPj src_conv(proj_create(ctx.get(), "+proj=cart +inv +ellps=WGS84"));
  ScopedPj dst_conv(proj_create(ctx.get(), "+proj=cart +inv +ellps=GRS80"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, src_conv.get());
  ASSERT_NE(nullptr, dst_conv.get());

  PJCoordOperation op(0, -180.0, -90.0, 180.0, 90.0, -180.0, -90.0, 180.0, 90.0,
                      pj.release(), "Geocentric helper clone test", 1.0, 100.0,
                      "World", src_conv.get(), dst_conv.get());
  ASSERT_NE(nullptr, op.pjSrcGeocentricToLonLat);
  ASSERT_NE(nullptr, op.pjDstGeocentricToLonLat);
  EXPECT_NE(src_conv.get(), op.pjSrcGeocentricToLonLat);
  EXPECT_NE(dst_conv.get(), op.pjDstGeocentricToLonLat);

  // Move construction nulls source pointers; destroying moved-from op safely
  // invokes proj_destroy(nullptr) on all three PJ* members.
  PJCoordOperation moved(std::move(op));
  EXPECT_EQ(nullptr, op.pj);
  EXPECT_EQ(nullptr, op.pjSrcGeocentricToLonLat);
  EXPECT_EQ(nullptr, op.pjDstGeocentricToLonLat);
  EXPECT_NE(nullptr, moved.pj);
  EXPECT_NE(nullptr, moved.pjSrcGeocentricToLonLat);
  EXPECT_NE(nullptr, moved.pjDstGeocentricToLonLat);
}

TEST(CoordOperationTest, IsInstantiableCachesTrueAndFalseResults) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  InMemoryFileSystem empty_fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Instantiable ISO-19111 coordinate operation (WGS84 -> WebMercator).
  ScopedPj crs_4326(proj_create(ctx.get(), "EPSG:4326"));
  ScopedPj crs_3857(proj_create(ctx.get(), "EPSG:3857"));
  ASSERT_NE(nullptr, crs_4326.get());
  ASSERT_NE(nullptr, crs_3857.get());
  ScopedPj instantiable_op =
      MakeFirstOperation(ctx.get(), crs_4326.get(), crs_3857.get());
  ASSERT_NE(nullptr, instantiable_op.get());

  PJCoordOperation op_true(0, -180.0, -85.0, 180.0, 85.0, -2e7, -2e7, 2e7, 2e7,
                           instantiable_op.release(), "WGS 84 to WebMercator",
                           0.0, 1000.0, "World", nullptr, nullptr);
  // First call evaluates proj_coordoperation_is_instantiable; second reuses
  // the cached result.
  EXPECT_TRUE(op_true.isInstantiable());
  EXPECT_TRUE(op_true.isInstantiable());

  // 2. Non-instantiable coordinate operation (missing grid under empty_fs).
  ASSERT_TRUE(empty_fs.Install(ctx.get()));
  ScopedPj crs_egm08(proj_create(ctx.get(), "EPSG:4326+3855"));
  ScopedPj crs_4979(proj_create(ctx.get(), "EPSG:4979"));
  ASSERT_NE(nullptr, crs_egm08.get());
  ASSERT_NE(nullptr, crs_4979.get());

  ScopedOpFactoryContext fctx(
      proj_create_operation_factory_context(ctx.get(), nullptr));
  ASSERT_NE(nullptr, fctx.get());
  proj_operation_factory_context_set_grid_availability_use(
      ctx.get(), fctx.get(), PROJ_GRID_AVAILABILITY_IGNORED);
  proj_operation_factory_context_set_allow_ballpark_transformations(
      ctx.get(), fctx.get(), 0);
  ScopedObjList ops(proj_create_operations(ctx.get(), crs_egm08.get(),
                                           crs_4979.get(), fctx.get()));
  ASSERT_NE(nullptr, ops.get());
  const int op_count = proj_list_get_count(ops.get());
  ASSERT_GT(op_count, 0);
  ScopedPj missing_grid_op;
  for (int i = 0; i < op_count; ++i) {
    ScopedPj candidate(proj_list_get(ctx.get(), ops.get(), i));
    if (candidate != nullptr &&
        proj_coordoperation_is_instantiable(ctx.get(), candidate.get()) == 0) {
      missing_grid_op = std::move(candidate);
      break;
    }
  }
  ASSERT_NE(nullptr, missing_grid_op.get());

  PJCoordOperation op_false(1, -180.0, -90.0, 180.0, 90.0, -180.0, -90.0, 180.0,
                            90.0, missing_grid_op.release(), "Missing grid op",
                            0.1, 1000.0, "World", nullptr, nullptr);
  // First call caches 0; second call hits the cached false branch.
  EXPECT_FALSE(op_false.isInstantiable());
  EXPECT_FALSE(op_false.isInstantiable());
}

TEST(CoordOperationTest, ClassifiesGeographic2D3DUnitsAndAxisOrders) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj crs_84(proj_create(ctx.get(), "OGC:CRS84"));
  ScopedPj crs_4326(proj_create(ctx.get(), "EPSG:4326"));
  ScopedPj crs_4979(proj_create(ctx.get(), "EPSG:4979"));
  ScopedPj crs_32632(proj_create(ctx.get(), "EPSG:32632"));
  ScopedPj crs_4978(proj_create(ctx.get(), "EPSG:4978"));
  ASSERT_NE(nullptr, crs_84.get());
  ASSERT_NE(nullptr, crs_4326.get());
  ASSERT_NE(nullptr, crs_4979.get());
  ASSERT_NE(nullptr, crs_32632.get());
  ASSERT_NE(nullptr, crs_4978.get());

  ScopedPj wgs84_datum(proj_crs_get_datum(ctx.get(), crs_4326.get()));
  if (!wgs84_datum) {
    wgs84_datum.reset(proj_crs_get_datum_ensemble(ctx.get(), crs_4326.get()));
  }
  ASSERT_NE(nullptr, wgs84_datum.get());

  // 1. 2D Lon/Lat degrees (OGC:CRS84) -> 2D Lat/Lon degrees (EPSG:4326).
  {
    ScopedPj op_pj =
        MakeFirstOperation(ctx.get(), crs_84.get(), crs_4326.get());
    ASSERT_NE(nullptr, op_pj.get());
    PJCoordOperation op(0, -180, -90, 180, 90, -90, -180, 90, 180,
                        op_pj.release(), "CRS84 to 4326", 0.0, 1.0, "World",
                        nullptr, nullptr);
    EXPECT_TRUE(op.srcIsLonLatDegree);
    EXPECT_FALSE(op.srcIsLatLonDegree);
    EXPECT_FALSE(op.dstIsLonLatDegree);
    EXPECT_TRUE(op.dstIsLatLonDegree);
  }

  // 2. 3D Lat/Lon/Height degrees (EPSG:4979) -> 3D Lon/Lat/Height degrees.
  {
    ScopedPj cs_3d_lonlat(proj_create_ellipsoidal_3D_cs(
        ctx.get(), PJ_ELLPS3D_LONGITUDE_LATITUDE_HEIGHT, "degree", kDegToRad,
        "metre", 1.0));
    ASSERT_NE(nullptr, cs_3d_lonlat.get());
    ScopedPj crs_3d_lonlat(proj_create_geographic_crs_from_datum(
        ctx.get(), "WGS 84 3D LonLat", wgs84_datum.get(), cs_3d_lonlat.get()));
    ASSERT_NE(nullptr, crs_3d_lonlat.get());

    ScopedPj op_pj =
        MakeFirstOperation(ctx.get(), crs_4979.get(), crs_3d_lonlat.get());
    ASSERT_NE(nullptr, op_pj.get());
    PJCoordOperation op(0, -90, -180, 90, 180, -180, -90, 180, 90,
                        op_pj.release(), "3D axis swap", 0.0, 1.0, "World",
                        nullptr, nullptr);
    EXPECT_FALSE(op.srcIsLonLatDegree);
    EXPECT_TRUE(op.srcIsLatLonDegree);
    EXPECT_TRUE(op.dstIsLonLatDegree);
    EXPECT_FALSE(op.dstIsLatLonDegree);
  }

  // 3. Geographic 2D CRSs in radians (East on axis 0 for src, East on axis 1
  // for dst): axis direction is "East", but conv_factor != pi/180 -> false.
  {
    ScopedPj cs_lonlat_rad(proj_create_ellipsoidal_2D_cs(
        ctx.get(), PJ_ELLPS2D_LONGITUDE_LATITUDE, "radian", 1.0));
    ScopedPj cs_latlon_rad(proj_create_ellipsoidal_2D_cs(
        ctx.get(), PJ_ELLPS2D_LATITUDE_LONGITUDE, "radian", 1.0));
    ASSERT_NE(nullptr, cs_lonlat_rad.get());
    ASSERT_NE(nullptr, cs_latlon_rad.get());
    ScopedPj crs_lonlat_rad(proj_create_geographic_crs_from_datum(
        ctx.get(), "WGS 84 LonLat rad", wgs84_datum.get(),
        cs_lonlat_rad.get()));
    ScopedPj crs_latlon_rad(proj_create_geographic_crs_from_datum(
        ctx.get(), "WGS 84 LatLon rad", wgs84_datum.get(),
        cs_latlon_rad.get()));
    ASSERT_NE(nullptr, crs_lonlat_rad.get());
    ASSERT_NE(nullptr, crs_latlon_rad.get());

    ScopedPj op_pj = MakeFirstOperation(ctx.get(), crs_lonlat_rad.get(),
                                        crs_latlon_rad.get());
    ASSERT_NE(nullptr, op_pj.get());
    PJCoordOperation op(0, -kPi, -kPi / 2, kPi, kPi / 2, -kPi / 2, -kPi,
                        kPi / 2, kPi, op_pj.release(), "Radian Geog 2D", 0.0,
                        1.0, "World", nullptr, nullptr);
    EXPECT_FALSE(op.srcIsLonLatDegree);
    EXPECT_FALSE(op.srcIsLatLonDegree);
    EXPECT_FALSE(op.dstIsLonLatDegree);
    EXPECT_FALSE(op.dstIsLatLonDegree);
  }

  // 4. Geographic 2D CRS with West / South axes (neither axis 0 nor axis 1 is
  // "East") -> target EPSG:4326.
  {
    std::string lon_name = "Geodetic longitude";
    std::string lon_abbr = "lon";
    std::string west_dir = "west";
    std::string lat_name = "Geodetic latitude";
    std::string lat_abbr = "lat";
    std::string south_dir = "south";
    std::string unit_deg = "degree";
    const PJ_AXIS_DESCRIPTION axes[2] = {
        {lon_name.data(), lon_abbr.data(), west_dir.data(), unit_deg.data(),
         kDegToRad, PJ_UT_ANGULAR},
        {lat_name.data(), lat_abbr.data(), south_dir.data(), unit_deg.data(),
         kDegToRad, PJ_UT_ANGULAR},
    };
    ScopedPj cs_west_south(
        proj_create_cs(ctx.get(), PJ_CS_TYPE_ELLIPSOIDAL, 2, axes));
    ASSERT_NE(nullptr, cs_west_south.get());
    ScopedPj crs_west_south(proj_create_geographic_crs_from_datum(
        ctx.get(), "WGS 84 West-South", wgs84_datum.get(),
        cs_west_south.get()));
    ASSERT_NE(nullptr, crs_west_south.get());

    ScopedPj op_pj =
        MakeFirstOperation(ctx.get(), crs_west_south.get(), crs_4326.get());
    ASSERT_NE(nullptr, op_pj.get());
    PJCoordOperation op(0, -180, -90, 180, 90, -90, -180, 90, 180,
                        op_pj.release(), "West-South to 4326", 0.0, 1.0,
                        "World", nullptr, nullptr);
    EXPECT_FALSE(op.srcIsLonLatDegree);
    EXPECT_FALSE(op.srcIsLatLonDegree);
    EXPECT_FALSE(op.dstIsLonLatDegree);
    EXPECT_TRUE(op.dstIsLatLonDegree);
  }

  // 5. Non-geographic CRS types (Projected -> Geocentric).
  {
    ScopedPj op_pj =
        MakeFirstOperation(ctx.get(), crs_32632.get(), crs_4978.get());
    ASSERT_NE(nullptr, op_pj.get());
    PJCoordOperation op(0, 0, 0, 1e6, 1e7, -6.4e6, -6.4e6, 6.4e6, 6.4e6,
                        op_pj.release(), "Projected to Geocentric", 0.0, 1.0,
                        "World", nullptr, nullptr);
    EXPECT_FALSE(op.srcIsLonLatDegree);
    EXPECT_FALSE(op.srcIsLatLonDegree);
    EXPECT_FALSE(op.dstIsLonLatDegree);
    EXPECT_FALSE(op.dstIsLatLonDegree);
  }

  // 6. Integration via pj_create_prepared_operations with geocentric CRS.
  {
    ScopedPj crs_4936(proj_create(ctx.get(), "EPSG:4936"));
    ASSERT_NE(nullptr, crs_4936.get());
    ScopedOpFactoryContext fctx(
        proj_create_operation_factory_context(ctx.get(), nullptr));
    ASSERT_NE(nullptr, fctx.get());
    ScopedObjList geoc_ops(proj_create_operations(ctx.get(), crs_4978.get(),
                                                  crs_4936.get(), fctx.get()));
    ASSERT_NE(nullptr, geoc_ops.get());
    ASSERT_GT(proj_list_get_count(geoc_ops.get()), 0);
    std::vector<PJCoordOperation> prepared = pj_create_prepared_operations(
        ctx.get(), crs_4978.get(), crs_4936.get(), geoc_ops.get());
    ASSERT_FALSE(prepared.empty());
    EXPECT_TRUE(prepared[0].isInstantiable());
    EXPECT_NE(nullptr, prepared[0].pjSrcGeocentricToLonLat);
    EXPECT_NE(nullptr, prepared[0].pjDstGeocentricToLonLat);
  }
}

}  // namespace
}  // namespace proj
