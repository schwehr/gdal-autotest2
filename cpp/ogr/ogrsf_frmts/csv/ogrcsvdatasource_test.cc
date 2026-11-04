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
// Tests for OGR CSV datasource (ogrcsvdatasource.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/csv.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/csv/ogrcsvdatasource.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_csv.py

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/csv/ogr_csv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/ogrcsvdatasource_test";

void WriteVsiFile(const std::string& path, std::string_view content) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  EXPECT_THAT(fp, NotNull());
  if (fp == nullptr) {
    return;
  }
  if (!content.empty()) {
    EXPECT_EQ(VSIFWriteL(content.data(), 1, content.size(), fp),
              content.size());
  }
  EXPECT_EQ(0, VSIFCloseL(fp));
}

std::string ReadVsiFile(const std::string& path) {
  vsi_l_offset len = 0;
  GByte* data = VSIGetMemFileBuffer(path.c_str(), &len, /*bUnlinkAndSeize=*/FALSE);
  if (data == nullptr) {
    return "";
  }
  return std::string(reinterpret_cast<const char*>(data), len);
}

class OgrCsvDataSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0777));
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }
};

TEST_F(OgrCsvDataSourceTest, GetRealExtension) {
  EXPECT_EQ(OGRCSVDataSource::GetRealExtension("/vsimem/file.csv"), "csv");
  EXPECT_EQ(OGRCSVDataSource::GetRealExtension("/vsimem/file.tsv"), "tsv");
  EXPECT_EQ(OGRCSVDataSource::GetRealExtension("/vsimem/file.psv"), "psv");
  EXPECT_EQ(OGRCSVDataSource::GetRealExtension("/vsimem/file.gz"), "gz");
  EXPECT_EQ(
      OGRCSVDataSource::GetRealExtension("/vsigzip//vsimem/file.csv.gz"),
      "csv");
  EXPECT_EQ(
      OGRCSVDataSource::GetRealExtension("/vsigzip//vsimem/file.tsv.gz"),
      "tsv");
  EXPECT_EQ(
      OGRCSVDataSource::GetRealExtension("/vsigzip//vsimem/file.psv.gz"),
      "psv");
  EXPECT_EQ(
      OGRCSVDataSource::GetRealExtension("/vsigzip//vsimem/file.txt.gz"),
      "gz");
  EXPECT_EQ(
      OGRCSVDataSource::GetRealExtension("/vsigzip//vsimem/short.gz"), "gz");
}

TEST_F(OgrCsvDataSourceTest, GetLayerAndLayerCount) {
  OGRCSVDataSource ds;
  EXPECT_EQ(ds.GetLayerCount(), 0);
  EXPECT_EQ(ds.GetLayer(-1), nullptr);
  EXPECT_EQ(ds.GetLayer(0), nullptr);
  EXPECT_EQ(ds.GetLayer(1), nullptr);

  const std::string csv_file = std::string(kTestDir) + "/layer.csv";
  WriteVsiFile(csv_file, "id,name\n1,alice\n");

  EXPECT_TRUE(ds.Open(csv_file.c_str(), false, false, nullptr, true));
  EXPECT_EQ(ds.GetLayerCount(), 1);
  EXPECT_EQ(ds.GetLayer(-1), nullptr);
  ASSERT_NE(ds.GetLayer(0), nullptr);
  EXPECT_EQ(ds.GetLayer(1), nullptr);
  EXPECT_STREQ(ds.GetLayer(0)->GetName(), "layer");
}

TEST_F(OgrCsvDataSourceTest, TestCapability) {
  OGRCSVDataSource ds;
  EXPECT_FALSE(ds.TestCapability(ODsCCreateLayer));
  EXPECT_FALSE(ds.TestCapability(ODsCDeleteLayer));
  EXPECT_FALSE(ds.TestCapability(ODsCCreateGeomFieldAfterCreateLayer));
  EXPECT_TRUE(ds.TestCapability(ODsCCurveGeometries));
  EXPECT_TRUE(ds.TestCapability(ODsCMeasuredGeometries));
  EXPECT_TRUE(ds.TestCapability(ODsCZGeometries));
  EXPECT_FALSE(ds.TestCapability(ODsCRandomLayerWrite));
  EXPECT_FALSE(ds.TestCapability("UnknownCapability"));

  // Single file datasource
  ds.CreateForSingleFile(kTestDir,
                         (std::string(kTestDir) + "/single.csv").c_str());
  EXPECT_TRUE(ds.TestCapability(ODsCCreateLayer));
  EXPECT_TRUE(ds.TestCapability(ODsCDeleteLayer));

  // Directory in update mode
  OGRCSVDataSource dir_ds;
  EXPECT_TRUE(dir_ds.Open(kTestDir, true, true, nullptr, true));
  EXPECT_TRUE(dir_ds.TestCapability(ODsCCreateLayer));
  EXPECT_TRUE(dir_ds.TestCapability(ODsCDeleteLayer));

  EXPECT_FALSE(dir_ds.TestCapability(ODsCCreateGeomFieldAfterCreateLayer));
  dir_ds.EnableGeometryFields();
  EXPECT_TRUE(dir_ds.TestCapability(ODsCCreateGeomFieldAfterCreateLayer));
}

TEST_F(OgrCsvDataSourceTest, DeleteLayer) {
  const std::string csv_file = std::string(kTestDir) + "/test_del.csv";
  const std::string csvt_file = std::string(kTestDir) + "/test_del.csvt";
  WriteVsiFile(csv_file, "col1,col2\n10,20\n");
  WriteVsiFile(csvt_file, "Integer,Integer\n");

  // Read-only datasource
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(kTestDir, false, false, nullptr, true));
    EXPECT_EQ(ds.GetLayerCount(), 1);
    WithQuietHandler handler;
    EXPECT_EQ(ds.DeleteLayer(0), OGRERR_FAILURE);
  }

  // Update mode
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(kTestDir, true, false, nullptr, true));
    EXPECT_EQ(ds.GetLayerCount(), 1);

    WithQuietHandler handler;
    // Out of bounds index
    EXPECT_EQ(ds.DeleteLayer(-1), OGRERR_FAILURE);
    EXPECT_EQ(ds.DeleteLayer(1), OGRERR_FAILURE);

    // Valid delete
    EXPECT_EQ(ds.DeleteLayer(0), OGRERR_NONE);
    EXPECT_EQ(ds.GetLayerCount(), 0);
  }

  // Verify files were unlinked
  VSIStatBufL stat_buf;
  EXPECT_NE(VSIStatL(csv_file.c_str(), &stat_buf), 0);
  EXPECT_NE(VSIStatL(csvt_file.c_str(), &stat_buf), 0);
}

TEST_F(OgrCsvDataSourceTest, CreateForSingleFileAndGetFileList) {
  OGRCSVDataSource ds;
  const std::string single_path = std::string(kTestDir) + "/mysingle.csv";
  ds.CreateForSingleFile(kTestDir, single_path.c_str());

  OGRLayer* layer = ds.CreateLayer("mysingle", nullptr, nullptr);
  ASSERT_THAT(layer, NotNull());

  OGRFieldDefn field("str_field", OFTString);
  EXPECT_EQ(layer->CreateField(&field), OGRERR_NONE);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("str_field", "value");
  EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);

  CPLStringList file_list(ds.GetFileList(), /*bTakeOwnership=*/TRUE);
  ASSERT_THAT(file_list.List(), NotNull());
  EXPECT_EQ(1, file_list.Count());
  EXPECT_STREQ(single_path.c_str(), file_list[0]);
}

TEST_F(OgrCsvDataSourceTest, OpenVsistdoutAndVsizipWritable) {
  // /vsistdout/
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open("/vsistdout/", true, true, nullptr, false));
  }

  // /vsizip/
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(
        ds.Open("/vsizip//vsimem/some_archive.zip", true, true, nullptr, false));
  }
}

TEST_F(OgrCsvDataSourceTest, OpenCsvPrefixAndNonExistent) {
  {
    OGRCSVDataSource ds;
    EXPECT_FALSE(
        ds.Open("/vsimem/non_existent.csv", false, false, nullptr, true));
  }

  const std::string file = std::string(kTestDir) + "/prefixed.csv";
  WriteVsiFile(file, "a,b\n1,2\n");
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(
        ds.Open(("CSV:" + file).c_str(), false, false, nullptr, false));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }
}

TEST_F(OgrCsvDataSourceTest, OpenNfdcFiles) {
  const std::string nfdc_fac = std::string(kTestDir) + "/NfdcFacilities.xls";
  const std::string nfdc_run = std::string(kTestDir) + "/NfdcRunways.xls";
  // NFDC files are tab-delimited
  WriteVsiFile(nfdc_fac,
               "ID\tNAME\tARPLatitudeS\tARPLongitudeS\n"
               "SFO\tSAN FRANCISCO\t135428.3000N\t440549.6000W\n");
  WriteVsiFile(nfdc_run,
               "ID\tRWY\tBaseEndPhysicalLatitudeS\tBaseEndPhysicalLongitudeS\t"
               "BaseEndDisplacedLatitudeS\tBaseEndDisplacedLongitudeS\t"
               "ReciprocalEndPhysicalLatitudeS\tReciprocalEndPhysicalLongitudeS\t"
               "ReciprocalEndDisplacedLatitudeS\tReciprocalEndDisplacedLongitudeS\n"
               "1\t10L\t135428N\t440549W\t135429N\t440550W\t"
               "135430N\t440551W\t135431N\t440552W\n");

  // Update mode must fail
  {
    OGRCSVDataSource ds;
    EXPECT_FALSE(ds.Open(nfdc_fac.c_str(), true, false, nullptr, false));
  }
  {
    OGRCSVDataSource ds;
    EXPECT_FALSE(ds.Open(nfdc_run.c_str(), true, false, nullptr, false));
  }

  // Read-only mode
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(nfdc_fac.c_str(), false, false, nullptr, false));
    EXPECT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_THAT(layer, NotNull());
    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_THAT(feat, NotNull());
    EXPECT_THAT(feat->GetGeometryRef(), NotNull());
  }
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(nfdc_run.c_str(), false, false, nullptr, false));
    EXPECT_EQ(ds.GetLayerCount(), 4);
  }
}

TEST_F(OgrCsvDataSourceTest, OpenGnisFiles) {
  const std::string fed_codes =
      std::string(kTestDir) + "/NationalFedCodes_1.txt";
  const std::string govt_units = std::string(kTestDir) + "/GOVT_UNITS_1.txt";
  const std::string nat_file = std::string(kTestDir) + "/NationalFile_1.txt";

  // OGRCSVLayer::BuildFeatureDefn only matches *_DEC or *_LATITUDE/*_LONGITUDE
  // suffixes when binding GNIS coordinate columns (m_bIsGNIS).
  WriteVsiFile(fed_codes,
               "FEATURE_ID|FEATURE_NAME|PRIMARY_LAT_DEC|PRIMARY_LONG_DEC\n"
               "1|Place|37.6188|-122.3747\n");
  WriteVsiFile(govt_units, "ID|NAME\n1|Unit\n");
  WriteVsiFile(nat_file,
               "FEATURE_ID|FEATURE_NAME|PRIM_LAT_DEC|PRIM_LONG_DEC|"
               "SOURCE_LAT_DEC|SOURCE_LONG_DEC\n"
               "1|Peak|37.6188|-122.3747|37.6189|-122.3748\n");

  // Update mode must fail
  {
    OGRCSVDataSource ds;
    EXPECT_FALSE(ds.Open(fed_codes.c_str(), true, false, nullptr, false));
  }

  // Read-only mode
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(fed_codes.c_str(), false, false, nullptr, false));
    ASSERT_EQ(ds.GetLayerCount(), 1);
    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_THAT(layer, NotNull());
    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_THAT(feat, NotNull());
    ASSERT_THAT(feat->GetGeometryRef(), NotNull());
    EXPECT_NEAR(feat->GetGeometryRef()->toPoint()->getY(), 37.6188, 1e-4);
  }
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(govt_units.c_str(), false, false, nullptr, false));
    EXPECT_EQ(ds.GetLayerCount(), 1);
  }
  {
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(nat_file.c_str(), false, false, nullptr, false));
    ASSERT_EQ(ds.GetLayerCount(), 2);
    for (int i = 0; i < 2; ++i) {
      OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(i));
      ASSERT_THAT(layer, NotNull());
      std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
      ASSERT_THAT(feat, NotNull());
      EXPECT_THAT(feat->GetGeometryRef(), NotNull());
    }
  }
}

TEST_F(OgrCsvDataSourceTest, OpenGeonamesFiles) {
  const std::string all_countries =
      std::string(kTestDir) + "/allCountries.txt";
  WriteVsiFile(all_countries,
               "geonameid\tname\tasciiname\talternatenames\tlatitude\tlongitude\t"
               "fclass\tfcode\tcountry\tcc2\tadmin1\tadmin2\tadmin3\tadmin4\t"
               "population\televation\tdem\ttimezone\tmoddate\n"
               "1\tParis\tParis\t\t48.85\t2.35\tP\tPPLC\tFR\t\t11\t75\t75056\t\t"
               "2000000\t35\t35\tEurope/Paris\t2020-01-01\n");

  OGRCSVDataSource update_ds;
  EXPECT_FALSE(
      update_ds.Open(all_countries.c_str(), true, false, nullptr, false));

  OGRCSVDataSource read_ds;
  EXPECT_TRUE(
      read_ds.Open(all_countries.c_str(), false, false, nullptr, false));
  EXPECT_EQ(read_ds.GetLayerCount(), 1);
}

TEST_F(OgrCsvDataSourceTest, OpenSingleZipFile) {
  // A zip file with 1 csv file inside
  const std::string zip_path = std::string(kTestDir) + "/archive.zip";
  // We can create a valid zip file in /vsimem/ using /vsizip/
  const std::string vsi_zip_csv = "/vsizip/" + zip_path + "/data.csv";
  WriteVsiFile(vsi_zip_csv, "col1,col2\nval1,val2\n");

  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(("/vsizip/" + zip_path).c_str(), false, false, nullptr,
                      false));
  EXPECT_EQ(ds.GetLayerCount(), 1);

  // A zip file with a non-csv file inside
  const std::string bad_zip_path = std::string(kTestDir) + "/bad.zip";
  WriteVsiFile("/vsizip/" + bad_zip_path + "/data.txt", "some text\n");
  OGRCSVDataSource bad_ds;
  EXPECT_FALSE(bad_ds.Open(("/vsizip/" + bad_zip_path).c_str(), false, false,
                           nullptr, false));
}

TEST_F(OgrCsvDataSourceTest, OpenDirectory) {
  const std::string dir = std::string(kTestDir) + "/dir_dataset";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));

  WriteVsiFile(dir + "/layer1.csv", "id,name\n1,alpha\n");
  WriteVsiFile(dir + "/layer2.tsv", "id\tname\n2\tbeta\n");
  WriteVsiFile(dir + "/layer3.psv", "id|name\n3|gamma\n");
  WriteVsiFile(dir + "/layer1.csvt", "Integer,String\n");
  WriteVsiFile(dir + "/layer1.prj", "LOCAL_CS[\"arbitrary\"]\n");
  WriteVsiFile(dir + "/CA_Features_1.txt",
               "FEATURE_ID|FEATURE_NAME|PRIM_LAT_DEC|PRIM_LONG_DEC|"
               "SOURCE_LAT_DEC|SOURCE_LONG_DEC\n"
               "10|Test|37.0|-122.0|38.0|-123.0\n");
  WriteVsiFile(dir + "/CA_FedCodes_1.txt",
               "FEATURE_ID|FEATURE_NAME|PRIMARY_LAT_DEC|PRIMARY_LONG_DEC\n"
               "20|Fed|37.0|-122.0\n");
  WriteVsiFile(dir + "/empty.csv", "");
  WriteVsiFile(dir + "/ignored.bin", "\x01\x02\x03");

  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(dir.c_str(), false, true, nullptr, true));
  EXPECT_EQ(4, ds.GetLayerCount());
}

TEST_F(OgrCsvDataSourceTest, OpenTableSeparatorsAndOptions) {
  // Non-existent file
  {
    OGRCSVDataSource ds;
    WithQuietHandler handler;
    EXPECT_FALSE(ds.OpenTable("/vsimem/missing.csv", nullptr));
  }

  // Empty file
  {
    const std::string empty_file = std::string(kTestDir) + "/empty.csv";
    WriteVsiFile(empty_file, "");
    OGRCSVDataSource ds;
    EXPECT_FALSE(ds.OpenTable(empty_file.c_str(), nullptr));
  }

  // MAX_LINE_SIZE=0 and custom
  {
    const std::string file = std::string(kTestDir) + "/max_line.csv";
    WriteVsiFile(file, "a,b\n1,2\n");
    OGRCSVDataSource ds;
    char** options = CSLSetNameValue(nullptr, "MAX_LINE_SIZE", "0");
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    EXPECT_TRUE(ds.OpenTable(file.c_str(), options));
  }

  // SEPARATOR options: AUTO, COMMA, SEMICOLON, TAB, SPACE, PIPE, INVALID
  const std::vector<std::pair<std::string, std::string>> sep_cases = {
      {"COMMA", "a,b\n1,2\n"},
      {"SEMICOLON", "a;b\n1;2\n"},
      {"TAB", "a\tb\n1\t2\n"},
      {"SPACE", "a b\n1 2\n"},
      {"PIPE", "a|b\n1|2\n"},
      {"INVALID", "a,b\n1,2\n"},
  };

  for (const auto& [sep, content] : sep_cases) {
    const std::string file = std::string(kTestDir) + "/sep_" + sep + ".csv";
    WriteVsiFile(file, content);
    OGRCSVDataSource ds;
    char** options = CSLSetNameValue(nullptr, "SEPARATOR", sep.c_str());
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    WithQuietHandler handler;
    EXPECT_TRUE(ds.OpenTable(file.c_str(), options));
    ASSERT_NE(ds.GetLayer(0), nullptr);
    EXPECT_EQ(ds.GetLayer(0)->GetLayerDefn()->GetFieldCount(), 2);
  }

  // AUTO separator: .tsv file with tabs
  {
    const std::string tsv_file = std::string(kTestDir) + "/auto.tsv";
    WriteVsiFile(tsv_file, "col1\tcol2\nval1\tval2\n");
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.OpenTable(tsv_file.c_str(), nullptr));
  }

  // AUTO separator: two lines checking tabs vs commas
  {
    const std::string mix_file = std::string(kTestDir) + "/mix.csv";
    WriteVsiFile(mix_file, "a,b\tc\n1,2\t3\n");
    OGRCSVDataSource ds;
    WithQuietHandler handler;
    EXPECT_TRUE(ds.OpenTable(mix_file.c_str(), nullptr));
  }
}

TEST_F(OgrCsvDataSourceTest, DealWithOgrSchemaOpenOption) {
  const std::string file = std::string(kTestDir) + "/schema_test.csv";
  WriteVsiFile(file, "f1,f2,f3\n1,2,3\n");

  // Invalid JSON file
  {
    const std::string bad_schema = std::string(kTestDir) + "/bad_schema.json";
    WriteVsiFile(bad_schema, "{invalid_json}");
    OGRCSVDataSource ds;
    char** options = CSLSetNameValue(nullptr, "OGR_SCHEMA", bad_schema.c_str());
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    WithQuietHandler handler;
    EXPECT_FALSE(ds.Open(file.c_str(), false, false, options, true));
  }

  // Valid schema deleting a field
  {
    const std::string good_schema = std::string(kTestDir) + "/good_schema.json";
    WriteVsiFile(good_schema,
                 "{\"layers\": [{\"name\": \"schema_test\", \"schemaType\": "
                 "\"Full\", \"fields\": [{\"name\": \"f1\", \"type\": "
                 "\"Integer\"}, {\"name\": \"f2\", \"type\": \"Integer\"}]}]}");
    OGRCSVDataSource ds;
    char** options = CSLSetNameValue(nullptr, "OGR_SCHEMA", good_schema.c_str());
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    EXPECT_TRUE(ds.Open(file.c_str(), false, false, options, true));
    ASSERT_EQ(ds.DeletedFieldIndexes().size(), 1);
    EXPECT_EQ(ds.DeletedFieldIndexes()[0], 2);  // f3 was deleted
  }
}

TEST_F(OgrCsvDataSourceTest, ICreateLayerErrors) {
  // Read-only datasource
  {
    OGRCSVDataSource ds;
    WithQuietHandler handler;
    EXPECT_EQ(ds.CreateLayer("layer", nullptr, nullptr), nullptr);
  }

  // Non-directory datasource
  {
    const std::string file = std::string(kTestDir) + "/reg_file.csv";
    WriteVsiFile(file, "a,b\n1,2\n");
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open(file.c_str(), true, false, nullptr, true));
    WithQuietHandler handler;
    EXPECT_EQ(ds.CreateLayer("layer", nullptr, nullptr), nullptr);
  }

  // Directory datasource
  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(kTestDir, true, true, nullptr, true));

  // Invalid characters in layer name
  {
    WithQuietHandler handler;
    EXPECT_EQ(ds.CreateLayer("layer/sub", nullptr, nullptr), nullptr);
  }

  // Existing file in directory
  {
    const std::string exist_file = std::string(kTestDir) + "/exist.csv";
    WriteVsiFile(exist_file, "a,b\n1,2\n");
    WithQuietHandler handler;
    EXPECT_EQ(ds.CreateLayer("exist", nullptr, nullptr), nullptr);
  }

  // Incompatible geometry option
  {
    WithQuietHandler handler;
    char** options = CSLSetNameValue(nullptr, "GEOMETRY", "AS_XY");
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    EXPECT_EQ(ds.CreateLayer("geom_incompat", nullptr, wkbPolygon, options),
              nullptr);
  }

  // Unsupported geometry option
  {
    WithQuietHandler handler;
    char** options = CSLSetNameValue(nullptr, "GEOMETRY", "UNSUPPORTED");
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    EXPECT_EQ(ds.CreateLayer("geom_unsupp", nullptr, wkbPoint, options),
              nullptr);
  }

  // /vsistdout/ with CREATE_CSVT=YES
  {
    OGRCSVDataSource stdout_ds;
    EXPECT_TRUE(stdout_ds.Open("/vsistdout/", true, true, nullptr, false));
    char** options = CSLSetNameValue(nullptr, "CREATE_CSVT", "YES");
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    WithQuietHandler handler;
    EXPECT_EQ(stdout_ds.CreateLayer("stdout_layer", nullptr, wkbNone, options),
              nullptr);
  }
}

TEST_F(OgrCsvDataSourceTest, ICreateLayerSuccessOptions) {
  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(kTestDir, true, true, nullptr, true));

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);

  // Test SEPARATOR, LINEFORMAT, STRING_QUOTING, GEOMETRY, CREATE_CSVT, WRITE_BOM
  char** options = nullptr;
  options = CSLSetNameValue(options, "SEPARATOR", "SEMICOLON");
  options = CSLSetNameValue(options, "LINEFORMAT", "CRLF");
  options = CSLSetNameValue(options, "STRING_QUOTING", "ALWAYS");
  options = CSLSetNameValue(options, "GEOMETRY", "AS_WKT");
  options = CSLSetNameValue(options, "GEOMETRY_NAME", "my_wkt");
  options = CSLSetNameValue(options, "CREATE_CSVT", "YES");
  options = CSLSetNameValue(options, "WRITE_BOM", "YES");
  options = CSLSetNameValue(options, "HEADER", "YES");
  absl::Cleanup cl = [options] { CSLDestroy(options); };

  OGRLayer* layer = ds.CreateLayer("full_options", &srs, wkbPoint, options);
  ASSERT_THAT(layer, NotNull());

  OGRFieldDefn field("str_field", OFTString);
  EXPECT_EQ(layer->CreateField(&field), OGRERR_NONE);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("str_field", "hello;world");
  OGRPoint pt(10.5, 20.5);
  feat->SetGeometry(&pt);
  EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);

  // Verify .csv, .csvt, and .prj were created with expected content
  const std::string csv_path = std::string(kTestDir) + "/full_options.csv";
  const std::string csvt_path = std::string(kTestDir) + "/full_options.csvt";
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(csv_path.c_str(), &stat_buf));
  EXPECT_EQ(0, VSIStatL(csvt_path.c_str(), &stat_buf));
  EXPECT_EQ(0,
            VSIStatL((std::string(kTestDir) + "/full_options.prj").c_str(),
                     &stat_buf));
  EXPECT_EQ(ReadVsiFile(csv_path),
            "\xEF\xBB\xBFmy_wkt;\"str_field\"\r\n"
            "\"POINT (10.5 20.5)\";\"hello;world\"\r\n");
  EXPECT_EQ(ReadVsiFile(csvt_path), "WKT,String\r\n");
}

TEST_F(OgrCsvDataSourceTest, ICreateLayerGeometryModes) {
  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(kTestDir, true, true, nullptr, true));

  for (const char* geom_mode : {"AS_XYZ", "AS_XY", "AS_YX"}) {
    char** options = CSLSetNameValue(nullptr, "GEOMETRY", geom_mode);
    absl::Cleanup cl = [options] { CSLDestroy(options); };

    std::string name = std::string("geom_") + geom_mode;
    OGRLayer* layer = ds.CreateLayer(name.c_str(), nullptr, wkbPoint, options);
    ASSERT_THAT(layer, NotNull());

    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    OGRPoint pt(1.0, 2.0, 3.0);
    feat->SetGeometry(&pt);
    EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

    EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);
  }
}

TEST_F(OgrCsvDataSourceTest, EditableLayerFieldOperationsAndFilters) {
  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(kTestDir, true, true, nullptr, true));

  char** options = CSLSetNameValue(nullptr, "GEOMETRY", "AS_WKT");
  absl::Cleanup cl = [options] { CSLDestroy(options); };

  OGRLayer* layer = ds.CreateLayer("editable", nullptr, wkbPoint, options);
  ASSERT_THAT(layer, NotNull());

  // CreateField
  OGRFieldDefn f1("int_field", OFTInteger);
  EXPECT_EQ(layer->CreateField(&f1), OGRERR_NONE);

  // Duplicate field name
  {
    WithQuietHandler handler;
    EXPECT_EQ(layer->CreateField(&f1), OGRERR_FAILURE);
  }

  // Add a feature
  auto feat1 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat1->SetField("int_field", 100);
  OGRPoint pt1(1.0, 2.0);
  feat1->SetGeometry(&pt1);
  EXPECT_EQ(layer->CreateFeature(feat1.get()), OGRERR_NONE);

  // Add another feature
  auto feat2 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat2->SetField("int_field", 200);
  OGRPoint pt2(10.0, 20.0);
  feat2->SetGeometry(&pt2);
  EXPECT_EQ(layer->CreateFeature(feat2.get()), OGRERR_NONE);

  EXPECT_EQ(layer->GetFeatureCount(TRUE), 2);

  // Set active attribute filter before SyncToDisk
  EXPECT_EQ(layer->SetAttributeFilter("int_field = 100"), OGRERR_NONE);

  // SyncToDisk should save all features while restoring the filter
  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);

  // Filter should still be active
  EXPECT_EQ(layer->GetFeatureCount(TRUE), 1);
  EXPECT_EQ(layer->SetAttributeFilter(nullptr), OGRERR_NONE);

  // AlterFieldDefn
  OGRFieldDefn f_altered("altered_int", OFTInteger);
  EXPECT_EQ(layer->AlterFieldDefn(0, &f_altered, ALTER_NAME_FLAG),
            OGRERR_NONE);

  // DeleteField
  EXPECT_EQ(layer->DeleteField(0), OGRERR_NONE);
  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);
}

TEST_F(OgrCsvDataSourceTest, EditableLayerHasXYWithoutKeepingGeomColumns) {
  // Test bHasXY && !bKeepGeomColmuns branch in EditableSyncToDisk
  const std::string file = std::string(kTestDir) + "/xy_test.csv";
  WriteVsiFile(file, "X,Y,Z,name\n1.0,2.0,3.0,point1\n4.0,5.0,6.0,point2\n");

  OGRCSVDataSource ds;
  char** options = nullptr;
  options = CSLSetNameValue(options, "KEEP_GEOM_COLUMNS", "NO");
  options = CSLSetNameValue(options, "X_POSSIBLE_NAMES", "X");
  options = CSLSetNameValue(options, "Y_POSSIBLE_NAMES", "Y");
  options = CSLSetNameValue(options, "Z_POSSIBLE_NAMES", "Z");
  absl::Cleanup cl = [options] { CSLDestroy(options); };

  EXPECT_TRUE(ds.Open(file.c_str(), true, false, options, true));
  EXPECT_EQ(ds.GetLayerCount(), 1);

  OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
  ASSERT_THAT(layer, NotNull());

  // Add a feature
  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("name", "point3");
  OGRPoint pt(7.0, 8.0, 9.0);
  feat->SetGeometry(&pt);
  EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);
}

TEST_F(OgrCsvDataSourceTest, AdditionalBranchesForCoverage) {
  // 1. OpenTable with /vsigzip/ .csv.gz, .tsv.gz, .psv.gz
  {
    const std::string gz_csv = "/vsigzip/" + std::string(kTestDir) + "/g1.csv.gz";
    const std::string gz_tsv = "/vsigzip/" + std::string(kTestDir) + "/g2.tsv.gz";
    const std::string gz_psv = "/vsigzip/" + std::string(kTestDir) + "/g3.psv.gz";
    WriteVsiFile(gz_csv, "a,b\n1,2\n");
    WriteVsiFile(gz_tsv, "a\tb\n1\t2\n");
    WriteVsiFile(gz_psv, "a|b\n1|2\n");

    OGRCSVDataSource ds1;
    EXPECT_TRUE(ds1.OpenTable(gz_csv.c_str(), nullptr));
    ASSERT_EQ(ds1.GetLayerCount(), 1);
    EXPECT_STREQ(ds1.GetLayer(0)->GetName(), "g1");

    OGRCSVDataSource ds2;
    EXPECT_TRUE(ds2.OpenTable(gz_tsv.c_str(), nullptr));
    ASSERT_EQ(ds2.GetLayerCount(), 1);
    EXPECT_STREQ(ds2.GetLayer(0)->GetName(), "g2");

    OGRCSVDataSource ds3;
    EXPECT_TRUE(ds3.OpenTable(gz_psv.c_str(), nullptr));
    ASSERT_EQ(ds3.GetLayerCount(), 1);
    EXPECT_STREQ(ds3.GetLayer(0)->GetName(), "g3");
  }

  // 2. Open on .zip GNIS and Geonames without /vsizip/ prefix
  {
    const std::string zip_gnis =
        "/vsizip/" + std::string(kTestDir) + "/NationalFedCodes_1.zip/NationalFedCodes_1.txt";
    WriteVsiFile(zip_gnis,
                 "FEATURE_ID|FEATURE_NAME|PRIMARY_LAT_DEC|PRIMARY_LONG_DEC\n"
                 "1|Place|37.6188|-122.3747\n");
    OGRCSVDataSource ds;
    EXPECT_TRUE(ds.Open((std::string(kTestDir) + "/NationalFedCodes_1.zip").c_str(),
                        false, false, nullptr, false));

    const std::string zip_geo =
        "/vsizip/" + std::string(kTestDir) + "/allCountries.zip/allCountries.txt";
    WriteVsiFile(zip_geo, "geonameid\tname\tasciiname\talternatenames\tlatitude\tlongitude\tfclass\tfcode\tcountry\tcc2\tadmin1\tadmin2\tadmin3\tadmin4\tpopulation\televation\tdem\ttimezone\tmoddate\n1\tParis\tParis\t\t48.85\t2.35\tP\tPPLC\tFR\t\t11\t75\t75056\t\t2000000\t35\t35\tEurope/Paris\t2020-01-01\n");
    OGRCSVDataSource ds2;
    EXPECT_TRUE(ds2.Open((std::string(kTestDir) + "/allCountries.zip").c_str(),
                         false, false, nullptr, false));
  }

  // 3. ICreateLayer options
  {
    OGRCSVDataSource dir_ds;
    EXPECT_TRUE(dir_ds.Open(kTestDir, true, true, nullptr, true));
    dir_ds.EnableGeometryFields();

    for (const char* sep : {"COMMA", "TAB", "SPACE", "PIPE", "INVALID"}) {
      char** options = CSLSetNameValue(nullptr, "SEPARATOR", sep);
      absl::Cleanup cl = [options] { CSLDestroy(options); };
      WithQuietHandler handler;
      OGRLayer* layer = dir_ds.CreateLayer((std::string("sep_") + sep).c_str(),
                                          nullptr, wkbPoint, options);
      EXPECT_THAT(layer, NotNull());
    }

    for (const char* lf : {"LF", "INVALID"}) {
      char** options = CSLSetNameValue(nullptr, "LINEFORMAT", lf);
      absl::Cleanup cl = [options] { CSLDestroy(options); };
      WithQuietHandler handler;
      OGRLayer* layer = dir_ds.CreateLayer((std::string("lf_") + lf).c_str(),
                                          nullptr, wkbPoint, options);
      EXPECT_THAT(layer, NotNull());
    }

    char** options = CSLSetNameValue(nullptr, "STRING_QUOTING", "IF_NEEDED");
    absl::Cleanup cl = [options] { CSLDestroy(options); };
    OGRLayer* layer =
        dir_ds.CreateLayer("quoting_layer", nullptr, wkbPoint, options);
    EXPECT_THAT(layer, NotNull());

    // /vsistdout/ creation
    OGRCSVDataSource stdout_ds;
    EXPECT_TRUE(stdout_ds.Open("/vsistdout/", true, true, nullptr, false));
    OGRLayer* stdout_layer =
        stdout_ds.CreateLayer("stdout_ok", nullptr, wkbNone, nullptr);
    EXPECT_THAT(stdout_layer, NotNull());
  }

  // 4. EditableSyncToDisk with _WKT geom column and spatial filter
  {
    const std::string multi_wkt = std::string(kTestDir) + "/multi_wkt.csv";
    WriteVsiFile(multi_wkt,
                 "id,_WKT_g1,_WKT_g2\n"
                 "1,\"POINT (1 2)\",\"POINT (3 4)\"\n");

    OGRCSVDataSource ds;
    char** options = CSLSetNameValue(nullptr, "KEEP_GEOM_COLUMNS", "YES");
    absl::Cleanup cl = [options] { CSLDestroy(options); };

    EXPECT_TRUE(ds.Open(multi_wkt.c_str(), true, false, options, true));
    ASSERT_EQ(ds.GetLayerCount(), 1);

    OGRLayer* layer = const_cast<OGRLayer*>(ds.GetLayer(0));
    ASSERT_THAT(layer, NotNull());

    OGRPoint filter_pt(1, 2);
    layer->SetSpatialFilter(&filter_pt);

    std::unique_ptr<OGRFeature> feat =
        std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("id", 2);
    OGRPoint pt1(10, 20);
    OGRPoint pt2(30, 40);
    feat->SetGeomFieldDirectly(0, pt1.clone());
    feat->SetGeomFieldDirectly(1, pt2.clone());
    EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

    EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);
  }
}

TEST_F(OgrCsvDataSourceTest,
       OpenDirectoryUnforcedSucceedsWhenCsvFilesAreMajority) {
  const std::string csv_dir = std::string(kTestDir) + "/csv_majority_dir";
  ASSERT_EQ(0, VSIMkdir(csv_dir.c_str(), 0777));
  WriteVsiFile(csv_dir + "/a.csv", "id,name\n1,a\n");
  WriteVsiFile(csv_dir + "/b.csv", "id,name\n2,b\n");
  WriteVsiFile(csv_dir + "/other.bin", "x");

  OGRCSVDataSource ds;
  EXPECT_TRUE(ds.Open(csv_dir.c_str(), /*bUpdate=*/false, /*bForceOpen=*/false,
                      nullptr, /*bSingleDriver=*/true));
  EXPECT_EQ(2, ds.GetLayerCount());
}

// TODO: b/335317901 - Cover remaining edge cases in OGRCSVDataSource.
}  // namespace
}  // namespace autotest2
