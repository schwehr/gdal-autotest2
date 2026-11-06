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
// Tests gdalgeolocquadtree (alg/gdalgeolocquadtree.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgeolocquadtree.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgeolocquadtree.h

#include "third_party/gdal/alg/gdalgeolocquadtree.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/alg/gdalgeoloc.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_quad_tree.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Creates an in-memory 2-band dataset for geolocation testing.
GDALDatasetUniquePtr CreateGeolocDataset(int width, int height,
                                         const std::vector<double>& x_coords,
                                         const std::vector<double>& y_coords,
                                         double no_data_value = -9999.0,
                                         bool set_no_data = false) {
  GDALRegister_MEM();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!driver) {
    return nullptr;
  }
  GDALDatasetUniquePtr ds(
      driver->Create("", width, height, 2, GDT_Float64, nullptr));
  if (!ds) {
    return nullptr;
  }
  GDALRasterBand* band_x = ds->GetRasterBand(1);
  GDALRasterBand* band_y = ds->GetRasterBand(2);
  if (set_no_data) {
    band_x->SetNoDataValue(no_data_value);
    band_y->SetNoDataValue(no_data_value);
  }
  if (band_x->RasterIO(GF_Write, 0, 0, width, height,
                       const_cast<double*>(x_coords.data()), width, height,
                       GDT_Float64, 0, 0) != CE_None) {
    return nullptr;
  }
  if (band_y->RasterIO(GF_Write, 0, 0, width, height,
                       const_cast<double*>(y_coords.data()), width, height,
                       GDT_Float64, 0, 0) != CE_None) {
    return nullptr;
  }
  return ds;
}

// Creates a geolocation transformer configured to use the quadtree method.
void* CreateGeolocTransformer(GDALDatasetH hBaseDS,
                              const char* origin_convention = "TOP_LEFT_CORNER",
                              bool swap_xy = false,
                              bool normalize_long_180 = false) {
  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
  geoloc_info.SetNameValue("LINE_OFFSET", "0");
  geoloc_info.SetNameValue("PIXEL_STEP", "1");
  geoloc_info.SetNameValue("LINE_STEP", "1");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "2");
  geoloc_info.SetNameValue("GEOREFERENCING_CONVENTION", origin_convention);
  geoloc_info.SetNameValue("SWAP_XY", swap_xy ? "YES" : "NO");

  CPLStringList transform_options;
  transform_options.SetNameValue(
      "GEOLOC_NORMALIZE_LONGITUDE_MINUS_180_PLUS_180",
      normalize_long_180 ? "YES" : "NO");

  CPLConfigOptionSetter setter("GDAL_GEOLOC_INVERSE_METHOD", "QUADTREE",
                               /*bSetOnlyIfUndefined=*/false);
  return GDALCreateGeoLocTransformerEx(hBaseDS, geoloc_info.List(),
                                       /*bReversed=*/FALSE, nullptr,
                                       transform_options.List());
}

TEST(GdalGeoLocQuadTreeTest, BuildQuadTreeOversizedArrayErrors) {
  // 1. Array with nGeoLocXSize > INT_MAX - nExtraPixel.
  GDALGeoLocTransformInfo info{};
  info.nGeoLocXSize = INT_MAX;
  info.nGeoLocYSize = 10;
  info.bOriginIsTopLeftCorner =
      false;  // nExtraPixel = 1, triggers INT_MAX check.

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GDALGeoLocBuildQuadTree(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too big geolocation array"));
    CPLErrorReset();
  }

  // 2. Array with nGeoLocYSize > INT_MAX - nExtraPixel.
  info.nGeoLocXSize = 10;
  info.nGeoLocYSize = INT_MAX;
  info.bOriginIsTopLeftCorner =
      false;  // nExtraPixel = 1, triggers YSize check.

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GDALGeoLocBuildQuadTree(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too big geolocation array"));
    CPLErrorReset();
  }
}

TEST(GdalGeoLocQuadTreeTest, TopLeftCornerConvention) {
  // 3x3 regular grid:
  // X: 10, 20, 30 across each row.
  // Y: 100, 200, 300 down each column.
  const std::vector<double> x_coords = {
      10.0, 20.0, 30.0, 10.0, 20.0, 30.0, 10.0, 20.0, 30.0,
  };
  const std::vector<double> y_coords = {
      100.0, 100.0, 100.0, 200.0, 200.0, 200.0, 300.0, 300.0, 300.0,
  };

  GDALDatasetUniquePtr ds = CreateGeolocDataset(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
                              /*normalize_long_180=*/false);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  EXPECT_TRUE(psTransform->bOriginIsTopLeftCorner);
  EXPECT_NE(nullptr, psTransform->hQuadTree);

  // Destroy and explicitly rebuild the quadtree with GDALGeoLocBuildQuadTree.
  CPLQuadTreeDestroy(psTransform->hQuadTree);
  psTransform->hQuadTree = nullptr;
  EXPECT_TRUE(GDALGeoLocBuildQuadTree(psTransform));
  EXPECT_NE(nullptr, psTransform->hQuadTree);

  // 1. Point inside cell: (15.0, 150.0) -> center of first cell (pixel 0.5,
  // line 0.5).
  double x = 15.0;
  double y = 150.0;
  int success = 0;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(0.5, x, 1e-5);
  EXPECT_NEAR(0.5, y, 1e-5);

  // 2. Point on cell boundary: (20.0, 150.0).
  x = 20.0;
  y = 150.0;
  success = 0;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1.0, x, 1e-5);
  EXPECT_NEAR(0.5, y, 1e-5);

  // 3. Point with HUGE_VAL on X.
  x = HUGE_VAL;
  y = 150.0;
  success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);

  // 4. Point with HUGE_VAL on Y.
  x = 15.0;
  y = HUGE_VAL;
  success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);

  // 5. Point completely outside quadtree bounds (500.0, 500.0).
  x = 500.0;
  y = 500.0;
  success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);
  EXPECT_EQ(HUGE_VAL, x);
  EXPECT_EQ(HUGE_VAL, y);

  // 6. Batch transformation of multiple points to verify ring/point reuse.
  std::vector<double> xs = {15.0, 25.0, HUGE_VAL, 999.0};
  std::vector<double> ys = {150.0, 250.0, 150.0, 999.0};
  std::vector<int> successes(4, 0);
  GDALGeoLocInverseTransformQuadtree(psTransform, 4, xs.data(), ys.data(),
                                     successes.data());
  EXPECT_EQ(TRUE, successes[0]);
  EXPECT_NEAR(0.5, xs[0], 1e-5);
  EXPECT_NEAR(0.5, ys[0], 1e-5);

  EXPECT_EQ(TRUE, successes[1]);
  EXPECT_NEAR(1.5, xs[1], 1e-5);
  EXPECT_NEAR(1.5, ys[1], 1e-5);

  EXPECT_EQ(FALSE, successes[2]);
  EXPECT_EQ(FALSE, successes[3]);
}

TEST(GdalGeoLocQuadTreeTest, PixelCenterConventionOffset) {
  // 3x3 regular grid:
  const std::vector<double> x_coords = {
      10.0, 20.0, 30.0, 10.0, 20.0, 30.0, 10.0, 20.0, 30.0,
  };
  const std::vector<double> y_coords = {
      100.0, 100.0, 100.0, 200.0, 200.0, 200.0, 300.0, 300.0, 300.0,
  };

  GDALDatasetUniquePtr ds = CreateGeolocDataset(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "PIXEL_CENTER", /*swap_xy=*/false,
                              /*normalize_long_180=*/false);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  EXPECT_FALSE(psTransform->bOriginIsTopLeftCorner);
  ASSERT_NE(nullptr, psTransform->hQuadTree);

  // Destroy and explicitly rebuild quadtree to verify pixel-center branch in
  // GDALGeoLocBuildQuadTree (nExtraPixel = 1).
  CPLQuadTreeDestroy(psTransform->hQuadTree);
  psTransform->hQuadTree = nullptr;
  EXPECT_TRUE(GDALGeoLocBuildQuadTree(psTransform));
  EXPECT_NE(nullptr, psTransform->hQuadTree);

  // For PIXEL_CENTER, dfGeorefConventionOffset = 0.5 is added to pixel/line.
  double x = 15.0;
  double y = 150.0;
  int success = 0;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1.0, x, 1e-5);
  EXPECT_NEAR(1.0, y, 1e-5);
}

TEST(GdalGeoLocQuadTreeTest, SwapXY) {
  const std::vector<double> x_coords = {
      10.0, 20.0, 30.0, 10.0, 20.0, 30.0, 10.0, 20.0, 30.0,
  };
  const std::vector<double> y_coords = {
      100.0, 100.0, 100.0, 200.0, 200.0, 200.0, 300.0, 300.0, 300.0,
  };

  GDALDatasetUniquePtr ds = CreateGeolocDataset(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/true,
                              /*normalize_long_180=*/false);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  EXPECT_TRUE(psTransform->bSwapXY);
  ASSERT_NE(nullptr, psTransform->hQuadTree);

  // In SwapXY mode, input padfX contains Y, and padfY contains X.
  double x = 150.0;
  double y = 15.0;
  int success = 0;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(0.5, x, 1e-5);
  EXPECT_NEAR(0.5, y, 1e-5);

  // HUGE_VAL check precedes swapping.
  x = HUGE_VAL;
  y = 15.0;
  success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);
}

TEST(GdalGeoLocQuadTreeTest, AntimeridianCrossing) {
  // 2x2 grid crossing the antimeridian (+/-180 longitude):
  // (0,0): lon=179.0, lat=10.0      (1,0): lon=-179.0, lat=10.0
  // (0,1): lon=179.0, lat=9.0       (1,1): lon=-179.0, lat=9.0
  const std::vector<double> x_coords = {
      179.0,
      -179.0,
      179.0,
      -179.0,
  };
  const std::vector<double> y_coords = {
      10.0,
      10.0,
      9.0,
      9.0,
  };

  GDALDatasetUniquePtr ds = CreateGeolocDataset(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
                              /*normalize_long_180=*/true);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  EXPECT_TRUE(psTransform->bGeographicSRSWithMinus180Plus180LongRange);

  // Extend globalBounds so both shifted versions (-181 to -179, and 179 to 181)
  // are fully contained within the quadtree's global bounds.
  psTransform->dfMinX = -190.0;
  psTransform->dfMaxX = 190.0;
  psTransform->dfMinY = 8.0;
  psTransform->dfMaxY = 11.0;

  CPLQuadTreeDestroy(psTransform->hQuadTree);
  psTransform->hQuadTree = nullptr;
  EXPECT_TRUE(GDALGeoLocBuildQuadTree(psTransform));
  ASSERT_NE(nullptr, psTransform->hQuadTree);

  // 1. Query near +180 deg (lon = 179.5, lat = 9.5).
  // Hits the feature inserted with BIT_IDX_RANGE_180_SET (bXRefAt180 = true).
  double x = 179.5;
  double y = 9.5;
  int success = 0;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(0.25, x, 1e-4);
  EXPECT_NEAR(0.5, y, 1e-4);

  // 2. Query near -180 deg (lon = -179.5, lat = 9.5).
  // Hits the base feature (bXRefAt180 = false).
  x = -179.5;
  y = 9.5;
  success = 0;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(0.75, x, 1e-4);
  EXPECT_NEAR(0.5, y, 1e-4);
}

TEST(GdalGeoLocQuadTreeTest, AntimeridianSkipTooLargeGeometries) {
  // When bGeographicSRSWithMinus180Plus180LongRange is true, geometries
  // spanning > 180 degrees where NOT all four corners have |x| > 170 are
  // skipped. Here: x0 = 175.0, x1 = -10.0, x2 = 175.0, x3 = -10.0. |x1 - x0| =
  // 185 > 180, but |x1| = 10 <= 170.
  const std::vector<double> x_coords = {
      175.0,
      -10.0,
      175.0,
      -10.0,
  };
  const std::vector<double> y_coords = {
      10.0,
      10.0,
      9.0,
      9.0,
  };

  GDALDatasetUniquePtr ds = CreateGeolocDataset(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
                              /*normalize_long_180=*/true);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  ASSERT_NE(nullptr, psTransform->hQuadTree);

  // Querying lon=170.0 lies strictly inside skipped cell (0, 0) [-10, 175]
  // and outside extrapolated cell (1, 0) [-10, 165], returning FALSE.
  double x = 170.0;
  double y = 9.5;
  int success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);
  EXPECT_EQ(HUGE_VAL, x);
  EXPECT_EQ(HUGE_VAL, y);
}

TEST(GdalGeoLocQuadTreeTest, PointInBBoxButNotInAnyCell) {
  // Create a triangular cell within a 2x2 grid where the cell bounding box
  // covers [0, 10] x [0, 10], but a point inside the bbox lies outside the cell
  // ring. Corners: (0, 0), (10, 0), (0, 10), (1, 1).
  const std::vector<double> x_coords = {
      0.0,
      10.0,
      0.0,
      1.0,
  };
  const std::vector<double> y_coords = {
      0.0,
      0.0,
      10.0,
      1.0,
  };

  GDALDatasetUniquePtr ds = CreateGeolocDataset(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
                              /*normalize_long_180=*/false);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  ASSERT_NE(nullptr, psTransform->hQuadTree);

  // Point (8.0, 8.0) is inside the bbox [0, 10] x [0, 10], so CPLQuadTreeSearch
  // returns the feature, but isPointInRing and isPointOnRingBoundary are false.
  double x = 8.0;
  double y = 8.0;
  int success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);
  EXPECT_EQ(HUGE_VAL, x);
  EXPECT_EQ(HUGE_VAL, y);
}

TEST(GdalGeoLocQuadTreeTest, NodataCellSkipped) {
  // 3x3 grid where one cell has a nodata value (-9999.0).
  const std::vector<double> x_coords = {
      10.0, 20.0, 30.0, 10.0, -9999.0, 30.0, 10.0, 20.0, 30.0,
  };
  const std::vector<double> y_coords = {
      100.0, 100.0, 100.0, 200.0, -9999.0, 200.0, 300.0, 300.0, 300.0,
  };

  GDALDatasetUniquePtr ds =
      CreateGeolocDataset(3, 3, x_coords, y_coords, /*no_data_value=*/-9999.0,
                          /*set_no_data=*/true);
  ASSERT_NE(nullptr, ds);

  void* hTransform =
      CreateGeolocTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
                              /*normalize_long_180=*/false);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGeoLocTransformer(hTransform);
  };

  auto* psTransform = static_cast<GDALGeoLocTransformInfo*>(hTransform);
  ASSERT_NE(nullptr, psTransform->hQuadTree);

  // Querying inside the nodata cell fails cleanly.
  double x = 20.0;
  double y = 200.0;
  int success = -1;
  GDALGeoLocInverseTransformQuadtree(psTransform, 1, &x, &y, &success);
  EXPECT_EQ(FALSE, success);
  EXPECT_EQ(HUGE_VAL, x);
  EXPECT_EQ(HUGE_VAL, y);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Geolocation quadtree non-convex mesh traversal.
