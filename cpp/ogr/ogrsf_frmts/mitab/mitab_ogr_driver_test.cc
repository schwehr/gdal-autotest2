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

#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_ogr_driver.h"

#include <memory>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_priv.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogrsf_frmts.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"

namespace autotest2 {
namespace {

void EnsureDriverRegistered() {
  GDALDriverManager* poMgr = GetGDALDriverManager();
  GDALDriver* poExisting = poMgr->GetDriverByName("MapInfo File");
  if (poExisting != nullptr) {
    poMgr->DeregisterDriver(poExisting);
    delete poExisting;
  }
  // First call registers the driver and sets all metadata and function pointers
  RegisterOGRTAB();
  // Second call tests early exit
  RegisterOGRTAB();
}

TEST(MitabOgrDriverTest, RegistrationAndMetadata) {
  EnsureDriverRegistered();

  GDALDriverManager* poMgr = GetGDALDriverManager();
  GDALDriver* poDriver = poMgr->GetDriverByName("MapInfo File");
  ASSERT_NE(poDriver, nullptr);

  EXPECT_STREQ(poDriver->GetDescription(), "MapInfo File");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VECTOR), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_CREATE_LAYER), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_CREATE_FIELD), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_DELETE_FIELD), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_REORDER_FIELDS), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_EXTENSIONS), "tab mif mid");
  EXPECT_NE(poDriver->pfnOpen, nullptr);
  EXPECT_NE(poDriver->pfnIdentify, nullptr);
  EXPECT_NE(poDriver->pfnCreate, nullptr);
  EXPECT_NE(poDriver->pfnDelete, nullptr);
  EXPECT_NE(poDriver->pfnUnloadDriver, nullptr);
}

TEST(MitabOgrDriverTest, Identify) {
  EnsureDriverRegistered();

  GDALDriver* poDriver =
      GetGDALDriverManager()->GetDriverByName("MapInfo File");
  ASSERT_NE(poDriver, nullptr);

  constexpr const char kDir[] = "/vsimem/drv_test/identify";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/drv_test"); };
  VSIMkdir(kDir, 0755);

  // 1. Non-existent path -> FALSE
  {
    GDALOpenInfo oInfo("/vsimem/drv_test/identify/nonexistent.tab",
                       GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), FALSE);
  }

  // 2. Directory -> -1 (unsure)
  {
    GDALOpenInfo oInfo(kDir, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), -1);
  }

  // 3. Regular file not ending in .mif/.mid/.tab -> FALSE
  constexpr const char kTxtPath[] = "/vsimem/drv_test/identify/file.txt";
  VSIFCloseL(VSIFOpenL(kTxtPath, "wb"));
  {
    GDALOpenInfo oInfo(kTxtPath, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), FALSE);
  }

  // 4. .mif and .mid files -> TRUE
  constexpr const char kMifPath[] = "/vsimem/drv_test/identify/file.mif";
  constexpr const char kMidPath[] = "/vsimem/drv_test/identify/file.mid";
  VSIFCloseL(VSIFOpenL(kMifPath, "wb"));
  VSIFCloseL(VSIFOpenL(kMidPath, "wb"));
  {
    GDALOpenInfo oInfoMif(kMifPath, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfoMif), TRUE);
    GDALOpenInfo oInfoMid(kMidPath, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfoMid), TRUE);
  }

  // 5. .tab file with "Fields" -> TRUE
  constexpr const char kTabFields[] = "/vsimem/drv_test/identify/fields.tab";
  const std::string sFields = "!table\nDefinition Table\n  Fields 1\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kTabFields, reinterpret_cast<GByte*>(const_cast<char*>(sFields.data())),
      sFields.size(), FALSE));
  {
    GDALOpenInfo oInfo(kTabFields, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), TRUE);
  }

  // 6. .tab file with "create view" -> TRUE
  constexpr const char kTabView[] = "/vsimem/drv_test/identify/view.tab";
  const std::string sView = "!table\ncreate view myview\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kTabView, reinterpret_cast<GByte*>(const_cast<char*>(sView.data())),
      sView.size(), FALSE));
  {
    GDALOpenInfo oInfo(kTabView, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), TRUE);
  }

  // 7. .tab file with "\IsSeamless" = "TRUE" -> TRUE
  constexpr const char kTabSeamless[] =
      "/vsimem/drv_test/identify/seamless.tab";
  const std::string sSeamless = "!table\n\"\\IsSeamless\" = \"TRUE\"\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kTabSeamless,
      reinterpret_cast<GByte*>(const_cast<char*>(sSeamless.data())),
      sSeamless.size(), FALSE));
  {
    GDALOpenInfo oInfo(kTabSeamless, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), TRUE);
  }

  // 8. .tab file without expected keywords -> FALSE
  constexpr const char kTabOther[] = "/vsimem/drv_test/identify/other.tab";
  const std::string sOther = "!table\nSome other content\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kTabOther, reinterpret_cast<GByte*>(const_cast<char*>(sOther.data())),
      sOther.size(), FALSE));
  {
    GDALOpenInfo oInfo(kTabOther, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), FALSE);
  }

#ifdef DEBUG
  // 9. .cur_input archive name -> -1
  constexpr const char kCurInput[] = "/vsimem/drv_test/identify/.cur_input";
  VSIFCloseL(VSIFOpenL(kCurInput, "wb"));
  {
    GDALOpenInfo oInfo(kCurInput, GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnIdentify(&oInfo), -1);
  }
#endif
}

TEST(MitabOgrDriverTest, Open) {
  EnsureDriverRegistered();

  GDALDriver* poDriver =
      GetGDALDriverManager()->GetDriverByName("MapInfo File");
  ASSERT_NE(poDriver, nullptr);

  constexpr const char kDir[] = "/vsimem/drv_test/open";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/drv_test"); };
  VSIMkdir(kDir, 0755);

  // 1. Unidentified file -> nullptr
  {
    GDALOpenInfo oInfo("/vsimem/drv_test/open/nonexistent.tab", GA_ReadOnly);
    EXPECT_EQ(poDriver->pfnOpen(&oInfo), nullptr);
  }

  // 2. .mif with GA_Update -> nullptr
  constexpr const char kMifPath[] = "/vsimem/drv_test/open/test.mif";
  VSIFCloseL(VSIFOpenL(kMifPath, "wb"));
  {
    GDALOpenInfo oInfo(kMifPath, GA_Update);
    EXPECT_EQ(poDriver->pfnOpen(&oInfo), nullptr);
  }

  // 3. Valid TAB file -> non-null dataset
  constexpr const char kTabPath[] = "/vsimem/drv_test/open/valid.tab";
  {
    TABFile tab(nullptr);
    ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);
    tab.AddFieldNative("col", TABFInteger, 10);
    tab.Close();
  }

  {
    GDALOpenInfo oInfo(kTabPath, GA_ReadOnly);
    GDALDataset* poDS = poDriver->pfnOpen(&oInfo);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    GDALClose(poDS);
  }

  // 4. Valid MIF dataset -> non-null dataset
  constexpr const char kValidMifPath[] = "/vsimem/drv_test/open/valid_mif.mif";
  constexpr const char kValidMidPath[] = "/vsimem/drv_test/open/valid_mif.mid";
  {
    constexpr const char kMifContent[] =
        "Version 300\n"
        "Charset \"Neutral\"\n"
        "Delimiter \"\\t\"\n"
        "Columns 1\n"
        "  col Integer\n"
        "Data\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        kValidMifPath, reinterpret_cast<GByte*>(const_cast<char*>(kMifContent)),
        sizeof(kMifContent) - 1, FALSE));
    VSIFCloseL(VSIFileFromMemBuffer(kValidMidPath, nullptr, 0, FALSE));
  }

  {
    GDALOpenInfo oInfo(kValidMifPath, GA_ReadOnly);
    GDALDataset* poDS = poDriver->pfnOpen(&oInfo);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    GDALClose(poDS);
  }

  // 5. Corrupt .tab with "Fields" keyword but missing .dat/.map -> Open fails
  // -> nullptr
  constexpr const char kCorruptTab[] = "/vsimem/drv_test/open/corrupt.tab";
  const std::string sCorrupt =
      "!table\nDefinition Table\n  Fields 1\n  x Integer ;\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kCorruptTab, reinterpret_cast<GByte*>(const_cast<char*>(sCorrupt.data())),
      sCorrupt.size(), FALSE));
  {
    GDALOpenInfo oInfo(kCorruptTab, GA_ReadOnly);
    WithQuietHandler quiet;
    EXPECT_EQ(poDriver->pfnOpen(&oInfo), nullptr);
  }
}

TEST(MitabOgrDriverTest, Create) {
  EnsureDriverRegistered();

  GDALDriver* poDriver =
      GetGDALDriverManager()->GetDriverByName("MapInfo File");
  ASSERT_NE(poDriver, nullptr);

  constexpr const char kDir[] = "/vsimem/drv_test/create";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/drv_test"); };
  VSIMkdir(kDir, 0755);

  // 1. Create single .tab dataset
  constexpr const char kTabPath[] = "/vsimem/drv_test/create/new.tab";
  GDALDataset* poDS1 =
      poDriver->pfnCreate(kTabPath, 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS1, nullptr);
  GDALClose(poDS1);

  // 2. Create directory dataset
  constexpr const char kSubDir[] = "/vsimem/drv_test/create/new_dir";
  GDALDataset* poDS2 =
      poDriver->pfnCreate(kSubDir, 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS2, nullptr);
  GDALClose(poDS2);

  // 3. Create failure when target directory is an existing file
  constexpr const char kExistingFile[] = "/vsimem/drv_test/create/reg_file";
  VSIFCloseL(VSIFOpenL(kExistingFile, "wb"));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(poDriver->pfnCreate(kExistingFile, 0, 0, 0, GDT_Unknown, nullptr),
              nullptr);
  }
}

TEST(MitabOgrDriverTest, Delete) {
  EnsureDriverRegistered();

  GDALDriver* poDriver =
      GetGDALDriverManager()->GetDriverByName("MapInfo File");
  ASSERT_NE(poDriver, nullptr);

  constexpr const char kDir[] = "/vsimem/drv_test/delete";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/drv_test"); };
  VSIMkdir(kDir, 0755);

  // 1. Delete non-existent dataset -> CE_Failure
  {
    WithQuietHandler quiet;
    EXPECT_EQ(poDriver->pfnDelete("/vsimem/drv_test/delete/nonexistent.tab"),
              CE_Failure);
  }

  // 2. Delete valid single .tab dataset
  constexpr const char kTabPath[] = "/vsimem/drv_test/delete/to_delete.tab";
  {
    TABFile tab(nullptr);
    ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);
    tab.AddFieldNative("val", TABFInteger, 10);
    tab.Close();
  }

  VSIStatBufL sStat;
  EXPECT_EQ(VSIStatL(kTabPath, &sStat), 0);
  EXPECT_EQ(poDriver->pfnDelete(kTabPath), CE_None);
  EXPECT_NE(VSIStatL(kTabPath, &sStat), 0);

  // 3. Delete valid directory dataset
  constexpr const char kSubDir[] = "/vsimem/drv_test/delete/dir_to_delete";
  VSIMkdir(kSubDir, 0755);
  constexpr const char kSubTab[] =
      "/vsimem/drv_test/delete/dir_to_delete/t.tab";
  {
    TABFile tab(nullptr);
    ASSERT_EQ(tab.Open(kSubTab, TABWrite), 0);
    tab.AddFieldNative("val", TABFInteger, 10);
    tab.Close();
  }

  EXPECT_EQ(VSIStatL(kSubDir, &sStat), 0);
  EXPECT_EQ(poDriver->pfnDelete(kSubDir), CE_None);
  EXPECT_NE(VSIStatL(kSubDir, &sStat), 0);
}

TEST(MitabOgrDriverTest, UnloadDriver) {
  EnsureDriverRegistered();

  GDALDriver* poDriver =
      GetGDALDriverManager()->GetDriverByName("MapInfo File");
  ASSERT_NE(poDriver, nullptr);

  // Call pfnUnloadDriver
  poDriver->pfnUnloadDriver(poDriver);
}

}  // namespace
}  // namespace autotest2
