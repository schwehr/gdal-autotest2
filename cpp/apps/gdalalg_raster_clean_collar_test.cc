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
// Tests for GDALRasterCleanCollarAlgorithm
// (apps/gdalalg_raster_clean_collar.cpp and
// apps/gdalalg_raster_clean_collar.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_clean_collar.cpp

#include "third_party/gdal/apps/gdalalg_raster_clean_collar.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>  // NOLINT
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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_clean_collar_test";

// Creates a 6x6 GTiff with a noisy black/white collar around a 2x2 center.
void CreateCollarTiff(const std::string& path, int bands, uint8_t collar_val,
                      uint8_t center_val, bool last_band_alpha = false,
                      bool per_dataset_mask = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 6, 6, bands, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  for (int b = 1; b <= bands; ++b) {
    std::vector<uint8_t> pixels(36, collar_val);
    if (last_band_alpha && b == bands) {
      std::fill(pixels.begin(), pixels.end(), 255);
      ds->GetRasterBand(b)->SetColorInterpretation(GCI_AlphaBand);
    } else {
      for (int y = 2; y < 4; ++y) {
        for (int x = 2; x < 4; ++x) {
          pixels[y * 6 + x] = center_val;
        }
      }
    }
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 6, 6,
                                                      pixels.data(), 6, 6,
                                                      GDT_Byte, 0, 0, nullptr));
  }

  if (per_dataset_mask) {
    ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
  }
}

class GdalalgRasterCleanCollarTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterCleanCollarTest, PropertiesAndArgumentValidation) {
  const std::string in_tif = std::string(kTestDir) + "/val_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/val_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(in_tif, 3, 2, 120));

  GDALRasterCleanCollarAlgorithm alg;
  EXPECT_EQ("clean-collar", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_STREQ("/programs/gdal_raster_clean_collar.html",
               alg.GetHelpURL().c_str());

  // Invalid color tuple fails validation.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm bad;
    EXPECT_FALSE(bad.ParseCommandLineArguments(
        {"--color", "10,notanint,30", in_tif, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("tuple of integer"));
    CPLErrorReset();
  }

  // Mutually exclusive --add-alpha and --add-mask fail.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm bad;
    EXPECT_FALSE(bad.ParseCommandLineArguments(
        {"--add-alpha", "--add-mask", in_tif, out_tif}));
    CPLErrorReset();
  }

  // Invalid algorithm choice fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm bad;
    EXPECT_FALSE(bad.ParseCommandLineArguments(
        {"--algorithm", "invalid_algo", in_tif, out_tif}));
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterCleanCollarTest, ErrorPathsOnInputAndOutputDatasets) {
  const std::string in_tif = std::string(kTestDir) + "/err_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(in_tif, 1, 2, 120));
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(out_tif, 1, 2, 120));

  // Missing output without --update fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Output dataset is not specified"));
    CPLErrorReset();
  }

  // Existing output without --overwrite or --update fails validation.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments({in_tif, out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // Same read-only dataset as both input and output fails.
  {
    std::unique_ptr<GDALDataset> ro_ds(
        GDALDataset::Open(in_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ro_ds);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm alg;
    alg.GetArg(GDAL_ARG_NAME_INPUT)
        ->Get<GDALArgDatasetValue>()
        .Set(ro_ds.get());
    alg.GetArg(GDAL_ARG_NAME_OUTPUT)
        ->Get<GDALArgDatasetValue>()
        .Set(ro_ds.get());
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("opened in update mode"));
    CPLErrorReset();
  }

  // Existing output with -f MEM bypasses ValidateArguments() existence check
  // and hits RunImpl()'s existing-file check.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "MEM", in_tif, out_tif}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
    CPLErrorReset();
  }

  // Non-existent parent directory causes GDALNearblack() to fail in RunImpl().
  {
    const std::string bad_out = (std::filesystem::path(::testing::TempDir()) /
                                 "missing_clean_collar_dir" / "out.tif")
                                    .string();
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_tif, bad_out}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterCleanCollarTest,
       CleanCollarBlackWhiteCustomColorsAndAlpha) {
  const std::string in_rgb = std::string(kTestDir) + "/rgb.tif";
  const std::string in_rgba = std::string(kTestDir) + "/rgba.tif";
  const std::string in_white = std::string(kTestDir) + "/white.tif";
  const std::string in_mask = std::string(kTestDir) + "/masked.tif";
  const std::string out_tif = std::string(kTestDir) + "/cleaned.tif";
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(in_rgb, 3, 5, 120));
  ASSERT_NO_FATAL_FAILURE(
      CreateCollarTiff(in_rgba, 4, 3, 120, /*last_band_alpha=*/true));
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(in_white, 3, 250, 120));
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(in_mask, 1, 3, 120, false, true));

  // Pre-create out_tif to exercise --overwrite deletion path.
  ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(out_tif, 3, 0, 0));

  // 1. Overwrite existing output with --color black, --add-alpha, -of, --co.
  {
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "-f", "GTiff", "--co", "COMPRESS=NONE", "--color",
         "black", "--color-threshold", "10", "--pixel-distance", "1",
         "--add-alpha", "--algorithm", "floodfill", in_rgb, out_tif}));
    int progress_calls = 0;
    auto cb = [](double, const char*, void* p) -> int {
      ++(*static_cast<int*>(p));
      return TRUE;
    };
    EXPECT_TRUE(alg.Run(cb, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand,
              out_ds->GetRasterBand(4)->GetColorInterpretation());
    uint8_t corner_alpha = 255;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(4)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &corner_alpha, 1, 1, GDT_Byte,
                           0, 0, nullptr));
    EXPECT_EQ(0, corner_alpha);
  }

  // 2. White color + twopasses + --add-mask.
  {
    const std::string out_white = std::string(kTestDir) + "/white_out.tif";
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--color", "white", "--color-threshold", "10", "--pixel-distance", "1",
         "--add-mask", "--algorithm", "twopasses", in_white, out_white}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_white.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(GMF_PER_DATASET, out_ds->GetRasterBand(1)->GetMaskFlags());
  }

  // 3. Explicit numeric color tuple on RGBA input (auto-preserves alpha).
  {
    const std::string out_rgba = std::string(kTestDir) + "/rgba_out.tif";
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--color", "black", "--color",
                                               "4,4,4", "--color-threshold",
                                               "5", in_rgba, out_rgba}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 4. Input with per-dataset mask auto-propagates -setmask.
  {
    const std::string out_masked = std::string(kTestDir) + "/masked_out.tif";
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({in_mask, out_masked}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 5. Update existing output dataset via --update.
  {
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--update", "--color", "black", in_rgb, out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 6. Overwrite an existing file when -f MEM is specified (exercises
  //    RunImpl() unlink of existing output dataset).
  {
    const std::string mem_target = std::string(kTestDir) + "/mem_target.tif";
    ASSERT_NO_FATAL_FAILURE(CreateCollarTiff(mem_target, 3, 0, 0));
    GDALRasterCleanCollarAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--overwrite", "-f", "MEM", "--color", "black", in_rgb, mem_target}));
    EXPECT_TRUE(alg.Run());
    EXPECT_NE(nullptr, alg.GetArg(GDAL_ARG_NAME_OUTPUT)
                           ->Get<GDALArgDatasetValue>()
                           .GetDatasetRef());
    EXPECT_TRUE(alg.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
