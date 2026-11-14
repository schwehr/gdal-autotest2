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
// Tests for GDALRasterCreateAlgorithm (apps/gdalalg_raster_create.cpp and
// apps/gdalalg_raster_create.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_create.cpp

#include "third_party/gdal/apps/gdalalg_raster_create.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_create_test";

void CreateTemplateTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  {
    GDALDatasetUniquePtr ds(
        drv->Create(path.c_str(), 8, 8, 2, GDT_Int16, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ds->SetSpatialRef(&srs);
    std::array<double, 6> gt = {10.0, 0.5, 0.0, 20.0, 0.0, -0.5};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt.data()));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("SRC_KEY", "SRC_VAL"));
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(-32768));
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->SetMetadataItem("BAND_KEY", "BAND_VAL"));
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->SetNoDataValue(-32768));
    std::array<int, 1> levels = {2};
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, levels.data(), 0,
                                          nullptr, nullptr, nullptr, nullptr));
  }
}

TEST(GdalalgRasterCreateTest, BasicPropertiesAndCreateFromScratch) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string out_tif = std::string(kTestDir) + "/scratch.tif";

  {
    GDALRasterCreateAlgorithm alg;
    EXPECT_EQ("create", alg.GetName());
    EXPECT_FALSE(alg.GetDescription().empty());
    EXPECT_FALSE(alg.GetHelpURL().empty());

    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--size=4,6", "--band-count=2", "--datatype=Int16", "--nodata=-999",
         "--burn=10,20", "--crs=EPSG:4326", "--bbox=1,2,5,8",
         "--metadata=FOO=BAR", "--co=COMPRESS=NONE", out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(4, ds->GetRasterXSize());
  EXPECT_EQ(6, ds->GetRasterYSize());
  EXPECT_EQ(2, ds->GetRasterCount());
  EXPECT_EQ(GDT_Int16, ds->GetRasterBand(1)->GetRasterDataType());
  EXPECT_STREQ("BAR", ds->GetMetadataItem("FOO"));
  EXPECT_NE(nullptr, ds->GetSpatialRef());

  std::array<double, 6> gt{};
  ASSERT_EQ(CE_None, ds->GetGeoTransform(gt.data()));
  EXPECT_DOUBLE_EQ(1.0, gt[0]);
  EXPECT_DOUBLE_EQ(1.0, gt[1]);
  EXPECT_DOUBLE_EQ(8.0, gt[3]);
  EXPECT_DOUBLE_EQ(-1.0, gt[5]);

  int has_nd = 0;
  EXPECT_DOUBLE_EQ(-999.0, ds->GetRasterBand(1)->GetNoDataValue(&has_nd));
  EXPECT_EQ(1, has_nd);

  int16_t b1_val = 0;
  int16_t b2_val = 0;
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &b1_val, 1, 1,
                                           GDT_Int16, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 1, 1, &b2_val, 1, 1,
                                           GDT_Int16, 0, 0, nullptr));
  EXPECT_EQ(10, b1_val);
  EXPECT_EQ(20, b2_val);
}

TEST(GdalalgRasterCreateTest, LikeDatasetCopyMetadataAndOverviews) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string tmpl = std::string(kTestDir) + "/tmpl.tif";
  const std::string out = std::string(kTestDir) + "/copied.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTemplateTiff(tmpl));

  {
    GDALRasterCreateAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--like", tmpl, "--copy-metadata",
                                       "--copy-overviews", "--burn=5", out}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(8, ds->GetRasterXSize());
  EXPECT_EQ(8, ds->GetRasterYSize());
  EXPECT_EQ(2, ds->GetRasterCount());
  EXPECT_EQ(GDT_Int16, ds->GetRasterBand(1)->GetRasterDataType());
  EXPECT_STREQ("SRC_VAL", ds->GetMetadataItem("SRC_KEY"));
  EXPECT_STREQ("BAND_VAL", ds->GetRasterBand(1)->GetMetadataItem("BAND_KEY"));
  EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
  int has_nd = 0;
  EXPECT_DOUBLE_EQ(-32768.0, ds->GetRasterBand(1)->GetNoDataValue(&has_nd));
  EXPECT_EQ(1, has_nd);
}

TEST(GdalalgRasterCreateTest, AppendSubdatasetAndSpecialValues) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  RegisterOGRGeoPackage();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string gpkg = std::string(kTestDir) + "/sub.gpkg";
  {
    GDALRasterCreateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GPKG", "--size=4,4", "--crs=EPSG:4326", "--bbox=0,0,4,4",
         "--co=RASTER_TABLE=t1", "--burn=1", gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALRasterCreateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GPKG", "--append", "--size=4,4", "--crs=EPSG:4326",
         "--bbox=0,0,4,4", "--co=RASTER_TABLE=t2", "--burn=2", gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Test --crs=none and --nodata=none and MEM output.
  {
    GDALRasterCreateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "MEM", "--size=2,2", "--crs=none", "--nodata=none", "mem_ds"}));
    EXPECT_TRUE(alg.Run());
    const auto* out_arg = alg.GetArg(GDAL_ARG_NAME_OUTPUT);
    ASSERT_NE(nullptr, out_arg);
    const GDALDataset* out_ds =
        out_arg->Get<GDALArgDatasetValue>().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(nullptr, out_ds->GetSpatialRef());
  }
}

TEST(GdalalgRasterCreateTest, ErrorPaths) {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string tmpl = std::string(kTestDir) + "/tmpl_err.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTemplateTiff(tmpl));

  struct ErrorCase {
    std::vector<std::string> args;
    std::string expected_msg;
  };
  const ErrorCase cases[] = {
      {.args = {"/vsimem/gdalalg_raster_create_test/out.badext"},
       .expected_msg = "Cannot guess driver"},
      {.args = {"-f", "MEM", "mem_no_size"},
       .expected_msg = "Argument 'size' should be specified"},
      {.args = {"-f", "MEM", "--size=2,2", "--band-count=1", "--burn=1,2",
                "mem_bad_burn1"},
       .expected_msg = "given there is one band"},
      {.args = {"-f", "MEM", "--size=2,2", "--band-count=3", "--burn=1,2",
                "mem_bad_burn3"},
       .expected_msg = "given there are 3 bands"},
      {.args = {"-f", "MEM", "--append", "--size=2,2", "mem_no_append"},
       .expected_msg = "-append option not supported"},
      {.args = {"-f", "MEM", "--size=0,2", "--bbox=0,0,1,1", "mem_zero_w"},
       .expected_msg = "height or width is null"},
      {.args = {"-f", "MEM", "--size=2,2", "--datatype=Byte", "--nodata=999",
                "mem_nd_range"},
       .expected_msg = "cannot be represented on its data type"},
      {.args = {"-f", "MEM", "--size=2,2", "--copy-metadata", "mem_no_like_md"},
       .expected_msg = "copy-metadata"},
      {.args = {"-f", "MEM", "--size=2,2", "--copy-overviews",
                "mem_no_like_ovr"},
       .expected_msg = "copy-overviews"},
      {.args = {"-f", "MEM", "--like", tmpl, "--size=4,4", "--copy-overviews",
                "mem_diff_dim_ovr"},
       .expected_msg = "have the same dimension"},
  };

  for (const auto& tc : cases) {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCreateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(tc.args));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_msg));
    CPLErrorReset();
  }

  // Invalid non-numeric nodata fails during argument parsing.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCreateAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({"-f", "MEM", "--size=2,2",
                                                "--datatype=Byte",
                                                "--nodata=bad", "mem_nd_bad"}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Value of 'nodata' should be"));
    CPLErrorReset();
  }

  // Create() failure on non-existent parent directory.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCreateAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GTiff", "--size=2,2", "/nonexistent_parent_dir_xyz/out.tif"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
