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
// Tests for GDALRasterPixelInfoAlgorithm (apps/gdalalg_raster_pixel_info.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_pixel_info.cpp

#include "third_party/gdal/apps/gdalalg_raster_pixel_info.h"

#include <unistd.h>

#include <array>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_pixel_info_test";

class GdalalgRasterPixelInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

std::string GetOutputString(const GDALRasterPixelInfoAlgorithm& alg) {
  const auto* arg = alg.GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
  return arg ? arg->Get<std::string>() : "";
}

std::unique_ptr<GDALDataset> CreateMemRaster(GDALDataType dt, bool with_srs,
                                             int epsg, bool with_gt) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(drv->Create("", 4, 4, 2, dt, nullptr));
  if (!ds) return nullptr;
  if (with_gt) {
    GDALGeoTransform gt(10.0, 2.0, 0.0, 50.0, 0.0, -2.0);
    if (ds->SetGeoTransform(gt) != CE_None) return nullptr;
  }
  if (with_srs) {
    OGRSpatialReference srs;
    if (epsg > 0) {
      if (srs.importFromEPSG(epsg) != OGRERR_NONE) return nullptr;
    } else {
      // Custom CRS without EPSG authority code to exercise poCTToWGS84.
      if (srs.SetFromUserInput("+proj=merc +datum=WGS84 +units=m +no_defs") !=
          OGRERR_NONE) {
        return nullptr;
      }
    }
    if (ds->SetSpatialRef(&srs) != CE_None) return nullptr;
  }

  if (dt == GDT_Byte) {
    std::vector<uint8_t> b1(16);
    std::vector<uint8_t> b2(16);
    for (int i = 0; i < 16; ++i) {
      b1[i] = static_cast<uint8_t>(10 + i);
      b2[i] = static_cast<uint8_t>(100 + i);
    }
    if (ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, b1.data(), 4, 4,
                                       GDT_Byte, 0, 0, nullptr) != CE_None ||
        ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 4, 4, b2.data(), 4, 4,
                                       GDT_Byte, 0, 0, nullptr) != CE_None) {
      return nullptr;
    }
    ds->GetRasterBand(1)->SetScale(2.0);
    ds->GetRasterBand(1)->SetOffset(5.0);
  } else if (dt == GDT_Float32) {
    std::vector<float> b1(16, 1.5f);
    std::vector<float> b2(16, 2.5f);
    if (ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, b1.data(), 4, 4,
                                       GDT_Float32, 0, 0, nullptr) != CE_None ||
        ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 4, 4, b2.data(), 4, 4,
                                       GDT_Float32, 0, 0, nullptr) != CE_None) {
      return nullptr;
    }
  } else if (dt == GDT_CFloat32) {
    std::vector<std::complex<float>> b1(16, std::complex<float>(3.0f, -4.0f));
    std::vector<std::complex<float>> b2(16, std::complex<float>(5.0f, 6.0f));
    if (ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, b1.data(), 4, 4,
                                       GDT_CFloat32, 0, 0,
                                       nullptr) != CE_None ||
        ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 4, 4, b2.data(), 4, 4,
                                       GDT_CFloat32, 0, 0,
                                       nullptr) != CE_None) {
      return nullptr;
    }
  }
  return ds;
}

TEST_F(GdalalgRasterPixelInfoTest, BasicPropertiesAndValidationErrors) {
  GDALRasterPixelInfoAlgorithm alg;
  EXPECT_EQ("pixel-info", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_pixel_info.html", alg.GetHelpURL());

  auto ds = CreateMemRaster(GDT_Byte, false, 0, false);
  ASSERT_NE(nullptr, ds);

  // 1. Odd number of position values fails.
  {
    GDALRasterPixelInfoAlgorithm bad;
    ASSERT_TRUE(bad.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds.get()));
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.ParseCommandLineArguments({"-p", "1"}));
  }

  // 2. Empty position when not called from command line fails Run().
  {
    GDALRasterPixelInfoAlgorithm bad;
    ASSERT_TRUE(bad.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds.get()));
    WithQuietHandler quiet;
    EXPECT_FALSE(bad.Run());
  }

  // 3. Overview requested when dataset has 0 overviews fails validation.
  {
    GDALRasterPixelInfoAlgorithm bad;
    ASSERT_TRUE(bad.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds.get()));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        bad.ParseCommandLineArguments({"--overview", "0", "-p", "0", "0"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has no overviews"));
  }
}

TEST_F(GdalalgRasterPixelInfoTest, GeoJSONAndCsvAcrossTypesAndCRS) {
  // 1. Byte raster with EPSG:4326, scale/offset, and GeoJSON output.
  auto ds_4326 = CreateMemRaster(GDT_Byte, /*with_srs=*/true, 4326, true);
  ASSERT_NE(nullptr, ds_4326);
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_4326.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "geojson", "--pos", "1", "1", "2", "2"}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("urn:ogc:def:crs:OGC:1.3:CRS84"));
    EXPECT_THAT(out, HasSubstr("\"raw_value\":15"));
    EXPECT_THAT(out, HasSubstr("\"unscaled_value\":35"));
  }

  // 2. CSV output on Byte raster including out-of-bounds pixel.
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_4326.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "csv", "-b", "1", "-p", "0", "0", "99", "99"}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("band_1_raw_value,band_1_unscaled_value"));
    EXPECT_THAT(out, HasSubstr("10,25"));
    EXPECT_THAT(out, HasSubstr("99,99,\"\",99,99,,"));
  }

  // 3. CSV output with SetCalledFromCommandLine() and non-empty position.
  {
    GDALRasterPixelInfoAlgorithm alg;
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_4326.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "csv", "-b", "1", "-p", "0", "0"}));
    ASSERT_TRUE(alg.Run());
  }

  // 4. Float32 raster with EPSG:32632 and dataset/EPSG:4326 position-crs.
  auto ds_utm = CreateMemRaster(GDT_Float32, /*with_srs=*/true, 32632, true);
  ASSERT_NE(nullptr, ds_utm);
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_utm.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position-crs", "dataset", "-r", "bilinear", "-p", "11", "49"}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("urn:ogc:def:crs:EPSG::32632"));
    EXPECT_THAT(out, HasSubstr("\"raw_value\":1.5"));
  }
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_utm.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position-crs", "EPSG:32632", "-p", "11", "49"}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(GetOutputString(alg), HasSubstr("\"raw_value\":1.5"));
  }

  // 5. Custom CRS without EPSG authority exercises poCTToWGS84.
  auto ds_custom = CreateMemRaster(GDT_Float32, /*with_srs=*/true, 0, true);
  ASSERT_NE(nullptr, ds_custom);
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_custom.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-p", "1", "1"}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(GetOutputString(alg),
                HasSubstr("urn:ogc:def:crs:OGC:1.3:CRS84"));
  }

  // 6. Complex raster (GDT_CFloat32) in GeoJSON and CSV, with null geometry.
  auto ds_cpx = CreateMemRaster(GDT_CFloat32, /*with_srs=*/false, 0, false);
  ASSERT_NE(nullptr, ds_cpx);
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_cpx.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-p", "1", "1"}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("\"real\":3"));
    EXPECT_THAT(out, HasSubstr("\"imaginary\":-4"));
    EXPECT_THAT(out, HasSubstr("\"geometry\":null"));
  }
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_cpx.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "csv", "-p", "1", "1"}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("band_1_real_value,band_1_imaginary_value"));
    EXPECT_THAT(out, HasSubstr("3,-4"));
  }
}

TEST_F(GdalalgRasterPixelInfoTest, PositionCrsErrorPaths) {
  // 1. No CRS when position-crs != pixel.
  auto ds_no_srs = CreateMemRaster(GDT_Byte, false, 0, true);
  ASSERT_NE(nullptr, ds_no_srs);
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_no_srs.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position-crs", "dataset", "-p", "0", "0"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 2. CRS present but no geotransform.
  auto ds_no_gt = CreateMemRaster(GDT_Byte, true, 4326, false);
  ASSERT_NE(nullptr, ds_no_gt);
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_no_gt.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position-crs", "dataset", "-p", "0", "0"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 3. Non-invertible geotransform.
  auto ds_bad_gt = CreateMemRaster(GDT_Byte, true, 4326, true);
  ASSERT_NE(nullptr, ds_bad_gt);
  GDALGeoTransform zero_gt(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  ASSERT_EQ(CE_None, ds_bad_gt->SetGeoTransform(zero_gt));
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_bad_gt.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position-crs", "dataset", "-p", "0", "0"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
}

TEST_F(GdalalgRasterPixelInfoTest, OverviewAndVrtLocationInfo) {
  // 1. GTiff with overviews: valid overview and out-of-range overview errors.
  const std::string tif_path = std::string(kTestDir) + "/ovr.tif";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds(
        drv->Create(tif_path.c_str(), 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> pixels(64, 77);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 8,
                                                      pixels.data(), 8, 8,
                                                      GDT_Byte, 0, 0, nullptr));
    constexpr std::array<int, 2> kLevels = {2, 4};
    ASSERT_EQ(CE_None,
              ds->BuildOverviews("NEAREST", kLevels.size(), kLevels.data(), 0,
                                 nullptr, nullptr, nullptr, nullptr));
  }

  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tif_path, "--overview", "0", "-p", "7", "7"}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(GetOutputString(alg), HasSubstr("\"raw_value\":77"));
  }

  {
    GDALRasterPixelInfoAlgorithm alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {tif_path, "--overview", "5", "-p", "0", "0"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("overview levels"));
  }

  // 2. VRT dataset exercising LocationInfo XML parsing and raw LocationInfo.
  const std::string vrt_path = std::string(kTestDir) + "/src.vrt";
  VsiMemTempWrapper vrt_file(
      vrt_path,
      "<VRTDataset rasterXSize=\"4\" rasterYSize=\"4\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename relativeToVRT=\"0\">" +
          tif_path +
          "</SourceFilename>\n"
          "      <SourceBand>1</SourceBand>\n"
          "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"4\" ySize=\"4\"/>\n"
          "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"4\" ySize=\"4\"/>\n"
          "    </SimpleSource>\n"
          "  </VRTRasterBand>\n"
          "</VRTDataset>\n");

  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({vrt_path, "-p", "1", "1"}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("\"files\""));
    EXPECT_THAT(out, HasSubstr("ovr.tif"));
  }

  // Non-LocationInfo metadata on MEM band exercises else branch of
  // LocationInfo.
  auto mem_ds = CreateMemRaster(GDT_Byte, false, 0, false);
  ASSERT_NE(nullptr, mem_ds);
  ASSERT_EQ(CE_None, mem_ds->GetRasterBand(1)->SetMetadataItem(
                         "Pixel_1_1", "raw_loc", "LocationInfo"));
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(mem_ds.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-p", "1", "1"}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(GetOutputString(alg),
                HasSubstr("\"location_info\":\"raw_loc\""));
  }

  // 3. Band 1 has an overview while band 2 does not -> error on band 2.
  const std::string no_ovr_tif = std::string(kTestDir) + "/no_ovr_src.tif";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds(
        drv->Create(no_ovr_tif.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> pixels(16, 42);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4,
                                                      pixels.data(), 4, 4,
                                                      GDT_Byte, 0, 0, nullptr));
  }
  const std::string mixed_ovr_vrt = std::string(kTestDir) + "/mixed_ovr.vrt";
  VsiMemTempWrapper mixed_vrt_file(
      mixed_ovr_vrt,
      "<VRTDataset rasterXSize=\"4\" rasterYSize=\"4\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename relativeToVRT=\"0\">" +
          tif_path +
          "</SourceFilename>\n"
          "      <SourceBand>1</SourceBand>\n"
          "    </SimpleSource>\n"
          "    <Overview>\n"
          "      <SourceFilename relativeToVRT=\"0\">" +
          tif_path +
          "</SourceFilename>\n"
          "      <SourceBand>1</SourceBand>\n"
          "    </Overview>\n"
          "  </VRTRasterBand>\n"
          "  <VRTRasterBand dataType=\"Byte\" band=\"2\">\n"
          "    <SimpleSource>\n"
          "      <SourceFilename relativeToVRT=\"0\">" +
          no_ovr_tif +
          "</SourceFilename>\n"
          "      <SourceBand>1</SourceBand>\n"
          "    </SimpleSource>\n"
          "  </VRTRasterBand>\n"
          "</VRTDataset>\n");
  {
    GDALRasterPixelInfoAlgorithm alg;
    WithQuietHandler quiet;
    CPLErrorReset();
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {mixed_ovr_vrt, "--band", "1,2", "--overview", "0", "-p", "1", "1"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot get overview 0 of band 2"));
    CPLErrorReset();
  }
}

class StdinRedirector {
 public:
  explicit StdinRedirector(std::string_view input) {
    saved_fd_ = dup(STDIN_FILENO);
    if (saved_fd_ < 0) return;
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
      close(saved_fd_);
      saved_fd_ = -1;
      return;
    }
    const ssize_t written = write(fds[1], input.data(), input.size());
    close(fds[1]);
    if (dup2(fds[0], STDIN_FILENO) < 0) {
      close(fds[0]);
      close(saved_fd_);
      saved_fd_ = -1;
      return;
    }
    close(fds[0]);
    std::clearerr(stdin);
    ok_ = (written == static_cast<ssize_t>(input.size()));
  }

  ~StdinRedirector() {
    if (saved_fd_ >= 0) {
      dup2(saved_fd_, STDIN_FILENO);
      close(saved_fd_);
      std::clearerr(stdin);
    }
  }

  bool ok() const { return ok_; }

 private:
  int saved_fd_ = -1;
  bool ok_ = false;
};

TEST_F(GdalalgRasterPixelInfoTest, StdinCoordinatesAndUnconvertibleCrs) {
  auto ds_4326 = CreateMemRaster(GDT_Byte, /*with_srs=*/true, 4326, true);
  ASSERT_NE(nullptr, ds_4326);

  // 1. Stdin input with extra_content tokens and trailing whitespace.
  {
    StdinRedirector redirect("1 1 extra info \n\n");
    ASSERT_TRUE(redirect.ok());
    GDALRasterPixelInfoAlgorithm alg;
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_4326.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({}));
    ASSERT_TRUE(alg.Run());
    const std::string out = GetOutputString(alg);
    EXPECT_THAT(out, HasSubstr("\"extra_content\":\"extra info\""));
    EXPECT_THAT(out, HasSubstr("\"raw_value\":15"));
  }

  // 2. Stdin input with fewer than 2 tokens triggers error on line 1.
  {
    StdinRedirector redirect("only_one\n");
    ASSERT_TRUE(redirect.ok());
    GDALRasterPixelInfoAlgorithm alg;
    alg.SetCalledFromCommandLine();
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_4326.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "csv"}));
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    EXPECT_FALSE(alg.Run());
    (void)::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_THAT(err, HasSubstr("Not enough values at line 1"));
  }

  // 3. Incompatible CRS between dataset (LOCAL_CS) and position-crs fails
  //    OGRCreateCoordinateTransformation().
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  auto ds_local = std::unique_ptr<GDALDataset>(
      mem_drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds_local);
  GDALGeoTransform gt(0.0, 1.0, 0.0, 4.0, 0.0, -1.0);
  ASSERT_EQ(CE_None, ds_local->SetGeoTransform(gt));
  OGRSpatialReference local_srs;
  ASSERT_EQ(OGRERR_NONE, local_srs.SetLocalCS("local"));
  ASSERT_EQ(CE_None, ds_local->SetSpatialRef(&local_srs));
  {
    GDALRasterPixelInfoAlgorithm alg;
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(ds_local.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--position-crs", "EPSG:4326", "-p", "0", "0"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
