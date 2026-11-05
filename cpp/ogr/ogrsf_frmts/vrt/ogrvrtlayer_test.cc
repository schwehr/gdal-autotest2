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
// Tests the OGR VRT layer class, geometry parsing, feature translation,
// spatial and attribute filtering, and read/write operations (ogrvrtlayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/vrt/ogrvrtlayer.cpp
//   https://gdal.org/drivers/vector/vrt.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_vrt.py

#include <limits>
#include <memory>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/ogr/ogrfeaturedefn.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/vrt/ogr_vrt.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrvrtlayer_test";

class OgrVrtLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRVRT();
    RegisterOGRCSV();
    RegisterOGRSQLite();
    GDALRegister_MEM();
    poVrtDriver_ = GetGDALDriverManager()->GetDriverByName("OGR_VRT");
    ASSERT_NE(nullptr, poVrtDriver_);
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
  }

  GDALDriver* poVrtDriver_ = nullptr;
};

// ----------------------------------------------------------------------------
// 1. FastInitialize & GetGeomType
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, FastInitializeAndGeomType) {
  OGRVRTDataSource ds(poVrtDriver_);

  // 1. Tag is not OGRVRTLayer.
  {
    OGRVRTLayer layer(&ds);
    CPLXMLTreeCloser xml(CPLParseXMLString("<NotVRTLayer/>"));
    EXPECT_FALSE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
  }

  // 2. Missing name attribute.
  {
    OGRVRTLayer layer(&ds);
    CPLXMLTreeCloser xml(CPLParseXMLString("<OGRVRTLayer/>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Missing name attribute on OGRVRTLayer"));
  }

  // 3. Unrecognized GeometryType on layer.
  {
    OGRVRTLayer layer(&ds);
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"test\" GeometryType=\"wkbInvalidType\"/>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GeometryType wkbInvalidType not recognised"));
  }

  // 4. Invalid LayerSRS.
  {
    OGRVRTLayer layer(&ds);
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"test\" LayerSRS=\"INVALID:SRS\"/>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import LayerSRS"));
  }

  // 5. Valid FastInitialize with GeometryField, LayerSRS="NULL", FeatureCount, Extents.
  {
    OGRVRTLayer layer(&ds);
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"lyr\" FeatureCount=\"42\">\n"
        "  <GeometryField GeometryType=\"wkbPoint\" SRS=\"NULL\"\n"
        "                 ExtentXMin=\"-10\" ExtentYMin=\"-20\"\n"
        "                 ExtentXMax=\"10\" ExtentYMax=\"20\"/>\n"
        "</OGRVRTLayer>"));
    EXPECT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    EXPECT_STREQ("lyr", layer.GetName());
    EXPECT_EQ(wkbPoint, layer.GetGeomType());
    EXPECT_EQ(42, layer.GetFeatureCount(FALSE));
  }

  // 6. GeometryType = wkbNone.
  {
    OGRVRTLayer none_layer(&ds);
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"none_lyr\" GeometryType=\"wkbNone\"/>"));
    EXPECT_TRUE(none_layer.FastInitialize(xml.get(), kTestDir, FALSE));
    EXPECT_EQ(wkbNone, none_layer.GetGeomType());
  }
}

// ----------------------------------------------------------------------------
// 2. FullInitialize Source Resolution, Recursion & Dialects
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, FullInitializeSourceResolutionAndErrors) {
  // 1. Missing SrcDataSource.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"missing_src\"/>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(0, lyr->GetLayerDefn()->GetFieldCount());
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Missing SrcDataSource"));
  }

  // 2. Relative to VRT path resolution with CSV: prefix.
  {
    constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/dir1/data.csv";
    VsiMemTempWrapper csv(kCsvPath, "id,name\n1,alice\n");

    constexpr char kVrtPath[] = "/vsimem/ogrvrtlayer_test/dir1/test_rel.vrt";
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"rel_layer\">\n"
        "    <SrcDataSource relativetoVRT=\"1\">CSV:data.csv</SrcDataSource>\n"
        "    <SrcLayer>data</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    VsiMemTempWrapper vrt_file(kVrtPath, kVrt);

    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(1, lyr->GetFeatureCount(TRUE));
  }

  // 3. Cyclic opening in shared mode.
  {
    constexpr char kSelfVrtPath[] = "/vsimem/ogrvrtlayer_test/cyclic.vrt";
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"cyclic_layer\">\n"
        "    <SrcDataSource shared=\"1\">/vsimem/ogrvrtlayer_test/cyclic.vrt</SrcDataSource>\n"
        "    <SrcLayer>cyclic_layer</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    VsiMemTempWrapper vrt_file(kSelfVrtPath, kVrt);

    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kSelfVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cyclic VRT opening detected"));
  }

  // 4. Deep recursion in non-shared mode (>= 32 levels).
  {
    OGRVRTDataSource ds(poVrtDriver_);
    ds.SetCallLevel(32);
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"deep\">\n"
        "  <SrcDataSource>/does_not_matter.vrt</SrcDataSource>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Trying to open a VRT from a VRT"));
  }

  // 5. SrcSQL with empty and explicit dialect, and invalid SQL error.
  {
    constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/sql_src.csv";
    VsiMemTempWrapper csv(kCsvPath, "id,score\n1,100\n2,200\n");

    // Valid SQL.
    constexpr char kVrtValidSql[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"sql_lyr\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/sql_src.csv</SrcDataSource>\n"
        "    <SrcSQL dialect=\"\">SELECT id FROM sql_src WHERE id = '1'</SrcSQL>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtValidSql, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(1, lyr->GetFeatureCount(TRUE));

    // Invalid SQL.
    constexpr char kVrtBadSql[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_sql_lyr\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/sql_src.csv</SrcDataSource>\n"
        "    <SrcSQL dialect=\"OGRSQL\">SELECT non_existent_col FROM sql_src</SrcSQL>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> bad_ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtBadSql, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, bad_ds);
    OGRLayer* bad_lyr = bad_ds->GetLayer(0);
    ASSERT_NE(nullptr, bad_lyr);
    EXPECT_EQ(nullptr, bad_lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("SQL statement failed"));
  }

  // 6. Layer not found on source datasource.
  {
    constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/lyr_test.csv";
    VsiMemTempWrapper csv(kCsvPath, "id\n1\n");
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"not_found_lyr\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/lyr_test.csv</SrcDataSource>\n"
        "    <SrcLayer>no_such_layer</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed to find layer"));
  }
}

// ----------------------------------------------------------------------------
// 3. Geometry Encodings & SrcRegion
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, GeometryEncodingsAndSrcRegion) {
  // Test WKT, WKB, Shape, PointFromColumns, Direct, and None encodings.
  constexpr char kCsvData[] =
      "fid,wkt,wkb,x,y,z,m\n"
      "10,\"POINT (10 20)\",010100000000000000000024400000000000003440,10,20,30,40\n"
      "20,\"LINESTRING (0 0, 1 1)\",010100000000000000000000000000000000000000,50,60,70,80\n";
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/geoms.csv";
  VsiMemTempWrapper csv(kCsvPath, kCsvData);

  // 1. WKT, WKB, and PointFromColumns (with Z and M) and SrcRegion clipping.
  constexpr char kVrt[] =
      R"vrt(<OGRVRTDataSource>
        <OGRVRTLayer name="multi_geom">
          <SrcDataSource>/vsimem/ogrvrtlayer_test/geoms.csv</SrcDataSource>
          <SrcLayer>geoms</SrcLayer>
          <FID>fid</FID>
          <GeometryField name="geom_wkt" field="wkt" encoding="WKT" reportSrcColumn="FALSE"/>
          <GeometryField name="geom_wkb" field="wkb" encoding="WKB" reportSrcColumn="FALSE"/>
          <GeometryField name="geom_pts" encoding="PointFromColumns" x="x" y="y" z="z" m="m"
                         reportSrcColumn="FALSE">
            <SrcRegion clip="TRUE">POLYGON ((5 5, 25 5, 25 25, 5 25, 5 5))</SrcRegion>
            <XYResolution>0.001</XYResolution>
            <ZResolution>0.01</ZResolution>
            <MResolution>0.1</MResolution>
          </GeometryField>
        </OGRVRTLayer>
      </OGRVRTDataSource>)vrt";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // Feature 1: (10, 20) inside SrcRegion.
  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(10, feat->GetFID());

  OGRGeometry* g_wkt = feat->GetGeomFieldRef(0);
  ASSERT_NE(nullptr, g_wkt);
  EXPECT_EQ(wkbPoint, wkbFlatten(g_wkt->getGeometryType()));

  OGRGeometry* g_wkb = feat->GetGeomFieldRef(1);
  ASSERT_NE(nullptr, g_wkb);
  EXPECT_EQ(wkbPoint, wkbFlatten(g_wkb->getGeometryType()));

  OGRGeometry* g_pts = feat->GetGeomFieldRef(2);
  ASSERT_NE(nullptr, g_pts);
  EXPECT_TRUE(g_pts->Is3D());
  EXPECT_TRUE(g_pts->IsMeasured());
  OGRPoint* pt = g_pts->toPoint();
  EXPECT_DOUBLE_EQ(10.0, pt->getX());
  EXPECT_DOUBLE_EQ(20.0, pt->getY());
  EXPECT_DOUBLE_EQ(30.0, pt->getZ());
  EXPECT_DOUBLE_EQ(40.0, pt->getM());

  // Feature 2: (50, 60) outside SrcRegion -> skipped by retry in TranslateFeature!
  // So GetNextFeature returns nullptr.
  std::unique_ptr<OGRFeature> feat2(lyr->GetNextFeature());
  EXPECT_EQ(nullptr, feat2);
}

TEST_F(OgrVrtLayerTest, GeometryEncodingErrors) {
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/err_src.csv";
  VsiMemTempWrapper csv(kCsvPath, "a,b\n1,2\n");

  // 1. Unrecognized encoding.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_enc\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/err_src.csv</SrcDataSource>\n"
        "    <SrcLayer>err_src</SrcLayer>\n"
        "    <GeometryField encoding=\"UnknownEncoding\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("encoding=\"UnknownEncoding\" not recognised"));
  }

  // 2. PointFromColumns missing x or y.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_pts\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/err_src.csv</SrcDataSource>\n"
        "    <SrcLayer>err_src</SrcLayer>\n"
        "    <GeometryField encoding=\"PointFromColumns\" x=\"missing_x\" y=\"b\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify source X or Y field"));
  }

  // 3. WKT missing field.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_wkt\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/err_src.csv</SrcDataSource>\n"
        "    <SrcLayer>err_src</SrcLayer>\n"
        "    <GeometryField encoding=\"WKT\" field=\"no_such_field\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify source field 'no_such_field'"));
  }

  // 4. Direct geometry field missing on layer with multiple geom fields.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_direct\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/err_src.csv</SrcDataSource>\n"
        "    <SrcLayer>err_src</SrcLayer>\n"
        "    <GeometryField encoding=\"Direct\" field=\"missing_geom\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify source geometry field"));
  }

  // 5. Invalid SrcRegion warning.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_srcregion\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "    <SrcRegion>INVALID WKT</SrcRegion>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_NE(nullptr, lyr->GetLayerDefn());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Ignoring SrcRegion. It must be a valid WKT geometry"));
  }
}

// ----------------------------------------------------------------------------
// 4. Schema Definitions, Field Subtypes & Errors
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, FieldDefinitionsSubtypesAndErrors) {
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/fields_src.csv";
  VsiMemTempWrapper csv(kCsvPath, "f_int,f_str,f_real\n1,hello,3.14\n");

  // 1. Missing field name.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_field\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fields_src.csv</SrcDataSource>\n"
        "    <SrcLayer>fields_src</SrcLayer>\n"
        "    <Field/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to identify Field name"));
  }

  // 2. Unrecognized field type.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_type\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fields_src.csv</SrcDataSource>\n"
        "    <SrcLayer>fields_src</SrcLayer>\n"
        "    <Field name=\"f1\" type=\"NotAType\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to identify Field type 'NotAType'"));
  }

  // 3. Unrecognized field subtype.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"bad_subtype\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fields_src.csv</SrcDataSource>\n"
        "    <SrcLayer>fields_src</SrcLayer>\n"
        "    <Field name=\"f1\" type=\"Integer\" subtype=\"NotASubType\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify Field subtype 'NotASubType'"));
  }

  // 4. Incompatible type and subtype.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"incompatible\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fields_src.csv</SrcDataSource>\n"
        "    <SrcLayer>fields_src</SrcLayer>\n"
        "    <Field name=\"f1\" type=\"String\" subtype=\"Boolean\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid subtype 'Boolean' for type 'String'"));
  }

  // 5. Missing source field referenced in src attribute.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"missing_src_field\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fields_src.csv</SrcDataSource>\n"
        "    <SrcLayer>fields_src</SrcLayer>\n"
        "    <Field name=\"f1\" src=\"non_existent_col\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to find source field 'non_existent_col'"));
  }

  // 6. Valid fields with subtypes, width, precision, alternativeName, comment, nullable, unique, default.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"full_fields\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fields_src.csv</SrcDataSource>\n"
        "    <SrcLayer>fields_src</SrcLayer>\n"
        "    <Field name=\"f_b\" src=\"f_int\" type=\"Integer\" subtype=\"Boolean\"/>\n"
        "    <Field name=\"f_s\" src=\"f_str\" type=\"String\" width=\"32\"\n"
        "           alternativeName=\"alt_s\" comment=\"my comment\" nullable=\"false\"\n"
        "           unique=\"true\" default=\"'def'\"/>\n"
        "    <Field name=\"f_r\" src=\"f_real\" type=\"Real\" width=\"10\" precision=\"2\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    const OGRFeatureDefn* defn = lyr->GetLayerDefn();
    ASSERT_NE(nullptr, defn);
    EXPECT_EQ(3, defn->GetFieldCount());

    const OGRFieldDefn* fd0 = defn->GetFieldDefn(0);
    EXPECT_EQ(OFTInteger, fd0->GetType());
    EXPECT_EQ(OFSTBoolean, fd0->GetSubType());

    const OGRFieldDefn* fd1 = defn->GetFieldDefn(1);
    EXPECT_EQ(OFTString, fd1->GetType());
    EXPECT_EQ(32, fd1->GetWidth());
    EXPECT_STREQ("alt_s", fd1->GetAlternativeNameRef());
    EXPECT_STREQ("my comment", fd1->GetComment().c_str());
    EXPECT_FALSE(fd1->IsNullable());
    EXPECT_TRUE(fd1->IsUnique());
    EXPECT_STREQ("'def'", fd1->GetDefault());

    std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(1, feat->GetFieldAsInteger(0));
    EXPECT_STREQ("hello", feat->GetFieldAsString(1));
    EXPECT_DOUBLE_EQ(3.14, feat->GetFieldAsDouble(2));
  }
}

// ----------------------------------------------------------------------------
// 5. Spatial and Attribute Filtering & Subqueries
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, SpatialAndAttributeFiltering) {
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/filter_pts.csv";
  constexpr char kCsvtPath[] = "/vsimem/ogrvrtlayer_test/filter_pts.csvt";
  constexpr char kCsvData[] =
      "id,name,x,y\n"
      "1,pt1,10,10\n"
      "2,pt2,20,20\n"
      "3,pt3,30,30\n";
  VsiMemTempWrapper csv(kCsvPath, kCsvData);
  VsiMemTempWrapper csvt(kCsvtPath, "Integer,String,Real,Real\n");

  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"pts_lyr\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/filter_pts.csv</SrcDataSource>\n"
      "    <SrcLayer>filter_pts</SrcLayer>\n"
      "    <GeometryField encoding=\"PointFromColumns\" x=\"x\" y=\"y\" useSpatialSubquery=\"TRUE\"/>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // 1. Spatial filter.
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilterRect(5, 5, 15, 15));
  std::unique_ptr<OGRFeature> f1(lyr->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_STREQ("pt1", f1->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, lyr->GetNextFeature());

  // 2. Clear spatial filter and set attribute filter.
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilter(nullptr));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("id = '2'"));
  std::unique_ptr<OGRFeature> f2(lyr->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_STREQ("pt2", f2->GetFieldAsString("name"));
  EXPECT_EQ(nullptr, lyr->GetNextFeature());

  // 3. Clear attribute filter.
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(nullptr));
  lyr->ResetReading();
  EXPECT_EQ(3, lyr->GetFeatureCount(TRUE));

  // 4. SetNextByIndex.
  EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(1));
  std::unique_ptr<OGRFeature> f_idx(lyr->GetNextFeature());
  ASSERT_NE(nullptr, f_idx);
  EXPECT_STREQ("pt2", f_idx->GetFieldAsString("name"));

  // 5. Capabilities.
  EXPECT_FALSE(lyr->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(lyr->TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(lyr->TestCapability(OLCRandomWrite));
  EXPECT_FALSE(lyr->TestCapability(OLCDeleteFeature));
  EXPECT_FALSE(lyr->TestCapability(OLCTransactions));
  EXPECT_FALSE(lyr->TestCapability("NoSuchCap"));

  // 6. Extent.
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, lyr->GetExtent(&env, TRUE));
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(30.0, env.MaxX);

  // 7. GetSrcDataset.
  auto* vrt_lyr = cpl::down_cast<OGRVRTLayer*>(lyr);
  EXPECT_NE(nullptr, vrt_lyr->GetSrcDataset());
}

// ----------------------------------------------------------------------------
// 6. Read / Write Operations (Fast Path & Translated Path)
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, ReadWriteOperations) {
  // Create an updatable SQLite database in /vsimem/.
  constexpr char kSqlitePath[] = "/vsimem/ogrvrtlayer_test/updatable.sqlite";
  GDALDriver* sqlite_drv = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, sqlite_drv);

  {
    std::unique_ptr<GDALDataset> sqlite_ds(
        sqlite_drv->Create(kSqlitePath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, sqlite_ds);
    OGRLayer* sqlite_lyr = sqlite_ds->CreateLayer("points", nullptr, wkbPoint);
    ASSERT_NE(nullptr, sqlite_lyr);
    OGRFieldDefn f_name("name", OFTString);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateField(&f_name));
    OGRFieldDefn f_val("val", OFTReal);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateField(&f_val));

    // Insert an initial feature.
    OGRFeature feat(sqlite_lyr->GetLayerDefn());
    feat.SetField("name", "initial");
    feat.SetField("val", 12.34);
    OGRPoint pt(1.0, 2.0);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateFeature(&feat));
  }

  // 1. Read-only VRT rejecting mutations.
  {
    constexpr char kVrtRo[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"points_ro\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/updatable.sqlite</SrcDataSource>\n"
        "    <SrcLayer>points</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtRo, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);

    OGRFeature feat(lyr->GetLayerDefn());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE, lyr->CreateFeature(&feat));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unsupported operation on a read-only datasource"));
    EXPECT_EQ(OGRERR_FAILURE, lyr->SetFeature(&feat));
    EXPECT_EQ(OGRERR_FAILURE, lyr->DeleteFeature(1));
    EXPECT_EQ(OGRERR_FAILURE, lyr->StartTransaction());
    EXPECT_EQ(OGRERR_FAILURE, lyr->CommitTransaction());
    EXPECT_EQ(OGRERR_FAILURE, lyr->RollbackTransaction());
  }

  // 2. Updatable VRT on fast path (identical schema).
  {
    constexpr char kVrtUpdate[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"points\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/updatable.sqlite</SrcDataSource>\n"
        "    <SrcLayer>points</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtUpdate, GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_TRUE(lyr->TestCapability(OLCSequentialWrite));
    EXPECT_TRUE(lyr->TestCapability(OLCRandomWrite));
    EXPECT_TRUE(lyr->TestCapability(OLCDeleteFeature));
    EXPECT_TRUE(lyr->TestCapability(OLCTransactions));

    // Create a feature.
    OGRFeature new_feat(lyr->GetLayerDefn());
    new_feat.SetField("name", "added_fast");
    new_feat.SetField("val", 99.9);
    OGRPoint pt(3.0, 4.0);
    new_feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&new_feat));
    const GIntBig added_fid = new_feat.GetFID();

    // SetFeature.
    new_feat.SetField("val", 100.0);
    EXPECT_EQ(OGRERR_NONE, lyr->SetFeature(&new_feat));

    // GetFeature and verify.
    std::unique_ptr<OGRFeature> check_feat(lyr->GetFeature(added_fid));
    ASSERT_NE(nullptr, check_feat);
    EXPECT_DOUBLE_EQ(100.0, check_feat->GetFieldAsDouble("val"));

    // SyncToDisk.
    EXPECT_EQ(OGRERR_NONE, lyr->SyncToDisk());

    // Transactions.
    EXPECT_EQ(OGRERR_NONE, lyr->StartTransaction());
    EXPECT_EQ(OGRERR_NONE, lyr->CommitTransaction());
    EXPECT_EQ(OGRERR_NONE, lyr->StartTransaction());
    EXPECT_EQ(OGRERR_NONE, lyr->RollbackTransaction());

    // DeleteFeature.
    EXPECT_EQ(OGRERR_NONE, lyr->DeleteFeature(added_fid));
  }

  // 3. Updatable VRT on translated path (custom field mapping, WKT geometry).
  {
    constexpr char kVrtTrans[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"trans_update\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/updatable.sqlite</SrcDataSource>\n"
        "    <SrcLayer>points</SrcLayer>\n"
        "    <Field name=\"TITLE\" src=\"name\" type=\"String\"/>\n"
        "    <Field name=\"VALUE\" src=\"val\" type=\"Real\"/>\n"
        "    <GeometryField name=\"geom\" encoding=\"Direct\"/>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtTrans, GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);

    OGRFeature new_feat(lyr->GetLayerDefn());
    new_feat.SetField("TITLE", "translated");
    new_feat.SetField("VALUE", 77.7);
    OGRPoint pt(5.0, 6.0);
    new_feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&new_feat));
    const GIntBig fid = new_feat.GetFID();

    new_feat.SetField("VALUE", 88.8);
    EXPECT_EQ(OGRERR_NONE, lyr->SetFeature(&new_feat));

    std::unique_ptr<OGRFeature> check_feat(lyr->GetFeature(fid));
    ASSERT_NE(nullptr, check_feat);
    EXPECT_STREQ("translated", check_feat->GetFieldAsString("TITLE"));
    EXPECT_DOUBLE_EQ(88.8, check_feat->GetFieldAsDouble("VALUE"));

    EXPECT_EQ(OGRERR_NONE, lyr->DeleteFeature(fid));
  }
}

// ----------------------------------------------------------------------------
// 7. SetIgnoredFields
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, SetIgnoredFields) {
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/ignore_src.csv";
  VsiMemTempWrapper csv(kCsvPath, "id,a,b,c,x,y\n1,10,20,30,1,2\n");

  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"ignore_lyr\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/ignore_src.csv</SrcDataSource>\n"
      "    <SrcLayer>ignore_src</SrcLayer>\n"
      "    <Field name=\"A\" src=\"a\" type=\"Integer\"/>\n"
      "    <Field name=\"B\" src=\"b\" type=\"Integer\"/>\n"
      "    <Field name=\"X_COL\" src=\"x\" type=\"Integer\"/>\n"
      "    <GeometryField name=\"geom\" encoding=\"PointFromColumns\" x=\"x\" y=\"y\"/>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // Ignore field A and OGR_GEOMETRY.
  const char* const ignored[] = {"A", "OGR_GEOMETRY", nullptr};
  EXPECT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(ignored));

  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(20, feat->GetFieldAsInteger("B"));

  // Ignore X_COL (PointFromColumns coordinate column -> bOKToIgnore = false).
  const char* const ignored2[] = {"X_COL", nullptr};
  EXPECT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(ignored2));

  lyr->ResetReading();
  std::unique_ptr<OGRFeature> feat2(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat2);
  EXPECT_EQ(20, feat2->GetFieldAsInteger("B"));
  EXPECT_NE(nullptr, feat2->GetGeometryRef());

  // Reset ignored fields.
  EXPECT_EQ(OGRERR_NONE, lyr->SetIgnoredFields(nullptr));
}

// ----------------------------------------------------------------------------
// 8. Spatial Subquery with Numeric Fields & Infinite Envelopes
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, SpatialSubqueryNumericFieldsAndEnvelopes) {
  constexpr char kSqlitePath[] = "/vsimem/ogrvrtlayer_test/subquery.sqlite";
  GDALDriver* sqlite_drv = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, sqlite_drv);

  {
    std::unique_ptr<GDALDataset> sqlite_ds(
        sqlite_drv->Create(kSqlitePath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, sqlite_ds);
    OGRLayer* sqlite_lyr = sqlite_ds->CreateLayer("points", nullptr, wkbNone);
    ASSERT_NE(nullptr, sqlite_lyr);
    OGRFieldDefn f_id("id", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateField(&f_id));
    OGRFieldDefn f_x("x", OFTReal);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateField(&f_x));
    OGRFieldDefn f_y("y", OFTReal);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateField(&f_y));

    for (int i = 1; i <= 5; ++i) {
      OGRFeature feat(sqlite_lyr->GetLayerDefn());
      feat.SetField("id", i);
      feat.SetField("x", i * 10.0);
      feat.SetField("y", i * 10.0);
      EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateFeature(&feat));
    }
  }

  // 1. Spatial subquery with both spatial filter and SrcRegion (intersecting).
  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"pts_subquery\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/subquery.sqlite</SrcDataSource>\n"
      "    <SrcLayer>points</SrcLayer>\n"
      "    <GeometryField encoding=\"PointFromColumns\" x=\"x\" y=\"y\" useSpatialSubquery=\"TRUE\">\n"
      "      <SrcRegion>POLYGON ((5 5, 35 5, 35 35, 5 35, 5 5))</SrcRegion>\n"
      "    </GeometryField>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // Set spatial filter intersecting SrcRegion (15, 15, 25, 25) -> expects point (20, 20).
  lyr->SetSpatialFilterRect(15, 15, 25, 25);
  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(2, feat->GetFieldAsInteger("id"));
  EXPECT_EQ(nullptr, lyr->GetNextFeature());

  // Set spatial filter disjoint from SrcRegion (50, 50, 60, 60) -> expects 0 features.
  lyr->SetSpatialFilterRect(50, 50, 60, 60);
  EXPECT_EQ(nullptr, lyr->GetNextFeature());

  // Clear spatial filter -> uses only SrcRegion.
  lyr->SetSpatialFilter(nullptr);
  EXPECT_EQ(3, lyr->GetFeatureCount(TRUE));

  // Combined with attribute filter.
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("id = 1"));
  std::unique_ptr<OGRFeature> feat_attr(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat_attr);
  EXPECT_EQ(1, feat_attr->GetFieldAsInteger("id"));
  EXPECT_EQ(nullptr, lyr->GetNextFeature());
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(nullptr));

  // Intersection of SrcRegion with an infinite rectangle yields empty/null ->
  // envelope (0, 0, 0, 0) -> "\"x\" > 0 AND \"x\" < 0", returning 0 features.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr double kInf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilterRect(-kInf, -kInf, kInf, kInf));
    lyr->ResetReading();
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
  }

  // Spatial subquery without SrcRegion to test isinf branches (ogrvrtlayer.cpp:1214-1260).
  constexpr char kVrtNoSrcRegion[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"spatial_subq_no_srcregion\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/subquery.sqlite</SrcDataSource>\n"
      "    <SrcLayer>points</SrcLayer>\n"
      "    <GeometryField encoding=\"PointFromColumns\" x=\"x\" y=\"y\" useSpatialSubquery=\"TRUE\"/>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds_no_sr(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtNoSrcRegion, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds_no_sr);
  OGRLayer* lyr_no_sr = ds_no_sr->GetLayer(0);
  ASSERT_NE(nullptr, lyr_no_sr);

  // [-inf, inf] envelope leaves osFilter empty, returning all 5 features.
  constexpr double kInf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(OGRERR_NONE, lyr_no_sr->SetSpatialFilterRect(-kInf, -kInf, kInf, kInf));
  lyr_no_sr->ResetReading();
  EXPECT_EQ(5, lyr_no_sr->GetFeatureCount(TRUE));

  // Finite spatial filter covering all points returns 3 features.
  EXPECT_EQ(OGRERR_NONE, lyr_no_sr->SetSpatialFilterRect(5, 5, 35, 35));
  lyr_no_sr->ResetReading();
  EXPECT_EQ(3, lyr_no_sr->GetFeatureCount(TRUE));

  // [+inf, +inf] causes "0 = 1" in osFilter.
  EXPECT_EQ(OGRERR_NONE, lyr_no_sr->SetSpatialFilterRect(kInf, kInf, kInf, kInf));
  lyr_no_sr->ResetReading();
  EXPECT_EQ(nullptr, lyr_no_sr->GetNextFeature());
}

// ----------------------------------------------------------------------------
// 9. Direct Geometry with SrcRegion and Extent
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, DirectGeometrySrcRegionAndExtent) {
  constexpr char kSqlitePath[] = "/vsimem/ogrvrtlayer_test/direct_srcregion.sqlite";
  GDALDriver* sqlite_drv = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, sqlite_drv);

  {
    std::unique_ptr<GDALDataset> sqlite_ds(
        sqlite_drv->Create(kSqlitePath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, sqlite_ds);
    OGRLayer* sqlite_lyr = sqlite_ds->CreateLayer("points", nullptr, wkbPoint);
    ASSERT_NE(nullptr, sqlite_lyr);
    OGRFeature feat(sqlite_lyr->GetLayerDefn());
    OGRPoint pt(10.0, 10.0);
    feat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, sqlite_lyr->CreateFeature(&feat));
  }

  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"direct_srcregion\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/direct_srcregion.sqlite</SrcDataSource>\n"
      "    <SrcLayer>points</SrcLayer>\n"
      "    <GeometryField name=\"geom\" encoding=\"Direct\">\n"
      "      <SrcRegion clip=\"TRUE\">POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0))</SrcRegion>\n"
      "    </GeometryField>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // Plain SQLite table layers without Spatialite/RTree spatial index do not
  // support fast spatial filtering or fast extents.
  EXPECT_FALSE(lyr->TestCapability(OLCFastSpatialFilter));
  EXPECT_FALSE(lyr->TestCapability(OLCFastGetExtent));

  // Extent on GeometryField allows OLCFastGetExtent to return true.
  constexpr char kVrtStaticExtent[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"direct_static_extent\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/direct_srcregion.sqlite</SrcDataSource>\n"
      "    <SrcLayer>points</SrcLayer>\n"
      "    <GeometryField name=\"geom\" encoding=\"Direct\">\n"
      "      <ExtentXMin>0</ExtentXMin><ExtentYMin>0</ExtentYMin>\n"
      "      <ExtentXMax>20</ExtentXMax><ExtentYMax>20</ExtentYMax>\n"
      "    </GeometryField>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds_ext(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtStaticExtent, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds_ext);
  OGRLayer* lyr_ext = ds_ext->GetLayer(0);
  ASSERT_NE(nullptr, lyr_ext);
  EXPECT_TRUE(lyr_ext->TestCapability(OLCFastGetExtent));
  EXPECT_NE(nullptr, lyr_ext->GetLayerDefn());
  OGREnvelope static_env;
  EXPECT_EQ(OGRERR_NONE, lyr_ext->GetExtent(0, &static_env, TRUE));
  EXPECT_DOUBLE_EQ(0.0, static_env.MinX);
  EXPECT_DOUBLE_EQ(20.0, static_env.MaxX);

  // GetExtent with SrcRegion clip.
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, lyr->GetExtent(0, &env, TRUE));
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(10.0, env.MaxX);

  // Invalid geom field index extent.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE, lyr->GetExtent(1, &env, TRUE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid geometry field index : 1"));
  }

  // Spatial filter with infinite envelope.
  constexpr double kInf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilterRect(-kInf, -kInf, kInf, kInf));
  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  // Spatial filter intersecting SrcRegion.
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilterRect(5, 5, 15, 15));
  feat.reset(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  // Disjoint spatial filter.
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilterRect(50, 50, 60, 60));
  feat.reset(lyr->GetNextFeature());
  EXPECT_EQ(nullptr, feat);
}

// ----------------------------------------------------------------------------
// 10. Shape Geometry Encoding Read and Write
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, ShapeGeometryEncodingReadAndWrite) {
  // Shape point binary format:
  // int32 type (1 = Point) in little-endian, double x (10.0), double y (20.0).
  // Total 20 bytes.
  // Hex string: 01000000 0000000000002440 0000000000003440
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/shape_geom.csv";
  constexpr char kCsvData[] =
      "id,shape_hex\n"
      "1,0100000000000000000024400000000000003440\n";
  VsiMemTempWrapper csv(kCsvPath, kCsvData);

  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"shape_lyr\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/shape_geom.csv</SrcDataSource>\n"
      "    <SrcLayer>shape_geom</SrcLayer>\n"
      "    <GeometryField name=\"geom\" field=\"shape_hex\" encoding=\"Shape\"/>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  OGRGeometry* geom = feat->GetGeomFieldRef(0);
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbPoint, wkbFlatten(geom->getGeometryType()));
  OGRPoint* pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(10.0, pt->getX());
  EXPECT_DOUBLE_EQ(20.0, pt->getY());
}

// ----------------------------------------------------------------------------
// 11. Custom FID Column, GetFeature by FID, and Style Field
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, CustomFIDAndStyleField) {
  constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/custom_fid.csv";
  constexpr char kCsvData[] =
      "my_fid,style,val\n"
      "100,PEN(c:#FF0000),first\n"
      "200,,second\n";
  VsiMemTempWrapper csv(kCsvPath, kCsvData);

  constexpr char kCsvtPath[] = "/vsimem/ogrvrtlayer_test/custom_fid.csvt";
  VsiMemTempWrapper csvt(kCsvtPath, "Integer,String,String\n");

  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"fid_style_lyr\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/custom_fid.csv</SrcDataSource>\n"
      "    <SrcLayer>custom_fid</SrcLayer>\n"
      "    <FID name=\"CUSTOM_FID\">my_fid</FID>\n"
      "    <Style>style</Style>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  EXPECT_STREQ("CUSTOM_FID", lyr->GetFIDColumn());

  // GetFeature(100) using custom FID.
  std::unique_ptr<OGRFeature> f100(lyr->GetFeature(100));
  ASSERT_NE(nullptr, f100);
  EXPECT_EQ(100, f100->GetFID());
  EXPECT_STREQ("PEN(c:#FF0000)", f100->GetStyleString());
  EXPECT_STREQ("first", f100->GetFieldAsString("val"));

  // GetFeature(200) without style string.
  std::unique_ptr<OGRFeature> f200(lyr->GetFeature(200));
  ASSERT_NE(nullptr, f200);
  EXPECT_EQ(200, f200->GetFID());
  EXPECT_STREQ("", f200->GetStyleString());
  EXPECT_STREQ("second", f200->GetFieldAsString("val"));

  // Non-existent feature.
  std::unique_ptr<OGRFeature> f999(lyr->GetFeature(999));
  EXPECT_EQ(nullptr, f999);
}

// ----------------------------------------------------------------------------
// 12. TranslateVRTFeatureToSrcFeature with Multiple Geometry Types & Styles
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, TranslateVRTFeatureToSrcFeatureComprehensive) {
  constexpr char kSqlitePath[] = "/vsimem/ogrvrtlayer_test/trans_all.sqlite";
  GDALDriver* sqlite_drv = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, sqlite_drv);

  {
    std::unique_ptr<GDALDataset> sqlite_ds(
        sqlite_drv->Create(kSqlitePath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, sqlite_ds);
    OGRLayer* lyr = sqlite_ds->CreateLayer("all_types", nullptr, wkbNone);
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn f_style("style_col", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_style));
    OGRFieldDefn f_wkt("wkt_col", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_wkt));
    OGRFieldDefn f_wkb("wkb_col", OFTBinary);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_wkb));
    OGRFieldDefn f_wkb_hex("wkb_hex", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_wkb_hex));
    OGRFieldDefn f_px("px", OFTReal);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_px));
    OGRFieldDefn f_py("py", OFTReal);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_py));
    OGRFieldDefn f_pz("pz", OFTReal);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_pz));
    OGRFieldDefn f_pm("pm", OFTReal);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_pm));
    OGRFieldDefn f_extra("extra_col", OFTString);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&f_extra));
  }

  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"trans_layer\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtlayer_test/trans_all.sqlite</SrcDataSource>\n"
      "    <SrcLayer>all_types</SrcLayer>\n"
      "    <Style>style_col</Style>\n"
      "    <GeometryField name=\"geom_none\" encoding=\"None\"/>\n"
      "    <GeometryField name=\"geom_wkt\" field=\"wkt_col\" encoding=\"WKT\"/>\n"
      "    <GeometryField name=\"geom_wkb\" field=\"wkb_col\" encoding=\"WKB\"/>\n"
      "    <GeometryField name=\"geom_wkb_hex\" field=\"wkb_hex\" encoding=\"WKB\"/>\n"
      "    <GeometryField name=\"geom_pts\" encoding=\"PointFromColumns\" x=\"px\" y=\"py\" z=\"pz\" m=\"pm\"/>\n"
      "    <Field name=\"extra_fld\" src=\"extra_col\" type=\"String\"/>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr);

  // 1. Create a feature writing to all geometry fields.
  OGRFeature feat(lyr->GetLayerDefn());
  feat.SetStyleString("BRUSH(fc:#00FF00)");
  feat.SetField("extra_fld", "my_extra_value");

  OGRPoint pt_wkt(1.0, 2.0);
  feat.SetGeomField(1, &pt_wkt);

  OGRPoint pt_wkb(3.0, 4.0);
  feat.SetGeomField(2, &pt_wkb);

  OGRPoint pt_wkb_hex(5.0, 6.0);
  feat.SetGeomField(3, &pt_wkb_hex);

  OGRPoint pt_xyzm(10.0, 20.0, 30.0, 40.0);
  feat.SetGeomField(4, &pt_xyzm);

  EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
  const GIntBig fid = feat.GetFID();

  // 2. Read back and verify all translated geometries and style.
  std::unique_ptr<OGRFeature> read_feat(lyr->GetFeature(fid));
  ASSERT_NE(nullptr, read_feat);
  EXPECT_STREQ("BRUSH(fc:#00FF00)", read_feat->GetStyleString());
  EXPECT_STREQ("my_extra_value", read_feat->GetFieldAsString("extra_fld"));

  ASSERT_NE(nullptr, read_feat->GetGeomFieldRef(1));
  EXPECT_DOUBLE_EQ(1.0, read_feat->GetGeomFieldRef(1)->toPoint()->getX());

  ASSERT_NE(nullptr, read_feat->GetGeomFieldRef(2));
  EXPECT_DOUBLE_EQ(3.0, read_feat->GetGeomFieldRef(2)->toPoint()->getX());

  ASSERT_NE(nullptr, read_feat->GetGeomFieldRef(3));
  EXPECT_DOUBLE_EQ(5.0, read_feat->GetGeomFieldRef(3)->toPoint()->getX());

  ASSERT_NE(nullptr, read_feat->GetGeomFieldRef(4));
  EXPECT_DOUBLE_EQ(10.0, read_feat->GetGeomFieldRef(4)->toPoint()->getX());
  EXPECT_DOUBLE_EQ(30.0, read_feat->GetGeomFieldRef(4)->toPoint()->getZ());
  EXPECT_DOUBLE_EQ(40.0, read_feat->GetGeomFieldRef(4)->toPoint()->getM());

  // 3. Attempt to set a non-punctual geometry on PointFromColumns (warning).
  OGRLineString line;
  line.addPoint(0, 0);
  line.addPoint(1, 1);
  feat.SetGeomField(4, &line);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_NONE, lyr->SetFeature(&feat));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot set a non ponctual geometry"));
  }
}

// ----------------------------------------------------------------------------
// 13. Additional Error Paths and Edge Cases
// ----------------------------------------------------------------------------

TEST_F(OgrVrtLayerTest, AdditionalErrorPathsAndEdgeCases) {
  OGRVRTDataSource ds(poVrtDriver_);

  // 1. Invalid width (< 0) in FullInitialize.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"bad_width\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <Field name=\"f\" width=\"-5\"/>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid width for field f"));
  }

  // 2. Invalid precision (< 0 or > 1024) in FullInitialize.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"bad_prec\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <Field name=\"f\" precision=\"-5\"/>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid precision for field f"));
  }

  // 3. Invalid FID field name in FullInitialize.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"bad_fid\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <FID>non_existent_fid_col</FID>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify FID field 'non_existent_fid_col'"));
  }

  // 4. Invalid Style field name in FullInitialize.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"bad_style\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <Style>non_existent_style_col</Style>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify Style field 'non_existent_style_col'"));
  }

  // 5. attrFilterPassThrough="FALSE" override attribute on layer.
  {
    constexpr char kCsvPath[] = "/vsimem/ogrvrtlayer_test/attr_override.csv";
    VsiMemTempWrapper csv(kCsvPath, "a\nfoo\nbar\n");
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"attr_override\" attrFilterPassThrough=\"FALSE\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/attr_override.csv</SrcDataSource>\n"
        "    <SrcLayer>attr_override</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> dataset(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, dataset);
    OGRLayer* lyr = dataset->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("a = 'foo'"));
    std::unique_ptr<OGRFeature> feat(lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_STREQ("foo", feat->GetFieldAsString("a"));
    EXPECT_EQ(nullptr, lyr->GetNextFeature());
  }

  // 6. Updatable VRT with FID set rejecting CreateFeature, SetFeature, DeleteFeature.
  {
    constexpr char kSqlitePath[] = "/vsimem/ogrvrtlayer_test/fid_block.sqlite";
    GDALDriver* sqlite_drv = GetGDALDriverManager()->GetDriverByName("SQLite");
    ASSERT_NE(nullptr, sqlite_drv);
    {
      std::unique_ptr<GDALDataset> sqlite_ds(
          sqlite_drv->Create(kSqlitePath, 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, sqlite_ds);
      OGRLayer* l = sqlite_ds->CreateLayer("t", nullptr, wkbNone);
      ASSERT_NE(nullptr, l);
      OGRFieldDefn f("id", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, l->CreateField(&f));
    }
    constexpr char kVrtFidBlock[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"fid_block\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtlayer_test/fid_block.sqlite</SrcDataSource>\n"
        "    <SrcLayer>t</SrcLayer>\n"
        "    <FID>id</FID>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> dataset(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtFidBlock, GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, dataset);
    OGRLayer* lyr = dataset->GetLayer(0);
    ASSERT_NE(nullptr, lyr);

    OGRFeature feat(lyr->GetLayerDefn());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(OGRERR_FAILURE, lyr->CreateFeature(&feat));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("CreateFeature() operation is not supported if the FID option is specified"));
    EXPECT_EQ(OGRERR_FAILURE, lyr->SetFeature(&feat));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("SetFeature() operation is not supported if the FID option is specified"));
    EXPECT_EQ(OGRERR_FAILURE, lyr->DeleteFeature(1));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("DeleteFeature() operation is not supported if the FID option is specified"));
  }

  // 7. GeometryField with SRS in FastInitialize + FullInitialize, and 2nd GeometryField without name / invalid GeometryType / invalid SRS.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"geom_srs_valid\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <GeometryField name=\"g1\" encoding=\"None\" SRS=\"EPSG:4326\"/>\n"
        "  <GeometryField encoding=\"None\" SRS=\"EPSG:4326\"/>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    ASSERT_NE(nullptr, layer.GetLayerDefn());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("A 'name' attribute should be defined"));
  }
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"geom2_bad_gtype\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <GeometryField name=\"g1\" encoding=\"None\"/>\n"
        "  <GeometryField name=\"g2\" encoding=\"None\" GeometryType=\"BadType\"/>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GeometryType BadType not recognised"));
  }
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"geom2_bad_srs\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <GeometryField name=\"g1\" encoding=\"None\"/>\n"
        "  <GeometryField name=\"g2\" encoding=\"None\" SRS=\"INVALID_SRS_STR\"/>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import SRS"));
  }
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTLayer name=\"direct_missing_field\">\n"
        "  <SrcDataSource>@dummy@</SrcDataSource>\n"
        "  <SrcLayer>@dummy@</SrcLayer>\n"
        "  <GeometryField name=\"g1\" encoding=\"Direct\" field=\"non_existent_geom\"/>\n"
        "</OGRVRTLayer>"));
    OGRVRTLayer layer(&ds);
    ASSERT_TRUE(layer.FastInitialize(xml.get(), kTestDir, FALSE));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, layer.GetNextFeature());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify source geometry field 'non_existent_geom'"));
  }

  // 8. Uninitialized layer methods when FullInitialize fails (missing source).
  {
    auto make_bad_layer = [&](int bUpdate) {
      auto xml = std::make_unique<CPLXMLTreeCloser>(CPLParseXMLString(
          "<OGRVRTLayer name=\"uninit_fail\">\n"
          "  <SrcDataSource relativeToVRT=\"1\">missing_src.sqlite</SrcDataSource>\n"
          "  <SrcLayer>none</SrcLayer>\n"
          "  <FeatureCount>42</FeatureCount>\n"
          "</OGRVRTLayer>"));
      auto layer = std::make_unique<OGRVRTLayer>(&ds);
      EXPECT_TRUE(layer->FastInitialize(xml->get(), kTestDir, bUpdate));
      return std::make_pair(std::move(xml), std::move(layer));
    };

    WithQuietHandler quiet;
    // FeatureCount fast capability before FullInitialize:
    {
      auto [xml, layer] = make_bad_layer(TRUE);
      EXPECT_TRUE(layer->TestCapability(OLCFastFeatureCount));
      // Trigger FullInitialize in update mode -> falls back to read-only -> fails:
      EXPECT_EQ(nullptr, layer->GetFeature(1));
      EXPECT_FALSE(layer->TestCapability(OLCRandomRead));
      EXPECT_EQ(42, layer->GetFeatureCount(TRUE));
      EXPECT_STREQ("", layer->GetFIDColumn());
      EXPECT_EQ(wkbNone, layer->GetGeomType());
      EXPECT_EQ(OGRERR_FAILURE, layer->SetIgnoredFields(nullptr));
    }
    {
      auto [xml, layer] = make_bad_layer(FALSE);
      EXPECT_EQ(OGRERR_FAILURE, layer->SetNextByIndex(0));
    }
    {
      auto [xml, layer] = make_bad_layer(TRUE);
      OGRFeatureDefnBuilder defn_builder("d");
      OGRFeatureDefn* dummy_defn = defn_builder.Build();
      absl::Cleanup cl = [dummy_defn] { dummy_defn->Release(); };
      OGRFeature dummy(dummy_defn);
      EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&dummy));
      EXPECT_EQ(OGRERR_FAILURE, layer->SetFeature(&dummy));
      EXPECT_EQ(OGRERR_FAILURE, layer->DeleteFeature(1));
    }
    {
      auto [xml, layer] = make_bad_layer(FALSE);
      OGREnvelope env;
      EXPECT_EQ(OGRERR_FAILURE, layer->GetExtent(0, &env, TRUE));
      EXPECT_EQ(OGRERR_FAILURE, layer->SetSpatialFilter(nullptr));
      EXPECT_EQ(OGRERR_FAILURE, layer->SetAttributeFilter("1=1"));
    }
    {
      auto [xml, layer] = make_bad_layer(TRUE);
      EXPECT_EQ(OGRERR_FAILURE, layer->StartTransaction());
      EXPECT_EQ(OGRERR_FAILURE, layer->CommitTransaction());
      EXPECT_EQ(OGRERR_FAILURE, layer->RollbackTransaction());
      EXPECT_EQ(nullptr, layer->GetSrcDataset());
    }
  }
  // TODO: b/335317901 - Cover CSV:z:/ Windows drive-letter branch, VGS_Shape
  // write no-op, and MaxX/MaxY = -inf branches in OGRVRTLayer.
}

}  // namespace
}  // namespace autotest2
