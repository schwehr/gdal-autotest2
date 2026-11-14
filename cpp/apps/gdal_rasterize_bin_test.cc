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
// Tests for gdal_rasterize CLI binary (apps/gdal_rasterize_bin.cpp).
//
// Caveats:
//   --help/-h/--long-usage call ::std::exit(0) inside GDALArgumentParser in
//   //third_party/gdal and cannot be caught by the binary shim without process
//   exit; general options and Usage() are tested via --help-general and invalid
//   arguments instead.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_rasterize_bin.cpp
//   https://gdal.org/programs/gdal_rasterize.html

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdal_rasterize_bin_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdal_rasterize_bin::RunGdalRasterizeMain;
using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kPolygonGeoJson[] = R"json({
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "geometry": {
        "type": "Polygon",
        "coordinates": [[[2.0, 2.0], [2.0, 8.0], [8.0, 8.0], [8.0, 2.0], [2.0, 2.0]]]
      },
      "properties": { "val": 77 }
    }
  ]
})json";

void CreateBlankGeoTiff(const std::string& path, int width = 10,
                        int height = 10) {
  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), width, height, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::vector<uint8_t> zeros(width * height, 0);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, width, height, zeros.data(), width,
                         height, GDT_Byte, 0, 0, nullptr));
}

void CreateTwoLayerGeoPackage(const std::string& path) {
  RegisterOGRGeoPackage();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  OGRLayer* layer1 = ds->CreateLayer("layer1", &srs, wkbPoint, nullptr);
  ASSERT_THAT(layer1, NotNull());
  OGRLayer* layer2 = ds->CreateLayer("layer2", &srs, wkbPoint, nullptr);
  ASSERT_THAT(layer2, NotNull());
}

TEST(GdalRasterizeBinTest, GeneralCmdLineOptions) {
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize", "--version"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize", "--formats"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GTiff"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret =
        RunGdalRasterizeMain({"gdal_rasterize", "--format", "GTiff"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GeoTIFF"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize", "--help-general"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("--version"));
  }
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalRasterizeMain({"gdal_rasterize", "--config"}));
  }
}

TEST(GdalRasterizeBinTest, InvalidArgsAndUsageErrors) {
  constexpr char kDir[] = "/vsimem/gdal_rasterize_bin_invalid";
  constexpr char kSrcGpkg[] = "/vsimem/gdal_rasterize_bin_invalid/two.gpkg";
  constexpr char kDstTif[] = "/vsimem/gdal_rasterize_bin_invalid/out.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  // 1. Missing required args triggers Usage() and exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_rasterize"));
  }

  // 2. Unknown option triggers Usage() and exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize", "--unknown_opt"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_rasterize"));
  }

  // 3. Non-existent source vector exits 1.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalRasterizeMain(
                     {"gdal_rasterize", "-q", "-burn", "1", "-tr", "1", "1",
                      "/vsimem/gdal_rasterize_bin_invalid/missing.geojson",
                      kDstTif}));
  }

  // 4. Multi-layer input without -l or -sql triggers bUsageError == TRUE.
  ASSERT_NO_FATAL_FAILURE(CreateTwoLayerGeoPackage(kSrcGpkg));
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize", "-q", "-burn", "1",
                                          "-tr", "1", "1", "-te", "0", "0",
                                          "10", "10", kSrcGpkg, kDstTif});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_rasterize"));
  }
}

TEST(GdalRasterizeBinTest, OutputDriverValidationErrors) {
  constexpr char kDir[] = "/vsimem/gdal_rasterize_bin_drv";
  constexpr char kSrc[] = "/vsimem/gdal_rasterize_bin_drv/poly.geojson";
  constexpr char kDst[] = "/vsimem/gdal_rasterize_bin_drv/out.dat";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };
  VsiMemTempWrapper src_file(kSrc, kPolygonGeoJson);

  // 1. Unrecognized driver prints supported direct-write drivers and exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalRasterizeMain(
        {"gdal_rasterize", "-q", "-of", "NO_SUCH_DRIVER", "-burn", "1", "-tr",
         "1", "1", "-te", "0", "0", "10", "10", kSrc, kDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("not recognised"));
    EXPECT_THAT(err, HasSubstr("GTiff"));
  }

  // 2. Raster driver without DCAP_CREATE (e.g. PNG) exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalRasterizeMain({"gdal_rasterize", "-q", "-of", "PNG",
                                          "-burn", "1", "-tr", "1", "1", "-te",
                                          "0", "0", "10", "10", kSrc, kDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("does not support direct output file creation"));
  }

  // 3. Vector-only driver (GeoJSON) exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalRasterizeMain(
        {"gdal_rasterize", "-q", "-of", "GeoJSON", "-burn", "1", "-tr", "1",
         "1", "-te", "0", "0", "10", "10", kSrc, kDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("not recognised"));
  }
}

TEST(GdalRasterizeBinTest, CreateNewOutputRasterAndNonQuietProgress) {
  constexpr char kDir[] = "/vsimem/gdal_rasterize_bin_create";
  constexpr char kSrc[] = "/vsimem/gdal_rasterize_bin_create/poly.geojson";
  constexpr char kDst[] = "/vsimem/gdal_rasterize_bin_create/out.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };
  VsiMemTempWrapper src_file(kSrc, kPolygonGeoJson);

  ::testing::internal::CaptureStdout();
  const int ret = RunGdalRasterizeMain(
      {"gdal_rasterize", "-of", "GTiff", "-ot", "Byte", "-init", "0", "-burn",
       "77", "-tr", "1", "1", "-te", "0", "0", "10", "10", kSrc, kDst});
  const std::string out = ::testing::internal::GetCapturedStdout();
  EXPECT_EQ(0, ret);
  EXPECT_THAT(out, HasSubstr("done."));

  GDALRegister_GTiff();
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kDst, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(10, ds->GetRasterXSize());
  EXPECT_EQ(10, ds->GetRasterYSize());

  std::vector<uint8_t> pixels(100, 255);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 10, 10, pixels.data(),
                                           10, 10, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(0, pixels[0]);            // Outside polygon at (0,0)
  EXPECT_EQ(77, pixels[5 * 10 + 5]);  // Inside polygon at (5,5)
}

TEST(GdalRasterizeBinTest, UpdateExistingOutputRasterAndRuntimeFailure) {
  constexpr char kDir[] = "/vsimem/gdal_rasterize_bin_update";
  constexpr char kSrc[] = "/vsimem/gdal_rasterize_bin_update/poly.geojson";
  constexpr char kDst[] = "/vsimem/gdal_rasterize_bin_update/existing.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };
  VsiMemTempWrapper src_file(kSrc, kPolygonGeoJson);
  ASSERT_NO_FATAL_FAILURE(CreateBlankGeoTiff(kDst, 10, 10));

  // 1. Update existing raster with -a attribute burn in quiet mode.
  EXPECT_EQ(0, RunGdalRasterizeMain(
                   {"gdal_rasterize", "-q", "-a", "val", kSrc, kDst}));

  GDALRegister_GTiff();
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kDst, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  std::vector<uint8_t> pixels(100, 255);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 10, 10, pixels.data(),
                                           10, 10, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(0, pixels[0]);
  EXPECT_EQ(77, pixels[5 * 10 + 5]);
  ds.reset();

  // 2. Runtime failure (missing burn attribute) returns 1.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalRasterizeMain({"gdal_rasterize", "-q", "-a",
                                       "missing_attr", kSrc, kDst}));
  }
}

}  // namespace
}  // namespace autotest2
