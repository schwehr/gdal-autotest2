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
// Tests for GDALRasterNoDataToAlphaAlgorithm
// (apps/gdalalg_raster_nodata_to_alpha.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_nodata_to_alpha.cpp

#include "third_party/gdal/apps/gdalalg_raster_nodata_to_alpha.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_nodata_to_alpha_test";

class GdalalgRasterNodataToAlphaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTestTiff(const std::string& path, int bands, bool set_nodata,
                    bool with_alpha = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 2, 2, bands, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());

  for (int b = 1; b <= bands; ++b) {
    if (set_nodata) {
      ASSERT_EQ(CE_None, ds->GetRasterBand(b)->SetNoDataValue(0));
    }
    std::vector<uint8_t> pixels = {0, 10, 20, 30};
    if (with_alpha && b == bands) {
      ASSERT_EQ(CE_None,
                ds->GetRasterBand(b)->SetColorInterpretation(GCI_AlphaBand));
      pixels = {0, 255, 255, 255};
    } else if (bands > 1 && b > 1) {
      pixels = {static_cast<uint8_t>(b * 10), 10, 20, 30};
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 2, 2,
                                                      pixels.data(), 2, 2,
                                                      GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterNodataToAlphaTest, SingleBandWithExistingNodata) {
  const std::string in_tif = std::string(kTestDir) + "/in_nd.tif";
  const std::string out_tif = std::string(kTestDir) + "/out_nd.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(in_tif, /*bands=*/1,
                                         /*set_nodata=*/true));

  GDALRasterNoDataToAlphaAlgorithmStandalone alg;
  EXPECT_EQ("nodata-to-alpha", alg.GetName());
  ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
  EXPECT_TRUE(alg.Run());
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(2, out_ds->GetRasterCount());
  EXPECT_EQ(GCI_AlphaBand, out_ds->GetRasterBand(2)->GetColorInterpretation());
  int has_nodata = 0;
  out_ds->GetRasterBand(1)->GetNoDataValue(&has_nodata);
  EXPECT_EQ(0, has_nodata);

  std::vector<uint8_t> alpha(4, 128);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(2)->RasterIO(
                         GF_Read, 0, 0, 2, 2, alpha.data(), 2, 2, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(0, alpha[0]);
  EXPECT_EQ(255, alpha[1]);
}

TEST_F(GdalalgRasterNodataToAlphaTest, SingleNodataOverrideInPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/in_raw.tif";
  const std::string out_tif = std::string(kTestDir) + "/out_pipe.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(in_tif, /*bands=*/1,
                                         /*set_nodata=*/false));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments({
      "read",
      in_tif,
      "!",
      "nodata-to-alpha",
      "--nodata",
      "20",
      "!",
      "write",
      out_tif,
      "--overwrite",
  }));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(2, out_ds->GetRasterCount());
  std::vector<uint8_t> alpha(4, 128);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(2)->RasterIO(
                         GF_Read, 0, 0, 2, 2, alpha.data(), 2, 2, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(255, alpha[0]);
  EXPECT_EQ(0, alpha[2]);  // Pixel 2 had value 20.
}

TEST_F(GdalalgRasterNodataToAlphaTest, MultiBandNodataValuesAndMismatchError) {
  const std::string in_tif = std::string(kTestDir) + "/in_3band.tif";
  const std::string out_tif = std::string(kTestDir) + "/out_3band.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTestTiff(in_tif, /*bands=*/3,
                                         /*set_nodata=*/false));

  // Matching count (3 nodata values -> NODATA_VALUES metadata path).
  {
    GDALRasterNoDataToAlphaAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--nodata", "0,20,30", in_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand,
              out_ds->GetRasterBand(4)->GetColorInterpretation());
    std::vector<uint8_t> alpha(4, 128);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(4)->RasterIO(
                           GF_Read, 0, 0, 2, 2, alpha.data(), 2, 2, GDT_Byte, 0,
                           0, nullptr));
    // Pixel 0 is (0, 20, 30), matching NODATA_VALUES.
    EXPECT_EQ(0, alpha[0]);
    EXPECT_EQ(255, alpha[1]);
  }

  // Mismatched count (2 values for 3 bands) fails cleanly.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterNoDataToAlphaAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--nodata", "0,20", in_tif, out_tif, "--overwrite"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("There should be 3 nodata values"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterNodataToAlphaTest, NoNodataAndExistingAlphaPassthrough) {
  // 1. GMF_ALL_VALID dataset without --nodata passes through without adding
  //    an alpha band.
  {
    const std::string in_tif = std::string(kTestDir) + "/all_valid.tif";
    const std::string out_tif = std::string(kTestDir) + "/all_valid_out.tif";
    ASSERT_NO_FATAL_FAILURE(CreateTestTiff(in_tif, /*bands=*/1,
                                           /*set_nodata=*/false));
    GDALRasterNoDataToAlphaAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetRasterCount());
  }

  // 2. Dataset already having an alpha band passes through unchanged.
  {
    const std::string in_tif = std::string(kTestDir) + "/has_alpha.tif";
    const std::string out_tif = std::string(kTestDir) + "/has_alpha_out.tif";
    ASSERT_NO_FATAL_FAILURE(CreateTestTiff(in_tif, /*bands=*/2,
                                           /*set_nodata=*/false,
                                           /*with_alpha=*/true));
    GDALRasterNoDataToAlphaAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(2, out_ds->GetRasterCount());
  }
}

}  // namespace
}  // namespace autotest2
