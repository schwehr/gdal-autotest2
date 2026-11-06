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
// Tests for GDALDEMProcessing (apps/gdaldem_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdaldem_lib.cpp
//   https://gdal.org/programs/gdaldem.html

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

struct GDALDEMProcessingOptionsDeleter {
  void operator()(GDALDEMProcessingOptions* psOptions) const {
    if (psOptions) {
      GDALDEMProcessingOptionsFree(psOptions);
    }
  }
};
using GDALDEMProcessingOptionsUniquePtr =
    std::unique_ptr<GDALDEMProcessingOptions, GDALDEMProcessingOptionsDeleter>;

GDALDEMProcessingOptionsUniquePtr MakeOptions(
    const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  return GDALDEMProcessingOptionsUniquePtr(
      GDALDEMProcessingOptionsNew(argv.data(), nullptr));
}

// Setup standard drivers once for all tests.
class GdaldemLibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }
};

// Helper to create synthetic test rasters in MEM.
GDALDatasetUniquePtr CreateMemDataset(int width, int height, GDALDataType type,
                                      const double* geo_transform,
                                      const char* wkt_srs,
                                      const std::vector<double>& values,
                                      double nodata_value, bool has_nodata) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!mem_driver) {
    return nullptr;
  }
  GDALDatasetUniquePtr ds(
      mem_driver->Create("", width, height, 1, type, nullptr));
  if (!ds) {
    return nullptr;
  }
  if (geo_transform) {
    ds->SetGeoTransform(const_cast<double*>(geo_transform));
  }
  if (wkt_srs && wkt_srs[0] != '\0') {
    OGRSpatialReference srs;
    srs.importFromWkt(wkt_srs);
    ds->SetSpatialRef(&srs);
  }
  GDALRasterBand* band = ds->GetRasterBand(1);
  if (has_nodata) {
    band->SetNoDataValue(nodata_value);
  }
  if (!values.empty()) {
    std::vector<double> buf = values;
    buf.resize(width * height, 0.0);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, width, height, buf.data(),
                                width, height, GDT_Float64, 0, 0, nullptr);
    if (err != CE_None) {
      return nullptr;
    }
  }
  return ds;
}

// -----------------------------------------------------------------------------
// Options parsing tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, OptionsBasicAndProgress) {
  auto options = MakeOptions({"hillshade", "-b", "1", "-compute_edges"});
  ASSERT_NE(nullptr, options);

  int counter = 0;
  GDALProgressFunc pfnProgress = [](double, const char*,
                                    void* pProgressData) -> int {
    if (pProgressData) {
      *static_cast<int*>(pProgressData) += 1;
    }
    return 1;
  };
  GDALDEMProcessingOptionsSetProgress(options.get(), pfnProgress, &counter);
}

TEST_F(GdaldemLibTest, OptionsHillshadeAllFlags) {
  auto options = MakeOptions({
      "hillshade",
      "-alg",
      "ZevenbergenThorne",
      "-z",
      "2.0",
      "-s",
      "1.5",
      "-az",
      "315.0",
      "-alt",
      "45.0",
      "-co",
      "TILED=YES",
      "-of",
      "GTiff",
  });
  EXPECT_NE(nullptr, options);

  // Hidden aliases
  auto options_alias = MakeOptions({
      "hillshade",
      "--b",
      "1",
      "--scale",
      "1.5",
      "--azimuth",
      "300.0",
      "--altitude",
      "40.0",
  });
  EXPECT_NE(nullptr, options_alias);

  // Horn algorithm
  auto options_horn = MakeOptions({"hillshade", "-alg", "Horn"});
  EXPECT_NE(nullptr, options_horn);

  // Shading variants
  auto options_combined = MakeOptions({"hillshade", "-combined"});
  EXPECT_NE(nullptr, options_combined);

  auto options_multi = MakeOptions({"hillshade", "-multidirectional"});
  EXPECT_NE(nullptr, options_multi);

  auto options_igor = MakeOptions({"hillshade", "-igor"});
  EXPECT_NE(nullptr, options_igor);

  // xscale and yscale
  auto options_xy =
      MakeOptions({"hillshade", "-xscale", "2.0", "-yscale", "3.0"});
  EXPECT_NE(nullptr, options_xy);
}

TEST_F(GdaldemLibTest, OptionsSlopeAllFlags) {
  auto options1 = MakeOptions({"slope", "-p", "-alg", "Horn", "-s", "2.0"});
  EXPECT_NE(nullptr, options1);

  auto options2 = MakeOptions({"slope", "-alg", "ZevenbergenThorne", "-xscale",
                               "1.5", "-yscale", "1.5"});
  EXPECT_NE(nullptr, options2);
}

TEST_F(GdaldemLibTest, OptionsAspectAllFlags) {
  auto options1 = MakeOptions(
      {"aspect", "-trigonometric", "-zero_for_flat", "-alg", "Horn"});
  EXPECT_NE(nullptr, options1);

  auto options2 = MakeOptions({"aspect", "-alg", "ZevenbergenThorne"});
  EXPECT_NE(nullptr, options2);
}

TEST_F(GdaldemLibTest, OptionsColorReliefAllFlags) {
  auto options1 = MakeOptions({"color-relief", "-alpha", "-exact_color_entry"});
  EXPECT_NE(nullptr, options1);

  auto options2 = MakeOptions({"color-relief", "-nearest_color_entry"});
  EXPECT_NE(nullptr, options2);
}

TEST_F(GdaldemLibTest, OptionsTRIAllFlags) {
  auto options1 = MakeOptions({"TRI", "-alg", "Wilson"});
  EXPECT_NE(nullptr, options1);

  auto options2 = MakeOptions({"TRI", "-alg", "Riley"});
  EXPECT_NE(nullptr, options2);
}

TEST_F(GdaldemLibTest, OptionsTPIAndRoughness) {
  auto options1 = MakeOptions({"TPI"});
  EXPECT_NE(nullptr, options1);

  auto options2 = MakeOptions({"roughness"});
  EXPECT_NE(nullptr, options2);
}

TEST_F(GdaldemLibTest, OptionsSwigHackFallback) {
  // Option list starting directly with '-' (no mode specified)
  auto options = MakeOptions({"-z", "2.0", "-compute_edges"});
  EXPECT_NE(nullptr, options);

  // Invalid arguments for any mode
  WithQuietHandler quiet;
  auto invalid = MakeOptions({"-invalid_arg_everywhere", "foo"});
  EXPECT_EQ(nullptr, invalid);
}

TEST_F(GdaldemLibTest, OptionsErrorHandling) {
  WithQuietHandler quiet;

  // Invalid alg
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-alg", "InvalidAlg"}));
  EXPECT_EQ(nullptr, MakeOptions({"slope", "-alg", "InvalidAlg"}));
  EXPECT_EQ(nullptr, MakeOptions({"aspect", "-alg", "InvalidAlg"}));
  EXPECT_EQ(nullptr, MakeOptions({"TRI", "-alg", "InvalidAlg"}));

  // Non-positive parameters
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-b", "0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-b", "-1"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-z", "0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-z", "-2"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-s", "0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-s", "-1"}));
  EXPECT_EQ(nullptr,
            MakeOptions({"hillshade", "-xscale", "0", "-yscale", "1"}));
  EXPECT_EQ(nullptr,
            MakeOptions({"hillshade", "-xscale", "1", "-yscale", "0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-alt", "0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-alt", "-10"}));

  // Mutually exclusive / missing pairs
  EXPECT_EQ(nullptr,
            MakeOptions({"hillshade", "-multidirectional", "-az", "315"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-igor", "-alt", "45"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-s", "2.0", "-xscale", "2.0",
                                  "-yscale", "2.0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-xscale", "2.0"}));
  EXPECT_EQ(nullptr, MakeOptions({"hillshade", "-yscale", "2.0"}));
}

TEST_F(GdaldemLibTest, ParserUsage) {
  EXPECT_FALSE(GDALDEMAppGetParserUsage("").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("hillshade").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("slope").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("aspect").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("color-relief").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("TRI").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("TPI").empty());
  EXPECT_FALSE(GDALDEMAppGetParserUsage("roughness").empty());

  WithQuietHandler quiet;
  EXPECT_FALSE(GDALDEMAppGetParserUsage("nonexistent_mode").empty());
}

// -----------------------------------------------------------------------------
// GDALDEMProcessing argument validation tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, ValidationErrors) {
  constexpr double gt[6] = {0, 1, 0, 0, 0, -1};
  auto src_ds =
      CreateMemDataset(4, 4, GDT_Float32, gt, nullptr, {}, -9999.0, true);
  ASSERT_NE(nullptr, src_ds);

  WithQuietHandler quiet;
  int usage_error = 0;

  // Null source dataset
  EXPECT_EQ(nullptr, GDALDEMProcessing("/vsimem/test.tif", nullptr, "hillshade",
                                       nullptr, nullptr, &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Null dest
  usage_error = 0;
  EXPECT_EQ(nullptr, GDALDEMProcessing(nullptr, src_ds.get(), "hillshade",
                                       nullptr, nullptr, &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Null processing
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), nullptr,
                              nullptr, nullptr, &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Invalid processing mode
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "invalid_mode",
                              nullptr, nullptr, &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Color relief without color file
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "color-relief",
                              nullptr, nullptr, &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Non-color relief with color file
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "hillshade",
                              "some_color.txt", nullptr, &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Combined with non-hillshade
  auto opt_combined = MakeOptions({"hillshade", "-combined"});
  ASSERT_NE(nullptr, opt_combined);
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "slope",
                              nullptr, opt_combined.get(), &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // Igor with non-hillshade
  auto opt_igor = MakeOptions({"hillshade", "-igor"});
  ASSERT_NE(nullptr, opt_igor);
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "slope",
                              nullptr, opt_igor.get(), &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // MultiDirectional with non-hillshade
  auto opt_multi = MakeOptions({"hillshade", "-multidirectional"});
  ASSERT_NE(nullptr, opt_multi);
  usage_error = 0;
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "slope",
                              nullptr, opt_multi.get(), &usage_error));
  EXPECT_EQ(TRUE, usage_error);

  // GradientAlg on TRI mode
  auto opt_gradient_tri = MakeOptions({"hillshade", "-alg", "Horn"});
  ASSERT_NE(nullptr, opt_gradient_tri);
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "TRI", nullptr,
                              opt_gradient_tri.get(), nullptr));

  // TRIAlg on hillshade mode
  auto opt_tri_hillshade = MakeOptions({"TRI", "-alg", "Wilson"});
  ASSERT_NE(nullptr, opt_tri_hillshade);
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "hillshade",
                              nullptr, opt_tri_hillshade.get(), nullptr));

  // Invalid band index (> 1 on 1-band dataset)
  auto opt_bad_band = MakeOptions({"hillshade", "-b", "2"});
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "hillshade",
                              nullptr, opt_bad_band.get(), nullptr));

  // Unknown driver format
  auto opt_bad_driver =
      MakeOptions({"hillshade", "-of", "NonExistentDriverName"});
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.tif", src_ds.get(), "hillshade",
                              nullptr, opt_bad_driver.get(), nullptr));

  // VRT output format on non-color-relief mode
  auto opt_vrt = MakeOptions({"hillshade", "-of", "VRT"});
  EXPECT_EQ(nullptr,
            GDALDEMProcessing("/vsimem/test.vrt", src_ds.get(), "hillshade",
                              nullptr, opt_vrt.get(), nullptr));
}

// -----------------------------------------------------------------------------
// Georeference and coordinate units handling tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, GeographicAndProjectedUnits) {
  constexpr double gt_square[6] = {0, 0.01, 0, 45.0, 0, -0.01};
  OGRSpatialReference geog_srs;
  geog_srs.SetWellKnownGeogCS("WGS84");
  char* geog_wkt = nullptr;
  geog_srs.exportToWkt(&geog_wkt);
  ASSERT_NE(nullptr, geog_wkt);
  absl::Cleanup free_geog_wkt = [geog_wkt] { CPLFree(geog_wkt); };

  auto opt_mem = MakeOptions({"hillshade", "-of", "MEM"});

  // 1. Geographic standard latitude (45 deg)
  auto ds_geog = CreateMemDataset(4, 4, GDT_Float32, gt_square, geog_wkt, {},
                                  -9999.0, false);
  ASSERT_NE(nullptr, ds_geog);
  GDALDatasetUniquePtr out_geog(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_geog.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_geog);

  // 2. Geographic high latitude (> 80 deg) -> triggers warning
  constexpr double gt_polar[6] = {0, 0.01, 0, 85.0, 0, -0.01};
  auto ds_polar = CreateMemDataset(4, 4, GDT_Float32, gt_polar, geog_wkt, {},
                                   -9999.0, false);
  ASSERT_NE(nullptr, ds_polar);
  {
    WithQuietHandler quiet;
    GDALDatasetUniquePtr out_polar(GDALDataset::FromHandle(GDALDEMProcessing(
        "", ds_polar.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
    EXPECT_NE(nullptr, out_polar);
  }

  // 3. Projected units: meters
  OGRSpatialReference proj_m;
  proj_m.importFromEPSG(32631);  // UTM 31N (meters)
  char* proj_m_wkt = nullptr;
  proj_m.exportToWkt(&proj_m_wkt);
  absl::Cleanup free_proj_m = [proj_m_wkt] { CPLFree(proj_m_wkt); };
  constexpr double gt_proj[6] = {500000, 10, 0, 4000000, 0, -10};
  auto ds_proj_m = CreateMemDataset(4, 4, GDT_Float32, gt_proj, proj_m_wkt, {},
                                    -9999.0, false);
  ASSERT_NE(nullptr, ds_proj_m);
  GDALDatasetUniquePtr out_proj_m(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_proj_m.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_proj_m);

  // 4. Projected units: US survey feet
  OGRSpatialReference proj_ft;
  proj_ft.importFromEPSG(2230);  // California zone 6 (ftUS)
  char* proj_ft_wkt = nullptr;
  proj_ft.exportToWkt(&proj_ft_wkt);
  absl::Cleanup free_proj_ft = [proj_ft_wkt] { CPLFree(proj_ft_wkt); };
  auto ds_proj_ft = CreateMemDataset(4, 4, GDT_Float32, gt_proj, proj_ft_wkt,
                                     {}, -9999.0, false);
  ASSERT_NE(nullptr, ds_proj_ft);
  GDALDatasetUniquePtr out_proj_ft(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_proj_ft.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_proj_ft);

  // 5. Projected units: international foot
  OGRSpatialReference proj_intl_ft;
  proj_intl_ft.importFromEPSG(32631);
  proj_intl_ft.SetLinearUnits("foot", 0.3048);
  char* proj_intl_ft_wkt = nullptr;
  proj_intl_ft.exportToWkt(&proj_intl_ft_wkt);
  absl::Cleanup free_intl_ft = [proj_intl_ft_wkt] {
    CPLFree(proj_intl_ft_wkt);
  };
  auto ds_proj_intl_ft = CreateMemDataset(4, 4, GDT_Float32, gt_proj,
                                          proj_intl_ft_wkt, {}, -9999.0, false);
  ASSERT_NE(nullptr, ds_proj_intl_ft);
  GDALDatasetUniquePtr out_intl_ft(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_proj_intl_ft.get(), "hillshade", nullptr,
                        opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_intl_ft);

  // 5b. Projected units: custom Foot_US
  OGRSpatialReference proj_foot_us;
  proj_foot_us.importFromEPSG(32631);
  proj_foot_us.SetLinearUnits("Foot_US", 12.0 / 39.37);
  char* proj_foot_us_wkt = nullptr;
  proj_foot_us.exportToWkt(&proj_foot_us_wkt);
  absl::Cleanup free_foot_us = [proj_foot_us_wkt] {
    CPLFree(proj_foot_us_wkt);
  };
  auto ds_proj_foot_us = CreateMemDataset(4, 4, GDT_Float32, gt_proj,
                                          proj_foot_us_wkt, {}, -9999.0, false);
  ASSERT_NE(nullptr, ds_proj_foot_us);
  GDALDatasetUniquePtr out_foot_us(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_proj_foot_us.get(), "hillshade", nullptr,
                        opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_foot_us);

  // 6. Projected units: unknown units -> triggers warning
  OGRSpatialReference proj_unknown;
  proj_unknown.importFromEPSG(32631);
  proj_unknown.SetLinearUnits("cubit", 0.5);
  char* proj_unknown_wkt = nullptr;
  proj_unknown.exportToWkt(&proj_unknown_wkt);
  absl::Cleanup free_unknown = [proj_unknown_wkt] {
    CPLFree(proj_unknown_wkt);
  };
  auto ds_unknown = CreateMemDataset(4, 4, GDT_Float32, gt_proj,
                                     proj_unknown_wkt, {}, -9999.0, false);
  ASSERT_NE(nullptr, ds_unknown);
  {
    WithQuietHandler quiet;
    GDALDatasetUniquePtr out_unknown(GDALDataset::FromHandle(GDALDEMProcessing(
        "", ds_unknown.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
    EXPECT_NE(nullptr, out_unknown);
  }

  // 7. No geotransform and no SRS
  auto ds_no_georef =
      CreateMemDataset(4, 4, GDT_Float32, nullptr, nullptr, {}, -9999.0, false);
  ASSERT_NE(nullptr, ds_no_georef);
  GDALDatasetUniquePtr out_no_georef(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_no_georef.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_no_georef);
}

// -----------------------------------------------------------------------------
// Hillshade processing tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, HillshadeAllModesAndDataTypes) {
  constexpr double gt_square[6] = {0, 10, 0, 100, 0, -10};
  constexpr double gt_anisotropic[6] = {0, 10, 0, 100, 0, -20};

  std::vector<double> dem_values(36);
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 6; ++x) {
      dem_values[y * 6 + x] = 100.0 + x * 20.0 - y * 10.0;
    }
  }

  // 1. Integer DEM (GDT_Int16) - Horn square res (same_res fast path)
  auto ds_int = CreateMemDataset(6, 6, GDT_Int16, gt_square, nullptr,
                                 dem_values, -9999.0, true);
  ASSERT_NE(nullptr, ds_int);

  auto opt_compute_edges =
      MakeOptions({"hillshade", "-of", "MEM", "-compute_edges"});
  GDALDatasetUniquePtr out_int_edges(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_int.get(), "hillshade", nullptr,
                        opt_compute_edges.get(), nullptr)));
  ASSERT_NE(nullptr, out_int_edges);
  EXPECT_EQ(1, out_int_edges->GetRasterCount());
  EXPECT_EQ(GDT_Byte, out_int_edges->GetRasterBand(1)->GetRasterDataType());

  // 2. Float DEM (GDT_Float32) - Horn square res
  auto ds_float = CreateMemDataset(6, 6, GDT_Float32, gt_square, nullptr,
                                   dem_values, -9999.0, true);
  ASSERT_NE(nullptr, ds_float);
  auto opt_mem = MakeOptions({"hillshade", "-of", "MEM"});
  GDALDatasetUniquePtr out_float(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  ASSERT_NE(nullptr, out_float);

  // 3. Float DEM with NaN nodata
  auto ds_float_nan =
      CreateMemDataset(6, 6, GDT_Float32, gt_square, nullptr, dem_values,
                       std::numeric_limits<double>::quiet_NaN(), true);
  ASSERT_NE(nullptr, ds_float_nan);
  GDALDatasetUniquePtr out_float_nan(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float_nan.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_float_nan);

  // 4. Anisotropic resolution (Horn general path)
  auto ds_aniso = CreateMemDataset(6, 6, GDT_Int16, gt_anisotropic, nullptr,
                                   dem_values, -9999.0, false);
  ASSERT_NE(nullptr, ds_aniso);
  GDALDatasetUniquePtr out_aniso(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_aniso.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_aniso);

  // 5. ZevenbergenThorne algorithm
  auto opt_zt =
      MakeOptions({"hillshade", "-of", "MEM", "-alg", "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_zt_int(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "hillshade", nullptr, opt_zt.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_int);

  GDALDatasetUniquePtr out_zt_float(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "hillshade", nullptr, opt_zt.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_float);

  // 6. Combined shading (Horn and ZevenbergenThorne)
  auto opt_comb_horn =
      MakeOptions({"hillshade", "-of", "MEM", "-combined", "-alg", "Horn"});
  GDALDatasetUniquePtr out_comb_horn(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "hillshade", nullptr, opt_comb_horn.get(), nullptr)));
  EXPECT_NE(nullptr, out_comb_horn);

  auto opt_comb_zt = MakeOptions(
      {"hillshade", "-of", "MEM", "-combined", "-alg", "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_comb_zt(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "hillshade", nullptr, opt_comb_zt.get(), nullptr)));
  EXPECT_NE(nullptr, out_comb_zt);

  // 7. Multidirectional shading (Horn and ZevenbergenThorne, square and
  // non-square)
  auto opt_multi_horn = MakeOptions(
      {"hillshade", "-of", "MEM", "-multidirectional", "-alg", "Horn"});
  GDALDatasetUniquePtr out_multi_horn(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "hillshade", nullptr, opt_multi_horn.get(), nullptr)));
  EXPECT_NE(nullptr, out_multi_horn);

  auto opt_multi_zt =
      MakeOptions({"hillshade", "-of", "MEM", "-multidirectional", "-alg",
                   "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_multi_zt(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "hillshade", nullptr, opt_multi_zt.get(), nullptr)));
  EXPECT_NE(nullptr, out_multi_zt);

  GDALDatasetUniquePtr out_multi_aniso(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_aniso.get(), "hillshade", nullptr,
                        opt_multi_horn.get(), nullptr)));
  EXPECT_NE(nullptr, out_multi_aniso);

  // 8. Igor hillshade (Horn and ZevenbergenThorne)
  auto opt_igor_horn =
      MakeOptions({"hillshade", "-of", "MEM", "-igor", "-alg", "Horn"});
  GDALDatasetUniquePtr out_igor_horn(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "hillshade", nullptr, opt_igor_horn.get(), nullptr)));
  EXPECT_NE(nullptr, out_igor_horn);

  auto opt_igor_zt = MakeOptions(
      {"hillshade", "-of", "MEM", "-igor", "-alg", "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_igor_zt(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "hillshade", nullptr, opt_igor_zt.get(), nullptr)));
  EXPECT_NE(nullptr, out_igor_zt);

  // 9. Nodata out of data type range (e.g. -99999 on Byte band)
  auto ds_byte =
      CreateMemDataset(4, 4, GDT_Byte, gt_square, nullptr, {}, -99999.0, true);
  ASSERT_NE(nullptr, ds_byte);
  GDALDatasetUniquePtr out_byte(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_byte.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_byte);

  // 10. Progress cancellation mid-way
  auto opt_cancel = MakeOptions({"hillshade", "-of", "MEM"});
  GDALProgressFunc pfnCancel = [](double, const char*, void*) -> int {
    return 0;
  };
  GDALDEMProcessingOptionsSetProgress(opt_cancel.get(), pfnCancel, nullptr);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr out_cancel(GDALDataset::FromHandle(GDALDEMProcessing(
        "", ds_int.get(), "hillshade", nullptr, opt_cancel.get(), nullptr)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
  }

  // 11. Multidirectional on completely flat DEM (xx_plus_yy == 0 branch)
  std::vector<double> flat_vals(16, 100.0);
  auto ds_flat_dem = CreateMemDataset(4, 4, GDT_Float32, gt_square, nullptr,
                                      flat_vals, -9999.0, false);
  ASSERT_NE(nullptr, ds_flat_dem);
  auto opt_multi_flat =
      MakeOptions({"hillshade", "-of", "MEM", "-multidirectional"});
  GDALDatasetUniquePtr out_multi_flat(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_flat_dem.get(), "hillshade", nullptr,
                        opt_multi_flat.get(), nullptr)));
  EXPECT_NE(nullptr, out_multi_flat);
}

// -----------------------------------------------------------------------------
// Slope processing tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, SlopeAllModes) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> dem_values(25);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 5; ++x) {
      dem_values[y * 5 + x] = 50.0 + x * 10.0;
    }
  }

  auto ds_int = CreateMemDataset(5, 5, GDT_Int16, gt, nullptr, dem_values,
                                 -9999.0, false);
  ASSERT_NE(nullptr, ds_int);
  auto ds_float = CreateMemDataset(5, 5, GDT_Float32, gt, nullptr, dem_values,
                                   -9999.0, true);
  ASSERT_NE(nullptr, ds_float);

  // 1. Horn in degrees
  auto opt_horn_deg =
      MakeOptions({"slope", "-of", "MEM", "-alg", "Horn", "-compute_edges"});
  GDALDatasetUniquePtr out_horn_deg(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "slope", nullptr, opt_horn_deg.get(), nullptr)));
  ASSERT_NE(nullptr, out_horn_deg);
  EXPECT_EQ(GDT_Float32, out_horn_deg->GetRasterBand(1)->GetRasterDataType());

  // 2. Horn in percent (-p)
  auto opt_horn_pct =
      MakeOptions({"slope", "-of", "MEM", "-p", "-alg", "Horn"});
  GDALDatasetUniquePtr out_horn_pct(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "slope", nullptr, opt_horn_pct.get(), nullptr)));
  EXPECT_NE(nullptr, out_horn_pct);

  // 3. ZevenbergenThorne in degrees
  auto opt_zt_deg =
      MakeOptions({"slope", "-of", "MEM", "-alg", "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_zt_deg(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "slope", nullptr, opt_zt_deg.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_deg);

  // 4. ZevenbergenThorne in percent (-p)
  auto opt_zt_pct =
      MakeOptions({"slope", "-of", "MEM", "-p", "-alg", "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_zt_pct(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "slope", nullptr, opt_zt_pct.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_pct);
}

// -----------------------------------------------------------------------------
// Aspect processing tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, AspectAllModes) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> dem_values = {
      100, 100, 100, 100, 100, 100, 100, 100,  // Flat top rows
      50,  60,  70,  80,  10,  20,  30,  40,
  };

  auto ds_int = CreateMemDataset(4, 4, GDT_Int16, gt, nullptr, dem_values,
                                 -9999.0, false);
  ASSERT_NE(nullptr, ds_int);
  auto ds_float = CreateMemDataset(4, 4, GDT_Float32, gt, nullptr, dem_values,
                                   -9999.0, true);
  ASSERT_NE(nullptr, ds_float);

  // 1. Horn azimuth (default flat returns -9999)
  auto opt_horn_az =
      MakeOptions({"aspect", "-of", "MEM", "-alg", "Horn", "-compute_edges"});
  GDALDatasetUniquePtr out_horn_az(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "aspect", nullptr, opt_horn_az.get(), nullptr)));
  ASSERT_NE(nullptr, out_horn_az);

  // 2. Horn trigonometric (-trigonometric) and zero_for_flat
  auto opt_horn_trig_zero =
      MakeOptions({"aspect", "-of", "MEM", "-trigonometric", "-zero_for_flat",
                   "-alg", "Horn"});
  GDALDatasetUniquePtr out_horn_trig_zero(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_float.get(), "aspect", nullptr,
                        opt_horn_trig_zero.get(), nullptr)));
  EXPECT_NE(nullptr, out_horn_trig_zero);

  // 3. ZevenbergenThorne azimuth
  auto opt_zt_az = MakeOptions(
      {"aspect", "-of", "MEM", "-alg", "ZevenbergenThorne", "-zero_for_flat"});
  GDALDatasetUniquePtr out_zt_az(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "aspect", nullptr, opt_zt_az.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_az);

  // 4. ZevenbergenThorne trigonometric
  auto opt_zt_trig = MakeOptions(
      {"aspect", "-of", "MEM", "-trigonometric", "-alg", "ZevenbergenThorne"});
  GDALDatasetUniquePtr out_zt_trig(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "aspect", nullptr, opt_zt_trig.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_trig);
}

// -----------------------------------------------------------------------------
// TRI, TPI, Roughness processing tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, TRIModes) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> dem_values(25);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 5; ++x) {
      dem_values[y * 5 + x] =
          100.0 + (x - 2) * (x - 2) * 5.0 + (y - 2) * (y - 2) * 5.0;
    }
  }

  auto ds_int = CreateMemDataset(5, 5, GDT_Int16, gt, nullptr, dem_values,
                                 -9999.0, false);
  ASSERT_NE(nullptr, ds_int);
  auto ds_float = CreateMemDataset(5, 5, GDT_Float32, gt, nullptr, dem_values,
                                   -9999.0, true);
  ASSERT_NE(nullptr, ds_float);

  // 1. Wilson algorithm (Int16 and Float32)
  auto opt_wilson =
      MakeOptions({"TRI", "-of", "MEM", "-alg", "Wilson", "-compute_edges"});
  GDALDatasetUniquePtr out_wilson_int(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "TRI", nullptr, opt_wilson.get(), nullptr)));
  EXPECT_NE(nullptr, out_wilson_int);

  GDALDatasetUniquePtr out_wilson_float(
      GDALDataset::FromHandle(GDALDEMProcessing(
          "", ds_float.get(), "TRI", nullptr, opt_wilson.get(), nullptr)));
  EXPECT_NE(nullptr, out_wilson_float);

  // 2. Riley algorithm (Int16 and Float32)
  auto opt_riley =
      MakeOptions({"TRI", "-of", "MEM", "-alg", "Riley", "-compute_edges"});
  GDALDatasetUniquePtr out_riley_int(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "TRI", nullptr, opt_riley.get(), nullptr)));
  EXPECT_NE(nullptr, out_riley_int);

  GDALDatasetUniquePtr out_riley_float(
      GDALDataset::FromHandle(GDALDEMProcessing(
          "", ds_float.get(), "TRI", nullptr, opt_riley.get(), nullptr)));
  EXPECT_NE(nullptr, out_riley_float);
}

TEST_F(GdaldemLibTest, TPIAndRoughness) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> dem_values(25);
  for (int i = 0; i < 25; ++i) {
    dem_values[i] = 100.0 + i * 5.0;
  }

  auto ds_int =
      CreateMemDataset(5, 5, GDT_Int16, gt, nullptr, dem_values, -9999.0, true);
  ASSERT_NE(nullptr, ds_int);
  auto ds_float = CreateMemDataset(5, 5, GDT_Float32, gt, nullptr, dem_values,
                                   -9999.0, false);
  ASSERT_NE(nullptr, ds_float);

  // TPI
  auto opt_tpi = MakeOptions({"TPI", "-of", "MEM", "-compute_edges"});
  GDALDatasetUniquePtr out_tpi_int(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "TPI", nullptr, opt_tpi.get(), nullptr)));
  EXPECT_NE(nullptr, out_tpi_int);

  GDALDatasetUniquePtr out_tpi_float(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "TPI", nullptr, opt_tpi.get(), nullptr)));
  EXPECT_NE(nullptr, out_tpi_float);

  // Roughness
  auto opt_rough = MakeOptions({"roughness", "-of", "MEM", "-compute_edges"});
  GDALDatasetUniquePtr out_rough_int(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "roughness", nullptr, opt_rough.get(), nullptr)));
  EXPECT_NE(nullptr, out_rough_int);

  GDALDatasetUniquePtr out_rough_float(
      GDALDataset::FromHandle(GDALDEMProcessing(
          "", ds_float.get(), "roughness", nullptr, opt_rough.get(), nullptr)));
  EXPECT_NE(nullptr, out_rough_float);
}

// -----------------------------------------------------------------------------
// Color Relief processing tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, ColorReliefModesAndFormats) {
  constexpr char kColorFile[] = "/vsimem/gdaldem_lib_test/color_table.txt";
  constexpr char kColorContent[] =
      "nv 0 0 0 0\n"
      "0 0 0 255\n"
      "50 0 255 0\n"
      "100 255 255 0\n"
      "100 255 0 0\n"  // Repeated entry
      "200 255 255 255 255\n";

  VsiMemTempWrapper color_file_wrapper(kColorFile, kColorContent);

  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> dem_values = {
      0,   25,  50,
      75,  100, 150,
      200, 250, std::numeric_limits<double>::quiet_NaN(),
  };

  auto ds_float =
      CreateMemDataset(3, 3, GDT_Float32, gt, nullptr, dem_values,
                       std::numeric_limits<double>::quiet_NaN(), true);
  ASSERT_NE(nullptr, ds_float);

  // 1. Default interpolation mode (3 bands RGB)
  auto opt_rgb = MakeOptions({"color-relief", "-of", "MEM"});
  GDALDatasetUniquePtr out_rgb(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "color-relief", kColorFile, opt_rgb.get(), nullptr)));
  ASSERT_NE(nullptr, out_rgb);
  EXPECT_EQ(3, out_rgb->GetRasterCount());

  // 2. Alpha channel mode (4 bands RGBA)
  auto opt_alpha = MakeOptions({"color-relief", "-of", "MEM", "-alpha"});
  GDALDatasetUniquePtr out_rgba(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_float.get(), "color-relief", kColorFile,
                        opt_alpha.get(), nullptr)));
  ASSERT_NE(nullptr, out_rgba);
  EXPECT_EQ(4, out_rgba->GetRasterCount());

  // 3. Exact color entry mode
  auto opt_exact =
      MakeOptions({"color-relief", "-of", "MEM", "-exact_color_entry"});
  GDALDatasetUniquePtr out_exact(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_float.get(), "color-relief", kColorFile,
                        opt_exact.get(), nullptr)));
  EXPECT_NE(nullptr, out_exact);

  // 4. Nearest color entry mode
  auto opt_nearest =
      MakeOptions({"color-relief", "-of", "MEM", "-nearest_color_entry"});
  GDALDatasetUniquePtr out_nearest(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_float.get(), "color-relief", kColorFile,
                        opt_nearest.get(), nullptr)));
  EXPECT_NE(nullptr, out_nearest);

  // 5. Precomputed LUT path for Byte dataset
  auto ds_byte =
      CreateMemDataset(3, 3, GDT_Byte, gt, nullptr, dem_values, 0, false);
  ASSERT_NE(nullptr, ds_byte);
  GDALDatasetUniquePtr out_byte(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_byte.get(), "color-relief", kColorFile, opt_rgb.get(), nullptr)));
  EXPECT_NE(nullptr, out_byte);

  // 6. Precomputed LUT path for Int16 with > 65536 cells
  constexpr int kBigW = 300;
  constexpr int kBigH = 300;  // 90000 cells > 65536
  std::vector<double> big_dem(kBigW * kBigH, 50.0);
  auto ds_big_int = CreateMemDataset(kBigW, kBigH, GDT_Int16, gt, nullptr,
                                     big_dem, -9999.0, true);
  ASSERT_NE(nullptr, ds_big_int);
  GDALDatasetUniquePtr out_big_int(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_big_int.get(), "color-relief", kColorFile,
                        opt_rgb.get(), nullptr)));
  EXPECT_NE(nullptr, out_big_int);

  // 7. VRT format output (with file-backed GTiff input)
  constexpr char kSrcTif[] = "/vsimem/gdaldem_lib_test/vrt_input.tif";
  constexpr char kVrtFile[] = "/vsimem/gdaldem_lib_test/color_relief.vrt";
  absl::Cleanup clean_vrt_files = [] {
    VSIUnlink("/vsimem/gdaldem_lib_test/vrt_input.tif");
    VSIUnlink("/vsimem/gdaldem_lib_test/color_relief.vrt");
  };

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr gtiff_src(
      gtiff_drv->Create(kSrcTif, 3, 3, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, gtiff_src);
  gtiff_src->SetGeoTransform(const_cast<double*>(gt));
  EXPECT_EQ(CE_None, gtiff_src->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 3, 3, dem_values.data(), 3, 3,
                         GDT_Float64, 0, 0, nullptr));
  gtiff_src.reset();  // Flush and close GTiff so it exists in vsimem

  GDALDatasetUniquePtr reopened_src(GDALDataset::Open(kSrcTif, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, reopened_src);

  auto opt_vrt = MakeOptions({"color-relief", "-of", "VRT", "-alpha"});
  GDALDatasetUniquePtr out_vrt(GDALDataset::FromHandle(
      GDALDEMProcessing(kVrtFile, reopened_src.get(), "color-relief",
                        kColorFile, opt_vrt.get(), nullptr)));
  ASSERT_NE(nullptr, out_vrt);
  EXPECT_EQ(4, out_vrt->GetRasterCount());

  // Also test VRT creation with pszDest = "" (bTmpFile in-memory VRT path)
  GDALDatasetUniquePtr out_vrt_mem(GDALDataset::FromHandle(
      GDALDEMProcessing("", reopened_src.get(), "color-relief", kColorFile,
                        opt_vrt.get(), nullptr)));
  EXPECT_NE(nullptr, out_vrt_mem);

  // Re-open VRT to test reading
  GDALDatasetUniquePtr reopened_vrt(
      GDALDataset::Open(kVrtFile, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, reopened_vrt);
  std::vector<uint8_t> vrt_buf(3 * 3);
  EXPECT_EQ(CE_None, reopened_vrt->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 3, 3, vrt_buf.data(), 3, 3, GDT_Byte, 0,
                         0, nullptr));

  // 8. Non-existent color file
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr out_bad(GDALDataset::FromHandle(GDALDEMProcessing(
        "", ds_float.get(), "color-relief", "/vsimem/nonexistent_color.txt",
        opt_rgb.get(), nullptr)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 9. Empty color file
  {
    constexpr char kEmptyColorFile[] =
        "/vsimem/gdaldem_lib_test/empty_color.txt";
    VsiMemTempWrapper empty_wrapper(kEmptyColorFile, "");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr out_empty(GDALDataset::FromHandle(
        GDALDEMProcessing("", ds_float.get(), "color-relief", kEmptyColorFile,
                          opt_rgb.get(), nullptr)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Intermediate Dataset and CreateCopy paths
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, IntermediateDatasetAndCreateCopy) {
  constexpr char kOutDir[] = "/vsimem/gdaldem_lib_test/intermediate";
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdaldem_lib_test/intermediate");
  };
  ASSERT_EQ(0, VSIMkdir(kOutDir, 0755));

  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> dem_values(64);
  for (int i = 0; i < 64; ++i) {
    dem_values[i] = 100.0 + i * 2.0;
  }

  // 1. GDALGeneric3x3Dataset<GInt32> via GTiff COMPRESS=DEFLATE TILED=YES
  auto ds_int =
      CreateMemDataset(8, 8, GDT_Int16, gt, nullptr, dem_values, -9999.0, true);
  ASSERT_NE(nullptr, ds_int);

  const std::string tif_int = std::string(kOutDir) + "/hillshade_int.tif";
  auto opt_tiled_int = MakeOptions({
      "hillshade",
      "-of",
      "GTiff",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
      "-compute_edges",
  });
  GDALDatasetUniquePtr out_tiled_int(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_int.c_str(), ds_int.get(), "hillshade", nullptr,
                        opt_tiled_int.get(), nullptr)));
  ASSERT_NE(nullptr, out_tiled_int);
  EXPECT_EQ(1, out_tiled_int->GetRasterCount());

  // 2. GDALGeneric3x3Dataset<float> via GTiff COMPRESS=DEFLATE TILED=YES
  auto ds_float = CreateMemDataset(8, 8, GDT_Float32, gt, nullptr, dem_values,
                                   -9999.0, true);
  ASSERT_NE(nullptr, ds_float);

  const std::string tif_float = std::string(kOutDir) + "/hillshade_float.tif";
  auto opt_tiled_float = MakeOptions({
      "hillshade",
      "-of",
      "GTiff",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
  });
  GDALDatasetUniquePtr out_tiled_float(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_float.c_str(), ds_float.get(), "hillshade", nullptr,
                        opt_tiled_float.get(), nullptr)));
  ASSERT_NE(nullptr, out_tiled_float);

  // 3. GDALColorReliefDataset via GTiff COMPRESS=DEFLATE TILED=YES
  const std::string color_file = std::string(kOutDir) + "/color.txt";
  VsiMemTempWrapper color_wrapper(color_file,
                                  "0 0 0 255\n100 0 255 0\n200 255 0 0\n");

  const std::string tif_color = std::string(kOutDir) + "/color.tif";
  auto opt_tiled_color = MakeOptions({
      "color-relief",
      "-of",
      "GTiff",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
      "-alpha",
  });
  GDALDatasetUniquePtr out_tiled_color(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_color.c_str(), ds_float.get(), "color-relief",
                        color_file.c_str(), opt_tiled_color.get(), nullptr)));
  ASSERT_NE(nullptr, out_tiled_color);
  EXPECT_EQ(4, out_tiled_color->GetRasterCount());
}

// -----------------------------------------------------------------------------
// Degenerate sizes and edge condition tests
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, DegenerateSizesAndNodataLines) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  auto opt_edges_mem =
      MakeOptions({"hillshade", "-of", "MEM", "-compute_edges"});

  // 1x1 raster
  auto ds_1x1 =
      CreateMemDataset(1, 1, GDT_Float32, gt, nullptr, {100.0}, -9999.0, false);
  ASSERT_NE(nullptr, ds_1x1);
  GDALDatasetUniquePtr out_1x1(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_1x1.get(), "hillshade", nullptr, opt_edges_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_1x1);

  // 2x2 raster
  auto ds_2x2 = CreateMemDataset(2, 2, GDT_Int16, gt, nullptr, {10, 20, 30, 40},
                                 -9999.0, false);
  ASSERT_NE(nullptr, ds_2x2);
  GDALDatasetUniquePtr out_2x2(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_2x2.get(), "hillshade", nullptr, opt_edges_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_2x2);

  // 3x1 raster (height < 2, width >= 2)
  auto ds_3x1 = CreateMemDataset(3, 1, GDT_Int16, gt, nullptr, {10, 20, 30},
                                 -9999.0, false);
  ASSERT_NE(nullptr, ds_3x1);
  GDALDatasetUniquePtr out_3x1(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_3x1.get(), "hillshade", nullptr, opt_edges_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_3x1);

  // 1x3 raster (width < 2, height >= 2)
  auto ds_1x3 = CreateMemDataset(1, 3, GDT_Float32, gt, nullptr, {10, 20, 30},
                                 -9999.0, false);
  ASSERT_NE(nullptr, ds_1x3);
  GDALDatasetUniquePtr out_1x3(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_1x3.get(), "hillshade", nullptr, opt_edges_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_1x3);

  // Line with all nodata
  std::vector<double> nodata_line = {
      -9999.0, -9999.0, -9999.0, 100.0, 110.0, 120.0, 130.0, 140.0, 150.0,
  };
  auto ds_nodata_line = CreateMemDataset(3, 3, GDT_Float32, gt, nullptr,
                                         nodata_line, -9999.0, true);
  ASSERT_NE(nullptr, ds_nodata_line);
  GDALDatasetUniquePtr out_nodata_line(GDALDataset::FromHandle(
      GDALDEMProcessing("", ds_nodata_line.get(), "hillshade", nullptr,
                        opt_edges_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_nodata_line);
}

// -----------------------------------------------------------------------------
// Advanced branches: warping, overviews, all quadrants, interpol, colors
// -----------------------------------------------------------------------------

TEST_F(GdaldemLibTest, RotatedAndSkewedAutoWarp) {
  constexpr double gt_rot[6] = {100, 10, 5, 200, 5, -10};
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  char* wkt = nullptr;
  srs.exportToWkt(&wkt);
  ASSERT_NE(nullptr, wkt);
  absl::Cleanup free_wkt = [wkt] { CPLFree(wkt); };

  std::vector<double> vals(16, 50.0);
  auto ds =
      CreateMemDataset(4, 4, GDT_Float32, gt_rot, wkt, vals, -9999.0, false);
  ASSERT_NE(nullptr, ds);

  // Hillshade auto-warps rotated input
  auto opt_mem = MakeOptions({"hillshade", "-of", "MEM"});
  GDALDatasetUniquePtr out_hs(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_hs);

  // Slope auto-warps rotated input
  auto opt_slope = MakeOptions({"slope", "-of", "MEM"});
  GDALDatasetUniquePtr out_slope(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "slope", nullptr, opt_slope.get(), nullptr)));
  EXPECT_NE(nullptr, out_slope);

  // Aspect with south-up geotransform (gt[5] > 0) auto-warps
  constexpr double gt_south_up[6] = {100, 10, 0, 200, 0, 10};
  auto ds_south = CreateMemDataset(4, 4, GDT_Float32, gt_south_up, wkt, vals,
                                   -9999.0, false);
  ASSERT_NE(nullptr, ds_south);
  auto opt_aspect = MakeOptions({"aspect", "-of", "MEM"});
  GDALDatasetUniquePtr out_aspect(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_south.get(), "aspect", nullptr, opt_aspect.get(), nullptr)));
  EXPECT_NE(nullptr, out_aspect);
}

TEST_F(GdaldemLibTest, AspectAllQuadrants) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> vals = {
      0, 10, 0, 10, 50, 10, 0, 10, 0,
  };
  auto ds =
      CreateMemDataset(3, 3, GDT_Float32, gt, nullptr, vals, -9999.0, false);
  ASSERT_NE(nullptr, ds);

  // Horn: azimuth & trig
  auto opt_az =
      MakeOptions({"aspect", "-of", "MEM", "-alg", "Horn", "-compute_edges"});
  GDALDatasetUniquePtr out_az(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "aspect", nullptr, opt_az.get(), nullptr)));
  EXPECT_NE(nullptr, out_az);

  auto opt_trig = MakeOptions({"aspect", "-of", "MEM", "-alg", "Horn",
                               "-trigonometric", "-compute_edges"});
  GDALDatasetUniquePtr out_trig(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "aspect", nullptr, opt_trig.get(), nullptr)));
  EXPECT_NE(nullptr, out_trig);

  // ZevenbergenThorne: azimuth & trig
  auto opt_zt_az = MakeOptions(
      {"aspect", "-of", "MEM", "-alg", "ZevenbergenThorne", "-compute_edges"});
  GDALDatasetUniquePtr out_zt_az(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "aspect", nullptr, opt_zt_az.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_az);

  auto opt_zt_trig = MakeOptions({
      "aspect",
      "-of",
      "MEM",
      "-alg",
      "ZevenbergenThorne",
      "-trigonometric",
      "-compute_edges",
  });
  GDALDatasetUniquePtr out_zt_trig(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "aspect", nullptr, opt_zt_trig.get(), nullptr)));
  EXPECT_NE(nullptr, out_zt_trig);
}

TEST_F(GdaldemLibTest, InterpolAndComputeValNodata) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> vals = {
      50, 100, 50, 0, 50, 0, 50, 50, 50,
  };

  // Float32
  auto ds_float =
      CreateMemDataset(3, 3, GDT_Float32, gt, nullptr, vals, 100.0, true);
  ASSERT_NE(nullptr, ds_float);
  auto opt_edges_f = MakeOptions({"slope", "-of", "MEM", "-compute_edges"});
  GDALDatasetUniquePtr out_f1(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "slope", nullptr, opt_edges_f.get(), nullptr)));
  EXPECT_NE(nullptr, out_f1);

  auto opt_no_edges_f = MakeOptions({"slope", "-of", "MEM"});
  GDALDatasetUniquePtr out_f2(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "slope", nullptr, opt_no_edges_f.get(), nullptr)));
  EXPECT_NE(nullptr, out_f2);

  // Int32 / Int16
  auto ds_int =
      CreateMemDataset(3, 3, GDT_Int16, gt, nullptr, vals, 100.0, true);
  ASSERT_NE(nullptr, ds_int);
  auto opt_edges_i = MakeOptions({"slope", "-of", "MEM", "-compute_edges"});
  GDALDatasetUniquePtr out_i1(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "slope", nullptr, opt_edges_i.get(), nullptr)));
  EXPECT_NE(nullptr, out_i1);

  auto opt_no_edges_i = MakeOptions({"slope", "-of", "MEM"});
  GDALDatasetUniquePtr out_i2(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "slope", nullptr, opt_no_edges_i.get(), nullptr)));
  EXPECT_NE(nullptr, out_i2);
}

TEST_F(GdaldemLibTest, Generic3x3DatasetFloatAndOverviews) {
  constexpr char kOutDir[] = "/vsimem/gdaldem_lib_test/ovr_test";
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdaldem_lib_test/ovr_test");
  };
  ASSERT_EQ(0, VSIMkdir(kOutDir, 0755));

  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  const std::string tif_src = std::string(kOutDir) + "/src_with_ovr.tif";
  std::vector<double> vals(64 * 64, 50.0);
  for (int i = 0; i < 64 * 64; ++i) {
    vals[i] = 100.0 + (i % 64) * 2.0 + (i / 64) * 3.0;
  }

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr ds(
      gtiff_drv->Create(tif_src.c_str(), 64, 64, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  ds->SetGeoTransform(const_cast<double*>(gt));
  ds->GetRasterBand(1)->SetNoDataValue(-9999.0);
  EXPECT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64, vals.data(),
                                           64, 64, GDT_Float64, 0, 0, nullptr));

  // Build external overview on source
  CPLSetConfigOption("GDAL_FORCE_EXTERNAL_OVERVIEWS", "YES");
  absl::Cleanup reset_overviews = [] {
    CPLSetConfigOption("GDAL_FORCE_EXTERNAL_OVERVIEWS", nullptr);
  };
  int anOverviewList[1] = {2};
  EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, anOverviewList, 0,
                                        nullptr, nullptr, nullptr, nullptr));
  std::move(reset_overviews).Invoke();
  ds.reset();

  GDALDatasetUniquePtr src(GDALDataset::Open(tif_src.c_str(), GA_ReadOnly));
  ASSERT_NE(nullptr, src);

  // 1. Slope with TILED=YES COMPRESS=DEFLATE (hits GDALGeneric3x3Dataset<float>
  // and GDT_Float32 path)
  const std::string tif_slope = std::string(kOutDir) + "/out_slope.tif";
  auto opt_slope = MakeOptions({
      "slope",
      "-of",
      "GTiff",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
  });
  GDALDatasetUniquePtr out_slope(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_slope.c_str(), src.get(), "slope", nullptr,
                        opt_slope.get(), nullptr)));
  ASSERT_NE(nullptr, out_slope);
  EXPECT_EQ(GDT_Float32, out_slope->GetRasterBand(1)->GetRasterDataType());

  // 2. Aspect with TILED=YES COMPRESS=DEFLATE
  const std::string tif_aspect = std::string(kOutDir) + "/out_aspect.tif";
  auto opt_aspect = MakeOptions({
      "aspect",
      "-of",
      "GTiff",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
  });
  GDALDatasetUniquePtr out_aspect(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_aspect.c_str(), src.get(), "aspect", nullptr,
                        opt_aspect.get(), nullptr)));
  ASSERT_NE(nullptr, out_aspect);

  // 3. MultiDirectional with TILED=YES COMPRESS=DEFLATE (hits
  // CreateScaledParameters for multidirectional)
  const std::string tif_multi = std::string(kOutDir) + "/out_multi.tif";
  auto opt_multi = MakeOptions({
      "hillshade",
      "-of",
      "GTiff",
      "-multidirectional",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
  });
  GDALDatasetUniquePtr out_multi(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_multi.c_str(), src.get(), "hillshade", nullptr,
                        opt_multi.get(), nullptr)));
  ASSERT_NE(nullptr, out_multi);

  // 4. Color relief with Byte raster and TILED=YES COMPRESS=DEFLATE (hits
  // panSourceBuf path in IReadBlock)
  std::vector<double> byte_vals(64 * 64, 50.0);
  auto ds_byte =
      CreateMemDataset(64, 64, GDT_Byte, gt, nullptr, byte_vals, 0, false);
  ASSERT_NE(nullptr, ds_byte);
  const std::string color_file = std::string(kOutDir) + "/color.txt";
  VsiMemTempWrapper color_wrapper(color_file,
                                  "0 0 0 255\n100 0 255 0\n200 255 0 0\n");
  const std::string tif_cr_byte = std::string(kOutDir) + "/cr_byte.tif";
  auto opt_cr_byte = MakeOptions({
      "color-relief",
      "-of",
      "GTiff",
      "-co",
      "COMPRESS=DEFLATE",
      "-co",
      "TILED=YES",
  });
  GDALDatasetUniquePtr out_cr_byte(GDALDataset::FromHandle(
      GDALDEMProcessing(tif_cr_byte.c_str(), ds_byte.get(), "color-relief",
                        color_file.c_str(), opt_cr_byte.get(), nullptr)));
  ASSERT_NE(nullptr, out_cr_byte);
}

TEST_F(GdaldemLibTest, VrtColorReliefNearestAndExact) {
  constexpr char kOutDir[] = "/vsimem/gdaldem_lib_test/vrt_modes";
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdaldem_lib_test/vrt_modes");
  };
  ASSERT_EQ(0, VSIMkdir(kOutDir, 0755));

  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  const std::string tif_src = std::string(kOutDir) + "/input.tif";
  std::vector<double> vals = {10.5, 25.0, 50.5, 75.0};
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr ds(
      gtiff_drv->Create(tif_src.c_str(), 2, 2, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);
  ds->SetGeoTransform(const_cast<double*>(gt));
  EXPECT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, vals.data(), 2,
                                           2, GDT_Float64, 0, 0, nullptr));
  ds.reset();

  GDALDatasetUniquePtr src(GDALDataset::Open(tif_src.c_str(), GA_ReadOnly));
  ASSERT_NE(nullptr, src);

  const std::string color_file = std::string(kOutDir) + "/color.txt";
  VsiMemTempWrapper color_wrapper(color_file,
                                  "0 0 0 255\n"
                                  "12.5 0 255 0\n"
                                  "50.5 255 255 0\n"
                                  "100 255 0 0\n");

  // 1. Nearest color entry VRT
  const std::string vrt_nearest = std::string(kOutDir) + "/nearest.vrt";
  auto opt_nearest =
      MakeOptions({"color-relief", "-of", "VRT", "-nearest_color_entry"});
  GDALDatasetUniquePtr out_nearest(GDALDataset::FromHandle(
      GDALDEMProcessing(vrt_nearest.c_str(), src.get(), "color-relief",
                        color_file.c_str(), opt_nearest.get(), nullptr)));
  ASSERT_NE(nullptr, out_nearest);
  EXPECT_EQ(3, out_nearest->GetRasterCount());

  // 2. Exact color entry VRT
  const std::string vrt_exact = std::string(kOutDir) + "/exact.vrt";
  auto opt_exact =
      MakeOptions({"color-relief", "-of", "VRT", "-exact_color_entry"});
  GDALDatasetUniquePtr out_exact(GDALDataset::FromHandle(
      GDALDEMProcessing(vrt_exact.c_str(), src.get(), "color-relief",
                        color_file.c_str(), opt_exact.get(), nullptr)));
  ASSERT_NE(nullptr, out_exact);
  EXPECT_EQ(3, out_exact->GetRasterCount());
}

TEST_F(GdaldemLibTest, NodataScanIntermediateLines) {
  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  // 6x6 raster where line 3 has NoData (-9999)
  std::vector<double> vals(36, 50.0);
  vals[3 * 6 + 2] = -9999.0;

  // Int16
  auto ds_int =
      CreateMemDataset(6, 6, GDT_Int16, gt, nullptr, vals, -9999.0, true);
  ASSERT_NE(nullptr, ds_int);
  auto opt_mem = MakeOptions({"hillshade", "-of", "MEM"});
  GDALDatasetUniquePtr out_int(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_int.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_int);

  // Float32
  auto ds_float =
      CreateMemDataset(6, 6, GDT_Float32, gt, nullptr, vals, -9999.0, true);
  ASSERT_NE(nullptr, ds_float);
  GDALDatasetUniquePtr out_float(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_float.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_float);

  // Float32 with NaN on line 3
  std::vector<double> nan_vals(36, 50.0);
  nan_vals[3 * 6 + 2] = std::numeric_limits<double>::quiet_NaN();
  auto ds_nan =
      CreateMemDataset(6, 6, GDT_Float32, gt, nullptr, nan_vals,
                       std::numeric_limits<double>::quiet_NaN(), true);
  ASSERT_NE(nullptr, ds_nan);
  GDALDatasetUniquePtr out_nan(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds_nan.get(), "hillshade", nullptr, opt_mem.get(), nullptr)));
  EXPECT_NE(nullptr, out_nan);
}

TEST_F(GdaldemLibTest, ColorReliefProcessColorsRepeatedAndNodata) {
  constexpr char kColorFile[] = "/vsimem/gdaldem_lib_test/process_colors.txt";
  constexpr char kColorContent[] =
      "0 0 0 0\n"
      "10 10 10 10\n"  // NoData value is 10.0
      "50 50 50 50\n"
      "100 100 0 0\n"
      "100 0 100 0\n"  // 2 repeated entries (nRepeatedEntryIndex == 1)
      "150 150 0 0\n"
      "200 200 0 0\n"
      "200 0 200 0\n"
      "200 0 0 200\n"  // 3 repeated entries (nRepeatedEntryIndex >= 2)
      "300 255 255 255\n";

  VsiMemTempWrapper wrapper(kColorFile, kColorContent);

  constexpr double gt[6] = {0, 10, 0, 100, 0, -10};
  std::vector<double> vals = {
      -10, 0, 5, 10, 50, 100, 150, 200, 350,
  };

  auto ds = CreateMemDataset(3, 3, GDT_Float32, gt, nullptr, vals, 10.0, true);
  ASSERT_NE(nullptr, ds);

  auto opt_exact =
      MakeOptions({"color-relief", "-of", "MEM", "-exact_color_entry"});
  GDALDatasetUniquePtr out_exact(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "color-relief", kColorFile, opt_exact.get(), nullptr)));
  EXPECT_NE(nullptr, out_exact);

  auto opt_nearest =
      MakeOptions({"color-relief", "-of", "MEM", "-nearest_color_entry"});
  GDALDatasetUniquePtr out_nearest(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "color-relief", kColorFile, opt_nearest.get(), nullptr)));
  EXPECT_NE(nullptr, out_nearest);

  auto opt_interp = MakeOptions({"color-relief", "-of", "MEM"});
  GDALDatasetUniquePtr out_interp(GDALDataset::FromHandle(GDALDEMProcessing(
      "", ds.get(), "color-relief", kColorFile, opt_interp.get(), nullptr)));
  EXPECT_NE(nullptr, out_interp);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdaldem_lib.cpp.
