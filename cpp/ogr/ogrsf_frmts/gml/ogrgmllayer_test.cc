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
// Tests for OGRGMLLayer (ogrgmllayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/ogrgmllayer.cpp

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/ogr_gml.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

class OGRGMLLayerTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRGML(); }
};

TEST_F(OGRGMLLayerTest, WriterDisallowsReadAndReaderDisallowsWrite) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/writer_rw.gml";
  VsiMemTempWrapper file(kPath, "");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  // 1. Writer layer rejects read operations.
  {
    std::unique_ptr<GDALDataset> ds(
        driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layer = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer);

    layer->ResetReading();  // Does nothing when bWriter is true.

    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, layer->GetNextFeature());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    EXPECT_NE(nullptr, layer->GetDataset());
  }

  // 2. Reader layer rejects write operations.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);

    OGRFieldDefn field_defn("new_field", OFTInteger);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateField(&field_defn));

    OGRGeomFieldDefn geom_defn("new_geom", wkbPoint);
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateGeomField(&geom_defn));

    OGRFeature feat(layer->GetLayerDefn());
    EXPECT_EQ(OGRERR_FAILURE, layer->CreateFeature(&feat));
  }
}

TEST_F(OGRGMLLayerTest, WriterCreateFieldAndGeomFieldValidations) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/writer_fields.gml";
  VsiMemTempWrapper file(kPath, "");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  // Field with invalid XML element name:
  OGRFieldDefn bad_field("123 invalid name", OFTInteger);
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE,
              layer->CreateField(&bad_field, /*bApproxOK=*/FALSE));
  }
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_NONE, layer->CreateField(&bad_field, /*bApproxOK=*/TRUE));
  }

  // GeomField with invalid XML element name:
  OGRGeomFieldDefn bad_geom("456 invalid geom", wkbPoint);
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE,
              layer->CreateGeomField(&bad_geom, /*bApproxOK=*/FALSE));
  }
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_NONE,
              layer->CreateGeomField(&bad_geom, /*bApproxOK=*/TRUE));
  }

  // Write a feature:
  OGRFeature feat(layer->GetLayerDefn());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));

  // After feature is written (m_iNextGMLId != 0), creating fields is refused:
  OGRFieldDefn extra_field("extra", OFTString);
  EXPECT_EQ(OGRERR_FAILURE, layer->CreateField(&extra_field));

  OGRGeomFieldDefn extra_geom("extra_geom", wkbPolygon);
  EXPECT_EQ(OGRERR_FAILURE, layer->CreateGeomField(&extra_geom));
}

TEST_F(OGRGMLLayerTest, WriterGML2VariantsAndNonLinearGeometries) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/gml2.gml";
  VsiMemTempWrapper file(kPath, "");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {"FORMAT=GML2", nullptr};

  CPLSetConfigOption("GML_USE_OLD_FID_FORMAT", "TRUE");
  absl::Cleanup reset_opt = [] {
    CPLSetConfigOption("GML_USE_OLD_FID_FORMAT", nullptr);
  };

  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(options)));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("gml2_layer", nullptr, wkbUnknown, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn fid_field("fid", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&fid_field));

  // 1. Feature with auto-FID and non-linear geometry (CircularString -> forced
  // to LineString).
  {
    OGRFeature feat(layer->GetLayerDefn());
    OGRCircularString curve;
    curve.addPoint(0, 0);
    curve.addPoint(1, 1);
    curve.addPoint(2, 0);
    EXPECT_EQ(OGRERR_NONE, feat.SetGeometry(&curve));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // 2. Feature with explicit fid and wkbTriangle.
  {
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("fid", "custom.fid.1");
    OGRTriangle triangle;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(1, 0);
    ring.addPoint(0, 1);
    ring.addPoint(0, 0);
    triangle.addRing(&ring);
    EXPECT_EQ(OGRERR_NONE, feat.SetGeometry(&triangle));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
  }

  // 3. Dataset without GML_USE_OLD_FID_FORMAT to hit lines 757-767:
  {
    constexpr char kPath2[] = "/vsimem/ogrgmllayer_test/gml2_standard_fid.gml";
    VsiMemTempWrapper file2(kPath2, "");
    CPLSetConfigOption("GML_USE_OLD_FID_FORMAT", "FALSE");
    std::unique_ptr<GDALDataset> ds2(driver->Create(
        kPath2, 0, 0, 0, GDT_Unknown, const_cast<char**>(options)));
    ASSERT_NE(nullptr, ds2);
    OGRLayer* layer2 =
        ds2->CreateLayer("gml2_layer2", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, layer2);

    OGRFieldDefn fid_field2("fid", OFTString);
    EXPECT_EQ(OGRERR_NONE, layer2->CreateField(&fid_field2));

    // A. Feature with fid field set: hits lines 760-762:
    OGRFeature feat_with_fid(layer2->GetLayerDefn());
    feat_with_fid.SetField("fid", "explicit_fid_val");
    OGRPoint pt1(1, 2);
    EXPECT_EQ(OGRERR_NONE, feat_with_fid.SetGeometry(&pt1));
    EXPECT_EQ(OGRERR_NONE, layer2->CreateFeature(&feat_with_fid));

    // B. Feature with fid field unset: hits lines 765-767:
    OGRFeature feat_no_fid(layer2->GetLayerDefn());
    OGRPoint pt2(3, 4);
    EXPECT_EQ(OGRERR_NONE, feat_no_fid.SetGeometry(&pt2));
    EXPECT_EQ(OGRERR_NONE, layer2->CreateFeature(&feat_no_fid));
  }
}

TEST_F(OGRGMLLayerTest, WriterGML3AndGML32VariantsWithPrecisionAndSwappedSRS) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/gml3.gml";
  VsiMemTempWrapper file(kPath, "");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {"FORMAT=GML3.2",
                                 "GML_FEATURE_COLLECTION=NO",
                                 "SPACE_INDENTATION=NO",
                                 "STRIP_PREFIX=YES",
                                 "SRSNAME_FORMAT=OGC_URN",
                                 "SRSDIMENSION_LOC=GEOMETRY",
                                 "WRITE_FEATURE_BOUNDED_BY=YES",
                                 nullptr};

  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(options)));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs_latlong;
  srs_latlong.importFromEPSG(4326);

  OGRLayer* layer =
      ds->CreateLayer("gml3_layer", &srs_latlong, wkbPoint25D, nullptr);
  ASSERT_NE(nullptr, layer);

  // Add a second geometry field to test multi-geom export under GML3.2.
  OGRGeomFieldDefn geom2("geom2", wkbTriangle);
  geom2.SetSpatialRef(&srs_latlong);
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.01;
  prec.dfZResolution = 0.1;
  geom2.SetCoordinatePrecision(prec);
  EXPECT_EQ(OGRERR_NONE, layer->CreateGeomField(&geom2));

  OGRFieldDefn gml_id_field("gml_id", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&gml_id_field));

  OGRFeature feat(layer->GetLayerDefn());
  feat.SetField("gml_id", "custom_id.99");

  OGRPoint pt(10.12345, 20.6789, 5.0);
  EXPECT_EQ(OGRERR_NONE, feat.SetGeometry(&pt));

  OGRTriangle tri;
  OGRLinearRing ring;
  ring.addPoint(10.1, 20.1, 1.0);
  ring.addPoint(10.2, 20.1, 1.0);
  ring.addPoint(10.1, 20.2, 1.0);
  ring.addPoint(10.1, 20.1, 1.0);
  tri.addRing(&ring);
  EXPECT_EQ(OGRERR_NONE, feat.SetGeomField(1, &tri));

  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));

  // Single-geom layer with unprojected/planar SRS to exercise lines 840-851 &
  // 889-892:
  OGRSpatialReference srs_utm;
  srs_utm.importFromEPSG(32631);

  OGRLayer* layer2 =
      ds->CreateLayer("gml3_single_geom", &srs_utm, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer2);

  OGRFieldDefn bool_list_field("bool_list", OFTInteger64List);
  bool_list_field.SetSubType(OFSTBoolean);
  EXPECT_EQ(OGRERR_NONE, layer2->CreateField(&bool_list_field));

  OGRFeature feat2(layer2->GetLayerDefn());
  const GIntBig vals[] = {0, 1};
  feat2.SetField("bool_list", 2, vals);
  OGRPoint pt2(500000, 4000000);
  EXPECT_EQ(OGRERR_NONE, feat2.SetGeometry(&pt2));
  EXPECT_EQ(OGRERR_NONE, layer2->CreateFeature(&feat2));

  // Exercise SRSNAME_FORMAT=SHORT and SRSNAME_FORMAT=OGC_URL (lines 867-874):
  {
    constexpr char kPathShort[] = "/vsimem/ogrgmllayer_test/gml3_short.gml";
    VsiMemTempWrapper fileShort(kPathShort, "");
    const char* const optsShort[] = {"FORMAT=GML3", "SRSNAME_FORMAT=SHORT",
                                     nullptr};
    std::unique_ptr<GDALDataset> dsShort(driver->Create(
        kPathShort, 0, 0, 0, GDT_Unknown, const_cast<char**>(optsShort)));
    ASSERT_NE(nullptr, dsShort);
    OGRLayer* lShort =
        dsShort->CreateLayer("layer_short", &srs_utm, wkbPoint, nullptr);
    ASSERT_NE(nullptr, lShort);
    OGRFeature fShort(lShort->GetLayerDefn());
    OGRPoint pt(1, 2);
    EXPECT_EQ(OGRERR_NONE, fShort.SetGeometry(&pt));
    EXPECT_EQ(OGRERR_NONE, lShort->CreateFeature(&fShort));
  }
  {
    constexpr char kPathUrl[] = "/vsimem/ogrgmllayer_test/gml3_url.gml";
    VsiMemTempWrapper fileUrl(kPathUrl, "");
    const char* const optsUrl[] = {"FORMAT=GML3", "SRSNAME_FORMAT=OGC_URL",
                                   nullptr};
    std::unique_ptr<GDALDataset> dsUrl(driver->Create(
        kPathUrl, 0, 0, 0, GDT_Unknown, const_cast<char**>(optsUrl)));
    ASSERT_NE(nullptr, dsUrl);
    OGRLayer* lUrl =
        dsUrl->CreateLayer("layer_url", &srs_utm, wkbPoint, nullptr);
    ASSERT_NE(nullptr, lUrl);
    OGRFeature fUrl(lUrl->GetLayerDefn());
    OGRPoint pt(1, 2);
    EXPECT_EQ(OGRERR_NONE, fUrl.SetGeometry(&pt));
    EXPECT_EQ(OGRERR_NONE, lUrl->CreateFeature(&fUrl));
  }
}

TEST_F(OGRGMLLayerTest, WriterAllFieldTypesAndNullValues) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/fields.gml";
  VsiMemTempWrapper file(kPath, "");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {"FORMAT=GML3", nullptr};
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(options)));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("all_fields", nullptr, wkbNone, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn f_str("f_str", OFTString);
  OGRFieldDefn f_null("f_null", OFTString);
  OGRFieldDefn f_str_list("f_str_list", OFTStringList);
  OGRFieldDefn f_int_list("f_int_list", OFTIntegerList);
  OGRFieldDefn f_bool_list("f_bool_list", OFTIntegerList);
  f_bool_list.SetSubType(OFSTBoolean);
  OGRFieldDefn f_i64_list("f_i64_list", OFTInteger64List);
  OGRFieldDefn f_i64_bool_list("f_i64_bool_list", OFTInteger64List);
  f_i64_bool_list.SetSubType(OFSTBoolean);
  OGRFieldDefn f_real_list("f_real_list", OFTRealList);
  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  OGRFieldDefn f_date("f_date", OFTDate);
  OGRFieldDefn f_datetime("f_datetime", OFTDateTime);

  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_str));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_null));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_str_list));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_int_list));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_bool_list));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_i64_list));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_i64_bool_list));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_real_list));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_date));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_datetime));

  OGRFeature feat(layer->GetLayerDefn());
  feat.SetField("f_str", "   <escaped & text>   ");
  feat.SetFieldNull(feat.GetFieldIndex("f_null"));

  const char* const str_list[] = {"item1", "<item2>", nullptr};
  feat.SetField("f_str_list", str_list);

  const int int_list[] = {10, 20};
  feat.SetField("f_int_list", 2, int_list);

  const int bool_list[] = {1, 0};
  feat.SetField("f_bool_list", 2, bool_list);

  const GIntBig i64_list[] = {10000000000LL, 20000000000LL};
  feat.SetField("f_i64_list", 2, i64_list);

  const GIntBig i64_bool_list[] = {0, 1};
  feat.SetField("f_i64_bool_list", 2, i64_bool_list);

  const double real_list[] = {1.5, 2.75};
  feat.SetField("f_real_list", 2, real_list);

  feat.SetField("f_bool", 1);
  feat.SetField("f_date", 2026, 11, 3, 0, 0, 0, 0);
  feat.SetField("f_datetime", 2026, 11, 3, 12, 34, 56, 0);

  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(&feat));
}

TEST_F(OGRGMLLayerTest, WriterAndReaderTestCapability) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/cap.gml";
  VsiMemTempWrapper file(kPath, "");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GML");
  ASSERT_NE(nullptr, driver);

  const char* const options[] = {"FORMAT=GML3", nullptr};
  std::unique_ptr<GDALDataset> ds(
      driver->Create(kPath, 0, 0, 0, GDT_Unknown, const_cast<char**>(options)));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("cap_layer", nullptr, wkbPoint25D, nullptr);
  ASSERT_NE(nullptr, layer);

  EXPECT_TRUE(layer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(layer->TestCapability(OLCCreateField));
  EXPECT_TRUE(layer->TestCapability(OLCCreateGeomField));
  EXPECT_FALSE(layer->TestCapability(OLCFastGetExtent));
  EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(layer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(layer->TestCapability(OLCCurveGeometries));
  EXPECT_TRUE(layer->TestCapability(OLCZGeometries));
  EXPECT_FALSE(layer->TestCapability("NonExistentCap"));
}

constexpr std::string_view kMultiLayerGml =
    R"xml(<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <LayerA fid="f.10">
      <propA>alpha1</propA>
      <gml:Point><gml:coordinates>10,20</gml:coordinates></gml:Point>
    </LayerA>
  </gml:featureMember>
  <gml:featureMember>
    <LayerB fid="f.20">
      <propB>beta1</propB>
    </LayerB>
  </gml:featureMember>
  <gml:featureMember>
    <LayerA fid="f.10">
      <propA>alpha2_dup_fid</propA>
      <gml:Point><gml:coordinates>11,21</gml:coordinates></gml:Point>
    </LayerA>
  </gml:featureMember>
  <gml:featureMember>
    <LayerA fid="other.30">
      <propA>alpha3_mismatch_prefix</propA>
      <gml:Point><gml:coordinates>12,22</gml:coordinates></gml:Point>
    </LayerA>
  </gml:featureMember>
  <gml:featureMember>
    <LayerA fid="f.non_numeric">
      <propA>alpha4_bad_suffix</propA>
      <gml:Point><gml:coordinates>13,23</gml:coordinates></gml:Point>
    </LayerA>
  </gml:featureMember>
</root>)xml";

constexpr std::string_view kMultiLayerGfs = R"xml(<GMLFeatureClassList>
  <GMLFeatureClass>
    <Name>LayerA</Name>
    <ElementPath>LayerA</ElementPath>
    <PropertyDefn>
      <Name>propA</Name>
      <ElementPath>propA</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
    <GeomPropertyDefn>
      <Name>geometry</Name>
      <ElementPath></ElementPath>
      <Type>Point</Type>
    </GeomPropertyDefn>
  </GMLFeatureClass>
  <GMLFeatureClass>
    <Name>LayerB</Name>
    <ElementPath>LayerB</ElementPath>
    <PropertyDefn>
      <Name>propB</Name>
      <ElementPath>propB</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
  </GMLFeatureClass>
</GMLFeatureClassList>)xml";

TEST_F(OGRGMLLayerTest, ReaderMultiLayerAndFIDParsingVariants) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmllayer_test/multilayer.gml";
  constexpr char kGfsPath[] = "/vsimem/ogrgmllayer_test/multilayer.gfs";
  VsiMemTempWrapper f_gml(kGmlPath, std::string(kMultiLayerGml));
  VsiMemTempWrapper f_gfs(kGfsPath, std::string(kMultiLayerGfs));

  // 1. Test READ_MODE=STANDARD: exercises ResetReading with multiple layers.
  {
    const char* const open_opts[] = {"READ_MODE=STANDARD", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetLayerCount());

    OGRLayer* layerA = ds->GetLayerByName("LayerA");
    ASSERT_NE(nullptr, layerA);
    layerA->ResetReading();

    int countA = 0;
    while (std::unique_ptr<OGRFeature> feat{layerA->GetNextFeature()}) {
      countA++;
    }
    EXPECT_EQ(4, countA);

    OGRLayer* layerB = ds->GetLayerByName("LayerB");
    ASSERT_NE(nullptr, layerB);
    layerB->ResetReading();

    int countB = 0;
    while (std::unique_ptr<OGRFeature> feat{layerB->GetNextFeature()}) {
      countB++;
    }
    EXPECT_EQ(1, countB);

    // Exercise ResetReading with pipe in ElementName:
    auto* gmlDS = dynamic_cast<OGRGMLDataSource*>(ds.get());
    if (gmlDS != nullptr && gmlDS->GetReader() != nullptr &&
        gmlDS->GetReader()->GetClass(0) != nullptr) {
      gmlDS->GetReader()->GetClass(0)->SetElementName("prefix|LayerA");
      layerA->ResetReading();
      EXPECT_STREQ("LayerA", gmlDS->GetReader()->GetFilteredClassName());
    }
  }

  // 2. Test READ_MODE=INTERLEAVED_LAYERS:
  {
    const char* const open_opts[] = {"READ_MODE=INTERLEAVED_LAYERS", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layerA = ds->GetLayerByName("LayerA");
    OGRLayer* layerB = ds->GetLayerByName("LayerB");
    ASSERT_NE(nullptr, layerA);
    ASSERT_NE(nullptr, layerB);

    // Read 1 from A:
    std::unique_ptr<OGRFeature> featA1(layerA->GetNextFeature());
    ASSERT_NE(nullptr, featA1);

    // Call layerA->GetNextFeature() again: encounters LayerB, stores it,
    // returns nullptr (lines 169-174):
    EXPECT_EQ(nullptr, layerA->GetNextFeature());

    // Switch to B: ResetReading while stored feature belongs to LayerB (lines
    // 80-82):
    layerB->ResetReading();
    // PeekStoredGMLFeature peeks LayerB (lines 145-148):
    std::unique_ptr<OGRFeature> featB1(layerB->GetNextFeature());
    ASSERT_NE(nullptr, featB1);

    // Reset A and read again:
    layerA->ResetReading();
    std::unique_ptr<OGRFeature> featA_reset(layerA->GetNextFeature());
    ASSERT_NE(nullptr, featA_reset);
  }

  // 3. Test READ_MODE=SEQUENTIAL_LAYERS:
  {
    const char* const open_opts[] = {"READ_MODE=SEQUENTIAL_LAYERS", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layerA = ds->GetLayerByName("LayerA");
    OGRLayer* layerB = ds->GetLayerByName("LayerB");
    ASSERT_NE(nullptr, layerA);
    ASSERT_NE(nullptr, layerB);

    std::unique_ptr<OGRFeature> featA1(layerA->GetNextFeature());
    ASSERT_NE(nullptr, featA1);
    // Encounter LayerB with m_iNextGMLId != 0: returns nullptr and stores
    // feature
    EXPECT_EQ(nullptr, layerA->GetNextFeature());

    std::unique_ptr<OGRFeature> featB1(layerB->GetNextFeature());
    ASSERT_NE(nullptr, featB1);
  }
}

constexpr std::string_view kAllTypesGml =
    R"xml(<root xmlns:gml="http://www.opengis.net/gml" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <gml:featureMember>
    <Item fid="item.1">
      <f_real>123.456</f_real>
      <f_int_list>10 20 30</f_int_list>
      <f_i64_list>10000000000 20000000000</f_i64_list>
      <f_real_list>1.25 3.75</f_real_list>
      <f_str_list>one</f_str_list>
      <f_str_list>two</f_str_list>
      <f_feat_prop_list>#c1</f_feat_prop_list>
      <f_bool_t>true</f_bool_t>
      <f_bool_1>1</f_bool_1>
      <f_bool_f>false</f_bool_f>
      <f_bool_0>0</f_bool_0>
      <f_bool_invalid>unknown</f_bool_invalid>
      <f_bool_list>true 0</f_bool_list>
      <f_nil xsi:nil="true"/>
      <gml:Point><gml:coordinates>10,20</gml:coordinates></gml:Point>
    </Item>
  </gml:featureMember>
</root>)xml";

constexpr std::string_view kAllTypesGfs = R"xml(<GMLFeatureClassList>
  <GMLFeatureClass>
    <Name>Item</Name>
    <ElementPath>Item</ElementPath>
    <DatasetSpecificInfo>
      <FeatureCount>1</FeatureCount>
      <ExtentXMin>10.0</ExtentXMin>
      <ExtentXMax>10.0</ExtentXMax>
      <ExtentYMin>20.0</ExtentYMin>
      <ExtentYMax>20.0</ExtentYMax>
    </DatasetSpecificInfo>
    <PropertyDefn>
      <Name>f_real</Name>
      <ElementPath>f_real</ElementPath>
      <Type>Real</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_int_list</Name>
      <ElementPath>f_int_list</ElementPath>
      <Type>IntegerList</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_i64_list</Name>
      <ElementPath>f_i64_list</ElementPath>
      <Type>IntegerList</Type>
      <Subtype>Integer64</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_real_list</Name>
      <ElementPath>f_real_list</ElementPath>
      <Type>RealList</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_str_list</Name>
      <ElementPath>f_str_list</ElementPath>
      <Type>StringList</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_feat_prop_list</Name>
      <ElementPath>f_feat_prop_list</ElementPath>
      <Type>FeaturePropertyList</Type>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_bool_t</Name>
      <ElementPath>f_bool_t</ElementPath>
      <Type>String</Type>
      <Subtype>Boolean</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_bool_1</Name>
      <ElementPath>f_bool_1</ElementPath>
      <Type>String</Type>
      <Subtype>Boolean</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_bool_f</Name>
      <ElementPath>f_bool_f</ElementPath>
      <Type>String</Type>
      <Subtype>Boolean</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_bool_0</Name>
      <ElementPath>f_bool_0</ElementPath>
      <Type>String</Type>
      <Subtype>Boolean</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_bool_invalid</Name>
      <ElementPath>f_bool_invalid</ElementPath>
      <Type>String</Type>
      <Subtype>Boolean</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_bool_list</Name>
      <ElementPath>f_bool_list</ElementPath>
      <Type>StringList</Type>
      <Subtype>Boolean</Subtype>
    </PropertyDefn>
    <PropertyDefn>
      <Name>f_nil</Name>
      <ElementPath>f_nil</ElementPath>
      <Type>String</Type>
    </PropertyDefn>
    <GeomPropertyDefn>
      <Name>geometry</Name>
      <ElementPath></ElementPath>
      <Type>Point</Type>
    </GeomPropertyDefn>
  </GMLFeatureClass>
</GMLFeatureClassList>)xml";

TEST_F(OGRGMLLayerTest, ReaderAllFieldTypesAndGetExtentAndFeatureCount) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmllayer_test/alltypes.gml";
  constexpr char kGfsPath[] = "/vsimem/ogrgmllayer_test/alltypes.gfs";
  VsiMemTempWrapper f_gml(kGmlPath, std::string(kAllTypesGml));
  VsiMemTempWrapper f_gfs(kGfsPath, std::string(kAllTypesGfs));

  const char* const open_opts[] = {"EXPOSE_GML_ID=YES", nullptr};
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);

  // Fast feature count & extent from .gfs:
  EXPECT_TRUE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(1, layer->GetFeatureCount(/*bForce=*/FALSE));

  EXPECT_TRUE(layer->TestCapability(OLCFastGetExtent));
  OGREnvelope ext;
  EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&ext, /*bForce=*/FALSE));
  EXPECT_DOUBLE_EQ(10.0, ext.MinX);
  EXPECT_DOUBLE_EQ(20.0, ext.MinY);

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  EXPECT_DOUBLE_EQ(123.456, feat->GetFieldAsDouble("f_real"));
  EXPECT_EQ(1, feat->GetFieldAsInteger("f_bool_t"));
  EXPECT_EQ(1, feat->GetFieldAsInteger("f_bool_1"));
  EXPECT_EQ(0, feat->GetFieldAsInteger("f_bool_f"));
  EXPECT_EQ(0, feat->GetFieldAsInteger("f_bool_0"));
  EXPECT_EQ(0, feat->GetFieldAsInteger("f_bool_invalid"));
  EXPECT_TRUE(feat->IsFieldNull(feat->GetFieldIndex("f_nil")));

  // Attribute filter:
  layer->SetAttributeFilter("f_real > 100");
  EXPECT_EQ(1, layer->GetFeatureCount(/*bForce=*/TRUE));
  layer->SetAttributeFilter("f_real < 100");
  EXPECT_EQ(0, layer->GetFeatureCount(/*bForce=*/TRUE));
  layer->SetAttributeFilter(nullptr);
}

constexpr std::string_view kMultiGeomGml =
    R"xml(<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <Item fid="item.1">
      <pt1><gml:Point><gml:coordinates>1,1</gml:coordinates></gml:Point></pt1>
      <pt2><gml:Point><gml:coordinates>2,2</gml:coordinates></gml:Point></pt2>
    </Item>
  </gml:featureMember>
  <gml:featureMember>
    <Item fid="item.2">
      <pt1><gml:Point><gml:coordinates>10,10</gml:coordinates></gml:Point></pt1>
      <pt2><gml:Point><gml:coordinates>20,20</gml:coordinates></gml:Point></pt2>
    </Item>
  </gml:featureMember>
</root>)xml";

constexpr std::string_view kMultiGeomGfs = R"xml(<GMLFeatureClassList>
  <GMLFeatureClass>
    <Name>Item</Name>
    <ElementPath>Item</ElementPath>
    <GeomPropertyDefn>
      <Name>pt1</Name>
      <ElementPath>pt1</ElementPath>
      <Type>Point</Type>
    </GeomPropertyDefn>
    <GeomPropertyDefn>
      <Name>pt2</Name>
      <ElementPath>pt2</ElementPath>
      <Type>Point</Type>
    </GeomPropertyDefn>
  </GMLFeatureClass>
</GMLFeatureClassList>)xml";

TEST_F(OGRGMLLayerTest, ReaderMultiGeometryAndSpatialFilters) {
  constexpr char kGmlPath[] = "/vsimem/ogrgmllayer_test/multigeom.gml";
  constexpr char kGfsPath[] = "/vsimem/ogrgmllayer_test/multigeom.gfs";
  VsiMemTempWrapper f_gml(kGmlPath, std::string(kMultiGeomGml));
  VsiMemTempWrapper f_gfs(kGfsPath, std::string(kMultiGeomGfs));

  const char* const open_opts[] = {"EXPOSE_GML_ID=YES", nullptr};
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kGmlPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  ASSERT_EQ(2, layer->GetLayerDefn()->GetGeomFieldCount());

  // Filter on geom field 1 (pt2):
  layer->SetSpatialFilterRect(1, 0, 0, 5, 5);
  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_STREQ("item.1", feat->GetFieldAsString("gml_id"));
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  layer->SetSpatialFilter(nullptr);
}

constexpr std::string_view kCorruptedGeomGml =
    R"xml(<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <Item fid="item.1">
      <gml:Point><gml:coordinates>invalid_coords</gml:coordinates></gml:Point>
      <prop>val1</prop>
    </Item>
  </gml:featureMember>
  <gml:featureMember>
    <Item fid="item.2">
      <gml:Point><gml:coordinates>10,20</gml:coordinates></gml:Point>
      <prop>val2</prop>
    </Item>
  </gml:featureMember>
</root>)xml";

TEST_F(OGRGMLLayerTest, ReaderCorruptedGeometryHandling) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/corrupt.gml";
  VsiMemTempWrapper file(kPath, std::string(kCorruptedGeomGml));

  // 1. SKIP_CORRUPTED_FEATURES=NO (default): stops reading.
  {
    WithQuietHandler quiet_handler;
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(nullptr, layer->GetNextFeature());
  }

  // 2. SKIP_CORRUPTED_FEATURES=YES: skips to next feature.
  {
    WithQuietHandler quiet_handler;
    const char* const open_opts[] = {"SKIP_CORRUPTED_FEATURES=YES", nullptr};
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);

    std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_STREQ("val2", feat->GetFieldAsString("prop"));
  }
}

constexpr std::string_view kBoundedByFallbackGml =
    R"xml(<root xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <Item fid="item.1">
      <gml:boundedBy>
        <gml:Envelope srsName="EPSG:4326">
          <lowerCorner>10 20</lowerCorner>
          <upperCorner>30 40</upperCorner>
        </gml:Envelope>
      </gml:boundedBy>
      <prop>has_bbox</prop>
    </Item>
  </gml:featureMember>
  <gml:featureMember>
    <Item fid="item.2">
      <gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>0,0 0,1 1,1 1,0 0,0</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon>
      <prop>has_geom</prop>
    </Item>
  </gml:featureMember>
</root>)xml";

TEST_F(OGRGMLLayerTest, ReaderBoundedByFallback) {
  constexpr char kPath[] = "/vsimem/ogrgmllayer_test/bbox_fallback.gml";
  VsiMemTempWrapper file(kPath, std::string(kBoundedByFallbackGml));

  const char* const open_opts[] = {"USE_BBOX=YES", nullptr};
  std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
      kPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, open_opts));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);

  std::unique_ptr<OGRFeature> feat(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  ASSERT_NE(nullptr, feat->GetGeometryRef());
  EXPECT_EQ(wkbPolygon, wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
}

}  // namespace
}  // namespace autotest2
