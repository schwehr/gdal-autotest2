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
// Tests for GDALDatasetIdentifyAlgorithm (apps/gdalalg_dataset_identify.cpp
// and apps/gdalalg_dataset_identify.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_dataset_identify.cpp

#include "third_party/gdal/apps/gdalalg_dataset_identify.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_dataset_identify_test";

void WriteVsimemFile(const std::string& path, const std::string& content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(content.size(), VSIFWriteL(content.data(), 1, content.size(), fp));
  ASSERT_EQ(0, VSIFCloseL(fp));
}

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16, 7);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

void CreateTinyShapefile(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);
  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  ASSERT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
}

class GdalalgDatasetIdentifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    RegisterOGRGeoJSON();
    RegisterOGRShape();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgDatasetIdentifyTest, BasicPropertiesAndMissingRequiredArg) {
  GDALDatasetIdentifyAlgorithm alg;
  EXPECT_EQ("identify", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_STREQ("/programs/gdal_dataset_identify.html",
               alg.GetHelpURL().c_str());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
}

TEST_F(GdalalgDatasetIdentifyTest, DefaultJsonAndExplicitTextOutputs) {
  const std::string tif_path = std::string(kTestDir) + "/sample.tif";
  const std::string unknown_path = std::string(kTestDir) + "/unknown.bin";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));
  ASSERT_NO_FATAL_FAILURE(WriteVsimemFile(unknown_path, "unrecognized bytes"));

  // Programmatic default output format is JSON.
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({tif_path, unknown_path}));
    EXPECT_TRUE(alg.Run());
    const std::string json_out =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    CPLJSONDocument doc;
    ASSERT_TRUE(doc.LoadMemory(json_out));
    CPLJSONArray arr = doc.GetRoot().ToArray();
    ASSERT_EQ(1, arr.Size());
    EXPECT_EQ(tif_path, arr[0].GetString("name"));
    EXPECT_EQ("GTiff", arr[0].GetString("driver"));
  }

  // JSON with --report-failures includes null driver for unrecognized files.
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--report-failures", unknown_path}));
    EXPECT_TRUE(alg.Run());
    const std::string json_out =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    CPLJSONDocument doc;
    ASSERT_TRUE(doc.LoadMemory(json_out));
    CPLJSONArray arr = doc.GetRoot().ToArray();
    ASSERT_EQ(1, arr.Size());
    EXPECT_EQ(unknown_path, arr[0].GetString("name"));
    EXPECT_EQ(CPLJSONObject::Type::Null, arr[0].GetObj("driver").GetType());
  }

  // CLI default output format is text; test recognized and unrecognized.
  {
    GDALDatasetIdentifyAlgorithm alg;
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--report-failures", tif_path, unknown_path}));
    EXPECT_TRUE(alg.Run());
    const std::string text_out =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    EXPECT_THAT(text_out, HasSubstr(tif_path + ": GTiff\n"));
    EXPECT_THAT(text_out, HasSubstr(unknown_path + ": unrecognized\n"));
  }

  // Unrecognized without --report-failures in text mode produces empty output.
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "text", unknown_path}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>().empty());
  }
}

TEST_F(GdalalgDatasetIdentifyTest, RecursiveAndForceRecursiveDirectories) {
  const std::string sub_dir = std::string(kTestDir) + "/sub";
  const std::string shp_dir = std::string(kTestDir) + "/shp_folder";
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(shp_dir.c_str(), 0755));

  const std::string top_tif = std::string(kTestDir) + "/top.tif";
  const std::string sub_tif = sub_dir + "/nested.tif";
  const std::string shp_path = shp_dir + "/pts.shp";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(top_tif));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(sub_tif));
  ASSERT_NO_FATAL_FAILURE(CreateTinyShapefile(shp_path));

  // -r recurses into plain directories, but stops at a directory recognized
  // as a GDAL dataset (ESRI Shapefile folder).
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "text", "-r", kTestDir}));
    int progress_calls = 0;
    auto progress_cb = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return TRUE;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_calls));
    EXPECT_GT(progress_calls, 0);

    const std::string out =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    EXPECT_THAT(out, HasSubstr(top_tif + ": GTiff"));
    EXPECT_THAT(out, HasSubstr(sub_tif + ": GTiff"));
    EXPECT_THAT(out, HasSubstr(shp_dir + ": ESRI Shapefile"));
    EXPECT_EQ(std::string::npos, out.find(shp_path));
  }

  // --force-recursive descends even into recognized dataset folders.
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "text", "--force-recursive", shp_dir}));
    EXPECT_TRUE(alg.Run());
    const std::string out =
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>();
    EXPECT_THAT(out, HasSubstr(shp_dir + ": ESRI Shapefile"));
    EXPECT_THAT(out, HasSubstr(shp_path + ": ESRI Shapefile"));
  }

  // Cancelling progress stops processing and returns false.
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "text", "-r", kTestDir}));
    auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
    EXPECT_FALSE(alg.Run(cancel_cb, nullptr));
  }

  // --stdout writes directly to stdout without populating output-string.
  {
    GDALDatasetIdentifyAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "text", "--stdout", top_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING)->Get<std::string>().empty());
  }
}

}  // namespace
}  // namespace autotest2
