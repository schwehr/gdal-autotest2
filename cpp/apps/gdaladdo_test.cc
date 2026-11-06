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
// Tests for gdaladdo (apps/gdaladdo.cpp).
//
// Caveats:
//   gdaladdo_lib compiles apps/gdaladdo.cpp with
//   -DGTI_DRIVER_DISABLED_OR_PLUGIN and -DWITHOUT_CPLDEBUG.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdaladdo.cpp

#include <utime.h>

#include <cstdint>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdaladdo_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdaladdo::RunGdalAddoMain;
using ::testing::NotNull;

void EnsureDriversRegistered() {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  GDALRegister_COG();
  GDALRegister_VRT();
}

std::unique_ptr<GDALDataset> CreateGeoTiff(const std::string& path,
                                           int width = 100, int height = 100,
                                           int bands = 1,
                                           bool with_geotransform = true) {
  EnsureDriversRegistered();
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), width, height, bands, GDT_Byte, nullptr));
  if (!ds) return nullptr;
  if (with_geotransform) {
    double gt[6] = {1000.0, 1.0, 0.0, 2000.0, 0.0, -1.0};
    ds->SetGeoTransform(gt);
  }
  for (int b = 1; b <= bands; ++b) {
    std::vector<uint8_t> data(width * height, static_cast<uint8_t>(b * 10));
    if (ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, width, height,
                                       data.data(), width, height, GDT_Byte, 0,
                                       0, nullptr) != CE_None) {
      return nullptr;
    }
  }
  ds->FlushCache();
  return ds;
}

TEST(GdaladdoTest, NonExistentFileFails) {
  WithQuietHandler quiet;
  EXPECT_EQ(2, RunGdalAddoMain(
                   {"gdaladdo", "/vsimem/gdaladdo_test/does_not_exist.tif"}));
}

TEST(GdaladdoTest, InvalidArguments) {
  constexpr char kDir[] = "/vsimem/gdaladdo_invalid";
  constexpr char kPath[] = "/vsimem/gdaladdo_invalid/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath), NotNull());

  WithQuietHandler quiet;
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", kPath, "abc"}));
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", kPath, "0"}));
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", kPath, "-2"}));
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", "-b", "0", kPath, "2"}));
}

TEST(GdaladdoTest, BuildOverviewsBasicAndClean) {
  constexpr char kDir[] = "/vsimem/gdaladdo_basic";
  constexpr char kPath[] = "/vsimem/gdaladdo_basic/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1), NotNull());

  EXPECT_EQ(
      0, RunGdalAddoMain({"gdaladdo", "-q", "-r", "nearest", kPath, "2", "4"}));

  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        kPath, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
    EXPECT_EQ(50, ds->GetRasterBand(1)->GetOverview(0)->GetXSize());
    EXPECT_EQ(25, ds->GetRasterBand(1)->GetOverview(1)->GetXSize());
  }

  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-clean", kPath}));

  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        kPath, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
  }
}

TEST(GdaladdoTest, BuildOverviewLevelOneAndReuseOverviews) {
  constexpr char kDir[] = "/vsimem/gdaladdo_reuse";
  constexpr char kPath[] = "/vsimem/gdaladdo_reuse/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1), NotNull());

  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kPath, "1", "2"}));

  // Re-run without specifying levels: reuses existing levels.
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kPath}));

  // Re-run without resampling: reuses existing resampling method.
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kPath, "2"}));
}

TEST(GdaladdoTest, BuildOverviewsExternalReadOnly) {
  constexpr char kDir[] = "/vsimem/gdaladdo_ro";
  constexpr char kPath[] = "/vsimem/gdaladdo_ro/test.tif";
  constexpr char kOvrPath[] = "/vsimem/gdaladdo_ro/test.tif.ovr";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1), NotNull());

  EXPECT_EQ(0, RunGdalAddoMain(
                   {"gdaladdo", "-q", "-ro", "-r", "average", kPath, "2"}));

  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(kOvrPath, &sStat));
}

TEST(GdaladdoTest, MinSizeAutomaticLevels) {
  constexpr char kDir[] = "/vsimem/gdaladdo_minsize";
  constexpr char kPath[] = "/vsimem/gdaladdo_minsize/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 1000, 1000, 1), NotNull());

  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-minsize", "200", kPath}));

  {
    EnsureDriversRegistered();
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        kPath, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_GT(ds->GetRasterBand(1)->GetOverviewCount(), 0);
  }
}

TEST(GdaladdoTest, PartialRefreshFromProjWin) {
  constexpr char kDir[] = "/vsimem/gdaladdo_projwin";
  constexpr char kPath[] = "/vsimem/gdaladdo_projwin/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1, /*with_geotransform=*/true),
              NotNull());
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kPath, "2", "4"}));

  // Partial refresh with explicit levels
  EXPECT_EQ(0,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "1010", "1990", "1050", "1950", kPath, "2"}));

  // Partial refresh without explicit levels
  EXPECT_EQ(0,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "1010", "1990", "1050", "1950", kPath}));

  // Partial refresh with -minsize
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-minsize", "30",
                                "--partial-refresh-from-projwin", "1010",
                                "1990", "1050", "1950", kPath}));
}

TEST(GdaladdoTest, PartialRefreshFromProjWinNoGeotransformFails) {
  constexpr char kDir[] = "/vsimem/gdaladdo_nogt";
  constexpr char kPath[] = "/vsimem/gdaladdo_nogt/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1, /*with_geotransform=*/false),
              NotNull());
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kPath, "2"}));

  WithQuietHandler quiet;
  EXPECT_EQ(1,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "0", "100", "50", "50", kPath, "2"}));
}

TEST(GdaladdoTest, PartialRefreshFromSourceExtent) {
  constexpr char kDir[] = "/vsimem/gdaladdo_srcextent";
  constexpr char kSrc1[] = "/vsimem/gdaladdo_srcextent/src1.tif";
  constexpr char kSrc2[] = "/vsimem/gdaladdo_srcextent/src2.tif";
  constexpr char kMain[] = "/vsimem/gdaladdo_srcextent/main.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };

  ASSERT_THAT(CreateGeoTiff(kSrc1, 50, 50, 1), NotNull());
  ASSERT_THAT(CreateGeoTiff(kSrc2, 50, 50, 1), NotNull());
  ASSERT_THAT(CreateGeoTiff(kMain, 100, 100, 1), NotNull());

  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kMain, "2", "4"}));

  std::string sources = std::string(kSrc1) + "," + kSrc2;
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q",
                                "--partial-refresh-from-source-extent", sources,
                                kMain, "2"}));
}

TEST(GdaladdoTest, MissingRequiredFilenameFails) {
  WithQuietHandler quiet;
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo"}));
}

TEST(GdaladdoTest, CogLayoutErrorAndIgnoreBreak) {
  constexpr char kDir[] = "/vsimem/gdaladdo_cog";
  constexpr char kSrcPath[] = "/vsimem/gdaladdo_cog/src.tif";
  constexpr char kCogPath[] = "/vsimem/gdaladdo_cog/cog.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  EnsureDriversRegistered();
  auto* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());
  auto* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
  ASSERT_THAT(cog_drv, NotNull());

  {
    auto src = std::unique_ptr<GDALDataset>(
        gtiff_drv->Create(kSrcPath, 32, 32, 1, GDT_Byte, nullptr));
    ASSERT_THAT(src, NotNull());
    EXPECT_EQ(CE_None, src->GetRasterBand(1)->Fill(10));
    CPLStringList co;
    co.SetNameValue("COMPRESS", "LZW");
    auto cog = std::unique_ptr<GDALDataset>(cog_drv->CreateCopy(
        kCogPath, src.get(), FALSE, co.List(), nullptr, nullptr));
    ASSERT_THAT(cog, NotNull());
  }

  // In update mode without ignore flag: gdaladdo detects COG layout and exits
  // with 1.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", kCogPath, "2"}));
  }

  // With IGNORE_COG_LAYOUT_BREAK: succeeds.
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-oo",
                                "IGNORE_COG_LAYOUT_BREAK=YES", kCogPath, "2"}));

  // Clean COG: automatically sets IGNORE_COG_LAYOUT_BREAK=YES.
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-clean", kCogPath}));
}

TEST(GdaladdoTest, PartialRefreshFromSourceTimestampVRT) {
  const std::filesystem::path temp_dir =
      std::filesystem::path(::testing::TempDir()) / "gdaladdo_srcts";
  std::filesystem::create_directories(temp_dir);
  const std::string kSrc = (temp_dir / "tile.tif").string();
  const std::string kVrt = (temp_dir / "mosaic.vrt").string();
  absl::Cleanup clean_dir = [&temp_dir] {
    std::filesystem::remove_all(temp_dir);
  };

  ASSERT_THAT(CreateGeoTiff(kSrc, 50, 50, 1), NotNull());

  // Create VRT
  EnsureDriversRegistered();
  auto* vrt_drv = GetGDALDriverManager()->GetDriverByName("VRT");
  ASSERT_THAT(vrt_drv, NotNull());
  auto vrt_ds = std::unique_ptr<VRTDataset>(static_cast<VRTDataset*>(
      vrt_drv->Create(kVrt.c_str(), 50, 50, 0, GDT_Byte, nullptr)));
  ASSERT_THAT(vrt_ds, NotNull());
  double gt[6] = {1000.0, 1.0, 0.0, 2000.0, 0.0, -1.0};
  vrt_ds->SetGeoTransform(GDALGeoTransform(gt));
  vrt_ds->AddBand(GDT_Byte, nullptr);
  auto* band = static_cast<VRTSourcedRasterBand*>(vrt_ds->GetRasterBand(1));
  ASSERT_THAT(band, NotNull());

  auto src_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(kSrc.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(src_ds, NotNull());
  band->AddSimpleSource(src_ds->GetRasterBand(1), 0, 0, 50, 50, 0, 0, 50, 50);
  // Source with negative offsets and overflowing extent to test clipping
  // adjustments
  band->AddSimpleSource(src_ds->GetRasterBand(1), 0, 0, 50, 50, -5, -5, 60, 60);
  // Source completely outside extent to test skipping
  band->AddSimpleSource(src_ds->GetRasterBand(1), 0, 0, 50, 50, 200, 200, 50,
                        50);
  vrt_ds->FlushCache(false);
  vrt_ds.reset();
  src_ds.reset();

  // Error case: .ovr doesn't exist yet
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", "-q",
                                  "--partial-refresh-from-source-timestamp",
                                  kVrt, "2"}));
  }

  // Build external overviews for VRT (non-quiet to test progress reporting)
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-ro", kVrt, "2"}));

  // First run: source is not newer than overviews (test both verbose and
  // quiet)
  EXPECT_EQ(
      0, RunGdalAddoMain({"gdaladdo", "--partial-refresh-from-source-timestamp",
                          kVrt, "2"}));
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q",
                                "--partial-refresh-from-source-timestamp", kVrt,
                                "2"}));

  // Update source file timestamp to be newer than overview file
  VSIStatBufL sStatOvr;
  const std::string ovr_path = kVrt + ".ovr";
  ASSERT_EQ(0, VSIStatL(ovr_path.c_str(), &sStatOvr));
  struct utimbuf new_times;
  new_times.actime = sStatOvr.st_mtime + 10;
  new_times.modtime = sStatOvr.st_mtime + 10;
  ASSERT_EQ(0, utime(kSrc.c_str(), &new_times));

  // Second run: source is newer than overviews (test both verbose and quiet)
  EXPECT_EQ(
      0, RunGdalAddoMain({"gdaladdo", "--partial-refresh-from-source-timestamp",
                          kVrt, "2"}));
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q",
                                "--partial-refresh-from-source-timestamp", kVrt,
                                "2"}));

  // Error case: unlinked .ovr file triggers "Cannot find ... .ovr"
  ASSERT_EQ(0, VSIUnlink(ovr_path.c_str()));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", "-q",
                                  "--partial-refresh-from-source-timestamp",
                                  kVrt, "2"}));
  }
}

TEST(GdaladdoTest, PartialRefreshErrors) {
  constexpr char kDir[] = "/vsimem/gdaladdo_errors";
  constexpr char kPath[] = "/vsimem/gdaladdo_errors/test.tif";
  constexpr char kNoGt[] = "/vsimem/gdaladdo_errors/nogt.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1), NotNull());
  ASSERT_THAT(CreateGeoTiff(kNoGt, 100, 100, 1, /*with_geotransform=*/false),
              NotNull());

  WithQuietHandler quiet;
  // Non-VRT dataset for source-timestamp
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", "-q",
                                "--partial-refresh-from-source-timestamp",
                                kPath, "2"}));

  // No overviews exist on dataset
  EXPECT_EQ(1,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "0", "100", "50", "50", kPath, "2"}));

  // Build overview 2, but ask for overview 8 which doesn't exist
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", kPath, "2"}));
  EXPECT_EQ(1,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "0", "100", "50", "50", kPath, "8"}));

  // Projwin outside image bounds
  EXPECT_EQ(1,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "5000", "6000", "5100", "5900", kPath, "2"}));

  // Negative coordinates projwin
  EXPECT_EQ(0,
            RunGdalAddoMain({"gdaladdo", "-q", "--partial-refresh-from-projwin",
                             "900", "2100", "1050", "1950", kPath, "2"}));

  // Source extent: non-existent source
  EXPECT_EQ(1, RunGdalAddoMain(
                   {"gdaladdo", "-q", "--partial-refresh-from-source-extent",
                    "/vsimem/gdaladdo_errors/missing.tif", kPath, "2"}));

  // Source extent: source has no geotransform
  EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", "-q",
                                "--partial-refresh-from-source-extent", kNoGt,
                                kPath, "2"}));

  // VRT on /vsimem/ has st_mtime == 0
  {
    constexpr char kMemVrt[] = "/vsimem/gdaladdo_errors/mem.vrt";
    constexpr char kMemOvr[] = "/vsimem/gdaladdo_errors/mem.vrt.ovr";
    EnsureDriversRegistered();
    auto* vrt_drv = GetGDALDriverManager()->GetDriverByName("VRT");
    if (vrt_drv) {
      auto ds = std::unique_ptr<GDALDataset>(
          vrt_drv->Create(kMemVrt, 10, 10, 1, GDT_Byte, nullptr));
      ds.reset();
      VSIFCloseL(VSIFOpenL(kMemOvr, "wb"));
      EXPECT_EQ(1, RunGdalAddoMain({"gdaladdo", "-q",
                                    "--partial-refresh-from-source-timestamp",
                                    kMemVrt, "2"}));
    }
  }
}

TEST(GdaladdoTest, ResamplingMethodsAndNonQuiet) {
  constexpr char kDir[] = "/vsimem/gdaladdo_resampl";
  constexpr char kPath[] = "/vsimem/gdaladdo_resampl/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 1), NotNull());

  // Test various resampling methods
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-r", "average", kPath, "2"}));
  EXPECT_EQ(0,
            RunGdalAddoMain({"gdaladdo", "-q", "-r", "bilinear", kPath, "2"}));
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-r", "cubic", kPath, "2"}));
  EXPECT_EQ(
      0, RunGdalAddoMain({"gdaladdo", "-q", "-r", "cubicspline", kPath, "2"}));
  EXPECT_EQ(0,
            RunGdalAddoMain({"gdaladdo", "-q", "-r", "lanczos", kPath, "2"}));
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-r", "mode", kPath, "2"}));
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-r", "rms", kPath, "2"}));
  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-r", "gauss", kPath, "2"}));
}

TEST(GdaladdoTest, BandSelectionAndOpenOptions) {
  constexpr char kDir[] = "/vsimem/gdaladdo_bands";
  constexpr char kPath[] = "/vsimem/gdaladdo_bands/test.tif";
  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };
  ASSERT_THAT(CreateGeoTiff(kPath, 100, 100, 2), NotNull());

  EXPECT_EQ(0, RunGdalAddoMain({"gdaladdo", "-q", "-oo", "NUM_THREADS=2", "-b",
                                "1", kPath, "2"}));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdaladdo.cpp.
