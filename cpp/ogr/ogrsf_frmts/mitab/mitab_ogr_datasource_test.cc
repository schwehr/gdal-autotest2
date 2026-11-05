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
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_ogr_driver.h"

namespace autotest2 {
namespace {

TEST(OGRTABDataSourceTest, CreateDirectoryMode) {
  constexpr const char kDir[] = "/vsimem/ds_test/dir_mode";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/ds_test"); };

  OGRTABDataSource ds;
  EXPECT_TRUE(ds.Create(kDir, nullptr));

  // Creating when directory already exists
  OGRTABDataSource ds2;
  EXPECT_TRUE(ds2.Create(kDir, nullptr));

  // Creating when path is an existing regular file -> should fail
  constexpr const char kFilePath[] = "/vsimem/ds_test/reg_file";
  VSIFCloseL(VSIFOpenL(kFilePath, "wb"));
  OGRTABDataSource ds_fail;
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(ds_fail.Create(kFilePath, nullptr));
  }
}

TEST(OGRTABDataSourceTest, CreateSingleFilesAndOptions) {
  constexpr const char kBaseDir[] = "/vsimem/ds_test/single";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/ds_test"); };
  VSIMkdir(kBaseDir, 0755);

  // 1. Single TAB file with options: QUICK spatial index, BLOCKSIZE=1024,
  // ENCODING=UTF-8
  {
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "SPATIAL_INDEX_MODE", "QUICK");
    papszOptions = CSLSetNameValue(papszOptions, "BLOCKSIZE", "1024");
    papszOptions = CSLSetNameValue(papszOptions, "ENCODING", "UTF-8");
    papszOptions =
        CSLSetNameValue(papszOptions, "STRICT_FIELDS_NAME_LAUNDERING", "NO");

    OGRTABDataSource ds;
    constexpr const char kTabPath[] = "/vsimem/ds_test/single/test.tab";
    EXPECT_TRUE(ds.Create(kTabPath, papszOptions));
    CSLDestroy(papszOptions);

    EXPECT_EQ(ds.GetLayerCount(), 0);
    EXPECT_TRUE(ds.TestCapability(ODsCCreateLayer));
    EXPECT_TRUE(ds.TestCapability(ODsCRandomLayerWrite));
    EXPECT_FALSE(ds.TestCapability("UnknownCapability"));

    // First ICreateLayer on single file mode initializes the layer
    OGRLayer* poLayer = ds.CreateLayer("test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poLayer, nullptr);
    EXPECT_EQ(ds.GetLayerCount(), 1);
    EXPECT_EQ(ds.GetLayer(0), poLayer);
    EXPECT_EQ(ds.GetLayer(-1), nullptr);
    EXPECT_EQ(ds.GetLayer(1), nullptr);

    // Second ICreateLayer on single file mode must fail
    {
      WithQuietHandler quiet;
      EXPECT_EQ(ds.CreateLayer("second_layer", nullptr, wkbPoint, nullptr),
                nullptr);
    }
    EXPECT_FALSE(ds.TestCapability(ODsCCreateLayer));
  }

  // 2. Single MIF file by extension
  {
    OGRTABDataSource ds;
    constexpr const char kMifPath[] = "/vsimem/ds_test/single/test.mif";
    EXPECT_TRUE(ds.Create(kMifPath, nullptr));
    EXPECT_EQ(ds.GetLayerCount(), 0);
    OGRLayer* poLayer = ds.CreateLayer("test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poLayer, nullptr);
    OGRFieldDefn f("col", OFTInteger);
    EXPECT_EQ(poLayer->CreateField(&f), OGRERR_NONE);
  }

  // 3. Single file with FORMAT=MIF option, OPTIMIZED spatial index mode
  {
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "MIF");
    papszOptions =
        CSLSetNameValue(papszOptions, "SPATIAL_INDEX_MODE", "OPTIMIZED");

    OGRTABDataSource ds;
    constexpr const char kMifPath2[] = "/vsimem/ds_test/single/test_fmt.mif";
    EXPECT_TRUE(ds.Create(kMifPath2, papszOptions));
    CSLDestroy(papszOptions);
    OGRLayer* poLayer = ds.CreateLayer("test_fmt", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poLayer, nullptr);
    OGRFieldDefn f("col", OFTInteger);
    EXPECT_EQ(poLayer->CreateField(&f), OGRERR_NONE);
  }

  // 4. Single TAB file failing Open (invalid block size < 512 or > 32256)
  {
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "BLOCKSIZE", "99999");
    OGRTABDataSource ds;
    WithQuietHandler quiet;
    constexpr const char kBadBlockPath[] =
        "/vsimem/ds_test/single/bad_block.tab";
    EXPECT_FALSE(ds.Create(kBadBlockPath, papszOptions));
    CSLDestroy(papszOptions);
  }

  // 5. Single MIF file failing Open (invalid extension)
  {
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "MIF");
    OGRTABDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Create("/vsimem/ds_test/single/bad_mif.xyz", papszOptions));
    CSLDestroy(papszOptions);
  }
}

TEST(OGRTABDataSourceTest, ICreateLayerInDirectoryDataset) {
  constexpr const char kDir[] = "/vsimem/ds_test/create_layers";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/ds_test"); };

  // 1. Directory with TAB layers
  {
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "SPATIAL_INDEX_MODE", "QUICK");
    OGRTABDataSource ds;
    ASSERT_TRUE(ds.Create(kDir, papszOptions));
    CSLDestroy(papszOptions);

    // Layer with illegal characters for filename
    {
      WithQuietHandler quiet;
      EXPECT_EQ(ds.CreateLayer("bad/name", nullptr, wkbPoint, nullptr),
                nullptr);
    }

    // Layer with WGS84 Geographic SRS
    OGRSpatialReference srs_wgs84;
    srs_wgs84.SetWellKnownGeogCS("WGS84");
    char** papszLOptions = nullptr;
    papszLOptions = CSLSetNameValue(papszLOptions, "BOUNDS", "0,0,100,100");
    papszLOptions = CSLSetNameValue(papszLOptions, "DESCRIPTION", "Test Table");
    papszLOptions = CSLSetNameValue(papszLOptions, "ENCODING", "UTF-8");
    papszLOptions =
        CSLSetNameValue(papszLOptions, "STRICT_FIELDS_NAME_LAUNDERING", "YES");

    OGRLayer* poLayer1 =
        ds.CreateLayer("layer1", &srs_wgs84, wkbPoint, papszLOptions);
    ASSERT_NE(poLayer1, nullptr);
    EXPECT_EQ(ds.GetLayerCount(), 1);
    CSLDestroy(papszLOptions);

    // Layer with Projected SRS and invalid BOUNDS syntax
    OGRSpatialReference srs_proj;
    srs_proj.importFromEPSG(3857);
    char** papszBadBounds = nullptr;
    papszBadBounds =
        CSLSetNameValue(papszBadBounds, "BOUNDS", "invalid,bounds");
    {
      WithQuietHandler quiet;
      OGRLayer* poLayer2 =
          ds.CreateLayer("layer2", &srs_proj, wkbPolygon, papszBadBounds);
      ASSERT_NE(poLayer2, nullptr);
      EXPECT_EQ(ds.GetLayerCount(), 2);
    }
    CSLDestroy(papszBadBounds);

    // Layer with null SRS (default bounds branch)
    OGRLayer* poLayer3 =
        ds.CreateLayer("layer3", nullptr, wkbLineString, nullptr);
    ASSERT_NE(poLayer3, nullptr);
    EXPECT_EQ(ds.GetLayerCount(), 3);
  }

  // 2. Directory with MIF layers (FORMAT=MIF)
  constexpr const char kDirMif[] = "/vsimem/ds_test/create_mif_layers";
  {
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "MIF");
    papszOptions =
        CSLSetNameValue(papszOptions, "SPATIAL_INDEX_MODE", "OPTIMIZED");
    OGRTABDataSource ds;
    ASSERT_TRUE(ds.Create(kDirMif, papszOptions));
    CSLDestroy(papszOptions);

    OGRLayer* poMifLayer =
        ds.CreateLayer("miflayer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(poMifLayer, nullptr);
    OGRFieldDefn f("col", OFTInteger);
    EXPECT_EQ(poMifLayer->CreateField(&f), OGRERR_NONE);
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }
}

TEST(OGRTABDataSourceTest, OpenSingleFileAndDirectory) {
  constexpr const char kBaseDir[] = "/vsimem/ds_test/open_test";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/ds_test"); };
  VSIMkdir(kBaseDir, 0755);

  // Create a valid TAB dataset
  constexpr const char kTabPath[] = "/vsimem/ds_test/open_test/t1.tab";
  {
    TABFile tab(nullptr);
    ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);
    tab.AddFieldNative("val", TABFInteger, 10);
    tab.Close();
  }

  // Create a valid MIF dataset
  constexpr const char kMifPath[] = "/vsimem/ds_test/open_test/m1.mif";
  constexpr const char kMidPath[] = "/vsimem/ds_test/open_test/m1.mid";
  {
    constexpr const char kMifContent[] =
        "Version 300\n"
        "Charset \"Neutral\"\n"
        "Delimiter \"\\t\"\n"
        "Columns 1\n"
        "  val Integer\n"
        "Data\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        kMifPath, reinterpret_cast<GByte*>(const_cast<char*>(kMifContent)),
        sizeof(kMifContent) - 1, FALSE));
    VSIFCloseL(VSIFileFromMemBuffer(kMidPath, nullptr, 0, FALSE));
  }

  // Also add an ignored non-mapinfo file (.txt)
  constexpr const char kTxtPath[] = "/vsimem/ds_test/open_test/readme.txt";
  VSIFCloseL(VSIFOpenL(kTxtPath, "wb"));

  // 1. Open single file via GDALOpenInfo
  {
    GDALOpenInfo oInfo(kTabPath, GA_ReadOnly);
    OGRTABDataSource ds;
    EXPECT_TRUE(ds.Open(&oInfo, FALSE));
    EXPECT_EQ(ds.GetLayerCount(), 1);
    EXPECT_NE(ds.GetLayer(0), nullptr);

    // Read-only dataset cannot create layers
    WithQuietHandler quiet;
    EXPECT_EQ(ds.CreateLayer("new_l", nullptr, wkbPoint, nullptr), nullptr);
  }

  // 2. Open non-existent single file
  {
    GDALOpenInfo oInfo("/vsimem/ds_test/open_test/does_not_exist.tab",
                       GA_ReadOnly);
    OGRTABDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oInfo, TRUE));
  }

  // 3. Open directory containing both .tab and .mif
  {
    GDALOpenInfo oInfo(kBaseDir, GA_ReadOnly);
    OGRTABDataSource ds;
    EXPECT_TRUE(ds.Open(&oInfo, FALSE));
    EXPECT_EQ(ds.GetLayerCount(), 2);
  }

  // 4. Open empty directory
  constexpr const char kEmptyDir[] = "/vsimem/ds_test/empty_dir";
  VSIMkdir(kEmptyDir, 0755);
  {
    GDALOpenInfo oInfo(kEmptyDir, GA_ReadOnly);
    OGRTABDataSource ds1;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds1.Open(&oInfo, FALSE));

    OGRTABDataSource ds2;
    EXPECT_FALSE(ds2.Open(&oInfo, TRUE));
  }

  // 5. Open directory containing a corrupt .tab file
  constexpr const char kCorruptDir[] = "/vsimem/ds_test/corrupt_dir";
  VSIMkdir(kCorruptDir, 0755);
  constexpr const char kCorruptTab[] =
      "/vsimem/ds_test/corrupt_dir/corrupt.tab";
  constexpr const char kCorruptContent[] =
      "!table\n"
      "!version 300\n"
      "!charset Neutral\n"
      "Definition Table\n"
      "  Fields 1\n"
      "    x Integer ;\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kCorruptTab, reinterpret_cast<GByte*>(const_cast<char*>(kCorruptContent)),
      sizeof(kCorruptContent) - 1, FALSE));
  {
    GDALOpenInfo oInfo(kCorruptDir, GA_ReadOnly);
    OGRTABDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&oInfo, FALSE));
  }
}

TEST(OGRTABDataSourceTest, GetFileList) {
  constexpr const char kBaseDir[] = "/vsimem/ds_test/filelist";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/ds_test"); };
  VSIMkdir(kBaseDir, 0755);

  // 1. Single TAB dataset file list
  constexpr const char kTabPath[] = "/vsimem/ds_test/filelist/table1.tab";
  {
    TABFile tab(nullptr);
    ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);
    tab.AddFieldNative("val", TABFInteger, 10);
    tab.Close();
  }

  {
    GDALOpenInfo oInfo(kTabPath, GA_ReadOnly);
    OGRTABDataSource ds;
    ASSERT_TRUE(ds.Open(&oInfo, FALSE));
    char** papszList = ds.GetFileList();
    ASSERT_NE(papszList, nullptr);
    EXPECT_GE(CSLCount(papszList), 1);
    CSLDestroy(papszList);
  }

  // 2. Single MIF dataset file list
  constexpr const char kMifPath[] = "/vsimem/ds_test/filelist/table2.mif";
  constexpr const char kMidPath[] = "/vsimem/ds_test/filelist/table2.mid";
  {
    constexpr const char kMifContent[] =
        "Version 300\n"
        "Charset \"Neutral\"\n"
        "Delimiter \"\\t\"\n"
        "Columns 1\n"
        "  val Integer\n"
        "Data\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        kMifPath, reinterpret_cast<GByte*>(const_cast<char*>(kMifContent)),
        sizeof(kMifContent) - 1, FALSE));
    VSIFCloseL(VSIFileFromMemBuffer(kMidPath, nullptr, 0, FALSE));
  }

  {
    OGRTABDataSource ds;
    ds.SetDescription(kMifPath);
    char** papszList = ds.GetFileList();
    ASSERT_NE(papszList, nullptr);
    EXPECT_EQ(CSLCount(papszList), 2);
    CSLDestroy(papszList);
  }

  // 3. Directory dataset file list
  {
    GDALOpenInfo oInfo(kBaseDir, GA_ReadOnly);
    OGRTABDataSource ds;
    ASSERT_TRUE(ds.Open(&oInfo, FALSE));
    char** papszList = ds.GetFileList();
    ASSERT_NE(papszList, nullptr);
    EXPECT_GE(CSLCount(papszList), 3);
    CSLDestroy(papszList);
  }
}

TEST(OGRTABDataSourceTest, ExecuteSQL) {
  constexpr const char kBaseDir[] = "/vsimem/ds_test/exec_sql";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/ds_test"); };
  VSIMkdir(kBaseDir, 0755);

  constexpr const char kTabPath[] = "/vsimem/ds_test/exec_sql/t.tab";
  {
    TABFile tab(nullptr);
    ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);
    tab.AddFieldNative("id_field", TABFInteger, 10);
    tab.Close();
  }

  GDALOpenInfo oInfo(kTabPath, GA_Update);
  OGRTABDataSource ds;
  ASSERT_TRUE(ds.Open(&oInfo, FALSE));

  // 1. CREATE INDEX ON layer USING field - non-existent layer
  {
    WithQuietHandler quiet;
    EXPECT_EQ(ds.ExecuteSQL("CREATE INDEX ON nosuchlayer USING fld", nullptr,
                            nullptr),
              nullptr);
  }

  // 2. CREATE INDEX ON layer USING field - non-existent field
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        ds.ExecuteSQL("CREATE INDEX ON t USING nosuchfield", nullptr, nullptr),
        nullptr);
  }

  // 3. CREATE INDEX ON layer USING field - valid
  EXPECT_EQ(ds.ExecuteSQL("CREATE INDEX ON t USING id_field", nullptr, nullptr),
            nullptr);

  // 4. Normal SQL query delegating to GDALDataset::ExecuteSQL
  OGRLayer* poResult = ds.ExecuteSQL("SELECT * FROM t", nullptr, nullptr);
  ASSERT_NE(poResult, nullptr);
  ds.ReleaseResultSet(poResult);
}

}  // namespace
}  // namespace autotest2
