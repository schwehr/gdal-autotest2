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
// Tests for GDALVectorConvertAlgorithm (apps/gdalalg_vector_convert.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_convert.cpp

#include "third_party/gdal/apps/gdalalg_vector_convert.h"

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_convert_test";

constexpr char kSampleGeoJson[] = R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [1.5, 2.5] },
      "properties": { "name": "alpha", "val": 42 }
    },
    {
      "type": "Feature",
      "geometry": { "type": "Point", "coordinates": [3.5, 4.5] },
      "properties": { "name": "beta", "val": 99 }
    }
  ]
})json";

class GdalalgVectorConvertTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorConvertTest, MetadataAndAliases) {
  GDALVectorConvertAlgorithm alg;
  EXPECT_EQ("convert", alg.GetName());
  EXPECT_EQ("Convert a vector dataset.", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_vector_convert.html", alg.GetHelpURL());
  EXPECT_NE(nullptr, alg.GetArg(GDAL_ARG_NAME_INPUT));
  EXPECT_NE(nullptr, alg.GetArg(GDAL_ARG_NAME_OUTPUT));
  EXPECT_NE(nullptr, alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT));
  EXPECT_THAT(
      GDALVectorConvertAlgorithm::GetAliasesStatic(),
      ElementsAre(GDALAlgorithmRegistry::HIDDEN_ALIAS_SEPARATOR, "translate"));
  EXPECT_THAT(alg.GetUsageForCLI(false), HasSubstr("convert"));
  EXPECT_THAT(alg.GetUsageAsJSON(), HasSubstr("output-layer"));
}

TEST_F(GdalalgVectorConvertTest, GeoJsonToGeoPackageAndBack) {
  const std::string in_json = std::string(kTestDir) + "/in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  const std::string roundtrip_json = std::string(kTestDir) + "/rt.geojson";
  VsiMemTempWrapper in_file(in_json, kSampleGeoJson);

  // 1. GeoJSON -> GPKG with explicit layer name and creation options.
  {
    GDALVectorConvertAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "-f",
        "GPKG",
        "--output-layer",
        "pts",
        "--lco",
        "SPATIAL_INDEX=NO",
        in_json,
        out_gpkg,
    }));
    int progress_calls = 0;
    auto progress_cb = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    EXPECT_TRUE(alg.Finalize());
  }

  // 2. Verify GPKG and convert back to GeoJSON.
  {
    std::unique_ptr<GDALDataset> gpkg_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_THAT(gpkg_ds, NotNull());
    OGRLayer* lyr = gpkg_ds->GetLayerByName("pts");
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(2, lyr->GetFeatureCount());
  }

  {
    GDALVectorConvertAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--input-layer", "pts", out_gpkg, roundtrip_json}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> rt_ds(GDALDataset::Open(
        roundtrip_json.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_THAT(rt_ds, NotNull());
    OGRLayer* lyr = rt_ds->GetLayer(0);
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(2, lyr->GetFeatureCount());
    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_THAT(feat, NotNull());
    EXPECT_STREQ("alpha", feat->GetFieldAsString("name"));
    EXPECT_EQ(42, feat->GetFieldAsInteger("val"));
    ASSERT_THAT(feat->GetGeometryRef(), NotNull());
    ASSERT_EQ(wkbPoint, wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
    EXPECT_DOUBLE_EQ(1.5, feat->GetGeometryRef()->toPoint()->getX());
  }
}

TEST_F(GdalalgVectorConvertTest, OutputToMemAndOverwriteAppend) {
  const std::string in_json = std::string(kTestDir) + "/mem_in.geojson";
  const std::string out_gpkg = std::string(kTestDir) + "/append.gpkg";
  VsiMemTempWrapper in_file(in_json, kSampleGeoJson);

  // Convert to MEM dataset.
  {
    GDALVectorConvertAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "MEM", in_json, "mem_out"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    ASSERT_EQ(1, out_ds->GetLayerCount());
    EXPECT_EQ(2, out_ds->GetLayer(0)->GetFeatureCount());
  }

  // Create GPKG then append to it.
  {
    GDALVectorConvertAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-layer", "items", in_json, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALVectorConvertAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--append", "--output-layer", "items", in_json, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> gpkg_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(gpkg_ds, NotNull());
  OGRLayer* lyr = gpkg_ds->GetLayerByName("items");
  ASSERT_THAT(lyr, NotNull());
  EXPECT_EQ(4, lyr->GetFeatureCount());
}

TEST_F(GdalalgVectorConvertTest, ErrorPaths) {
  const std::string in_json = std::string(kTestDir) + "/err_in.geojson";
  const std::string existing_gpkg = std::string(kTestDir) + "/exists.gpkg";
  VsiMemTempWrapper in_file(in_json, kSampleGeoJson);
  VsiMemTempWrapper existing_file(existing_gpkg, "placeholder");

  // 1. Missing input fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorConvertAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {std::string(kTestDir) + "/missing.geojson",
         std::string(kTestDir) + "/out.gpkg"}));
  }

  // 2. Existing output without --overwrite fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorConvertAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({in_json, existing_gpkg}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
  }

  // 3. Invalid output format fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorConvertAlgorithm alg;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"-f", "NO_SUCH_DRIVER", in_json,
                                       std::string(kTestDir) + "/out2.gpkg"}));
  }
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
