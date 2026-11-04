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
// Tests for OGR CSV driver (ogrcsvdriver.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/csv.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/csv/ogrcsvdriver.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_csv.py

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/csv/ogr_csv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/ogrcsvdriver_test";

class OgrCsvDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRCSV();
    poDriver_ = GetGDALDriverManager()->GetDriverByName("CSV");
    ASSERT_THAT(poDriver_, NotNull());
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }

  GDALDriver* poDriver_ = nullptr;
};

TEST_F(OgrCsvDriverTest, RegistrationAndMetadata) {
  // Calling RegisterOGRCSV when already registered is a no-op.
  RegisterOGRCSV();
  EXPECT_EQ(poDriver_, GetGDALDriverManager()->GetDriverByName("CSV"));

  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_CREATE_LAYER));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_DELETE_LAYER));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_CREATE_FIELD));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_DELETE_FIELD));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_REORDER_FIELDS));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_CURVE_GEOMETRIES));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_Z_GEOMETRIES));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_UPDATE));
  EXPECT_STREQ("csv tsv psv", poDriver_->GetMetadataItem(GDAL_DMD_EXTENSIONS));
  EXPECT_STREQ("drivers/vector/csv.html",
               poDriver_->GetMetadataItem(GDAL_DMD_HELPTOPIC));

  EXPECT_THAT(poDriver_->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST),
              HasSubstr("CreationOptionList"));
  EXPECT_THAT(poDriver_->GetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST),
              HasSubstr("LayerCreationOptionList"));
  EXPECT_THAT(poDriver_->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST),
              HasSubstr("OpenOptionList"));
}

TEST_F(OgrCsvDriverTest, IdentifyFileWithSingleAllowedDriver) {
  // A non-csv extension is accepted if CSV is the single allowed driver.
  const std::string filename = std::string(kTestDir) + "/test.dat";
  VsiMemTempWrapper file(filename, "col1,col2\n1,2\n");

  char** papszDrivers = CSLAddString(nullptr, "CSV");
  absl::Cleanup cleanup_drivers = [papszDrivers] { CSLDestroy(papszDrivers); };

  GDALOpenInfo open_info(filename.c_str(), GDAL_OF_VECTOR);
  open_info.papszAllowedDrivers = papszDrivers;
  EXPECT_EQ(TRUE, poDriver_->pfnIdentify(&open_info));
}

TEST_F(OgrCsvDriverTest, IdentifySpecialFilenames) {
  struct TestCase {
    std::string_view name;
    int expected_identify;
  };
  constexpr TestCase kCases[] = {
      // FAA NFDC files
      {.name = "NfdcFacilities.xls", .expected_identify = TRUE},
      {.name = "NfdcRunways.xls", .expected_identify = TRUE},
      {.name = "NfdcRemarks.xls", .expected_identify = TRUE},
      {.name = "NfdcSchedules.xls", .expected_identify = TRUE},
      // GNIS prefixes with .txt or .zip
      {.name = "NationalFile_test.txt", .expected_identify = TRUE},
      {.name = "POP_PLACES_test.txt", .expected_identify = TRUE},
      {.name = "HIST_FEATURES_test.txt", .expected_identify = TRUE},
      {.name = "US_CONCISE_test.txt", .expected_identify = TRUE},
      {.name = "AllNames_test.txt", .expected_identify = TRUE},
      {.name = "Feature_Description_History_test.txt", .expected_identify = TRUE},
      {.name = "ANTARCTICA_test.txt", .expected_identify = TRUE},
      {.name = "GOVT_UNITS_test.txt", .expected_identify = TRUE},
      {.name = "NationalFedCodes_test.txt", .expected_identify = TRUE},
      {.name = "AllStates_test.txt", .expected_identify = TRUE},
      {.name = "AllStatesFedCodes_test.txt", .expected_identify = TRUE},
      {.name = "CA_Features_test.txt", .expected_identify = TRUE},
      {.name = "CA_FedCodes_test.zip", .expected_identify = TRUE},
      // Geonames files
      {.name = "allCountries.txt", .expected_identify = TRUE},
      {.name = "allCountries.zip", .expected_identify = TRUE},
      // Standard extensions
      {.name = "data.csv", .expected_identify = TRUE},
      {.name = "data.tsv", .expected_identify = TRUE},
      {.name = "data.psv", .expected_identify = TRUE},
      // Unrecognized extension
      {.name = "data.bin", .expected_identify = FALSE},
  };

  for (const auto& tc : kCases) {
    const std::string filename = std::string(kTestDir) + "/" + std::string(tc.name);
    VsiMemTempWrapper file(filename, "col1,col2\n1,2\n");
    GDALOpenInfo open_info(filename.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(tc.expected_identify, poDriver_->pfnIdentify(&open_info))
        << "For filename: " << tc.name;
  }
}

TEST_F(OgrCsvDriverTest, IdentifyVsizip) {
  // A .zip accessed via /vsizip/ returns -1 (Unsure).
  const std::string zip_path = std::string(kTestDir) + "/container.zip";
  const std::string inside_path = "/vsizip/" + zip_path + "/dummy.txt";
  VSILFILE* fp = VSIFOpenL(inside_path.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  EXPECT_EQ(4, VSIFWriteL("test", 1, 4, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  absl::Cleanup cleanup_zip = [&zip_path] { VSIUnlink(zip_path.c_str()); };

  const std::string vsizip_filename = "/vsizip/" + zip_path;
  GDALOpenInfo open_info(vsizip_filename.c_str(), GDAL_OF_VECTOR);
  EXPECT_EQ(-1, poDriver_->pfnIdentify(&open_info));
}

TEST_F(OgrCsvDriverTest, IdentifyWithoutOpenFile) {
  // 1. Prefix "CSV:" returns TRUE even when file pointer is null.
  {
    GDALOpenInfo open_info("CSV:/vsimem/dummy.dat", GDAL_OF_VECTOR);
    EXPECT_EQ(TRUE, poDriver_->pfnIdentify(&open_info));
  }

  // 2. Directory without single allowed driver returns -1 (Unsure).
  {
    const std::string dirname = std::string(kTestDir) + "/some_dir";
    ASSERT_EQ(0, VSIMkdir(dirname.c_str(), 0755));
    absl::Cleanup cleanup_dir = [&dirname] { VSIRmdir(dirname.c_str()); };

    GDALOpenInfo open_info(dirname.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(-1, poDriver_->pfnIdentify(&open_info));

    // With single allowed driver, directory returns TRUE.
    char** papszDrivers = CSLAddString(nullptr, "CSV");
    absl::Cleanup cleanup_drivers = [papszDrivers] {
      CSLDestroy(papszDrivers);
    };
    GDALOpenInfo open_info_single(dirname.c_str(), GDAL_OF_VECTOR);
    open_info_single.papszAllowedDrivers = papszDrivers;
    EXPECT_EQ(TRUE, poDriver_->pfnIdentify(&open_info_single));
  }

  // 3. Non-existent path returns FALSE.
  {
    GDALOpenInfo open_info("/vsimem/does_not_exist_file", GDAL_OF_VECTOR);
    EXPECT_EQ(FALSE, poDriver_->pfnIdentify(&open_info));
  }
}

TEST_F(OgrCsvDriverTest, OpenNonCsvAndOpenFailure) {
  // Unrecognized extension -> Identify FALSE -> Open nullptr
  const std::string bin_file = std::string(kTestDir) + "/file.bin";
  constexpr std::array<uint8_t, 4> kBinData = {0x00, 0x01, 0x02, 0x03};
  VsiMemTempWrapper file(
      bin_file, std::string(reinterpret_cast<const char*>(kBinData.data()),
                            kBinData.size()));

  GDALOpenInfo open_info(bin_file.c_str(), GDAL_OF_VECTOR);
  EXPECT_EQ(nullptr, poDriver_->pfnOpen(&open_info));

  // "CSV:" with non-existent target returns nullptr cleanly.
  CPLErrorReset();
  GDALOpenInfo open_info_fail("CSV:/vsimem/missing.csv", GDAL_OF_VECTOR);
  EXPECT_EQ(nullptr, poDriver_->pfnOpen(&open_info_fail));
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
}

TEST_F(OgrCsvDriverTest, OpenInUpdateModeAndMapManagement) {
  // Test update mode map caching, flush on re-open, and RemoveFromMap.
  const std::string filename = std::string(kTestDir) + "/update.csv";
  VsiMemTempWrapper file(filename, "col1,col2\n1,2\n");

  GDALOpenInfo open_info1(filename.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR);
  std::unique_ptr<GDALDataset> ds1(poDriver_->pfnOpen(&open_info1));
  ASSERT_THAT(ds1, NotNull());

  // Re-opening the same dataset in read-only mode triggers the FlushCache path
  // through poMap in OGRCSVDriverOpen.
  GDALOpenInfo open_info2(filename.c_str(), GDAL_OF_READONLY | GDAL_OF_VECTOR);
  std::unique_ptr<GDALDataset> ds2(poDriver_->pfnOpen(&open_info2));
  ASSERT_THAT(ds2, NotNull());

  // Also exercise OGRCSVDriverRemoveFromMap directly with unmatched and matched pointers.
  OGRCSVDriverRemoveFromMap("non_existent_key", ds1.get());
  OGRCSVDriverRemoveFromMap(filename.c_str(), ds2.get());  // Unmatched DS: does not erase.
  OGRCSVDriverRemoveFromMap(filename.c_str(), ds1.get());  // Matched DS: erases.

  ds1.reset();
  ds2.reset();

  // Exercise OGRCSVDriverRemoveFromMap when map is empty.
  OGRCSVDriverRemoveFromMap(filename.c_str(), nullptr);
}

TEST_F(OgrCsvDriverTest, UnloadDriverCleansMapAndMutex) {
  const std::string filename = std::string(kTestDir) + "/unload_test.csv";
  VsiMemTempWrapper file(filename, "a,b\n1,2\n");

  GDALOpenInfo open_info(filename.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR);
  std::unique_ptr<GDALDataset> ds(poDriver_->pfnOpen(&open_info));
  ASSERT_THAT(ds, NotNull());

  // Call OGRCSVDriverUnload while map and mutex are populated.
  // Driver stays registered; next Open lazily recreates map/mutex.
  ASSERT_THAT(poDriver_->pfnUnloadDriver, NotNull());
  poDriver_->pfnUnloadDriver(poDriver_);
  ds.reset();

  poDriver_ = GetGDALDriverManager()->GetDriverByName("CSV");
  ASSERT_THAT(poDriver_, NotNull());

  // Exercise RemoveFromMap when poMap is nullptr.
  OGRCSVDriverRemoveFromMap("test", nullptr);
}

TEST_F(OgrCsvDriverTest, CreateFailsWhenAlreadyExists) {
  const std::string filename = std::string(kTestDir) + "/exists.csv";
  VsiMemTempWrapper file(filename, "a,b\n1,2\n");

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALDataset* ds = poDriver_->pfnCreate(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
}

TEST_F(OgrCsvDriverTest, CreateDevStdout) {
  // /dev/stdout is mapped to /vsistdout/
  std::unique_ptr<GDALDataset> ds(
      poDriver_->Create("/dev/stdout", 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_THAT(ds, NotNull());
}

TEST_F(OgrCsvDriverTest, CreateForSingleFile) {
  // 1. Single CSV with empty path defaults to "." without creating a file until
  // a layer is created; ensure any stray file in cwd is unlinked before/after.
  {
    VSIUnlink("single_rel.csv");
    absl::Cleanup cleanup_rel = [] { VSIUnlink("single_rel.csv"); };
    std::unique_ptr<GDALDataset> ds(
        poDriver_->Create("single_rel.csv", 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_THAT(ds, NotNull());
  }

  // 2. Single CSV directly under /vsimem (exercises the "/vsimem" path branch)
  {
    const std::string filename = "/vsimem/top_level_driver.csv";
    absl::Cleanup cleanup_file = [&filename] {
      VSIUnlink(filename.c_str());
    };
    CPLStringList options;
    options.AddString("GEOMETRY=AS_WKT");

    std::unique_ptr<GDALDataset> ds(
        poDriver_->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, options.List()));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* layer = ds->CreateLayer("layer", nullptr, wkbPoint, nullptr);
    ASSERT_THAT(layer, NotNull());
    ds.reset();
  }
}

TEST_F(OgrCsvDriverTest, CreateDirectoryDatasource) {
  // 1. Creating a directory datasource in /vsimem
  const std::string dirname = std::string(kTestDir) + "/new_csv_dir";
  std::unique_ptr<GDALDataset> ds(
      poDriver_->Create(dirname.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRLayer* layer = ds->CreateLayer("test_layer", nullptr, wkbNone, nullptr);
  ASSERT_THAT(layer, NotNull());
  ds.reset();

  // 2. Creating /vsizip/ directory datasource
  const std::string zipname = "/vsizip/" + std::string(kTestDir) + "/out.zip";
  std::unique_ptr<GDALDataset> zip_ds(
      poDriver_->Create(zipname.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_THAT(zip_ds, NotNull());

  // 3. Directory creation failure on read-only virtual filesystem (/vsigzip/)
  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> fail_ds(poDriver_->Create(
      "/vsigzip//vsimem/cannot_create_dir", 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_EQ(nullptr, fail_ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed to create directory"));
}

TEST_F(OgrCsvDriverTest, DeleteDataset) {
  const std::string filename = std::string(kTestDir) + "/to_delete.csv";
  VSILFILE* fp = VSIFOpenL(filename.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  EXPECT_EQ(5, VSIFWriteL("col1\n", 1, 5, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  EXPECT_EQ(CE_None, poDriver_->Delete(filename.c_str()));

  // Deleting non-existent file returns CE_Failure.
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, poDriver_->Delete("/vsimem/does_not_exist_del.csv"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

}  // namespace
}  // namespace autotest2
