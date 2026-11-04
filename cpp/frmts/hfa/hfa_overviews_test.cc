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
// Tests HFA auxiliary overview building (hfa_overviews.cpp).
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfa_overviews.cpp

#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/hfa_overviews_test";

class HfaOverviewsTest : public ::testing::Test {
 protected:
  // Deliberately isolates HFA + MEM registrations per test case.
  void SetUp() override {
    VSIMkdir(kTestDir, 0755);
    GDALRegister_HFA();
    GDALRegister_MEM();
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    GDALDestroyDriverManager();
    CPLErrorReset();
  }

  static GDALDatasetUniquePtr CreateMemDataset(int xsize, int ysize, int bands,
                                               GDALDataType dt) {
    GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
    EXPECT_NE(nullptr, mem_driver);
    if (mem_driver == nullptr) {
      return nullptr;
    }
    return GDALDatasetUniquePtr(
        mem_driver->Create("", xsize, ysize, bands, dt, nullptr));
  }
};

TEST_F(HfaOverviewsTest, BuildOverviewsCreatesAndAppendsAuxFile) {
  GDALDatasetUniquePtr parent_ds = CreateMemDataset(64, 64, 2, GDT_Byte);
  ASSERT_NE(nullptr, parent_ds);

  constexpr char kOvrFilename[] = "/vsimem/hfa_overviews_test/parent_img.aux";
  constexpr int kBandList[] = {1, 2};
  constexpr int kFirstOverviews[] = {2};
  constexpr int kSecondOverviews[] = {4};

  GDALDataset* ovr_raw = nullptr;
  absl::Cleanup ovr_cleanup = [&] {
    if (ovr_raw != nullptr) {
      GDALClose(ovr_raw);
    }
  };

  // 1. First call creates the .aux dataset when *ppoODS == nullptr.
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            HFAAuxBuildOverviews(kOvrFilename, parent_ds.get(), &ovr_raw, 2,
                                 kBandList, 1, kFirstOverviews, "NEAREST",
                                 GDALDummyProgress, nullptr, nullptr));
  ASSERT_NE(nullptr, ovr_raw);
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, ovr_raw->GetRasterBand(1));
  EXPECT_EQ(1, ovr_raw->GetRasterBand(1)->GetOverviewCount());

  // 2. Second call reuses the existing *ppoODS dataset and adds another level.
  EXPECT_EQ(CE_None,
            HFAAuxBuildOverviews(kOvrFilename, parent_ds.get(), &ovr_raw, 2,
                                 kBandList, 1, kSecondOverviews, "NEAREST",
                                 GDALDummyProgress, nullptr, nullptr));
  EXPECT_EQ(2, ovr_raw->GetRasterBand(1)->GetOverviewCount());
}

TEST_F(HfaOverviewsTest, MixedBandTypesReturnsFailure) {
  GDALDatasetUniquePtr parent_ds = CreateMemDataset(32, 32, 1, GDT_Byte);
  ASSERT_NE(nullptr, parent_ds);
  ASSERT_EQ(CE_None, parent_ds->AddBand(GDT_UInt16, nullptr));

  GDALDataset* ovr_raw = nullptr;
  absl::Cleanup ovr_cleanup = [&] {
    if (ovr_raw != nullptr) {
      GDALClose(ovr_raw);
    }
  };
  constexpr int kBandList[] = {1, 2};
  constexpr int kOverviews[] = {2};

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              HFAAuxBuildOverviews("/vsimem/hfa_overviews_test/mixed_bands.aux",
                                   parent_ds.get(), &ovr_raw, 2, kBandList, 1,
                                   kOverviews, "NEAREST", GDALDummyProgress,
                                   nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("mixture of band data types"));
    CPLErrorReset();
  }
  EXPECT_EQ(nullptr, ovr_raw);
}

TEST_F(HfaOverviewsTest, UnsupportedBandTypeReturnsFailure) {
  GDALDatasetUniquePtr parent_ds = CreateMemDataset(32, 32, 1, GDT_CInt16);
  ASSERT_NE(nullptr, parent_ds);

  GDALDataset* ovr_raw = nullptr;
  absl::Cleanup ovr_cleanup = [&] {
    if (ovr_raw != nullptr) {
      GDALClose(ovr_raw);
    }
  };
  constexpr int kBandList[] = {1};
  constexpr int kOverviews[] = {2};

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              HFAAuxBuildOverviews(
                  "/vsimem/hfa_overviews_test/unsupported_dt.aux",
                  parent_ds.get(), &ovr_raw, 1, kBandList, 1, kOverviews,
                  "NEAREST", GDALDummyProgress, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    CPLErrorReset();
  }
  EXPECT_EQ(nullptr, ovr_raw);
}

TEST_F(HfaOverviewsTest, MissingHfaDriverReturnsFailure) {
  // 1. Deregister HFA driver so HFAAuxBuildOverviews fails driver lookup.
  GDALDriver* hfa_driver = GetGDALDriverManager()->GetDriverByName("HFA");
  ASSERT_NE(nullptr, hfa_driver);
  GetGDALDriverManager()->DeregisterDriver(hfa_driver);
  absl::Cleanup reregister = [&] {
    GetGDALDriverManager()->RegisterDriver(hfa_driver);
  };

  GDALDatasetUniquePtr parent_ds = CreateMemDataset(32, 32, 1, GDT_Byte);
  ASSERT_NE(nullptr, parent_ds);

  GDALDataset* ovr_raw = nullptr;
  absl::Cleanup ovr_cleanup = [&] {
    if (ovr_raw != nullptr) {
      GDALClose(ovr_raw);
    }
  };
  constexpr int kBandList[] = {1};
  constexpr int kOverviews[] = {2};

  // 2. Verify CPLError failure when HFA driver is unavailable.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              HFAAuxBuildOverviews("/vsimem/hfa_overviews_test/no_hfa.aux",
                                   parent_ds.get(), &ovr_raw, 1, kBandList, 1,
                                   kOverviews, "NEAREST", GDALDummyProgress,
                                   nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("HFA driver is unavailable"));
    CPLErrorReset();
  }
  EXPECT_EQ(nullptr, ovr_raw);
}

// TODO: b/335317901 - Cover any additional overview error branches added to
// hfa_overviews.cpp in future upstream GDAL upgrades.

}  // namespace
}  // namespace autotest2
