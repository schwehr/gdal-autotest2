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

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/ogrshape.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::NotNull;

using DatasetCloser = std::unique_ptr<GDALDataset, decltype(&GDALClose)>;

DatasetCloser MakeDatasetCloser(GDALDataset* ds) {
  return DatasetCloser(ds, &GDALClose);
}

class OGRShapeDriverTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterOGRShape(); }
  static void TearDownTestSuite() { OGRCleanupAll(); }

  void SetUp() override {
    driver_ = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    ASSERT_THAT(driver_, NotNull());
  }

  GDALDriver* driver_ = nullptr;
};

TEST_F(OGRShapeDriverTest, ReRegisterIsNoOp) {
  RegisterOGRShape();
  EXPECT_THAT(GetGDALDriverManager()->GetDriverByName("ESRI Shapefile"),
              NotNull());
}

TEST_F(OGRShapeDriverTest, IdentifyNonExistent) {
  GDALOpenInfo open_info("/vsimem/does_not_exist_file.shp", GA_ReadOnly);
  EXPECT_FALSE(driver_->pfnIdentify(&open_info));
}

TEST_F(OGRShapeDriverTest, IdentifyShpAndShx) {
  constexpr const char* kShpPath = "/vsimem/driver_test.shp";
  constexpr const char* kShxPath = "/vsimem/driver_test.shx";
  constexpr const char* kCorruptPath = "/vsimem/driver_corrupt.shp";
  constexpr const char* kShortPath = "/vsimem/driver_short.shp";

  auto cleanup =
      absl::MakeCleanup([kShpPath, kShxPath, kCorruptPath, kShortPath]() {
        VSIUnlink(kShpPath);
        VSIUnlink(kShxPath);
        VSIUnlink(kCorruptPath);
        VSIUnlink(kShortPath);
      });

  // Valid 100-byte shapefile header with big-endian magic 9994 (0x0000270a)
  std::vector<unsigned char> valid_header(100, 0);
  valid_header[0] = 0x00;
  valid_header[1] = 0x00;
  valid_header[2] = 0x27;
  valid_header[3] = 0x0A;
  valid_header[27] = 0xE8;  // version 1000 little-endian
  valid_header[26] = 0x03;

  VSIFCloseL(VSIFileFromMemBuffer(kShpPath, valid_header.data(),
                                  valid_header.size(), FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kShxPath, valid_header.data(),
                                  valid_header.size(), FALSE));

  {
    GDALOpenInfo open_shp(kShpPath, GA_ReadOnly);
    EXPECT_TRUE(driver_->pfnIdentify(&open_shp));
  }
  {
    GDALOpenInfo open_shx(kShxPath, GA_ReadOnly);
    EXPECT_TRUE(driver_->pfnIdentify(&open_shx));
  }

  // Alternative magic 9997 (0x0000270d)
  valid_header[3] = 0x0D;
  VSIFCloseL(VSIFileFromMemBuffer(kShpPath, valid_header.data(),
                                  valid_header.size(), FALSE));
  {
    GDALOpenInfo open_shp(kShpPath, GA_ReadOnly);
    EXPECT_TRUE(driver_->pfnIdentify(&open_shp));
  }

  // Wrong magic
  valid_header[3] = 0x00;
  VSIFCloseL(VSIFileFromMemBuffer(kCorruptPath, valid_header.data(),
                                  valid_header.size(), FALSE));
  {
    GDALOpenInfo open_corrupt(kCorruptPath, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_corrupt));
  }

  // Short header (< 4 bytes)
  const unsigned char short_data[2] = {0, 0};
  VSIFCloseL(VSIFileFromMemBuffer(
      kShortPath, const_cast<unsigned char*>(short_data), 2, FALSE));
  {
    GDALOpenInfo open_short(kShortPath, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_short));
  }
}

TEST_F(OGRShapeDriverTest, IdentifyDbf) {
  constexpr const char* kDbfPath = "/vsimem/driver_test.dbf";
  constexpr const char* kCorruptDbf = "/vsimem/driver_corrupt.dbf";

  auto cleanup = absl::MakeCleanup([kDbfPath, kCorruptDbf]() {
    VSIUnlink(kDbfPath);
    VSIUnlink(kCorruptDbf);
  });

  // Valid minimal DBF header (32 bytes header + 32 bytes for 1 field + 1 byte
  // terminator = 65 bytes)
  std::vector<unsigned char> dbf_data(65, 0);
  dbf_data[0] = 0x03;  // dBASE III
  dbf_data[8] = 65;    // Header length LSB (65)
  dbf_data[9] = 0;     // Header length MSB
  dbf_data[10] = 10;   // Record length LSB (10 >= 1 field)
  dbf_data[11] = 0;
  dbf_data[64] = 0x0D;  // Header terminator

  VSIFCloseL(
      VSIFileFromMemBuffer(kDbfPath, dbf_data.data(), dbf_data.size(), FALSE));
  {
    GDALOpenInfo open_dbf(kDbfPath, GA_ReadOnly);
    EXPECT_TRUE(driver_->pfnIdentify(&open_dbf));
  }

  // Corrupt DBF header: header length < 32
  dbf_data[8] = 20;
  VSIFCloseL(VSIFileFromMemBuffer(kCorruptDbf, dbf_data.data(), dbf_data.size(),
                                  FALSE));
  {
    GDALOpenInfo open_corrupt(kCorruptDbf, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_corrupt));
  }

  // Corrupt DBF: record length < nFields
  dbf_data[8] = 65;
  dbf_data[10] = 0;  // record length = 0 < 1 field
  VSIFCloseL(VSIFileFromMemBuffer(kCorruptDbf, dbf_data.data(), dbf_data.size(),
                                  FALSE));
  {
    GDALOpenInfo open_corrupt(kCorruptDbf, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_corrupt));
  }

  // Short DBF < 32 bytes
  VSIFCloseL(VSIFileFromMemBuffer(kCorruptDbf, dbf_data.data(), 16, FALSE));
  {
    GDALOpenInfo open_short(kCorruptDbf, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_short));
  }
}

TEST_F(OGRShapeDriverTest, IdentifyZipAndShz) {
  constexpr const char* kShzPath = "/vsimem/driver_test.shz";
  constexpr const char* kShpZipPath = "/vsimem/driver_test.shp.zip";
  constexpr const char* kOtherZip = "/vsimem/driver_test.zip";
  constexpr const char* kTxtPath = "/vsimem/driver_test.txt";

  auto cleanup =
      absl::MakeCleanup([kShzPath, kShpZipPath, kOtherZip, kTxtPath]() {
        VSIUnlink(kShzPath);
        VSIUnlink(kShpZipPath);
        VSIUnlink(kOtherZip);
        VSIUnlink(kTxtPath);
      });

  // Valid zip magic: PK\x03\x04
  std::vector<unsigned char> zip_data = {0x50, 0x4B, 0x03, 0x04, 0, 0, 0, 0};
  VSIFCloseL(
      VSIFileFromMemBuffer(kShzPath, zip_data.data(), zip_data.size(), FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kShpZipPath, zip_data.data(), zip_data.size(),
                                  FALSE));
  VSIFCloseL(
      VSIFileFromMemBuffer(kOtherZip, zip_data.data(), zip_data.size(), FALSE));
  VSIFCloseL(
      VSIFileFromMemBuffer(kTxtPath, zip_data.data(), zip_data.size(), FALSE));

  {
    GDALOpenInfo open_shz(kShzPath, GA_ReadOnly);
    EXPECT_TRUE(driver_->pfnIdentify(&open_shz));
  }
  {
    GDALOpenInfo open_shp_zip(kShpZipPath, GA_ReadOnly);
    EXPECT_TRUE(driver_->pfnIdentify(&open_shp_zip));
  }
  {
    // Generic zip not ending in .shp.zip is not identified
    GDALOpenInfo open_other(kOtherZip, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_other));
  }
  {
    // Other extension (.txt)
    GDALOpenInfo open_txt(kTxtPath, GA_ReadOnly);
    EXPECT_FALSE(driver_->pfnIdentify(&open_txt));
  }
}

TEST_F(OGRShapeDriverTest, OpenIdentifyFails) {
  constexpr const char* kTxtFile = "/vsimem/driver_not_shape.txt";
  unsigned char dummy = 'a';
  VSIFCloseL(VSIFileFromMemBuffer(kTxtFile, &dummy, 1, FALSE));
  auto cleanup = absl::MakeCleanup([kTxtFile]() { VSIUnlink(kTxtFile); });

  GDALOpenInfo open_info(kTxtFile, GA_ReadOnly);
  EXPECT_THAT(driver_->pfnOpen(&open_info), IsNull());
}

TEST_F(OGRShapeDriverTest, IdentifyVsizipDirectory) {
  constexpr const char* kDirPath = "/vsimem/driver_test_dir";
  VSIMkdir(kDirPath, 0755);
  auto cleanup = absl::MakeCleanup([kDirPath]() { VSIRmdir(kDirPath); });

  GDALOpenInfo open_dir(kDirPath, GA_ReadOnly);
  EXPECT_EQ(driver_->pfnIdentify(&open_dir), GDAL_IDENTIFY_UNKNOWN);

  char* orig_name = open_dir.pszFilename;
  char vsizip1[] = "/vsizip/dir_test.shp.zip";
  open_dir.pszFilename = vsizip1;
  EXPECT_TRUE(driver_->pfnIdentify(&open_dir));

  char vsizip2[] = "/vsizip/dir_test.SHP.ZIP";
  open_dir.pszFilename = vsizip2;
  EXPECT_TRUE(driver_->pfnIdentify(&open_dir));

  // Test fpL == nullptr when bStatOK is true and not a directory
  open_dir.bIsDirectory = false;
  open_dir.fpL = nullptr;
  EXPECT_FALSE(driver_->pfnIdentify(&open_dir));

  open_dir.pszFilename = orig_name;

  // Test Open on empty .shz exercises OpenZip path
  constexpr const char* kEmptyShz = "/vsimem/empty_test.shz";
  MakeDatasetCloser(driver_->Create(kEmptyShz, 0, 0, 0, GDT_Unknown, nullptr));
  auto cleanup2 =
      absl::MakeCleanup([this, kEmptyShz]() { driver_->pfnDelete(kEmptyShz); });

  GDALOpenInfo open_empty_shz(kEmptyShz, GA_ReadOnly);
  EXPECT_THAT(driver_->pfnOpen(&open_empty_shz), IsNull());

  // Test Delete on uppercase .SHP.ZIP
  constexpr const char* kUpperShpZip = "/vsimem/empty_test.SHP.ZIP";
  MakeDatasetCloser(
      driver_->Create(kUpperShpZip, 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_EQ(driver_->pfnDelete(kUpperShpZip), CE_None);
}

TEST_F(OGRShapeDriverTest, CreateSingleFileAndOpen) {
  constexpr const char* kPath = "/vsimem/driver_create_test.shp";

  auto cleanup =
      absl::MakeCleanup([this, kPath]() { driver_->pfnDelete(kPath); });

  auto ds =
      MakeDatasetCloser(driver_->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRLayer* layer = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(layer, NotNull());

  OGRFieldDefn field_defn("name", OFTString);
  EXPECT_EQ(layer->CreateField(&field_defn), OGRERR_NONE);

  OGRFeature* feature = new OGRFeature(layer->GetLayerDefn());
  feature->SetField("name", "point_a");
  OGRPoint pt(10.0, 20.0);
  feature->SetGeometry(&pt);
  EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
  OGRFeature::DestroyFeature(feature);

  // Close dataset so files are flushed to /vsimem/
  ds.reset();

  // Reopen via driver Open
  GDALOpenInfo open_info(kPath, GA_ReadOnly);
  auto opened_ds = MakeDatasetCloser(driver_->pfnOpen(&open_info));
  ASSERT_THAT(opened_ds, NotNull());
  EXPECT_EQ(opened_ds->GetLayerCount(), 1);
  OGRLayer* read_layer = opened_ds->GetLayer(0);
  ASSERT_THAT(read_layer, NotNull());
  EXPECT_EQ(read_layer->GetFeatureCount(), 1);
  opened_ds.reset();
}

TEST_F(OGRShapeDriverTest, CreateDbfDataset) {
  constexpr const char* kPath = "/vsimem/driver_dbf_test.dbf";

  auto cleanup =
      absl::MakeCleanup([this, kPath]() { driver_->pfnDelete(kPath); });

  auto ds =
      MakeDatasetCloser(driver_->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRLayer* layer = ds->CreateLayer("dbf_layer", nullptr, wkbNone, nullptr);
  ASSERT_THAT(layer, NotNull());

  OGRFieldDefn field_defn("val", OFTInteger);
  EXPECT_EQ(layer->CreateField(&field_defn), OGRERR_NONE);

  OGRFeature* feature = new OGRFeature(layer->GetLayerDefn());
  feature->SetField("val", 42);
  EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
  OGRFeature::DestroyFeature(feature);

  ds.reset();

  GDALOpenInfo open_info(kPath, GA_ReadOnly);
  auto opened_ds = MakeDatasetCloser(driver_->pfnOpen(&open_info));
  ASSERT_THAT(opened_ds, NotNull());
  EXPECT_EQ(opened_ds->GetLayerCount(), 1);
  opened_ds.reset();
}

TEST_F(OGRShapeDriverTest, CreateDirectoryDataset) {
  constexpr const char* kDirPath = "/vsimem/driver_dir_dataset";

  auto cleanup =
      absl::MakeCleanup([this, kDirPath]() { driver_->pfnDelete(kDirPath); });

  auto ds = MakeDatasetCloser(
      driver_->Create(kDirPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRLayer* layer1 = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  ASSERT_THAT(layer1, NotNull());
  OGRLayer* layer2 = ds->CreateLayer("layer2", nullptr, wkbLineString, nullptr);
  ASSERT_THAT(layer2, NotNull());

  ds.reset();

  // Re-open/create on existing directory
  auto ds2 = MakeDatasetCloser(
      driver_->Create(kDirPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds2, NotNull());
  EXPECT_GE(ds2->GetLayerCount(), 2);
  ds2.reset();
}

TEST_F(OGRShapeDriverTest, CreateErrors) {
  constexpr const char* kRegularFile = "/vsimem/driver_existing_reg.bin";
  unsigned char dummy = 0;
  VSIFCloseL(VSIFileFromMemBuffer(kRegularFile, &dummy, 1, FALSE));
  auto cleanup =
      absl::MakeCleanup([kRegularFile]() { VSIUnlink(kRegularFile); });

  WithQuietHandler quiet_handler;
  // Creating where target is an existing regular file that is not a directory
  EXPECT_THAT(driver_->pfnCreate(kRegularFile, 0, 0, 0, GDT_Unknown, nullptr),
              IsNull());

  // Creating directory under /dev/null fails VSIMkdir
  EXPECT_THAT(
      driver_->pfnCreate("/dev/null/cant_mkdir", 0, 0, 0, GDT_Unknown, nullptr),
      IsNull());
}

TEST_F(OGRShapeDriverTest, CreateZipDataset) {
  constexpr const char* kZipPath = "/vsimem/driver_test_archive.shz";
  constexpr const char* kShpZipPath = "/vsimem/driver_test_archive.shp.zip";
  auto cleanup = absl::MakeCleanup([kZipPath, kShpZipPath]() {
    VSIUnlink(kZipPath);
    VSIUnlink(kShpZipPath);
  });

  // Test Create on .shz
  {
    auto ds = MakeDatasetCloser(
        driver_->Create(kZipPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(ds, NotNull());
    ds.reset();

    // File exists in /vsimem/
    VSIStatBufL sStat;
    EXPECT_EQ(VSIStatL(kZipPath, &sStat), 0);

    // Delete zip via pfnDelete
    EXPECT_EQ(driver_->pfnDelete(kZipPath), CE_None);
    EXPECT_EQ(VSIStatL(kZipPath, &sStat), -1);
  }

  // Test Create on .shp.zip
  {
    auto ds = MakeDatasetCloser(
        driver_->Create(kShpZipPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_THAT(ds, NotNull());
    ds.reset();

    VSIStatBufL sStat;
    EXPECT_EQ(VSIStatL(kShpZipPath, &sStat), 0);

    EXPECT_EQ(driver_->pfnDelete(kShpZipPath), CE_None);
    EXPECT_EQ(VSIStatL(kShpZipPath, &sStat), -1);
  }
}

TEST_F(OGRShapeDriverTest, DeleteSingleFileAndSidecars) {
  constexpr const char* kShp = "/vsimem/driver_del.shp";
  constexpr const char* kShx = "/vsimem/driver_del.shx";
  constexpr const char* kDbf = "/vsimem/driver_del.dbf";
  constexpr const char* kPrj = "/vsimem/driver_del.prj";
  constexpr const char* kCpg = "/vsimem/driver_del.cpg";
  constexpr const char* kQix = "/vsimem/driver_del.qix";
  constexpr const char* kSbn = "/vsimem/driver_del.sbn";
  constexpr const char* kSbx = "/vsimem/driver_del.sbx";

  unsigned char byte = 'x';
  VSIFCloseL(VSIFileFromMemBuffer(kShp, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kShx, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kDbf, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kPrj, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kCpg, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kQix, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kSbn, &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kSbx, &byte, 1, FALSE));

  EXPECT_EQ(driver_->pfnDelete(kShp), CE_None);

  VSIStatBufL sStatBuf;
  EXPECT_NE(VSIStatL(kShp, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kShx, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kDbf, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kPrj, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kCpg, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kQix, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kSbn, &sStatBuf), 0);
  EXPECT_NE(VSIStatL(kSbx, &sStatBuf), 0);
}

TEST_F(OGRShapeDriverTest, DeleteDirectory) {
  constexpr const char* kDir = "/vsimem/driver_del_dir";
  VSIMkdir(kDir, 0755);
  const std::string shpFile = std::string(kDir) + "/layer.shp";
  const std::string dbfFile = std::string(kDir) + "/layer.dbf";
  unsigned char byte = 'x';
  VSIFCloseL(VSIFileFromMemBuffer(shpFile.c_str(), &byte, 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(dbfFile.c_str(), &byte, 1, FALSE));

  EXPECT_EQ(driver_->pfnDelete(kDir), CE_None);

  VSIStatBufL sStatBuf;
  EXPECT_NE(VSIStatL(kDir, &sStatBuf), 0);
}

TEST_F(OGRShapeDriverTest, DeleteNonExistentFails) {
  WithQuietHandler quiet_handler;
  EXPECT_EQ(driver_->pfnDelete("/vsimem/does_not_exist_del.shp"), CE_Failure);
}

}  // namespace
}  // namespace autotest2
