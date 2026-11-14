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
// Tests for GDALRasterTileAlgorithm (apps/gdalalg_raster_tile.cpp and
// apps/gdalalg_raster_tile.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_tile.cpp
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_tile.h

// clang-format off
#include <atomic>  // Required before gdalalg_raster_tile.h.
#include "third_party/gdal/apps/gdalalg_raster_tile.h"
// clang-format on

#include <sys/stat.h>

#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_tile_test";

class GdalalgRasterTileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_COG();
    GDALRegister_PNG();
    GDALRegister_JPEG();
    GDALRegister_WEBP();
    GDALRegister_VRT();
    GDALRegister_GDALG();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }
};

// Creates an in-memory GDALDataset with optional SRS, geotransform, and
// gradient pixel values.
std::unique_ptr<GDALDataset> CreateMemDataset(
    int width, int height, int bands, GDALDataType dt, int epsg = 3857,
    const GDALGeoTransform* gt = nullptr, bool fill_gradient = true,
    bool last_band_alpha = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", width, height, bands, dt, nullptr));
  if (!ds) return nullptr;

  if (epsg > 0) {
    OGRSpatialReference srs;
    if (srs.importFromEPSG(epsg) != OGRERR_NONE) return nullptr;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (ds->SetSpatialRef(&srs) != CE_None) return nullptr;
  }

  if (gt != nullptr) {
    GDALGeoTransform gt_copy = *gt;
    if (ds->SetGeoTransform(gt_copy) != CE_None) return nullptr;
  } else if (epsg == 3857) {
    // Cover a portion of WebMercatorQuad around origin.
    GDALGeoTransform default_gt(-10018754.171394622, 20037508.342789244 / width,
                                0.0, 10018754.171394622, 0.0,
                                -20037508.342789244 / height);
    if (ds->SetGeoTransform(default_gt) != CE_None) return nullptr;
  } else if (epsg == 4326) {
    GDALGeoTransform default_gt(-90.0, 180.0 / width, 0.0, 45.0, 0.0,
                                -90.0 / height);
    if (ds->SetGeoTransform(default_gt) != CE_None) return nullptr;
  }

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    if (!band) return nullptr;
    if (last_band_alpha && b == bands) {
      band->SetColorInterpretation(GCI_AlphaBand);
    } else if (bands >= 3 && b <= 3) {
      band->SetColorInterpretation(
          static_cast<GDALColorInterp>(GCI_RedBand + b - 1));
    }
  }

  if (fill_gradient && width > 0 && height > 0) {
    for (int b = 1; b <= bands; ++b) {
      GDALRasterBand* band = ds->GetRasterBand(b);
      if (!band) return nullptr;
      if (dt == GDT_Byte) {
        std::vector<uint8_t> buf(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            if (last_band_alpha && b == bands) {
              // Half transparent on the left half, opaque on the right half
              // unless width < 2.
              buf[static_cast<size_t>(y) * width + x] = 255;
            } else {
              buf[static_cast<size_t>(y) * width + x] =
                  static_cast<uint8_t>((x * 7 + y * 13 + b * 41) & 0xFF);
            }
          }
        }
        if (band->RasterIO(GF_Write, 0, 0, width, height, buf.data(), width,
                           height, GDT_Byte, 0, 0, nullptr) != CE_None) {
          return nullptr;
        }
      } else if (dt == GDT_UInt16) {
        std::vector<uint16_t> buf(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            buf[static_cast<size_t>(y) * width + x] =
                static_cast<uint16_t>((x * 17 + y * 29 + b * 101) & 0x0FFF);
          }
        }
        if (band->RasterIO(GF_Write, 0, 0, width, height, buf.data(), width,
                           height, GDT_UInt16, 0, 0, nullptr) != CE_None) {
          return nullptr;
        }
      }
    }
  }
  return ds;
}

// Writes a MEM dataset out to a GeoTIFF file (under /vsimem/ or POSIX path).
bool WriteTiffFromMem(const std::string& path, GDALDataset* src_ds) {
  GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!gtiff || !src_ds) return false;
  std::unique_ptr<GDALDataset> out(gtiff->CreateCopy(
      path.c_str(), src_ds, FALSE, nullptr, nullptr, nullptr));
  if (!out) return false;
  return out->Close() == CE_None;
}

bool FileExists(const std::string& path) {
  VSIStatBufL statbuf;
  return VSIStatL(path.c_str(), &statbuf) == 0;
}

std::string ReadVsiFile(const std::string& path) {
  GByte* data = nullptr;
  vsi_l_offset len = 0;
  if (!VSIIngestFile(nullptr, path.c_str(), &data, &len, 10 * 1024 * 1024) ||
      !data) {
    return "";
  }
  std::unique_ptr<GByte, CplFreeDeleter> owned(data);
  return std::string(reinterpret_cast<const char*>(owned.get()),
                     static_cast<size_t>(len));
}

bool WriteRawFile(const std::string& path, std::string_view content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  if (!fp) return false;
  const size_t written = VSIFWriteL(content.data(), 1, content.size(), fp);
  const int closed = VSIFCloseL(fp);
  return written == content.size() && closed == 0;
}

TEST_F(GdalalgRasterTileTest, BasicPropertiesAndArguments) {
  GDALRasterTileAlgorithmStandalone standalone;
  EXPECT_EQ("tile", standalone.GetName());
  EXPECT_FALSE(standalone.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_tile.html", standalone.GetHelpURL());
  EXPECT_TRUE(standalone.CanBeLastStep());
  EXPECT_FALSE(standalone.IsNativelyStreamingCompatible());
  EXPECT_TRUE(standalone.SupportsInputMultiThreading());

  const std::string cli_usage = standalone.GetUsageForCLI(/*shortUsage=*/false);
  EXPECT_THAT(cli_usage, HasSubstr("tiling-scheme"));
  EXPECT_THAT(cli_usage, HasSubstr("webviewer"));

  const std::string json_usage = standalone.GetUsageAsJSON();
  EXPECT_THAT(json_usage, HasSubstr("\"tile\""));

  const auto* fmt_arg = standalone.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT);
  ASSERT_THAT(fmt_arg, NotNull());
  EXPECT_EQ("PNG", fmt_arg->Get<std::string>());

  const auto* scheme_arg = standalone.GetArg("tiling-scheme");
  ASSERT_THAT(scheme_arg, NotNull());
  EXPECT_EQ("WebMercatorQuad", scheme_arg->Get<std::string>());
  EXPECT_THAT(scheme_arg->GetChoices(), Contains("raster"));
  EXPECT_THAT(scheme_arg->GetChoices(), Contains("WebMercatorQuad"));
  EXPECT_THAT(scheme_arg->GetChoices(), Contains("WorldCRS84Quad"));

  const auto* conv_arg = standalone.GetArg("convention");
  ASSERT_THAT(conv_arg, NotNull());
  EXPECT_EQ("xyz", conv_arg->Get<std::string>());

  const auto* resamp_arg = standalone.GetArg("resampling");
  ASSERT_THAT(resamp_arg, NotNull());
  EXPECT_EQ("cubic", resamp_arg->Get<std::string>());

  const auto* excl_pct_arg = standalone.GetArg("excluded-values-pct-threshold");
  ASSERT_THAT(excl_pct_arg, NotNull());
  EXPECT_DOUBLE_EQ(50.0, excl_pct_arg->Get<double>());

  const auto* nodata_pct_arg = standalone.GetArg("nodata-values-pct-threshold");
  ASSERT_THAT(nodata_pct_arg, NotNull());
  EXPECT_DOUBLE_EQ(100.0, nodata_pct_arg->Get<double>());

  // Hidden alias "mo" for "metadata" and hidden choice "near" for resampling.
  EXPECT_EQ(standalone.GetArg("metadata"), standalone.GetArg("mo"));
  EXPECT_THAT(resamp_arg->GetHiddenChoices(), Contains("near"));

  // Autocomplete smoke check.
  std::vector<std::string> ac_args = {"--convention="};
  const auto ac = standalone.GetAutoComplete(ac_args, false, false);
  EXPECT_THAT(ac, Contains("xyz"));
  EXPECT_THAT(ac, Contains("tms"));

  // Pipeline-step (non-standalone) constructor also initializes cleanly.
  GDALRasterTileAlgorithm step_alg(/*standaloneStep=*/false);
  EXPECT_EQ("tile", step_alg.GetName());
  EXPECT_TRUE(step_alg.CanBeLastStep());
}

TEST_F(GdalalgRasterTileTest, ArgumentValidationErrors) {
  auto mem_ds = CreateMemDataset(16, 16, 3, GDT_Byte);
  ASSERT_THAT(mem_ds, NotNull());

  // 1. Missing required arguments in standalone mode.
  {
    WithQuietHandler quiet;
    GDALRasterTileAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({}));
  }

  // 2. Table-driven ParseCommandLineArguments validation errors.
  struct InvalidCase {
    std::string_view name;
    std::vector<std::string> args;
    std::string_view expected_substr;
  };
  const std::vector<InvalidCase> cases = {
      {
          .name = "MutuallyExclusiveAlphaFlags",
          .args = {"--add-alpha", "--no-alpha", "/vsimem/out"},
          .expected_substr = "mutually exclusive",
      },
      {
          .name = "AddAlphaAndDstNoData",
          .args = {"--add-alpha", "--dst-nodata=0", "/vsimem/out"},
          .expected_substr = "mutually exclusive",
      },
      {
          .name = "MinXGreaterThanMaxX",
          .args = {"--min-x=5", "--max-x=2", "/vsimem/out"},
          .expected_substr = "'min-x' must be lesser or equal to 'max-x'",
      },
      {
          .name = "MinYGreaterThanMaxY",
          .args = {"--min-y=5", "--max-y=2", "/vsimem/out"},
          .expected_substr = "'min-y' must be lesser or equal to 'max-y'",
      },
      {
          .name = "MinZoomGreaterThanMaxZoom",
          .args = {"--min-zoom=5", "--max-zoom=2", "/vsimem/out"},
          .expected_substr = "'min-zoom' must be lesser or equal to 'max-zoom'",
      },
      {
          .name = "ExcludedValuesWithoutAverageResampling",
          .args = {"--excluded-values=0,0,0", "/vsimem/out"},
          .expected_substr = "'resampling' is set to 'average'",
      },
      {
          .name = "ExcludedValuesPctWithoutAverageResampling",
          .args = {"--excluded-values-pct-threshold=50", "/vsimem/out"},
          .expected_substr = "'resampling' is set to 'average'",
      },
      {
          .name = "NodataValuesPctWithoutAverageResampling",
          .args = {"--nodata-values-pct-threshold=80", "/vsimem/out"},
          .expected_substr = "'resampling' is set to 'average'",
      },
      {
          .name = "ExcludedValuesWithNonAverageOverviewResampling",
          .args = {"--resampling=average", "--overview-resampling=bilinear",
                   "--excluded-values=0,0,0", "/vsimem/out"},
          .expected_substr = "'overview-resampling' is set to 'average'",
      },
      {
          .name = "InvalidTilingScheme",
          .args = {"--tiling-scheme=NoSuchScheme", "/vsimem/out"},
          .expected_substr = "Invalid value",
      },
      {
          .name = "InvalidConvention",
          .args = {"--convention=bad", "/vsimem/out"},
          .expected_substr = "Invalid value",
      },
      {
          .name = "InvalidResampling",
          .args = {"--resampling=invalid_interp", "/vsimem/out"},
          .expected_substr = "Invalid value",
      },
      {
          .name = "InvalidOverviewResampling",
          .args = {"--overview-resampling=invalid_interp", "/vsimem/out"},
          .expected_substr = "Invalid value",
      },
      {
          .name = "InvalidWebviewer",
          .args = {"--webviewer=invalid_viewer", "/vsimem/out"},
          .expected_substr = "Invalid value",
      },
      {
          .name = "InvalidParallelMethod",
          .args = {"--parallel-method=invalid_method", "/vsimem/out"},
          .expected_substr = "Invalid value",
      },
      {
          .name = "TileSizeTooSmall",
          .args = {"--tile-size=32", "/vsimem/out"},
          .expected_substr = "",
      },
      {
          .name = "TileSizeTooLarge",
          .args = {"--tile-size=65536", "/vsimem/out"},
          .expected_substr = "",
      },
      {
          .name = "NegativeMinZoom",
          .args = {"--min-zoom=-1", "/vsimem/out"},
          .expected_substr = "",
      },
      {
          .name = "InvalidMetadataKeyValue",
          .args = {"--metadata=NO_EQUALS_SIGN", "/vsimem/out"},
          .expected_substr = "",
      },
      {
          .name = "EmptyMapmlTemplate",
          .args = {"--mapml-template=", "/vsimem/out"},
          .expected_substr = "",
      },
      {
          .name = "ExcludedPctOver100",
          .args = {"--resampling=average",
                   "--excluded-values-pct-threshold=101", "/vsimem/out"},
          .expected_substr = "",
      },
      {
          .name = "NodataPctNegative",
          .args = {"--resampling=average", "--nodata-values-pct-threshold=-1",
                   "/vsimem/out"},
          .expected_substr = "",
      },
  };

  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.name);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg(/*standaloneStep=*/false);
    alg.GetInputDatasets().emplace_back(mem_ds.get());
    EXPECT_FALSE(alg.ParseCommandLineArguments(tc.args));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    if (!tc.expected_substr.empty()) {
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_substr));
    }
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterTileTest, RuntimeValidationAndFormatErrors) {
  const std::string out_dir = std::string(kTestDir) + "/err_out";

  // 1. 0-band MEM dataset -> "Invalid source dataset".
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_THAT(drv, NotNull());
    std::unique_ptr<GDALDataset> empty_ds(
        drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(empty_ds, NotNull());
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(empty_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({out_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid source dataset"));
    CPLErrorReset();
  }

  // 2. Ungeoreferenced dataset (no GT, no SRS) with WebMercatorQuad.
  {
    auto unref_ds = CreateMemDataset(16, 16, 1, GDT_Byte, /*epsg=*/0);
    ASSERT_THAT(unref_ds, NotNull());
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(unref_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({out_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Ungeoreferenced datasets are not supported"));
    CPLErrorReset();
  }

  // 3. Has GT but no SRS -> still ungeoreferenced for non-raster TMS.
  {
    GDALGeoTransform gt(0.0, 1.0, 0.0, 16.0, 0.0, -1.0);
    auto no_srs_ds = CreateMemDataset(16, 16, 1, GDT_Byte, /*epsg=*/0, &gt);
    ASSERT_THAT(no_srs_ds, NotNull());
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(no_srs_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({out_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Ungeoreferenced datasets are not supported"));
    CPLErrorReset();
  }

  // 4. Palette-indexed dataset with non-nearest/non-mode resampling.
  {
    auto pal_ds = CreateMemDataset(16, 16, 1, GDT_Byte);
    ASSERT_THAT(pal_ds, NotNull());
    GDALColorTable ct;
    GDALColorEntry e0{0, 0, 0, 255};
    GDALColorEntry e1{255, 0, 0, 255};
    ct.SetColorEntry(0, &e0);
    ct.SetColorEntry(1, &e1);
    ASSERT_EQ(CE_None, pal_ds->GetRasterBand(1)->SetColorTable(&ct));
    ASSERT_EQ(CE_None, pal_ds->GetRasterBand(1)->SetColorInterpretation(
                           GCI_PaletteIndex));

    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(pal_ds.get());
      ASSERT_TRUE(
          alg.ParseCommandLineArguments({"--resampling=cubic", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Datasets with color table not supported"));
      CPLErrorReset();
    }
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(pal_ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--resampling=nearest", "--overview-resampling=bilinear", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Datasets with color table not supported"));
      CPLErrorReset();
    }
  }

  // 5. Output format validation errors (PNG, JPEG, WEBP).
  {
    auto ds_5band = CreateMemDataset(16, 16, 5, GDT_Byte);
    auto ds_float = CreateMemDataset(16, 16, 1, GDT_Float32, 3857, nullptr,
                                     /*fill_gradient=*/false);
    auto ds_u16_1 = CreateMemDataset(16, 16, 1, GDT_UInt16);
    auto ds_u16_3 = CreateMemDataset(16, 16, 3, GDT_UInt16);
    auto ds_byte_1 = CreateMemDataset(16, 16, 1, GDT_Byte);
    ASSERT_THAT(ds_5band, NotNull());
    ASSERT_THAT(ds_float, NotNull());
    ASSERT_THAT(ds_u16_1, NotNull());
    ASSERT_THAT(ds_u16_3, NotNull());
    ASSERT_THAT(ds_byte_1, NotNull());

    const auto expect_format_fail = [&](GDALDataset* ds, const char* fmt,
                                        std::string_view expected_err) {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(ds);
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"-f", fmt, "--min-zoom=1", "--max-zoom=1", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(expected_err));
      CPLErrorReset();
    };

    expect_format_fail(ds_5band.get(), "PNG",
                       "Only up to 4 bands supported for PNG");
    expect_format_fail(ds_float.get(), "PNG",
                       "Only Byte and UInt16 data types supported for PNG");
    expect_format_fail(ds_5band.get(), "JPEG",
                       "Only up to 4 bands supported for JPEG");
    expect_format_fail(ds_float.get(), "JPEG", "supported for JPEG");
    expect_format_fail(ds_byte_1.get(), "WEBP",
                       "Only 3 or 4 bands supported for WEBP");
    expect_format_fail(ds_u16_3.get(), "WEBP",
                       "Only Byte data type supported for WEBP");

    // Check JPEG UInt16 behavior depending on whether libjpeg-turbo has 12-bit.
    GDALDriver* jpeg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
    ASSERT_THAT(jpeg_drv, NotNull());
    const char* dtypes = jpeg_drv->GetMetadataItem(GDAL_DMD_CREATIONDATATYPES);
    if (dtypes != nullptr &&
        std::string_view(dtypes).find("UInt16") != std::string_view::npos) {
      ds_u16_1->GetRasterBand(1)->SetMetadataItem("NBITS", "13",
                                                  "IMAGE_STRUCTURE");
      expect_format_fail(ds_u16_1.get(), "JPEG", "up to 12 bits");
    } else {
      expect_format_fail(ds_u16_1.get(), "JPEG",
                         "Only Byte data type supported for JPEG");
    }
  }

  // 6. Invalid max-zoom and super-fine resolution (no zoom level matches).
  {
    auto ds = CreateMemDataset(16, 16, 3, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--max-zoom=99", out_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("max-zoom = 99 is invalid"));
    CPLErrorReset();

    GDALGeoTransform tiny_gt(0.0, 1e-8, 0.0, 100.0, 0.0, -1e-8);
    auto fine_ds = CreateMemDataset(16, 16, 1, GDT_Byte, 3857, &tiny_gt);
    ASSERT_THAT(fine_ds, NotNull());
    WithQuietHandler quiet2;
    GDALRasterTileAlgorithm alg2;
    alg2.GetInputDatasets().emplace_back(fine_ds.get());
    ASSERT_TRUE(alg2.ParseCommandLineArguments({out_dir}));
    EXPECT_FALSE(alg2.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Could not find an appropriate zoom level"));
    CPLErrorReset();
  }

  // 7. Non-intersecting extent with WorldCRS84Quad (error vs
  // --no-intersection-ok).
  {
    GDALGeoTransform outside_gt(200.0, 1.0, 0.0, 110.0, 0.0, -1.0);
    auto outside_ds = CreateMemDataset(16, 16, 1, GDT_Byte, 4326, &outside_gt);
    ASSERT_THAT(outside_ds, NotNull());
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(outside_ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--tiling-scheme=WorldCRS84Quad", "--min-zoom=1", "--max-zoom=1",
           out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("not compatible with extent"));
      CPLErrorReset();
    }
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(outside_ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--tiling-scheme=WorldCRS84Quad", "--min-zoom=1", "--max-zoom=1",
           "--no-intersection-ok", out_dir}));
      EXPECT_TRUE(alg.Run());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      CPLErrorReset();
    }
  }

  // 8. Tile X/Y out of range or non-intersecting tile window.
  {
    GDALGeoTransform nw_gt(-10000000.0, 10000.0, 0.0, 10000000.0, 0.0,
                           -10000.0);
    auto nw_ds = CreateMemDataset(16, 16, 1, GDT_Byte, 3857, &nw_gt);
    ASSERT_THAT(nw_ds, NotNull());

    const auto expect_tile_range_fail = [&](const std::vector<std::string>& a,
                                            std::string_view err) {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(nw_ds.get());
      std::vector<std::string> full_args = {"--min-zoom=1", "--max-zoom=1"};
      full_args.insert(full_args.end(), a.begin(), a.end());
      full_args.push_back(out_dir);
      ASSERT_TRUE(alg.ParseCommandLineArguments(full_args));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(err));
      CPLErrorReset();
    };

    expect_tile_range_fail({"--min-x=5"}, "'min-x' value must be in");
    expect_tile_range_fail({"--max-x=5"}, "'max-x' value must be in");
    expect_tile_range_fail({"--min-y=5"}, "'min-y' value must be in");
    expect_tile_range_fail({"--max-y=5"}, "'max-y' value must be in");
    expect_tile_range_fail(
        {"--min-x=1", "--max-x=1", "--min-y=1", "--max-y=1"},
        "Dataset extent not intersecting specified min/max X/Y");

    // Same non-intersecting X/Y window with --no-intersection-ok succeeds.
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(nw_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=1", "--max-zoom=1", "--min-x=1", "--max-x=1", "--min-y=1",
         "--max-y=1", "--no-intersection-ok", out_dir}));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 9. Too large zoom level (raster overflow check) and oversized tile buffer.
  {
    auto ds = CreateMemDataset(16, 16, 3, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--min-zoom=25", "--max-zoom=25", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too large zoom level"));
      CPLErrorReset();
    }
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(ds.get());
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--min-zoom=0", "--max-zoom=0", "--tile-size=32768", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Tile size and/or number of bands too large"));
      CPLErrorReset();
    }
  }

  // 10. Output path is an existing regular file -> "Cannot create output
  // directory".
  {
    const std::string file_path = std::string(kTestDir) + "/existing_file";
    ASSERT_TRUE(WriteRawFile(file_path, "not a dir"));
    auto ds = CreateMemDataset(16, 16, 1, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=0", file_path}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot create output directory"));
    CPLErrorReset();
  }

  // 11. KML incompatible with raster tiling scheme without SRS.
  {
    auto unref_ds = CreateMemDataset(64, 64, 1, GDT_Byte, /*epsg=*/0);
    ASSERT_THAT(unref_ds, NotNull());
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(unref_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--tiling-scheme=raster", "--kml", out_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Tiling scheme not compatible with KML output"));
    CPLErrorReset();
  }

  // 12. Spawn and fork parallel-method validation errors.
  {
    auto mem_ds = CreateMemDataset(16, 16, 1, GDT_Byte);
    ASSERT_THAT(mem_ds, NotNull());
    const std::string src_tif = std::string(kTestDir) + "/spawn_err_src.tif";
    ASSERT_TRUE(WriteTiffFromMem(src_tif, mem_ds.get()));

    // MEM source not supported by spawn.
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(mem_ds.get());
      ASSERT_TRUE(
          alg.ParseCommandLineArguments({"--parallel-method=spawn", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("memory dataset sources are not supported"));
      CPLErrorReset();
    }

    // /vsimem/ output not supported by spawn.
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithmStandalone alg;
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--parallel-method=spawn", src_tif, out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("/vsimem/ output directory not supported"));
      CPLErrorReset();
    }

    // /vsimem/ output not supported by fork.
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithmStandalone alg;
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--parallel-method=fork", src_tif, out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("/vsimem/ output directory not supported"));
      CPLErrorReset();
    }

    // Unnamed non-MEM source not supported by fork.
    {
      std::unique_ptr<GDALDataset> opened_ds(GDALDataset::Open(
          src_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_THAT(opened_ds, NotNull());
      opened_ds->SetDescription("");
      CPLErrorReset();
      WithQuietHandler quiet;
      GDALRasterTileAlgorithm alg;
      alg.GetInputDatasets().emplace_back(opened_ds.get());
      ASSERT_TRUE(
          alg.ParseCommandLineArguments({"--parallel-method=fork", out_dir}));
      EXPECT_FALSE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Unnamed non-MEM source are not supported"));
      CPLErrorReset();
    }
  }
}

TEST_F(GdalalgRasterTileTest, WebMercatorQuadPngOptimFiltersAndSkipBlank) {
  // Exercise 1, 2, 3, and 4 band Byte PNG optimization (SSE2 + scalar tail
  // at tile-size=64), PAETH / AVERAGE / auto filters, CreateCopy fallback
  // (GDAL_RASTER_TILE_USE_PNG_OPTIM=NO), alpha stripping, --add-alpha, and
  // --skip-blank with overview generation.
  for (int bands : {1, 2, 3, 4}) {
    SCOPED_TRACE(bands);
    const bool has_alpha = (bands == 2 || bands == 4);
    auto ds = CreateMemDataset(128, 128, bands, GDT_Byte, 3857, nullptr,
                               /*fill_gradient=*/true,
                               /*last_band_alpha=*/has_alpha);
    ASSERT_THAT(ds, NotNull());

    // Make the left half completely transparent for 2- and 4-band inputs so
    // some zoom-2 tiles are completely blank and skipped by --skip-blank,
    // while right-half tiles are partially opaque / partially transparent.
    if (has_alpha) {
      std::vector<uint8_t> alpha(128 * 128, 0);
      for (int y = 0; y < 128; ++y) {
        for (int x = 64; x < 128; ++x) {
          alpha[static_cast<size_t>(y) * 128 + x] = (y < 64) ? 255 : 128;
        }
      }
      ASSERT_EQ(CE_None, ds->GetRasterBand(bands)->RasterIO(
                             GF_Write, 0, 0, 128, 128, alpha.data(), 128, 128,
                             GDT_Byte, 0, 0, nullptr));
    }

    const char* filter = (bands == 1)   ? ""
                         : (bands == 2) ? "PAETH"
                         : (bands == 3) ? "AVERAGE"
                                        : "";
    const CPLConfigOptionSetter filter_setter("GDAL_RASTER_TILE_PNG_FILTER",
                                              filter, false);

    const std::string out_dir =
        std::string(kTestDir) + "/png_bands_" + std::to_string(bands);
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    std::vector<std::string> args = {"--min-zoom=0", "--max-zoom=2",
                                     "--tile-size=64", "--num-threads=1",
                                     "--webviewer=none"};
    if (has_alpha) {
      args.push_back("--skip-blank");
    }
    if (bands == 2) {
      // Force keeping alpha even on all-opaque tiles.
      args.push_back("--add-alpha");
    }
    args.push_back(out_dir);

    ASSERT_TRUE(alg.ParseCommandLineArguments(args));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();

    // Base zoom z=2 should have tile (2, 1) on the right side.
    const std::string right_tile = out_dir + "/2/2/1.png";
    ASSERT_TRUE(FileExists(right_tile));
    std::unique_ptr<GDALDataset> tile_ds(GDALDataset::Open(
        right_tile.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(tile_ds, NotNull());
    EXPECT_EQ(64, tile_ds->GetRasterXSize());
    EXPECT_EQ(64, tile_ds->GetRasterYSize());

    // Overview z=0 tile (0, 0) must exist.
    const std::string ovr_tile = out_dir + "/0/0/0.png";
    ASSERT_TRUE(FileExists(ovr_tile));
    std::unique_ptr<GDALDataset> ovr_ds(
        GDALDataset::Open(ovr_tile.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(ovr_ds, NotNull());
    EXPECT_EQ(64, ovr_ds->GetRasterXSize());

    if (has_alpha) {
      // Left-side tile x=1, y=1 in z=2 had alpha=0 everywhere -> skipped!
      EXPECT_FALSE(FileExists(out_dir + "/2/1/1.png"));
    }
  }

  // Test opaque 4-band input WITHOUT --add-alpha -> alpha band is stripped to
  // 3 bands in GenerateTile and GenerateOverviewTile when tiles are full.
  {
    GDALGeoTransform full_gt(-20037508.342789244, 40075016.685578488 / 64, 0.0,
                             20037508.342789244, 0.0, -40075016.685578488 / 64);
    auto opaque_rgba = CreateMemDataset(64, 64, 4, GDT_Byte, 3857, &full_gt,
                                        /*fill_gradient=*/true,
                                        /*last_band_alpha=*/true);
    ASSERT_THAT(opaque_rgba, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/png_strip_alpha";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(opaque_rgba.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();

    std::unique_ptr<GDALDataset> base_ds(GDALDataset::Open(
        (out_dir + "/1/0/0.png").c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(base_ds, NotNull());
    EXPECT_EQ(3, base_ds->GetRasterCount());
    std::unique_ptr<GDALDataset> ovr_ds(GDALDataset::Open(
        (out_dir + "/0/0/0.png").c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(ovr_ds, NotNull());
    EXPECT_EQ(3, ovr_ds->GetRasterCount());
  }

  // Test PNG with GDAL_RASTER_TILE_USE_PNG_OPTIM=NO (libpng CreateCopy path).
  {
    const CPLConfigOptionSetter no_optim("GDAL_RASTER_TILE_USE_PNG_OPTIM", "NO",
                                         false);
    auto rgb_ds = CreateMemDataset(64, 64, 3, GDT_Byte);
    ASSERT_THAT(rgb_ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/png_no_optim";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(rgb_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/1/0/0.png"));
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }
}

TEST_F(GdalalgRasterTileTest, TilingSchemesAndConventions) {
  // 1. WebMercatorQuad: convention=xyz vs convention=tms (Y-flip).
  {
    GDALGeoTransform nw_gt(-10000000.0, 20000.0, 0.0, 10000000.0, 0.0,
                           -20000.0);
    auto ds = CreateMemDataset(64, 64, 3, GDT_Byte, 3857, &nw_gt);
    ASSERT_THAT(ds, NotNull());

    const std::string xyz_dir = std::string(kTestDir) + "/conv_xyz";
    const std::string tms_dir = std::string(kTestDir) + "/conv_tms";

    GDALRasterTileAlgorithm alg_xyz;
    alg_xyz.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg_xyz.ParseCommandLineArguments(
        {"--tiling-scheme=mercator", "--min-zoom=1", "--max-zoom=1",
         "--convention=xyz", "--tile-size=64", "--num-threads=1",
         "--webviewer=leaflet", xyz_dir}));
    ASSERT_TRUE(alg_xyz.Run()) << CPLGetLastErrorMsg();
    // Top-left quadrant at z=1 is x=0, y=0 in XYZ.
    EXPECT_TRUE(FileExists(xyz_dir + "/1/0/0.png"));

    GDALRasterTileAlgorithm alg_tms;
    alg_tms.GetInputDatasets().emplace_back(ds.get());
    // Use WebMercatorQuad instead of GoogleMapsCompatible: upstream GDAL bug at
    // apps/gdalalg_raster_tile.cpp:4513 translates "mercator" and "geodetic"
    // but omits "GoogleMapsCompatible", causing a null poTMS dereference.
    ASSERT_TRUE(alg_tms.ParseCommandLineArguments(
        {"--tiling-scheme=WebMercatorQuad", "--min-zoom=1", "--max-zoom=1",
         "--convention=tms", "--tile-size=64", "--num-threads=1",
         "--webviewer=all", "--kml", tms_dir}));
    ASSERT_TRUE(alg_tms.Run()) << CPLGetLastErrorMsg();
    // In TMS at z=1 (matrixHeight=2), y=0 flips to 1.
    EXPECT_TRUE(FileExists(tms_dir + "/1/0/1.png"));
    EXPECT_TRUE(FileExists(tms_dir + "/1/0/1.kml"));
    EXPECT_TRUE(FileExists(tms_dir + "/doc.kml"));
    EXPECT_TRUE(FileExists(tms_dir + "/leaflet.html"));
    EXPECT_TRUE(FileExists(tms_dir + "/openlayers.html"));
    EXPECT_THAT(ReadVsiFile(tms_dir + "/leaflet.html"), HasSubstr("tms: 1"));
    EXPECT_THAT(ReadVsiFile(tms_dir + "/openlayers.html"),
                HasSubstr("{-y}.png"));
  }

  // 2. WorldCRS84Quad (and hidden alias "geodetic") with both xyz and tms.
  {
    auto ds4326 = CreateMemDataset(64, 64, 3, GDT_Byte, 4326);
    ASSERT_THAT(ds4326, NotNull());

    const std::string w84_xyz = std::string(kTestDir) + "/w84_xyz";
    const std::string w84_tms = std::string(kTestDir) + "/w84_tms";

    GDALRasterTileAlgorithm alg_xyz;
    alg_xyz.GetInputDatasets().emplace_back(ds4326.get());
    ASSERT_TRUE(alg_xyz.ParseCommandLineArguments(
        {"--tiling-scheme=WorldCRS84Quad", "--min-zoom=0", "--max-zoom=1",
         "--convention=xyz", "--tile-size=64", "--num-threads=1",
         "--webviewer=all", "--kml", w84_xyz}));
    ASSERT_TRUE(alg_xyz.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(w84_xyz + "/0/0/0.png"));
    EXPECT_TRUE(FileExists(w84_xyz + "/openlayers.html"));
    EXPECT_TRUE(FileExists(w84_xyz + "/mapml.mapml"));
    EXPECT_TRUE(FileExists(w84_xyz + "/stacta.json"));
    EXPECT_TRUE(FileExists(w84_xyz + "/doc.kml"));
    // WorldCRS84Quad has axis-aligned tiles -> KML uses LatLonBox without
    // gx:LatLonQuad.
    const std::string tile_kml = ReadVsiFile(w84_xyz + "/0/0/0.kml");
    EXPECT_THAT(tile_kml, HasSubstr("<LatLonBox>"));
    EXPECT_THAT(tile_kml, Not(HasSubstr("gx:LatLonQuad")));
    EXPECT_THAT(ReadVsiFile(w84_xyz + "/mapml.mapml"), HasSubstr("WGS84"));

    GDALRasterTileAlgorithm alg_tms;
    alg_tms.GetInputDatasets().emplace_back(ds4326.get());
    ASSERT_TRUE(alg_tms.ParseCommandLineArguments(
        {"--tiling-scheme=geodetic", "--min-zoom=0", "--max-zoom=1",
         "--convention=tms", "--tile-size=64", "--num-threads=1",
         "--webviewer=openlayers", w84_tms}));
    ASSERT_TRUE(alg_tms.Run()) << CPLGetLastErrorMsg();
    EXPECT_THAT(ReadVsiFile(w84_tms + "/openlayers.html"),
                HasSubstr("[-180,-90]"));
  }

  // 3. Global EPSG:4326 spanning +/-90 lat into WebMercatorQuad (exercises
  // MAX_LAT = 85.0511287798066 clamping workaround).
  {
    GDALGeoTransform global_gt(-180.0, 360.0 / 64, 0.0, 90.0, 0.0, -180.0 / 64);
    auto global_ds = CreateMemDataset(64, 64, 3, GDT_Byte, 4326, &global_gt);
    ASSERT_THAT(global_ds, NotNull());

    const std::string out_dir = std::string(kTestDir) + "/epsg3857_lat_clamp";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(global_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--tiling-scheme=WebMercatorQuad", "--min-zoom=0", "--max-zoom=0",
         "--tile-size=64", "--num-threads=1", "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }

  // 4. Predefined TMS from gdal_data: NZTM2000 (hits generic TMS branch in
  // OpenLayers, MapML, STAC, KML).
  {
    GDALGeoTransform nztm_gt(1500000.0, 1000.0, 0.0, 5500000.0, 0.0, -1000.0);
    auto nztm_ds = CreateMemDataset(64, 64, 3, GDT_Byte, 2193, &nztm_gt);
    ASSERT_THAT(nztm_ds, NotNull());

    const std::string out_dir = std::string(kTestDir) + "/nztm2000";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(nztm_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--tiling-scheme=NZTM2000", "--min-zoom=0", "--max-zoom=1",
         "--tile-size=64", "--num-threads=1", "--webviewer=all", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/openlayers.html"));
    EXPECT_TRUE(FileExists(out_dir + "/mapml.mapml"));
    EXPECT_TRUE(FileExists(out_dir + "/stacta.json"));
    EXPECT_THAT(ReadVsiFile(out_dir + "/openlayers.html"),
                HasSubstr("EPSG:2193"));
    EXPECT_THAT(ReadVsiFile(out_dir + "/mapml.mapml"), HasSubstr("NZTM2000"));
  }

  // 5. "raster" tiling scheme:
  // 5a. Ungeoreferenced dataset (non-north-up fallback + VRT -a_srs none +
  // automatic max-zoom + default tile-size=256 + STAC without SRS).
  {
    auto raw_ds = CreateMemDataset(300, 200, 3, GDT_Byte, /*epsg=*/0);
    ASSERT_THAT(raw_ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/raster_raw";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(raw_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--tiling-scheme=raster", "--min-zoom=0", "--num-threads=1",
         "--webviewer=all", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
    EXPECT_TRUE(FileExists(out_dir + "/openlayers.html"));
    EXPECT_TRUE(FileExists(out_dir + "/stacta.json"));
    EXPECT_FALSE(FileExists(out_dir + "/mapml.mapml"));
  }

  // 5b. "raster" tiling scheme with non-EPSG SRS -> STAC writes proj:projjson.
  {
    auto custom_ds = CreateMemDataset(128, 128, 1, GDT_Byte, /*epsg=*/0);
    ASSERT_THAT(custom_ds, NotNull());
    OGRSpatialReference custom_srs;
    ASSERT_EQ(OGRERR_NONE,
              custom_srs.importFromProj4("+proj=merc +lon_0=10 +datum=WGS84"));
    custom_ds->SetSpatialRef(&custom_srs);
    GDALGeoTransform gt(0.0, 10.0, 0.0, 1280.0, 0.0, -10.0);
    ASSERT_EQ(CE_None, custom_ds->SetGeoTransform(gt));

    const std::string out_dir = std::string(kTestDir) + "/raster_projjson";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(custom_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--tiling-scheme=raster", "--min-zoom=0", "--max-zoom=1",
         "--tile-size=64", "--convention=tms", "--num-threads=1",
         "--webviewer=all", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    // convention=tms disables stac/mapml, so check openlayers; run xyz for
    // STAC.
    EXPECT_TRUE(FileExists(out_dir + "/openlayers.html"));

    const std::string out_dir_xyz =
        std::string(kTestDir) + "/raster_projjson_xyz";
    GDALRasterTileAlgorithm alg_xyz;
    alg_xyz.GetInputDatasets().emplace_back(custom_ds.get());
    ASSERT_TRUE(alg_xyz.ParseCommandLineArguments(
        {"--tiling-scheme=raster", "--min-zoom=0", "--max-zoom=1",
         "--tile-size=64", "--num-threads=1", "--webviewer=stac",
         out_dir_xyz}));
    ASSERT_TRUE(alg_xyz.Run()) << CPLGetLastErrorMsg();
    EXPECT_THAT(ReadVsiFile(out_dir_xyz + "/stacta.json"),
                HasSubstr("proj:projjson"));
  }
}

TEST_F(GdalalgRasterTileTest, OutputFormatsAndMetadataOptions) {
  // 1. GTiff with dst-nodata, no-alpha, metadata, copy-src-metadata, aux-xml,
  // and tile-size=64 (BLOCKYSIZE) vs tile-size=512 (or explicit TILED=YES).
  {
    auto ds = CreateMemDataset(64, 64, 3, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    ds->SetMetadataItem("SRC_TAG", "from_src");
    ds->SetMetadataItem("OVERRIDE_TAG", "old_val");
    ds->GetRasterBand(1)->SetNoDataValue(0.0);

    const std::string out_dir = std::string(kTestDir) + "/gtiff_out";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GTiff", "--min-zoom=0", "--max-zoom=1", "--tile-size=64",
         "--no-alpha", "--dst-nodata=255", "--metadata=FOO=BAR", "--mo",
         "OVERRIDE_TAG=new_val", "--copy-src-metadata", "--aux-xml",
         "--num-threads=1", "--webviewer=stac", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();

    const std::string tile_path = out_dir + "/1/0/0.tif";
    ASSERT_TRUE(FileExists(tile_path));
    std::unique_ptr<GDALDataset> tile_ds(GDALDataset::Open(
        tile_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(tile_ds, NotNull());
    EXPECT_EQ(3, tile_ds->GetRasterCount());
    int has_nodata = FALSE;
    EXPECT_DOUBLE_EQ(255.0,
                     tile_ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
    EXPECT_TRUE(has_nodata);
    EXPECT_STREQ("BAR", tile_ds->GetMetadataItem("FOO"));
    EXPECT_STREQ("from_src", tile_ds->GetMetadataItem("SRC_TAG"));
    EXPECT_STREQ("new_val", tile_ds->GetMetadataItem("OVERRIDE_TAG"));
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.tif"));
    EXPECT_THAT(ReadVsiFile(out_dir + "/stacta.json"),
                HasSubstr("image/tiff; application=geotiff"));
  }

  // 2. GTiff with large tile-size=576 (> 512) -> exercises TILED=YES branch.
  {
    auto ds = CreateMemDataset(64, 64, 1, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/gtiff_tiled";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GTiff", "--min-zoom=0", "--max-zoom=0", "--tile-size=576",
         "--num-threads=1", "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    std::unique_ptr<GDALDataset> tile_ds(GDALDataset::Open(
        (out_dir + "/0/0/0.tif").c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(tile_ds, NotNull());
    int bx = 0;
    int by = 0;
    tile_ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
    EXPECT_EQ(256, bx);
    EXPECT_EQ(256, by);
  }

  // 3. JPEG (bSupportsCreateOnlyVisibleAtCloseTime).
  {
    auto ds = CreateMemDataset(64, 64, 3, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/jpeg_out";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "JPEG", "--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--co",
         "QUALITY=85", "--num-threads=1", "--webviewer=stac", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    const std::string tile_path = out_dir + "/1/0/0.jpg";
    ASSERT_TRUE(FileExists(tile_path));
    std::unique_ptr<GDALDataset> tile_ds(GDALDataset::Open(
        tile_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(tile_ds, NotNull());
    EXPECT_EQ(3, tile_ds->GetRasterCount());
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.jpg"));
    EXPECT_THAT(ReadVsiFile(out_dir + "/stacta.json"), HasSubstr("image/jpeg"));
  }

  // 4. WEBP (3-band and 4-band).
  {
    auto ds = CreateMemDataset(64, 64, 4, GDT_Byte, 3857, nullptr, true, true);
    ASSERT_THAT(ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/webp_out";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "WEBP", "--min-zoom=0", "--max-zoom=1", "--tile-size=64",
         "--add-alpha", "--num-threads=1", "--webviewer=stac", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    const std::string tile_path = out_dir + "/1/0/0.webp";
    ASSERT_TRUE(FileExists(tile_path));
    std::unique_ptr<GDALDataset> tile_ds(GDALDataset::Open(
        tile_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(tile_ds, NotNull());
    EXPECT_EQ(4, tile_ds->GetRasterCount());
    EXPECT_THAT(ReadVsiFile(out_dir + "/stacta.json"), HasSubstr("image/webp"));
  }

  // 5. COG (bSupportsCreateOnlyVisibleAtCloseTime + GetUpdatedCreationOptions +
  // STAC profile=cloud-optimized).
  {
    auto ds = CreateMemDataset(64, 64, 3, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/cog_out";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "COG", "--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--co",
         "COMPRESS=LZW", "--num-threads=1", "--webviewer=stac", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    const std::string tile_path = out_dir + "/1/0/0.tif";
    ASSERT_TRUE(FileExists(tile_path));
    std::unique_ptr<GDALDataset> tile_ds(GDALDataset::Open(
        tile_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(tile_ds, NotNull());
    EXPECT_THAT(ReadVsiFile(out_dir + "/stacta.json"),
                HasSubstr("profile=cloud-optimized"));
  }

  // 6. PNG UInt16 + ColorTable (palette-indexed) with nearest/mode resampling.
  {
    auto u16_ds = CreateMemDataset(64, 64, 1, GDT_UInt16);
    ASSERT_THAT(u16_ds, NotNull());
    const std::string u16_dir = std::string(kTestDir) + "/png_u16";
    GDALRasterTileAlgorithm alg_u16;
    alg_u16.GetInputDatasets().emplace_back(u16_ds.get());
    ASSERT_TRUE(alg_u16.ParseCommandLineArguments(
        {"-f", "PNG", "--min-zoom=0", "--max-zoom=1", "--tile-size=64",
         "--num-threads=1", "--webviewer=none", u16_dir}));
    ASSERT_TRUE(alg_u16.Run()) << CPLGetLastErrorMsg();
    std::unique_ptr<GDALDataset> out_u16(GDALDataset::Open(
        (u16_dir + "/1/0/0.png").c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_u16, NotNull());
    EXPECT_EQ(GDT_UInt16, out_u16->GetRasterBand(1)->GetRasterDataType());

    auto pal_ds = CreateMemDataset(64, 64, 1, GDT_Byte, 3857, nullptr,
                                   /*fill_gradient=*/false);
    ASSERT_THAT(pal_ds, NotNull());
    std::vector<uint8_t> pal_buf(64 * 64);
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        pal_buf[static_cast<size_t>(y) * 64 + x] =
            static_cast<uint8_t>((x + y) & 1);
      }
    }
    ASSERT_EQ(CE_None, pal_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 64, 64, pal_buf.data(), 64, 64,
                           GDT_Byte, 0, 0, nullptr));
    GDALColorTable ct;
    GDALColorEntry c0{10, 20, 30, 255};
    GDALColorEntry c1{200, 100, 50, 255};
    ct.SetColorEntry(0, &c0);
    ct.SetColorEntry(1, &c1);
    ASSERT_EQ(CE_None, pal_ds->GetRasterBand(1)->SetColorTable(&ct));
    ASSERT_EQ(CE_None, pal_ds->GetRasterBand(1)->SetColorInterpretation(
                           GCI_PaletteIndex));

    const std::string pal_dir = std::string(kTestDir) + "/png_palette";
    GDALRasterTileAlgorithm alg_pal;
    alg_pal.GetInputDatasets().emplace_back(pal_ds.get());
    ASSERT_TRUE(alg_pal.ParseCommandLineArguments(
        {"--resampling=nearest", "--overview-resampling=mode", "--min-zoom=0",
         "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", pal_dir}));
    ASSERT_TRUE(alg_pal.Run()) << CPLGetLastErrorMsg();
    std::unique_ptr<GDALDataset> out_pal(GDALDataset::Open(
        (pal_dir + "/0/0/0.png").c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_pal, NotNull());
    EXPECT_THAT(out_pal->GetRasterBand(1)->GetColorTable(), NotNull());
  }
}

TEST_F(GdalalgRasterTileTest, ResamplingOverviewPathsAndExcludedValues) {
  // 1. Excluded values + average resampling -> sets warp options so
  // GenerateOverviewTile uses GDALWarp path.
  {
    auto ds = CreateMemDataset(64, 64, 3, GDT_Byte);
    ASSERT_THAT(ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/excl_values";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resampling=average", "--overview-resampling=average",
         "--excluded-values=(0,0,0),(255,255,255)",
         "--excluded-values-pct-threshold=50",
         "--nodata-values-pct-threshold=90", "--min-zoom=0", "--max-zoom=1",
         "--tile-size=64", "--num-threads=1", "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/1/0/0.png"));
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }

  // 2. "near" hidden alias + "rms" overview resampling -> exercises GDALWarp
  // overview path without excluded-values, plus --skip-blank after GDALWarp on
  // a fully transparent source.
  {
    auto transparent_ds =
        CreateMemDataset(64, 64, 4, GDT_Byte, 3857, nullptr,
                         /*fill_gradient=*/false, /*last_band_alpha=*/true);
    ASSERT_THAT(transparent_ds, NotNull());
    // Fill all bands with 0 (so alpha == 0 everywhere).
    for (int b = 1; b <= 4; ++b) {
      std::vector<uint8_t> zeros(64 * 64, 0);
      ASSERT_EQ(CE_None, transparent_ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 64, 64, zeros.data(), 64, 64,
                             GDT_Byte, 0, 0, nullptr));
    }

    const std::string out_dir = std::string(kTestDir) + "/warp_skip_blank";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(transparent_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resampling=near", "--overview-resampling=rms", "--skip-blank",
         "--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_FALSE(FileExists(out_dir + "/1/0/0.png"));
    EXPECT_FALSE(FileExists(out_dir + "/0/0/0.png"));
  }

  // 3. Partial overview extent (uses GDALTranslate VRT padding branch in
  // GenerateOverviewTile when z=2 is restricted via --min-x/--max-x so z=1's
  // MosaicDataset is smaller than z=1's full tile bounds).
  {
    GDALGeoTransform gt(-15000000.0, 200000.0, 0.0, 15000000.0, 0.0, -200000.0);
    auto ds = CreateMemDataset(64, 64, 3, GDT_Byte, 3857, &gt);
    ASSERT_THAT(ds, NotNull());
    const std::string out_dir = std::string(kTestDir) + "/translate_pad";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resampling=bilinear", "--overview-resampling=bilinear",
         "--min-zoom=0", "--max-zoom=2", "--min-x=1", "--max-x=1", "--min-y=1",
         "--max-y=1", "--tile-size=64", "--num-threads=1", "--webviewer=none",
         out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/2/1/1.png"));
    EXPECT_TRUE(FileExists(out_dir + "/1/0/0.png"));
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }

  // 4. Overview MosaicRasterBand alpha fill (255) when an upper tile had its
  // opaque alpha stripped (3 bands) while another tile kept 4 bands.
  {
    auto ds = CreateMemDataset(128, 64, 4, GDT_Byte, 3857, nullptr,
                               /*fill_gradient=*/true,
                               /*last_band_alpha=*/true);
    ASSERT_THAT(ds, NotNull());
    std::vector<uint8_t> alpha(128 * 64, 255);
    // Left half opaque (255 -> stripped to 3 bands), right half
    // semi-transparent (128 -> kept as 4 bands).
    for (int y = 0; y < 64; ++y) {
      for (int x = 64; x < 128; ++x) {
        alpha[static_cast<size_t>(y) * 128 + x] = 128;
      }
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(4)->RasterIO(GF_Write, 0, 0, 128, 64,
                                                      alpha.data(), 128, 64,
                                                      GDT_Byte, 0, 0, nullptr));
    const std::string out_dir = std::string(kTestDir) + "/mixed_alpha_mosaic";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=2", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }
}

TEST_F(GdalalgRasterTileTest, WebviewersStacMapmlAndKml) {
  auto ds = CreateMemDataset(64, 64, 3, GDT_Byte, 3857);
  ASSERT_THAT(ds, NotNull());
  ds->SetMetadataItem("TIFFTAG_DATETIME", "2025:05:10 14:30:00");
  ds->SetMetadataItem("start_datetime", "2025-05-01T00:00:00Z");
  ds->SetMetadataItem("end_datetime", "2025-05-31T23:59:59Z");
  ds->GetRasterBand(1)->SetDescription("RedBand");
  ds->GetRasterBand(1)->SetMetadataItem("CENTRAL_WAVELENGTH_UM", "0.665",
                                        "IMAGERY");
  ds->GetRasterBand(1)->SetMetadataItem("FWHM_UM", "0.030", "IMAGERY");

  const std::string out_dir = std::string(kTestDir) + "/pub_all";
  {
    const CPLConfigOptionSetter html_prec("GDAL_RASTER_TILE_HTML_PREC", "10",
                                          false);
    const CPLConfigOptionSetter kml_prec("GDAL_RASTER_TILE_KML_PREC", "10",
                                         false);
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=2", "--tile-size=64", "--num-threads=1",
         "--webviewer=all", "--kml", "--url=https://example.com/tiles",
         "--title=Map \"One\" & Two", "--copyright=Copyright (c) 2026",
         "--metadata=datetime=2025-05-11T08:00:00Z", "--copy-src-metadata",
         out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
  }

  // Verify leaflet.html.
  const std::string leaflet = ReadVsiFile(out_dir + "/leaflet.html");
  EXPECT_THAT(leaflet, HasSubstr("Map \\\"One\\\" & Two"));
  EXPECT_THAT(leaflet, HasSubstr("Copyright (c) 2026"));

  // Verify openlayers.html.
  const std::string openlayers = ReadVsiFile(out_dir + "/openlayers.html");
  EXPECT_THAT(openlayers, HasSubstr("Map &quot;One&quot; &amp; Two"));

  // Verify mapml.mapml.
  const std::string mapml = ReadVsiFile(out_dir + "/mapml.mapml");
  EXPECT_THAT(mapml, HasSubstr("OSMTILE"));
  EXPECT_THAT(mapml, HasSubstr("https://example.com/tiles/pub_all"));

  // Verify stacta.json via CPLJSONDocument.
  CPLJSONDocument stac_doc;
  ASSERT_TRUE(stac_doc.Load(out_dir + "/stacta.json"));
  const auto root = stac_doc.GetRoot();
  EXPECT_EQ("1.1.0", root.GetString("stac_version"));
  EXPECT_EQ("Map \"One\" & Two", root.GetString("id"));
  EXPECT_EQ("Polygon", root.GetObj("geometry").GetString("type"));
  const auto props = root.GetObj("properties");
  EXPECT_EQ("EPSG:3857", props.GetString("proj:code"));
  EXPECT_EQ("Copyright (c) 2026", props.GetString("copyright"));
  EXPECT_THAT(props.GetString("datetime"), HasSubstr("2025-05-11"));
  EXPECT_THAT(props.GetString("start_datetime"), HasSubstr("2025-05-01"));
  EXPECT_THAT(props.GetString("end_datetime"), HasSubstr("2025-05-31"));
  const auto bands =
      root.GetObj("asset_templates").GetObj("bands").GetArray("bands");
  ASSERT_EQ(3, bands.Size());
  EXPECT_EQ("RedBand", bands[0].GetString("name"));
  EXPECT_EQ("uint8", bands[0].GetString("data_type"));
  EXPECT_EQ("red", bands[0].GetString("eo:common_name"));
  EXPECT_DOUBLE_EQ(0.665, bands[0].GetDouble("eo:center_wavelength"));
  EXPECT_DOUBLE_EQ(0.030, bands[0].GetDouble("eo:full_width_half_max"));

  // Verify KML hierarchy (doc.kml and overview KML with NetworkLink +
  // LatLonBox for WebMercatorQuad).
  const std::string root_kml = ReadVsiFile(out_dir + "/doc.kml");
  EXPECT_THAT(root_kml, HasSubstr("<NetworkLink>"));
  EXPECT_THAT(root_kml, HasSubstr("0/0/0.kml"));
  const std::string z0_kml = ReadVsiFile(out_dir + "/0/0/0.kml");
  EXPECT_THAT(z0_kml, HasSubstr("<NetworkLink>"));
  EXPECT_THAT(z0_kml, HasSubstr("<LatLonBox>"));

  // Custom MapML template and /vsis3/ URL normalization in STAC.
  const std::string custom_tpl = std::string(kTestDir) + "/custom.mapml";
  ASSERT_TRUE(
      WriteRawFile(custom_tpl, "<custom title=\"${TITLE}\" url=\"${URL}\"/>"));
  const std::string custom_dir = std::string(kTestDir) + "/pub_custom";
  GDALRasterTileAlgorithm alg_custom;
  alg_custom.GetInputDatasets().emplace_back(ds.get());
  ASSERT_TRUE(alg_custom.ParseCommandLineArguments(
      {"--min-zoom=0", "--max-zoom=0", "--tile-size=64", "--num-threads=1",
       "--webviewer=mapml", "--webviewer=stac",
       "--mapml-template=" + custom_tpl, "--url=/vsis3/my-bucket/tiles/",
       custom_dir}));
  ASSERT_TRUE(alg_custom.Run()) << CPLGetLastErrorMsg();
  EXPECT_THAT(ReadVsiFile(custom_dir + "/mapml.mapml"),
              HasSubstr("<custom title="));
  EXPECT_THAT(ReadVsiFile(custom_dir + "/stacta.json"),
              HasSubstr("s3://my-bucket/tiles/pub_custom/"));

  // --webviewer=none creates no viewer files.
  const std::string none_dir = std::string(kTestDir) + "/pub_none";
  GDALRasterTileAlgorithm alg_none;
  alg_none.GetInputDatasets().emplace_back(ds.get());
  ASSERT_TRUE(alg_none.ParseCommandLineArguments(
      {"--min-zoom=0", "--max-zoom=0", "--tile-size=64", "--num-threads=1",
       "--webviewer=none", none_dir}));
  ASSERT_TRUE(alg_none.Run()) << CPLGetLastErrorMsg();
  EXPECT_FALSE(FileExists(none_dir + "/leaflet.html"));
  EXPECT_FALSE(FileExists(none_dir + "/openlayers.html"));
  EXPECT_FALSE(FileExists(none_dir + "/mapml.mapml"));
  EXPECT_FALSE(FileExists(none_dir + "/stacta.json"));
}

TEST_F(GdalalgRasterTileTest, ResumeAndCorruptTileError) {
  auto ds = CreateMemDataset(64, 64, 3, GDT_Byte);
  ASSERT_THAT(ds, NotNull());
  const std::string out_dir = std::string(kTestDir) + "/resume_dir";

  {
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
  }

  const std::string base_tile = out_dir + "/1/0/0.png";
  const std::string ovr_tile = out_dir + "/0/0/0.png";
  ASSERT_TRUE(FileExists(base_tile));
  ASSERT_TRUE(FileExists(ovr_tile));

  // Unlink z=0 overview tile and overwrite z=1 base tile with a sentinel so we
  // can confirm --resume preserves the existing base tile and recreates z=0.
  // First test preserving a valid PNG file by checking its exact byte size/tag
  // or deleting another tile.
  ASSERT_EQ(0, VSIUnlink(ovr_tile.c_str()));
  ASSERT_FALSE(FileExists(ovr_tile));

  {
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resume", "--min-zoom=0", "--max-zoom=1", "--tile-size=64",
         "--num-threads=1", "--webviewer=none", out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
  }
  EXPECT_TRUE(FileExists(ovr_tile));

  // Overwrite z=1 base tile with corrupt bytes and remove z=0 overview tile ->
  // MosaicRasterBand::IReadBlock detects corrupt existing file and fails.
  // Use --overview-resampling=nearest to avoid triggering an upstream MSAN bug
  // in gcore/rasterio.cpp:1856-1870 where GDALDataset::RasterIOResampled()
  // reads uninitialized pabyChunkNoDataMask bytes when poMaskBand->RasterIO()
  // returns CE_Failure on the corrupt base tile.
  ASSERT_TRUE(WriteRawFile(base_tile, "corrupt_not_a_png"));
  ASSERT_EQ(0, VSIUnlink(ovr_tile.c_str()));
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--resume", "--overview-resampling=nearest", "--min-zoom=0",
         "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("exists but cannot be opened"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterTileTest, ProgressCallbackAndCancellation) {
  auto ds = CreateMemDataset(64, 64, 3, GDT_Byte);
  ASSERT_THAT(ds, NotNull());

  // 1. Successful progress reporting.
  {
    struct ProgressState {
      int calls = 0;
      double last_pct = -1.0;
    } state;

    auto progress_cb = [](double pct, const char*, void* data) -> int {
      auto* s = static_cast<ProgressState*>(data);
      s->calls++;
      EXPECT_GE(pct, s->last_pct);
      s->last_pct = pct;
      return TRUE;
    };

    const std::string out_dir = std::string(kTestDir) + "/prog_ok";
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    EXPECT_TRUE(alg.Run(progress_cb, &state));
    EXPECT_GE(state.calls, 2);
    EXPECT_DOUBLE_EQ(1.0, state.last_pct);
  }

  // 2. Cancel during base tile generation.
  {
    auto cancel_first = [](double, const char*, void*) -> int { return FALSE; };
    const std::string out_dir = std::string(kTestDir) + "/prog_cancel_base";
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    EXPECT_FALSE(alg.Run(cancel_first, nullptr));
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
    CPLErrorReset();
  }

  // 3. Cancel during overview tile generation (on last call).
  {
    int call_count = 0;
    auto cancel_ovr = [](double pct, const char*, void* data) -> int {
      auto* count = static_cast<int*>(data);
      (*count)++;
      return pct >= 1.0 ? FALSE : TRUE;
    };
    const std::string out_dir = std::string(kTestDir) + "/prog_cancel_ovr";
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithm alg;
    alg.GetInputDatasets().emplace_back(ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--min-zoom=0", "--max-zoom=1", "--tile-size=64", "--num-threads=1",
         "--webviewer=none", out_dir}));
    EXPECT_FALSE(alg.Run(cancel_ovr, &call_count));
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterTileTest, MultiThreadedThreadPoolExecution) {
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
  // TODO(schwehr): Re-enable multi-threaded TSan once upstream
  // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
  GTEST_SKIP() << "Upstream GDALRasterBlock::TakeLock()/Touch() data race";
#endif
  // Lower the tiles-per-job threshold so 2x2 / 4x4 tile grids exercise the
  // multi-threaded CPLWorkerThreadPool path for both base and overview levels.
  const CPLConfigOptionSetter min_tiles("GDAL_THRESHOLD_MIN_TILES_PER_JOB", "1",
                                        false);

  // Use a file-backed GeoTIFF in /vsimem/ covering all 4 quadrants around (0,0)
  // so z=2 has 4x4 tiles and z=1 has 2x2 overview tiles.
  GDALGeoTransform gt(-10000000.0, 20000000.0 / 128, 0.0, 10000000.0, 0.0,
                      -20000000.0 / 128);
  auto mem_ds = CreateMemDataset(128, 128, 3, GDT_Byte, 3857, &gt);
  ASSERT_THAT(mem_ds, NotNull());
  const std::string src_tif = std::string(kTestDir) + "/mt_src.tif";
  ASSERT_TRUE(WriteTiffFromMem(src_tif, mem_ds.get()));

  // 1. Multi-threaded success with --num-threads=2 --parallel-method=thread.
  {
    const std::string out_dir = std::string(kTestDir) + "/mt_out_2";
    GDALRasterTileAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--num-threads=2", "--parallel-method=thread", "--min-zoom=0",
         "--max-zoom=2", "--tile-size=64", "--webviewer=none", src_tif,
         out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/2/1/1.png"));
    EXPECT_TRUE(FileExists(out_dir + "/1/0/0.png"));
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }

  // 2. More threads than tiles (8 threads on 2x2 tiles -> exercises the
  // ComputeJobChunkSize branch where nTilesPerCol <= nMaxJobCount /
  // nTilesPerRow).
  {
    const std::string out_dir = std::string(kTestDir) + "/mt_out_8";
    GDALRasterTileAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--num-threads=8", "--parallel-method=thread", "--min-zoom=1",
         "--max-zoom=2", "--tile-size=64", "--webviewer=none", src_tif,
         out_dir}));
    ASSERT_TRUE(alg.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(FileExists(out_dir + "/1/1/1.png"));
  }

  // 3. Cancellation during multi-threaded execution.
  {
    auto cancel_soon = [](double pct, const char*, void*) -> int {
      return pct > 0.0 ? FALSE : TRUE;
    };
    const std::string out_dir = std::string(kTestDir) + "/mt_cancel";
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--num-threads=2", "--parallel-method=thread", "--min-zoom=0",
         "--max-zoom=2", "--tile-size=64", "--webviewer=none", src_tif,
         out_dir}));
    EXPECT_FALSE(alg.Run(cancel_soon, nullptr));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterTileTest, SpawnParallelMethodWithMockGdal) {
  const auto temp_root =
      std::filesystem::path(::testing::TempDir()) / "gdalalg_raster_tile_spawn";
  std::filesystem::create_directories(temp_root);
  absl::Cleanup cleanup_temp = [&] { std::filesystem::remove_all(temp_root); };

  const auto bin_dir = temp_root / "bin";
  const auto out_dir = temp_root / "tiles_ok";
  const auto out_fail_dir = temp_root / "tiles_fail";
  std::filesystem::create_directories(bin_dir);

  const std::string src_tif = (temp_root / "spawn_src.tif").string();
  GDALGeoTransform gt(-10000000.0, 20000000.0 / 128, 0.0, 10000000.0, 0.0,
                      -20000000.0 / 128);
  auto mem_ds = CreateMemDataset(128, 128, 3, GDT_Byte, 3857, &gt);
  ASSERT_THAT(mem_ds, NotNull());
  ASSERT_TRUE(WriteTiffFromMem(src_tif, mem_ds.get()));

  // Mock gdal binary implementing the spawn IPC protocol:
  //   stdin:  --config ...\n ... END_CONFIG\n ... STOP\n
  //   stdout: '!.x' per tile in requested X/Y range, then '?E?N?D?'
  const auto mock_gdal = bin_dir / "gdal";
  {
    std::ofstream ofs(mock_gdal);
    ofs << R"sh(#!/bin/sh
fail=0
spurious=0
while IFS= read -r line; do
  case "$line" in
    END_CONFIG) break ;;
    *MOCK_GDAL_FAIL%3D1*|*MOCK_GDAL_FAIL=1*) fail=1 ;;
    *MOCK_GDAL_SPURIOUS%3D1*|*MOCK_GDAL_SPURIOUS=1*) spurious=1 ;;
  esac
done

if [ "$fail" = "1" ]; then
  printf 'GDAL: debug info\n' >&2
  printf 'ERROR 1: simulated child failure\n' >&2
  exit 1
fi

minx=0; maxx=0; miny=0; maxy=0
while [ $# -gt 0 ]; do
  case "$1" in
    --min-x|--ovr-min-x) minx="$2"; shift 2 ;;
    --max-x|--ovr-max-x) maxx="$2"; shift 2 ;;
    --min-y|--ovr-min-y) miny="$2"; shift 2 ;;
    --max-y|--ovr-max-y) maxy="$2"; shift 2 ;;
    *) shift ;;
  esac
done

count=$(( (maxx - minx + 1) * (maxy - miny + 1) ))
if [ "$spurious" = "1" ]; then
  printf 'Z'
fi
i=0
while [ "$i" -lt "$count" ]; do
  printf '!.x'
  i=$((i + 1))
done
printf '?E?N?D?'

while IFS= read -r line; do
  [ "$line" = "STOP" ] && break
done
exit 0
)sh";
  }
  ASSERT_EQ(0, chmod(mock_gdal.c_str(), 0755));

  const CPLConfigOptionSetter gdal_path("GDAL_PATH", bin_dir.c_str(), false);
  const CPLConfigOptionSetter min_tiles("GDAL_THRESHOLD_MIN_TILES_PER_JOB", "1",
                                        false);
  const CPLConfigOptionSetter min_threads(
      "GDAL_THRESHOLD_MIN_THREADS_FOR_SPAWN", "2", false);
  // Ensure at least one config option is forwarded by SendConfigOptions().
  const CPLConfigOptionSetter extra_cfg("GDAL_RASTER_TILE_TEST_OPT", "val",
                                        false);

  // 1. Successful spawn for both base (z=2) and overview (z=1) tiles, also
  // exercising AddArgToArgv for string, string-list, real, int, and bool args,
  // and spurious-char tolerance in GetProgressForChildProcesses().
  {
    const CPLConfigOptionSetter spurious("MOCK_GDAL_SPURIOUS", "1", false);
    double last_progress = 0.0;
    auto progress_cb = [](double pct, const char*, void* data) -> int {
      *static_cast<double*>(data) = pct;
      return TRUE;
    };

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--parallel-method=spawn", "--num-threads=2", "--min-zoom=0",
         "--max-zoom=2", "--tile-size=64", "--resampling=average",
         "--excluded-values=0,0,0", "--excluded-values-pct-threshold=50",
         "--metadata=K1=V1", "--metadata=K2=V2", "--skip-blank",
         "--webviewer=none", src_tif, out_dir.string()}));
    EXPECT_TRUE(alg.Run(progress_cb, &last_progress)) << CPLGetLastErrorMsg();
    EXPECT_DOUBLE_EQ(1.0, last_progress);
    CPLErrorReset();
  }

  // 2. Child failure path in WaitForSpawnedProcesses().
  {
    const CPLConfigOptionSetter fail_env("MOCK_GDAL_FAIL", "1", false);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterTileAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--parallel-method=spawn", "--num-threads=2", "--min-zoom=1",
         "--max-zoom=2", "--tile-size=64", "--webviewer=none", src_tif,
         out_fail_dir.string()}));
    EXPECT_FALSE(alg.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("failed"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterTileTest, PipelineIntegration) {
  auto mem_ds = CreateMemDataset(64, 64, 3, GDT_Byte, 3857);
  ASSERT_THAT(mem_ds, NotNull());
  const std::string src_tif = std::string(kTestDir) + "/pipe_src.tif";
  ASSERT_TRUE(WriteTiffFromMem(src_tif, mem_ds.get()));

  // 1. Direct read ! tile pipeline.
  {
    const std::string out_dir = std::string(kTestDir) + "/pipe_out_1";
    GDALRasterPipelineAlgorithm pipe;
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", src_tif, "!", "tile", "--min-zoom=0", "--max-zoom=1",
         "--tile-size=64", "--num-threads=1", "--webviewer=none", out_dir}));
    ASSERT_TRUE(pipe.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(pipe.Finalize());
    EXPECT_TRUE(FileExists(out_dir + "/1/0/0.png"));
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }

#if !defined(THREAD_SANITIZER) && !defined(__SANITIZE_THREAD__)
  // TODO(schwehr): Re-enable multi-threaded TSan once upstream
  // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
  // 2. Multi-step pipeline (read ! clip ! tile) using GDALG serialization in
  // GDALAbstractPipelineAlgorithm when num-threads > 1.
  {
    const std::string out_dir = std::string(kTestDir) + "/pipe_out_2";
    GDALRasterPipelineAlgorithm pipe;
    pipe.SetCallPath({"gdal", "raster", "pipeline"});
    ASSERT_TRUE(pipe.ParseCommandLineArguments(
        {"read", src_tif, "!", "clip",
         "--bbox=-5000000,-5000000,5000000,5000000", "!", "tile",
         "--min-zoom=0", "--max-zoom=0", "--tile-size=64", "--num-threads=2",
         "--webviewer=none", out_dir}));
    ASSERT_TRUE(pipe.Run()) << CPLGetLastErrorMsg();
    EXPECT_TRUE(pipe.Finalize());
    EXPECT_TRUE(FileExists(out_dir + "/0/0/0.png"));
  }
#endif
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover --parallel-method=fork and --spawned worker-side
// stdin/stdout loops in apps/gdalalg_raster_tile.cpp.
