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
// Tests for the GDAL Streamed Algorithm (GDALG) driver
// (frmts/gdalg/gdalgdriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gdalg/gdalgdriver.cpp

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalgdriver_test";

class GdalgDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GDALG();
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
}

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 7);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

void CreateTinyGeoJson(const std::string& path) {
  WriteVsimemFile(path,
                  R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.0, 2.0] },
      "properties": { "name": "alpha" }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [3.0, 4.0] },
      "properties": { "name": "beta" }
    }
  ]
})json");
}

TEST_F(GdalgDriverTest, RegisterAndIdentify) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GDALG");
  ASSERT_NE(nullptr, drv);
  EXPECT_STREQ("GDALG", drv->GetDescription());

  // Repeated registration is idempotent.
  GDALRegister_GDALG();
  EXPECT_EQ(drv, GetGDALDriverManager()->GetDriverByName("GDALG"));

  // Inline JSON containing gdal_streamed_alg identifies directly.
  {
    GDALOpenInfo info(
        R"json({"type":"gdal_streamed_alg","command_line":""})json",
        GA_ReadOnly);
    EXPECT_EQ(GDALIdentifyEnum::GDAL_IDENTIFY_TRUE, drv->pfnIdentify(&info));
  }

  // Non-existent file returns false.
  {
    GDALOpenInfo missing("/does_not_exist", GA_ReadOnly);
    EXPECT_EQ(GDALIdentifyEnum::GDAL_IDENTIFY_FALSE,
              drv->pfnIdentify(&missing));
  }

  // Wrong header / non-gdalg file returns false.
  const std::string non_gdalg = std::string(kTestDir) + "/other.json";
  ASSERT_NO_FATAL_FAILURE(
      WriteVsimemFile(non_gdalg, "{\"type\": \"FeatureCollection\"}"));
  {
    GDALOpenInfo info(non_gdalg.c_str(), GA_ReadOnly);
    EXPECT_EQ(GDALIdentifyEnum::GDAL_IDENTIFY_FALSE, drv->pfnIdentify(&info));
  }

  // Valid .gdalg.json header with type gdal_streamed_alg identifies true.
  const std::string gdalg_file = std::string(kTestDir) + "/sample.gdalg.json";
  ASSERT_NO_FATAL_FAILURE(
      WriteVsimemFile(gdalg_file,
                      "{\"type\":\"gdal_streamed_alg\",\"command_line\":\"gdal "
                      "raster info\"}"));
  {
    GDALOpenInfo info(gdalg_file.c_str(), GA_ReadOnly);
    EXPECT_EQ(GDALIdentifyEnum::GDAL_IDENTIFY_TRUE, drv->pfnIdentify(&info));
  }
}

TEST_F(GdalgDriverTest, OpenRasterPipelineFileAndInline) {
  const std::string tif_path = std::string(kTestDir) + "/input.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));

  // Relative path resolution from .gdalg.json directory.
  const std::string gdalg_file = std::string(kTestDir) + "/raster.gdalg.json";
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(gdalg_file,
                                          R"json({
  "type": "gdal_streamed_alg",
  "command_line": "gdal raster pipeline ! read input.tif ! set-type --datatype UInt16",
  "relative_paths_relative_to_this_file": true
})json"));

  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      gdalg_file.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  ASSERT_NE(nullptr, ds->GetDriver());
  EXPECT_STREQ("GDALG", ds->GetDriver()->GetDescription());
  EXPECT_EQ(4, ds->GetRasterXSize());
  EXPECT_EQ(4, ds->GetRasterYSize());
  EXPECT_EQ(GDT_UInt16, ds->GetRasterBand(1)->GetRasterDataType());

  // Exercise GDALProxyDataset::RefUnderlyingDataset / UnrefUnderlyingDataset
  // and GDALGRasterBand::RefUnderlyingRasterBand / UnrefUnderlyingRasterBand.
  GDALGeoTransform gt;
  EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
  std::array<uint16_t, 16> pixels{};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_UInt16, 0, 0, nullptr));
  EXPECT_EQ(7, pixels[0]);

  StringListPtr file_list(ds->GetFileList(), CSLDestroy);
  ASSERT_NE(nullptr, file_list);
  EXPECT_EQ(1, CSLCount(file_list.get()));
  EXPECT_STREQ(gdalg_file.c_str(), file_list.get()[0]);
  ds.reset();

  // Opening raster-only GDALG as vector-only returns nullptr.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(
                         GDALDataset::Open(gdalg_file.c_str(), GDAL_OF_VECTOR,
                                           nullptr, nullptr, nullptr)));

  // Inline JSON specification without relative_paths_relative_to_this_file.
  const std::string inline_spec =
      "{\"type\":\"gdal_streamed_alg\",\"command_line\":\"gdal raster "
      "pipeline ! read " +
      tif_path + "\",\"relative_paths_relative_to_this_file\":false}";
  std::unique_ptr<GDALDataset> inline_ds(GDALDataset::Open(
      inline_spec.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, inline_ds);
  StringListPtr inline_files(inline_ds->GetFileList(), CSLDestroy);
  EXPECT_EQ(0, CSLCount(inline_files.get()));
}

TEST_F(GdalgDriverTest, OpenVectorPipelineAndLayerMethods) {
  const std::string geojson_path = std::string(kTestDir) + "/points.geojson";
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoJson(geojson_path));

  const std::string gdalg_file = std::string(kTestDir) + "/vector.gdalg.json";
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(gdalg_file,
                                          R"json({
  "type": "gdal_streamed_alg",
  "command_line": "gdal vector pipeline ! read points.geojson ! clip --bbox 0,0,5,5"
})json"));

  // Asking for raster-only on vector GDALG returns nullptr.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(
                         GDALDataset::Open(gdalg_file.c_str(), GDAL_OF_RASTER,
                                           nullptr, nullptr, nullptr)));

  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      gdalg_file.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());

  const OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(layer, ds->GetLayerByName(layer->GetName()));
  EXPECT_EQ(0, ds->TestCapability("NonExistentCapability"));

  // Exercise ResetReading / GetNextFeature / ExecuteSQL delegation.
  ds->ResetReading();
  OGRLayer* belonging = nullptr;
  int feat_count = 0;
  for (;;) {
    OGRFeatureUniquePtr feat(
        ds->GetNextFeature(&belonging, nullptr, nullptr, nullptr));
    if (feat == nullptr) {
      break;
    }
    ++feat_count;
  }
  EXPECT_EQ(2, feat_count);

  OGRLayer* sql_layer =
      ds->ExecuteSQL("SELECT COUNT(*) FROM points", nullptr, nullptr);
  if (sql_layer != nullptr) {
    ds->ReleaseResultSet(sql_layer);
  }
}

TEST_F(GdalgDriverTest, OpenErrorPaths) {
  WithQuietHandler quiet;
  const char* const allowed[] = {"GDALG", nullptr};

  // Missing file fails to open.
  EXPECT_EQ(nullptr,
            std::unique_ptr<GDALDataset>(GDALDataset::Open(
                (std::string(kTestDir) + "/missing.gdalg.json").c_str(),
                GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Invalid inline JSON.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         "{invalid", GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Wrong root type.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         "{\"type\":\"other\",\"command_line\":\"gdal\"}",
                         GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Missing or empty command_line.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         "{\"type\":\"gdal_streamed_alg\"}", GDAL_OF_ALL,
                         allowed, nullptr, nullptr)));

  // Single token command_line ("gdal") has < 2 args.
  EXPECT_EQ(nullptr,
            std::unique_ptr<GDALDataset>(GDALDataset::Open(
                "{\"type\":\"gdal_streamed_alg\",\"command_line\":\"gdal\"}",
                GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Command does not start with gdal.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         "{\"type\":\"gdal_streamed_alg\",\"command_"
                         "line\":\"othercmd info\"}",
                         GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Unknown algorithm.
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         "{\"type\":\"gdal_streamed_alg\",\"command_"
                         "line\":\"gdal nonexistent_alg\"}",
                         GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Algorithm that does not support streamed output (e.g. "dataset identify").
  const std::string tif_path = std::string(kTestDir) + "/err_input.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));
  CPLErrorReset();
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         ("{\"type\":\"gdal_streamed_alg\","
                          "\"command_line\":\"gdal dataset identify " +
                          tif_path + "\"}")
                             .c_str(),
                         GDAL_OF_ALL, allowed, nullptr, nullptr)));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("does not support a streamed output"));

  // Existing file with invalid JSON triggers oDoc.Load() failure.
  const std::string bad_json_file =
      std::string(kTestDir) + "/invalid.gdalg.json";
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(bad_json_file, "{not valid json"));
  CPLErrorReset();
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(
                         GDALDataset::Open(bad_json_file.c_str(), GDAL_OF_ALL,
                                           allowed, nullptr, nullptr)));

  // Newer gdal_version triggers CheckVersion() warning/error on failure.
  CPLErrorReset();
  EXPECT_EQ(nullptr,
            std::unique_ptr<GDALDataset>(GDALDataset::Open(
                "{\"type\":\"gdal_streamed_alg\","
                "\"gdal_version\":\"99999999\","
                "\"command_line\":\"gdal raster pipeline --bad-flag\"}",
                GDAL_OF_ALL, allowed, nullptr, nullptr)));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GDAL VERSION_NUM=99999999"));

  // Parse failure in algorithm (unknown option).
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         "{\"type\":\"gdal_streamed_alg\",\"command_"
                         "line\":\"gdal raster pipeline --bad-flag\"}",
                         GDAL_OF_ALL, allowed, nullptr, nullptr)));

  // Update open mode on GDALG returns nullptr.
  const std::string gdalg_file = std::string(kTestDir) + "/ro.gdalg.json";
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(gdalg_file,
                                          R"json({
  "type": "gdal_streamed_alg",
  "command_line": "gdal raster pipeline ! read err_input.tif"
})json"));
  EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(GDALDataset::Open(
                         gdalg_file.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                         allowed, nullptr, nullptr)));
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
