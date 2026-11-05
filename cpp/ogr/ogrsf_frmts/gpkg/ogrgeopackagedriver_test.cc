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

// Unit tests for GeoPackage driver.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpkg/ogrgeopackagedriver.cpp
//   https://gdal.org/drivers/vector/gpkg.html
//   https://gdal.org/drivers/raster/gpkg.html

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/gcore/gdalsubdatasetinfo.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgeopackagedriver_test";

class OGRGeoPackageDriverTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    RegisterOGRGeoPackage();
    GDALRegister_MEM();
  }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void SetUp() override {
    VSIMkdir(kTestDir, 0755);
    CPLSetConfigOption("CPL_TMPDIR", kTestDir);
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
  }
};

TEST_F(OGRGeoPackageDriverTest, DriverMetadataAndOptions) {
  GDALDriverManager* manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, manager);
  GDALDriver* driver = manager->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // InitializeCreationOptionList called through GetMetadataItem and
  // GetMetadata.
  const char* co_list = driver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, co_list);
  EXPECT_NE(nullptr, strstr(co_list, "CreationOptionList"));

  // Call second time to test cached early-return path.
  EXPECT_NE(nullptr, driver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST));

  // GetMetadata on domain.
  char** metadata = driver->GetMetadata("");
  EXPECT_NE(nullptr, metadata);

  // Other metadata item.
  EXPECT_EQ(nullptr, driver->GetMetadataItem("NON_EXISTENT_METADATA_ITEM"));
}

TEST_F(OGRGeoPackageDriverTest, SubdatasetInfoParsing) {
  // Non-GPKG.
  EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("NETCDF:file:var"));

  // 2 parts (no subdataset component).
  EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GPKG:file.gpkg"));

  // 3 parts normal.
  std::unique_ptr<GDALSubdatasetInfo> info3(
      GDALGetSubdatasetInfo("GPKG:/vsimem/test.gpkg:layer"));
  ASSERT_NE(nullptr, info3);
  EXPECT_EQ("/vsimem/test.gpkg", info3->GetPathComponent());
  EXPECT_EQ("layer", info3->GetSubdatasetComponent());

  // 3 parts with drive letter (invalid, requires 4 parts).
  EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GPKG:C:layer"));

  // 4 parts with Windows drive letter.
  std::unique_ptr<GDALSubdatasetInfo> info4(
      GDALGetSubdatasetInfo("GPKG:C:\\data\\test.gpkg:layer"));
  ASSERT_NE(nullptr, info4);
  EXPECT_EQ("C:\\data\\test.gpkg", info4->GetPathComponent());
  EXPECT_EQ("layer", info4->GetSubdatasetComponent());

  // 4 parts without single-letter drive (invalid).
  EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GPKG:XYZ:path:layer"));
}

TEST_F(OGRGeoPackageDriverTest, IdentifyAndOpenVariousFormats) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // 1. STARTS_WITH GPKG:
  {
    GDALOpenInfo open_info("GPKG:/nonexistent:tbl", GDAL_OF_VECTOR);
    EXPECT_TRUE(driver->pfnIdentify(&open_info));
  }

  // 2. Short header or non-SQLite.
  {
    const std::string short_file = std::string(kTestDir) + "/short.gpkg";
    VSILFILE* fp = VSIFOpenL(short_file.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL("short", 1, 5, fp);
    VSIFCloseL(fp);

    GDALOpenInfo open_info(short_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_FALSE(driver->pfnIdentify(&open_info));
  }

  // 3. .gti.gpkg with GDAL_OF_RASTER.
  {
    const std::string gti_file = std::string(kTestDir) + "/test.gti.gpkg";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(gti_file.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    ds.reset();

    GDALOpenInfo open_info(gti_file.c_str(), GDAL_OF_RASTER);
    EXPECT_EQ(GDAL_IDENTIFY_UNKNOWN, driver->pfnIdentify(&open_info));

    GDALOpenInfo open_info_vec(gti_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_TRUE(driver->pfnIdentify(&open_info_vec));
  }

  // 4. Valid GPKG with non-standard extension (.bin / .db).
  {
    const std::string non_ext_file = std::string(kTestDir) + "/valid.bin";
    {
      WithQuietHandler quiet_handler;
      std::unique_ptr<GDALDataset> ds(
          driver->Create(non_ext_file.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      ASSERT_NE(nullptr, ds->CreateLayer("lyr", nullptr, wkbPoint, nullptr));
    }
    // Opening with warning emitted:
    {
      WithQuietHandler quiet_handler;
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          non_ext_file.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }
  }

  // 5. Corrupt / unrecognized application_id on .gpkg and .bin.
  {
    const std::string bad_app_gpkg = std::string(kTestDir) + "/bad_app.gpkg";
    const std::string bad_app_bin = std::string(kTestDir) + "/bad_app.bin";
    // Create valid GPKG first, then patch application_id at offset 68.
    {
      std::unique_ptr<GDALDataset> ds(
          driver->Create(bad_app_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      ASSERT_NE(nullptr, ds->CreateLayer("lyr", nullptr, wkbPoint, nullptr));
    }
    // Copy to bad_app_bin.
    CPLCopyFile(bad_app_bin.c_str(), bad_app_gpkg.c_str());

    // Patch bad application_id 0x12345678 at offset 68.
    auto patch_app_id = [](const std::string& path) {
      VSILFILE* fp = VSIFOpenL(path.c_str(), "r+b");
      ASSERT_NE(nullptr, fp);
      VSIFSeekL(fp, 68, SEEK_SET);
      const GByte bad_id[4] = {0x12, 0x34, 0x56, 0x78};
      VSIFWriteL(bad_id, 1, 4, fp);
      VSIFCloseL(fp);
    };
    patch_app_id(bad_app_gpkg);
    patch_app_id(bad_app_bin);

    // .bin with bad application_id -> Identify returns FALSE.
    {
      GDALOpenInfo open_info(bad_app_bin.c_str(), GDAL_OF_VECTOR);
      EXPECT_FALSE(driver->pfnIdentify(&open_info));
    }

    // .gpkg with bad application_id -> emits warning if
    // GPKG_WARN_UNRECOGNIZED_APPLICATION_ID=YES.
    {
      WithQuietHandler quiet_handler;
      GDALOpenInfo open_info(bad_app_gpkg.c_str(), GDAL_OF_VECTOR);
      EXPECT_TRUE(driver->pfnIdentify(&open_info));
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          bad_app_gpkg.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }

    // With GPKG_WARN_UNRECOGNIZED_APPLICATION_ID=NO.
    {
      CPLSetConfigOption("GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", "NO");
      absl::Cleanup reset_opt = [] {
        CPLSetConfigOption("GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", nullptr);
      };
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          bad_app_gpkg.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }
  }

  // 6. Unrecognized user_version (< 10200 and > 10400).
  {
    const std::string uv_old = std::string(kTestDir) + "/uv_old.gpkg";
    const std::string uv_new = std::string(kTestDir) + "/uv_new.gpkg";
    const std::string uv_bin = std::string(kTestDir) + "/uv_old.bin";

    auto create_patched_uv = [&](const std::string& path, GUInt32 uv) {
      std::unique_ptr<GDALDataset> ds(
          driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      ASSERT_NE(nullptr, ds->CreateLayer("lyr", nullptr, wkbPoint, nullptr));
      ds.reset();
      VSILFILE* fp = VSIFOpenL(path.c_str(), "r+b");
      ASSERT_NE(nullptr, fp);
      VSIFSeekL(fp, 60, SEEK_SET);  // user_version at offset 60
      const GUInt32 msb_uv = CPL_MSBWORD32(uv);
      VSIFWriteL(&msb_uv, 1, 4, fp);
      VSIFCloseL(fp);
    };

    create_patched_uv(uv_old, 10100);  // < 10200
    create_patched_uv(uv_new, 99999);  // > 10400
    create_patched_uv(uv_bin, 10100);  // non-gpkg extension

    // uv_bin with non-standard extension -> Identify returns FALSE.
    {
      GDALOpenInfo open_info(uv_bin.c_str(), GDAL_OF_VECTOR);
      EXPECT_FALSE(driver->pfnIdentify(&open_info));
    }

    // uv_old with warning YES.
    {
      WithQuietHandler quiet_handler;
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          uv_old.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }

    // uv_old with warning NO.
    {
      CPLSetConfigOption("GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", "NO");
      absl::Cleanup reset_opt = [] {
        CPLSetConfigOption("GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", nullptr);
      };
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          uv_old.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }

    // uv_new with warning YES (> 10400 "may only be partially supported").
    {
      WithQuietHandler quiet_handler;
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          uv_new.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }

    // uv_new with warning NO.
    {
      CPLSetConfigOption("GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", "NO");
      absl::Cleanup reset_opt = [] {
        CPLSetConfigOption("GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", nullptr);
      };
      std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
          uv_new.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
      EXPECT_NE(nullptr, ds);
    }
  }

  // 7. .gpkg.zip recognition.
  {
    // Create zip containing 1 gpkg.
    const std::string zip_file = std::string(kTestDir) + "/one.gpkg.zip";
    {
      std::unique_ptr<GDALDataset> ds(
          driver->Create(zip_file.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      ASSERT_NE(nullptr, ds->CreateLayer("lyr", nullptr, wkbPoint, nullptr));
    }
    // Identify on one.gpkg.zip:
    GDALOpenInfo open_info(zip_file.c_str(), GDAL_OF_VECTOR);
    EXPECT_TRUE(driver->pfnIdentify(&open_info));

    // Open on one.gpkg.zip:
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(GDALOpenEx(
        zip_file.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    EXPECT_NE(nullptr, ds);

    // Create zip containing no gpkg files (e.g. text file).
    const std::string zip_no_gpkg = std::string(kTestDir) + "/no.gpkg.zip";
    void* h_zip_no = CPLCreateZip(zip_no_gpkg.c_str(), nullptr);
    ASSERT_NE(nullptr, h_zip_no);
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip_no, "readme.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip_no, "hello", 5));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip_no));
    EXPECT_EQ(CE_None, CPLCloseZip(h_zip_no));

    GDALOpenInfo open_info_no_gpkg(zip_no_gpkg.c_str(), GDAL_OF_VECTOR);
    EXPECT_FALSE(driver->pfnIdentify(&open_info_no_gpkg));

    // Create zip containing 2 gpkg files.
    const std::string zip_two_gpkg = std::string(kTestDir) + "/two.gpkg.zip";
    void* h_zip_two = CPLCreateZip(zip_two_gpkg.c_str(), nullptr);
    ASSERT_NE(nullptr, h_zip_two);
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip_two, "one.gpkg", nullptr));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip_two));
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip_two, "two.gpkg", nullptr));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip_two));
    EXPECT_EQ(CE_None, CPLCloseZip(h_zip_two));

    GDALOpenInfo open_info_two_gpkg(zip_two_gpkg.c_str(), GDAL_OF_VECTOR);
    EXPECT_FALSE(driver->pfnIdentify(&open_info_two_gpkg));
  }
}

TEST_F(OGRGeoPackageDriverTest, CreateOptionsAndExtensions) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // :memory:
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(":memory:", 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_NE(nullptr, ds);
  }

  // .gpkx extension.
  {
    const std::string gpkx_path = std::string(kTestDir) + "/test.gpkx";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(gpkx_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_NE(nullptr, ds);
  }

  // .gpkg.zip creation.
  {
    const std::string zip_path = std::string(kTestDir) + "/out.gpkg.zip";
    std::unique_ptr<GDALDataset> ds(
        driver->Create(zip_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_NE(nullptr, ds);
  }

  // Non-conformant extension (.db) generates warning.
  {
    const std::string db_path = std::string(kTestDir) + "/test.db";
    WithQuietHandler quiet_handler;
    std::unique_ptr<GDALDataset> ds(
        driver->Create(db_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_NE(nullptr, ds);
  }

  // Invalid creation path fails.
  {
    WithQuietHandler quiet_handler;
    std::unique_ptr<GDALDataset> ds(driver->Create(
        "/nonexistent_dir_12345/fail.gpkg", 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_EQ(nullptr, ds);
  }
}

TEST_F(OGRGeoPackageDriverTest, DeleteDataset) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // Delete valid GPKG with .aux.xml sidecar.
  const std::string del_path = std::string(kTestDir) + "/del.gpkg";
  const std::string aux_path = del_path + ".aux.xml";
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(del_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
  }
  // Create .aux.xml sidecar.
  VSILFILE* fp = VSIFOpenL(aux_path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL("<PAMDataset/>", 1, 13, fp);
  VSIFCloseL(fp);

  EXPECT_EQ(CE_None, driver->Delete(del_path.c_str()));
  VSIStatBufL sStat;
  EXPECT_NE(0, VSIStatL(del_path.c_str(), &sStat));
  EXPECT_NE(0, VSIStatL(aux_path.c_str(), &sStat));

  // Delete nonexistent path returns CE_Failure.
  EXPECT_EQ(CE_Failure, driver->Delete("/vsimem/missing_file.gpkg"));
}

TEST_F(OGRGeoPackageDriverTest, RepackAlgorithm) {
  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, driver);

  // Unknown algorithm path.
  EXPECT_EQ(nullptr, driver->InstantiateAlgorithm({"unknown"}));
  EXPECT_EQ(nullptr, driver->InstantiateAlgorithm({"repack", "extra"}));

  // Valid repack algorithm on GPKG dataset.
  const std::string gpkg_path = std::string(kTestDir) + "/repack.gpkg";
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(gpkg_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // Open mutable GPKG dataset.
  std::unique_ptr<GDALDataset> gpkg_ds(static_cast<GDALDataset*>(
      GDALOpenEx(gpkg_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, gpkg_ds);

  {
    std::unique_ptr<GDALAlgorithm> algo(
        driver->InstantiateAlgorithm({"repack"}));
    ASSERT_NE(nullptr, algo);
    auto* arg = algo->GetArg("dataset");
    ASSERT_NE(nullptr, arg);
    EXPECT_TRUE(arg->Set(gpkg_ds.get()));
    EXPECT_TRUE(algo->Run(nullptr, nullptr));
  }

  // Run on non-GPKG dataset (MEM).
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  std::unique_ptr<GDALDataset> mem_ds(
      mem_driver->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);

  {
    std::unique_ptr<GDALAlgorithm> algo_mem(
        driver->InstantiateAlgorithm({"repack"}));
    ASSERT_NE(nullptr, algo_mem);
    auto* arg_mem = algo_mem->GetArg("dataset");
    ASSERT_NE(nullptr, arg_mem);
    EXPECT_TRUE(arg_mem->Set(mem_ds.get()));
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(algo_mem->Run(nullptr, nullptr));
  }
}

// TODO: b/335317901 - Cover remaining unrecognized driver registration and
// metadata branches in ogrgeopackagedriver.cpp.

}  // namespace
}  // namespace autotest2
