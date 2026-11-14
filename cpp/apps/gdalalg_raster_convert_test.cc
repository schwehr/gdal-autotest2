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
// Tests for GDALRasterConvertAlgorithm (apps/gdalalg_raster_convert.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_convert.cpp

#include "third_party/gdal/apps/gdalalg_raster_convert.h"

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
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_convert_test";

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 3, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 3, pixels.data(),
                                           4, 3, GDT_Byte, 0, 0, nullptr));
}

class GdalalgRasterConvertTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterConvertTest, PropertiesAndAliases) {
  GDALRasterConvertAlgorithm alg;
  EXPECT_EQ("convert", alg.GetName());
  EXPECT_EQ("Convert a raster dataset.", alg.GetDescription());
  EXPECT_THAT(GDALRasterConvertAlgorithm::GetAliasesStatic(),
              Contains("translate"));

  GDALRasterConvertAlgorithm mixed_alg(/*standalone=*/true,
                                       /*openForMixedRasterVector=*/true);
  EXPECT_EQ("convert", mixed_alg.GetName());
}

TEST_F(GdalalgRasterConvertTest, ConvertTiffToTiffAndVrt) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  const std::string out_vrt = std::string(kTestDir) + "/out.vrt";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  {
    GDALRasterConvertAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-format", "GTiff", "--creation-option", "COMPRESS=NONE",
         in_tif, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(3, out_ds->GetRasterYSize());
    std::vector<uint8_t> pixels(12, 0);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 4, 3, pixels.data(), 4, 3, GDT_Byte,
                           0, 0, nullptr));
    EXPECT_EQ(1, pixels[0]);
    EXPECT_EQ(12, pixels[11]);
  }

  // Overwrite existing output and write VRT.
  {
    GDALRasterConvertAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--overwrite", in_tif, out_vrt}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> vrt_ds(
        GDALDataset::Open(out_vrt.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_EQ(4, vrt_ds->GetRasterXSize());
  }
}

TEST_F(GdalalgRasterConvertTest, ErrorPaths) {
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(out_tif));

  // Existing output without --overwrite fails during argument validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterConvertAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({in_tif, out_tif}));
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // Non-existent input fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterConvertAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {std::string(kTestDir) + "/missing.tif", out_tif, "--overwrite"}));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
