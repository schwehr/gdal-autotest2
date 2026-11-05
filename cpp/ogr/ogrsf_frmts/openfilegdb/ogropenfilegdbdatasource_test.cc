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
// Tests Open FileGDB datasource driver (ogropenfilegdbdatasource.cpp).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogr_openfilegdb.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern "C" void RegisterOGROpenFileGDB();

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr const char* kTestDir = "/vsimem/ogropenfilegdbdatasource_test";

inline CPLErr CloseDS(GDALDataset& ds) { return ds.Close(); }

inline CPLErr CloseDS(GDALDataset* ds) {
  return ds != nullptr ? ds->Close() : CE_None;
}

class OgrOpenFileGdbDataSourceTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();
    RegisterOGROpenFileGDB();
  }

  static void TearDownTestSuite() {
    GDALDestroyDriverManager();
    OSRCleanup();
  }

  void SetUp() override { VSIMkdirRecursive(kTestDir, 0755); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

// Helper to open a GDALDataset with OpenFileGDB
std::unique_ptr<OGROpenFileGDBDataSource> OpenGDB(
    const std::string& path, int nOpenFlags = GDAL_OF_VECTOR | GDAL_OF_READONLY,
    CSLConstList open_options = nullptr) {
  GDALOpenInfo oOpenInfo(path.c_str(), nOpenFlags);
  oOpenInfo.papszOpenOptions = const_cast<char**>(open_options);
  auto ds = std::make_unique<OGROpenFileGDBDataSource>();
  bool bRetry = false;
  if (!ds->Open(&oOpenInfo, bRetry)) return nullptr;
  return ds;
}

// -----------------------------------------------------------------------------
// Test 1: OpenValidationAndArchivesAndBackup
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbDataSourceTest, OpenValidationAndArchivesAndBackup) {
  // 1. GDAL_OF_RASTER | GDAL_OF_UPDATE rejected
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              OpenGDB("/vsimem/dummy.gdb", GDAL_OF_RASTER | GDAL_OF_UPDATE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Update mode of rasters is not supported"));
  }

  // 2. OpenFileGDB: connection strings
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenGDB("OpenFileGDB:bad"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid connection string"));

    // 4 tokens with 1-char drive letter
    EXPECT_EQ(nullptr, OpenGDB("OpenFileGDB:C:/nonexistent.gdb:ras"));

    // 3 tokens
    EXPECT_EQ(nullptr, OpenGDB("OpenFileGDB:/vsimem/nonexistent.gdb:ras"));
  }

  // 3. .tar archive (without /vsitar/)
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OpenGDB("/vsimem/nonexistent.tar"));
  }

  // 4. .zip archive with a single inner .gdb directory
  const std::string valid_gdb = std::string(kTestDir) + "/valid.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(valid_gdb.c_str()));
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRGeomFieldDefn geom_field("SHAPE", wkbPoint);
    geom_field.SetSpatialRef(&srs);
    auto* lyr = ds.ICreateLayer("test_lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn fld("name", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetField("name", "foo");
    OGRPoint pt(1.0, 2.0);
    feat.SetGeometry(&pt);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    ASSERT_EQ(CE_None, CloseDS(ds));
  }

  // Create single_gdb.zip containing "inner.gdb/..."
  const std::string single_zip = std::string(kTestDir) + "/single_gdb.zip";
  {
    void* h_zip = CPLCreateZip(single_zip.c_str(), nullptr);
    ASSERT_NE(nullptr, h_zip);

    char** papszFiles = VSIReadDir(valid_gdb.c_str());
    ASSERT_NE(nullptr, papszFiles);
    for (int i = 0; papszFiles[i] != nullptr; ++i) {
      if (strcmp(papszFiles[i], ".") == 0 || strcmp(papszFiles[i], "..") == 0)
        continue;
      std::string src_path = valid_gdb + "/" + papszFiles[i];
      vsi_l_offset nLen = 0;
      GByte* pBuf = VSIGetMemFileBuffer(src_path.c_str(), &nLen, false);
      if (pBuf) {
        std::string zip_entry = std::string("inner.gdb/") + papszFiles[i];
        EXPECT_EQ(CE_None,
                  CPLCreateFileInZip(h_zip, zip_entry.c_str(), nullptr));
        EXPECT_EQ(CE_None,
                  CPLWriteFileInZip(h_zip, pBuf, static_cast<int>(nLen)));
        EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
      }
    }
    CSLDestroy(papszFiles);
    EXPECT_EQ(CE_None, CPLCloseZip(h_zip));
  }

  // Open single_gdb.zip (without /vsizip/ prefix)
  {
    auto ds = OpenGDB(single_zip, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
    EXPECT_STREQ("test_lyr", ds->GetLayer(0)->GetName());
  }

  // Create multi_gdb.zip containing "a.gdb/a00000001.gdbtable" and
  // "b.gdb/a00000001.gdbtable"
  const std::string multi_zip = std::string(kTestDir) + "/multi_gdb.zip";
  {
    void* h_zip = CPLCreateZip(multi_zip.c_str(), nullptr);
    ASSERT_NE(nullptr, h_zip);
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip, "a.gdb/dummy.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip, "a", 1));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
    EXPECT_EQ(CE_None, CPLCreateFileInZip(h_zip, "b.gdb/dummy.txt", nullptr));
    EXPECT_EQ(CE_None, CPLWriteFileInZip(h_zip, "b", 1));
    EXPECT_EQ(CE_None, CPLCloseFileInZip(h_zip));
    EXPECT_EQ(CE_None, CPLCloseZip(h_zip));

    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OpenGDB(multi_zip, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  }

  // 5. .ogrtransaction_backup test
  const std::string backup_gdb = std::string(kTestDir) + "/backup_test.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(backup_gdb.c_str()));
    ASSERT_NE(nullptr, ds.ICreateLayer("lyr", nullptr, nullptr));
    CloseDS(ds);
    std::string backup_dir = backup_gdb + "/.ogrtransaction_backup";
    ASSERT_EQ(0, VSIMkdir(backup_dir.c_str(), 0755));

    // GA_Update fails with error
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, OpenGDB(backup_gdb, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("A previous backup directory"));
    }

    // GA_ReadOnly succeeds with warning
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds_ro = OpenGDB(backup_gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
      EXPECT_NE(nullptr, ds_ro);
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("A previous backup directory"));
    }
    VSIUnlink(backup_dir.c_str());
  }

  // 6. Close() while transaction is active
  {
    auto ds = OpenGDB(valid_gdb, GDAL_OF_VECTOR | GDAL_OF_UPDATE);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(OGRERR_NONE, ds->StartTransaction(1));
    EXPECT_TRUE(ds->IsInTransaction());
    EXPECT_EQ(CE_None, CloseDS(ds.get()));
  }

  // 7. GDB_SystemCatalog schema edge cases
  const std::string bad_cat_gdb = std::string(kTestDir) + "/bad_cat.gdb";
  VSIMkdir(bad_cat_gdb.c_str(), 0755);
  std::string cat_file = bad_cat_gdb + "/a00000001.gdbtable";
  {
    OpenFileGDB::FileGDBTable t;
    ASSERT_TRUE(
        t.Create(cat_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    // Missing FileFormat field
    t.Sync();
    t.Close();

    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OpenGDB(bad_cat_gdb));
  }

  // 8. Neither GDB_Items nor GDB_FeatureClasses in GDB_SystemCatalog
  {
    OpenFileGDB::FileGDBTable t;
    ASSERT_TRUE(
        t.Create(cat_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "FileFormat", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));

    // Row 1: deleted row
    std::vector<OGRField> r1(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
    r1[1].String = const_cast<char*>("dummy");
    r1[2].Integer = 0;
    t.CreateFeature(r1, nullptr);
    t.DeleteFeature(1);

    // Row 2: NULL Name
    std::vector<OGRField> r2(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
    OGR_RawField_SetNull(&r2[1]);
    r2[2].Integer = 0;
    t.CreateFeature(r2, nullptr);

    // Row 3: GDB_SpatialRefs
    std::vector<OGRField> r3(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
    r3[1].String = const_cast<char*>("GDB_SpatialRefs");
    r3[2].Integer = 0;
    t.CreateFeature(r3, nullptr);

    t.Sync();
    t.Close();

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenGDB(bad_cat_gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("No GDB_Items nor GDB_FeatureClasses table"));
  }
}

// -----------------------------------------------------------------------------
// Test 2: DirectTableOpenAndFileList
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbDataSourceTest, DirectTableOpenAndFileList) {
  // 1. Standalone table in a non-GDB directory
  const std::string standalone_dir = std::string(kTestDir) + "/standalone";
  VSIMkdir(standalone_dir.c_str(), 0755);
  std::string table_path = standalone_dir + "/a00000009.gdbtable";
  std::string tablx_path = standalone_dir + "/a00000009.gdbtablx";
  {
    OpenFileGDB::FileGDBTable t;
    ASSERT_TRUE(
        t.Create(table_path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "val", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    std::vector<OGRField> f(2, OpenFileGDB::FileGDBField::UNSET_FIELD);
    f[1].Integer = 42;
    ASSERT_TRUE(t.CreateFeature(f, nullptr));
    t.Sync();
    t.Close();

    auto ds = OpenGDB(table_path, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
    EXPECT_STREQ("a00000009", ds->GetLayer(0)->GetName());
  }

  // 2. Standalone invalid table (0 fields, 0 features, no .gdbtablx)
  std::string bad_table_path = standalone_dir + "/a0000000a.gdbtable";
  std::string bad_tablx_path = standalone_dir + "/a0000000a.gdbtablx";
  {
    OpenFileGDB::FileGDBTable t;
    ASSERT_TRUE(t.Create(bad_table_path.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                         false, false));
    t.Sync();
    t.Close();
    VSIUnlink(bad_tablx_path.c_str());

    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              OpenGDB(bad_table_path, GDAL_OF_VECTOR | GDAL_OF_READONLY));
  }

  // 3. Direct table open inside a valid .gdb
  const std::string gdb = std::string(kTestDir) + "/direct_gdb.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));
    CloseDS(ds);
  }

  // Open a00000003.gdbtable (GDB_SpatialRefs, in GDB_SystemCatalog but not a
  // vector layer)
  {
    std::string srs_tbl = gdb + "/a00000003.gdbtable";
    auto ds = OpenGDB(srs_tbl, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
    EXPECT_STREQ("GDB_SpatialRefs", ds->GetLayer(0)->GetName());

    // Test GetFileList()
    char** papszFileList = ds->GetFileList();
    EXPECT_NE(nullptr, papszFileList);
    CSLDestroy(papszFileList);
  }

  // Copy a00000003 to a000000ff (> aosTableNames.size())
  {
    std::string src_table = gdb + "/a00000003.gdbtable";
    std::string src_tablx = gdb + "/a00000003.gdbtablx";
    std::string dst_table = gdb + "/a000000ff.gdbtable";
    std::string dst_tablx = gdb + "/a000000ff.gdbtablx";

    vsi_l_offset nLen1 = 0, nLen2 = 0;
    GByte* b1 = VSIGetMemFileBuffer(src_table.c_str(), &nLen1, false);
    GByte* b2 = VSIGetMemFileBuffer(src_tablx.c_str(), &nLen2, false);
    VSIFCloseL(VSIFileFromMemBuffer(dst_table.c_str(), b1, nLen1, false));
    VSIFCloseL(VSIFileFromMemBuffer(dst_tablx.c_str(), b2, nLen2, false));

    auto ds = OpenGDB(dst_table, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetLayerCount());
    EXPECT_STREQ("a000000ff", ds->GetLayer(0)->GetName());
  }
}

// -----------------------------------------------------------------------------
// Test 3: OpenFileGDBv10GroupsDomainsRelationshipsRastersAndCdfSdc
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbDataSourceTest,
       OpenFileGDBv10GroupsDomainsRelationshipsRastersAndCdfSdc) {
  const std::string gdb = std::string(kTestDir) + "/v10_full.gdb";

  // Create v10 dataset
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRGeomFieldDefn geom_field("SHAPE", wkbPoint);
    geom_field.SetSpatialRef(&srs);

    // Root layer
    auto* root_lyr = ds.ICreateLayer("root_lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, root_lyr);
    OGRFieldDefn fld1("id", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, root_lyr->CreateField(&fld1));

    // Feature dataset layer
    const char* const fds_opts[] = {"FEATURE_DATASET=my_fds", nullptr};
    auto* fds_lyr = ds.ICreateLayer("fds_lyr", &geom_field, fds_opts);
    ASSERT_NE(nullptr, fds_lyr);
    OGRFieldDefn fld2("id", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, fds_lyr->CreateField(&fld2));

    // Add field domain
    std::string reason;
    OGRField sMin;
    sMin.Integer = 1;
    OGRField sMax;
    sMax.Integer = 100;
    auto domain = std::make_unique<OGRRangeFieldDomain>(
        "my_domain", "desc", OFTInteger, OFSTNone, sMin, true, sMax, true);
    ASSERT_TRUE(ds.AddFieldDomain(std::move(domain), reason));

    // Add relationship
    auto rel = std::make_unique<GDALRelationship>(
        "rel_root_fds", "root_lyr", "fds_lyr",
        GDALRelationshipCardinality::GRC_ONE_TO_MANY);
    rel->SetLeftTableFields({"id"});
    rel->SetRightTableFields({"id"});
    ASSERT_TRUE(ds.AddRelationship(std::move(rel), reason));

    // Register orphan layer in items (references missing feature dataset
    // \missing_fds)
    ds.RegisterASpatialTableInItems(
        OFGDBGenerateUUID(), "orphan_lyr", "\\missing_fds\\orphan_lyr",
        "<DETableInfo><Name>orphan_lyr</Name></DETableInfo>", "");
    ds.RegisterLayerInSystemCatalog("orphan_lyr");

    // Register raster item in items
    const char* pszRastDoc =
        "<metadata><spdoinfo><rastinfo><rowcount>4</rowcount><colcount>4</"
        "colcount>"
        "<rastband>1</rastband><rastbpp>8</rastbpp></rastinfo></spdoinfo></"
        "metadata>";
    ds.RegisterASpatialTableInItems(
        OFGDBGenerateUUID(), "ras", "\\ras",
        "<DERasterDataset><Name>ras</Name></DERasterDataset>", pszRastDoc);
    ds.RegisterLayerInSystemCatalog("ras");
    ds.RegisterLayerInSystemCatalog("fras_ras_ras");
    ds.RegisterLayerInSystemCatalog("fras_aux_ras");
    ds.RegisterLayerInSystemCatalog("fras_bnd_ras");
    ds.RegisterLayerInSystemCatalog("fras_blk_ras");

    CloseDS(ds);

    // Add deleted row to GDB_Items (a00000004.gdbtable)
    std::string items_path = gdb + "/a00000004.gdbtable";
    OpenFileGDB::FileGDBTable t_items;
    ASSERT_TRUE(t_items.Open(items_path.c_str(), true));
    std::vector<OGRField> f(t_items.GetFieldCount(),
                            OpenFileGDB::FileGDBField::UNSET_FIELD);
    f[1].String = const_cast<char*>("{00000000-0000-0000-0000-000000000000}");
    f[2].String = const_cast<char*>("{00000000-0000-0000-0000-000000000000}");
    f[3].String = const_cast<char*>("del");
    f[4].String = const_cast<char*>("del");
    f[5].String = const_cast<char*>("\\del");
    t_items.CreateFeature(f, nullptr);
    t_items.DeleteFeature(t_items.GetTotalRecordCount());
    t_items.Sync();
    t_items.Close();
  }

  // 1. Open in GDAL_OF_VECTOR mode
  {
    WithQuietHandler quiet;  // quiet warning about orphan layer
    auto ds = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);

    // Root group
    auto root_group = ds->GetRootGroup();
    ASSERT_NE(nullptr, root_group);
    auto group_names = root_group->GetGroupNames();
    EXPECT_THAT(group_names, ::testing::Contains("my_fds"));
    auto sub_group = root_group->OpenGroup("my_fds");
    ASSERT_NE(nullptr, sub_group);
    EXPECT_EQ("my_fds", sub_group->GetName());
    EXPECT_EQ(nullptr, root_group->OpenGroup("nonexistent"));

    auto sub_lyr_names = sub_group->GetVectorLayerNames();
    EXPECT_THAT(sub_lyr_names, ::testing::Contains("fds_lyr"));
    EXPECT_NE(nullptr, sub_group->OpenVectorLayer("fds_lyr"));
    EXPECT_EQ(nullptr, sub_group->OpenVectorLayer("nonexistent"));

    auto root_lyr_names = root_group->GetVectorLayerNames();
    EXPECT_THAT(root_lyr_names, ::testing::Contains("root_lyr"));
    EXPECT_NE(nullptr, root_group->OpenVectorLayer("root_lyr"));
    EXPECT_EQ(nullptr, root_group->OpenVectorLayer("nonexistent"));

    // Relationships & Field Domains
    auto rel_names = ds->GetRelationshipNames();
    EXPECT_THAT(rel_names, ::testing::Contains("rel_root_fds"));
    auto domain_names = ds->GetFieldDomainNames();
    EXPECT_THAT(domain_names, ::testing::Contains("my_domain"));

    // GetLayerByName: existing, hidden (GDB_SystemCatalog), and nonexistent
    EXPECT_NE(nullptr, ds->GetLayerByName("root_lyr"));
    EXPECT_NE(nullptr, ds->GetLayerByName("GDB_SystemCatalog"));
    EXPECT_NE(nullptr,
              ds->GetLayerByName(
                  "GDB_SystemCatalog"));  // cached in m_apoHiddenLayers
    EXPECT_EQ(nullptr, ds->GetLayerByName("nonexistent"));
  }

  // 2. Open with LIST_ALL_TABLES=YES
  {
    const char* const opts[] = {"LIST_ALL_TABLES=YES", nullptr};
    auto ds = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY, opts);
    ASSERT_NE(nullptr, ds);

    EXPECT_FALSE(ds->IsLayerPrivate(-1));
    EXPECT_FALSE(ds->IsLayerPrivate(999));
    bool found_private = false;
    bool found_public = false;
    for (int i = 0; i < ds->GetLayerCount(); ++i) {
      if (ds->IsLayerPrivate(i))
        found_private = true;
      else
        found_public = true;
    }
    EXPECT_TRUE(found_private);
    EXPECT_TRUE(found_public);
    EXPECT_EQ(nullptr, ds->GetLayer(-1));
    EXPECT_EQ(nullptr, ds->GetLayer(999));
    EXPECT_NE(nullptr, ds->GetLayer(0));
  }

  // 3. Open in GDAL_OF_RASTER mode (multi subdatasets)
  {
    auto ds = OpenGDB(gdb, GDAL_OF_RASTER | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    char** papszSubdatasets = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, papszSubdatasets);
    const char* desc = CSLFetchNameValue(papszSubdatasets, "SUBDATASET_1_DESC");
    ASSERT_NE(nullptr, desc);
    EXPECT_THAT(desc, HasSubstr("[4x4x1], 8 bits"));
  }

  // 4. Test .sdc and .cdf detection in AddLayer
  const std::string sdc_gdb = std::string(kTestDir) + "/sdc_test.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(sdc_gdb.c_str()));
    ASSERT_NE(nullptr, ds.ICreateLayer("lyr", nullptr, nullptr));
    CloseDS(ds);

    std::string sdc_file = sdc_gdb + "/a00000009.gdbtable.sdc";
    VSIFCloseL(VSIFileFromMemBuffer(
        sdc_file.c_str(),
        const_cast<GByte*>(reinterpret_cast<const GByte*>("1")), 1, false));

    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds_sdc = OpenGDB(sdc_gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Smart Data Compression (SDC) that is unhandled"));
    VSIUnlink(sdc_file.c_str());

    std::string cdf_file = sdc_gdb + "/a00000009.gdbtable.cdf";
    VSIFCloseL(VSIFileFromMemBuffer(
        cdf_file.c_str(),
        const_cast<GByte*>(reinterpret_cast<const GByte*>("1")), 1, false));
    CPLErrorReset();
    auto ds_cdf = OpenGDB(sdc_gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Compressed Data Format (CDF) that is unhandled"));
    VSIUnlink(cdf_file.c_str());
  }

  // 5. Wrong GDB_Items structure
  const std::string bad_items_gdb = std::string(kTestDir) + "/bad_items.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(bad_items_gdb.c_str()));
    CloseDS(ds);

    std::string items_path = bad_items_gdb + "/a00000004.gdbtable";
    OpenFileGDB::FileGDBTable t;
    ASSERT_TRUE(
        t.Create(items_path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "wrong", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    t.Sync();
    t.Close();

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenGDB(bad_items_gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong structure for GDB_Items table"));
  }

  // 6. Empty v10 .gdb opened in vector GA_ReadOnly or raster GA_ReadOnly ->
  // false
  const std::string empty_gdb = std::string(kTestDir) + "/empty.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(empty_gdb.c_str()));
    CloseDS(ds);

    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OpenGDB(empty_gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    EXPECT_EQ(nullptr, OpenGDB(empty_gdb, GDAL_OF_RASTER | GDAL_OF_READONLY));
  }
}

// -----------------------------------------------------------------------------
// Test 4: OpenFileGDBv9
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbDataSourceTest, OpenFileGDBv9) {
  const std::string gdb = std::string(kTestDir) + "/v9.gdb";
  VSIMkdir(gdb.c_str(), 0755);

  std::string cat_file = gdb + "/a00000001.gdbtable";
  std::string obj_file = gdb + "/a00000002.gdbtable";
  std::string fc_file = gdb + "/a00000003.gdbtable";

  // 1. Wrong structure for GDB_ObjectClasses
  {
    // Create Catalog pointing to GDB_ObjectClasses and GDB_FeatureClasses
    OpenFileGDB::FileGDBTable cat;
    ASSERT_TRUE(
        cat.Create(cat_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    cat.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    cat.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    cat.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "FileFormat", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    std::vector<OGRField> r(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
    r[1].String = const_cast<char*>("GDB_SystemCatalog");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);
    r[1].String = const_cast<char*>("GDB_ObjectClasses");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);
    r[1].String = const_cast<char*>("GDB_FeatureClasses");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);
    cat.Sync();
    cat.Close();

    OpenFileGDB::FileGDBTable obj;
    ASSERT_TRUE(
        obj.Create(obj_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "bad", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    obj.Sync();
    obj.Close();

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenGDB(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong structure for GDB_ObjectClasses table"));
  }

  // 2. Wrong structure for GDB_FeatureClasses
  {
    OpenFileGDB::FileGDBTable obj;
    ASSERT_TRUE(
        obj.Create(obj_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "CLSID", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    obj.Sync();
    obj.Close();

    OpenFileGDB::FileGDBTable fc;
    ASSERT_TRUE(
        fc.Create(fc_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    fc.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "bad", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    fc.Sync();
    fc.Close();

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenGDB(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong structure for GDB_FeatureClasses table"));
  }

  // 3. Build valid v9 dataset with:
  // - Non-spatial table "non_spatial"
  // - Spatial layers with POINT, MULTIPOINT, LINE, POLYGON, MULTIPATCH, NONE
  // - Raster layer "ras_v9"
  {
    OpenFileGDB::FileGDBTable cat;
    ASSERT_TRUE(
        cat.Create(cat_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    cat.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    cat.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    cat.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "FileFormat", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    std::vector<OGRField> r(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
    r[1].String = const_cast<char*>("GDB_SystemCatalog");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);
    r[1].String = const_cast<char*>("GDB_ObjectClasses");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);
    r[1].String = const_cast<char*>("GDB_FeatureClasses");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);
    r[1].String = const_cast<char*>("non_spatial");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 4
    r[1].String = const_cast<char*>("pt_lyr");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 5
    r[1].String = const_cast<char*>("mp_lyr");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 6
    r[1].String = const_cast<char*>("line_lyr");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 7
    r[1].String = const_cast<char*>("poly_lyr");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 8
    r[1].String = const_cast<char*>("patch_lyr");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 9
    r[1].String = const_cast<char*>("none_lyr");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 10
    r[1].String = const_cast<char*>("ras_v9");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 11
    r[1].String = const_cast<char*>("fras_ras_ras_v9");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 12
    r[1].String = const_cast<char*>("fras_aux_ras_v9");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 13
    r[1].String = const_cast<char*>("fras_bnd_ras_v9");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 14
    r[1].String = const_cast<char*>("fras_blk_ras_v9");
    r[2].Integer = 0;
    cat.CreateFeature(r, nullptr);  // 15
    cat.Sync();
    cat.Close();

    // Create GDB_ObjectClasses
    OpenFileGDB::FileGDBTable obj;
    ASSERT_TRUE(
        obj.Create(obj_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    obj.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "CLSID", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    std::vector<OGRField> ro(3, OpenFileGDB::FileGDBField::UNSET_FIELD);

    // Row 1: non_spatial
    ro[1].String = const_cast<char*>("non_spatial");
    ro[2].String = const_cast<char*>("{7A566981-C114-11D2-8A28-006097AFF44E}");
    obj.CreateFeature(ro, nullptr);

    // Spatial classes
    const char* names[] = {"pt_lyr",    "mp_lyr",   "line_lyr", "poly_lyr",
                           "patch_lyr", "none_lyr", "ras_v9"};
    for (const char* n : names) {
      ro[1].String = const_cast<char*>(n);
      ro[2].String =
          const_cast<char*>("{5235BE55-7F10-11D2-AAD8-00C04FA33A14}");
      obj.CreateFeature(ro, nullptr);
    }
    // Add and delete row to test deleted row handling in SelectRow
    ro[1].String = const_cast<char*>("del");
    ro[2].String = const_cast<char*>("{5235BE55-7F10-11D2-AAD8-00C04FA33A14}");
    obj.CreateFeature(ro, nullptr);
    obj.DeleteFeature(obj.GetTotalRecordCount());
    obj.Sync();
    obj.Close();

    // Create GDB_FeatureClasses
    OpenFileGDB::FileGDBTable fc;
    ASSERT_TRUE(
        fc.Create(fc_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
    fc.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    fc.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "ObjectClassID", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    fc.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "FeatureType", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    fc.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "GeometryType", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    std::vector<OGRField> rf(4, OpenFileGDB::FileGDBField::UNSET_FIELD);

    // ObjectClassIDs correspond to 1-based index in aosName from
    // GDB_ObjectClasses row 1 is non_spatial ("") row 2 is pt_lyr -> idx 2
    rf[1].Integer = 2;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_POINT;
    fc.CreateFeature(rf, nullptr);
    rf[1].Integer = 3;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_MULTIPOINT;
    fc.CreateFeature(rf, nullptr);
    rf[1].Integer = 4;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_LINE;
    fc.CreateFeature(rf, nullptr);
    rf[1].Integer = 5;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_POLYGON;
    fc.CreateFeature(rf, nullptr);
    rf[1].Integer = 6;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_MULTIPATCH;
    fc.CreateFeature(rf, nullptr);
    rf[1].Integer = 7;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_NONE;
    fc.CreateFeature(rf, nullptr);
    // row 8 is ras_v9 -> FeatureType = 14
    rf[1].Integer = 8;
    rf[2].Integer = 14;
    rf[3].Integer = OpenFileGDB::FGTGT_NONE;
    fc.CreateFeature(rf, nullptr);

    // Add and delete row in GDB_FeatureClasses
    rf[1].Integer = 99;
    rf[2].Integer = 1;
    rf[3].Integer = OpenFileGDB::FGTGT_POINT;
    fc.CreateFeature(rf, nullptr);
    fc.DeleteFeature(fc.GetTotalRecordCount());
    fc.Sync();
    fc.Close();

    // Create the actual table files for each layer (tables 4 through 10)
    for (int t_idx = 4; t_idx <= 10; ++t_idx) {
      std::string tbl_path =
          CPLSPrintf("%s/a%08x.gdbtable", gdb.c_str(), t_idx);
      OpenFileGDB::FileGDBTable t;
      ASSERT_TRUE(
          t.Create(tbl_path.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
      t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD));
      t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "f", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD));
      t.Sync();
      t.Close();
    }
  }

  // 4. Test opening in GA_Update -> fails
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Edition of existing FileGDB v9 datasets not supported"));
  }

  // 5. Test opening in GDAL_OF_VECTOR | GA_ReadOnly -> succeeds
  {
    auto ds = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    EXPECT_GE(ds->GetLayerCount(), 6);
    EXPECT_NE(nullptr, ds->GetLayerByName("non_spatial"));
    EXPECT_NE(nullptr, ds->GetLayerByName("pt_lyr"));
    EXPECT_NE(nullptr, ds->GetLayerByName("mp_lyr"));
    EXPECT_NE(nullptr, ds->GetLayerByName("line_lyr"));
    EXPECT_NE(nullptr, ds->GetLayerByName("poly_lyr"));
    EXPECT_NE(nullptr, ds->GetLayerByName("patch_lyr"));
  }

  // 6. Test opening in GDAL_OF_RASTER | GA_ReadOnly -> populates SUBDATASETS
  {
    auto ds = OpenGDB(gdb, GDAL_OF_RASTER | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);
    char** papszSubdatasets = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, papszSubdatasets);
    EXPECT_NE(nullptr,
              CSLFetchNameValue(papszSubdatasets, "SUBDATASET_1_NAME"));
  }

  // 7. Test v9 where all candidate layers have .cdf -> returns false
  {
    std::string cdf_path = gdb + "/a00000004.gdbtable.cdf";
    VSIFCloseL(VSIFileFromMemBuffer(
        cdf_path.c_str(),
        const_cast<GByte*>(reinterpret_cast<const GByte*>("1")), 1, false));
    // Test with only 1 layer by interest table
    std::string single_tbl = gdb + "/a00000004.gdbtable";
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OpenGDB(single_tbl, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    VSIUnlink(cdf_path.c_str());
  }
}

// -----------------------------------------------------------------------------
// Test 5: ExecuteSQLAndSimpleSQLLayer
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbDataSourceTest, ExecuteSQLAndSimpleSQLLayer) {
  const std::string gdb = std::string(kTestDir) + "/sql_test.gdb";

  // Create dataset with indexed and non-indexed columns
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRGeomFieldDefn geom_field("SHAPE", wkbPoint);
    geom_field.SetSpatialRef(&srs);

    // Layer 1: lyr
    auto* lyr = ds.ICreateLayer("lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn f_val("val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_val));
    OGRFieldDefn f_rval("fval", OFTReal);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_rval));
    OGRFieldDefn f_dt("dt", OFTDateTime);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_dt));
    OGRFieldDefn f_un("unindexed", OFTString);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&f_un));

    for (int i = 1; i <= 4; ++i) {
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("val", i * 10);
      feat.SetField("fval", i + 0.5);
      feat.SetField("dt", 2023, 5, 15, 12, 0, 0, 0);
      feat.SetField("unindexed", "test");
      OGRPoint pt(i, i);
      feat.SetGeometry(&pt);
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
    }

    // Layer 2: empty_lyr
    auto* empty_lyr = ds.ICreateLayer("empty_lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, empty_lyr);
    OGRFieldDefn f_eval("val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, empty_lyr->CreateField(&f_eval));

    // Layer 3: null_lyr
    auto* null_lyr = ds.ICreateLayer("null_lyr", &geom_field, nullptr);
    ASSERT_NE(nullptr, null_lyr);
    OGRFieldDefn f_nval("val", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, null_lyr->CreateField(&f_nval));
    OGRFeature feat1(null_lyr->GetLayerDefn());
    feat1.SetField("val", 1);
    EXPECT_EQ(OGRERR_NONE, null_lyr->CreateFeature(&feat1));
    OGRFeature feat2(null_lyr->GetLayerDefn());
    feat2.SetFieldNull(0);
    EXPECT_EQ(OGRERR_NONE, null_lyr->CreateFeature(&feat2));

    // Layer 4: del_lyr
    auto* del_lyr = ds.ICreateLayer("del_lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, del_lyr);

    // Test special SQL commands while in update mode
    // 1. GetLayerDefinition
    auto* rs = ds.ExecuteSQL("GetLayerDefinition lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    auto* f_res = rs->GetNextFeature();
    ASSERT_NE(nullptr, f_res);
    delete f_res;
    EXPECT_EQ(nullptr,
              rs->GetNextFeature());  // test second call to GetNextFeature
    ds.ReleaseResultSet(rs);

    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("GetLayerDefinition bad", nullptr, nullptr));
    }

    // 2. GetLayerMetadata
    rs = ds.ExecuteSQL("GetLayerMetadata lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("GetLayerMetadata bad", nullptr, nullptr));
    }

    // 3. GetLayerAttrIndexUse
    rs = ds.ExecuteSQL("GetLayerAttrIndexUse lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);
    EXPECT_EQ(nullptr,
              ds.ExecuteSQL("GetLayerAttrIndexUse bad", nullptr, nullptr));

    // 4. GetLayerSpatialIndexState
    rs = ds.ExecuteSQL("GetLayerSpatialIndexState lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);
    EXPECT_EQ(nullptr,
              ds.ExecuteSQL("GetLayerSpatialIndexState bad", nullptr, nullptr));

    // 5. GetLastSQLUsedOptimizedImplementation
    rs = ds.ExecuteSQL("GetLastSQLUsedOptimizedImplementation", nullptr,
                       nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);

    // 6. CREATE SPATIAL INDEX ON
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("CREATE SPATIAL INDEX ON lyr", nullptr, nullptr));
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("CREATE SPATIAL INDEX ON bad", nullptr, nullptr));
    }

    // 7. CREATE INDEX
    EXPECT_EQ(nullptr, ds.ExecuteSQL("CREATE INDEX idx_val ON lyr(val)",
                                     nullptr, nullptr));
    EXPECT_EQ(nullptr, ds.ExecuteSQL("CREATE INDEX idx_dt ON lyr(dt)", nullptr,
                                     nullptr));
    EXPECT_EQ(nullptr, ds.ExecuteSQL("CREATE INDEX idx_e ON empty_lyr(val)",
                                     nullptr, nullptr));
    EXPECT_EQ(nullptr, ds.ExecuteSQL("CREATE INDEX idx_n ON null_lyr(val)",
                                     nullptr, nullptr));
    {
      WithQuietHandler quiet;
      // Multi-column not supported
      EXPECT_EQ(nullptr, ds.ExecuteSQL("CREATE INDEX idx_m ON lyr(val,fval)",
                                       nullptr, nullptr));
      // Bad layer
      EXPECT_EQ(nullptr, ds.ExecuteSQL("CREATE INDEX idx_b ON bad(val)",
                                       nullptr, nullptr));
      // Bad syntax
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("CREATE INDEX bad_syntax", nullptr, nullptr));
    }

    // 8. RECOMPUTE EXTENT ON
    EXPECT_EQ(nullptr,
              ds.ExecuteSQL("RECOMPUTE EXTENT ON lyr", nullptr, nullptr));
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("RECOMPUTE EXTENT ON bad", nullptr, nullptr));
    }

    // 9. CHECK_FREELIST_CONSISTENCY
    rs = ds.ExecuteSQL("CHECK_FREELIST_CONSISTENCY:lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr, ds.ExecuteSQL("CHECK_FREELIST_CONSISTENCY:bad",
                                       nullptr, nullptr));
    }

    // 10. REPACK
    rs = ds.ExecuteSQL("REPACK", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);
    rs = ds.ExecuteSQL("REPACK lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    ds.ReleaseResultSet(rs);
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr, ds.ExecuteSQL("REPACK bad", nullptr, nullptr));
    }

    // 11. DELLAYER
    EXPECT_EQ(nullptr, ds.ExecuteSQL("DELLAYER:del_lyr", nullptr, nullptr));
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr, ds.ExecuteSQL("DELLAYER:bad", nullptr, nullptr));
    }

    CloseDS(ds);
  }

  // Re-open in read-only mode to load indices and test SQL optimizations
  {
    auto ds = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds);

    // 1. MIN/MAX/SUM/AVG/COUNT optimization
    auto* rs = ds->ExecuteSQL(
        "SELECT MIN(val), MAX(val), SUM(val), AVG(val), COUNT(val), AVG(dt) "
        "FROM lyr",
        nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    auto* rs_opt = ds->ExecuteSQL("GetLastSQLUsedOptimizedImplementation",
                                  nullptr, nullptr);
    ASSERT_NE(nullptr, rs_opt);
    auto* f_opt = rs_opt->GetNextFeature();
    ASSERT_NE(nullptr, f_opt);
    EXPECT_STREQ("1", f_opt->GetFieldAsString(0));
    delete f_opt;
    ds->ReleaseResultSet(rs_opt);

    auto* feat = rs->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(10, feat->GetFieldAsInteger("MIN_val"));
    EXPECT_EQ(40, feat->GetFieldAsInteger("MAX_val"));
    EXPECT_DOUBLE_EQ(100.0, feat->GetFieldAsDouble("SUM_val"));
    EXPECT_DOUBLE_EQ(25.0, feat->GetFieldAsDouble("AVG_val"));
    EXPECT_EQ(4, feat->GetFieldAsInteger("COUNT_val"));
    delete feat;
    ds->ReleaseResultSet(rs);

    // AVG on empty layer (nCount == 0)
    rs = ds->ExecuteSQL("SELECT AVG(val) FROM empty_lyr", nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    feat = rs->GetNextFeature();
    ASSERT_NE(nullptr, feat);
    EXPECT_FALSE(feat->IsFieldSetAndNotNull(0));
    delete feat;
    ds->ReleaseResultSet(rs);

    // Fallback cases for MIN/MAX:
    // SUM on datetime is unsupported -> fallback
    rs = ds->ExecuteSQL("SELECT MIN(val), SUM(dt) FROM lyr", nullptr, nullptr);
    if (rs) ds->ReleaseResultSet(rs);
    // COUNT(DISTINCT) -> fallback
    rs =
        ds->ExecuteSQL("SELECT COUNT(DISTINCT val) FROM lyr", nullptr, nullptr);
    if (rs) ds->ReleaseResultSet(rs);
    // unindexed column -> fallback
    rs = ds->ExecuteSQL("SELECT MIN(unindexed) FROM lyr", nullptr, nullptr);
    if (rs) ds->ReleaseResultSet(rs);
    // Preparse syntax error
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr, ds->ExecuteSQL("SELECT FROM WHERE", nullptr, nullptr));
    }

    // 2. ORDER BY optimization & OGROpenFileGDBSimpleSQLLayer
    rs = ds->ExecuteSQL("SELECT * FROM lyr ORDER BY val ASC LIMIT 2 OFFSET 1",
                        nullptr, nullptr);
    ASSERT_NE(nullptr, rs);

    // Verify SimpleSQLLayer methods
    EXPECT_NE(nullptr, rs->GetLayerDefn());
    EXPECT_STREQ("OBJECTID", rs->GetFIDColumn());
    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, rs->GetExtent(&env, false));
    OGREnvelope3D env3d;
    EXPECT_EQ(OGRERR_NONE, rs->GetExtent3D(0, &env3d, false));

    EXPECT_TRUE(rs->TestCapability(OLCFastFeatureCount));
    EXPECT_TRUE(rs->TestCapability(OLCFastGetExtent));
    EXPECT_TRUE(rs->TestCapability(OLCRandomRead));
    EXPECT_TRUE(rs->TestCapability(OLCStringsAsUTF8));
    EXPECT_FALSE(rs->TestCapability("other"));

    EXPECT_EQ(2, rs->GetFeatureCount(true));

    // GetFeature random read
    auto* f_rnd = rs->GetFeature(1);
    ASSERT_NE(nullptr, f_rnd);
    delete f_rnd;
    EXPECT_EQ(nullptr, rs->GetFeature(999));

    // GetNextFeature iteration
    auto* f1 = rs->GetNextFeature();
    ASSERT_NE(nullptr, f1);
    EXPECT_EQ(20, f1->GetFieldAsInteger("val"));
    delete f1;
    auto* f2 = rs->GetNextFeature();
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(30, f2->GetFieldAsInteger("val"));
    delete f2;
    EXPECT_EQ(nullptr, rs->GetNextFeature());

    rs->ResetReading();
    f1 = rs->GetNextFeature();
    ASSERT_NE(nullptr, f1);
    delete f1;

    // Set filters on SimpleSQLLayer
    rs->SetSpatialFilterRect(0, 0, 2.5, 2.5);
    rs->SetAttributeFilter("val >= 20");
    EXPECT_FALSE(rs->TestCapability(OLCFastFeatureCount));
    EXPECT_GE(rs->GetFeatureCount(true), 0);
    rs->ResetReading();
    while ((f1 = rs->GetNextFeature()) != nullptr) {
      delete f1;
    }

    ds->ReleaseResultSet(rs);

    // Large offset (m_nOffset > nRowCount)
    rs = ds->ExecuteSQL("SELECT * FROM lyr ORDER BY val ASC OFFSET 100",
                        nullptr, nullptr);
    ASSERT_NE(nullptr, rs);
    EXPECT_EQ(0, rs->GetFeatureCount(true));
    ds->ReleaseResultSet(rs);

    // Multi-column projection and WHERE with >= operator
    rs = ds->ExecuteSQL(
        "SELECT val, * FROM lyr WHERE val >= 20 ORDER BY val DESC", nullptr,
        nullptr);
    ASSERT_NE(nullptr, rs);
    f_rnd = rs->GetFeature(4);
    if (f_rnd) delete f_rnd;
    while ((f1 = rs->GetNextFeature()) != nullptr) {
      delete f1;
    }
    ds->ReleaseResultSet(rs);

    // Fallbacks for ORDER BY:
    // WHERE with !=
    rs = ds->ExecuteSQL("SELECT val FROM lyr WHERE val != 20 ORDER BY val ASC",
                        nullptr, nullptr);
    if (rs) ds->ReleaseResultSet(rs);
    // DISTINCT
    rs = ds->ExecuteSQL("SELECT DISTINCT val FROM lyr ORDER BY val ASC",
                        nullptr, nullptr);
    if (rs) ds->ReleaseResultSet(rs);
    // Table with NULL values
    rs = ds->ExecuteSQL("SELECT * FROM null_lyr ORDER BY val ASC", nullptr,
                        nullptr);
    if (rs) ds->ReleaseResultSet(rs);
  }
}

// -----------------------------------------------------------------------------
// Test 6: CapabilitiesAndBuildSRS
// -----------------------------------------------------------------------------
TEST_F(OgrOpenFileGdbDataSourceTest, CapabilitiesAndBuildSRS) {
  const std::string gdb = std::string(kTestDir) + "/cap_test.gdb";
  {
    OGROpenFileGDBDataSource ds;
    ASSERT_TRUE(ds.Create(gdb.c_str()));
    ASSERT_NE(nullptr, ds.ICreateLayer("lyr", nullptr, nullptr));
    CloseDS(ds);
  }

  // 1. TestCapability
  {
    auto ds_ro = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_READONLY);
    ASSERT_NE(nullptr, ds_ro);
    EXPECT_FALSE(ds_ro->TestCapability(ODsCCreateLayer));
    EXPECT_FALSE(ds_ro->TestCapability(ODsCDeleteLayer));
    EXPECT_FALSE(ds_ro->TestCapability(ODsCAddFieldDomain));
    EXPECT_FALSE(ds_ro->TestCapability(ODsCDeleteFieldDomain));
    EXPECT_FALSE(ds_ro->TestCapability(ODsCUpdateFieldDomain));
    EXPECT_FALSE(ds_ro->TestCapability(GDsCAddRelationship));
    EXPECT_FALSE(ds_ro->TestCapability(GDsCDeleteRelationship));
    EXPECT_FALSE(ds_ro->TestCapability(GDsCUpdateRelationship));
    EXPECT_FALSE(ds_ro->TestCapability(ODsCEmulatedTransactions));
    EXPECT_TRUE(ds_ro->TestCapability(ODsCMeasuredGeometries));
    EXPECT_TRUE(ds_ro->TestCapability(ODsCZGeometries));
    EXPECT_TRUE(ds_ro->TestCapability(ODsCCurveGeometries));
    EXPECT_FALSE(ds_ro->TestCapability("other"));

    auto ds_rw = OpenGDB(gdb, GDAL_OF_VECTOR | GDAL_OF_UPDATE);
    ASSERT_NE(nullptr, ds_rw);
    EXPECT_TRUE(ds_rw->TestCapability(ODsCCreateLayer));
    EXPECT_TRUE(ds_rw->TestCapability(ODsCDeleteLayer));
    EXPECT_TRUE(ds_rw->TestCapability(ODsCAddFieldDomain));
    EXPECT_TRUE(ds_rw->TestCapability(ODsCEmulatedTransactions));
  }

  // 2. BuildSRS(const char* pszWKT)
  {
    OGROpenFileGDBDataSource ds;
    // Invalid WKT
    EXPECT_EQ(nullptr, ds.BuildSRS("INVALID_WKT"));

    // Valid WKT
    OGRSpatialReference srs_expected;
    srs_expected.importFromEPSG(4326);
    char* pszWKT = nullptr;
    srs_expected.exportToWkt(&pszWKT);
    ASSERT_NE(nullptr, pszWKT);

    auto* srs1 = ds.BuildSRS(pszWKT);
    ASSERT_NE(nullptr, srs1);
    srs1->Release();

    // Cache hit
    auto* srs2 = ds.BuildSRS(pszWKT);
    ASSERT_NE(nullptr, srs2);
    srs2->Release();

    // USE_OSR_FIND_MATCHES = NO
    CPLSetThreadLocalConfigOption("USE_OSR_FIND_MATCHES", "NO");
    auto* srs3 = ds.BuildSRS(pszWKT);
    CPLSetThreadLocalConfigOption("USE_OSR_FIND_MATCHES", nullptr);
    ASSERT_NE(nullptr, srs3);
    srs3->Release();

    CPLFree(pszWKT);
  }

  // 3. BuildSRS(const CPLXMLNode* psInfo)
  {
    OGROpenFileGDBDataSource ds;

    // LatestWKID = 4326 (<= 32767)
    {
      CPLXMLTreeCloser xml(
          CPLParseXMLString("<root><SpatialReference><LatestWKID>4326</"
                            "LatestWKID></SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      auto* srs = ds.BuildSRS(xml.get());
      ASSERT_NE(nullptr, srs);
      srs->Release();
    }

    // LatestWKID = 102100 (> 32767)
    {
      CPLXMLTreeCloser xml(
          CPLParseXMLString("<root><SpatialReference><LatestWKID>102100</"
                            "LatestWKID></SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      auto* srs = ds.BuildSRS(xml.get());
      ASSERT_NE(nullptr, srs);
      srs->Release();
    }

    // LatestWKID = 999999 (invalid) + WKID = 4326
    {
      CPLXMLTreeCloser xml(CPLParseXMLString(
          "<root><SpatialReference><LatestWKID>999999</LatestWKID><WKID>4326</"
          "WKID></SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      auto* srs = ds.BuildSRS(xml.get());
      ASSERT_NE(nullptr, srs);
      srs->Release();
    }

    // LatestWKID = 999999 + WKID = 102100 (> 32767)
    {
      CPLXMLTreeCloser xml(CPLParseXMLString(
          "<root><SpatialReference><LatestWKID>999999</"
          "LatestWKID><WKID>102100</WKID></SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      auto* srs = ds.BuildSRS(xml.get());
      ASSERT_NE(nullptr, srs);
      srs->Release();
    }

    // Both invalid and no WKT -> nullptr
    {
      CPLXMLTreeCloser xml(CPLParseXMLString(
          "<root><SpatialReference><LatestWKID>999999</"
          "LatestWKID><WKID>999998</WKID></SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      EXPECT_EQ(nullptr, ds.BuildSRS(xml.get()));
    }

    // Compound CS: WKID = 4326 + VCSWKID = 5773
    {
      CPLXMLTreeCloser xml(CPLParseXMLString(
          "<root><SpatialReference><WKID>4326</WKID><VCSWKID>5773</VCSWKID></"
          "SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      auto* srs = ds.BuildSRS(xml.get());
      ASSERT_NE(nullptr, srs);
      EXPECT_TRUE(srs->IsCompound());
      srs->Release();
    }

    // Failed vertical CS without WKT -> nullptr
    {
      CPLXMLTreeCloser xml(CPLParseXMLString(
          "<root><SpatialReference><WKID>4326</WKID><VCSWKID>999999</VCSWKID></"
          "SpatialReference></root>"));
      ASSERT_NE(nullptr, xml);
      EXPECT_EQ(nullptr, ds.BuildSRS(xml.get()));
    }

    // Failed vertical CS with WKT containing VERTCS -> falls back to
    // BuildSRS(pszWKT)
    {
      const char* pszXML =
          "<root><SpatialReference><WKID>4326</WKID><VCSWKID>999999</VCSWKID>"
          "<WKT>COMPD_CS[\"WGS 84 + EGM96 geoid height\",GEOGCS[\"WGS "
          "84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
          "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT["
          "\"degree\",0.0174532925199433]],VERT_CS[\"EGM96 geoid "
          "height\",VERT_DATUM[\"EGM96 geoid\",2005],UNIT[\"metre\",1]]]</WKT>"
          "</SpatialReference></root>";
      CPLXMLTreeCloser xml(CPLParseXMLString(pszXML));
      ASSERT_NE(nullptr, xml);
      auto* srs = ds.BuildSRS(xml.get());
      ASSERT_NE(nullptr, srs);
      srs->Release();
    }
  }
}

}  // namespace
}  // namespace autotest2
