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
// Unit tests for proj_create_crs_to_crs(), proj_create_crs_to_crs_from_pj(),
// and prepared operation selection (proj/src/crs_to_crs.cpp). Note: in
// google3 default non-GCE builds, proj.db and select datum grids are embedded
// in memory (ntv2_0.gsb / ca_nrc_ntv2_0.tif are skipped) and curl networking
// is disabled.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/crs_to_crs.cpp

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(CrsToCrsTest, AddTypeCrsIfNeeded) {
  EXPECT_EQ("proj=merc +type=crs", pj_add_type_crs_if_needed("proj=merc"));
  EXPECT_EQ("+proj=merc +type=crs", pj_add_type_crs_if_needed("+proj=merc"));
  EXPECT_EQ("+init=epsg:4326 +type=crs",
            pj_add_type_crs_if_needed("+init=epsg:4326"));
  EXPECT_EQ("+title=custom +proj=merc +type=crs",
            pj_add_type_crs_if_needed("+title=custom +proj=merc"));
  EXPECT_EQ("+proj=merc +type=crs",
            pj_add_type_crs_if_needed("+proj=merc +type=crs"));
  EXPECT_EQ("EPSG:4326", pj_add_type_crs_if_needed("EPSG:4326"));
  EXPECT_EQ("", pj_add_type_crs_if_needed(""));
}

TEST(CrsToCrsTest, CreateCrsToCrsBasicValidationAndTransforms) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // Invalid source or target CRS strings return nullptr cleanly.
  EXPECT_EQ(nullptr, proj_create_crs_to_crs(ctx.get(), "INVALID_CRS_SOURCE",
                                            "EPSG:4326", nullptr));
  EXPECT_EQ(nullptr, proj_create_crs_to_crs(ctx.get(), "EPSG:4326",
                                            "INVALID_CRS_TARGET", nullptr));

  // PROJ string without +type=crs gets +type=crs appended automatically.
  // UTM zone 32N takes (easting, northing) in meters; EPSG:4326 uses
  // (latitude, longitude) in degrees.
  ScopedPj utm_to_geog(proj_create_crs_to_crs(
      ctx.get(), "+proj=utm +zone=32 +datum=WGS84", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, utm_to_geog.get());
  EXPECT_EQ(0, proj_errno(utm_to_geog.get()));

  const PJ_COORD utm_pt = proj_coord(500000.0, 5000000.0, 250.0, 2020.0);
  const PJ_COORD geog_pt = proj_trans(utm_to_geog.get(), PJ_FWD, utm_pt);
  EXPECT_EQ(0, proj_errno(utm_to_geog.get()));
  EXPECT_GT(geog_pt.xyzt.x, 44.0);
  EXPECT_LT(geog_pt.xyzt.x, 46.0);
  EXPECT_NEAR(9.0, geog_pt.xyzt.y, 1e-6);
  EXPECT_NEAR(250.0, geog_pt.xyzt.z, 1e-6);
  EXPECT_DOUBLE_EQ(2020.0, geog_pt.xyzt.t);

  const PJ_COORD utm_rt = proj_trans(utm_to_geog.get(), PJ_INV, geog_pt);
  EXPECT_EQ(0, proj_errno(utm_to_geog.get()));
  EXPECT_NEAR(utm_pt.xyzt.x, utm_rt.xyzt.x, 1e-4);
  EXPECT_NEAR(utm_pt.xyzt.y, utm_rt.xyzt.y, 1e-4);

  PJ_COORD batch[2] = {
      proj_coord(500000.0, 5000000.0, 0.0, 0.0),
      proj_coord(600000.0, 5200000.0, 0.0, 0.0),
  };
  ASSERT_EQ(0, proj_trans_array(utm_to_geog.get(), PJ_FWD, 2, batch));
  EXPECT_NEAR(9.0, batch[0].xyzt.y, 1e-6);
  EXPECT_GT(batch[1].xyzt.y, 9.0);

  ScopedPj last_used(proj_trans_get_last_used_operation(utm_to_geog.get()));
  EXPECT_NE(nullptr, last_used.get());

  // Non-empty PJ_AREA with both bbox and area name.
  ScopedArea area(proj_area_create());
  ASSERT_NE(nullptr, area.get());
  proj_area_set_bbox(area.get(), -125.0, 24.0, -65.0, 50.0);
  proj_area_set_name(area.get(), "CONUS area");
  ScopedPj nad83_to_wgs84(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4269", "EPSG:4326", area.get()));
  ASSERT_NE(nullptr, nad83_to_wgs84.get());
  proj_errno_reset(nad83_to_wgs84.get());
  EXPECT_EQ(0, proj_errno(nad83_to_wgs84.get()));

  // Coordinate domain error sets proj_errno and returns HUGE_VAL;
  // proj_errno_reset clears it for subsequent valid points.
  ScopedPj geog_to_webmerc(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, geog_to_webmerc.get());
  const PJ_COORD invalid_pt = proj_coord(95.0, 0.0, 0.0, 0.0);
  const PJ_COORD err_out =
      proj_trans(geog_to_webmerc.get(), PJ_FWD, invalid_pt);
  EXPECT_EQ(HUGE_VAL, err_out.xy.x);
  EXPECT_EQ(HUGE_VAL, err_out.xy.y);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
            proj_errno(geog_to_webmerc.get()));
  proj_errno_reset(geog_to_webmerc.get());
  EXPECT_EQ(0, proj_errno(geog_to_webmerc.get()));

  const PJ_COORD valid_out = proj_trans(geog_to_webmerc.get(), PJ_FWD,
                                        proj_coord(45.0, 9.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(geog_to_webmerc.get()));
  EXPECT_TRUE(std::isfinite(valid_out.xy.x));
  EXPECT_TRUE(std::isfinite(valid_out.xy.y));
}

TEST(CrsToCrsTest, PreparedOperationsWorldAntimeridianMetadataAndGeocentric) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj crs_utm(
      proj_create(ctx.get(), "+proj=utm +zone=32 +datum=WGS84 +type=crs"));
  ScopedPj crs_4326(proj_create(ctx.get(), "EPSG:4326"));
  ScopedPj crs_4269(proj_create(ctx.get(), "EPSG:4269"));
  ScopedPj crs_4267(proj_create(ctx.get(), "EPSG:4267"));
  ScopedPj crs_4978(proj_create(ctx.get(), "EPSG:4978"));  // WGS 84 geocentric
  ScopedPj crs_4936(proj_create(ctx.get(), "EPSG:4936"));  // ETRS89 geocentric
  ScopedPj crs_5800(proj_create(ctx.get(), "EPSG:5800"));  // Engineering CRS
  ASSERT_NE(nullptr, crs_utm.get());
  ASSERT_NE(nullptr, crs_4326.get());
  ASSERT_NE(nullptr, crs_4269.get());
  ASSERT_NE(nullptr, crs_4267.get());
  ASSERT_NE(nullptr, crs_4978.get());
  ASSERT_NE(nullptr, crs_4936.get());
  ASSERT_NE(nullptr, crs_5800.get());

  // 1. Operations without area_of_use trigger world bbox (-180,-90,180,90)
  // shortcut in reproject_bbox().
  {
    ScopedOpFactoryContext fctx(
        proj_create_operation_factory_context(ctx.get(), nullptr));
    ASSERT_NE(nullptr, fctx.get());
    ScopedObjList ops_list(proj_create_operations(ctx.get(), crs_utm.get(),
                                                  crs_4326.get(), fctx.get()));
    ASSERT_NE(nullptr, ops_list.get());
    ASSERT_GT(proj_list_get_count(ops_list.get()), 0);

    std::vector<PJCoordOperation> prepared = pj_create_prepared_operations(
        ctx.get(), crs_utm.get(), crs_4326.get(), ops_list.get());
    ASSERT_FALSE(prepared.empty());
    EXPECT_DOUBLE_EQ(-std::numeric_limits<double>::max(), prepared[0].minxSrc);
    EXPECT_DOUBLE_EQ(std::numeric_limits<double>::max(), prepared[0].maxxSrc);

    // Non-geodetic source or target CRS causes create_operation_to_geog_crs to
    // return nullptr and pj_create_prepared_operations to return empty.
    EXPECT_TRUE(pj_create_prepared_operations(ctx.get(), crs_5800.get(),
                                              crs_4326.get(), ops_list.get())
                    .empty());
    EXPECT_TRUE(pj_create_prepared_operations(ctx.get(), crs_4326.get(),
                                              crs_5800.get(), ops_list.get())
                    .empty());
  }

  // 2. CoordinateMetadata as source and target CRS unwraps underlying CRS in
  // create_operation_to_geog_crs().
  {
    ScopedPj crs_9000(proj_create(ctx.get(), "EPSG:9000"));  // ITRF2014 dynamic
    ASSERT_NE(nullptr, crs_9000.get());
    ScopedPj meta_9000(
        proj_coordinate_metadata_create(ctx.get(), crs_9000.get(), 2020.0));
    ASSERT_NE(nullptr, meta_9000.get());
    EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(meta_9000.get()));

    ScopedPj meta_to_4326(proj_create_crs_to_crs_from_pj(
        ctx.get(), meta_9000.get(), crs_4326.get(), nullptr, nullptr));
    ASSERT_NE(nullptr, meta_to_4326.get());
    const PJ_COORD res = proj_trans(meta_to_4326.get(), PJ_FWD,
                                    proj_coord(45.0, -75.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(meta_to_4326.get()));
    EXPECT_NEAR(45.0, res.xy.x, 1e-4);
    EXPECT_NEAR(-75.0, res.xy.y, 1e-4);

    ScopedPj geog_to_meta(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_4326.get(), meta_9000.get(), nullptr, nullptr));
    ASSERT_NE(nullptr, geog_to_meta.get());
  }

  // 3. Geocentric CRSs exercise create_operation_geocentric_crs_to_geog_crs()
  // for source, target, and geocentric-to-geocentric pairs.
  {
    ScopedPj geoc_to_nad83(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_4978.get(), crs_4269.get(), nullptr, nullptr));
    ASSERT_NE(nullptr, geoc_to_nad83.get());
    // Geographic center of contiguous US (~40N, 100W) in ECEF meters.
    // EPSG:4269 uses (latitude_deg, longitude_deg) axis order (.xyzt.x, .y).
    const PJ_COORD ecef = proj_coord(-849500.0, -4817500.0, 4078000.0, 0.0);
    const PJ_COORD nad83 = proj_trans(geoc_to_nad83.get(), PJ_FWD, ecef);
    EXPECT_EQ(0, proj_errno(geoc_to_nad83.get()));
    EXPECT_NEAR(40.0052, nad83.xyzt.x, 1e-3);
    EXPECT_NEAR(-100.0005, nad83.xyzt.y, 1e-3);

    ScopedPj nad83_to_geoc(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_4269.get(), crs_4978.get(), nullptr, nullptr));
    ASSERT_NE(nullptr, nad83_to_geoc.get());
    const PJ_COORD to_ecef = proj_trans(nad83_to_geoc.get(), PJ_FWD,
                                        proj_coord(40.0, -100.0, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(nad83_to_geoc.get()));
    EXPECT_NEAR(-849609.76, to_ecef.xyz.x, 2.0);
    EXPECT_NEAR(-4818376.38, to_ecef.xyz.y, 2.0);
    EXPECT_NEAR(4077985.57, to_ecef.xyz.z, 2.0);
    const PJ_COORD back_nad83 =
        proj_trans(nad83_to_geoc.get(), PJ_INV, to_ecef);
    EXPECT_EQ(0, proj_errno(nad83_to_geoc.get()));
    EXPECT_NEAR(40.0, back_nad83.xyzt.x, 1e-6);
    EXPECT_NEAR(-100.0, back_nad83.xyzt.y, 1e-6);

    ScopedOpFactoryContext fctx(
        proj_create_operation_factory_context(ctx.get(), nullptr));
    ASSERT_NE(nullptr, fctx.get());
    ScopedObjList geoc_ops(proj_create_operations(ctx.get(), crs_4978.get(),
                                                  crs_4936.get(), fctx.get()));
    ASSERT_NE(nullptr, geoc_ops.get());
    ASSERT_GT(proj_list_get_count(geoc_ops.get()), 0);
    std::vector<PJCoordOperation> prepared_geoc = pj_create_prepared_operations(
        ctx.get(), crs_4978.get(), crs_4936.get(), geoc_ops.get());
    ASSERT_FALSE(prepared_geoc.empty());
    EXPECT_NE(nullptr, prepared_geoc[0].pjSrcGeocentricToLonLat);
    EXPECT_NE(nullptr, prepared_geoc[0].pjDstGeocentricToLonLat);
  }

  // 4. Antimeridian split (west_lon > east_lon) + partial & total
  // reproject_bbox() failures using Orthographic projected CRSs on NAD83.
  {
    ScopedPj cart_cs(proj_create_cartesian_2D_cs(
        ctx.get(), PJ_CART2D_EASTING_NORTHING, "metre", 1.0));
    ASSERT_NE(nullptr, cart_cs.get());

    // Equatorial Orthographic centered at (0, 0): North America bboxes near
    // 90W partially fail at the horizon while Aleutians/Hawaii fail completely.
    ScopedPj conv_equat(proj_create_conversion_orthographic(
        ctx.get(), 0.0, 0.0, 0.0, 0.0, "degree", proj_torad(1.0), "metre",
        1.0));
    ASSERT_NE(nullptr, conv_equat.get());
    ScopedPj ortho_equat(proj_create_projected_crs(
        ctx.get(), "NAD83 / Ortho Equat", crs_4269.get(), conv_equat.get(),
        cart_cs.get()));
    ASSERT_NE(nullptr, ortho_equat.get());

    ScopedPj ortho_to_4326(proj_create_crs_to_crs_from_pj(
        ctx.get(), ortho_equat.get(), crs_4326.get(), nullptr, nullptr));
    EXPECT_NE(nullptr, ortho_to_4326.get());

    // South Pole Orthographic centered at (-90, 0) with ALLOW_BALLPARK=NO:
    // every NAD83->WGS84 bbox is strictly northern hemisphere, so
    // reproject_bbox yields HUGE_VAL for all perimeter points and
    // preparedOpList is empty -> returns nullptr.
    ScopedPj conv_south(proj_create_conversion_orthographic(
        ctx.get(), -90.0, 0.0, 0.0, 0.0, "degree", proj_torad(1.0), "metre",
        1.0));
    ASSERT_NE(nullptr, conv_south.get());
    ScopedPj ortho_south(proj_create_projected_crs(
        ctx.get(), "NAD83 / Ortho South Pole", crs_4269.get(), conv_south.get(),
        cart_cs.get()));
    ASSERT_NE(nullptr, ortho_south.get());

    const char* const no_ballpark[] = {"ALLOW_BALLPARK=NO", nullptr};
    ScopedPj ortho_south_op(proj_create_crs_to_crs_from_pj(
        ctx.get(), ortho_south.get(), crs_4326.get(), nullptr, no_ballpark));
    EXPECT_EQ(nullptr, ortho_south_op.get());
  }

  // Geographic 3D (EPSG:4979) and Projected (EPSG:32632) exercises
  // create_operation_to_geog_crs() for geographic 3D and projected CRS types.
  {
    ScopedPj crs_4979(proj_create(ctx.get(), "EPSG:4979"));
    ScopedPj crs_32632(proj_create(ctx.get(), "EPSG:32632"));
    ASSERT_NE(nullptr, crs_4979.get());
    ASSERT_NE(nullptr, crs_32632.get());
    ScopedPj op(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_32632.get(), crs_4979.get(), nullptr, nullptr));
    EXPECT_NE(nullptr, op.get());
  }
}

TEST(CrsToCrsTest, OptionsAreaOfInterestAndZeroOpBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj crs_4326(proj_create(ctx.get(), "EPSG:4326"));
  ScopedPj crs_3857(proj_create(ctx.get(), "EPSG:3857"));
  ScopedPj crs_4269(proj_create(ctx.get(), "EPSG:4269"));
  ScopedPj crs_5800(proj_create(ctx.get(), "EPSG:5800"));
  ScopedPj crs_bessel(
      proj_create(ctx.get(), "+proj=merc +ellps=bessel +type=crs"));
  ASSERT_NE(nullptr, crs_4326.get());
  ASSERT_NE(nullptr, crs_3857.get());
  ASSERT_NE(nullptr, crs_4269.get());
  ASSERT_NE(nullptr, crs_5800.get());
  ASSERT_NE(nullptr, crs_bessel.get());

  // Option validation error paths log PJ_LOG_ERROR and return nullptr.
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    const char* const bad_ballpark[] = {"ALLOW_BALLPARK=maybe", nullptr};
    EXPECT_EQ(nullptr, proj_create_crs_to_crs_from_pj(ctx.get(), crs_4326.get(),
                                                      crs_3857.get(), nullptr,
                                                      bad_ballpark));
    EXPECT_TRUE(capture.ContainsMessage("ALLOW_BALLPARK"));

    capture.Clear();
    const char* const bad_only_best[] = {"ONLY_BEST=maybe", nullptr};
    EXPECT_EQ(nullptr, proj_create_crs_to_crs_from_pj(ctx.get(), crs_4326.get(),
                                                      crs_3857.get(), nullptr,
                                                      bad_only_best));
    EXPECT_TRUE(capture.ContainsMessage("ONLY_BEST"));

    capture.Clear();
    const char* const unknown_opt[] = {"NON_EXISTENT_OPT=1", nullptr};
    EXPECT_EQ(nullptr, proj_create_crs_to_crs_from_pj(ctx.get(), crs_4326.get(),
                                                      crs_3857.get(), nullptr,
                                                      unknown_opt));
    EXPECT_TRUE(capture.ContainsMessage("Unknown option"));
  }

  ScopedLogSuppressor suppress(ctx.get());

  // Valid single-op options (case-insensitive keys and values).
  {
    const char* const opts[] = {"authority=EPSG",     "ACCURACY=10.0",
                                "allow_ballpark=YES", "only_best=YES",
                                "FORCE_OVER=yes",     nullptr};
    ScopedPj pj(proj_create_crs_to_crs_from_pj(ctx.get(), crs_4326.get(),
                                               crs_3857.get(), nullptr, opts));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_NE(0, pj->over);
    EXPECT_TRUE(pj->errorIfBestTransformationNotAvailable);
  }
  {
    const char* const opts[] = {"ALLOW_BALLPARK=NO", "ONLY_BEST=NO",
                                "FORCE_OVER=NO", nullptr};
    ScopedPj pj(proj_create_crs_to_crs_from_pj(ctx.get(), crs_4326.get(),
                                               crs_3857.get(), nullptr, opts));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_EQ(0, pj->over);
    EXPECT_FALSE(pj->errorIfBestTransformationNotAvailable);
  }

  // Multi-op with FORCE_OVER=yes propagates to P->over.
  {
    const char* const opts[] = {"FORCE_OVER=yes", nullptr};
    ScopedPj pj(proj_create_crs_to_crs_from_pj(ctx.get(), crs_4269.get(),
                                               crs_4326.get(), nullptr, opts));
    ASSERT_NE(nullptr, pj.get());
    EXPECT_NE(0, pj->over);
  }

  // Area-of-interest variants: empty PJ_AREA, bbox-only, bbox + name.
  ScopedArea empty_area(proj_area_create());
  ASSERT_NE(nullptr, empty_area.get());
  {
    ScopedPj pj(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_4269.get(), crs_4326.get(), empty_area.get(), nullptr));
    EXPECT_NE(nullptr, pj.get());
  }

  ScopedArea bbox_area(proj_area_create());
  ASSERT_NE(nullptr, bbox_area.get());
  proj_area_set_bbox(bbox_area.get(), -80.0, 40.0, -70.0, 46.0);
  {
    ScopedPj pj(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_4269.get(), crs_4326.get(), bbox_area.get(), nullptr));
    EXPECT_NE(nullptr, pj.get());
  }
  proj_area_set_name(bbox_area.get(), "Northeast North America");
  {
    ScopedPj pj(proj_create_crs_to_crs_from_pj(
        ctx.get(), crs_4269.get(), crs_4326.get(), bbox_area.get(), nullptr));
    EXPECT_NE(nullptr, pj.get());
  }

  // op_count == 0 returns nullptr.
  EXPECT_EQ(nullptr,
            proj_create_crs_to_crs_from_pj(ctx.get(), crs_5800.get(),
                                           crs_4326.get(), nullptr, nullptr));
  const char* const no_ballpark[] = {"ALLOW_BALLPARK=NO", nullptr};
  EXPECT_EQ(nullptr, proj_create_crs_to_crs_from_pj(ctx.get(), crs_bessel.get(),
                                                    crs_4326.get(), nullptr,
                                                    no_ballpark));
}

TEST(CrsToCrsTest, MissingGridSingleOpAndMultiOpFallbackRetry) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  // Empty InMemoryFileSystem declared before ctx outlives ctx and hides
  // embedded datum/geoid grids so missing-grid retry paths execute reliably.
  InMemoryFileSystem empty_fs;
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ASSERT_TRUE(empty_fs.Install(ctx.get()));

  // 1. Single-op with missing grid (EPSG:4326+3855 -> EPSG:4979,
  // ALLOW_BALLPARK=NO): ONLY_BEST=YES fails, default warns and succeeds.
  ScopedPj crs_egm08(proj_create(ctx.get(), "EPSG:4326+3855"));
  ScopedPj crs_4979(proj_create(ctx.get(), "EPSG:4979"));
  ASSERT_NE(nullptr, crs_egm08.get());
  ASSERT_NE(nullptr, crs_4979.get());

  const char* const only_best_no_ballpark[] = {"ALLOW_BALLPARK=NO",
                                               "ONLY_BEST=YES", nullptr};
  EXPECT_EQ(nullptr, proj_create_crs_to_crs_from_pj(ctx.get(), crs_egm08.get(),
                                                    crs_4979.get(), nullptr,
                                                    only_best_no_ballpark));

  const char* const no_ballpark[] = {"ALLOW_BALLPARK=NO", nullptr};
  ScopedPj egm_fallback(proj_create_crs_to_crs_from_pj(
      ctx.get(), crs_egm08.get(), crs_4979.get(), nullptr, no_ballpark));
  ASSERT_NE(nullptr, egm_fallback.get());
  EXPECT_EQ(0, proj_errno(egm_fallback.get()));
  EXPECT_EQ(0,
            proj_coordoperation_is_instantiable(ctx.get(), egm_fallback.get()));

  // 2. Multi-op with all preferred grid operations non-instantiable
  // (NAD27 EPSG:4267 -> NAD83 EPSG:4269) triggers re-run with
  // PROJ_GRID_AVAILABILITY_DISCARD_OPERATION_IF_MISSING_GRID including
  // desired accuracy and named area of interest.
  ScopedPj crs_4267(proj_create(ctx.get(), "EPSG:4267"));
  ScopedPj crs_4269(proj_create(ctx.get(), "EPSG:4269"));
  ASSERT_NE(nullptr, crs_4267.get());
  ASSERT_NE(nullptr, crs_4269.get());

  ScopedArea na_area(proj_area_create());
  ASSERT_NE(nullptr, na_area.get());
  proj_area_set_bbox(na_area.get(), -125.0, 25.0, -65.0, 50.0);
  proj_area_set_name(na_area.get(), "North America test AOI");

  const char* const retry_opts[] = {"ACCURACY=100.0", "FORCE_OVER=YES",
                                    nullptr};
  ScopedPj nad27_to_nad83(proj_create_crs_to_crs_from_pj(
      ctx.get(), crs_4267.get(), crs_4269.get(), na_area.get(), retry_opts));
  ASSERT_NE(nullptr, nad27_to_nad83.get());
  EXPECT_STREQ("Set of coordinate operations", nad27_to_nad83->descr);
  EXPECT_GT(nad27_to_nad83->alternativeCoordinateOperations.size(), 1u);
  EXPECT_STREQ("unavailable until proj_trans is called",
               proj_pj_info(nad27_to_nad83.get()).description);

  // Transform succeeds using fallback Helmert-via-WGS84 operation.
  // Both EPSG:4267 and EPSG:4269 use (lat_deg, lon_deg) axis order.
  const PJ_COORD nad27_pt = proj_coord(40.0, -100.0, 0.0, 0.0);
  const PJ_COORD nad83_pt = proj_trans(nad27_to_nad83.get(), PJ_FWD, nad27_pt);
  EXPECT_EQ(0, proj_errno(nad27_to_nad83.get()));
  EXPECT_NEAR(40.0, nad83_pt.xyzt.x, 0.01);
  EXPECT_NEAR(-100.0, nad83_pt.xyzt.y, 0.01);

  const PJ_COORD nad27_rt = proj_trans(nad27_to_nad83.get(), PJ_INV, nad83_pt);
  EXPECT_EQ(0, proj_errno(nad27_to_nad83.get()));
  EXPECT_NEAR(40.0, nad27_rt.xyzt.x, 1e-6);
  EXPECT_NEAR(-100.0, nad27_rt.xyzt.y, 1e-6);

  ScopedPj used_op(proj_trans_get_last_used_operation(nad27_to_nad83.get()));
  ASSERT_NE(nullptr, used_op.get());
  EXPECT_EQ(1, proj_coordoperation_is_instantiable(ctx.get(), used_op.get()));
}

TEST(CrsToCrsTest, MultiOpSelectionSphericalAndLastUsedOperation) {
  absl::Cleanup cleanup = [] { proj_cleanup(); };
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // NAD27 -> NAD83 with default embedded grids selects grid transformations
  // in CONUS and Alaska (exercising antimeridian-split candidate bboxes).
  ScopedPj nad27_to_nad83(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4267", "EPSG:4269", nullptr));
  ASSERT_NE(nullptr, nad27_to_nad83.get());
  EXPECT_EQ(nullptr, proj_trans_get_last_used_operation(nad27_to_nad83.get()));
  EXPECT_STREQ("Set of coordinate operations", nad27_to_nad83->descr);
  EXPECT_GT(nad27_to_nad83->alternativeCoordinateOperations.size(), 1u);
  EXPECT_STREQ("unavailable until proj_trans is called",
               proj_pj_info(nad27_to_nad83.get()).description);

  // EPSG:4267 -> EPSG:4269 inputs and outputs use (lat_deg, lon_deg).
  const PJ_COORD conus_out = proj_trans(nad27_to_nad83.get(), PJ_FWD,
                                        proj_coord(40.0, -100.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(nad27_to_nad83.get()));
  EXPECT_NEAR(40.0, conus_out.xyzt.x, 0.01);
  EXPECT_NEAR(-100.0, conus_out.xyzt.y, 0.01);

  ScopedPj conus_op(proj_trans_get_last_used_operation(nad27_to_nad83.get()));
  ASSERT_NE(nullptr, conus_op.get());

  const PJ_COORD alaska_out = proj_trans(nad27_to_nad83.get(), PJ_FWD,
                                         proj_coord(64.0, -150.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(nad27_to_nad83.get()));
  EXPECT_NEAR(64.0, alaska_out.xyzt.x, 0.01);
  EXPECT_NEAR(-150.0, alaska_out.xyzt.y, 0.01);

  // Compare spherical vs ellipsoidal Transverse Mercator (easting_m,
  // northing_m) to EPSG:4326 (lat_deg, lon_deg): both sit on the central
  // meridian (9.0 deg lon), while latitude differs due to spherical vs
  // ellipsoidal flattening. (k=1 is used so Conversion::isUTM does not rewrite
  // to +proj=utm, which rejects spherical es == 0.)
  ScopedPj sph_tmerc(proj_create_crs_to_crs(
      ctx.get(),
      "+proj=tmerc +lat_0=0 +lon_0=9 +k=1 +x_0=500000 +y_0=0 +R=6371000 "
      "+units=m +type=crs",
      "EPSG:4326", nullptr));
  ScopedPj ell_tmerc(proj_create_crs_to_crs(
      ctx.get(),
      "+proj=tmerc +lat_0=0 +lon_0=9 +k=1 +x_0=500000 +y_0=0 +ellps=WGS84 "
      "+units=m +type=crs",
      "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, sph_tmerc.get());
  ASSERT_NE(nullptr, ell_tmerc.get());

  const PJ_COORD sph_res = proj_trans(sph_tmerc.get(), PJ_FWD,
                                      proj_coord(500000.0, 5000000.0, 0, 0));
  const PJ_COORD ell_res = proj_trans(ell_tmerc.get(), PJ_FWD,
                                      proj_coord(500000.0, 5000000.0, 0, 0));
  EXPECT_EQ(0, proj_errno(sph_tmerc.get()));
  EXPECT_EQ(0, proj_errno(ell_tmerc.get()));
  EXPECT_NEAR(9.0, sph_res.xyzt.y, 1e-6);
  EXPECT_NEAR(9.0, ell_res.xyzt.y, 1e-6);
  EXPECT_NE(sph_res.xyzt.x, ell_res.xyzt.x);
}

}  // namespace
}  // namespace proj
