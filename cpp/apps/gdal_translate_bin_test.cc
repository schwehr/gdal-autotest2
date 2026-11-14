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
// Tests for gdal_translate CLI binary (apps/gdal_translate_bin.cpp).
//
// Caveats:
//   --help/-h/--long-usage call ::std::exit(0) inside GDALArgumentParser in
//   //third_party/gdal and cannot be caught by the binary shim without process
//   exit; general options and Usage() are tested via --help-general and invalid
//   arguments instead.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_translate_bin.cpp
//   https://gdal.org/programs/gdal_translate.html

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdal_translate_bin_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalplugindriverproxy.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdal_translate_bin::RunGdalTranslateMain;
using ::testing::HasSubstr;
using ::testing::NotNull;

void CreateTinyGeoTiff(const std::string& path, int width = 8, int height = 8,
                       uint8_t fill_val = 55) {
  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), width, height, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  double gt[6] = {100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  std::vector<uint8_t> pixels(width * height, fill_val);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, width, height, pixels.data(), width,
                         height, GDT_Byte, 0, 0, nullptr));
}

void CreateMultiSubdatasetGpkg(const std::string& gpkg_path,
                               const std::vector<std::string>& sub_paths) {
  RegisterOGRGeoPackage();
  GDALRegister_GTiff();
  GDALRegister_PNG();
  GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(gpkg_drv, NotNull());

  for (size_t i = 0; i < sub_paths.size(); ++i) {
    std::unique_ptr<GDALDataset> src_ds(GDALDataset::Open(
        sub_paths[i].c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_THAT(src_ds, NotNull());

    CPLStringList options;
    options.SetNameValue("TILE_FORMAT", "PNG");
    options.SetNameValue("RASTER_TABLE",
                         ("sub_" + std::to_string(i + 1)).c_str());
    if (i > 0) {
      options.SetNameValue("APPEND_SUBDATASET", "YES");
    }

    std::unique_ptr<GDALDataset> dst_ds(
        gpkg_drv->CreateCopy(gpkg_path.c_str(), src_ds.get(), FALSE,
                             options.List(), nullptr, nullptr));
    ASSERT_THAT(dst_ds, NotNull());
  }
}

TEST(GdalTranslateBinTest, GeneralCmdLineOptions) {
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalTranslateMain({"gdal_translate", "--version"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalTranslateMain({"gdal_translate", "--formats"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GTiff"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret =
        RunGdalTranslateMain({"gdal_translate", "--format", "GTiff"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GeoTIFF"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalTranslateMain({"gdal_translate", "--help-general"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("--version"));
  }
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalTranslateMain({"gdal_translate", "--config"}));
  }
}

TEST(GdalTranslateBinTest, InvalidArgsAndDriverErrors) {
  constexpr char kDir[] = "/vsimem/gdal_translate_bin_errors";
  constexpr char kSrc[] = "/vsimem/gdal_translate_bin_errors/src.tif";
  constexpr char kDst[] = "/vsimem/gdal_translate_bin_errors/dst.tif";
  constexpr char kUnknownDst[] = "/vsimem/gdal_translate_bin_errors/dst.unk99";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoTiff(kSrc));

  // 1. Missing required args triggers Usage() -> exit 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalTranslateMain({"gdal_translate"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_translate"));
  }

  // 2. Unknown option triggers Usage() -> exit 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalTranslateMain({"gdal_translate", "--unknown_opt"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_translate"));
  }

  // 3. Unrecognized output format prints enabled drivers and exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalTranslateMain(
        {"gdal_translate", "-q", "-of", "NO_SUCH_DRIVER", kSrc, kDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("not recognised"));
    EXPECT_THAT(err, HasSubstr("GTiff:"));
  }

  // 4. Non-existent input dataset exits 1.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalTranslateMain(
                     {"gdal_translate", "-q",
                      "/vsimem/gdal_translate_bin_errors/missing.tif", kDst}));
  }

  // 5. ZARR CONVERT_TO_KERCHUNK_PARQUET_REFERENCE rewrites source prefix.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalTranslateMain(
                     {"gdal_translate", "-q", "-of", "ZARR", "-co",
                      "CONVERT_TO_KERCHUNK_PARQUET_REFERENCE=YES",
                      "/vsimem/gdal_translate_bin_errors/missing.zarr", kDst}));
  }

  // 6. Unknown output extension without -of fails with "Output driver not
  // found."
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret =
        RunGdalTranslateMain({"gdal_translate", "-q", kSrc, kUnknownDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Output driver not found."));
  }

  // 7. Conflicting -a_ullr and -a_gt triggers bUsageError == TRUE -> Usage().
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalTranslateMain(
        {"gdal_translate", "-q", "-a_ullr", "0", "10", "10", "0", "-a_gt", "0",
         "1", "0", "10", "0", "-1", kSrc, kDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_translate"));
  }

  // 8. Hidden/missing plugin output driver reports plugin diagnostic message.
  {
    auto* missing_proxy = new GDALPluginDriverProxy("gdal_MissingPlug.so");
    missing_proxy->SetDescription("MissingPlug");
    GetGDALDriverManager()->DeclareDeferredPluginDriver(missing_proxy);

    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalTranslateMain(
        {"gdal_translate", "-q", "-of", "MissingPlug", kSrc, kDst});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("not found but is known"));
  }
}

TEST(GdalTranslateBinTest, BasicTranslateAndVsistdout) {
  constexpr char kDir[] = "/vsimem/gdal_translate_bin_basic";
  constexpr char kSrc[] = "/vsimem/gdal_translate_bin_basic/src.tif";
  constexpr char kDst[] = "/vsimem/gdal_translate_bin_basic/dst.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoTiff(kSrc, 8, 8, 99));

  // 1. Non-quiet translation prints input dimensions and progress, with
  // explicit GDAL_MAX_DATASET_POOL_SIZE already set.
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalTranslateMain(
        {"gdal_translate", "--config", "GDAL_MAX_DATASET_POOL_SIZE", "100",
         "-srcwin", "1", "1", "4", "4", kSrc, kDst});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("Input file size is 8, 8"));
    EXPECT_THAT(out, HasSubstr("done."));
  }

  GDALRegister_GTiff();
  std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
      kDst, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(4, out_ds->GetRasterXSize());
  EXPECT_EQ(4, out_ds->GetRasterYSize());
  std::vector<uint8_t> pixels(16, 0);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 4, 4, pixels.data(), 4, 4, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(99, pixels[0]);
  out_ds.reset();

  // 2. Output to /vsistdout/ implies quiet mode and writes AAIGrid to stdout.
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalTranslateMain(
        {"gdal_translate", "-of", "AAIGrid", kDst, "/vsistdout/"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("ncols        4"));
    EXPECT_THAT(out, HasSubstr("99"));
  }
}

TEST(GdalTranslateBinTest, SubdatasetsWithoutAndWithSds) {
  constexpr char kDir[] = "/vsimem/gdal_translate_bin_sds";
  constexpr char kSub1[] = "/vsimem/gdal_translate_bin_sds/sub1.tif";
  constexpr char kSub2[] = "/vsimem/gdal_translate_bin_sds/sub2.tif";
  constexpr char kGpkg2[] = "/vsimem/gdal_translate_bin_sds/container2.gpkg";
  constexpr char kGpkg10[] = "/vsimem/gdal_translate_bin_sds/container10.gpkg";
  constexpr char kOutBase2[] = "/vsimem/gdal_translate_bin_sds/out2.tif";
  constexpr char kOutBase10[] = "/vsimem/gdal_translate_bin_sds/out10.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoTiff(kSub1, 6, 6, 11));
  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoTiff(kSub2, 6, 6, 22));

  ASSERT_NO_FATAL_FAILURE(CreateMultiSubdatasetGpkg(kGpkg2, {kSub1, kSub2}));

  // 1. Without -sds, subdataset container exits 1 with prompt message.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret =
        RunGdalTranslateMain({"gdal_translate", "-q", kGpkg2, kOutBase2});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Input file contains subdatasets."));
  }

  // 2. With -sds (< 10 subdatasets), writes out2_1.tif and out2_2.tif.
  {
    ::testing::internal::CaptureStdout();
    const int ret =
        RunGdalTranslateMain({"gdal_translate", "-sds", kGpkg2, kOutBase2});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("Input file size is 6, 6"));
  }

  GDALRegister_GTiff();
  for (const auto& [path, expected_val] :
       std::vector<std::pair<std::string, uint8_t>>{
           {"/vsimem/gdal_translate_bin_sds/out2_1.tif", 11},
           {"/vsimem/gdal_translate_bin_sds/out2_2.tif", 22}}) {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    uint8_t val = 0;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(expected_val, val);
  }

  // 3. With -sds and 10 subdatasets, uses %s_%002d naming (out10_01.tif..10).
  std::vector<std::string> ten_subs(10, kSub1);
  ASSERT_NO_FATAL_FAILURE(CreateMultiSubdatasetGpkg(kGpkg10, ten_subs));
  EXPECT_EQ(0, RunGdalTranslateMain(
                   {"gdal_translate", "-q", "-sds", kGpkg10, kOutBase10}));
  VSIStatBufL stat_buf;
  EXPECT_EQ(0,
            VSIStatL("/vsimem/gdal_translate_bin_sds/out10_01.tif", &stat_buf));
  EXPECT_EQ(0,
            VSIStatL("/vsimem/gdal_translate_bin_sds/out10_10.tif", &stat_buf));

  // 4. With -sds and unwritable output directory, GDALTranslate returns null
  //    and exits the subdataset loop cleanly.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, RunGdalTranslateMain({"gdal_translate", "-q", "-sds", kGpkg2,
                                       "/vsimem/no_such_dir_sds/out.tif"}));
  }

  // 5. With -sds and conflicting options, bUsageError == TRUE triggers Usage().
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalTranslateMain(
        {"gdal_translate", "-q", "-sds", "-a_ullr", "0", "10", "10", "0",
         "-a_gt", "0", "1", "0", "10", "0", "-1", kGpkg2, kOutBase2});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal_translate"));
  }
}

}  // namespace
}  // namespace autotest2
