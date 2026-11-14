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
// Tests for GDALRasterReclassifyAlgorithm (apps/gdalalg_raster_reclassify.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_reclassify.cpp

#include "third_party/gdal/apps/gdalalg_raster_reclassify.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_reclassify_test";

class GdalalgRasterReclassifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateInputTiff(const std::string& path, int bands,
                     std::optional<double> nodata) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 2, 2, bands, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  GDALGeoTransform gt{0.0, 1.0, 0.0, 2.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  std::array<uint8_t, 4> pixels = {1, 5, 15, 255};
  for (int b = 1; b <= bands; ++b) {
    if (nodata.has_value()) {
      ASSERT_EQ(CE_None, ds->GetRasterBand(b)->SetNoDataValue(*nodata));
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 2, 2,
                                                      pixels.data(), 2, 2,
                                                      GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterReclassifyTest, StepProperties) {
  GDALRasterReclassifyAlgorithmStandalone alg;
  EXPECT_EQ("reclassify", alg.GetName());
  EXPECT_NE(nullptr, alg.GetArg("mapping"));
  EXPECT_NE(nullptr, alg.GetArg("output-data-type"));
}

TEST_F(GdalalgRasterReclassifyTest, InlineMappingWithAndWithoutNoDataAndType) {
  const std::string in_nodata = std::string(kTestDir) + "/in_nodata.tif";
  const std::string in_plain = std::string(kTestDir) + "/in_plain.tif";
  const std::string out1 = std::string(kTestDir) + "/out1.tif";
  const std::string out2 = std::string(kTestDir) + "/out2.tif";
  ASSERT_NO_FATAL_FAILURE(CreateInputTiff(in_nodata, 2, 255.0));
  ASSERT_NO_FATAL_FAILURE(CreateInputTiff(in_plain, 1, std::nullopt));

  // 1. With NoData, multi-band, and explicit output-data-type Int16.
  {
    GDALRasterReclassifyAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_nodata, "-o", out1, "-f", "GTiff", "--output-data-type",
         "Int16", "-m", "[0,10)=100; [10,20]=200; NO_DATA=NO_DATA"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out1.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetRasterCount());
    EXPECT_EQ(GDT_Int16, ds->GetRasterBand(1)->GetRasterDataType());
    std::array<int16_t, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(100, vals[0]);
    EXPECT_EQ(100, vals[1]);
    EXPECT_EQ(200, vals[2]);
    EXPECT_EQ(255, vals[3]);
  }

  // 2. Without NoData and default output-data-type (keeps Byte).
  {
    GDALRasterReclassifyAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-i", in_plain, "-o", out2, "-f",
                                       "GTiff", "-m", "[0,10)=1; DEFAULT=9"}));
    EXPECT_TRUE(alg.Run());
    ASSERT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(out2.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Byte, ds->GetRasterBand(1)->GetRasterDataType());
    std::array<uint8_t, 4> vals{};
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(),
                                             2, 2, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(1, vals[0]);
    EXPECT_EQ(1, vals[1]);
    EXPECT_EQ(9, vals[2]);
    EXPECT_EQ(9, vals[3]);
  }
}

TEST_F(GdalalgRasterReclassifyTest,
       MappingFromAtFileWithCommentsAndWhitespace) {
  const std::string in_tif = std::string(kTestDir) + "/file_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/file_out.tif";
  const std::string map_file = std::string(kTestDir) + "/mapping.txt";
  ASSERT_NO_FATAL_FAILURE(CreateInputTiff(in_tif, 1, std::nullopt));

  VsiMemTempWrapper map_wrapper(map_file,
                                "   # leading comment\n"
                                "\n"
                                "   [0, 10) = 10 # trailing comment\n"
                                "  DEFAULT = 50\n");

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "reclassify", "--mapping", "@" + map_file, "!",
       "write", out_tif, "-f", "GTiff", "--overwrite"}));
  EXPECT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  std::array<uint8_t, 4> vals{};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, vals.data(), 2,
                                           2, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(10, vals[0]);
  EXPECT_EQ(50, vals[2]);
}

TEST_F(GdalalgRasterReclassifyTest, ErrorPaths) {
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateInputTiff(in_tif, 1, std::nullopt));

  // 1. Missing mapping file via @filename.
  {
    GDALRasterReclassifyAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "-f", "GTiff", "-m",
         "@/vsimem/gdalalg_raster_reclassify_test/nonexistent.txt"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open"));
    CPLErrorReset();
  }

  // 2. Invalid mapping expression fails validation.
  {
    GDALRasterReclassifyAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-i", in_tif, "-o", out_tif, "-f", "GTiff", "-m", "bad_mapping"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
