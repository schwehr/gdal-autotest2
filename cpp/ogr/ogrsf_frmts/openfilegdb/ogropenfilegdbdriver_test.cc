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
// Tests OpenFileGDB driver entrypoints and repack algorithm
// (ogropenfilegdbdriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdbdriver.cpp

#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogr_openfilegdb.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdbdrivercore.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern "C" void RegisterOGROpenFileGDB();

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogropenfilegdbdriver_test";

class OgrOpenFileGdbDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGROpenFileGDB();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OgrOpenFileGdbDriverTest, RegisterTwice) {
  // Calling RegisterOGROpenFileGDB a second time hits the early exit
  // where the driver is already registered.
  RegisterOGROpenFileGDB();
  EXPECT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("OpenFileGDB"));
}

TEST_F(OgrOpenFileGdbDriverTest, CreateInvalidParameters) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);

  // Non-vector create (e.g. raster dimensions specified)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDataset* ds =
        driver->Create((std::string(kTestDir) + "/bad_raster.gdb").c_str(), 10,
                       10, 1, GDT_Byte, nullptr);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("only vector datasets supported"));
  }

  // Invalid file extension / name that FileGDB datasource cannot create
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDataset* ds =
        driver->Create((std::string(kTestDir) + "/invalid_ext.txt").c_str(), 0,
                       0, 0, GDT_Unknown, nullptr);
    EXPECT_EQ(nullptr, ds);
  }
}

TEST_F(OgrOpenFileGdbDriverTest, CreateAndOpenVector) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);

  const std::string gdb_path = std::string(kTestDir) + "/valid.gdb";
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFeatureDefn* defn = layer->GetLayerDefn();
    ASSERT_NE(nullptr, defn);
    auto feature = std::make_unique<OGRFeature>(defn);
    auto point = std::make_unique<OGRPoint>(1.0, 2.0);
    EXPECT_EQ(OGRERR_NONE, feature->SetGeometry(point.get()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feature.get()));
  }

  // Open non-existent or non-GDB path returns nullptr.
  {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        (std::string(kTestDir) + "/missing.gdb").c_str(), GDAL_OF_VECTOR));
    EXPECT_EQ(nullptr, ds);
  }

  // Open valid created dataset
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(gdb_path.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_STREQ("points", layer->GetName());
    EXPECT_EQ(1, layer->GetFeatureCount());
  }
}

TEST_F(OgrOpenFileGdbDriverTest, DeleteDataset) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);

  // Deleting non-existent dataset returns CE_Failure
  {
    EXPECT_EQ(
        CE_Failure,
        driver->Delete((std::string(kTestDir) + "/nonexistent.gdb").c_str()));
  }

  // Create valid dataset and delete it successfully
  {
    const std::string gdb_path = std::string(kTestDir) + "/to_delete.gdb";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    ds.reset();

    EXPECT_EQ(CE_None, driver->Delete(gdb_path.c_str()));
  }

  // Delete directory with a subdirectory that VSIUnlink cannot remove
  {
    const std::filesystem::path undeletable =
        std::filesystem::path(::testing::TempDir()) / "undeletable.gdb";
    std::filesystem::create_directories(undeletable / "sub");
    absl::Cleanup cleanup = [&] {
      std::error_code ec;
      std::filesystem::remove_all(undeletable, ec);
    };

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, driver->Delete(undeletable.string().c_str()));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot delete"));
  }
}

TEST_F(OgrOpenFileGdbDriverTest, RepackAlgorithm) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);

  // Non-matching algorithm name returns nullptr
  EXPECT_EQ(nullptr, driver->InstantiateAlgorithm({"unknown"}));

  // 1. Run on non-OpenFileGDB dataset returns error
  {
    GDALRegister_MEM();
    auto* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_driver);
    GDALDatasetUniquePtr mem_ds(
        mem_driver->Create("my_mem_ds", 1, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);

    std::unique_ptr<GDALAlgorithm> alg(
        driver->InstantiateAlgorithm({"repack"}));
    ASSERT_NE(nullptr, alg);
    EXPECT_TRUE(alg->GetArg("dataset")->Set(mem_ds.get()));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is not a FileGeoDatabase"));
  }

  // Create valid writable FileGDB dataset
  const std::string gdb_path = std::string(kTestDir) + "/repack.gdb";
  {
    GDALDatasetUniquePtr ds(
        driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
    auto feature = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    auto point = std::make_unique<OGRPoint>(10.0, 20.0);
    EXPECT_EQ(OGRERR_NONE, feature->SetGeometry(point.get()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feature.get()));
  }

  // 2. Run on writable dataset succeeds
  {
    GDALDatasetUniquePtr ds(
        GDALDataset::Open(gdb_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, ds);

    std::unique_ptr<GDALAlgorithm> alg(
        driver->InstantiateAlgorithm({"repack"}));
    ASSERT_NE(nullptr, alg);
    EXPECT_TRUE(alg->GetArg("dataset")->Set(ds.get()));
    EXPECT_TRUE(alg->Run());
  }

  // 3. Run on read-only dataset fails because Repack on layer requires update
  {
    GDALDatasetUniquePtr ds(
        GDALDataset::Open(gdb_path.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, ds);

    std::unique_ptr<GDALAlgorithm> alg(
        driver->InstantiateAlgorithm({"repack"}));
    ASSERT_NE(nullptr, alg);
    EXPECT_TRUE(alg->GetArg("dataset")->Set(ds.get()));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg->Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Repack of layer"));
  }
}

TEST_F(OgrOpenFileGdbDriverTest, OpenRetryFileGdb) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);

  const std::string gdb_path = std::string(kTestDir) + "/cdf.gdb";
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("cdf_layer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);
  }

  // Add dummy .cdf file to trigger bRetryFileGDB in Open
  VsiMemTempWrapper cdf_file(gdb_path + "/a00000009.gdbtable.cdf", "dummy");

  // Register a temporary mock "FileGDB" driver to verify retry dispatch
  static bool filegdb_open_called = false;
  filegdb_open_called = false;

  auto* fake_filegdb = new GDALDriver();
  fake_filegdb->SetDescription("FileGDB");
  fake_filegdb->pfnOpen = [](GDALOpenInfo* info) -> GDALDataset* {
    filegdb_open_called = true;
    EXPECT_STREQ("NO", CSLFetchNameValue(info->papszOpenOptions,
                                         "@MAY_USE_OPENFILEGDB"));
    return nullptr;
  };
  GetGDALDriverManager()->RegisterDriver(fake_filegdb);
  absl::Cleanup cleanup_driver = [fake_filegdb] {
    GetGDALDriverManager()->DeregisterDriver(fake_filegdb);
    delete fake_filegdb;
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(gdb_path.c_str(), GDAL_OF_VECTOR));
  EXPECT_TRUE(filegdb_open_called);
}

TEST_F(OgrOpenFileGdbDriverTest, OpenIdentifyFalse) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);
  GDALOpenInfo open_info("/vsimem/not_a_gdb.txt", GDAL_OF_VECTOR);
  EXPECT_EQ(nullptr, driver->pfnOpen(&open_info));
}

TEST_F(OgrOpenFileGdbDriverTest, OpenIdentifiedEmptyGdbFails) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);
  const std::string empty_gdb = std::string(kTestDir) + "/empty.gdb";
  ASSERT_EQ(0, VSIMkdir(empty_gdb.c_str(), 0755));
  GDALOpenInfo open_info(empty_gdb.c_str(), GDAL_OF_VECTOR);
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, driver->pfnOpen(&open_info));
}

TEST_F(OgrOpenFileGdbDriverTest, DeleteRmdirFails) {
  auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
  ASSERT_NE(nullptr, driver);

  const std::filesystem::path parent =
      std::filesystem::path(::testing::TempDir()) / "ro_parent";
  const std::filesystem::path target = parent / "target.gdb";
  std::filesystem::create_directories(target);
  auto* fp = VSIFOpenL((target / "dummy.txt").string().c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  ASSERT_EQ(0, VSIFCloseL(fp));

  // Make parent directory read-only so rmdir(target) fails with EACCES.
  std::filesystem::permissions(parent, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_exec);
  absl::Cleanup restore_perms = [&] {
    std::error_code ec;
    std::filesystem::permissions(parent, std::filesystem::perms::all, ec);
    std::filesystem::remove_all(parent, ec);
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, driver->Delete(target.string().c_str()));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot delete"));
}

}  // namespace
}  // namespace autotest2
