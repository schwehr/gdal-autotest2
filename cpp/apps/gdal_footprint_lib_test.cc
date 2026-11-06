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
// Tests for GDALFootprint (apps/gdal_footprint_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_footprint_lib.cpp

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class GdalFootprintTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRGeoJSON();
    RegisterOGRESRIJSON();
    RegisterOGRShape();
  }

  // Creates a test raster with 1 or more bands.
  // Pixels in [2..7, 2..7] are 255, surrounding pixels are 0.
  static GDALDatasetUniquePtr CreateTestRaster(const char* pszPath,
                                               int nWidth = 10,
                                               int nHeight = 10, int nBands = 1,
                                               bool bAddGeotransform = true,
                                               bool bAddSRS = true) {
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!poDriver) return nullptr;
    GDALDataset* poDS =
        poDriver->Create(pszPath, nWidth, nHeight, nBands, GDT_Byte, nullptr);
    if (!poDS) return nullptr;
    if (bAddGeotransform) {
      double adfGeoTransform[6] = {10.0, 0.01, 0.0, 20.0, 0.0, -0.01};
      poDS->SetGeoTransform(adfGeoTransform);
    }
    if (bAddSRS) {
      OGRSpatialReference oSRS;
      oSRS.importFromEPSG(4326);
      oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      poDS->SetSpatialRef(&oSRS);
    }
    std::vector<uint8_t> data(nWidth * nHeight, 0);
    for (int y = 2; y < std::min(8, nHeight); ++y) {
      for (int x = 2; x < std::min(8, nWidth); ++x) {
        data[y * nWidth + x] = 255;
      }
    }
    for (int b = 1; b <= nBands; ++b) {
      CPL_IGNORE_RET_VAL(poDS->GetRasterBand(b)->RasterIO(
          GF_Write, 0, 0, nWidth, nHeight, data.data(), nWidth, nHeight,
          GDT_Byte, 0, 0, nullptr));
    }
    return GDALDatasetUniquePtr(poDS);
  }
};

TEST_F(GdalFootprintTest, ParserUsage) {
  const std::string usage = GDALFootprintAppGetParserUsage();
  EXPECT_THAT(usage, HasSubstr("footprint"));
  EXPECT_THAT(usage, HasSubstr("-combine_bands"));
}

TEST_F(GdalFootprintTest, OptionsNewAndFree) {
  GDALFootprintOptions* psOptions = GDALFootprintOptionsNew(nullptr, nullptr);
  ASSERT_NE(nullptr, psOptions);
  GDALFootprintOptionsSetProgress(psOptions, GDALDummyProgress, nullptr);
  GDALFootprintOptionsFree(psOptions);
  // Freeing nullptr must be a no-op and not crash.
  GDALFootprintOptionsFree(nullptr);
}

TEST_F(GdalFootprintTest, OptionsParsingValid) {
  const char* apszArgv[] = {
      "-b",
      "1",
      "-combine_bands",
      "union",
      "-srcnodata",
      "0",
      "-t_cs",
      "pixel",
      "-split_polys",
      "-convex_hull",
      "-densify",
      "1.0",
      "-simplify",
      "0.5",
      "-min_ring_area",
      "10.0",
      "-max_points",
      "10",
      "-q",
      "-oo",
      "FOO=BAR",
      "-of",
      "GeoJSON",
      "-location_field_name",
      "loc",
      "-write_absolute_path",
      "-lco",
      "LCO=VAL",
      "-dsco",
      "DSCO=VAL",
      "-lyr_name",
      "mylayer",
      "-overwrite",
      "in.tif",
      "out.geojson",
      nullptr,
  };
  GDALFootprintOptionsForBinary sOptionsForBinary;
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), &sOptionsForBinary);
  ASSERT_NE(nullptr, psOptions);
  EXPECT_TRUE(sOptionsForBinary.bCreateOutput);
  EXPECT_EQ("GeoJSON", sOptionsForBinary.osFormat);
  EXPECT_EQ("mylayer", sOptionsForBinary.osDestLayerName);
  EXPECT_TRUE(sOptionsForBinary.bQuiet);
  GDALFootprintOptionsFree(psOptions);
}

TEST_F(GdalFootprintTest, OptionsParsingOtherValidCombinations) {
  // Test -max_points unlimited, -no_location, -combine_bands intersection,
  // -t_cs georef, -t_srs
  const char* apszArgv[] = {
      "-max_points",  "unlimited", "-no_location", "-combine_bands",
      "intersection", "-t_cs",     "georef",       "-t_srs",
      "EPSG:4326",    nullptr,
  };
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  GDALFootprintOptionsFree(psOptions);
}

TEST_F(GdalFootprintTest, OptionsParsingInvalid) {
  WithQuietHandler quiet;

  // Unknown argument.
  {
    const char* apszArgv[] = {"-invalid_arg", nullptr};
    CPLErrorReset();
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unexpected exception"));
  }

  // Invalid -t_srs.
  {
    const char* apszArgv[] = {"-t_srs", "invalid_srs_xyz_123", nullptr};
    CPLErrorReset();
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to process SRS definition"));
  }

  // Invalid -max_points (< 3 and != 0).
  {
    const char* apszArgv[] = {"-max_points", "2", nullptr};
    CPLErrorReset();
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value for -max_points"));
  }

  // -t_cs pixel and -t_srs are mutually exclusive.
  {
    const char* apszArgv[] = {"-t_cs", "pixel", "-t_srs", "EPSG:4326", nullptr};
    CPLErrorReset();
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("mutually exclusive"));
  }
}

TEST_F(GdalFootprintTest, InputValidation) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/validation";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  WithQuietHandler quiet;

  // pszDest == NULL && hDstDS == NULL.
  {
    int bUsageError = 0;
    CPLErrorReset();
    GDALDatasetH hRes =
        GDALFootprint(nullptr, nullptr, GDALDataset::ToHandle(src_ds.get()),
                      nullptr, &bUsageError);
    EXPECT_EQ(nullptr, hRes);
    EXPECT_TRUE(bUsageError);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("pszDest == NULL && hDstDS == NULL"));
  }

  // hSrcDataset == NULL.
  {
    int bUsageError = 0;
    CPLErrorReset();
    GDALDatasetH hRes = GDALFootprint("/vsimem/gdal_footprint_test/out.geojson",
                                      nullptr, nullptr, nullptr, &bUsageError);
    EXPECT_EQ(nullptr, hRes);
    EXPECT_TRUE(bUsageError);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("hSrcDataset== NULL"));
  }

  // hDstDS != NULL but options imply creating output.
  {
    const char* apszArgv[] = {"-of", "GeoJSON", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    int bUsageError = 0;
    CPLErrorReset();
    GDALDatasetH hRes = GDALFootprint(
        nullptr, GDALDataset::ToHandle(src_ds.get()),
        GDALDataset::ToHandle(src_ds.get()), psOptions, &bUsageError);
    EXPECT_EQ(nullptr, hRes);
    EXPECT_TRUE(bUsageError);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("options that imply creating a new dataset"));
  }
}

TEST_F(GdalFootprintTest, SrcWithoutBands) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/no_bands";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  // 1. Dataset without raster bands and without SUBDATASETS.
  {
    GDALDatasetUniquePtr poDS(
        poMemDriver->Create("", 10, 10, 0, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDS);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRes =
        GDALFootprint("/vsimem/out.geojson", nullptr,
                      GDALDataset::ToHandle(poDS.get()), nullptr, nullptr);
    EXPECT_EQ(nullptr, hRes);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Input dataset has no raster band."));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                ::testing::Not(HasSubstr("specify one subdataset")));
  }

  // 2. Dataset without raster bands and with SUBDATASETS.
  {
    GDALDatasetUniquePtr poDS(
        poMemDriver->Create("", 10, 10, 0, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDS);
    char* apszSubDatasets[] = {const_cast<char*>("SUBDATASET_1_NAME=foo"),
                               nullptr};
    poDS->SetMetadata(apszSubDatasets, "SUBDATASETS");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRes =
        GDALFootprint("/vsimem/out.geojson", nullptr,
                      GDALDataset::ToHandle(poDS.get()), nullptr, nullptr);
    EXPECT_EQ(nullptr, hRes);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("You need to specify one subdataset."));
  }
}

TEST_F(GdalFootprintTest, BasicAndDefaultOptions) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/basic";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  const std::string output_path = std::string(kDir) + "/output.geojson";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  // Calling with psOptions == nullptr exercises the default options allocation
  // branch.
  GDALDatasetH hOutDS = GDALFootprint(output_path.c_str(), nullptr,
                                      GDALDataset::ToHandle(src_ds.get()),
                                      /*psOptionsIn=*/nullptr, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
      output_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(1, out_ds->GetLayerCount());
  OGRLayer* poLayer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, poLayer);
  EXPECT_EQ(1, poLayer->GetFeatureCount(true));
  std::unique_ptr<OGRFeature> poFeature(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature);
  OGRGeometry* poGeom = poFeature->GetGeometryRef();
  ASSERT_NE(nullptr, poGeom);
  EXPECT_EQ(wkbMultiPolygon, wkbFlatten(poGeom->getGeometryType()));
}

TEST_F(GdalFootprintTest, SplitPolysAndConvexHull) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/split_convex";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  const std::string output_path = std::string(kDir) + "/output.geojson";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  const char* apszArgv[] = {
      "-split_polys",
      "-convex_hull",
      nullptr,
  };
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };

  GDALDatasetH hOutDS =
      GDALFootprint(output_path.c_str(), nullptr,
                    GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
      output_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* poLayer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, poLayer);
  EXPECT_EQ(wkbPolygon, wkbFlatten(poLayer->GetGeomType()));
  std::unique_ptr<OGRFeature> poFeature(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature);
  OGRGeometry* poGeom = poFeature->GetGeometryRef();
  ASSERT_NE(nullptr, poGeom);
  EXPECT_EQ(wkbPolygon, wkbFlatten(poGeom->getGeometryType()));
}

TEST_F(GdalFootprintTest, DensifyAndSimplify) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/densify_simplify";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  const std::string output_path = std::string(kDir) + "/output.geojson";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  // Normal densification and simplification.
  {
    const char* apszArgv[] = {
        "-densify", "0.005", "-simplify", "0.001", nullptr,
    };
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(output_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Densification distance too small compared to extent.
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {
        "-densify",
        "0.000000001",
        nullptr,
    };
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/densify_fail.geojson").c_str(), nullptr,
        GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr(
            "Densification distance too small compared to geometry extent"));
  }

  // Simplification resulting in empty geometry.
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {
        "-simplify",
        "10000000.0",
        nullptr,
    };
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/simplify_fail.geojson").c_str(), nullptr,
        GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Simplification resulted in empty geometry"));
  }
}

TEST_F(GdalFootprintTest, MinRingAreaAndMaxPoints) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/min_area_max_points";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  const std::string output_path = std::string(kDir) + "/output.geojson";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str(), 20, 20);
  ASSERT_NE(nullptr, src_ds);

  // Test -min_ring_area and -max_points (triggers CountPoints and iterative
  // reduction).
  const char* apszArgv[] = {
      "-min_ring_area", "0.0001", "-max_points", "5", nullptr,
  };
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };

  GDALDatasetH hOutDS =
      GDALFootprint(output_path.c_str(), nullptr,
                    GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
      output_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* poLayer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, poLayer);
  EXPECT_EQ(1, poLayer->GetFeatureCount(true));
}

TEST_F(GdalFootprintTest, CombineBandsUnionAndIntersection) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/combine_bands";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  // Create 2-band raster
  GDALDatasetUniquePtr src_ds =
      CreateTestRaster(raster_path.c_str(), 10, 10, /*nBands=*/2);
  ASSERT_NE(nullptr, src_ds);

  // Band 2: write a different pattern (4..9, 4..9)
  std::vector<uint8_t> data2(100, 0);
  for (int y = 4; y < 9; ++y) {
    for (int x = 4; x < 9; ++x) {
      data2[y * 10 + x] = 255;
    }
  }
  ASSERT_EQ(CE_None, src_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, 10, 10, data2.data(), 10, 10, GDT_Byte,
                         0, 0, nullptr));

  // 1. Union mode (exercises GDALFootprintCombinedMaskBand with m_bUnion =
  // true).
  {
    const std::string out_union = std::string(kDir) + "/union.geojson";
    const char* apszArgv[] = {
        "-b",    "1",     "-b", "2", "-srcnodata", "0 0", "-combine_bands",
        "union", nullptr,
    };
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_union.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);

    GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
        out_union.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* poLayer = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, poLayer);
    EXPECT_EQ(1, poLayer->GetFeatureCount(true));
  }

  // 2. Intersection mode (exercises GDALFootprintCombinedMaskBand with m_bUnion
  // = false).
  {
    const std::string out_inter = std::string(kDir) + "/intersection.geojson";
    const char* apszArgv[] = {
        "-b",
        "1",
        "-b",
        "2",
        "-srcnodata",
        "0 0",
        "-combine_bands",
        "intersection",
        nullptr,
    };
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_inter.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);

    GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
        out_inter.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* poLayer = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, poLayer);
    EXPECT_EQ(1, poLayer->GetFeatureCount(true));
  }
}

TEST_F(GdalFootprintTest, NoDataProcessing) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/nodata";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds =
      CreateTestRaster(raster_path.c_str(), 10, 10, /*nBands=*/2);
  ASSERT_NE(nullptr, src_ds);

  // 1. Single -srcnodata value on 2 bands.
  {
    const std::string out_path = std::string(kDir) + "/nodata1.geojson";
    const char* apszArgv[] = {"-srcnodata", "0", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // 2. Matching number of -srcnodata values (2 values for 2 bands).
  {
    const std::string out_path = std::string(kDir) + "/nodata2.geojson";
    const char* apszArgv[] = {"-srcnodata", "0 255", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // 3. Mismatched -srcnodata values (3 values for 2 bands).
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {"-srcnodata", "0 128 255", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint((std::string(kDir) + "/fail.geojson").c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Number of values in -srcnodata should be 1 or the "
                          "number of bands"));
  }
}

TEST_F(GdalFootprintTest, BandErrorsAndAlphaBand) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/bands";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds =
      CreateTestRaster(raster_path.c_str(), 10, 10, /*nBands=*/2);
  ASSERT_NE(nullptr, src_ds);

  // Band <= 0.
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {"-b", "0", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint((std::string(kDir) + "/fail0.geojson").c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid band number: 0"));
  }

  // Band > nBandCount.
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {"-b", "5", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint((std::string(kDir) + "/fail5.geojson").c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid band number: 5"));
  }

  // Alpha color interpretation band.
  {
    src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand);
    const std::string out_path = std::string(kDir) + "/alpha.geojson";
    const char* apszArgv[] = {"-b", "2", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }
}

TEST_F(GdalFootprintTest, OverviewHandling) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/overviews";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds =
      CreateTestRaster(raster_path.c_str(), 16, 16, /*nBands=*/1);
  ASSERT_NE(nullptr, src_ds);

  // 1. Overview requested on dataset with no overviews (mask band without
  // overviews).
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {"-ovr", "0", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/ovr_none1.geojson").c_str(), nullptr,
        GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Mask bands of this dataset have no precomputed overviews"));
  }

  // 2. Fresh dataset with nodata value (nMaskFlags == GMF_NODATA) and no
  // overviews.
  {
    const std::string nodata_path = std::string(kDir) + "/nodata_in.tif";
    GDALDatasetUniquePtr nodata_ds = CreateTestRaster(nodata_path.c_str());
    ASSERT_NE(nullptr, nodata_ds);
    ASSERT_EQ(CE_None, nodata_ds->GetRasterBand(1)->SetNoDataValue(0));

    WithQuietHandler quiet;
    const char* apszArgv[] = {"-ovr", "0", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/ovr_none2.geojson").c_str(), nullptr,
        GDALDataset::ToHandle(nodata_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Bands of this dataset have no precomputed overviews"));
  }

  // 3. Dataset with nodata and precomputed overviews.
  const std::string ovr_path = std::string(kDir) + "/ovr_in.tif";
  GDALDatasetUniquePtr ovr_ds = CreateTestRaster(ovr_path.c_str(), 16, 16);
  ASSERT_NE(nullptr, ovr_ds);
  ASSERT_EQ(CE_None, ovr_ds->GetRasterBand(1)->SetNoDataValue(0));
  int anOverviewList[1] = {2};
  ASSERT_EQ(CE_None,
            ovr_ds->BuildOverviews("NEAREST", 1, anOverviewList, 0, nullptr,
                                   nullptr, nullptr, nullptr));

  // Valid overview index 0.
  {
    const std::string out_path = std::string(kDir) + "/ovr0.geojson";
    const char* apszArgv[] = {"-ovr", "0", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_path.c_str(), nullptr,
                      GDALDataset::ToHandle(ovr_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // Out of range overview index 5.
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {"-ovr", "5", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint((std::string(kDir) + "/ovr_oor.geojson").c_str(), nullptr,
                      GDALDataset::ToHandle(ovr_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Value should be in [0,0] range"));
  }
}

TEST_F(GdalFootprintTest, CoordinateSystemsAndReprojection) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/cs";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  // 1. Pixel coordinate system (-t_cs pixel).
  {
    const std::string raster_path = std::string(kDir) + "/pixel_in.tif";
    const std::string output_path = std::string(kDir) + "/pixel_out.geojson";
    GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
    ASSERT_NE(nullptr, src_ds);

    const char* apszArgv[] = {"-t_cs", "pixel", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(output_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // 2. -t_cs georef requested, but input dataset has no geotransform.
  {
    const std::string raster_path = std::string(kDir) + "/no_gt.tif";
    GDALDatasetUniquePtr src_ds =
        CreateTestRaster(raster_path.c_str(), 10, 10, 1,
                         /*bAddGeotransform=*/false, /*bAddSRS=*/false);
    ASSERT_NE(nullptr, src_ds);

    WithQuietHandler quiet;
    const char* apszArgv[] = {"-t_cs", "georef", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/no_gt_fail.geojson").c_str(), nullptr,
        GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Georeferenced coordinates requested, but input "
                          "dataset has no geotransform"));
  }

  // 3. -t_srs reprojection to EPSG:3857.
  {
    const std::string raster_path = std::string(kDir) + "/reproj_in.tif";
    const std::string output_path = std::string(kDir) + "/reproj_out.geojson";
    GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
    ASSERT_NE(nullptr, src_ds);

    const char* apszArgv[] = {"-t_srs", "EPSG:3857", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(output_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);

    GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
        output_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* poLayer = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, poLayer);
    const OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
    ASSERT_NE(nullptr, poSRS);
    EXPECT_STREQ("WGS 84 / Pseudo-Mercator", poSRS->GetName());
  }

  // 4. Output layer has CRS, but input is not georeferenced.
  {
    const std::string raster_path = std::string(kDir) + "/unref_in.tif";
    GDALDatasetUniquePtr src_ds =
        CreateTestRaster(raster_path.c_str(), 10, 10, 1,
                         /*bAddGeotransform=*/false, /*bAddSRS=*/false);
    ASSERT_NE(nullptr, src_ds);

    WithQuietHandler quiet;
    const char* apszArgv[] = {"-t_srs", "EPSG:3857", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/unref_fail.geojson").c_str(), nullptr,
        GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Output layer has CRS, but input is not georeferenced"));
  }
}

TEST_F(GdalFootprintTest, LocationFieldAndProgress) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/location";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  const std::string output_path = std::string(kDir) + "/output.geojson";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  int nProgressCalls = 0;
  auto progress_fn = [](double dfComplete, const char* /*pszMessage*/,
                        void* pData) -> int {
    int* pnCalls = static_cast<int*>(pData);
    (*pnCalls)++;
    return TRUE;
  };

  const char* apszArgv[] = {
      "-location_field_name",
      "location_path",
      "-write_absolute_path",
      nullptr,
  };
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };
  GDALFootprintOptionsSetProgress(psOptions, progress_fn, &nProgressCalls);

  GDALDatasetH hOutDS =
      GDALFootprint(output_path.c_str(), nullptr,
                    GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);

  EXPECT_GT(nProgressCalls, 0);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
      output_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* poLayer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, poLayer);
  std::unique_ptr<OGRFeature> poFeature(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature);
  EXPECT_STREQ(raster_path.c_str(),
               poFeature->GetFieldAsString("location_path"));
}

TEST_F(GdalFootprintTest, DriverAndLayerHandling) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/driver_handling";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  // 1. Output format cannot be guessed.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hOutDS = GDALFootprint(
        (std::string(kDir) + "/out.unknown_ext_xyz").c_str(), nullptr,
        GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot guess driver"));
  }

  // 2. Output driver not supporting vector creation.
  {
    WithQuietHandler quiet;
    const char* apszArgv[] = {"-of", "GTiff", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint((std::string(kDir) + "/out.tif").c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr(
            "not recognised or does not support direct output file creation"));
  }

  // 3. Shapefile output: layer name defaults to basename.
  {
    const std::string shp_path = std::string(kDir) + "/test_poly.shp";
    const char* apszArgv[] = {"-of", "ESRI Shapefile", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(shp_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);

    GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(GDALOpenEx(
        shp_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(1, out_ds->GetLayerCount());
    EXPECT_STREQ("test_poly", out_ds->GetLayer(0)->GetName());
  }

  // 4. Update existing dataset by passing hDstDS.
  {
    const std::string shp_path = std::string(kDir) + "/test_poly.shp";
    GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(
        GDALOpenEx(shp_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, out_ds);
    const int nInitialFeatures = out_ds->GetLayer(0)->GetFeatureCount(true);

    GDALDatasetH hOutDS =
        GDALFootprint(nullptr, GDALDataset::ToHandle(out_ds.get()),
                      GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    EXPECT_EQ(nInitialFeatures + 1, out_ds->GetLayer(0)->GetFeatureCount(true));
  }

  // 5. Update existing dataset with invalid layer name.
  {
    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, poMemDriver);
    GDALDatasetUniquePtr out_ds(
        poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_NE(nullptr, out_ds->CreateLayer("existing_layer", nullptr,
                                           wkbPolygon, nullptr));

    WithQuietHandler quiet;
    const char* apszArgv[] = {"-lyr_name", "non_existent_layer", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint(nullptr, GDALDataset::ToHandle(out_ds.get()),
                      GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot find layer non_existent_layer"));
  }

  // 6. Update existing dataset using default layer name ("footprint").
  {
    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, poMemDriver);
    GDALDatasetUniquePtr out_ds(
        poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_NE(nullptr, out_ds->CreateLayer("footprint", nullptr,
                                           wkbMultiPolygon, nullptr));

    GDALDatasetH hOutDS =
        GDALFootprint(nullptr, GDALDataset::ToHandle(out_ds.get()),
                      GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount(true));
  }

  // 7. Multiple drivers matching extension (.json matches GeoJSON and fake
  // driver).
  {
    auto* poFakeDriver = new GDALDriver();
    poFakeDriver->SetDescription("FAKE_JSON");
    poFakeDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poFakeDriver->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    poFakeDriver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "json");
    GetGDALDriverManager()->RegisterDriver(poFakeDriver);
    absl::Cleanup dereg = [poFakeDriver] {
      GetGDALDriverManager()->DeregisterDriver(poFakeDriver);
      delete poFakeDriver;
    };

    WithQuietHandler quiet;
    const std::string json_path = std::string(kDir) + "/out_multi_match.json";
    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint(json_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Several drivers matching json extension"));
  }

  // 8. Output dataset creation failure (line 613).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint("/vsipath_does_not_exist/subdir/out.geojson", nullptr,
                      GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
  }

  // 9. Coordinate transformation creation failure (line 843).
  {
    const std::string raster_path = std::string(kDir) + "/eng_src.tif";
    GDALDatasetUniquePtr eng_ds = CreateTestRaster(raster_path.c_str());
    ASSERT_NE(nullptr, eng_ds);
    OGRSpatialReference oEngSRS;
    oEngSRS.SetLocalCS("Engineering Local");
    eng_ds->SetSpatialRef(&oEngSRS);

    WithQuietHandler quiet;
    const char* apszArgv[] = {"-t_srs", "EPSG:4326", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    CPLErrorReset();
    GDALDatasetH hOutDS =
        GDALFootprint((std::string(kDir) + "/eng_out.geojson").c_str(), nullptr,
                      GDALDataset::ToHandle(eng_ds.get()), psOptions, nullptr);
    EXPECT_EQ(nullptr, hOutDS);
  }
}

TEST_F(GdalFootprintTest, MaxPointsComplexSimplification) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/complex_max_points";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  // Create a 20x20 raster with a staircase/diamond shape producing > 12 points.
  const std::string raster_path = std::string(kDir) + "/complex.tif";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDriver);
  GDALDatasetUniquePtr ds(
      poDriver->Create(raster_path.c_str(), 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  double adfGeoTransform[6] = {10.0, 0.01, 0.0, 20.0, 0.0, -0.01};
  ds->SetGeoTransform(adfGeoTransform);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(0));

  std::vector<uint8_t> data(400, 0);
  for (int y = 2; y < 18; ++y) {
    int x_start = (y < 10) ? (10 - y + 2) : (y - 10 + 2);
    int x_end = 20 - x_start;
    for (int x = x_start; x < x_end; ++x) {
      data[y * 20 + x] = 255;
    }
  }
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, data.data(),
                                           20, 20, GDT_Byte, 0, 0, nullptr));

  // 1. Without -split_polys (wkbMultiPolygon path in CountPoints and
  // GetMinDistance).
  {
    const std::string out_mp = std::string(kDir) + "/out_mp.geojson";
    const char* apszArgv[] = {"-max_points", "6", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_mp.c_str(), nullptr, GDALDataset::ToHandle(ds.get()),
                      psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // 2. With -split_polys (wkbPolygon path in CountPoints and GetMinDistance).
  {
    const std::string out_poly = std::string(kDir) + "/out_poly.geojson";
    const char* apszArgv[] = {"-split_polys", "-max_points", "6", nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_poly.c_str(), nullptr,
                      GDALDataset::ToHandle(ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }

  // 3. With -split_polys and -min_ring_area (exercises Polygon ring filter).
  {
    const std::string out_area = std::string(kDir) + "/out_area.geojson";
    const char* apszArgv[] = {"-split_polys", "-min_ring_area", "0.0001",
                              nullptr};
    GDALFootprintOptions* psOptions =
        GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
    ASSERT_NE(nullptr, psOptions);
    absl::Cleanup cleanup_opts = [psOptions] {
      GDALFootprintOptionsFree(psOptions);
    };

    GDALDatasetH hOutDS =
        GDALFootprint(out_area.c_str(), nullptr,
                      GDALDataset::ToHandle(ds.get()), psOptions, nullptr);
    ASSERT_NE(nullptr, hOutDS);
    GDALClose(hOutDS);
  }
}

TEST_F(GdalFootprintTest, MultiFeatureSimplificationWarning) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/multi_feature_simpl";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  // Create raster with 2 components: a big square and a tiny 1x1 pixel
  // component.
  const std::string raster_path = std::string(kDir) + "/two_parts.tif";
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDriver);
  GDALDatasetUniquePtr ds(
      poDriver->Create(raster_path.c_str(), 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  double adfGeoTransform[6] = {10.0, 0.01, 0.0, 20.0, 0.0, -0.01};
  ds->SetGeoTransform(adfGeoTransform);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(0));

  std::vector<uint8_t> data(400, 0);
  // Big component (5..15, 5..15)
  for (int y = 5; y < 15; ++y) {
    for (int x = 5; x < 15; ++x) {
      data[y * 20 + x] = 255;
    }
  }
  // Tiny 1-pixel component at (1, 1)
  data[1 * 20 + 1] = 255;

  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, data.data(),
                                           20, 20, GDT_Byte, 0, 0, nullptr));

  // With -split_polys, two features are generated. A simplify tolerance of 0.02
  // collapses the 1x1 pixel component (size 0.01) to empty, triggering the
  // warning and continue branch (lines 1160 and 1171).
  WithQuietHandler quiet;
  const std::string out_path = std::string(kDir) + "/out.geojson";
  const char* apszArgv[] = {"-split_polys", "-simplify", "0.02", nullptr};
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };

  CPLErrorReset();
  GDALDatasetH hOutDS =
      GDALFootprint(out_path.c_str(), nullptr, GDALDataset::ToHandle(ds.get()),
                    psOptions, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);

  // The small feature became empty upon simplification and was dropped.
  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(
      GDALOpenEx(out_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, out_ds);
  ASSERT_NE(nullptr, out_ds->GetLayer(0));
  EXPECT_EQ(1, out_ds->GetLayer(0)->GetFeatureCount(true));
}

TEST_F(GdalFootprintTest, OverviewPixelCoordinates) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/ovr_pixel";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string ovr_path = std::string(kDir) + "/ovr_pixel_in.tif";
  GDALDatasetUniquePtr ds = CreateTestRaster(ovr_path.c_str(), 16, 16);
  ASSERT_NE(nullptr, ds);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(0));
  int anOverviewList[1] = {2};
  ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, anOverviewList, 0,
                                        nullptr, nullptr, nullptr, nullptr));

  // -t_cs pixel with -ovr 0 exercises the overview pixel coordinates branch
  // (lines 999-1005).
  const std::string out_path = std::string(kDir) + "/ovr_pixel_out.geojson";
  const char* apszArgv[] = {"-t_cs", "pixel", "-ovr", "0", nullptr};
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };

  GDALDatasetH hOutDS =
      GDALFootprint(out_path.c_str(), nullptr, GDALDataset::ToHandle(ds.get()),
                    psOptions, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);
}

TEST_F(GdalFootprintTest, RelativePathWithAbsolutePathFlag) {
  const std::string temp_dir = ::testing::TempDir();
  std::unique_ptr<char, CplFreeDeleter> cur_dir(CPLGetCurrentDir());
  ASSERT_NE(nullptr, cur_dir);
  ASSERT_EQ(0, chdir(temp_dir.c_str()));
  absl::Cleanup restore_dir = [&cur_dir] { chdir(cur_dir.get()); };

  const char* rel_in = "temp_footprint_in.tif";
  const char* rel_out = "temp_footprint_out.geojson";
  absl::Cleanup cleanup_files = [rel_in, rel_out] {
    VSIUnlink(rel_in);
    VSIUnlink(rel_out);
  };

  GDALDatasetUniquePtr src_ds = CreateTestRaster(rel_in);
  ASSERT_NE(nullptr, src_ds);

  const char* apszArgv[] = {
      "-location_field_name",
      "location_path",
      "-write_absolute_path",
      nullptr,
  };
  GDALFootprintOptions* psOptions =
      GDALFootprintOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };

  GDALDatasetH hOutDS =
      GDALFootprint(rel_out, nullptr, GDALDataset::ToHandle(src_ds.get()),
                    psOptions, nullptr);
  ASSERT_NE(nullptr, hOutDS);
  GDALClose(hOutDS);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(
      GDALOpenEx(rel_out, GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* poLayer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, poLayer);
  std::unique_ptr<OGRFeature> poFeature(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature);
  EXPECT_FALSE(
      CPLIsFilenameRelative(poFeature->GetFieldAsString("location_path")));
}

TEST_F(GdalFootprintTest, ProgressCancel) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/progress_cancel";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  auto cancel_progress = [](double /*dfComplete*/, const char* /*pszMessage*/,
                            void* /*pData*/) -> int {
    return FALSE;  // Cancels operation
  };

  GDALFootprintOptions* psOptions = GDALFootprintOptionsNew(nullptr, nullptr);
  ASSERT_NE(nullptr, psOptions);
  absl::Cleanup cleanup_opts = [psOptions] {
    GDALFootprintOptionsFree(psOptions);
  };
  GDALFootprintOptionsSetProgress(psOptions, cancel_progress, nullptr);

  WithQuietHandler quiet;
  const std::string out_path = std::string(kDir) + "/out.geojson";
  GDALDatasetH hOutDS =
      GDALFootprint(out_path.c_str(), nullptr,
                    GDALDataset::ToHandle(src_ds.get()), psOptions, nullptr);
  EXPECT_EQ(nullptr, hOutDS);
}

TEST_F(GdalFootprintTest, ReadOnlyDestinationLayerFailure) {
  constexpr char kDir[] = "/vsimem/gdal_footprint_test/readonly_dst";
  absl::Cleanup cleanup_dir = [kDir] { VSIRmdirRecursive(kDir); };

  const std::string raster_path = std::string(kDir) + "/input.tif";
  GDALDatasetUniquePtr src_ds = CreateTestRaster(raster_path.c_str());
  ASSERT_NE(nullptr, src_ds);

  const std::string shp_path = std::string(kDir) + "/ro.shp";
  {
    GDALDatasetH hInit =
        GDALFootprint(shp_path.c_str(), nullptr,
                      GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
    ASSERT_NE(nullptr, hInit);
    GDALClose(hInit);
  }

  GDALDatasetUniquePtr ro_ds(GDALDataset::FromHandle(
      GDALOpenEx(shp_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ro_ds);

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALDatasetH hOutDS =
      GDALFootprint(nullptr, GDALDataset::ToHandle(ro_ds.get()),
                    GDALDataset::ToHandle(src_ds.get()), nullptr, nullptr);
  EXPECT_EQ(nullptr, hOutDS);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdal_footprint_lib.cpp.
