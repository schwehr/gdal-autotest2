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
// Tests for GDALRasterOverviewAlgorithmDelete
// (apps/gdalalg_raster_overview_delete.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_overview_delete.cpp

#include "third_party/gdal/apps/gdalalg_raster_overview_delete.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_overview_delete_test";

class GdalalgRasterOverviewDeleteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTinyTiff(const std::string& path) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 16, 16, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  std::vector<uint8_t> pixels(16 * 16, 7);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16,
                                                    pixels.data(), 16, 16,
                                                    GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterOverviewDeleteTest, BasicPropertiesAndMissingArgs) {
  GDALRasterOverviewAlgorithmDelete alg;
  EXPECT_EQ("delete", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_overview_delete.html", alg.GetHelpURL());

  WithQuietHandler quiet;
  EXPECT_FALSE(alg.ParseCommandLineArguments({}));
}

TEST_F(GdalalgRasterOverviewDeleteTest, DeleteInternalOverviews) {
  const std::string tif_path = std::string(kTestDir) + "/internal.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    constexpr std::array<int, 2> kLevels = {2, 4};
    ASSERT_EQ(CE_None,
              ds->BuildOverviews("NEAREST", kLevels.size(), kLevels.data(), 0,
                                 nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
  }

  {
    GDALRasterOverviewAlgorithmDelete alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({tif_path}));
    int progress_calls = 0;
    auto progress = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress, &progress_calls));
    EXPECT_GE(progress_calls, 0);
    EXPECT_TRUE(alg.Finalize());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
  }
}

TEST_F(GdalalgRasterOverviewDeleteTest, DeleteExternalOverviews) {
  const std::string tif_path = std::string(kTestDir) + "/external.tif";
  const std::string ovr_path = tif_path + ".ovr";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    constexpr std::array<int, 2> kLevels = {2, 4};
    ASSERT_EQ(CE_None,
              ds->BuildOverviews("NEAREST", kLevels.size(), kLevels.data(), 0,
                                 nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
  }

  VSIStatBufL sbuf;
  ASSERT_EQ(0, VSIStatL(ovr_path.c_str(), &sbuf));

  {
    GDALRasterOverviewAlgorithmDelete alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--external", tif_path}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  EXPECT_NE(0, VSIStatL(ovr_path.c_str(), &sbuf));
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
  }
}

}  // namespace
}  // namespace autotest2
