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
// Tests for GDALRasterOverviewAlgorithmRefresh
// (apps/gdalalg_raster_overview_refresh.cpp).
//
// Note: GTI_DRIVER_DISABLED_OR_PLUGIN is defined in google3, so GTI datasets
// are not available; tests cover standard GTiff, VRT, and GPKG paths.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_overview_refresh.cpp
//   https://gdal.org/programs/gdal_raster_overview_refresh.html

#include "third_party/gdal/apps/gdalalg_raster_overview_refresh.h"

#include <utime.h>

#include <cstdint>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_overview_refresh_test";

void CreateTiff(const std::string& path, int xsize, int ysize, uint8_t fill,
                bool with_gt, bool non_invertible_gt = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), xsize, ysize, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  if (with_gt) {
    double gt[6] = {0.0, 1.0, 0.0, static_cast<double>(ysize), 0.0, -1.0};
    if (non_invertible_gt) {
      gt[1] = 0.0;
      gt[5] = 0.0;
    }
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(xsize) * ysize, fill);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, xsize, ysize, pixels.data(), xsize,
                         ysize, GDT_Byte, 0, 0, nullptr));
}

void BuildOverviews(const std::string& path, const std::vector<int>& levels,
                    const char* resampling, bool external) {
  const int open_flags =
      GDAL_OF_RASTER | (external ? GDAL_OF_READONLY : GDAL_OF_UPDATE);
  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(path.c_str(), open_flags, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  ASSERT_EQ(CE_None, ds->BuildOverviews(
                         resampling, static_cast<int>(levels.size()),
                         levels.data(), 0, nullptr, nullptr, nullptr, nullptr));
}

class GdalalgRasterOverviewRefreshTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_PNG();
    GDALRegister_VRT();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterOverviewRefreshTest, BasicPropertiesAndValidationErrors) {
  GDALRasterOverviewAlgorithmRefresh alg;
  EXPECT_EQ("refresh", alg.GetName());
  EXPECT_EQ("Refresh overviews.", alg.GetDescription());
  EXPECT_EQ("/programs/gdal_raster_overview_refresh.html", alg.GetHelpURL());

  // 1. 0-band MEM dataset fails.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> empty_ds(
      mem_drv->Create("", 8, 8, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, empty_ds);
  {
    GDALRasterOverviewAlgorithmRefresh a;
    ASSERT_TRUE(a.GetArg("dataset")->Set(empty_ds.get()));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(a.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no raster band"));
    CPLErrorReset();
  }

  // 2. GTiff with no overviews fails.
  const std::string no_ovr = std::string(kTestDir) + "/no_ovr.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(no_ovr, 8, 8, 10, /*with_gt=*/true));
  {
    GDALRasterOverviewAlgorithmRefresh a;
    ASSERT_TRUE(a.ParseCommandLineArguments({no_ovr}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(a.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No overviews to refresh"));
    CPLErrorReset();
  }

  // 3. Requesting missing overview level fails.
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(no_ovr, {2}, "NEAREST", /*external=*/false));
  {
    GDALRasterOverviewAlgorithmRefresh a;
    ASSERT_TRUE(a.ParseCommandLineArguments({"--levels", "8", no_ovr}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(a.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find overview level"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterOverviewRefreshTest, FullRefreshExternalAndGpkg) {
  // 1. External .ovr refresh with modified pixel values and reused resampling.
  const std::string tif = std::string(kTestDir) + "/ext.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(tif, 8, 8, 10, /*with_gt=*/true));
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(tif, {2, 4}, "AVERAGE", /*external=*/true));

  // Modify base pixels to 80.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
                          nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> pixels(64, 80);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 8,
                                                      pixels.data(), 8, 8,
                                                      GDT_Byte, 0, 0, nullptr));
  }

  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--external", tif}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr);
    uint8_t val = 0;
    ASSERT_EQ(CE_None, ovr->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Byte,
                                     0, 0, nullptr));
    EXPECT_EQ(80, val);
  }

  // 2. GeoPackage overviews refresh (must exceed 256x256 tile size).
  const std::string gpkg = std::string(kTestDir) + "/raster.gpkg";
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    std::unique_ptr<GDALDataset> mem_ds(
        mem_drv->Create("", 512, 512, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    mem_ds->SetSpatialRef(&srs);
    double gt[6] = {0.0, 0.01, 0.0, 5.12, 0.0, -0.01};
    ASSERT_EQ(CE_None, mem_ds->SetGeoTransform(gt));
    std::vector<uint8_t> pixels(512 * 512, 42);
    ASSERT_EQ(CE_None, mem_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 512, 512, pixels.data(), 512, 512,
                           GDT_Byte, 0, 0, nullptr));
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, gpkg_drv);
    std::unique_ptr<GDALDataset> gpkg_ds(gpkg_drv->CreateCopy(
        gpkg.c_str(), mem_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, gpkg_ds);
  }
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(gpkg, {2}, "NEAREST", /*external=*/false));
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resampling", "average", "--levels", "2", gpkg}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgRasterOverviewRefreshTest, PartialRefreshBboxAndLike) {
  const std::string tif = std::string(kTestDir) + "/partial.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(tif, 8, 8, 10, /*with_gt=*/true));
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(tif, {2}, "NEAREST", /*external=*/false));

  // Update top-left 4x4 quadrant to 200.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
                          nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> quad(16, 200);
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, quad.data(),
                                             4, 4, GDT_Byte, 0, 0, nullptr));
  }

  // 1. Refresh only [0, 4, 4, 8] via --bbox.
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,4,4,8", tif}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr);
    std::vector<uint8_t> ovr_data(16, 0);
    ASSERT_EQ(CE_None, ovr->RasterIO(GF_Read, 0, 0, 4, 4, ovr_data.data(), 4, 4,
                                     GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(200, ovr_data[0]);
    EXPECT_EQ(10, ovr_data[15]);
  }

  // 2. Refresh bottom-right via --like with two source extents.
  const std::string like1 = std::string(kTestDir) + "/like1.tif";
  const std::string like2 = std::string(kTestDir) + "/like2.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(like1, 4, 4, 1, /*with_gt=*/true));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(like2, 4, 4, 1, /*with_gt=*/true));
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--like", like1, "--like", like2, tif}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 3. Error paths: missing GT, non-invertible GT, missing --like, --like no
  // GT.
  const std::string no_gt = std::string(kTestDir) + "/no_gt.tif";
  const std::string bad_gt = std::string(kTestDir) + "/bad_gt.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(no_gt, 8, 8, 1, /*with_gt=*/false));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(bad_gt, 8, 8, 1, /*with_gt=*/true,
                                     /*non_invertible_gt=*/true));
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(no_gt, {2}, "NEAREST", /*external=*/false));
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(bad_gt, {2}, "NEAREST", /*external=*/false));

  auto expect_fail = [](const std::vector<std::string>& args,
                        const std::string& msg) {
    GDALRasterOverviewAlgorithmRefresh a;
    ASSERT_TRUE(a.ParseCommandLineArguments(args));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(a.Run());
    if (!msg.empty()) {
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(msg));
    }
    CPLErrorReset();
  };

  expect_fail({"--bbox", "0,0,4,4", no_gt}, "no geotransform");
  expect_fail({"--bbox", "0,0,4,4", bad_gt}, "Cannot invert geotransform");
  expect_fail({"--like", like1, no_gt}, "no geotransform");
  expect_fail({"--like", like1, bad_gt}, "Cannot invert geotransform");
  expect_fail({"--like", std::string(kTestDir) + "/missing.tif", tif}, "");
  expect_fail({"--like", no_gt, tif}, "Source dataset has no geotransform");
}

TEST_F(GdalalgRasterOverviewRefreshTest, MismatchedOverviewCountAndTimestamps) {
  // 1. VRT with mismatched overview counts across bands fails PartialRefresh.
  const std::string src_a = std::string(kTestDir) + "/m_a.tif";
  const std::string src_b = std::string(kTestDir) + "/m_b.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(src_a, 8, 8, 1, /*with_gt=*/true));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(src_b, 8, 8, 1, /*with_gt=*/true));
  const std::string mismatch_vrt = std::string(kTestDir) + "/mismatch.vrt";
  VsiMemTempWrapper mismatch_file(
      mismatch_vrt,
      R"vrt(<VRTDataset rasterXSize="8" rasterYSize="8">
  <GeoTransform>0, 1, 0, 8, 0, -1</GeoTransform>
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/gdalalg_raster_overview_refresh_test/m_a.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
    <Overview>
      <SourceFilename relativeToVRT="0">/vsimem/gdalalg_raster_overview_refresh_test/m_b.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </Overview>
  </VRTRasterBand>
  <VRTRasterBand dataType="Byte" band="2">
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/gdalalg_raster_overview_refresh_test/m_a.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)vrt");
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--bbox", "0,0,8,8", mismatch_vrt}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Not same number of overviews"));
    CPLErrorReset();
  }

  // 2. --use-source-timestamp error paths in /vsimem/.
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--use-source-timestamp", src_a}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No overviews to refresh"));
    CPLErrorReset();
  }

  // Build external .ovr on src_a -> fails because src_a is GTiff, not VRT.
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(src_a, {2}, "NEAREST", /*external=*/true));
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--external", "--use-source-timestamp", src_a}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("only works on a VRT or GTI"));
    CPLErrorReset();
  }

  // VRT without .ovr -> "Cannot find ...ovr".
  const std::string vrt_no_ovr = std::string(kTestDir) + "/no_ovr.vrt";
  VsiMemTempWrapper vrt_no_ovr_file(
      vrt_no_ovr,
      R"vrt(<VRTDataset rasterXSize="8" rasterYSize="8">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/gdalalg_raster_overview_refresh_test/m_a.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
    <Overview>
      <SourceFilename relativeToVRT="0">/vsimem/gdalalg_raster_overview_refresh_test/m_b.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </Overview>
  </VRTRasterBand>
</VRTDataset>)vrt");
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--use-source-timestamp", vrt_no_ovr}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find"));
    CPLErrorReset();
  }

  // CPL_VSI_MEM_MTIME=0 triggers "Cannot get modification time".
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(vrt_no_ovr, {2}, "NEAREST", /*external=*/true));
  {
    CPLConfigOptionSetter mtime_zero("CPL_VSI_MEM_MTIME", "0",
                                     /*bSetOnlyIfUndefined=*/false);
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--external", "--use-source-timestamp", vrt_no_ovr}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot get modification time"));
    CPLErrorReset();
  }

  // Warped VRT has VRTWarpedRasterBand (not VRTSourcedRasterBand).
  const std::string warped_vrt = std::string(kTestDir) + "/warped.vrt";
  {
    std::unique_ptr<GDALDataset> src_ds(
        GDALDataset::Open(src_a.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
    char* argv[] = {const_cast<char*>("-of"), const_cast<char*>("VRT"),
                    nullptr};
    GDALWarpAppOptions* opts = GDALWarpAppOptionsNew(argv, nullptr);
    ASSERT_NE(nullptr, opts);
    std::unique_ptr<GDALDataset> warped(GDALDataset::FromHandle(
        GDALWarp(warped_vrt.c_str(), nullptr, 1, &hSrc, opts, nullptr)));
    GDALWarpAppOptionsFree(opts);
    ASSERT_NE(nullptr, warped);
  }
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(warped_vrt, {2}, "NEAREST", /*external=*/true));
  // VRTWarpedDataset stores implicit overviews inside the VRT XML rather than
  // creating a .vrt.ovr sidecar; create a sidecar so timestamp lookup passes
  // and reaches the VRTSourcedRasterBand check.
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(warped_vrt + ".ovr", 4, 4, 10, /*with_gt=*/false));
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--external", "--use-source-timestamp", warped_vrt}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not a VRTSourcedRasterBand"));
    CPLErrorReset();
  }

  // 3. Real files in TempDir for deterministic utime() timestamp comparison.
  const std::filesystem::path tmp_dir =
      std::filesystem::path(::testing::TempDir()) / "gdalalg_ovr_refresh_ts";
  std::filesystem::create_directories(tmp_dir);
  absl::Cleanup rm_tmp = [&tmp_dir] {
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  };

  const std::string tile_path = (tmp_dir / "tile.tif").string();
  const std::string vrt_path = (tmp_dir / "mosaic.vrt").string();
  const std::string ovr_path = vrt_path + ".ovr";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(tile_path, 8, 8, 10, /*with_gt=*/true));
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize=\"8\" rasterYSize=\"8\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
        "    <SimpleSource>\n"
        "      <SourceFilename relativeToVRT=\"0\">" +
        tile_path +
        "</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"8\" ySize=\"8\"/>\n"
        "      <DstRect xOff=\"-1\" yOff=\"-1\" xSize=\"10\" ySize=\"10\"/>\n"
        "    </SimpleSource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    VSILFILE* fp = VSIFOpenL(vrt_path.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(vrt_xml.size(),
              VSIFWriteL(vrt_xml.data(), 1, vrt_xml.size(), fp));
    ASSERT_EQ(0, VSIFCloseL(fp));
  }
  ASSERT_NO_FATAL_FAILURE(
      BuildOverviews(vrt_path, {2}, "NEAREST", /*external=*/true));

  // Set tile older than ovr -> no refresh needed, succeeds cleanly.
  struct utimbuf old_ts{1000, 1000};
  struct utimbuf ovr_ts{2000, 2000};
  ASSERT_EQ(0, utime(tile_path.c_str(), &old_ts));
  ASSERT_EQ(0, utime(ovr_path.c_str(), &ovr_ts));
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--use-source-timestamp", vrt_path}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Update tile pixel values and set tile newer than ovr -> triggers refresh.
  ASSERT_NO_FATAL_FAILURE(CreateTiff(tile_path, 8, 8, 123, /*with_gt=*/true));
  struct utimbuf new_ts{3000, 3000};
  ASSERT_EQ(0, utime(tile_path.c_str(), &new_ts));
  {
    GDALRasterOverviewAlgorithmRefresh alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--use-source-timestamp", vrt_path}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(vrt_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* ovr = ds->GetRasterBand(1)->GetOverview(0);
  ASSERT_NE(nullptr, ovr);
  uint8_t val = 0;
  ASSERT_EQ(CE_None, ovr->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Byte, 0,
                                   0, nullptr));
  EXPECT_EQ(123, val);
}

}  // namespace
}  // namespace autotest2
