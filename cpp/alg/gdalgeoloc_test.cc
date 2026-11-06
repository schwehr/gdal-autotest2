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
// Tests gdalgeoloc (alg/gdalgeoloc.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgeoloc.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalgeoloc.h

#include "third_party/gdal/alg/gdalgeoloc.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern "C" {
CPLXMLNode* GDALSerializeGeoLocTransformer(void* pTransformArg);
void* GDALDeserializeGeoLocTransformer(CPLXMLNode* psTree);
}

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom RAII deleter for GeoLoc transformer handle.
struct GeoLocTransformerDeleter {
  void operator()(void* p) const {
    if (p != nullptr) {
      GDALDestroyGeoLocTransformer(p);
    }
  }
};
using GeoLocTransformerUniquePtr =
    std::unique_ptr<void, GeoLocTransformerDeleter>;

using XMLTreeUniquePtr = std::unique_ptr<CPLXMLNode, CPLXMLTreeCloserDeleter>;

class GdalGeoLocTest : public ::testing::Test {
 protected:
  static constexpr char kTmpDir[] = "/vsimem/gdalgeoloc_test_tmp";
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    VSIMkdir(kTmpDir, 0755);
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
  }
  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

GDALDatasetUniquePtr CreateSingleBandDatasetGTiff(
    const char* path, int width, int height,
    const std::vector<double>& coords = {}) {
  GDALRegister_GTiff();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!driver) {
    return nullptr;
  }
  GDALDatasetUniquePtr ds(
      driver->Create(path, width, height, 1, GDT_Float64, nullptr));
  if (!ds) {
    return nullptr;
  }
  if (!coords.empty()) {
    if (ds->GetRasterBand(1)->RasterIO(
            GF_Write, 0, 0, width, height, const_cast<double*>(coords.data()),
            width, height, GDT_Float64, 0, 0, nullptr) != CE_None) {
      return nullptr;
    }
  }
  return ds;
}

GDALDatasetUniquePtr CreateGeolocDatasetGTiff(
    const char* path, int width, int height,
    const std::vector<double>& x_coords, const std::vector<double>& y_coords,
    double no_data_value = -9999.0, bool set_no_data = false) {
  GDALRegister_GTiff();
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!driver) {
    return nullptr;
  }
  GDALDatasetUniquePtr ds(
      driver->Create(path, width, height, 2, GDT_Float64, nullptr));
  if (!ds) {
    return nullptr;
  }
  GDALRasterBand* band_x = ds->GetRasterBand(1);
  GDALRasterBand* band_y = ds->GetRasterBand(2);
  if (set_no_data) {
    band_x->SetNoDataValue(no_data_value);
    band_y->SetNoDataValue(no_data_value);
  }
  if (!x_coords.empty()) {
    if (band_x->RasterIO(GF_Write, 0, 0, width, height,
                         const_cast<double*>(x_coords.data()), width, height,
                         GDT_Float64, 0, 0, nullptr) != CE_None) {
      return nullptr;
    }
  }
  if (!y_coords.empty()) {
    if (band_y->RasterIO(GF_Write, 0, 0, width, height,
                         const_cast<double*>(y_coords.data()), width, height,
                         GDT_Float64, 0, 0, nullptr) != CE_None) {
      return nullptr;
    }
  }
  return ds;
}

// Creates an in-memory 2-band Float64 dataset for geolocation testing.
GDALDatasetUniquePtr CreateGeolocDatasetMEM(int width, int height,
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
                       GDT_Float64, 0, 0, nullptr) != CE_None) {
    return nullptr;
  }
  if (band_y->RasterIO(GF_Write, 0, 0, width, height,
                       const_cast<double*>(y_coords.data()), width, height,
                       GDT_Float64, 0, 0, nullptr) != CE_None) {
    return nullptr;
  }
  return ds;
}

// Creates a GeoLoc transformer with standard parameters.
GeoLocTransformerUniquePtr CreateTestTransformer(
    GDALDatasetH hBaseDS, const char* origin_convention = "TOP_LEFT_CORNER",
    bool swap_xy = false, bool normalize_long_180 = false,
    const char* use_temp_datasets = nullptr,
    const char* oversample_factor = nullptr, const char* srs_wkt = nullptr) {
  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
  geoloc_info.SetNameValue("LINE_OFFSET", "0");
  geoloc_info.SetNameValue("PIXEL_STEP", "1");
  geoloc_info.SetNameValue("LINE_STEP", "1");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "2");
  geoloc_info.SetNameValue("GEOREFERENCING_CONVENTION", origin_convention);
  geoloc_info.SetNameValue("SWAP_XY", swap_xy ? "YES" : "NO");
  if (srs_wkt != nullptr) {
    geoloc_info.SetNameValue("SRS", srs_wkt);
  }

  CPLStringList transform_options;
  if (normalize_long_180) {
    transform_options.SetNameValue(
        "GEOLOC_NORMALIZE_LONGITUDE_MINUS_180_PLUS_180", "YES");
  }
  if (use_temp_datasets != nullptr) {
    transform_options.SetNameValue("GEOLOC_USE_TEMP_DATASETS",
                                   use_temp_datasets);
  }
  if (oversample_factor != nullptr) {
    transform_options.SetNameValue("GEOLOC_BACKMAP_OVERSAMPLE_FACTOR",
                                   oversample_factor);
  }

  return GeoLocTransformerUniquePtr(GDALCreateGeoLocTransformerEx(
      hBaseDS, geoloc_info.List(), /*bReversed=*/FALSE, nullptr,
      transform_options.List()));
}

// ---------------------------------------------------------------------------
// 1. Inverse Bilinear Interpolation Direct Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, InverseBilinearInterpolationRectangle) {
  // Axis-aligned rectangle:
  // (x0, y0) = (10, 100), (x2, y2) = (20, 100)
  // (x1, y1) = (10, 200), (x3, y3) = (20, 200)
  // This exercises the denom == 0 rectangle/triangle branch.
  double i = 0.0;
  double j = 0.0;
  GDALInverseBilinearInterpolation(15.0, 150.0, 10.0, 100.0, 10.0, 200.0, 20.0,
                                   100.0, 20.0, 200.0, i, j);
  EXPECT_DOUBLE_EQ(0.5, i);
  EXPECT_DOUBLE_EQ(0.5, j);

  // Corner point (x0, y0)
  i = 0.0;
  j = 0.0;
  GDALInverseBilinearInterpolation(10.0, 100.0, 10.0, 100.0, 10.0, 200.0, 20.0,
                                   100.0, 20.0, 200.0, i, j);
  EXPECT_DOUBLE_EQ(0.0, i);
  EXPECT_DOUBLE_EQ(0.0, j);

  // Corner point (x3, y3)
  i = 0.0;
  j = 0.0;
  GDALInverseBilinearInterpolation(20.0, 200.0, 10.0, 100.0, 10.0, 200.0, 20.0,
                                   100.0, 20.0, 200.0, i, j);
  EXPECT_DOUBLE_EQ(1.0, i);
  EXPECT_DOUBLE_EQ(1.0, j);

  // Non-zero starting offsets
  i = 5.0;
  j = 10.0;
  GDALInverseBilinearInterpolation(15.0, 150.0, 10.0, 100.0, 10.0, 200.0, 20.0,
                                   100.0, 20.0, 200.0, i, j);
  EXPECT_DOUBLE_EQ(5.5, i);
  EXPECT_DOUBLE_EQ(10.5, j);
}

TEST_F(GdalGeoLocTest, InverseBilinearInterpolationGeneralQuad) {
  // General quadrilateral with non-zero denominator:
  // (x0, y0) = (0, 0), (x2, y2) = (10, 2)
  // (x1, y1) = (1, 10), (x3, y3) = (12, 11)
  const double x0 = 0.0;
  const double y0 = 0.0;
  const double x2 = 10.0;
  const double y2 = 2.0;
  const double x1 = 1.0;
  const double y1 = 10.0;
  const double x3 = 12.0;
  const double y3 = 11.0;

  // Choose s = 0.4, t = 0.6
  const double s = 0.4;
  const double t = 0.6;
  const double target_x =
      (1 - s) * (x0 + t * (x2 - x0)) + s * (x1 + t * (x3 - x1));
  const double target_y =
      (1 - s) * (y0 + t * (y2 - y0)) + s * (y1 + t * (y3 - y1));

  double i = 0.0;
  double j = 0.0;
  GDALInverseBilinearInterpolation(target_x, target_y, x0, y0, x1, y1, x2, y2,
                                   x3, y3, i, j);
  EXPECT_NEAR(t, i, 1e-9);
  EXPECT_NEAR(s, j, 1e-9);
}

TEST_F(GdalGeoLocTest, InverseBilinearInterpolationTDenomYFallback) {
  // Degenerate in X: x0 == x2 == 0, x1 == x3 == 10
  // t_denom_x is 0, so it falls back to t_denom_y.
  const double x0 = 0.0;
  const double y0 = 0.0;
  const double x2 = 0.0;
  const double y2 = 10.0;
  const double x1 = 10.0;
  const double y1 = 0.0;
  const double x3 = 10.0;
  const double y3 = 10.0;

  double i = 0.0;
  double j = 0.0;
  GDALInverseBilinearInterpolation(5.0, 5.0, x0, y0, x1, y1, x2, y2, x3, y3, i,
                                   j);
  EXPECT_DOUBLE_EQ(0.5, i);
  EXPECT_DOUBLE_EQ(0.5, j);
}

// ---------------------------------------------------------------------------
// 2. Square Extraction Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, ExtractSquareCArray) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  const auto* psInfo =
      static_cast<const GDALGeoLocTransformInfo*>(transformer.get());

  double x00 = 0;
  double y00 = 0;
  double x10 = 0;
  double y10 = 0;
  double x01 = 0;
  double y01 = 0;
  double x11 = 0;
  double y11 = 0;

  EXPECT_TRUE(GDALGeoLocExtractSquare(psInfo, 0, 0, x00, y00, x10, y10, x01,
                                      y01, x11, y11));
  EXPECT_DOUBLE_EQ(10.0, x00);
  EXPECT_DOUBLE_EQ(100.0, y00);
  EXPECT_DOUBLE_EQ(20.0, x10);
  EXPECT_DOUBLE_EQ(100.0, y10);
  EXPECT_DOUBLE_EQ(10.0, x01);
  EXPECT_DOUBLE_EQ(200.0, y01);
  EXPECT_DOUBLE_EQ(20.0, x11);
  EXPECT_DOUBLE_EQ(200.0, y11);

  // Extract cell (1, 1)
  EXPECT_TRUE(GDALGeoLocExtractSquare(psInfo, 1, 1, x00, y00, x10, y10, x01,
                                      y01, x11, y11));
  EXPECT_DOUBLE_EQ(20.0, x00);
  EXPECT_DOUBLE_EQ(200.0, y00);
  EXPECT_DOUBLE_EQ(30.0, x10);
  EXPECT_DOUBLE_EQ(200.0, y10);
  EXPECT_DOUBLE_EQ(20.0, x01);
  EXPECT_DOUBLE_EQ(300.0, y01);
  EXPECT_DOUBLE_EQ(30.0, x11);
  EXPECT_DOUBLE_EQ(300.0, y11);
}

TEST_F(GdalGeoLocTest, ExtractSquareDatasetAccessors) {
  GDALRegister_GTiff();
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
      ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
      /*normalize_long_180=*/false, /*use_temp_datasets=*/"YES");
  ASSERT_NE(nullptr, transformer);

  const auto* psInfo =
      static_cast<const GDALGeoLocTransformInfo*>(transformer.get());
  EXPECT_FALSE(psInfo->bUseArray);

  double x00 = 0;
  double y00 = 0;
  double x10 = 0;
  double y10 = 0;
  double x01 = 0;
  double y01 = 0;
  double x11 = 0;
  double y11 = 0;
  EXPECT_TRUE(GDALGeoLocExtractSquare(psInfo, 0, 0, x00, y00, x10, y10, x01,
                                      y01, x11, y11));
  EXPECT_DOUBLE_EQ(10.0, x00);
  EXPECT_DOUBLE_EQ(100.0, y00);
  EXPECT_DOUBLE_EQ(20.0, x10);
  EXPECT_DOUBLE_EQ(100.0, y10);
}

TEST_F(GdalGeoLocTest, ExtractSquareWithNoDataFails) {
  const std::vector<double> x_coords = {10.0,    20.0, 30.0, 10.0,
                                        -9999.0, 30.0,  // (1, 1) is nodata
                                        10.0,    20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, -9999.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords,
                                                   /*no_data_value=*/-9999.0,
                                                   /*set_no_data=*/true);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  const auto* psInfo =
      static_cast<const GDALGeoLocTransformInfo*>(transformer.get());

  double x00 = 0;
  double y00 = 0;
  double x10 = 0;
  double y10 = 0;
  double x01 = 0;
  double y01 = 0;
  double x11 = 0;
  double y11 = 0;

  // Cell (0, 0) has corner (1, 1) which is nodata, so extraction must fail.
  EXPECT_FALSE(GDALGeoLocExtractSquare(psInfo, 0, 0, x00, y00, x10, y10, x01,
                                       y01, x11, y11));
}

// ---------------------------------------------------------------------------
// 3. Transformer Creation & Validation Error Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, CreateValidationMissingRequiredFields) {
  struct TestCase {
    std::string_view missing_key;
  };
  constexpr TestCase kCases[] = {
      {.missing_key = "PIXEL_OFFSET"}, {.missing_key = "LINE_OFFSET"},
      {.missing_key = "PIXEL_STEP"},   {.missing_key = "LINE_STEP"},
      {.missing_key = "X_BAND"},       {.missing_key = "Y_BAND"},
  };

  const std::vector<double> coords = {10.0, 20.0, 10.0, 20.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, coords, coords);
  ASSERT_NE(nullptr, ds);

  for (const auto& tc : kCases) {
    CPLStringList geoloc_info;
    if (tc.missing_key != "PIXEL_OFFSET") {
      geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    }
    if (tc.missing_key != "LINE_OFFSET") {
      geoloc_info.SetNameValue("LINE_OFFSET", "0");
    }
    if (tc.missing_key != "PIXEL_STEP") {
      geoloc_info.SetNameValue("PIXEL_STEP", "1");
    }
    if (tc.missing_key != "LINE_STEP") {
      geoloc_info.SetNameValue("LINE_STEP", "1");
    }
    if (tc.missing_key != "X_BAND") {
      geoloc_info.SetNameValue("X_BAND", "1");
    }
    if (tc.missing_key != "Y_BAND") {
      geoloc_info.SetNameValue("Y_BAND", "2");
    }

    WithQuietHandler quiet;
    CPLErrorReset();
    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
        ds.get(), geoloc_info.List(), /*bReversed=*/FALSE));
    EXPECT_EQ(nullptr, transformer);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Missing some geolocation fields"));
    CPLErrorReset();
  }
}

TEST_F(GdalGeoLocTest, CreateValidationDatasetNotFound) {
  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("X_DATASET",
                           "/vsimem/gdalgeoloc_test/not_found_x.tif");
  geoloc_info.SetNameValue("Y_DATASET",
                           "/vsimem/gdalgeoloc_test/not_found_y.tif");
  geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
  geoloc_info.SetNameValue("LINE_OFFSET", "0");
  geoloc_info.SetNameValue("PIXEL_STEP", "1");
  geoloc_info.SetNameValue("LINE_STEP", "1");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "2");

  WithQuietHandler quiet;
  CPLErrorReset();
  GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
      nullptr, geoloc_info.List(), /*bReversed=*/FALSE));
  EXPECT_EQ(nullptr, transformer);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalGeoLocTest, CreateValidationRelativeToSource) {
  GDALRegister_GTiff();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/rel_src";
  constexpr char kGeolocFile[] =
      "/vsimem/gdalgeoloc_test/rel_src/geoloc_arr.tif";
  constexpr char kSourcePath[] = "/vsimem/gdalgeoloc_test/rel_src/source.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(kGeolocFile, 2, 2, 2, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds);
    double vals_x[4] = {10.0, 20.0, 10.0, 20.0};
    double vals_y[4] = {100.0, 100.0, 200.0, 200.0};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, vals_x, 2, 2,
                                             GDT_Float64, 0, 0));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 2, 2, vals_y, 2, 2,
                                             GDT_Float64, 0, 0));
  }

  // 1. Valid relative path with pszSourceDataset
  {
    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("X_DATASET", "geoloc_arr.tif");
    geoloc_info.SetNameValue("Y_DATASET", "geoloc_arr.tif");
    geoloc_info.SetNameValue("X_DATASET_RELATIVE_TO_SOURCE", "YES");
    geoloc_info.SetNameValue("Y_DATASET_RELATIVE_TO_SOURCE", "YES");
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "2");

    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformerEx(
        nullptr, geoloc_info.List(), FALSE, kSourcePath, nullptr));
    EXPECT_NE(nullptr, transformer);
  }

  // 2. Invalid relative path
  {
    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("X_DATASET", "non_existent.tif");
    geoloc_info.SetNameValue("Y_DATASET", "non_existent.tif");
    geoloc_info.SetNameValue("X_DATASET_RELATIVE_TO_SOURCE", "YES");
    geoloc_info.SetNameValue("Y_DATASET_RELATIVE_TO_SOURCE", "YES");
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "2");

    WithQuietHandler quiet;
    CPLErrorReset();
    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformerEx(
        nullptr, geoloc_info.List(), FALSE, kSourcePath, nullptr));
    EXPECT_EQ(nullptr, transformer);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalGeoLocTest, CreateValidationInvalidBands) {
  const std::vector<double> coords = {10.0, 20.0, 10.0, 20.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, coords, coords);
  ASSERT_NE(nullptr, ds);

  // X_BAND = 99
  {
    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "99");
    geoloc_info.SetNameValue("Y_BAND", "2");

    WithQuietHandler quiet;
    CPLErrorReset();
    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
        ds.get(), geoloc_info.List(), /*bReversed=*/FALSE));
    EXPECT_EQ(nullptr, transformer);
    CPLErrorReset();
  }

  // Y_BAND = 99
  {
    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "99");

    WithQuietHandler quiet;
    CPLErrorReset();
    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
        ds.get(), geoloc_info.List(), /*bReversed=*/FALSE));
    EXPECT_EQ(nullptr, transformer);
    CPLErrorReset();
  }
}

TEST_F(GdalGeoLocTest, CreateValidationDimensionMismatchRegularGrid) {
  const std::string dir = "/vsimem/gdalgeoloc_test/mismatch_reg";
  const std::string x_path = dir + "/x.tif";
  const std::string y_path = dir + "/y.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  // X band has height 1, Y band has height 2
  GDALDatasetUniquePtr ds_x =
      CreateSingleBandDatasetGTiff(x_path.c_str(), 4, 1);
  GDALDatasetUniquePtr ds_y =
      CreateSingleBandDatasetGTiff(y_path.c_str(), 4, 2);
  ASSERT_NE(nullptr, ds_x);
  ASSERT_NE(nullptr, ds_y);
  ds_x.reset();
  ds_y.reset();

  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
  geoloc_info.SetNameValue("LINE_OFFSET", "0");
  geoloc_info.SetNameValue("PIXEL_STEP", "1");
  geoloc_info.SetNameValue("LINE_STEP", "1");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "1");
  geoloc_info.SetNameValue("X_DATASET", x_path.c_str());
  geoloc_info.SetNameValue("Y_DATASET", y_path.c_str());

  WithQuietHandler quiet;
  CPLErrorReset();
  GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
      nullptr, geoloc_info.List(), /*bReversed=*/FALSE));
  EXPECT_EQ(nullptr, transformer);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("X_BAND and Y_BAND should have both nYSize == 1"));
  CPLErrorReset();
}

TEST_F(GdalGeoLocTest, CreateValidationDimensionMismatch2D) {
  const std::string dir = "/vsimem/gdalgeoloc_test/mismatch_2d";
  const std::string x_path = dir + "/x.tif";
  const std::string y_path = dir + "/y.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  // Width mismatch (3x4 vs 4x4)
  {
    GDALDatasetUniquePtr ds_x =
        CreateSingleBandDatasetGTiff(x_path.c_str(), 3, 4);
    GDALDatasetUniquePtr ds_y =
        CreateSingleBandDatasetGTiff(y_path.c_str(), 4, 4);
    ASSERT_NE(nullptr, ds_x);
    ASSERT_NE(nullptr, ds_y);
    ds_x.reset();
    ds_y.reset();

    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "1");
    geoloc_info.SetNameValue("X_DATASET", x_path.c_str());
    geoloc_info.SetNameValue("Y_DATASET", y_path.c_str());

    WithQuietHandler quiet;
    CPLErrorReset();
    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
        nullptr, geoloc_info.List(), /*bReversed=*/FALSE));
    EXPECT_EQ(nullptr, transformer);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("do not have the same dimensions"));
    CPLErrorReset();
  }

  // Height mismatch (3x4 vs 3x5)
  {
    GDALDatasetUniquePtr ds_x =
        CreateSingleBandDatasetGTiff(x_path.c_str(), 3, 4);
    GDALDatasetUniquePtr ds_y =
        CreateSingleBandDatasetGTiff(y_path.c_str(), 3, 5);
    ASSERT_NE(nullptr, ds_x);
    ASSERT_NE(nullptr, ds_y);
    ds_x.reset();
    ds_y.reset();

    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "1");
    geoloc_info.SetNameValue("X_DATASET", x_path.c_str());
    geoloc_info.SetNameValue("Y_DATASET", y_path.c_str());

    WithQuietHandler quiet;
    CPLErrorReset();
    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
        nullptr, geoloc_info.List(), /*bReversed=*/FALSE));
    EXPECT_EQ(nullptr, transformer);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("do not have the same dimensions"));
    CPLErrorReset();
  }
}

TEST_F(GdalGeoLocTest, CreateBaseDatasetLargerWarning) {
  const std::string dir = "/vsimem/gdalgeoloc_test/larger_warning";
  const std::string base_path = dir + "/base.tif";
  const std::string geoloc_path = dir + "/geoloc.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  // Base dataset is 20x20
  GDALDatasetUniquePtr base_ds =
      CreateSingleBandDatasetGTiff(base_path.c_str(), 20, 20);
  ASSERT_NE(nullptr, base_ds);

  // Set 3x3 geoloc values
  std::vector<double> vals_x = {10.0, 20.0, 30.0, 10.0, 20.0,
                                30.0, 10.0, 20.0, 30.0};
  std::vector<double> vals_y = {100.0, 100.0, 100.0, 200.0, 200.0,
                                200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr geoloc_ds =
      CreateGeolocDatasetGTiff(geoloc_path.c_str(), 3, 3, vals_x, vals_y);
  ASSERT_NE(nullptr, geoloc_ds);
  geoloc_ds.reset();

  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
  geoloc_info.SetNameValue("LINE_OFFSET", "0");
  geoloc_info.SetNameValue("PIXEL_STEP", "1");
  geoloc_info.SetNameValue("LINE_STEP", "1");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "2");
  geoloc_info.SetNameValue("X_DATASET", geoloc_path.c_str());
  geoloc_info.SetNameValue("Y_DATASET", geoloc_path.c_str());

  WithQuietHandler quiet;
  CPLErrorReset();
  GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
      base_ds.get(), geoloc_info.List(), /*bReversed=*/FALSE));
  EXPECT_NE(nullptr, transformer);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("whereas dataset is 20 x 20"));
  CPLErrorReset();
}

TEST_F(GdalGeoLocTest, CreateZeroAreaBackmapFails) {
  // All X coordinates are identical -> zero area, dfPixelSizeSquare == 0.0
  const std::vector<double> x_coords = {10.0, 10.0, 10.0, 10.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  WithQuietHandler quiet;
  CPLErrorReset();
  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  EXPECT_EQ(nullptr, transformer);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Invalid pixel size for backmap"));
  CPLErrorReset();
}

// ---------------------------------------------------------------------------
// 4. Forward Transformation Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, ForwardTransformTopLeftCornerNominal) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  double x[3] = {0.0, 1.0, 0.5};
  double y[3] = {0.0, 1.0, 0.5};
  int success[3] = {FALSE, FALSE, FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 3, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);
  EXPECT_EQ(TRUE, success[2]);

  EXPECT_DOUBLE_EQ(10.0, x[0]);
  EXPECT_DOUBLE_EQ(100.0, y[0]);
  EXPECT_DOUBLE_EQ(20.0, x[1]);
  EXPECT_DOUBLE_EQ(200.0, y[1]);
  EXPECT_DOUBLE_EQ(15.0, x[2]);
  EXPECT_DOUBLE_EQ(150.0, y[2]);
}

TEST_F(GdalGeoLocTest, ForwardTransformPixelCenter) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer =
      CreateTestTransformer(ds.get(), "PIXEL_CENTER");
  ASSERT_NE(nullptr, transformer);

  // In PIXEL_CENTER convention, offset is 0.5, so input (0.5, 0.5) maps to grid
  // node (0, 0)
  double x[2] = {0.5, 1.5};
  double y[2] = {0.5, 1.5};
  int success[2] = {FALSE, FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 2, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);

  EXPECT_DOUBLE_EQ(10.0, x[0]);
  EXPECT_DOUBLE_EQ(100.0, y[0]);
  EXPECT_DOUBLE_EQ(20.0, x[1]);
  EXPECT_DOUBLE_EQ(200.0, y[1]);
}

TEST_F(GdalGeoLocTest, ForwardTransformOffsetsAndSteps) {
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("PIXEL_OFFSET", "100");
  geoloc_info.SetNameValue("LINE_OFFSET", "200");
  geoloc_info.SetNameValue("PIXEL_STEP", "10");
  geoloc_info.SetNameValue("LINE_STEP", "20");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "2");

  GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
      ds.get(), geoloc_info.List(), /*bReversed=*/FALSE));
  ASSERT_NE(nullptr, transformer);

  // Pixel = (100 - 100) / 10 = 0, Line = (200 - 200) / 20 = 0 -> (10, 100)
  // Pixel = (110 - 100) / 10 = 1, Line = (220 - 200) / 20 = 1 -> (20, 200)
  double x[2] = {100.0, 110.0};
  double y[2] = {200.0, 220.0};
  int success[2] = {FALSE, FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 2, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);

  EXPECT_DOUBLE_EQ(10.0, x[0]);
  EXPECT_DOUBLE_EQ(100.0, y[0]);
  EXPECT_DOUBLE_EQ(20.0, x[1]);
  EXPECT_DOUBLE_EQ(200.0, y[1]);
}

TEST_F(GdalGeoLocTest, ForwardTransformSwapXY) {
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer =
      CreateTestTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/true);
  ASSERT_NE(nullptr, transformer);

  double x[1] = {0.0};
  double y[1] = {0.0};
  int success[1] = {FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  // With SWAP_XY, X becomes Y (100.0) and Y becomes X (10.0)
  EXPECT_DOUBLE_EQ(100.0, x[0]);
  EXPECT_DOUBLE_EQ(10.0, y[0]);
}

TEST_F(GdalGeoLocTest, ForwardTransformHugeVal) {
  const std::vector<double> coords = {10.0, 20.0, 10.0, 20.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, coords, coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  double x[2] = {HUGE_VAL, 0.0};
  double y[2] = {0.0, HUGE_VAL};
  int success[2] = {TRUE, TRUE};

  EXPECT_EQ(FALSE, GDALGeoLocTransform(transformer.get(), FALSE, 2, x, y,
                                       nullptr, success));
  EXPECT_EQ(FALSE, success[0]);
  EXPECT_EQ(FALSE, success[1]);
}

TEST_F(GdalGeoLocTest, ForwardTransformExtrapolation) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // 1. Right edge extrapolation (pixel = 2.5) -> exercises iX-- retry
  // 2. Bottom edge extrapolation (line = 2.5) -> exercises iY-- retry
  // 3. Bottom-right corner extrapolation (pixel = 2.5, line = 2.5) -> exercises
  // iX--, iY-- retry
  // 4. Negative extrapolation (pixel = -0.5, line = -0.5)
  double x[4] = {2.5, 1.0, 2.5, -0.5};
  double y[4] = {1.0, 2.5, 2.5, -0.5};
  int success[4] = {FALSE, FALSE, FALSE, FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 4, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);
  EXPECT_EQ(TRUE, success[2]);
  EXPECT_EQ(TRUE, success[3]);

  EXPECT_DOUBLE_EQ(35.0, x[0]);
  EXPECT_DOUBLE_EQ(200.0, y[0]);
  EXPECT_DOUBLE_EQ(20.0, x[1]);
  EXPECT_DOUBLE_EQ(350.0, y[1]);
  EXPECT_DOUBLE_EQ(35.0, x[2]);
  EXPECT_DOUBLE_EQ(350.0, y[2]);
  EXPECT_DOUBLE_EQ(5.0, x[3]);
  EXPECT_DOUBLE_EQ(50.0, y[3]);
}

TEST_F(GdalGeoLocTest, ForwardTransform1DVerticalArray) {
  // 2D geoloc array with width 1 and height 3 (not a regular grid)
  const std::vector<double> x_coords = {10.0, 10.0, 10.0};
  const std::vector<double> y_coords = {100.0, 200.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(1, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  CPLConfigOptionSetter method_setter("GDAL_GEOLOC_INVERSE_METHOD", "QUADTREE",
                                      /*bSetOnlyIfUndefined=*/false);
  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // Line interpolation along Y (lines 503-518 in PixelLineToXY)
  double x[2] = {0.0, 0.0};
  double y[2] = {0.5, 2.5};
  int success[2] = {FALSE, FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 2, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);

  EXPECT_DOUBLE_EQ(10.0, x[0]);
  EXPECT_DOUBLE_EQ(150.0, y[0]);
  EXPECT_DOUBLE_EQ(10.0, x[1]);
  EXPECT_DOUBLE_EQ(300.0, y[1]);
}

TEST_F(GdalGeoLocTest, ForwardTransformNoData) {
  const std::vector<double> x_coords = {10.0,    20.0, 30.0, 10.0,
                                        -9999.0, 30.0,  // (1, 1) is nodata
                                        10.0,    20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, -9999.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords,
                                                   /*no_data_value=*/-9999.0,
                                                   /*set_no_data=*/true);
  ASSERT_NE(nullptr, ds);

  CPLConfigOptionSetter method_setter("GDAL_GEOLOC_INVERSE_METHOD", "QUADTREE",
                                      /*bSetOnlyIfUndefined=*/false);
  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // Point directly on nodata node fails
  double x[1] = {1.0};
  double y[1] = {1.0};
  int success[1] = {TRUE};

  EXPECT_EQ(FALSE, GDALGeoLocTransform(transformer.get(), FALSE, 1, x, y,
                                       nullptr, success));
  EXPECT_EQ(FALSE, success[0]);
  EXPECT_EQ(HUGE_VAL, x[0]);
  EXPECT_EQ(HUGE_VAL, y[0]);
}

TEST_F(GdalGeoLocTest, ForwardTransformAntiMeridian) {
  // Points spanning the anti-meridian in geographic SRS
  const std::vector<double> x_coords = {179.0, -179.0, 179.0, -179.0};
  const std::vector<double> y_coords = {10.0, 10.0, 20.0, 20.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
      ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
      /*normalize_long_180=*/true, /*use_temp_datasets=*/nullptr,
      /*oversample_factor=*/nullptr, SRS_WKT_WGS84_LAT_LONG);
  ASSERT_NE(nullptr, transformer);

  // Midpoint x = 0.5 should interpolate to 180 (or -180) degrees longitude
  double x[1] = {0.5};
  double y[1] = {0.5};
  int success[1] = {FALSE};

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, x, y,
                                      nullptr, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_DOUBLE_EQ(180.0, std::fabs(x[0]));
  EXPECT_DOUBLE_EQ(15.0, y[0]);
}

TEST_F(GdalGeoLocTest, ForwardTransformMinus180Plus180AutoDetection) {
  // normalize_long_180 is false, but SRS is geographic WGS84, triggering
  // auto-detection of bGeographicSRSWithMinus180Plus180LongRange (L201-209).
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {10.0, 10.0, 20.0, 20.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
      ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
      /*normalize_long_180=*/false, /*use_temp_datasets=*/nullptr,
      /*oversample_factor=*/nullptr, SRS_WKT_WGS84_LAT_LONG);
  ASSERT_NE(nullptr, transformer);
  EXPECT_TRUE(static_cast<const GDALGeoLocTransformInfo*>(transformer.get())
                  ->bGeographicSRSWithMinus180Plus180LongRange);
}

// ---------------------------------------------------------------------------
// 5. Inverse Transformation Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, InverseTransformBackMapNominal) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // Forward then inverse round-trip
  double orig_x = 0.8;
  double orig_y = 1.3;
  double x = orig_x;
  double y = orig_y;
  int success = FALSE;

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);

  success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(orig_x, x, 1e-4);
  EXPECT_NEAR(orig_y, y, 1e-4);
}

TEST_F(GdalGeoLocTest, InverseTransformMaxAccuracyNo) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  CPLConfigOptionSetter setter("GDAL_GEOLOC_USE_MAX_ACCURACY", "NO",
                               /*bSetOnlyIfUndefined=*/false);

  double x = 20.0;
  double y = 200.0;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1.0, x, 0.1);
  EXPECT_NEAR(1.0, y, 0.1);
}

TEST_F(GdalGeoLocTest, InverseTransformOutOfBounds) {
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // Far outside bounding box
  double x[1] = {1000.0};
  double y[1] = {1000.0};
  int success[1] = {TRUE};

  EXPECT_EQ(FALSE, GDALGeoLocTransform(transformer.get(), TRUE, 1, x, y,
                                       nullptr, success));
  EXPECT_EQ(FALSE, success[0]);
  EXPECT_EQ(HUGE_VAL, x[0]);
  EXPECT_EQ(HUGE_VAL, y[0]);
}

TEST_F(GdalGeoLocTest, InverseTransformHugeVal) {
  const std::vector<double> coords = {10.0, 20.0, 10.0, 20.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, coords, coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  double x[1] = {HUGE_VAL};
  double y[1] = {100.0};
  int success[1] = {TRUE};

  EXPECT_EQ(FALSE, GDALGeoLocTransform(transformer.get(), TRUE, 1, x, y,
                                       nullptr, success));
  EXPECT_EQ(FALSE, success[0]);
}

TEST_F(GdalGeoLocTest, InverseTransformSwapXY) {
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer =
      CreateTestTransformer(ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/true);
  ASSERT_NE(nullptr, transformer);

  // In SWAP_XY mode, input to inverse transform has (Y, X)
  double orig_px = 0.5;
  double orig_py = 0.5;
  double x = orig_px;
  double y = orig_py;
  int success = FALSE;

  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);

  success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(orig_px, x, 1e-3);
  EXPECT_NEAR(orig_py, y, 1e-3);
}

TEST_F(GdalGeoLocTest, InverseTransformPolarRegion) {
  // High-latitude geographic coordinates to trigger nSearchRadius = 5
  const std::vector<double> x_coords = {-10.0, 0.0,   10.0, -10.0, 0.0,
                                        10.0,  -10.0, 0.0,  10.0};
  const std::vector<double> y_coords = {86.0, 86.0, 86.0, 87.0, 87.0,
                                        87.0, 88.0, 88.0, 88.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
      ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
      /*normalize_long_180=*/true, /*use_temp_datasets=*/nullptr,
      /*oversample_factor=*/nullptr, SRS_WKT_WGS84_LAT_LONG);
  ASSERT_NE(nullptr, transformer);

  double x = 0.0;
  double y = 87.2;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1.0, x, 1e-3);
  EXPECT_NEAR(1.2, y, 1e-3);
}

TEST_F(GdalGeoLocTest, InverseTransformAntiMeridian) {
  // Grid spanning the anti-meridian
  const std::vector<double> x_coords = {178.0, 179.0, -179.0, -178.0,
                                        178.0, 179.0, -179.0, -178.0,
                                        178.0, 179.0, -179.0, -178.0};
  const std::vector<double> y_coords = {10.0, 10.0, 10.0, 10.0, 20.0, 20.0,
                                        20.0, 20.0, 30.0, 30.0, 30.0, 30.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(4, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
      ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
      /*normalize_long_180=*/true, /*use_temp_datasets=*/nullptr,
      /*oversample_factor=*/nullptr, SRS_WKT_WGS84_LAT_LONG);
  ASSERT_NE(nullptr, transformer);

  // Invert a point at longitude 179.5 (unwrapped step between 179 and -179 is
  // 2 degrees wide, so pixel is 1 + 0.5 / 2.0 = 1.25)
  double x = 179.5;
  double y = 20.0;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1.25, x, 1e-2);
  EXPECT_NEAR(1.0, y, 1e-3);
}

TEST_F(GdalGeoLocTest, InverseTransformOversampleFactors) {
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  // 1. Oversample factor 0.5
  {
    GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
        ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
        /*normalize_long_180=*/false, /*use_temp_datasets=*/nullptr,
        /*oversample_factor=*/"0.5");
    ASSERT_NE(nullptr, transformer);

    double x = 15.0;
    double y = 150.0;
    int success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_NEAR(0.5, x, 1e-3);
    EXPECT_NEAR(0.5, y, 1e-3);
  }

  // 2. Oversample factor 1.8
  {
    GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
        ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
        /*normalize_long_180=*/false, /*use_temp_datasets=*/nullptr,
        /*oversample_factor=*/"1.8");
    ASSERT_NE(nullptr, transformer);

    double x = 15.0;
    double y = 150.0;
    int success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_NEAR(0.5, x, 1e-3);
    EXPECT_NEAR(0.5, y, 1e-3);
  }
}

TEST_F(GdalGeoLocTest, InverseTransformQuadtreeMethod) {
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  CPLConfigOptionSetter setter("GDAL_GEOLOC_INVERSE_METHOD", "QUADTREE",
                               /*bSetOnlyIfUndefined=*/false);
  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  const auto* psInfo =
      static_cast<const GDALGeoLocTransformInfo*>(transformer.get());
  EXPECT_NE(nullptr, psInfo->hQuadTree);

  // Round trip
  double x = 0.7;
  double y = 1.4;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);

  success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(0.7, x, 1e-3);
  EXPECT_NEAR(1.4, y, 1e-3);
}

// ---------------------------------------------------------------------------
// 6. Dataset Accessors & Regular Grid Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, DatasetAccessors2DGrid) {
  GDALRegister_GTiff();
  const std::vector<double> x_coords = {10.0, 20.0, 30.0, 10.0, 20.0,
                                        30.0, 10.0, 20.0, 30.0};
  const std::vector<double> y_coords = {100.0, 100.0, 100.0, 200.0, 200.0,
                                        200.0, 300.0, 300.0, 300.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(3, 3, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(
      ds.get(), "TOP_LEFT_CORNER", /*swap_xy=*/false,
      /*normalize_long_180=*/false, /*use_temp_datasets=*/"YES");
  ASSERT_NE(nullptr, transformer);

  double x = 1.0;
  double y = 1.0;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_DOUBLE_EQ(20.0, x);
  EXPECT_DOUBLE_EQ(200.0, y);

  x = 20.0;
  y = 200.0;
  success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1.0, x, 1e-3);
  EXPECT_NEAR(1.0, y, 1e-3);
}

TEST_F(GdalGeoLocTest, RegularGridCArrayAndDatasetAccessors) {
  GDALRegister_GTiff();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/reg_grid";
  constexpr char kXDataset[] = "/vsimem/gdalgeoloc_test/reg_grid/x.tif";
  constexpr char kYDataset[] = "/vsimem/gdalgeoloc_test/reg_grid/y.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // X band is 4x1, Y band is 3x1
  {
    GDALDatasetUniquePtr ds_x(
        gtiff_driver->Create(kXDataset, 4, 1, 1, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds_x);
    double x_vals[4] = {10.0, 20.0, 30.0, 40.0};
    EXPECT_EQ(CE_None,
              ds_x->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 1, x_vals, 4,
                                               1, GDT_Float64, 0, 0));

    GDALDatasetUniquePtr ds_y(
        gtiff_driver->Create(kYDataset, 3, 1, 1, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds_y);
    double y_vals[3] = {100.0, 200.0, 300.0};
    EXPECT_EQ(CE_None,
              ds_y->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 3, 1, y_vals, 3,
                                               1, GDT_Float64, 0, 0));
  }

  // 1. Regular grid with default CArray
  {
    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("X_DATASET", kXDataset);
    geoloc_info.SetNameValue("Y_DATASET", kYDataset);
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "1");

    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
        nullptr, geoloc_info.List(), /*bReversed=*/FALSE));
    ASSERT_NE(nullptr, transformer);

    double x = 2.0;
    double y = 1.0;
    int success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_DOUBLE_EQ(30.0, x);
    EXPECT_DOUBLE_EQ(200.0, y);

    x = 30.0;
    y = 200.0;
    success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_NEAR(2.0, x, 1e-3);
    EXPECT_NEAR(1.0, y, 1e-3);
  }

  // 2. Regular grid with DatasetAccessors
  {
    CPLStringList geoloc_info;
    geoloc_info.SetNameValue("X_DATASET", kXDataset);
    geoloc_info.SetNameValue("Y_DATASET", kYDataset);
    geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
    geoloc_info.SetNameValue("LINE_OFFSET", "0");
    geoloc_info.SetNameValue("PIXEL_STEP", "1");
    geoloc_info.SetNameValue("LINE_STEP", "1");
    geoloc_info.SetNameValue("X_BAND", "1");
    geoloc_info.SetNameValue("Y_BAND", "1");

    CPLStringList transform_options;
    transform_options.SetNameValue("GEOLOC_USE_TEMP_DATASETS", "YES");

    GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformerEx(
        nullptr, geoloc_info.List(), /*bReversed=*/FALSE, nullptr,
        transform_options.List()));
    ASSERT_NE(nullptr, transformer);

    double x = 2.0;
    double y = 1.0;
    int success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_DOUBLE_EQ(30.0, x);
    EXPECT_DOUBLE_EQ(200.0, y);

    x = 30.0;
    y = 200.0;
    success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_NEAR(2.0, x, 1e-3);
    EXPECT_NEAR(1.0, y, 1e-3);
  }
}

// ---------------------------------------------------------------------------
// 7. Modification, Cloning & Reversed Transformer Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, ReversedTransformer) {
  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds = CreateGeolocDatasetMEM(2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds);

  CPLStringList geoloc_info;
  geoloc_info.SetNameValue("PIXEL_OFFSET", "0");
  geoloc_info.SetNameValue("LINE_OFFSET", "0");
  geoloc_info.SetNameValue("PIXEL_STEP", "1");
  geoloc_info.SetNameValue("LINE_STEP", "1");
  geoloc_info.SetNameValue("X_BAND", "1");
  geoloc_info.SetNameValue("Y_BAND", "2");

  // Create with bReversed = TRUE
  GeoLocTransformerUniquePtr transformer(GDALCreateGeoLocTransformer(
      ds.get(), geoloc_info.List(), /*bReversed=*/TRUE));
  ASSERT_NE(nullptr, transformer);

  // Calling with bDstToSrc = FALSE actually performs inverse (geo -> pixel)
  double x = 15.0;
  double y = 150.0;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(0.5, x, 1e-3);
  EXPECT_NEAR(0.5, y, 1e-3);

  // Calling with bDstToSrc = TRUE actually performs forward (pixel -> geo)
  x = 0.5;
  y = 0.5;
  success = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), TRUE, 1, &x, &y,
                                      nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_DOUBLE_EQ(15.0, x);
  EXPECT_DOUBLE_EQ(150.0, y);
}

TEST_F(GdalGeoLocTest, CreateSimilarTransformer) {
  // nullptr input returns nullptr
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(nullptr, 1.0, 1.0));
    CPLErrorReset();
  }

  const std::string dir = "/vsimem/gdalgeoloc_test/similar";
  const std::string geoloc_path = dir + "/geoloc.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds_write =
      CreateGeolocDatasetGTiff(geoloc_path.c_str(), 2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds_write);
  ds_write.reset();

  GDALDatasetUniquePtr ds(GDALDataset::Open(geoloc_path.c_str(),
                                            GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // 1. Ratio (1.0, 1.0) creates identical transformer
  {
    GeoLocTransformerUniquePtr similar(
        GDALCreateSimilarTransformer(transformer.get(), 1.0, 1.0));
    ASSERT_NE(nullptr, similar);

    double x = 0.5;
    double y = 0.5;
    int success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(similar.get(), FALSE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_DOUBLE_EQ(15.0, x);
    EXPECT_DOUBLE_EQ(150.0, y);
  }

  // 2. Ratio (2.0, 3.0) rescales PIXEL_OFFSET/STEP and LINE_OFFSET/STEP
  {
    GeoLocTransformerUniquePtr similar(
        GDALCreateSimilarTransformer(transformer.get(), 2.0, 3.0));
    ASSERT_NE(nullptr, similar);

    // Rescaled steps: pixel step is 0.5, line step is 1/3
    // In GDALGeoLocTransform (bDstToSrc=FALSE, geo -> pixel):
    // dfGeoLocPixel = (x - PIXEL_OFFSET) / PIXEL_STEP = x / 0.5 = 2.0 * x.
    // For dfGeoLocPixel = 0.5, x = 0.25.
    // dfGeoLocLine = (y - LINE_OFFSET) / LINE_STEP = y / (1/3) = 3.0 * y.
    // For dfGeoLocLine = 0.5, y = 0.5 / 3.0.
    double x = 0.25;
    double y = 0.5 / 3.0;
    int success = FALSE;
    EXPECT_EQ(TRUE, GDALGeoLocTransform(similar.get(), FALSE, 1, &x, &y,
                                        nullptr, &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_DOUBLE_EQ(15.0, x);
    EXPECT_DOUBLE_EQ(150.0, y);
  }
}

// ---------------------------------------------------------------------------
// 8. Geolocation Metadata Synthesis Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, CreateGeolocationMetadataNonExistent) {
  WithQuietHandler quiet;
  CPLErrorReset();
  CPLStringList md = GDALCreateGeolocationMetadata(
      nullptr, "/vsimem/gdalgeoloc_test/non_existent.tif", true);
  EXPECT_TRUE(md.empty());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid dataset"));
  CPLErrorReset();
}

TEST_F(GdalGeoLocTest, CreateGeolocationMetadataInvalidBands) {
  GDALRegister_GTiff();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/meta_err";
  constexpr char kFile[] = "/vsimem/gdalgeoloc_test/meta_err/one_band.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // 1-band dataset without explicit X_BAND/Y_BAND fails
  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(kFile, 4, 4, 1, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  WithQuietHandler quiet;
  CPLErrorReset();
  CPLStringList md = GDALCreateGeolocationMetadata(nullptr, kFile, true);
  EXPECT_TRUE(md.empty());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Expected 2 bands"));
  CPLErrorReset();
}

TEST_F(GdalGeoLocTest, CreateGeolocationMetadataNominal) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/meta_nom";
  constexpr char kGeolocFile[] = "/vsimem/gdalgeoloc_test/meta_nom/geoloc.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(kGeolocFile, 10, 20, 2, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRSpatialReference srs;
    srs.importFromEPSG(4269);
    ds->SetSpatialRef(&srs);
    ds->SetMetadataItem("GEOREFERENCING_CONVENTION", "PIXEL_CENTER");
  }

  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  GDALDatasetUniquePtr base_ds(
      mem_driver->Create("", 100, 200, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, base_ds);

  // Call with bIsSource = true
  {
    CPLStringList md =
        GDALCreateGeolocationMetadata(base_ds.get(), kGeolocFile, true);
    EXPECT_STREQ(kGeolocFile, md.FetchNameValue("X_DATASET"));
    EXPECT_STREQ(kGeolocFile, md.FetchNameValue("Y_DATASET"));
    EXPECT_STREQ("1", md.FetchNameValue("X_BAND"));
    EXPECT_STREQ("2", md.FetchNameValue("Y_BAND"));
    EXPECT_STREQ("0", md.FetchNameValue("PIXEL_OFFSET"));
    EXPECT_STREQ("0", md.FetchNameValue("LINE_OFFSET"));
    EXPECT_STREQ("10", md.FetchNameValue("PIXEL_STEP"));
    EXPECT_STREQ("10", md.FetchNameValue("LINE_STEP"));
    EXPECT_STREQ("PIXEL_CENTER",
                 md.FetchNameValue("GEOREFERENCING_CONVENTION"));
    EXPECT_NE(nullptr, md.FetchNameValue("SRS"));
    EXPECT_THAT(md.FetchNameValue("SRS"), ::testing::HasSubstr("NAD83"));
  }

  // Call with bIsSource = false (exercises the target branch in CPLDebug)
  {
    CPLStringList md =
        GDALCreateGeolocationMetadata(base_ds.get(), kGeolocFile, false);
    EXPECT_FALSE(md.empty());
  }
}

TEST_F(GdalGeoLocTest, CreateGeolocationMetadataPreservesExisting) {
  GDALRegister_GTiff();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/meta_pres";
  constexpr char kGeolocFile[] = "/vsimem/gdalgeoloc_test/meta_pres/geoloc.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(kGeolocFile, 5, 5, 2, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds);
    ds->SetMetadataItem("PIXEL_OFFSET", "7", "GEOLOCATION");
    ds->SetMetadataItem("LINE_OFFSET", "8", "GEOLOCATION");
    ds->SetMetadataItem("PIXEL_STEP", "3", "GEOLOCATION");
    ds->SetMetadataItem("LINE_STEP", "4", "GEOLOCATION");
    ds->SetMetadataItem("X_BAND", "2", "GEOLOCATION");
    ds->SetMetadataItem("Y_BAND", "1", "GEOLOCATION");
  }

  CPLStringList md = GDALCreateGeolocationMetadata(nullptr, kGeolocFile, true);
  EXPECT_STREQ("7", md.FetchNameValue("PIXEL_OFFSET"));
  EXPECT_STREQ("8", md.FetchNameValue("LINE_OFFSET"));
  EXPECT_STREQ("3", md.FetchNameValue("PIXEL_STEP"));
  EXPECT_STREQ("4", md.FetchNameValue("LINE_STEP"));
  EXPECT_STREQ("2", md.FetchNameValue("X_BAND"));
  EXPECT_STREQ("1", md.FetchNameValue("Y_BAND"));
}

TEST_F(GdalGeoLocTest, CreateGeolocationMetadataDefaultWGS84) {
  GDALRegister_GTiff();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/meta_wgs84";
  constexpr char kGeolocFile[] =
      "/vsimem/gdalgeoloc_test/meta_wgs84/geoloc.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Dataset without SRS
  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(kGeolocFile, 2, 2, 2, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  CPLStringList md = GDALCreateGeolocationMetadata(nullptr, kGeolocFile, true);
  EXPECT_THAT(md.FetchNameValue("SRS"), HasSubstr("WGS 84"));
}

// ---------------------------------------------------------------------------
// 9. Serialization & Deserialization Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, SerializeDeserializeNullAndInvalid) {
  // Serialize nullptr returns nullptr
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALSerializeGeoLocTransformer(nullptr));
    CPLErrorReset();
  }

  // Deserialize nullptr returns nullptr
  EXPECT_EQ(nullptr, GDALDeserializeGeoLocTransformer(nullptr));

  // Invalid root element
  {
    XMLTreeUniquePtr tree(
        CPLCreateXMLNode(nullptr, CXT_Element, "InvalidTransformer"));
    EXPECT_EQ(nullptr, GDALDeserializeGeoLocTransformer(tree.get()));
  }

  // Tree without Metadata element
  {
    XMLTreeUniquePtr tree(
        CPLCreateXMLNode(nullptr, CXT_Element, "GeoLocTransformer"));
    CPLCreateXMLElementAndValue(tree.get(), "Reversed", "0");
    EXPECT_EQ(nullptr, GDALDeserializeGeoLocTransformer(tree.get()));
  }
}

TEST_F(GdalGeoLocTest, SerializeDeserializeRoundTrip) {
  const std::string dir = "/vsimem/gdalgeoloc_test/ser_rt";
  const std::string geoloc_path = dir + "/geoloc.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds_write =
      CreateGeolocDatasetGTiff(geoloc_path.c_str(), 2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds_write);
  ds_write.reset();

  GDALDatasetUniquePtr ds(GDALDataset::Open(geoloc_path.c_str(),
                                            GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // Serialize
  XMLTreeUniquePtr tree(GDALSerializeGeoLocTransformer(transformer.get()));
  ASSERT_NE(nullptr, tree);
  EXPECT_STREQ("GeoLocTransformer", tree->pszValue);
  EXPECT_STREQ("0", CPLGetXMLValue(tree.get(), "Reversed", ""));

  // Deserialize
  GeoLocTransformerUniquePtr deserialized(
      GDALDeserializeGeoLocTransformer(tree.get()));
  ASSERT_NE(nullptr, deserialized);

  // Both transformers produce identical results
  double x_orig = 0.5;
  double y_orig = 0.5;
  int success_orig = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(transformer.get(), FALSE, 1, &x_orig,
                                      &y_orig, nullptr, &success_orig));

  double x_deser = 0.5;
  double y_deser = 0.5;
  int success_deser = FALSE;
  EXPECT_EQ(TRUE, GDALGeoLocTransform(deserialized.get(), FALSE, 1, &x_deser,
                                      &y_deser, nullptr, &success_deser));

  EXPECT_EQ(TRUE, success_orig);
  EXPECT_EQ(TRUE, success_deser);
  EXPECT_DOUBLE_EQ(x_orig, x_deser);
  EXPECT_DOUBLE_EQ(y_orig, y_deser);
}

TEST_F(GdalGeoLocTest, GenericSerializeDeserializeRoundTrip) {
  const std::string dir = "/vsimem/gdalgeoloc_test/gen_ser_rt";
  const std::string geoloc_path = dir + "/geoloc.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  const std::vector<double> x_coords = {10.0, 20.0, 10.0, 20.0};
  const std::vector<double> y_coords = {100.0, 100.0, 200.0, 200.0};
  GDALDatasetUniquePtr ds_write =
      CreateGeolocDatasetGTiff(geoloc_path.c_str(), 2, 2, x_coords, y_coords);
  ASSERT_NE(nullptr, ds_write);
  ds_write.reset();

  GDALDatasetUniquePtr ds(GDALDataset::Open(geoloc_path.c_str(),
                                            GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);

  GeoLocTransformerUniquePtr transformer = CreateTestTransformer(ds.get());
  ASSERT_NE(nullptr, transformer);

  // Generic GDALSerializeTransformer
  XMLTreeUniquePtr tree(
      GDALSerializeTransformer(GDALGeoLocTransform, transformer.get()));
  ASSERT_NE(nullptr, tree);

  // Generic GDALDeserializeTransformer
  GDALTransformerFunc pfnFunc = nullptr;
  void* hDeserializedRaw = nullptr;
  EXPECT_EQ(CE_None, GDALDeserializeTransformer(tree.get(), &pfnFunc,
                                                &hDeserializedRaw));
  ASSERT_NE(nullptr, hDeserializedRaw);
  GeoLocTransformerUniquePtr deserialized(hDeserializedRaw);
  EXPECT_EQ(GDALGeoLocTransform, pfnFunc);

  double x = 0.6;
  double y = 0.4;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            pfnFunc(deserialized.get(), FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_DOUBLE_EQ(16.0, x);
  EXPECT_DOUBLE_EQ(140.0, y);
}

TEST_F(GdalGeoLocTest, DeserializeWithSourceDataset) {
  GDALRegister_GTiff();
  constexpr char kVsiDir[] = "/vsimem/gdalgeoloc_test/deser_src";
  constexpr char kGeolocFile[] = "/vsimem/gdalgeoloc_test/deser_src/geoloc.tif";
  constexpr char kSourcePath[] = "/vsimem/gdalgeoloc_test/deser_src/source.tif";
  absl::Cleanup cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(kGeolocFile, 2, 2, 2, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, ds);
    double vals_x[4] = {10.0, 20.0, 10.0, 20.0};
    double vals_y[4] = {100.0, 100.0, 200.0, 200.0};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, vals_x, 2, 2,
                                             GDT_Float64, 0, 0));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 2, 2, vals_y, 2, 2,
                                             GDT_Float64, 0, 0));
  }

  XMLTreeUniquePtr tree(
      CPLCreateXMLNode(nullptr, CXT_Element, "GeoLocTransformer"));
  CPLCreateXMLElementAndValue(tree.get(), "SourceDataset", kSourcePath);
  CPLCreateXMLElementAndValue(tree.get(), "Reversed", "0");
  CPLXMLNode* meta = CPLCreateXMLNode(tree.get(), CXT_Element, "Metadata");
  auto add_mdi = [meta](const char* key, const char* val) {
    CPLXMLNode* mdi = CPLCreateXMLNode(meta, CXT_Element, "MDI");
    CPLSetXMLValue(mdi, "#key", key);
    CPLCreateXMLNode(mdi, CXT_Text, val);
  };
  add_mdi("X_DATASET", "geoloc.tif");
  add_mdi("Y_DATASET", "geoloc.tif");
  add_mdi("X_DATASET_RELATIVE_TO_SOURCE", "YES");
  add_mdi("Y_DATASET_RELATIVE_TO_SOURCE", "YES");
  add_mdi("PIXEL_OFFSET", "0");
  add_mdi("LINE_OFFSET", "0");
  add_mdi("PIXEL_STEP", "1");
  add_mdi("LINE_STEP", "1");
  add_mdi("X_BAND", "1");
  add_mdi("Y_BAND", "2");

  GeoLocTransformerUniquePtr transformer(
      GDALDeserializeGeoLocTransformer(tree.get()));
  EXPECT_NE(nullptr, transformer);
}

// ---------------------------------------------------------------------------
// 10. Destruction Tests
// ---------------------------------------------------------------------------

TEST_F(GdalGeoLocTest, DestroyNullTransformer) {
  // Must safely no-op
  GDALDestroyGeoLocTransformer(nullptr);
  SUCCEED();
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Geolocation array back-search quadtree leaf boundary
// conditions.
