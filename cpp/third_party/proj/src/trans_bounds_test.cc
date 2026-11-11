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
// Unit tests for 2D and 3D bounding-box transformations across projections,
// antimeridian crossings, polar containment, domain singularities, geocentric
// bounds, CompoundCRS handling, and pipeline string fallbacks
// (proj/src/trans_bounds.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/trans_bounds.cpp

#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

struct Bounds2D {
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
};

struct Bounds3D {
  double xmin = 0.0;
  double ymin = 0.0;
  double zmin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
  double zmax = 0.0;
};

int TransBounds(PJ_CONTEXT* ctx, PJ* P, PJ_DIRECTION dir, const Bounds2D& in,
                Bounds2D& out, int densify_pts) {
  const Bounds2D input = in;
  out = {};
  return proj_trans_bounds(ctx, P, dir, input.xmin, input.ymin, input.xmax,
                           input.ymax, &out.xmin, &out.ymin, &out.xmax,
                           &out.ymax, densify_pts);
}

int TransBounds3D(PJ_CONTEXT* ctx, PJ* P, PJ_DIRECTION dir, const Bounds3D& in,
                  Bounds3D& out, int densify_pts) {
  const Bounds3D input = in;
  out = {};
  return proj_trans_bounds_3D(ctx, P, dir, input.xmin, input.ymin, input.zmin,
                              input.xmax, input.ymax, input.zmax, &out.xmin,
                              &out.ymin, &out.zmin, &out.xmax, &out.ymax,
                              &out.zmax, densify_pts);
}

void ExpectAllHugeVal(const Bounds2D& b) {
  EXPECT_EQ(HUGE_VAL, b.xmin);
  EXPECT_EQ(HUGE_VAL, b.ymin);
  EXPECT_EQ(HUGE_VAL, b.xmax);
  EXPECT_EQ(HUGE_VAL, b.ymax);
}

void ExpectAllHugeVal3D(const Bounds3D& b) {
  EXPECT_EQ(HUGE_VAL, b.xmin);
  EXPECT_EQ(HUGE_VAL, b.ymin);
  EXPECT_EQ(HUGE_VAL, b.zmin);
  EXPECT_EQ(HUGE_VAL, b.xmax);
  EXPECT_EQ(HUGE_VAL, b.ymax);
  EXPECT_EQ(HUGE_VAL, b.zmax);
}

TEST(TransBoundsTest, InvalidArguments2DReturnZeroAndResetOutputsToHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  absl::Cleanup reset_default = [] { proj_errno_reset(nullptr); };
  ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);

  Bounds2D out;
  const Bounds2D valid_lat_lon = {
      .xmin = 40.0, .ymin = 10.0, .xmax = 42.0, .ymax = 12.0};

  // 1. Null transformation object sets errno on default context.
  EXPECT_EQ(0, TransBounds(ctx.get(), nullptr, PJ_FWD, valid_lat_lon, out, 21));
  ExpectAllHugeVal(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_context_errno(nullptr));
  proj_errno_reset(nullptr);

  // 2. Out-of-range densify_pts (< 0 or > 10000).
  ScopedPj fwd(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, fwd.get());
  for (int bad_pts : {-1, 10001}) {
    capture.Clear();
    EXPECT_EQ(0, TransBounds(ctx.get(), fwd.get(), PJ_FWD, valid_lat_lon, out,
                             bad_pts));
    ExpectAllHugeVal(out);
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(fwd.get()));
    EXPECT_TRUE(capture.ContainsMessage("densify_pts must be between 0-10000"));
    proj_errno_reset(fwd.get());
  }

  // 3. Non-operation PJ (CRS with pj_info.id == nullptr).
  ScopedPj crs(proj_create(ctx.get(), "EPSG:4326"));
  ASSERT_NE(nullptr, crs.get());
  capture.Clear();
  EXPECT_EQ(0,
            TransBounds(ctx.get(), crs.get(), PJ_FWD, valid_lat_lon, out, 5));
  ExpectAllHugeVal(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(crs.get()));
  EXPECT_TRUE(capture.ContainsMessage("NULL transformation not allowed"));
  proj_errno_reset(crs.get());

  // 4. Geographic output requires densify_pts >= 2.
  ScopedPj inv(
      proj_create_crs_to_crs(ctx.get(), "EPSG:3857", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, inv.get());
  for (int few_pts : {0, 1}) {
    capture.Clear();
    EXPECT_EQ(0, TransBounds(ctx.get(), inv.get(), PJ_FWD,
                             {0.0, 0.0, 1000.0, 1000.0}, out, few_pts));
    ExpectAllHugeVal(out);
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(inv.get()));
    EXPECT_TRUE(capture.ContainsMessage("at least 2"));
    proj_errno_reset(inv.get());
  }

  // 5. Inverted latitude bounds fail for both (lat, lon) and (lon, lat) inputs.
  capture.Clear();
  EXPECT_EQ(0, TransBounds(ctx.get(), fwd.get(), PJ_FWD,
                           {45.0, 10.0, 40.0, 12.0}, out, 5));
  ExpectAllHugeVal(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(fwd.get()));
  EXPECT_TRUE(capture.ContainsMessage("latitude max < latitude min"));
  proj_errno_reset(fwd.get());

  ScopedPj crs84_to_3857(
      proj_create_crs_to_crs(ctx.get(), "OGC:CRS84", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, crs84_to_3857.get());
  capture.Clear();
  EXPECT_EQ(0, TransBounds(ctx.get(), crs84_to_3857.get(), PJ_FWD,
                           {10.0, 45.0, 12.0, 40.0}, out, 5));
  ExpectAllHugeVal(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_errno(crs84_to_3857.get()));
  EXPECT_TRUE(capture.ContainsMessage("latitude max < latitude min"));
  proj_errno_reset(crs84_to_3857.get());

  // 6. Unrecognized target/source CRS axis order in standalone unitconvert.
  ScopedPj unit_op(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=rad +xy_out=deg"));
  ASSERT_NE(nullptr, unit_op.get());
  EXPECT_EQ(1, proj_degree_output(unit_op.get(), PJ_FWD));
  EXPECT_EQ(1, proj_degree_input(unit_op.get(), PJ_INV));

  capture.Clear();
  EXPECT_EQ(0, TransBounds(ctx.get(), unit_op.get(), PJ_FWD,
                           {0.0, 0.0, 0.1, 0.1}, out, 4));
  ExpectAllHugeVal(out);
  EXPECT_TRUE(capture.ContainsMessage("Unable to retrieve target CRS"));

  capture.Clear();
  EXPECT_EQ(0, TransBounds(ctx.get(), unit_op.get(), PJ_INV,
                           {0.0, 0.0, 10.0, 10.0}, out, 4));
  ExpectAllHugeVal(out);
  EXPECT_TRUE(capture.ContainsMessage("Unable to retrieve target CRS"));
}

TEST(TransBoundsTest, InvalidArguments3DReturnZeroAndResetOutputsToHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  absl::Cleanup reset_default = [] { proj_errno_reset(nullptr); };
  ScopedLogCapture capture(ctx.get(), PJ_LOG_DEBUG);

  Bounds3D out;
  const Bounds3D box_geod = {.xmin = -5.0,
                             .ymin = -10.0,
                             .zmin = 0.0,
                             .xmax = 5.0,
                             .ymax = 10.0,
                             .zmax = 100.0};

  // 1. Null P object.
  EXPECT_EQ(0, TransBounds3D(ctx.get(), nullptr, PJ_FWD, box_geod, out, 5));
  ExpectAllHugeVal3D(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_context_errno(nullptr));
  proj_errno_reset(nullptr);

  // 2. Out-of-range densify_pts.
  ScopedPj geod_to_ecef(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4979", "EPSG:4978", nullptr));
  ASSERT_NE(nullptr, geod_to_ecef.get());
  for (int bad_pts : {-1, 10001}) {
    capture.Clear();
    EXPECT_EQ(0, TransBounds3D(ctx.get(), geod_to_ecef.get(), PJ_FWD, box_geod,
                               out, bad_pts));
    ExpectAllHugeVal3D(out);
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_errno(geod_to_ecef.get()));
    proj_errno_reset(geod_to_ecef.get());
  }

  // 3. CRS instead of coordinate operation (pj_info.id == nullptr).
  ScopedPj crs3d(proj_create(ctx.get(), "EPSG:4979"));
  ASSERT_NE(nullptr, crs3d.get());
  EXPECT_EQ(0, TransBounds3D(ctx.get(), crs3d.get(), PJ_FWD, box_geod, out, 5));
  ExpectAllHugeVal3D(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(crs3d.get()));
  proj_errno_reset(crs3d.get());

  // 4. Geographic output requires densify_pts >= 2.
  ScopedPj ecef_to_geod(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4978", "EPSG:4979", nullptr));
  ASSERT_NE(nullptr, ecef_to_geod.get());
  for (int few_pts : {0, 1}) {
    EXPECT_EQ(0, TransBounds3D(ctx.get(), ecef_to_geod.get(), PJ_FWD,
                               {6.2e6, -1.0e5, -1.0e5, 6.4e6, 1.0e5, 1.0e5},
                               out, few_pts));
    ExpectAllHugeVal3D(out);
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_errno(ecef_to_geod.get()));
    proj_errno_reset(ecef_to_geod.get());
  }

  // 5. Inverted latitude bounds for (lat, lon, h) and normalized (lon, lat, h).
  EXPECT_EQ(0, TransBounds3D(ctx.get(), geod_to_ecef.get(), PJ_FWD,
                             {10.0, -5.0, 0.0, 5.0, 5.0, 100.0}, out, 5));
  ExpectAllHugeVal3D(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_errno(geod_to_ecef.get()));
  proj_errno_reset(geod_to_ecef.get());

  ScopedPj geod_to_ecef_norm(
      proj_normalize_for_visualization(ctx.get(), geod_to_ecef.get()));
  ASSERT_NE(nullptr, geod_to_ecef_norm.get());
  EXPECT_EQ(0, TransBounds3D(ctx.get(), geod_to_ecef_norm.get(), PJ_FWD,
                             {-5.0, 10.0, 0.0, 5.0, 5.0, 100.0}, out, 5));
  ExpectAllHugeVal3D(out);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_errno(geod_to_ecef_norm.get()));
  proj_errno_reset(geod_to_ecef_norm.get());

  // 6. Unable to retrieve target CRS in 3D for both degree_output and input.
  ScopedPj unit_op(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=rad +xy_out=deg"));
  ASSERT_NE(nullptr, unit_op.get());
  capture.Clear();
  EXPECT_EQ(0, TransBounds3D(ctx.get(), unit_op.get(), PJ_FWD,
                             {0.0, 0.0, 0.0, 0.1, 0.1, 10.0}, out, 4));
  ExpectAllHugeVal3D(out);
  EXPECT_TRUE(capture.ContainsMessage("Unable to retrieve target CRS"));
  capture.Clear();
  EXPECT_EQ(0, TransBounds3D(ctx.get(), unit_op.get(), PJ_INV,
                             {0.0, 0.0, 0.0, 5.0, 5.0, 10.0}, out, 4));
  ExpectAllHugeVal3D(out);
  EXPECT_TRUE(capture.ContainsMessage("Unable to retrieve target CRS"));
}

TEST(TransBoundsTest, NoopAndIdentDirectionPassThroughInputBounds) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  ScopedPj xform(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, xform.get());

  Bounds2D out2d;
  const Bounds2D in2d = {
      .xmin = -12.5, .ymin = 34.25, .xmax = 8.75, .ymax = 51.5};
  EXPECT_EQ(1, TransBounds(ctx.get(), noop.get(), PJ_FWD, in2d, out2d, 0));
  EXPECT_EQ(0, proj_errno(noop.get()));
  EXPECT_DOUBLE_EQ(in2d.xmin, out2d.xmin);
  EXPECT_DOUBLE_EQ(in2d.ymin, out2d.ymin);
  EXPECT_DOUBLE_EQ(in2d.xmax, out2d.xmax);
  EXPECT_DOUBLE_EQ(in2d.ymax, out2d.ymax);

  EXPECT_EQ(1, TransBounds(ctx.get(), xform.get(), PJ_IDENT, in2d, out2d, 0));
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_DOUBLE_EQ(in2d.xmin, out2d.xmin);
  EXPECT_DOUBLE_EQ(in2d.ymin, out2d.ymin);
  EXPECT_DOUBLE_EQ(in2d.xmax, out2d.xmax);
  EXPECT_DOUBLE_EQ(in2d.ymax, out2d.ymax);

  Bounds3D out3d;
  const Bounds3D in3d = {.xmin = -5.0,
                         .ymin = 10.0,
                         .zmin = -25.5,
                         .xmax = 15.0,
                         .ymax = 25.0,
                         .zmax = 400.25};
  EXPECT_EQ(1, TransBounds3D(ctx.get(), noop.get(), PJ_FWD, in3d, out3d, 0));
  EXPECT_EQ(0, proj_errno(noop.get()));
  EXPECT_DOUBLE_EQ(in3d.xmin, out3d.xmin);
  EXPECT_DOUBLE_EQ(in3d.ymin, out3d.ymin);
  EXPECT_DOUBLE_EQ(in3d.zmin, out3d.zmin);
  EXPECT_DOUBLE_EQ(in3d.xmax, out3d.xmax);
  EXPECT_DOUBLE_EQ(in3d.ymax, out3d.ymax);
  EXPECT_DOUBLE_EQ(in3d.zmax, out3d.zmax);

  EXPECT_EQ(1, TransBounds3D(ctx.get(), xform.get(), PJ_IDENT, in3d, out3d, 0));
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_DOUBLE_EQ(in3d.xmin, out3d.xmin);
  EXPECT_DOUBLE_EQ(in3d.ymin, out3d.ymin);
  EXPECT_DOUBLE_EQ(in3d.zmin, out3d.zmin);
  EXPECT_DOUBLE_EQ(in3d.xmax, out3d.xmax);
  EXPECT_DOUBLE_EQ(in3d.ymax, out3d.ymax);
  EXPECT_DOUBLE_EQ(in3d.zmax, out3d.zmax);
}

TEST(TransBoundsTest, NonDegreeAndCurvedProjectedBoundsForwardAndInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. EPSG:4326 <-> Web Mercator EPSG:3857 with densify_pts = 0 and 21.
  ScopedPj xform(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, xform.get());

  constexpr double kExpectedXmin = 1113194.9079327357;
  constexpr double kExpectedYmin = 4865942.279503176;
  constexpr double kExpectedXmax = 1335833.8895192828;
  constexpr double kExpectedYmax = 5160979.444049781;

  Bounds2D merc_bounds_zero;
  EXPECT_EQ(1, TransBounds(ctx.get(), xform.get(), PJ_FWD,
                           {40.0, 10.0, 42.0, 12.0}, merc_bounds_zero, 0));
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_NEAR(kExpectedXmin, merc_bounds_zero.xmin, 1e-4);
  EXPECT_NEAR(kExpectedYmin, merc_bounds_zero.ymin, 1e-4);
  EXPECT_NEAR(kExpectedXmax, merc_bounds_zero.xmax, 1e-4);
  EXPECT_NEAR(kExpectedYmax, merc_bounds_zero.ymax, 1e-4);

  Bounds2D merc_bounds_dense;
  EXPECT_EQ(1, TransBounds(ctx.get(), xform.get(), PJ_FWD,
                           {40.0, 10.0, 42.0, 12.0}, merc_bounds_dense, 21));
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_NEAR(kExpectedXmin, merc_bounds_dense.xmin, 1e-4);
  EXPECT_NEAR(kExpectedYmin, merc_bounds_dense.ymin, 1e-4);
  EXPECT_NEAR(kExpectedXmax, merc_bounds_dense.xmax, 1e-4);
  EXPECT_NEAR(kExpectedYmax, merc_bounds_dense.ymax, 1e-4);

  // Inverse direction recovers (lat, lon) bounds on EPSG:4326.
  Bounds2D lat_lon_roundtrip;
  EXPECT_EQ(1, TransBounds(ctx.get(), xform.get(), PJ_INV, merc_bounds_dense,
                           lat_lon_roundtrip, 21));
  EXPECT_EQ(0, proj_errno(xform.get()));
  EXPECT_NEAR(40.0, lat_lon_roundtrip.xmin, 1e-9);
  EXPECT_NEAR(10.0, lat_lon_roundtrip.ymin, 1e-9);
  EXPECT_NEAR(42.0, lat_lon_roundtrip.xmax, 1e-9);
  EXPECT_NEAR(12.0, lat_lon_roundtrip.ymax, 1e-9);

  // OGC:CRS84 (lon, lat with "Lon" abbreviation) exercises the Lon axis check.
  ScopedPj crs84_to_3857(
      proj_create_crs_to_crs(ctx.get(), "OGC:CRS84", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, crs84_to_3857.get());
  Bounds2D from_crs84;
  EXPECT_EQ(1, TransBounds(ctx.get(), crs84_to_3857.get(), PJ_FWD,
                           {10.0, 40.0, 12.0, 42.0}, from_crs84, 5));
  EXPECT_EQ(0, proj_errno(crs84_to_3857.get()));
  EXPECT_NEAR(kExpectedXmin, from_crs84.xmin, 1e-4);
  EXPECT_NEAR(kExpectedYmin, from_crs84.ymin, 1e-4);
  EXPECT_NEAR(kExpectedXmax, from_crs84.xmax, 1e-4);
  EXPECT_NEAR(kExpectedYmax, from_crs84.ymax, 1e-4);

  // 2. Curved parallels in conic projection expand via edge densification
  // for both ellipsoidal (+datum=WGS84) and spherical (+R=6371000) models.
  ScopedPj aea_ellps(proj_create_crs_to_crs(
      ctx.get(), "EPSG:4326",
      "+proj=aea +lat_1=20 +lat_2=50 +lat_0=35 +lon_0=0 +datum=WGS84 +type=crs",
      nullptr));
  ASSERT_NE(nullptr, aea_ellps.get());
  Bounds2D aea_corners;
  Bounds2D aea_densified;
  EXPECT_EQ(1, TransBounds(ctx.get(), aea_ellps.get(), PJ_FWD,
                           {20.0, -60.0, 50.0, 60.0}, aea_corners, 0));
  EXPECT_EQ(0, proj_errno(aea_ellps.get()));
  EXPECT_EQ(1, TransBounds(ctx.get(), aea_ellps.get(), PJ_FWD,
                           {20.0, -60.0, 50.0, 60.0}, aea_densified, 21));
  EXPECT_EQ(0, proj_errno(aea_ellps.get()));
  EXPECT_LT(aea_densified.ymin, aea_corners.ymin);

  ScopedPj aea_sph(proj_create_crs_to_crs(
      ctx.get(), "+proj=longlat +R=6371000 +type=crs",
      "+proj=aea +lat_1=20 +lat_2=50 +lat_0=35 +lon_0=0 +R=6371000 +type=crs",
      nullptr));
  ASSERT_NE(nullptr, aea_sph.get());
  Bounds2D sph_corners;
  Bounds2D sph_densified;
  EXPECT_EQ(1, TransBounds(ctx.get(), aea_sph.get(), PJ_FWD,
                           {-60.0, 20.0, 60.0, 50.0}, sph_corners, 0));
  EXPECT_EQ(0, proj_errno(aea_sph.get()));
  EXPECT_EQ(1, TransBounds(ctx.get(), aea_sph.get(), PJ_FWD,
                           {-60.0, 20.0, 60.0, 50.0}, sph_densified, 21));
  EXPECT_EQ(0, proj_errno(aea_sph.get()));
  EXPECT_LT(sph_densified.ymin, sph_corners.ymin);
}

TEST(TransBoundsTest, OrthographicDomainSingularitiesAndHugeValWraparound) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Spherical orthographic inverse where index 0 and 11 of 12 ring points
  // return HUGE_VAL and only corner (4.4e6, 4.4e6) is inside the disk.
  // Exercises simple_max(max_value == HUGE_VAL), antimeridian HUGE_VAL skips,
  // find_previous_index wraparound inside the loop, prev_iii == iii exit,
  // and !std::isfinite in interior grid sampling.
  ScopedPj ortho_sph(
      proj_create_crs_to_crs(ctx.get(), "+proj=ortho +R=6371000 +type=crs",
                             "+proj=longlat +R=6371000 +type=crs", nullptr));
  ASSERT_NE(nullptr, ortho_sph.get());

  Bounds2D single_valid;
  EXPECT_EQ(1, TransBounds(ctx.get(), ortho_sph.get(), PJ_FWD,
                           {4.4e6, 4.4e6, 7.0e6, 7.0e6}, single_valid, 2));
  EXPECT_NEAR(72.7367958867, single_valid.xmin, 1e-6);
  EXPECT_NEAR(72.7367958867, single_valid.xmax, 1e-6);
  EXPECT_NEAR(43.6799531534, single_valid.ymin, 1e-6);
  EXPECT_NEAR(43.6799531534, single_valid.ymax, 1e-6);
  proj_errno_reset(ortho_sph.get());

  // 2. Partial box: (-4e6, 5e6) at index 0 is off-disk (HUGE_VAL) while
  // index 1 and interior points mix finite & HUGE_VAL coordinates.
  Bounds2D partial_sph;
  EXPECT_EQ(1, TransBounds(ctx.get(), ortho_sph.get(), PJ_FWD,
                           {-4.0e6, -4.0e6, 5.0e6, 5.0e6}, partial_sph, 4));
  EXPECT_TRUE(std::isfinite(partial_sph.xmin));
  EXPECT_TRUE(std::isfinite(partial_sph.xmax));
  EXPECT_TRUE(std::isfinite(partial_sph.ymin));
  EXPECT_TRUE(std::isfinite(partial_sph.ymax));
  EXPECT_LT(partial_sph.xmin, -50.0);
  EXPECT_GT(partial_sph.xmax, 55.0);
  EXPECT_LT(partial_sph.ymin, -38.0);
  EXPECT_GT(partial_sph.ymax, 50.0);
  proj_errno_reset(ortho_sph.get());

  // 3. Ellipsoidal orthographic forward and inverse with off-hemisphere points
  // (covers !degree_output simple_max(max_value == HUGE_VAL) + interior skips).
  ScopedPj ortho_ell(proj_create_crs_to_crs(
      ctx.get(), "+proj=ortho +datum=WGS84 +type=crs", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, ortho_ell.get());

  Bounds2D partial_ell;
  EXPECT_EQ(1, TransBounds(ctx.get(), ortho_ell.get(), PJ_FWD,
                           {-4.0e6, -4.0e6, 5.0e6, 5.0e6}, partial_ell, 4));
  EXPECT_LT(partial_ell.xmin, -38.0);
  EXPECT_GT(partial_ell.xmax, 50.0);
  EXPECT_LT(partial_ell.ymin, -50.0);
  EXPECT_GT(partial_ell.ymax, 55.0);
  proj_errno_reset(ortho_ell.get());

  // PJ_INV: EPSG:4326 (lat in [0, 30], lon in [70, 100]) -> ortho meters.
  // Corner (0, 100) at index 0 is behind the limb (HUGE_VAL).
  Bounds2D partial_fwd_proj;
  EXPECT_EQ(1, TransBounds(ctx.get(), ortho_ell.get(), PJ_INV,
                           {0.0, 70.0, 30.0, 100.0}, partial_fwd_proj, 4));
  EXPECT_TRUE(std::isfinite(partial_fwd_proj.xmin));
  EXPECT_TRUE(std::isfinite(partial_fwd_proj.xmax));
  EXPECT_TRUE(std::isfinite(partial_fwd_proj.ymin));
  EXPECT_TRUE(std::isfinite(partial_fwd_proj.ymax));
  EXPECT_GT(partial_fwd_proj.xmax, 5.0e6);
  EXPECT_GE(partial_fwd_proj.ymax, 0.0);
  proj_errno_reset(ortho_ell.get());
}

TEST(TransBoundsTest, AntimeridianCrossingsOutputAndInputDirections) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Two crossings (180 -> -180, delta >= 200): Mercator lon_0=180.
  ScopedPj merc180_latlon(proj_create_crs_to_crs(
      ctx.get(), "+proj=merc +lon_0=180 +datum=WGS84 +type=crs", "EPSG:4326",
      nullptr));
  ASSERT_NE(nullptr, merc180_latlon.get());
  ScopedPj merc180_norm(
      proj_normalize_for_visualization(ctx.get(), merc180_latlon.get()));
  ASSERT_NE(nullptr, merc180_norm.get());

  const Bounds2D merc_box = {
      .xmin = -500000.0, .ymin = -500000.0, .xmax = 500000.0, .ymax = 500000.0};
  Bounds2D out_norm;
  EXPECT_EQ(1, TransBounds(ctx.get(), merc180_norm.get(), PJ_FWD, merc_box,
                           out_norm, 21));
  EXPECT_EQ(0, proj_errno(merc180_norm.get()));
  EXPECT_GT(out_norm.xmin, out_norm.xmax);
  EXPECT_NEAR(175.5084236, out_norm.xmin, 1e-5);
  EXPECT_NEAR(-175.5084236, out_norm.xmax, 1e-5);
  EXPECT_NEAR(-out_norm.ymax, out_norm.ymin, 1e-9);
  EXPECT_NEAR(4.516, out_norm.ymax, 0.05);

  Bounds2D out_latlon;
  EXPECT_EQ(1, TransBounds(ctx.get(), merc180_latlon.get(), PJ_FWD, merc_box,
                           out_latlon, 21));
  EXPECT_EQ(0, proj_errno(merc180_latlon.get()));
  EXPECT_GT(out_latlon.ymin, out_latlon.ymax);
  EXPECT_NEAR(175.5084236, out_latlon.ymin, 1e-5);
  EXPECT_NEAR(-175.5084236, out_latlon.ymax, 1e-5);

  // 2. Two crossings starting with -180 -> 180 (delta <= -200): North Polar
  // Stereographic lon_0=0 box [+y only, excluding the pole].
  ScopedPj stere_np_latlon(proj_create_crs_to_crs(
      ctx.get(), "+proj=stere +lat_0=90 +lon_0=0 +datum=WGS84 +type=crs",
      "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, stere_np_latlon.get());
  ScopedPj stere_np_norm(
      proj_normalize_for_visualization(ctx.get(), stere_np_latlon.get()));
  ASSERT_NE(nullptr, stere_np_norm.get());

  const Bounds2D stere_no_pole = {
      .xmin = -200000.0, .ymin = 500000.0, .xmax = 200000.0, .ymax = 1000000.0};
  EXPECT_EQ(1, TransBounds(ctx.get(), stere_np_norm.get(), PJ_FWD,
                           stere_no_pole, out_norm, 20));
  EXPECT_EQ(0, proj_errno(stere_np_norm.get()));
  EXPECT_GT(out_norm.xmin, out_norm.xmax);
  EXPECT_NEAR(158.1985905, out_norm.xmin, 1e-4);
  EXPECT_NEAR(-158.1985905, out_norm.xmax, 1e-4);
  EXPECT_GT(out_norm.ymin, 80.0);
  EXPECT_LT(out_norm.ymax, 86.0);

  EXPECT_EQ(1, TransBounds(ctx.get(), stere_np_latlon.get(), PJ_FWD,
                           stere_no_pole, out_latlon, 20));
  EXPECT_EQ(0, proj_errno(stere_np_latlon.get()));
  EXPECT_GT(out_latlon.ymin, out_latlon.ymax);
  EXPECT_NEAR(158.1985905, out_latlon.ymin, 1e-4);
  EXPECT_NEAR(-158.1985905, out_latlon.ymax, 1e-4);

  // 3. Four crossings (crossed_meridian_count == 4): returns [-180, 180].
  ScopedPj merc3857_latlon(
      proj_create_crs_to_crs(ctx.get(), "EPSG:3857", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, merc3857_latlon.get());
  ScopedPj merc3857_norm(
      proj_normalize_for_visualization(ctx.get(), merc3857_latlon.get()));
  ASSERT_NE(nullptr, merc3857_norm.get());

  const Bounds2D global_wrap = {.xmin = -21000000.0,
                                .ymin = -1000000.0,
                                .xmax = 21000000.0,
                                .ymax = 1000000.0};
  EXPECT_EQ(1, TransBounds(ctx.get(), merc3857_norm.get(), PJ_FWD, global_wrap,
                           out_norm, 21));
  EXPECT_EQ(0, proj_errno(merc3857_norm.get()));
  EXPECT_DOUBLE_EQ(-180.0, out_norm.xmin);
  EXPECT_DOUBLE_EQ(180.0, out_norm.xmax);
  EXPECT_NEAR(-8.94657385, out_norm.ymin, 1e-5);
  EXPECT_NEAR(8.94657385, out_norm.ymax, 1e-5);

  EXPECT_EQ(1, TransBounds(ctx.get(), merc3857_latlon.get(), PJ_FWD,
                           global_wrap, out_latlon, 21));
  EXPECT_EQ(0, proj_errno(merc3857_latlon.get()));
  EXPECT_DOUBLE_EQ(-180.0, out_latlon.ymin);
  EXPECT_DOUBLE_EQ(180.0, out_latlon.ymax);

  // 4. Antimeridian geographic input in (lon, lat) and (lat, lon).
  Bounds2D in_norm_to_merc;
  EXPECT_EQ(1, TransBounds(ctx.get(), merc180_norm.get(), PJ_INV,
                           {175.0, -5.0, -175.0, 5.0}, in_norm_to_merc, 10));
  EXPECT_EQ(0, proj_errno(merc180_norm.get()));
  EXPECT_NEAR(-556597.454, in_norm_to_merc.xmin, 1.0);
  EXPECT_NEAR(556597.454, in_norm_to_merc.xmax, 1.0);

  Bounds2D in_latlon_to_merc;
  EXPECT_EQ(1, TransBounds(ctx.get(), merc180_latlon.get(), PJ_INV,
                           {-5.0, 175.0, 5.0, -175.0}, in_latlon_to_merc, 10));
  EXPECT_EQ(0, proj_errno(merc180_latlon.get()));
  EXPECT_NEAR(-556597.454, in_latlon_to_merc.xmin, 1.0);
  EXPECT_NEAR(556597.454, in_latlon_to_merc.xmax, 1.0);
}

TEST(TransBoundsTest, PolarContainmentNorthAndSouthPoles) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. North pole (EPSG:3413 <-> EPSG:4326 and normalized).
  ScopedPj np_latlon(
      proj_create_crs_to_crs(ctx.get(), "EPSG:3413", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, np_latlon.get());
  ScopedPj np_norm(
      proj_normalize_for_visualization(ctx.get(), np_latlon.get()));
  ASSERT_NE(nullptr, np_norm.get());

  const Bounds2D around_pole = {
      .xmin = -1.0e6, .ymin = -1.0e6, .xmax = 1.0e6, .ymax = 1.0e6};
  Bounds2D out;
  EXPECT_EQ(
      1, TransBounds(ctx.get(), np_norm.get(), PJ_FWD, around_pole, out, 21));
  EXPECT_EQ(0, proj_errno(np_norm.get()));
  EXPECT_DOUBLE_EQ(-180.0, out.xmin);
  EXPECT_DOUBLE_EQ(180.0, out.xmax);
  EXPECT_DOUBLE_EQ(90.0, out.ymax);
  EXPECT_LT(out.ymin, 79.0);

  EXPECT_EQ(
      1, TransBounds(ctx.get(), np_latlon.get(), PJ_FWD, around_pole, out, 21));
  EXPECT_EQ(0, proj_errno(np_latlon.get()));
  EXPECT_DOUBLE_EQ(-180.0, out.ymin);
  EXPECT_DOUBLE_EQ(180.0, out.ymax);
  EXPECT_DOUBLE_EQ(90.0, out.xmax);
  EXPECT_LT(out.xmin, 79.0);

  // Exclude North Pole cleanly and test PJ_INV direction on 4326 -> 3413.
  const Bounds2D off_pole = {
      .xmin = 1.0e6, .ymin = 1.0e6, .xmax = 2.0e6, .ymax = 2.0e6};
  EXPECT_EQ(1,
            TransBounds(ctx.get(), np_norm.get(), PJ_FWD, off_pole, out, 21));
  EXPECT_EQ(0, proj_errno(np_norm.get()));
  EXPECT_LT(out.ymax, 80.0);

  ScopedPj geog_to_np(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3413", nullptr));
  ASSERT_NE(nullptr, geog_to_np.get());
  EXPECT_EQ(1, TransBounds(ctx.get(), geog_to_np.get(), PJ_INV, around_pole,
                           out, 21));
  EXPECT_EQ(0, proj_errno(geog_to_np.get()));
  EXPECT_DOUBLE_EQ(-180.0, out.ymin);
  EXPECT_DOUBLE_EQ(180.0, out.ymax);
  EXPECT_DOUBLE_EQ(90.0, out.xmax);

  // 2. South pole (EPSG:3031 <-> EPSG:4326 and normalized).
  ScopedPj sp_latlon(
      proj_create_crs_to_crs(ctx.get(), "EPSG:3031", "EPSG:4326", nullptr));
  ASSERT_NE(nullptr, sp_latlon.get());
  ScopedPj sp_norm(
      proj_normalize_for_visualization(ctx.get(), sp_latlon.get()));
  ASSERT_NE(nullptr, sp_norm.get());

  EXPECT_EQ(
      1, TransBounds(ctx.get(), sp_norm.get(), PJ_FWD, around_pole, out, 21));
  EXPECT_EQ(0, proj_errno(sp_norm.get()));
  EXPECT_DOUBLE_EQ(-180.0, out.xmin);
  EXPECT_DOUBLE_EQ(180.0, out.xmax);
  EXPECT_DOUBLE_EQ(-90.0, out.ymin);
  EXPECT_GT(out.ymax, -79.0);

  EXPECT_EQ(
      1, TransBounds(ctx.get(), sp_latlon.get(), PJ_FWD, around_pole, out, 21));
  EXPECT_EQ(0, proj_errno(sp_latlon.get()));
  EXPECT_DOUBLE_EQ(-180.0, out.ymin);
  EXPECT_DOUBLE_EQ(180.0, out.ymax);
  EXPECT_DOUBLE_EQ(-90.0, out.xmin);
  EXPECT_GT(out.xmax, -79.0);

  EXPECT_EQ(1,
            TransBounds(ctx.get(), sp_norm.get(), PJ_FWD, off_pole, out, 21));
  EXPECT_EQ(0, proj_errno(sp_norm.get()));
  EXPECT_GT(out.ymin, -80.0);

  ScopedPj geog_to_sp(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3031", nullptr));
  ASSERT_NE(nullptr, geog_to_sp.get());
  EXPECT_EQ(1, TransBounds(ctx.get(), geog_to_sp.get(), PJ_INV, around_pole,
                           out, 21));
  EXPECT_EQ(0, proj_errno(geog_to_sp.get()));
  EXPECT_DOUBLE_EQ(-180.0, out.ymin);
  EXPECT_DOUBLE_EQ(180.0, out.ymax);
  EXPECT_DOUBLE_EQ(-90.0, out.xmin);
}

TEST(TransBoundsTest, CompoundCrsAndPipelineStringFallbackAxisOrder) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. CompoundCRS targets extract horizontal sub-CRS 0 axis order.
  ScopedPj crs3857(proj_create(ctx.get(), "EPSG:3857"));
  ScopedPj crs4326(proj_create(ctx.get(), "EPSG:4326"));
  ScopedPj crs84(proj_create(ctx.get(), "OGC:CRS84"));
  ScopedPj vert(proj_create_vertical_crs(ctx.get(), "TestVert", "TestDatum",
                                         "metre", 1.0));
  ASSERT_NE(nullptr, crs3857.get());
  ASSERT_NE(nullptr, crs4326.get());
  ASSERT_NE(nullptr, crs84.get());
  ASSERT_NE(nullptr, vert.get());

  ScopedPj comp_3857(
      proj_create_compound_crs(ctx.get(), "3857+V", crs3857.get(), vert.get()));
  ScopedPj comp_4326(
      proj_create_compound_crs(ctx.get(), "4326+V", crs4326.get(), vert.get()));
  ScopedPj comp_84(
      proj_create_compound_crs(ctx.get(), "CRS84+V", crs84.get(), vert.get()));
  ASSERT_NE(nullptr, comp_3857.get());
  ASSERT_NE(nullptr, comp_4326.get());
  ASSERT_NE(nullptr, comp_84.get());

  ScopedPj op_to_comp_latlon(proj_create_crs_to_crs_from_pj(
      ctx.get(), comp_3857.get(), comp_4326.get(), nullptr, nullptr));
  ScopedPj op_to_comp_lonlat(proj_create_crs_to_crs_from_pj(
      ctx.get(), comp_3857.get(), comp_84.get(), nullptr, nullptr));
  ASSERT_NE(nullptr, op_to_comp_latlon.get());
  ASSERT_NE(nullptr, op_to_comp_lonlat.get());

  const Bounds2D small_merc = {.xmin = 1113194.908,
                               .ymin = 4865942.280,
                               .xmax = 1335833.890,
                               .ymax = 5160979.444};
  Bounds2D out_latlon;
  Bounds2D out_lonlat;
  EXPECT_EQ(1, TransBounds(ctx.get(), op_to_comp_latlon.get(), PJ_FWD,
                           small_merc, out_latlon, 10));
  EXPECT_EQ(0, proj_errno(op_to_comp_latlon.get()));
  EXPECT_NEAR(40.0, out_latlon.xmin, 1e-4);
  EXPECT_NEAR(10.0, out_latlon.ymin, 1e-4);
  EXPECT_NEAR(42.0, out_latlon.xmax, 1e-4);
  EXPECT_NEAR(12.0, out_latlon.ymax, 1e-4);

  EXPECT_EQ(1, TransBounds(ctx.get(), op_to_comp_lonlat.get(), PJ_FWD,
                           small_merc, out_lonlat, 10));
  EXPECT_EQ(0, proj_errno(op_to_comp_lonlat.get()));
  EXPECT_NEAR(10.0, out_lonlat.xmin, 1e-4);
  EXPECT_NEAR(40.0, out_lonlat.ymin, 1e-4);
  EXPECT_NEAR(12.0, out_lonlat.xmax, 1e-4);
  EXPECT_NEAR(42.0, out_lonlat.ymax, 1e-4);

  // 2. Plain +proj=pipeline operations with no attached CRS: exercise all 4
  // string-matching fallback branches in both PJ_FWD and PJ_INV.
  struct PipelineCase {
    const char* def;
    PJ_DIRECTION dir;
    Bounds2D in;
    double expect_xmin;
    double expect_ymin;
    double expect_xmax;
    double expect_ymax;
  };
  constexpr PipelineCase kPipelines[] = {
      // Output ends with "+step +proj=unitconvert +xy_in=rad +xy_out=deg".
      {.def = "+proj=pipeline +step +inv +proj=merc +ellps=WGS84 "
              "+step +proj=unitconvert +xy_in=rad +xy_out=deg",
       .dir = PJ_FWD,
       .in = {111319.4908, 221194.0772, 222638.9816, 442661.6382},
       .expect_xmin = 1.0,
       .expect_ymin = 2.0,
       .expect_xmax = 2.0,
       .expect_ymax = 4.0},
      {.def = "+proj=pipeline +step +inv +proj=merc +ellps=WGS84 "
              "+step +proj=unitconvert +xy_in=rad +xy_out=deg",
       .dir = PJ_INV,
       .in = {1.0, 2.0, 2.0, 4.0},
       .expect_xmin = 111319.4908,
       .expect_ymin = 221194.0772,
       .expect_xmax = 222638.9816,
       .expect_ymax = 442661.6382},
      // Output ends with "... +step +proj=axisswap +order=2,1" -> (lat, lon).
      {.def = "+proj=pipeline +step +inv +proj=merc +ellps=WGS84 "
              "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
              "+step +proj=axisswap +order=2,1",
       .dir = PJ_FWD,
       .in = {111319.4908, 221194.0772, 222638.9816, 442661.6382},
       .expect_xmin = 2.0,
       .expect_ymin = 1.0,
       .expect_xmax = 4.0,
       .expect_ymax = 2.0},
      {.def = "+proj=pipeline +step +inv +proj=merc +ellps=WGS84 "
              "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
              "+step +proj=axisswap +order=2,1",
       .dir = PJ_INV,
       .in = {2.0, 1.0, 4.0, 2.0},
       .expect_xmin = 111319.4908,
       .expect_ymin = 221194.0772,
       .expect_xmax = 222638.9816,
       .expect_ymax = 442661.6382},
      // Input starts with unitconvert deg->rad -> (lon, lat) in FWD and INV.
      {.def = "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad "
              "+step +proj=merc +ellps=WGS84",
       .dir = PJ_FWD,
       .in = {1.0, 2.0, 2.0, 4.0},
       .expect_xmin = 111319.4908,
       .expect_ymin = 221194.0772,
       .expect_xmax = 222638.9816,
       .expect_ymax = 442661.6382},
      {.def = "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad "
              "+step +proj=merc +ellps=WGS84",
       .dir = PJ_INV,
       .in = {111319.4908, 221194.0772, 222638.9816, 442661.6382},
       .expect_xmin = 1.0,
       .expect_ymin = 2.0,
       .expect_xmax = 2.0,
       .expect_ymax = 4.0},
      // Input starts with axisswap +order=2,1 + unitconvert deg->rad.
      {.def = "+proj=pipeline +step +proj=axisswap +order=2,1 "
              "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
              "+step +proj=merc +ellps=WGS84",
       .dir = PJ_FWD,
       .in = {2.0, 1.0, 4.0, 2.0},
       .expect_xmin = 111319.4908,
       .expect_ymin = 221194.0772,
       .expect_xmax = 222638.9816,
       .expect_ymax = 442661.6382},
      {.def = "+proj=pipeline +step +proj=axisswap +order=2,1 "
              "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
              "+step +proj=merc +ellps=WGS84",
       .dir = PJ_INV,
       .in = {111319.4908, 221194.0772, 222638.9816, 442661.6382},
       .expect_xmin = 2.0,
       .expect_ymin = 1.0,
       .expect_xmax = 4.0,
       .expect_ymax = 2.0},
  };

  for (const auto& tc : kPipelines) {
    ScopedPj pipe(proj_create(ctx.get(), tc.def));
    ASSERT_NE(nullptr, pipe.get()) << tc.def;
    Bounds2D out;
    EXPECT_EQ(1, TransBounds(ctx.get(), pipe.get(), tc.dir, tc.in, out, 5))
        << tc.def;
    EXPECT_EQ(0, proj_errno(pipe.get()));
    EXPECT_NEAR(tc.expect_xmin, out.xmin, 1e-2) << tc.def;
    EXPECT_NEAR(tc.expect_ymin, out.ymin, 1e-2) << tc.def;
    EXPECT_NEAR(tc.expect_xmax, out.xmax, 1e-2) << tc.def;
    EXPECT_NEAR(tc.expect_ymax, out.ymax, 1e-2) << tc.def;
  }
}

TEST(TransBoundsTest, Bounds3DGeocentricPolesAndAntimeridian) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Geocentric (EPSG:4978) <-> Geographic 3D (EPSG:4979) forward & inverse.
  ScopedPj ecef_to_4979(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4978", "EPSG:4979", nullptr));
  ASSERT_NE(nullptr, ecef_to_4979.get());
  ScopedPj ecef_to_4979_norm(
      proj_normalize_for_visualization(ctx.get(), ecef_to_4979.get()));
  ASSERT_NE(nullptr, ecef_to_4979_norm.get());

  const Bounds3D ecef_box = {.xmin = 6257400.0,
                             .ymin = -1107800.0,
                             .zmin = -552300.0,
                             .xmax = 6379200.0,
                             .ymax = 1107800.0,
                             .zmax = 552300.0};
  Bounds3D out3d;
  EXPECT_EQ(1, TransBounds3D(ctx.get(), ecef_to_4979_norm.get(), PJ_FWD,
                             ecef_box, out3d, 10));
  EXPECT_EQ(0, proj_errno(ecef_to_4979_norm.get()));
  EXPECT_LT(out3d.xmin, -9.8);
  EXPECT_GT(out3d.xmax, 9.8);
  EXPECT_LT(out3d.ymin, -4.8);
  EXPECT_GT(out3d.ymax, 4.8);
  EXPECT_LT(out3d.zmin, 0.0);
  EXPECT_GT(out3d.zmax, 1000.0);

  // Geographic 3D -> Geocentric (!input_is_geocentric & !degree_output);
  // PJ_INV on geod_to_ecef / crs84_3d_to_ecef uses input_is_geocentric == true.
  ScopedPj geod_to_ecef(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4979", "EPSG:4978", nullptr));
  ASSERT_NE(nullptr, geod_to_ecef.get());
  // side_pts = densify_pts + 1 = 12 (even) samples (lat=0, lon=0, h=1000)
  // directly so xmax captures the equatorial bulge + height (a + 1000 m).
  EXPECT_EQ(1, TransBounds3D(ctx.get(), geod_to_ecef.get(), PJ_FWD,
                             {-5.0, -10.0, 0.0, 5.0, 10.0, 1000.0}, out3d, 11));
  EXPECT_EQ(0, proj_errno(geod_to_ecef.get()));
  EXPECT_NEAR(6257495.86, out3d.xmin, 1.0);
  EXPECT_NEAR(6379137.0, out3d.xmax, 1.0);
  EXPECT_NEAR(-1107725.52, out3d.ymin, 1.0);
  EXPECT_NEAR(1107725.52, out3d.ymax, 1.0);
  EXPECT_NEAR(-552271.12, out3d.zmin, 1.0);
  EXPECT_NEAR(552271.12, out3d.zmax, 1.0);

  // Antimeridian geographic 3D inputs in both (lon, lat, h) and (lat, lon, h).
  ScopedPj crs84_2d(proj_create(ctx.get(), "OGC:CRS84"));
  ASSERT_NE(nullptr, crs84_2d.get());
  ScopedPj crs84_3d(
      proj_crs_promote_to_3D(ctx.get(), "CRS84_3D", crs84_2d.get()));
  ScopedPj crs4978(proj_create(ctx.get(), "EPSG:4978"));
  ASSERT_NE(nullptr, crs84_3d.get());
  ASSERT_NE(nullptr, crs4978.get());
  ScopedPj crs84_3d_to_ecef(proj_create_crs_to_crs_from_pj(
      ctx.get(), crs84_3d.get(), crs4978.get(), nullptr, nullptr));
  ASSERT_NE(nullptr, crs84_3d_to_ecef.get());

  EXPECT_EQ(1, TransBounds3D(ctx.get(), crs84_3d_to_ecef.get(), PJ_FWD,
                             {175.0, -5.0, 0.0, -175.0, 5.0, 500.0}, out3d, 8));
  EXPECT_EQ(0, proj_errno(crs84_3d_to_ecef.get()));
  EXPECT_LT(out3d.xmin, -6300000.0);
  EXPECT_NEAR(0.0, (out3d.ymin + out3d.ymax) * 0.5, 1.0);

  // Inverse from geocentric back to (lon, lat, h) via input_is_geocentric.
  Bounds3D inv_out3d;
  EXPECT_EQ(1, TransBounds3D(ctx.get(), crs84_3d_to_ecef.get(), PJ_INV,
                             ecef_box, inv_out3d, 10));
  EXPECT_EQ(0, proj_errno(crs84_3d_to_ecef.get()));
  EXPECT_LT(inv_out3d.xmin, -9.8);
  EXPECT_GT(inv_out3d.xmax, 9.8);
  EXPECT_LT(inv_out3d.ymin, -4.8);
  EXPECT_GT(inv_out3d.ymax, 4.8);
  EXPECT_LT(inv_out3d.zmin, 0.0);
  EXPECT_GT(inv_out3d.zmax, 1000.0);

  EXPECT_EQ(1, TransBounds3D(ctx.get(), geod_to_ecef.get(), PJ_FWD,
                             {-5.0, 175.0, 0.0, 5.0, -175.0, 500.0}, out3d, 8));
  EXPECT_EQ(0, proj_errno(geod_to_ecef.get()));
  EXPECT_LT(out3d.xmin, -6300000.0);

  // Inverse of geod_to_ecef returns native EPSG:4979 (lat, lon, h) bounds.
  EXPECT_EQ(1, TransBounds3D(ctx.get(), geod_to_ecef.get(), PJ_INV, ecef_box,
                             inv_out3d, 10));
  EXPECT_EQ(0, proj_errno(geod_to_ecef.get()));
  EXPECT_LT(inv_out3d.xmin, -4.8);
  EXPECT_GT(inv_out3d.xmax, 4.8);
  EXPECT_LT(inv_out3d.ymin, -9.8);
  EXPECT_GT(inv_out3d.ymax, 9.8);

  // 2. Geocentric (EPSG:4978) -> 3D projected UTM 32N.
  ScopedPj utm32_2d(proj_create(ctx.get(), "EPSG:32632"));
  ASSERT_NE(nullptr, utm32_2d.get());
  ScopedPj utm32_3d(
      proj_crs_promote_to_3D(ctx.get(), "UTM32_3D", utm32_2d.get()));
  ASSERT_NE(nullptr, utm32_3d.get());
  ScopedPj ecef_to_utm3d(proj_create_crs_to_crs_from_pj(
      ctx.get(), crs4978.get(), utm32_3d.get(), nullptr, nullptr));
  ASSERT_NE(nullptr, ecef_to_utm3d.get());
  EXPECT_EQ(1, TransBounds3D(ctx.get(), ecef_to_utm3d.get(), PJ_FWD,
                             {4000000.0, 600000.0, 4800000.0, 4100000.0,
                              700000.0, 4900000.0},
                             out3d, 5));
  EXPECT_EQ(0, proj_errno(ecef_to_utm3d.get()));
  EXPECT_LT(out3d.xmin, out3d.xmax);
  EXPECT_LT(out3d.ymin, out3d.ymax);
  EXPECT_LT(out3d.zmin, out3d.zmax);

  // 3. Projected 3D -> normalized Geographic 3D: poles, antimeridian,
  // normal bounds, and partial-domain singularities.
  ScopedPj np3d(
      proj_create_crs_to_crs(ctx.get(), "EPSG:3413", "EPSG:4979", nullptr));
  ASSERT_NE(nullptr, np3d.get());
  ScopedPj np3d_norm(proj_normalize_for_visualization(ctx.get(), np3d.get()));
  ASSERT_NE(nullptr, np3d_norm.get());
  EXPECT_EQ(
      1, TransBounds3D(ctx.get(), np3d_norm.get(), PJ_FWD,
                       {-1.0e6, -1.0e6, 0.0, 1.0e6, 1.0e6, 500.0}, out3d, 10));
  EXPECT_EQ(0, proj_errno(np3d_norm.get()));
  EXPECT_DOUBLE_EQ(-180.0, out3d.xmin);
  EXPECT_DOUBLE_EQ(180.0, out3d.xmax);
  EXPECT_DOUBLE_EQ(90.0, out3d.ymax);
  EXPECT_NEAR(0.0, out3d.zmin, 1e-6);
  EXPECT_NEAR(500.0, out3d.zmax, 1e-6);

  ScopedPj sp3d(
      proj_create_crs_to_crs(ctx.get(), "EPSG:3031", "EPSG:4979", nullptr));
  ASSERT_NE(nullptr, sp3d.get());
  ScopedPj sp3d_norm(proj_normalize_for_visualization(ctx.get(), sp3d.get()));
  ASSERT_NE(nullptr, sp3d_norm.get());
  EXPECT_EQ(
      1, TransBounds3D(ctx.get(), sp3d_norm.get(), PJ_FWD,
                       {-1.0e6, -1.0e6, 10.0, 1.0e6, 1.0e6, 250.0}, out3d, 10));
  EXPECT_EQ(0, proj_errno(sp3d_norm.get()));
  EXPECT_DOUBLE_EQ(-180.0, out3d.xmin);
  EXPECT_DOUBLE_EQ(180.0, out3d.xmax);
  EXPECT_DOUBLE_EQ(-90.0, out3d.ymin);
  EXPECT_NEAR(10.0, out3d.zmin, 1e-6);
  EXPECT_NEAR(250.0, out3d.zmax, 1e-6);

  ScopedPj merc180_3d(proj_create_crs_to_crs(
      ctx.get(), "+proj=merc +lon_0=180 +datum=WGS84 +type=crs", "EPSG:4979",
      nullptr));
  ASSERT_NE(nullptr, merc180_3d.get());
  ScopedPj merc180_3d_norm(
      proj_normalize_for_visualization(ctx.get(), merc180_3d.get()));
  ASSERT_NE(nullptr, merc180_3d_norm.get());
  EXPECT_EQ(
      1, TransBounds3D(ctx.get(), merc180_3d_norm.get(), PJ_FWD,
                       {-500000.0, -500000.0, -50.0, 500000.0, 500000.0, 150.0},
                       out3d, 21));
  EXPECT_EQ(0, proj_errno(merc180_3d_norm.get()));
  EXPECT_GT(out3d.xmin, out3d.xmax);
  EXPECT_NEAR(175.5084236, out3d.xmin, 1e-5);
  EXPECT_NEAR(-175.5084236, out3d.xmax, 1e-5);
  EXPECT_NEAR(-50.0, out3d.zmin, 1e-6);
  EXPECT_NEAR(150.0, out3d.zmax, 1e-6);

  // Non-antimeridian 3D UTM -> normalized 4979 runs interior grid sampling.
  ScopedPj crs4979(proj_create(ctx.get(), "EPSG:4979"));
  ASSERT_NE(nullptr, crs4979.get());
  ScopedPj utm3d_to_4979(proj_create_crs_to_crs_from_pj(
      ctx.get(), utm32_3d.get(), crs4979.get(), nullptr, nullptr));
  ASSERT_NE(nullptr, utm3d_to_4979.get());
  ScopedPj utm3d_to_4979_norm(
      proj_normalize_for_visualization(ctx.get(), utm3d_to_4979.get()));
  ASSERT_NE(nullptr, utm3d_to_4979_norm.get());
  EXPECT_EQ(
      1, TransBounds3D(ctx.get(), utm3d_to_4979_norm.get(), PJ_FWD,
                       {400000.0, 5000000.0, 10.0, 500000.0, 5100000.0, 200.0},
                       out3d, 8));
  EXPECT_EQ(0, proj_errno(utm3d_to_4979_norm.get()));
  EXPECT_GT(out3d.xmin, 7.0);
  EXPECT_LT(out3d.xmax, 9.5);
  EXPECT_GT(out3d.ymin, 45.0);
  EXPECT_LT(out3d.ymax, 46.5);
  EXPECT_NEAR(10.0, out3d.zmin, 1e-6);
  EXPECT_NEAR(200.0, out3d.zmax, 1e-6);

  // Note: When degree_output && !output_lon_lat_order (e.g. un-normalized
  // EPSG:4979), proj_trans_bounds_3D() in PROJ 9.9.0 swaps out_xmin/out_ymin
  // on both iter_z == 0 and iter_z == 1 iterations, so only assert success here
  // to cover those branches without baking in axis-order artifacts.
  EXPECT_EQ(
      1, TransBounds3D(ctx.get(), utm3d_to_4979.get(), PJ_FWD,
                       {400000.0, 5000000.0, 10.0, 500000.0, 5100000.0, 200.0},
                       out3d, 4));
  EXPECT_EQ(0, proj_errno(utm3d_to_4979.get()));

  // Partial orthographic in 3D covers !std::isfinite skip in 3D interior loop.
  ScopedLogSuppressor suppress(ctx.get());
  ScopedPj ortho3d(
      proj_create_crs_to_crs(ctx.get(), "+proj=ortho +R=6371000 +type=crs",
                             "+proj=longlat +R=6371000 +type=crs", nullptr));
  ASSERT_NE(nullptr, ortho3d.get());
  EXPECT_EQ(
      1, TransBounds3D(ctx.get(), ortho3d.get(), PJ_FWD,
                       {-4.0e6, -4.0e6, -20.0, 5.0e6, 5.0e6, 80.0}, out3d, 4));
  EXPECT_TRUE(std::isfinite(out3d.xmin));
  EXPECT_TRUE(std::isfinite(out3d.xmax));
  EXPECT_NEAR(-20.0, out3d.zmin, 1e-6);
  EXPECT_NEAR(80.0, out3d.zmax, 1e-6);
  proj_errno_reset(ortho3d.get());
}

// TODO(schwehr): Covered >90% of trans_bounds.cpp; remaining defensive code:
// - OOM catch (const std::exception&) blocks during boundary vector resize.
// - target_crs_lon_lat_order defensive !horiz_crs, coord_system_pj == nullptr,
//   and proj_cs_get_axis_info failure (success != 1) branches.

}  // namespace
}  // namespace proj
