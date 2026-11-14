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
// Tests for gdalwarp CLI binary (apps/gdalwarp_bin.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalwarp_bin.cpp
//   https://gdal.org/programs/gdalwarp.html

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdalwarp_bin_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdalwarp_bin::RunGdalWarpMain;
using ::testing::Contains;
using ::testing::NotNull;

void EnsureDriversRegistered() {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  GDALRegister_PNG();
  GDALRegister_VRT();
}

GDALDatasetUniquePtr CreateGeoTiff(const std::string& path, int width,
                                   int height, double ulx, double uly,
                                   double pixel_size, uint8_t fill_value,
                                   const char* srs_def = "EPSG:4326") {
  EnsureDriversRegistered();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return nullptr;
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), width, height, 1, GDT_Byte, nullptr));
  if (!ds) return nullptr;

  if (srs_def != nullptr) {
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (srs.SetFromUserInput(srs_def) != OGRERR_NONE) return nullptr;
    if (ds->SetSpatialRef(&srs) != CE_None) return nullptr;
  }

  double gt[6] = {ulx, pixel_size, 0.0, uly, 0.0, -pixel_size};
  if (ds->SetGeoTransform(gt) != CE_None) return nullptr;

  std::vector<uint8_t> data(static_cast<size_t>(width) * height, fill_value);
  if (ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, width, height, data.data(),
                                     width, height, GDT_Byte, 0, 0,
                                     nullptr) != CE_None) {
    return nullptr;
  }
  return ds;
}

TEST(GdalwarpBinTest, HelpFlagExitsZero) {
  EXPECT_EXIT(RunGdalWarpMain({"gdalwarp", "--help"}),
              ::testing::ExitedWithCode(0), "");
}

TEST(GdalwarpBinTest, GeneralOptionsAndUsageErrors) {
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", "--version"}));
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", "--help-general"}));
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", "--formats"}));
  // Exercise GDALExit's CPL_DEBUG=ON dataset/shared-list dump branch.
  EXPECT_EQ(0, RunGdalWarpMain(
                   {"gdalwarp", "--config", "CPL_DEBUG", "ON", "--version"}));

  WithQuietHandler quiet;
  EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp", "--format", "NO_SUCH_FORMAT"}));
  EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp"}));
  EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp", "--unknown-flag"}));
  // Exercise the branch where GDAL_MAX_DATASET_POOL_SIZE is already set.
  EXPECT_EQ(
      1, RunGdalWarpMain({"gdalwarp", "--config", "GDAL_MAX_DATASET_POOL_SIZE",
                          "50", "--unknown-flag"}));
}

TEST(GdalwarpBinTest, SameSourceAndDestWithOverwriteFails) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_same_test";
  constexpr char kSrc[] = "/vsimem/gdalwarp_bin_same_test/src.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src = CreateGeoTiff(kSrc, 16, 16, 0.0, 16.0, 0.1, 42);
    ASSERT_THAT(src, NotNull());
  }

  WithQuietHandler quiet;
  EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp", "-q", "-overwrite", kSrc, kSrc}));
}

TEST(GdalwarpBinTest, MissingSourceFilesFail) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_missing_test";
  constexpr char kSrc1[] = "/vsimem/gdalwarp_bin_missing_test/src1.tif";
  constexpr char kMissing[] = "/vsimem/gdalwarp_bin_missing_test/missing.tif";
  constexpr char kDst[] = "/vsimem/gdalwarp_bin_missing_test/dst.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src1 = CreateGeoTiff(kSrc1, 16, 16, 0.0, 16.0, 0.1, 10);
    ASSERT_THAT(src1, NotNull());
  }

  WithQuietHandler quiet;
  EXPECT_EQ(2, RunGdalWarpMain({"gdalwarp", "-q", kMissing, kDst}));
  // Second source missing: verifies loop closes previously opened sources.
  EXPECT_EQ(2, RunGdalWarpMain({"gdalwarp", "-q", kSrc1, kMissing, kDst}));
}

TEST(GdalwarpBinTest, BasicReprojectionAndVerifyOutput) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_basic_test";
  constexpr char kSrc[] = "/vsimem/gdalwarp_bin_basic_test/src.tif";
  constexpr char kDst[] = "/vsimem/gdalwarp_bin_basic_test/dst.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src = CreateGeoTiff(kSrc, 20, 20, 10.0, 20.0, 0.05, 77);
    ASSERT_THAT(src, NotNull());
  }

  EXPECT_EQ(0, RunGdalWarpMain(
                   {"gdalwarp", "-q", "-t_srs", "EPSG:3857", kSrc, kDst}));

  EnsureDriversRegistered();
  auto dst = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      kDst, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  ASSERT_THAT(dst, NotNull());
  EXPECT_GT(dst->GetRasterXSize(), 0);
  EXPECT_GT(dst->GetRasterYSize(), 0);

  OGRSpatialReference expected_srs;
  ASSERT_EQ(OGRERR_NONE, expected_srs.importFromEPSG(3857));
  const OGRSpatialReference* actual_srs = dst->GetSpatialRef();
  ASSERT_THAT(actual_srs, NotNull());
  EXPECT_TRUE(actual_srs->IsSame(&expected_srs));

  const int w = dst->GetRasterXSize();
  const int h = dst->GetRasterYSize();
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h, 0);
  ASSERT_EQ(CE_None,
            dst->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, w, h, pixels.data(),
                                            w, h, GDT_Byte, 0, 0, nullptr));
  EXPECT_THAT(pixels, Contains(77));
}

TEST(GdalwarpBinTest, WarpTermProgressMultipleSourcesAndPng) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_progress_test";
  constexpr char kSrc1[] = "/vsimem/gdalwarp_bin_progress_test/src1.tif";
  constexpr char kSrc2[] = "/vsimem/gdalwarp_bin_progress_test/src2.tif";
  constexpr char kDstTif[] = "/vsimem/gdalwarp_bin_progress_test/mosaic.tif";
  constexpr char kDstPng[] = "/vsimem/gdalwarp_bin_progress_test/out.png";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src1 = CreateGeoTiff(kSrc1, 16, 16, 0.0, 16.0, 0.1, 25);
    ASSERT_THAT(src1, NotNull());
    auto src2 = CreateGeoTiff(kSrc2, 16, 16, 1.6, 16.0, 0.1, 50);
    ASSERT_THAT(src2, NotNull());
  }

  // Non-quiet multi-source run triggers WarpTermProgress across two sources.
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", kSrc1, kSrc2, kDstTif}));

  // Non-quiet PNG output triggers CreateCopy progress with pszMessage=nullptr.
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", "-of", "PNG", kSrc1, kDstPng}));

  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(kDstPng, &stat_buf));
}

TEST(GdalwarpBinTest, StreamingStdoutOutput) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_stdout_test";
  constexpr char kSrc[] = "/vsimem/gdalwarp_bin_stdout_test/src.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src = CreateGeoTiff(kSrc, 8, 8, 0.0, 8.0, 0.1, 15);
    ASSERT_THAT(src, NotNull());
  }

  EXPECT_EQ(0,
            RunGdalWarpMain({"gdalwarp", "-of", "VRT", kSrc, "/vsistdout/"}));
}

TEST(GdalwarpBinTest, ExistingDestinationHandling) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_existing_test";
  constexpr char kSrc[] = "/vsimem/gdalwarp_bin_existing_test/src.tif";
  constexpr char kDst[] = "/vsimem/gdalwarp_bin_existing_test/dst.tif";
  constexpr char kDstPng[] = "/vsimem/gdalwarp_bin_existing_test/dst.png";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src = CreateGeoTiff(kSrc, 16, 16, 0.0, 16.0, 0.1, 90);
    ASSERT_THAT(src, NotNull());
    auto dst = CreateGeoTiff(kDst, 16, 16, 0.0, 16.0, 0.1, 0);
    ASSERT_THAT(dst, NotNull());

    EnsureDriversRegistered();
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_THAT(mem_drv, NotNull());
    GDALDriver* png_drv = GetGDALDriverManager()->GetDriverByName("PNG");
    ASSERT_THAT(png_drv, NotNull());
    GDALDatasetUniquePtr mem_ds(
        mem_drv->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_THAT(mem_ds, NotNull());
    EXPECT_EQ(CE_None, mem_ds->GetRasterBand(1)->Fill(5));
    GDALDatasetUniquePtr png_ds(png_drv->CreateCopy(
        kDstPng, mem_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_THAT(png_ds, NotNull());
  }

  // 1. Existing destination with -of GTiff (bCreateOutput) without -overwrite
  //    fails.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1,
              RunGdalWarpMain({"gdalwarp", "-q", "-of", "GTiff", kSrc, kDst}));
  }

  // 2. Existing destination with -overwrite replaces it.
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", "-q", "-overwrite", kSrc, kDst}));

  // 3. Existing destination with -co APPEND_SUBDATASET=YES exercises the append
  //    subdataset path.
  EXPECT_EQ(0, RunGdalWarpMain({"gdalwarp", "-q", "-of", "GTiff", "-co",
                                "APPEND_SUBDATASET=YES", kSrc, kDst}));

  // 4. Existing read-only-only format (PNG) without -overwrite fails because
  //    it cannot be opened in update mode.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp", "-q", kSrc, kDstPng}));
  }
}

TEST(GdalwarpBinTest, UsageErrorAndWarpFailure) {
  constexpr char kDir[] = "/vsimem/gdalwarp_bin_err_test";
  constexpr char kSrc[] = "/vsimem/gdalwarp_bin_err_test/src.tif";
  constexpr char kDst[] = "/vsimem/gdalwarp_bin_err_test/dst.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  {
    auto src = CreateGeoTiff(kSrc, 16, 16, 0.0, 16.0, 0.1, 33);
    ASSERT_THAT(src, NotNull());
  }

  WithQuietHandler quiet;
  // Conflicting -tr and -ts sets bUsageError = TRUE in GDALWarp.
  EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp", "-q", "-tr", "1", "1", "-ts", "10",
                                "10", kSrc, kDst}));
  // Invalid target SRS causes GDALWarp to fail without bUsageError.
  EXPECT_EQ(1, RunGdalWarpMain({"gdalwarp", "-q", "-t_srs", "INVALID_SRS_DEF",
                                kSrc, kDst}));
}

}  // namespace
}  // namespace autotest2
