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
// Tests the OGR VRT data source class, warped layers, union layers, and
// proxied layer pool (ogrvrtdatasource.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/vrt/ogrvrtdatasource.cpp
//   https://gdal.org/drivers/vector/vrt.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_vrt.py

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/vrt/ogr_vrt.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2::IsSameAs;
using ::testing::Contains;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrvrtdatasource_test";

class OgrVrtDataSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRVRT();
    RegisterOGRCSV();
    poDriver_ = GetGDALDriverManager()->GetDriverByName("OGR_VRT");
    ASSERT_NE(nullptr, poDriver_);
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
  }

  GDALDriver* poDriver_ = nullptr;
};

TEST_F(OgrVrtDataSourceTest, BasicAccessorsAndLifecycle) {
  OGRVRTDataSource ds(poDriver_);

  // Initial state.
  EXPECT_EQ(0, ds.GetLayerCount());
  EXPECT_EQ(nullptr, ds.GetLayer(-1));
  EXPECT_EQ(nullptr, ds.GetLayer(0));
  EXPECT_EQ(nullptr, ds.GetLayer(10));
  EXPECT_EQ(0, ds.CloseDependentDatasets());

  // Call level and recursion.
  EXPECT_EQ(0, ds.GetCallLevel());
  ds.SetCallLevel(5);
  EXPECT_EQ(5, ds.GetCallLevel());

  EXPECT_EQ(nullptr, ds.GetParentDS());
  OGRVRTDataSource parent_ds(poDriver_);
  ds.SetParentDS(&parent_ds);
  EXPECT_EQ(&parent_ds, ds.GetParentDS());

  EXPECT_FALSE(ds.GetRecursionDetected());
  ds.SetRecursionDetected();
  EXPECT_TRUE(ds.GetRecursionDetected());

  // Forbidden names.
  EXPECT_FALSE(ds.IsInForbiddenNames("foo.vrt"));
  ds.AddForbiddenNames("foo.vrt");
  EXPECT_TRUE(ds.IsInForbiddenNames("foo.vrt"));

  // Capabilities.
  EXPECT_TRUE(ds.TestCapability(ODsCCurveGeometries));
  EXPECT_TRUE(ds.TestCapability(ODsCZGeometries));
  EXPECT_TRUE(ds.TestCapability(ODsCMeasuredGeometries));
  EXPECT_FALSE(ds.TestCapability(ODsCCreateLayer));
  EXPECT_FALSE(ds.TestCapability("UnknownCap"));
}

TEST_F(OgrVrtDataSourceTest, InitializeInvalidRootNode) {
  OGRVRTDataSource ds(poDriver_);
  CPLXMLTreeCloser xml(CPLParseXMLString("<InvalidRoot/>"));
  ASSERT_NE(nullptr, xml);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(ds.Initialize(xml.release(), "test.vrt", FALSE));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Did not find the <OGRVRTDataSource> node"));
}

TEST_F(OgrVrtDataSourceTest, InitializeWithDatasetMetadataAndComments) {
  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <!-- Ignored comment -->\n"
      "  <Metadata>\n"
      "    <MDI key=\"DATASET_KEY\">DATASET_VAL</MDI>\n"
      "  </Metadata>\n"
      "  <UnknownElement/>\n"
      "  <OGRVRTLayer name=\"layer1\">\n"
      "    <SrcDataSource>@dummy@</SrcDataSource>\n"
      "    <SrcLayer>@dummy@</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "  <OGRVRTLayer>\n"  // Invalid layer missing 'name' attribute
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                 nullptr)));
  ASSERT_NE(nullptr, ds);
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Missing name attribute on OGRVRTLayer"));
  EXPECT_STREQ("DATASET_VAL", ds->GetMetadataItem("DATASET_KEY"));
  EXPECT_EQ(1, ds->GetLayerCount());
  ASSERT_NE(nullptr, ds->GetLayer(0));
  EXPECT_STREQ("layer1", ds->GetLayer(0)->GetName());

  // CloseDependentDatasets returns 1 on first call and 0 on second call.
  auto* vrt_ds = cpl::down_cast<OGRVRTDataSource*>(ds.get());
  EXPECT_EQ(1, vrt_ds->CloseDependentDatasets());
  EXPECT_EQ(0, ds->GetLayerCount());
  EXPECT_EQ(0, vrt_ds->CloseDependentDatasets());
}

TEST_F(OgrVrtDataSourceTest, ProxiedLayersAndGetFileList) {
  // Setup source CSV files in /vsimem/.
  constexpr char kCsv1Path[] = "/vsimem/ogrvrtdatasource_test/src1.csv";
  constexpr char kCsv2Path[] = "/vsimem/ogrvrtdatasource_test/src2.csv";
  VsiMemTempWrapper csv1(kCsv1Path, "col1\nval1\n");
  VsiMemTempWrapper csv2(kCsv2Path, "col2\nval2\n");

  constexpr char kVrtPath[] = "/vsimem/ogrvrtdatasource_test/proxied.vrt";
  constexpr char kVrt[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"layer1\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtdatasource_test/src1.csv</SrcDataSource>\n"
      "    <SrcLayer>src1</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "  <OGRVRTLayer name=\"layer2\">\n"
      "    <SrcDataSource>/vsimem/ogrvrtdatasource_test/src2.csv</SrcDataSource>\n"
      "    <SrcLayer>src2</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";
  VsiMemTempWrapper vrt_file(kVrtPath, kVrt);

  // Set OGR_VRT_MAX_OPENED=1 to force layer proxying (layer count = 2 > 1).
  CPLSetConfigOption("OGR_VRT_MAX_OPENED", "1");
  absl::Cleanup opt_cleaner = [] {
    CPLSetConfigOption("OGR_VRT_MAX_OPENED", nullptr);
  };

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(2, ds->GetLayerCount());

  // Access proxied layers.
  OGRLayer* lyr1 = ds->GetLayer(0);
  ASSERT_NE(nullptr, lyr1);
  EXPECT_STREQ("layer1", lyr1->GetName());
  EXPECT_EQ(1, lyr1->GetFeatureCount());

  OGRLayer* lyr2 = ds->GetLayer(1);
  ASSERT_NE(nullptr, lyr2);
  EXPECT_STREQ("layer2", lyr2->GetName());
  EXPECT_EQ(1, lyr2->GetFeatureCount());

  // GetFileList exercises OGR_VRT_PROXIED_LAYER and underlying source GetFileList.
  std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> file_list(
      ds->GetFileList());
  ASSERT_NE(nullptr, file_list);
  std::vector<std::string> files = CslToVector(file_list.get());
  EXPECT_THAT(files, Contains(kVrtPath));
  EXPECT_THAT(files, Contains(kCsv1Path));
  EXPECT_THAT(files, Contains(kCsv2Path));
}

TEST_F(OgrVrtDataSourceTest, GetFileListNonProxiedAndUnionLayer) {
  // OGRVRTDataSource::GetFileList() in ogrvrtdatasource.cpp:854 switches on
  // paeLayerType[nLayers - 1] (the last layer) rather than looping over all
  // layer types, so regular and union layers must be the last layer in a
  // dataset to test their respective branches.
  constexpr char kCsvPath[] = "/vsimem/ogrvrtdatasource_test/reg.csv";
  VsiMemTempWrapper csv(kCsvPath, "a\n1\n");

  // 1. Regular OGRVRTLayer at the end.
  {
    constexpr char kVrtPath[] = "/vsimem/ogrvrtdatasource_test/reg.vrt";
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTLayer name=\"layer1\">\n"
        "    <SrcDataSource>/vsimem/ogrvrtdatasource_test/reg.csv</SrcDataSource>\n"
        "    <SrcLayer>reg</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTDataSource>";
    VsiMemTempWrapper vrt_file(kVrtPath, kVrt);

    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> file_list(
        ds->GetFileList());
    ASSERT_NE(nullptr, file_list);
    EXPECT_THAT(CslToVector(file_list.get()), Contains(kCsvPath));
  }

  // 2. Union layer at the end (hits default: in switch).
  {
    constexpr char kVrtPath[] = "/vsimem/ogrvrtdatasource_test/union_last.vrt";
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTUnionLayer name=\"union_lyr\">\n"
        "    <OGRVRTLayer name=\"sub\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "</OGRVRTDataSource>";
    VsiMemTempWrapper vrt_file(kVrtPath, kVrt);

    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> file_list(
        ds->GetFileList());
    ASSERT_NE(nullptr, file_list);
    EXPECT_THAT(CslToVector(file_list.get()), Contains(kVrtPath));
  }
}

TEST_F(OgrVrtDataSourceTest, WarpedLayerErrorPaths) {
  OGRVRTDataSource ds(poDriver_);

  // 1. Non-OGRVRTWarpedLayer tag returns nullptr.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString("<NotWarped/>"));
    EXPECT_EQ(nullptr,
              ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
  }

  // 2. Missing source layer inside OGRVRTWarpedLayer.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTWarpedLayer>\n"
        "  <!-- Comment -->\n"
        "  <TargetSRS>EPSG:4326</TargetSRS>\n"
        "</OGRVRTWarpedLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot instantiate source layer"));
  }

  // 3. Missing TargetSRS.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTWarpedLayer>\n"
        "  <OGRVRTLayer name=\"dummy\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTWarpedLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Missing TargetSRS element"));
  }

  // 4. WarpedGeomFieldName not found on source layer.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTWarpedLayer>\n"
        "  <OGRVRTLayer name=\"dummy\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "  <TargetSRS>EPSG:4326</TargetSRS>\n"
        "  <WarpedGeomFieldName>missing_geom</WarpedGeomFieldName>\n"
        "</OGRVRTWarpedLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot find source geometry field"));
  }

  // 5. Source layer has no SRS and no SrcSRS element provided.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTWarpedLayer>\n"
        "  <OGRVRTLayer name=\"dummy\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "  <TargetSRS>EPSG:4326</TargetSRS>\n"
        "</OGRVRTWarpedLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import source SRS"));
  }

  // 6. Invalid SrcSRS element.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTWarpedLayer>\n"
        "  <OGRVRTLayer name=\"dummy\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "  <SrcSRS>INVALID:SRS:12345</SrcSRS>\n"
        "  <TargetSRS>EPSG:4326</TargetSRS>\n"
        "</OGRVRTWarpedLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import source SRS"));
  }

  // 7. Invalid TargetSRS element.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTWarpedLayer>\n"
        "  <OGRVRTLayer name=\"dummy\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "    <LayerSRS>WGS84</LayerSRS>\n"
        "  </OGRVRTLayer>\n"
        "  <TargetSRS>INVALID:SRS:99999</TargetSRS>\n"
        "</OGRVRTWarpedLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import target SRS"));
  }
}

TEST_F(OgrVrtDataSourceTest, WarpedLayerIdentityAndValidWarp) {
  // 1. Identity warp (SrcSRS omitted, source SRS is WGS84, TargetSRS is WGS84):
  // Returns source layer directly.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTWarpedLayer>\n"
        "    <OGRVRTLayer name=\"identity_test\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "      <LayerSRS>WGS84</LayerSRS>\n"
        "    </OGRVRTLayer>\n"
        "    <TargetSRS>WGS84</TargetSRS>\n"
        "  </OGRVRTWarpedLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_STREQ("identity_test", layer->GetName());
  }

  // 2. Valid warp with WarpedGeomFieldName, Extent, and SrcSRS.
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTWarpedLayer>\n"
        "    <OGRVRTLayer name=\"warped_test\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "      <GeometryField name=\"geom\" GeometryType=\"wkbPoint\" SRS=\"WGS84\"/>\n"
        "    </OGRVRTLayer>\n"
        "    <WarpedGeomFieldName>geom</WarpedGeomFieldName>\n"
        "    <SrcSRS>WGS84</SrcSRS>\n"
        "    <TargetSRS>EPSG:3857</TargetSRS>\n"
        "    <ExtentXMin>-1000</ExtentXMin>\n"
        "    <ExtentYMin>-1000</ExtentYMin>\n"
        "    <ExtentXMax>1000</ExtentXMax>\n"
        "    <ExtentYMax>1000</ExtentYMax>\n"
        "  </OGRVRTWarpedLayer>\n"
        "</OGRVRTDataSource>";
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);

    OGRSpatialReference expected_srs;
    ASSERT_EQ(OGRERR_NONE, expected_srs.SetFromUserInput("EPSG:3857"));
    ASSERT_NE(nullptr, layer->GetSpatialRef());
    EXPECT_THAT(*layer->GetSpatialRef(), IsSameAs(expected_srs));

    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, layer->GetExtent(&env, FALSE));
    EXPECT_DOUBLE_EQ(-1000.0, env.MinX);
    EXPECT_DOUBLE_EQ(1000.0, env.MaxX);
  }
}

TEST_F(OgrVrtDataSourceTest, UnionLayerValidationErrors) {
  OGRVRTDataSource ds(poDriver_);

  // 1. Missing name attribute.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString("<OGRVRTUnionLayer/>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Missing name attribute on OGRVRTUnionLayer"));
  }

  // 2. Unrecognized GeometryType on union layer.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\" GeometryType=\"wkbInvalid\"/>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GeometryType wkbInvalid not recognised"));
  }

  // 3. Failed to import LayerSRS.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\" LayerSRS=\"INVALID:SRS\"/>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import LayerSRS"));
  }

  // 4. Missing Field name.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <Field/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify Field name"));
  }

  // 5. Unrecognized Field type.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <Field name=\"f\" type=\"NoSuchType\"/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify Field type 'NoSuchType'"));
  }

  // 6. Invalid width (< 0).
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <Field name=\"f\" type=\"String\" width=\"-1\"/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid width for field f"));
  }

  // 7. Invalid precision (< 0 or > 1024).
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <Field name=\"f\" type=\"Real\" precision=\"-1\"/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid precision for field f"));
  }

  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <Field name=\"f\" type=\"Real\" precision=\"2000\"/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid precision for field f"));
  }

  // 8. Missing GeometryField name.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <GeometryField/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unable to identify GeometryField name"));
  }

  // 9. Unrecognized GeometryField GeometryType or wkbNone.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <GeometryField name=\"geom\" GeometryType=\"wkbNone\"/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GeometryType wkbNone not recognised"));
  }

  // 10. Failed to import GeometryField SRS.
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <GeometryField name=\"geom\" SRS=\"INVALID:SRS\"/>\n"
        "  <OGRVRTLayer name=\"sub\">\n"
        "    <SrcDataSource>@dummy@</SrcDataSource>\n"
        "    <SrcLayer>@dummy@</SrcLayer>\n"
        "  </OGRVRTLayer>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<OGRLayer> layer(
        ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_NE(nullptr, layer);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to import SRS"));
  }

  // 11. No source layers found (executes cleanup of papoFields and papoGeomFields).
  {
    CPLXMLTreeCloser xml(CPLParseXMLString(
        "<OGRVRTUnionLayer name=\"u\">\n"
        "  <Field name=\"f\" type=\"Integer\" width=\"10\" precision=\"0\"/>\n"
        "  <GeometryField name=\"geom\" GeometryType=\"wkbPoint\" SRS=\"WGS84\"\n"
        "                 ExtentXMin=\"0\" ExtentYMin=\"0\" ExtentXMax=\"10\" ExtentYMax=\"10\"/>\n"
        "</OGRVRTUnionLayer>"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds.InstantiateLayer(xml.get(), kTestDir, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot find source layers"));
  }
}

TEST_F(OgrVrtDataSourceTest, UnionLayerFieldStrategiesAndFeatures) {
  // Test valid union layers with different FieldStrategy settings.
  // 1. FieldStrategy="Union" with Global extent and SRS and LayerSRS="NULL".
  {
    constexpr char kVrt[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTUnionLayer name=\"union_test\" GeometryType=\"wkbPoint\" LayerSRS=\"NULL\"\n"
        "                    ExtentXMin=\"-10\" ExtentYMin=\"-10\" ExtentXMax=\"10\" ExtentYMax=\"10\"\n"
        "                    SourceLayerFieldName=\"src_lyr_name\" PreserveSrcFID=\"TRUE\"\n"
        "                    FieldStrategy=\"Union\" FeatureCount=\"2\">\n"
        "    <OGRVRTLayer name=\"sub1\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "    <OGRVRTLayer name=\"sub2\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "</OGRVRTDataSource>";

    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(2, layer->GetFeatureCount(FALSE));
  }

  // 2. FieldStrategy="FirstLayer" and "Intersection" and warning on "InvalidStrategy".
  {
    constexpr char kVrtStrategies[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTUnionLayer name=\"u_first\" FieldStrategy=\"FirstLayer\">\n"
        "    <OGRVRTLayer name=\"s1\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "  <OGRVRTUnionLayer name=\"u_inter\" FieldStrategy=\"Intersection\">\n"
        "    <OGRVRTLayer name=\"s2\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "  <OGRVRTUnionLayer name=\"u_bad\" FieldStrategy=\"NotAStrategy\">\n"
        "    <OGRVRTLayer name=\"s3\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "  <OGRVRTUnionLayer name=\"u_override\" FieldStrategy=\"Union\">\n"
        "    <Field name=\"f1\" type=\"String\"/>\n"
        "    <OGRVRTLayer name=\"s4\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "</OGRVRTDataSource>";

    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtStrategies, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetLayerCount());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Ignoring FieldStrategy value"));
  }

  // 3. UnionLayer with GeometryField element inheriting layer GeometryType and LayerSRS.
  {
    constexpr char kVrtInheritGeom[] =
        "<OGRVRTDataSource>\n"
        "  <OGRVRTUnionLayer name=\"u_inherit\" GeometryType=\"wkbPoint\" LayerSRS=\"WGS84\">\n"
        "    <GeometryField name=\"geom\" SRS=\"NULL\"/>\n"
        "    <OGRVRTLayer name=\"sub\">\n"
        "      <SrcDataSource>@dummy@</SrcDataSource>\n"
        "      <SrcLayer>@dummy@</SrcLayer>\n"
        "    </OGRVRTLayer>\n"
        "  </OGRVRTUnionLayer>\n"
        "</OGRVRTDataSource>";

    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kVrtInheritGeom, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(1, ds->GetLayerCount());
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbPoint, layer->GetGeomType());
  }
  // TODO: b/335317901 - Cover paeLayerType[nLayers - 1] quirk in GetFileList
  // when mixed layer types precede it, and union layer global SRS dereference edge cases.
}

}  // namespace
}  // namespace autotest2
