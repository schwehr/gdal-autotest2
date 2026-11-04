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
// Tests for OGR CSV layer (ogrcsvlayer.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/csv.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/csv/ogrcsvlayer.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_csv.py

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/csv/ogr_csv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/ogrcsvlayer_test";

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

class OgrCsvLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRCSV();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0777));
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }
};

TEST_F(OgrCsvLayerTest, BuildFeatureDefnMatchesAndHeaders) {
  // Test Matches wildcard patterns via GEOM_POSSIBLE_NAMES:
  // exact_geom, *mid*, *suffix, prefix*
  const std::string file = std::string(kTestDir) + "/matches.csv";
  WriteVsiFile(file,
               "exact_geom,somemidfield,mysuffix,prefixfield,other,int_col\n"
               "POINT(1 2),POINT(3 4),POINT(5 6),POINT(7 8),nomatch,42\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  char** options = nullptr;
  options = CSLSetNameValue(
      options, "GEOM_POSSIBLE_NAMES",
      "exact_geom,*mid*,*suffix,prefix*,nomatch");
  options = CSLSetNameValue(options, "HEADERS", "YES");
  absl::Cleanup cl = [options] { CSLDestroy(options); };

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "matches", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, options);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetGeomFieldCount(), 4);
  EXPECT_STREQ(defn->GetGeomFieldDefn(0)->GetNameRef(), "exact_geom");
  EXPECT_STREQ(defn->GetGeomFieldDefn(1)->GetNameRef(), "somemidfield");
  EXPECT_STREQ(defn->GetGeomFieldDefn(2)->GetNameRef(), "mysuffix");
  EXPECT_STREQ(defn->GetGeomFieldDefn(3)->GetNameRef(), "prefixfield");
}

TEST_F(OgrCsvLayerTest, BuildFeatureDefnBomAndWhitespaceAndQuotes) {
  const std::string file = std::string(kTestDir) + "/bom.csv";
  // UTF-8 BOM, quoted fields, leading and trailing spaces
  WriteVsiFile(file,
               "\xEF\xBB\xBF\"  first  \",\" second \",\"third\"\n"
               "val1,val2,val3\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "bom", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetFieldCount(), 3);
  EXPECT_STREQ(defn->GetFieldDefn(0)->GetNameRef(), "first");
  EXPECT_STREQ(defn->GetFieldDefn(1)->GetNameRef(), "second");
  EXPECT_STREQ(defn->GetFieldDefn(2)->GetNameRef(), "third");

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_STREQ(feat->GetFieldAsString("first"), "val1");
}

TEST_F(OgrCsvLayerTest, BuildFeatureDefnSingleColumnTrailingComma) {
  const std::string file = std::string(kTestDir) + "/trailing_comma.csv";
  WriteVsiFile(file, "col1,\nval1,\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "trailing_comma", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetFieldCount(), 1);
  EXPECT_STREQ(defn->GetFieldDefn(0)->GetNameRef(), "col1");
}

TEST_F(OgrCsvLayerTest, BuildFeatureDefnHeadersNoAndAuto) {
  // Numeric first row with HEADERS=NO
  const std::string file = std::string(kTestDir) + "/numeric_no_headers.csv";
  WriteVsiFile(file, "10,20,30\n40,50,60\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "num", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetFieldCount(), 3);
  EXPECT_STREQ(defn->GetFieldDefn(0)->GetNameRef(), "field_1");
  EXPECT_STREQ(defn->GetFieldDefn(1)->GetNameRef(), "field_2");
  EXPECT_STREQ(defn->GetFieldDefn(2)->GetNameRef(), "field_3");

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_STREQ(feat->GetFieldAsString("field_1"), "10");

  // Non-numeric first row with explicit HEADERS=NO
  const std::string file2 = std::string(kTestDir) + "/alpha_headers_no.csv";
  WriteVsiFile(file2, "alpha,beta,gamma\n10,20,30\n");
  VSILFILE* fp2 = VSIFOpenL(file2.c_str(), "rb");
  ASSERT_THAT(fp2, NotNull());
  auto layer2 = std::make_unique<OGRCSVLayer>(
      nullptr, "alpha_no", fp2, -1, file2.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  const char* const options[] = {"HEADERS=NO", nullptr};
  layer2->BuildFeatureDefn(nullptr, nullptr, options);
  EXPECT_EQ(layer2->GetLayerDefn()->GetFieldCount(), 3);
  EXPECT_STREQ(layer2->GetLayerDefn()->GetFieldDefn(0)->GetNameRef(), "field_1");
}

TEST_F(OgrCsvLayerTest, BuildFeatureDefnMaxFieldCountWarning) {
  const std::string file = std::string(kTestDir) + "/many_fields.csv";
  WriteVsiFile(file, "f1,f2,f3,f4,f5\n1,2,3,4,5\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  CPLConfigOptionSetter setter("OGR_CSV_MAX_FIELD_COUNT", "3", false);

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "many", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  WithQuietHandler handler;
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  EXPECT_EQ(layer->GetLayerDefn()->GetFieldCount(), 3);
}

TEST_F(OgrCsvLayerTest, Geonames19ColTable) {
  const std::string file = std::string(kTestDir) + "/geonames.txt";
  WriteVsiFile(file,
               "2988507\tParis\tParis\t\t48.85341\t2.3488\tP\tPPLC\tFR\t\t11\t75\t"
               "75056\t\t2138551\t35\t42\tEurope/Paris\t2020-01-01\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "geonames", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, '\t');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetFieldCount(), 19);
  EXPECT_STREQ(defn->GetFieldDefn(0)->GetNameRef(), "GEONAMEID");
  EXPECT_STREQ(defn->GetFieldDefn(4)->GetNameRef(), "LATITUDE");
  EXPECT_STREQ(defn->GetFieldDefn(5)->GetNameRef(), "LONGITUDE");

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  OGRGeometry* geom = feat->GetGeometryRef();
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);
  OGRPoint* pt = geom->toPoint();
  EXPECT_NEAR(pt->getX(), 2.3488, 1e-4);
  EXPECT_NEAR(pt->getY(), 48.85341, 1e-4);
}

TEST_F(OgrCsvLayerTest, EurostatTsvFile) {
  const std::string file = std::string(kTestDir) + "/eurostat.tsv";
  // The trailing space after "2020 " is deliberate (Eurostat header quirk).
  WriteVsiFile(file,
               "unit,geo\\time\t2019\t2020 \n"
               "NR,FR\t10.5\t12.0 p\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "eurostat", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, '\t');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  ASSERT_EQ(6, defn->GetFieldCount());
  EXPECT_STREQ(defn->GetFieldDefn(0)->GetNameRef(), "unit");
  EXPECT_STREQ(defn->GetFieldDefn(1)->GetNameRef(), "geo");
  EXPECT_STREQ(defn->GetFieldDefn(2)->GetNameRef(), "time_2019");
  EXPECT_STREQ(defn->GetFieldDefn(3)->GetNameRef(), "time_2019_flag");
  EXPECT_STREQ(defn->GetFieldDefn(4)->GetNameRef(), "time_2020");
  EXPECT_STREQ(defn->GetFieldDefn(5)->GetNameRef(), "time_2020_flag");

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_STREQ(feat->GetFieldAsString("unit"), "NR");
  EXPECT_STREQ(feat->GetFieldAsString("geo"), "FR");
  EXPECT_DOUBLE_EQ(feat->GetFieldAsDouble("time_2019"), 10.5);
  EXPECT_DOUBLE_EQ(feat->GetFieldAsDouble("time_2020"), 12.0);
  EXPECT_STREQ(feat->GetFieldAsString("time_2020_flag"), "p");
}

TEST_F(OgrCsvLayerTest, CsvtAllFieldTypes) {
  const std::string file = std::string(kTestDir) + "/types.csv";
  const std::string csvt = std::string(kTestDir) + "/types.csvt";

  WriteVsiFile(file,
               "f_int,f_int5,f_bool,f_int16,f_int64,f_real,f_real102,f_float32,"
               "f_str,f_date,f_time,f_datetime,f_strlist,f_intlist,f_int64list,"
               "f_reallist,f_unsupp,coord_x,coord_y,coord_z\n"
               "1,2,1,3,999999999999,1.5,2.5,3.5,hello,2020-01-01,12:00:00,"
               "2020-01-01T12:00:00,\"[\"\"a\"\",\"\"b\"\"]\",\"[1,2]\",\"[100,200]\",\"[1.1,2.2]\","
               "foo,10.0,20.0,30.0\n");

  WriteVsiFile(csvt,
               "Integer,Integer(5),Integer(Boolean),Integer(Int16),Integer64,"
               "Real,Real(10.2),Real(Float32),String(50),Date,Time,DateTime,"
               "JSonStringList,JSonIntegerList,JSonInteger64List,JSonRealList,"
               "UnsupportedType,CoordX,CoordY,CoordZ\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "types", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  WithQuietHandler handler;
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetFieldDefn(0)->GetType(), OFTInteger);
  EXPECT_EQ(defn->GetFieldDefn(1)->GetWidth(), 5);
  EXPECT_EQ(defn->GetFieldDefn(2)->GetSubType(), OFSTBoolean);
  EXPECT_EQ(defn->GetFieldDefn(3)->GetSubType(), OFSTInt16);
  EXPECT_EQ(defn->GetFieldDefn(4)->GetType(), OFTInteger64);
  EXPECT_EQ(defn->GetFieldDefn(5)->GetType(), OFTReal);
  EXPECT_EQ(defn->GetFieldDefn(6)->GetWidth(), 10);
  EXPECT_EQ(defn->GetFieldDefn(6)->GetPrecision(), 2);
  EXPECT_EQ(defn->GetFieldDefn(7)->GetSubType(), OFSTFloat32);
  EXPECT_EQ(defn->GetFieldDefn(8)->GetType(), OFTString);
  EXPECT_EQ(defn->GetFieldDefn(8)->GetWidth(), 50);
  EXPECT_EQ(defn->GetFieldDefn(9)->GetType(), OFTDate);
  EXPECT_EQ(defn->GetFieldDefn(10)->GetType(), OFTTime);
  EXPECT_EQ(defn->GetFieldDefn(11)->GetType(), OFTDateTime);
  EXPECT_EQ(defn->GetFieldDefn(12)->GetType(), OFTStringList);
  EXPECT_EQ(defn->GetFieldDefn(13)->GetType(), OFTIntegerList);
  EXPECT_EQ(defn->GetFieldDefn(14)->GetType(), OFTInteger64List);
  EXPECT_EQ(defn->GetFieldDefn(15)->GetType(), OFTRealList);

  // Reading feature
  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_EQ(feat->GetFieldAsInteger("f_int"), 1);
  EXPECT_EQ(feat->GetFieldAsInteger("f_bool"), 1);
  EXPECT_EQ(feat->GetFieldAsInteger64("f_int64"), 999999999999LL);
  EXPECT_DOUBLE_EQ(feat->GetFieldAsDouble("f_real"), 1.5);
  EXPECT_STREQ(feat->GetFieldAsString("f_str"), "hello");

  // Geometries from CoordX/CoordY/CoordZ
  OGRGeometry* geom = feat->GetGeometryRef();
  ASSERT_THAT(geom, NotNull());
  EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);
  OGRPoint* pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(pt->getX(), 10.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 20.0);
  EXPECT_DOUBLE_EQ(pt->getZ(), 30.0);
}

TEST_F(OgrCsvLayerTest, WktGeomColumnAndSuffixes) {
  const std::string file = std::string(kTestDir) + "/wkt_suffixes.csv";
  WriteVsiFile(file,
               "WKT,_WKT_POINT_EPSG_4326,_WKT_LINESTRING,_WKT_POLYGON\n"
               "POINT(1 2),POINT(3 4),\"LINESTRING(0 0,1 1)\",\"POLYGON((0 0,1 0,1 1,0 0))\"\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "wkt_suffixes", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetGeomFieldCount(), 4);
  EXPECT_EQ(defn->GetGeomFieldDefn(0)->GetType(), wkbUnknown);
  EXPECT_EQ(defn->GetGeomFieldDefn(1)->GetType(), wkbPoint);
  EXPECT_EQ(defn->GetGeomFieldDefn(2)->GetType(), wkbLineString);
  EXPECT_EQ(defn->GetGeomFieldDefn(3)->GetType(), wkbPolygon);

  // EPSG 4326 SRS should be attached to geom field 1
  const OGRSpatialReference* srs = defn->GetGeomFieldDefn(1)->GetSpatialRef();
  ASSERT_THAT(srs, NotNull());
  EXPECT_THAT(srs->GetAuthorityCode(nullptr), HasSubstr("4326"));

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_THAT(feat->GetGeomFieldRef(0), NotNull());
  EXPECT_THAT(feat->GetGeomFieldRef(1), NotNull());
  EXPECT_THAT(feat->GetGeomFieldRef(2), NotNull());
  EXPECT_THAT(feat->GetGeomFieldRef(3), NotNull());
}

TEST_F(OgrCsvLayerTest, AutodetectFieldTypesAndWidth) {
  const std::string file = std::string(kTestDir) + "/autodetect.csv";
  WriteVsiFile(file,
               "int_to_real,int_to_str,bool_col,date_col,time_col,datetime_col,"
               "inf_to_str,empty_col\n"
               "1,10,true,2020-01-01,10:00:00,2020-01-01T10:00:00,inf,\n"
               "2.5,abc,false,2020-01-02,11:00:00,2020-01-02T11:00:00,not_a_num,\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  char** options = nullptr;
  options = CSLSetNameValue(options, "AUTODETECT_TYPE", "YES");
  options = CSLSetNameValue(options, "AUTODETECT_WIDTH", "YES");
  options = CSLSetNameValue(options, "QUOTED_FIELDS_AS_STRING", "YES");
  options = CSLSetNameValue(options, "KEEP_SOURCE_COLUMNS", "YES");
  absl::Cleanup cl = [options] { CSLDestroy(options); };

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "autodetect", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, options);

  const OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_THAT(defn, NotNull());
  EXPECT_EQ(defn->GetFieldDefn(0)->GetType(), OFTReal);
  EXPECT_GE(defn->GetFieldIndex("int_to_real_original"), 0);

  const int idx_int_to_str = defn->GetFieldIndex("int_to_str");
  ASSERT_GE(idx_int_to_str, 0);
  EXPECT_EQ(defn->GetFieldDefn(idx_int_to_str)->GetType(), OFTString);
  EXPECT_GT(defn->GetFieldDefn(idx_int_to_str)->GetWidth(), 0);

  const int idx_bool = defn->GetFieldIndex("bool_col");
  ASSERT_GE(idx_bool, 0);
  EXPECT_EQ(defn->GetFieldDefn(idx_bool)->GetSubType(), OFSTBoolean);

  const int idx_date = defn->GetFieldIndex("date_col");
  ASSERT_GE(idx_date, 0);
  EXPECT_EQ(defn->GetFieldDefn(idx_date)->GetType(), OFTDate);

  const int idx_time = defn->GetFieldIndex("time_col");
  ASSERT_GE(idx_time, 0);
  EXPECT_EQ(defn->GetFieldDefn(idx_time)->GetType(), OFTTime);

  const int idx_dt = defn->GetFieldIndex("datetime_col");
  ASSERT_GE(idx_dt, 0);
  EXPECT_EQ(defn->GetFieldDefn(idx_dt)->GetType(), OFTDateTime);

  const int idx_inf = defn->GetFieldIndex("inf_to_str");
  ASSERT_GE(idx_inf, 0);
  EXPECT_EQ(defn->GetFieldDefn(idx_inf)->GetType(), OFTString);

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_DOUBLE_EQ(feat->GetFieldAsDouble("int_to_real"), 1.0);
  EXPECT_EQ(feat->GetFieldAsInteger("bool_col"), 1);
}

TEST_F(OgrCsvLayerTest, FeatureReadingAndNavigation) {
  const std::string file = std::string(kTestDir) + "/nav.csv";
  WriteVsiFile(file,
               "id,name\n"
               "1,first\n"
               "2,second\n"
               "3,third\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "nav", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  EXPECT_EQ(layer->GetFeatureCount(FALSE), 3);

  // GetFeature(nFID)
  EXPECT_EQ(layer->GetFeature(0), nullptr);   // nFID < 1
  EXPECT_EQ(layer->GetFeature(-1), nullptr);  // nFID < 1

  std::unique_ptr<OGRFeature> feat2(layer->GetFeature(2));
  ASSERT_THAT(feat2, NotNull());
  EXPECT_EQ(feat2->GetFID(), 2);
  EXPECT_STREQ(feat2->GetFieldAsString("name"), "second");

  // Backward navigation to FID 1
  std::unique_ptr<OGRFeature> feat1(layer->GetFeature(1));
  ASSERT_THAT(feat1, NotNull());
  EXPECT_EQ(feat1->GetFID(), 1);
  EXPECT_STREQ(feat1->GetFieldAsString("name"), "first");

  // Seek past EOF
  EXPECT_EQ(layer->GetFeature(100), nullptr);

  // ResetReading
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat_seq(layer->GetNextFeature());
  ASSERT_THAT(feat_seq, NotNull());
  EXPECT_EQ(feat_seq->GetFID(), 1);
}

TEST_F(OgrCsvLayerTest, GeometryFormatsWktGeoJsonHexEwkb) {
  const std::string file = std::string(kTestDir) + "/geom_formats.csv";
  WriteVsiFile(file,
               "geom_col,val\n"
               "\"POINT(10 20)\",1\n"
               "\"{ \"\"type\"\": \"\"Point\"\", \"\"coordinates\"\": [30.0, 40.0] }\",2\n"
               "\"010100000000000000000049400000000000004e40\",3\n"
               "\"INVALID_WKT\",4\n");

  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());

  char** options = CSLSetNameValue(nullptr, "GEOM_POSSIBLE_NAMES", "geom_col");
  absl::Cleanup cl = [options] { CSLDestroy(options); };

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "geom_formats", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, options);

  // 1: WKT
  std::unique_ptr<OGRFeature> f1(layer->GetNextFeature());
  ASSERT_THAT(f1, NotNull());
  OGRGeometry* g1 = f1->GetGeometryRef();
  ASSERT_THAT(g1, NotNull());
  EXPECT_DOUBLE_EQ(g1->toPoint()->getX(), 10.0);

  // 2: GeoJSON
  std::unique_ptr<OGRFeature> f2(layer->GetNextFeature());
  ASSERT_THAT(f2, NotNull());
  OGRGeometry* g2 = f2->GetGeometryRef();
  ASSERT_THAT(g2, NotNull());
  EXPECT_DOUBLE_EQ(g2->toPoint()->getX(), 30.0);

  // 3: HexEWKB
  std::unique_ptr<OGRFeature> f3(layer->GetNextFeature());
  ASSERT_THAT(f3, NotNull());
  OGRGeometry* g3 = f3->GetGeometryRef();
  ASSERT_THAT(g3, NotNull());
  EXPECT_DOUBLE_EQ(g3->toPoint()->getX(), 50.0);

  // 4: Invalid WKT -> warning
  WithQuietHandler handler;
  std::unique_ptr<OGRFeature> f4(layer->GetNextFeature());
  ASSERT_THAT(f4, NotNull());
  EXPECT_EQ(f4->GetGeometryRef(), nullptr);
}

TEST_F(OgrCsvLayerTest, NFDCAndGNISLatLon) {
  // NFDC
  const std::string nfdc_file = std::string(kTestDir) + "/nfdc.csv";
  WriteVsiFile(nfdc_file,
               "ID,ARPLatitudeS,ARPLongitudeS\n"
               "SFO,135428.3000N,440549.6000W\n");

  VSILFILE* fp_nfdc = VSIFOpenL(nfdc_file.c_str(), "rb");
  ASSERT_THAT(fp_nfdc, NotNull());

  auto nfdc_layer = std::make_unique<OGRCSVLayer>(
      nullptr, "nfdc", fp_nfdc, -1, nfdc_file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  nfdc_layer->BuildFeatureDefn("ARP", nullptr, nullptr);

  std::unique_ptr<OGRFeature> f_nfdc(nfdc_layer->GetNextFeature());
  ASSERT_THAT(f_nfdc, NotNull());
  ASSERT_THAT(f_nfdc->GetGeometryRef(), NotNull());
  EXPECT_NEAR(f_nfdc->GetGeometryRef()->toPoint()->getY(), 37.61897, 1e-4);

  // GNIS
  const std::string gnis_file = std::string(kTestDir) + "/gnis.csv";
  WriteVsiFile(gnis_file,
               "ID,PRIM_LAT_DEC,PRIM_LONG_DEC\n"
               "1,37.61888,-122.37472\n");

  VSILFILE* fp_gnis = VSIFOpenL(gnis_file.c_str(), "rb");
  ASSERT_THAT(fp_gnis, NotNull());

  auto gnis_layer = std::make_unique<OGRCSVLayer>(
      nullptr, "gnis", fp_gnis, -1, gnis_file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/FALSE, ',');
  gnis_layer->BuildFeatureDefn(nullptr, "PRIM", nullptr);

  std::unique_ptr<OGRFeature> f_gnis(gnis_layer->GetNextFeature());
  ASSERT_THAT(f_gnis, NotNull());
  ASSERT_THAT(f_gnis->GetGeometryRef(), NotNull());
  EXPECT_NEAR(f_gnis->GetGeometryRef()->toPoint()->getY(), 37.61888, 1e-4);
}

TEST_F(OgrCsvLayerTest, TestCapabilityAndSchemaModifications) {
  const std::string file = std::string(kTestDir) + "/write_layer.csv";

  // Writable new layer
  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "write_layer", nullptr, -1, file.c_str(), /*bNew=*/TRUE,
      /*bInWriteMode=*/TRUE, ',');

  EXPECT_TRUE(layer->TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(layer->TestCapability(OLCRandomWrite));
  EXPECT_TRUE(layer->TestCapability(OLCCreateField));
  EXPECT_FALSE(layer->TestCapability(OLCCreateGeomField));
  layer->SetWriteGeometry(wkbPoint, OGR_CSV_GEOM_AS_WKT);
  EXPECT_TRUE(layer->TestCapability(OLCCreateGeomField));
  EXPECT_FALSE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCIgnoreFields));
  EXPECT_FALSE(layer->TestCapability(OLCRandomRead));
  EXPECT_FALSE(layer->TestCapability(OLCFastSpatialFilter));

  // CreateField
  OGRFieldDefn f_int("int_col", OFTInteger);
  EXPECT_EQ(layer->CreateField(&f_int), OGRERR_NONE);

  // Duplicate field name
  {
    WithQuietHandler handler;
    EXPECT_EQ(layer->CreateField(&f_int), OGRERR_FAILURE);
  }

  // OFTBinary field with bApproxOK=FALSE
  {
    OGRFieldDefn f_bin("bin_col", OFTBinary);
    WithQuietHandler handler;
    EXPECT_EQ(layer->CreateField(&f_bin, FALSE), OGRERR_FAILURE);
    // bApproxOK=TRUE succeeds with warning
    EXPECT_EQ(layer->CreateField(&f_bin, TRUE), OGRERR_NONE);
  }

  // CreateGeomField
  OGRGeomFieldDefn g_wkt("WKT", wkbPoint);
  EXPECT_EQ(layer->CreateGeomField(&g_wkt), OGRERR_NONE);

  // Duplicate geom field name
  {
    WithQuietHandler handler;
    EXPECT_EQ(layer->CreateGeomField(&g_wkt), OGRERR_FAILURE);
  }
}

TEST_F(OgrCsvLayerTest, WriteHeaderAndFeaturesVariousOptions) {
  const std::string file = std::string(kTestDir) + "/output.csv";

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "output", nullptr, -1, file.c_str(), /*bNew=*/TRUE,
      /*bInWriteMode=*/TRUE, ',');

  layer->SetCRLF(true);
  layer->SetCreateCSVT(true);
  layer->SetWriteBOM(true);
  layer->SetStringQuoting(OGRCSVLayer::StringQuoting::ALWAYS);
  layer->SetWriteGeometry(wkbNone, OGR_CSV_GEOM_AS_WKT);

  OGRFieldDefn f_str("str_col", OFTString);
  OGRFieldDefn f_int("int_col", OFTInteger);
  EXPECT_EQ(layer->CreateField(&f_str), OGRERR_NONE);
  EXPECT_EQ(layer->CreateField(&f_int), OGRERR_NONE);

  OGRGeomFieldDefn g_geom("geom", wkbPoint);
  EXPECT_EQ(layer->CreateGeomField(&g_geom), OGRERR_NONE);

  // Write feature 1
  std::unique_ptr<OGRFeature> feat1 =
      std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat1->SetField("str_col", "val1");
  feat1->SetField("int_col", 100);
  OGRPoint pt1(1.0, 2.0);
  feat1->SetGeomFieldDirectly(0, pt1.clone());
  EXPECT_EQ(layer->CreateFeature(feat1.get()), OGRERR_NONE);

  // Write feature 2 (null geometry)
  std::unique_ptr<OGRFeature> feat2 =
      std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat2->SetField("str_col", "val2");
  feat2->SetField("int_col", 200);
  EXPECT_EQ(layer->CreateFeature(feat2.get()), OGRERR_NONE);

  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);

  // Verify file contents
  const std::string csvt_file = std::string(kTestDir) + "/output.csvt";
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(file.c_str(), &stat_buf));
  EXPECT_EQ(0, VSIStatL(csvt_file.c_str(), &stat_buf));
  EXPECT_EQ(ReadVsiFile(file),
            "\xEF\xBB\xBF\"str_col\",\"int_col\",\"_WKTgeom\"\r\n"
            "\"val1\",100,\"POINT (1 2)\"\r\n"
            "\"val2\",200,\r\n");
  EXPECT_EQ(ReadVsiFile(csvt_file), "String,Integer,String\r\n");
}

TEST_F(OgrCsvLayerTest, WriteGeometryAsXYZModes) {
  // Test AS_XYZ, AS_XY, AS_YX writing modes
  for (auto geom_fmt : {OGR_CSV_GEOM_AS_XYZ, OGR_CSV_GEOM_AS_XY,
                        OGR_CSV_GEOM_AS_YX}) {
    std::string name = "out_geom_" + std::to_string(static_cast<int>(geom_fmt));
    std::string path = std::string(kTestDir) + "/" + name + ".csv";

    auto layer = std::make_unique<OGRCSVLayer>(
        nullptr, name.c_str(), nullptr, -1, path.c_str(), /*bNew=*/TRUE,
        /*bInWriteMode=*/TRUE, ',');

    layer->SetWriteGeometry(wkbPoint, geom_fmt);

    OGRFieldDefn f_name("name", OFTString);
    EXPECT_EQ(layer->CreateField(&f_name), OGRERR_NONE);

    // Feature with point
    std::unique_ptr<OGRFeature> feat =
        std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("name", "pt");
    OGRPoint pt(1.0, 2.0, 3.0);
    feat->SetGeometry(&pt);
    EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

    // Feature with null geometry
    std::unique_ptr<OGRFeature> null_feat =
        std::make_unique<OGRFeature>(layer->GetLayerDefn());
    null_feat->SetField("name", "null_pt");
    EXPECT_EQ(layer->CreateFeature(null_feat.get()), OGRERR_NONE);

    EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);

    VSIStatBufL stat_buf;
    EXPECT_EQ(0, VSIStatL(path.c_str(), &stat_buf));
  }
}

TEST_F(OgrCsvLayerTest, AppendingToExistingFileWithoutTrailingNewline) {
  const std::string file = std::string(kTestDir) + "/no_trailing_nl.csv";
  WriteVsiFile(file, "str_col\nval1");  // no trailing newline

  VSILFILE* fp = VSIFOpenL(file.c_str(), "r+b");
  ASSERT_THAT(fp, NotNull());

  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "no_trailing_nl", fp, -1, file.c_str(), /*bNew=*/FALSE,
      /*bInWriteMode=*/TRUE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, nullptr);

  std::unique_ptr<OGRFeature> feat =
      std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("str_col", "val2");
  EXPECT_EQ(layer->CreateFeature(feat.get()), OGRERR_NONE);

  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);
  layer.reset();
  EXPECT_EQ(ReadVsiFile(file), "str_col\nval1\nval2\n");
}

TEST_F(OgrCsvLayerTest, WriteCsvtAllFieldTypesAndCoordPrecision) {
  const std::string file = std::string(kTestDir) + "/write_csvt_all.csv";
  auto layer = std::make_unique<OGRCSVLayer>(
      nullptr, "write_csvt_all", nullptr, -1, file.c_str(), /*bNew=*/TRUE,
      /*bInWriteMode=*/TRUE, ' ');

  layer->SetCreateCSVT(true);
  layer->SetCRLF(true);
  layer->SetWriteGeometry(wkbPoint, OGR_CSV_GEOM_AS_WKT, "WKT");
  layer->SetStringQuoting(OGRCSVLayer::StringQuoting::IF_AMBIGUOUS);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  OGRGeomFieldDefn gfld("geom_second", wkbPointZM);
  gfld.SetSpatialRef(&srs);
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.001;
  prec.dfZResolution = 0.01;
  prec.dfMResolution = 0.1;
  gfld.SetCoordinatePrecision(prec);
  EXPECT_EQ(layer->CreateGeomField(&gfld), OGRERR_NONE);

  OGRFieldDefn f_b("b", OFTInteger);
  f_b.SetSubType(OFSTBoolean);
  EXPECT_EQ(layer->CreateField(&f_b), OGRERR_NONE);

  OGRFieldDefn f_i16("i16", OFTInteger);
  f_i16.SetSubType(OFSTInt16);
  EXPECT_EQ(layer->CreateField(&f_i16), OGRERR_NONE);

  OGRFieldDefn f_int("i", OFTInteger);
  f_int.SetWidth(5);
  EXPECT_EQ(layer->CreateField(&f_int), OGRERR_NONE);

  OGRFieldDefn f_i64("i64", OFTInteger64);
  EXPECT_EQ(layer->CreateField(&f_i64), OGRERR_NONE);

  OGRFieldDefn f_f32("f32", OFTReal);
  f_f32.SetSubType(OFSTFloat32);
  EXPECT_EQ(layer->CreateField(&f_f32), OGRERR_NONE);

  OGRFieldDefn f_r("r", OFTReal);
  f_r.SetWidth(8);
  f_r.SetPrecision(3);
  EXPECT_EQ(layer->CreateField(&f_r), OGRERR_NONE);

  OGRFieldDefn f_date("d", OFTDate);
  EXPECT_EQ(layer->CreateField(&f_date), OGRERR_NONE);

  OGRFieldDefn f_time("t", OFTTime);
  EXPECT_EQ(layer->CreateField(&f_time), OGRERR_NONE);

  OGRFieldDefn f_dt("dt", OFTDateTime);
  EXPECT_EQ(layer->CreateField(&f_dt), OGRERR_NONE);

  OGRFieldDefn f_slist("slist", OFTStringList);
  EXPECT_EQ(layer->CreateField(&f_slist), OGRERR_NONE);

  OGRFieldDefn f_ilist("ilist", OFTIntegerList);
  EXPECT_EQ(layer->CreateField(&f_ilist), OGRERR_NONE);

  OGRFieldDefn f_i64list("i64list", OFTInteger64List);
  EXPECT_EQ(layer->CreateField(&f_i64list), OGRERR_NONE);

  OGRFieldDefn f_rlist("rlist", OFTRealList);
  EXPECT_EQ(layer->CreateField(&f_rlist), OGRERR_NONE);

  OGRFieldDefn f_str("str", OFTString);
  f_str.SetWidth(10);
  EXPECT_EQ(layer->CreateField(&f_str), OGRERR_NONE);

  OGRFieldDefn f_ambig("ambig", OFTString);
  EXPECT_EQ(layer->CreateField(&f_ambig), OGRERR_NONE);

  // Feature 1: all populated
  std::unique_ptr<OGRFeature> feat1 =
      std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat1->SetField("b", 1);
  feat1->SetField("i16", 12);
  feat1->SetField("i", 123);
  feat1->SetField("i64", static_cast<GIntBig>(5000000000LL));
  feat1->SetField("f32", 1.5);
  feat1->SetField("r", 12.345);
  feat1->SetField("d", 2025, 5, 1, 0, 0, 0.0f, 0);
  feat1->SetField("t", 0, 0, 0, 12, 30, 0.0f, 0);
  feat1->SetField("dt", 2025, 5, 1, 12, 30, 0.0f, 0);

  const char* const slist[] = {"a", "b", nullptr};
  feat1->SetField(feat1->GetFieldIndex("slist"), slist);

  int ilist[] = {1, 2};
  feat1->SetField(feat1->GetFieldIndex("ilist"), 2, ilist);

  GIntBig i64list[] = {100, 200};
  feat1->SetField(feat1->GetFieldIndex("i64list"), 2, i64list);

  double rlist[] = {1.1, 2.2};
  feat1->SetField(feat1->GetFieldIndex("rlist"), 2, rlist);

  feat1->SetField("str", "hello world");
  feat1->SetField("ambig", "0123");

  OGRPoint pt(10, 20);
  feat1->SetGeometry(&pt);

  OGRPoint pt_zm(1.1111, 2.2222, 3.3333);
  pt_zm.setM(4.4444);
  feat1->SetGeomField(1, &pt_zm);

  EXPECT_EQ(layer->CreateFeature(feat1.get()), OGRERR_NONE);

  // Feature 2: unset/null
  std::unique_ptr<OGRFeature> feat2 =
      std::make_unique<OGRFeature>(layer->GetLayerDefn());
  EXPECT_EQ(layer->CreateFeature(feat2.get()), OGRERR_NONE);

  EXPECT_EQ(layer->SyncToDisk(), OGRERR_NONE);

  // Separate empty-line layer
  {
    const std::string empty_csv = std::string(kTestDir) + "/empty_line.csv";
    auto empty_layer = std::make_unique<OGRCSVLayer>(
        nullptr, "empty_line", nullptr, -1, empty_csv.c_str(), /*bNew=*/TRUE,
        /*bInWriteMode=*/TRUE, ',');
    OGRFieldDefn f_e("e", OFTString);
    EXPECT_EQ(empty_layer->CreateField(&f_e), OGRERR_NONE);
    std::unique_ptr<OGRFeature> feat_e =
        std::make_unique<OGRFeature>(empty_layer->GetLayerDefn());
    EXPECT_EQ(empty_layer->CreateFeature(feat_e.get()), OGRERR_NONE);
    EXPECT_EQ(empty_layer->SyncToDisk(), OGRERR_NONE);
  }
}

TEST_F(OgrCsvLayerTest, WktSuffixesPrjAndCsvtGeom) {
  // Part A: .csv, .csvt and .prj sidecar
  const std::string prj_file = std::string(kTestDir) + "/wkt_prj.prj";
  WriteVsiFile(prj_file,
               "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
               "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT["
               "\"degree\",0.0174532925199433]]");
  const std::string csv_file = std::string(kTestDir) + "/wkt_prj.csv";
  WriteVsiFile(csv_file,
               "WKT,CoordX,CoordY,Z,other\n\"POINT (1 2)\",10,20,30,foo\n");
  const std::string csvt_file = std::string(kTestDir) + "/wkt_prj.csvt";
  WriteVsiFile(csvt_file,
               "\"WKT\",\"CoordX\",\"CoordY\",\"Point(Z)\",\"String\"\n");

  char* papszOpenOptions[] = {
      const_cast<char*>("KEEP_GEOM_COLUMNS=YES"),
      nullptr,
  };
  GDALDatasetUniquePtr ds(
      GDALDataset::Open(csv_file.c_str(), GDAL_OF_VECTOR, nullptr,
                        papszOpenOptions, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_THAT(layer, NotNull());
  auto file_list = cpl::down_cast<OGRCSVLayer*>(layer)->GetFileList();
  EXPECT_GE(file_list.size(), 2);
  ASSERT_GE(layer->GetLayerDefn()->GetGeomFieldCount(), 1);
  EXPECT_THAT(layer->GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef(),
              NotNull());

  // Part B: _WKT suffix geometry types
  const std::string suff_file = std::string(kTestDir) + "/wkt_suffixes.csv";
  std::string header =
      "_WKT_EPSG_4326_POINT,_WKT_LINESTRING,_WKT_POLYGON,_WKT_MULTIPOINT,"
      "_WKT_MULTILINESTRING,_WKT_MULTIPOLYGON,_WKT_CIRCULARSTRING,"
      "_WKT_COMPOUNDCURVE,_WKT_CURVEPOLYGON,_WKT_CURVE,_WKT_SURFACE,"
      "_WKT_MULTICURVE,_WKT_MULTISURFACE,_WKT_POLYHEDRALSURFACE,_WKT_TIN,"
      "_WKT_TRIANGLE,X,Y\n";
  std::string row =
      "\"POINT (0 0)\",\"LINESTRING (0 0, 1 1)\",\"POLYGON ((0 0, 1 0, 1 1, 0 "
      "1, 0 0))\","
      "\"MULTIPOINT (0 0, 1 1)\",\"MULTILINESTRING ((0 0, 1 1))\","
      "\"MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)))\",\"CIRCULARSTRING (0 0, "
      "1 1, 2 0)\","
      "\"COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0))\","
      "\"CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 0 0))\","
      "\"CIRCULARSTRING (0 0, 1 1, 2 0)\",\"POLYGON ((0 0, 1 0, 1 1, 0 1, 0 "
      "0))\","
      "\"MULTICURVE ((0 0, 1 1))\",\"MULTISURFACE (((0 0, 1 0, 1 1, 0 1, 0 "
      "0)))\","
      "\"POLYHEDRALSURFACE (((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)))\","
      "\"TIN (((0 0 0, 0 1 0, 1 0 0, 0 0 0)))\",\"TRIANGLE ((0 0 0, 0 1 0, 1 "
      "0 0, 0 0 0))\","
      "10.5,20.5\n";
  WriteVsiFile(suff_file, header + row);

  char* papszOptions2[] = {
      const_cast<char*>("KEEP_GEOM_COLUMNS=NO"),
      const_cast<char*>("X_POSSIBLE_NAMES=X"),
      const_cast<char*>("Y_POSSIBLE_NAMES=Y"),
      nullptr,
  };
  GDALDatasetUniquePtr ds2(GDALDataset::Open(
      suff_file.c_str(), GDAL_OF_VECTOR, nullptr, papszOptions2, nullptr));
  ASSERT_THAT(ds2, NotNull());
  OGRLayer* layer2 = ds2->GetLayer(0);
  ASSERT_THAT(layer2, NotNull());
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldCount(), 16);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(0)->GetType(), wkbPoint);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(1)->GetType(),
            wkbLineString);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(2)->GetType(), wkbPolygon);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(3)->GetType(),
            wkbMultiPoint);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(4)->GetType(),
            wkbMultiLineString);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(5)->GetType(),
            wkbMultiPolygon);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(6)->GetType(),
            wkbCircularString);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(7)->GetType(),
            wkbCompoundCurve);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(8)->GetType(),
            wkbCurvePolygon);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(9)->GetType(), wkbCurve);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(10)->GetType(),
            wkbSurface);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(11)->GetType(),
            wkbMultiCurve);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(12)->GetType(),
            wkbMultiSurface);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(13)->GetType(),
            wkbPolyhedralSurface);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(14)->GetType(), wkbTIN);
  EXPECT_EQ(layer2->GetLayerDefn()->GetGeomFieldDefn(15)->GetType(),
            wkbTriangle);
  EXPECT_THAT(layer2->GetLayerDefn()->GetGeomFieldDefn(0)->GetSpatialRef(),
              NotNull());
}

TEST_F(OgrCsvLayerTest, AutodetectPromotionsDemotionsAndSimulateStdin) {
  CPLConfigOptionSetter setter("OGR_CSV_SIMULATE_VSISTDIN", "YES", false);
  const std::string file = std::string(kTestDir) + "/autodetect_promotions.csv";
  std::string content =
      "c_i64_real;c_i64_str;c_r_str;c_d_dt;c_d_str;c_dt_str;c_t_str;c_quoted;"
      "c_inf;c_nan\n"
      "5000000000;5000000000;1.5;2025-01-01;2025-01-01;2025-01-01T12:00:00;12:"
      "00:00;\"quoted\";inf;nan\n"
      "1,25;abc;abc;2025-01-01T12:00:00;abc;abc;abc;\"more\";-inf;nan\n";
  WriteVsiFile(file, content);

  char* options[] = {
      const_cast<char*>("AUTODETECT_TYPE=YES"),
      const_cast<char*>("AUTODETECT_WIDTH=YES"),
      const_cast<char*>("AUTODETECT_SIZE_LIMIT=0"),
      const_cast<char*>("SEPARATOR=SEMICOLON"),
      nullptr,
  };
  GDALDatasetUniquePtr ds;
  {
    WithQuietHandler quiet;
    ds.reset(GDALDataset::Open(file.c_str(), GDAL_OF_VECTOR, nullptr, options,
                               nullptr));
  }
  ASSERT_THAT(ds, NotNull());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_THAT(layer, NotNull());
  OGRFeatureDefn* defn = layer->GetLayerDefn();
  ASSERT_GE(defn->GetFieldIndex("c_i64_real"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_i64_real"))->GetType(),
            OFTReal);
  ASSERT_GE(defn->GetFieldIndex("c_i64_str"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_i64_str"))->GetType(),
            OFTString);
  ASSERT_GE(defn->GetFieldIndex("c_r_str"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_r_str"))->GetType(),
            OFTString);
  ASSERT_GE(defn->GetFieldIndex("c_d_dt"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_d_dt"))->GetType(),
            OFTDateTime);
  ASSERT_GE(defn->GetFieldIndex("c_d_str"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_d_str"))->GetType(),
            OFTString);
  ASSERT_GE(defn->GetFieldIndex("c_dt_str"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_dt_str"))->GetType(),
            OFTString);
  ASSERT_GE(defn->GetFieldIndex("c_t_str"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_t_str"))->GetType(),
            OFTString);
  ASSERT_GE(defn->GetFieldIndex("c_inf"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_inf"))->GetType(),
            OFTReal);
  ASSERT_GE(defn->GetFieldIndex("c_nan"), 0);
  EXPECT_EQ(defn->GetFieldDefn(defn->GetFieldIndex("c_nan"))->GetType(),
            OFTReal);
}

TEST_F(OgrCsvLayerTest, FieldWarningsKeepSourceAndFastTabFeatureCount) {
  // Part A: Warnings on bad value, width, and precision; and EMPTY_STRING_AS_NULL
  const std::string warn_csv = std::string(kTestDir) + "/warnings.csv";
  const std::string warn_csvt = std::string(kTestDir) + "/warnings.csvt";
  WriteVsiFile(warn_csv,
               "f_bool,f_int,f_real,f_date,f_str\n"
               "1,1234,12.3456,2025-01-01,abcdef\n"
               "bad_bool,bad_int,bad_real,bad_date,\n");
  WriteVsiFile(warn_csvt,
               "\"Integer(Boolean)\",\"Integer(2)\",\"Real(4.1)\",\"Date\","
               "\"String(3)\"\n");

  char* warn_options[] = {
      const_cast<char*>("EMPTY_STRING_AS_NULL=YES"),
      nullptr,
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::Open(warn_csv.c_str(), GDAL_OF_VECTOR,
                                              nullptr, warn_options, nullptr));
    ASSERT_THAT(ds, NotNull());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_THAT(layer, NotNull());

    // Read features
    std::unique_ptr<OGRFeature> feat1(layer->GetNextFeature());
    ASSERT_THAT(feat1, NotNull());
    std::unique_ptr<OGRFeature> feat2(layer->GetNextFeature());
    ASSERT_THAT(feat2, NotNull());
    const int idx_str = feat2->GetFieldIndex("f_str");
    ASSERT_GE(idx_str, 0);
    EXPECT_TRUE(feat2->IsFieldNull(idx_str));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // Part A2: KEEP_SOURCE_COLUMNS with AUTODETECT_TYPE
  const std::string keep_src_csv = std::string(kTestDir) + "/keep_src.csv";
  WriteVsiFile(keep_src_csv,
               "f_int,f_real\n"
               "123,45.6\n");
  char* keep_src_options[] = {
      const_cast<char*>("AUTODETECT_TYPE=YES"),
      const_cast<char*>("KEEP_SOURCE_COLUMNS=YES"),
      nullptr,
  };
  {
    GDALDatasetUniquePtr ds_src(GDALDataset::Open(
        keep_src_csv.c_str(), GDAL_OF_VECTOR, nullptr, keep_src_options, nullptr));
    ASSERT_THAT(ds_src, NotNull());
    OGRLayer* layer_src = ds_src->GetLayer(0);
    ASSERT_THAT(layer_src, NotNull());
    EXPECT_GE(layer_src->GetLayerDefn()->GetFieldIndex("f_int_original"), 0);
    EXPECT_GE(layer_src->GetLayerDefn()->GetFieldIndex("f_real_original"), 0);
  }

  // Part B: Fast tab delimiter buffer counting with NFDC geom
  const std::string tab_file = std::string(kTestDir) + "/nfdc_tab.csv";
  WriteVsiFile(tab_file,
               "ID\tARPLatitudeS\tARPLongitudeS\r\n"
               "1\t12345S\t12345W\r\n"
               "2\t23456S\t23456W\r\n");
  {
    VSILFILE* fp = VSIFOpenL(tab_file.c_str(), "rb");
    ASSERT_THAT(fp, NotNull());
    auto layer_tab = std::make_unique<OGRCSVLayer>(
        nullptr, "nfdc_tab", fp, -1, tab_file.c_str(), /*bNew=*/FALSE,
        /*bInWriteMode=*/FALSE, '\t');
    layer_tab->BuildFeatureDefn("ARP", nullptr, nullptr);
    EXPECT_EQ(layer_tab->GetFeatureCount(1), 2);
  }

  // Part B2: Geonames with GNIS dummy 0,0
  const std::string gnis_file = std::string(kTestDir) + "/gnis_dummy.csv";
  WriteVsiFile(gnis_file,
               "NAME,PRIM_LAT_DEC,PRIM_LONG_DEC\n"
               "pt0,0,0\n"
               "pt1,10.0,20.0\n");
  {
    VSILFILE* fp = VSIFOpenL(gnis_file.c_str(), "rb");
    ASSERT_THAT(fp, NotNull());
    auto layer_gnis = std::make_unique<OGRCSVLayer>(
        nullptr, "gnis_dummy", fp, -1, gnis_file.c_str(), /*bNew=*/FALSE,
        /*bInWriteMode=*/FALSE, ',');
    layer_gnis->BuildFeatureDefn(nullptr, "PRIM", nullptr);
    std::unique_ptr<OGRFeature> f0(layer_gnis->GetNextFeature());
    ASSERT_THAT(f0, NotNull());
    EXPECT_THAT(f0->GetGeometryRef(), IsNull());
    std::unique_ptr<OGRFeature> f1(layer_gnis->GetNextFeature());
    ASSERT_THAT(f1, NotNull());
    ASSERT_THAT(f1->GetGeometryRef(), NotNull());
  }

  // Part C: Comma-decimal X/Y coordinates
  const std::string comma_file = std::string(kTestDir) + "/comma_coords.csv";
  WriteVsiFile(comma_file,
               "name,X,Y\n"
               "pt1,\"12,34\",\"56,78\"\n"
               "pt2,0,0\n");
  char* comma_options[] = {
      const_cast<char*>("GEOM_POSSIBLE_NAMES=NONE"),
      const_cast<char*>("X_POSSIBLE_NAMES=X"),
      const_cast<char*>("Y_POSSIBLE_NAMES=Y"),
      nullptr,
  };
  {
    GDALDatasetUniquePtr ds_comma(GDALDataset::Open(
        comma_file.c_str(), GDAL_OF_VECTOR, nullptr, comma_options, nullptr));
    ASSERT_THAT(ds_comma, NotNull());
    OGRLayer* layer_comma = ds_comma->GetLayer(0);
    ASSERT_THAT(layer_comma, NotNull());
    std::unique_ptr<OGRFeature> f1(layer_comma->GetNextFeature());
    ASSERT_THAT(f1, NotNull());
    ASSERT_THAT(f1->GetGeometryRef(), NotNull());
    EXPECT_NEAR(f1->GetGeometryRef()->toPoint()->getX(), 12.34, 1e-4);
    EXPECT_NEAR(f1->GetGeometryRef()->toPoint()->getY(), 56.78, 1e-4);
  }
}

TEST_F(OgrCsvLayerTest, AutodetectQuotedNumericFieldAsStringWithUnquotedWidth) {
  const std::string file = std::string(kTestDir) + "/quoted_num.csv";
  WriteVsiFile(file, "code\n\"007\"\n");

  const char* const options[] = {
      "AUTODETECT_TYPE=YES",
      "AUTODETECT_WIDTH=YES",
      "QUOTED_FIELDS_AS_STRING=YES",
      nullptr,
  };
  VSILFILE* fp = VSIFOpenL(file.c_str(), "rb");
  ASSERT_THAT(fp, NotNull());
  auto layer = std::make_unique<OGRCSVLayer>(nullptr, "quoted_num", fp, -1,
                                             file.c_str(), /*bNew=*/FALSE,
                                             /*bInWriteMode=*/FALSE, ',');
  layer->BuildFeatureDefn(nullptr, nullptr, options);

  ASSERT_EQ(1, layer->GetLayerDefn()->GetFieldCount());
  const OGRFieldDefn* field_defn = layer->GetLayerDefn()->GetFieldDefn(0);
  EXPECT_EQ(OFTString, field_defn->GetType());
  EXPECT_EQ(3, field_defn->GetWidth());
}

TEST_F(OgrCsvLayerTest, RealFieldPrecisionOverflowEmitsWarning) {
  const std::string csv_file = std::string(kTestDir) + "/prec_warn.csv";
  const std::string csvt_file = std::string(kTestDir) + "/prec_warn.csvt";
  // "1.23" has length 4 (fitting width 4) so the width check passes and the
  // decimal precision check (2 fractional digits > precision 1) is reached.
  WriteVsiFile(csv_file, "val\n1.23\n");
  WriteVsiFile(csvt_file, "Real(4.1)\n");

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALDatasetUniquePtr ds(GDALDataset::Open(csv_file.c_str(), GDAL_OF_VECTOR,
                                            nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_THAT(layer, NotNull());

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("precision"));
}

TEST_F(OgrCsvLayerTest, InvalidBooleanFieldValueEmitsWarning) {
  const std::string csv_file = std::string(kTestDir) + "/bool_warn.csv";
  const std::string csvt_file = std::string(kTestDir) + "/bool_warn.csvt";
  WriteVsiFile(csv_file, "flag\nbad_bool\n");
  WriteVsiFile(csvt_file, "Integer(Boolean)\n");

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALDatasetUniquePtr ds(GDALDataset::Open(csv_file.c_str(), GDAL_OF_VECTOR,
                                            nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_THAT(layer, NotNull());

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid value type"));
}

// TODO: b/335317901 - Cover remaining edge cases in OGRCSVLayer.
}  // namespace
}  // namespace autotest2
