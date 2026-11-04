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

// Tests for the GeoJSONSeq driver.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrgeojsonseqdriver.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonseqdriver.cpp

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonutils.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_http.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgeojsonseqdriver_test";

class OGRGeoJSONSeqDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGeoJSONSeq();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRGeoJSONSeqDriverTest, RegistrationAndMetadata) {
  RegisterOGRGeoJSONSeq();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSONSeq");
  ASSERT_NE(poDriver, nullptr);

  EXPECT_STREQ(poDriver->GetDescription(), "GeoJSONSeq");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VECTOR), "YES");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_LONGNAME),
               "GeoJSON Sequence");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_EXTENSIONS),
               "geojsonl geojsons");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DMD_HELPTOPIC),
               "drivers/vector/geojsonseq.html");
  EXPECT_STREQ(poDriver->GetMetadataItem(GDAL_DCAP_VIRTUALIO), "YES");
  EXPECT_NE(poDriver->GetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST),
            nullptr);
  EXPECT_NE(poDriver->pfnOpen, nullptr);
  EXPECT_NE(poDriver->pfnIdentify, nullptr);
  EXPECT_NE(poDriver->pfnCreate, nullptr);
}

TEST_F(OGRGeoJSONSeqDriverTest, IdentifyAndOpenSources) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSONSeq");
  ASSERT_NE(poDriver, nullptr);

  // 1. Unknown text
  {
    GDALOpenInfo oi("{}", GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&oi), FALSE);
  }

  // 2. Prefixed GeoJSONSeq:
  {
    const std::string fn = std::string(kTestDir) + "/seq1.json";
    VSILFILE* fp = VSIFOpenL(fn.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    const char data[] = "{\"type\":\"Feature\",\"geometry\":null}\n";
    VSIFWriteL(data, 1, strlen(data), fp);
    VSIFCloseL(fp);

    std::string prefixed = "GeoJSONSeq:" + fn;
    GDALOpenInfo oi(prefixed.c_str(), GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnIdentify(&oi), TRUE);
    GDALDataset* poDS = poDriver->pfnOpen(&oi);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    GDALClose(poDS);
  }

  // 3. Update mode on read returns nullptr
  {
    const std::string fn = std::string(kTestDir) + "/update.geojsonl";
    VSILFILE* fp = VSIFOpenL(fn.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    const char data[] = "{\"type\":\"Feature\",\"geometry\":null}\n";
    VSIFWriteL(data, 1, strlen(data), fp);
    VSIFCloseL(fp);

    WithQuietHandler quiet;
    GDALOpenInfo oi(fn.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE);
    EXPECT_EQ(poDriver->pfnOpen(&oi), nullptr);
    CPLErrorReset();
  }

  // 4. Stored content in driver
  {
    const char kURL[] = "http://example.com/stream.geojsonl";
    OGRGeoJSONDriverStoreContent(
        kURL, CPLStrdup("\x1e{\"type\":\"Feature\",\"geometry\":null}\n"));

    GDALOpenInfo oi(kURL, GDAL_OF_VECTOR);
    GDALDataset* poDS = poDriver->pfnOpen(&oi);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetLayerCount(), 1);
    GDALClose(poDS);
  }

  // 5. Stored content that is NOT GeoJSONSeq
  {
    const char kURL[] = "http://example.com/not_seq.json";
    OGRGeoJSONDriverStoreContent(kURL, CPLStrdup("not json seq content"));

    GDALOpenInfo oi(kURL, GDAL_OF_VECTOR);
    EXPECT_EQ(poDriver->pfnOpen(&oi), nullptr);

    // Stored content was restored
    char* stolen = OGRGeoJSONDriverStealStoredContent(kURL);
    ASSERT_NE(stolen, nullptr);
    CPLFree(stolen);
  }
}

TEST_F(OGRGeoJSONSeqDriverTest, CreationAndLayerOptions) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSONSeq");
  ASSERT_NE(poDriver, nullptr);

  const std::string filename = std::string(kTestDir) + "/out.geojsons";

  // Invalid path (existing directory)
  const std::string dir_path = std::string(kTestDir) + "/is_a_dir.geojsonl";
  VSIMkdir(dir_path.c_str(), 0755);
  WithQuietHandler quiet;
  EXPECT_EQ(poDriver->Create(dir_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr),
            nullptr);
  CPLErrorReset();
  CPLErrorReset();

  // Create on /vsimem/ (.geojsons defaults to RS=YES)
  GDALDataset* poDS =
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS, nullptr);

  OGRSpatialReference oSRS3857;
  oSRS3857.importFromEPSG(3857);

  const char* const apszOptions[] = {"RS=YES",
                                     "COORDINATE_PRECISION=4",
                                     "XY_COORD_PRECISION=5",
                                     "Z_COORD_PRECISION=2",
                                     "SIGNIFICANT_FIGURES=6",
                                     "ID_FIELD=my_id",
                                     "ID_TYPE=String",
                                     "WRITE_BBOX=YES",
                                     "WRITE_NON_FINITE_VALUES=YES",
                                     "AUTODETECT_JSON_STRINGS=YES",
                                     nullptr};

  OGRLayer* poLayer =
      poDS->CreateLayer("seq_layer", &oSRS3857, wkbPoint, apszOptions);
  ASSERT_NE(poLayer, nullptr);

  OGRFieldDefn oFieldId("my_id", OFTString);
  EXPECT_EQ(poLayer->CreateField(&oFieldId), OGRERR_NONE);

  // Write feature with EPSG:3857 coordinate transformation
  OGRFeature* poFeat = new OGRFeature(poLayer->GetLayerDefn());
  poFeat->SetField("my_id", "id_1");
  OGRPoint pt(0.0, 0.0);  // (0, 0) WGS84
  poFeat->SetGeometry(&pt);
  EXPECT_EQ(poLayer->CreateFeature(poFeat), OGRERR_NONE);
  delete poFeat;

  // Layer capabilities
  EXPECT_TRUE(poLayer->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(poLayer->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(poLayer->TestCapability(OLCCreateField));

  // Write-only layer calling GetNextFeature fails when multiple layers or
  // during write
  EXPECT_EQ(poLayer->GetNextFeature(), nullptr);

  GDALClose(poDS);

  // Verify created file can be read back
  GDALDataset* poDSRead = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDSRead, nullptr);
  OGRLayer* poLayerRead = poDSRead->GetLayer(0);
  ASSERT_NE(poLayerRead, nullptr);
  EXPECT_EQ(poLayerRead->GetFeatureCount(), 1);
  GDALClose(poDSRead);
}

TEST_F(OGRGeoJSONSeqDriverTest, CoordinatePrecisionAndReprojectionBounds) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GeoJSONSeq");
  ASSERT_NE(poDriver, nullptr);

  const std::string filename = std::string(kTestDir) + "/prec.geojsonl";
  GDALDataset* poDS =
      poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(poDS, nullptr);

  // 1. Layer without SRS: emits warning and assumes WGS84
  WithQuietHandler quiet;
  OGRLayer* poLayer =
      poDS->CreateLayer("layer_nosrs", nullptr, wkbPoint, nullptr);
  ASSERT_NE(poLayer, nullptr);
  CPLErrorReset();

  OGRFeature* poFeatNoSRS = new OGRFeature(poLayer->GetLayerDefn());
  OGRPoint ptNoSRS(10.0, 20.0);
  poFeatNoSRS->SetGeometry(&ptNoSRS);
  EXPECT_EQ(poLayer->CreateFeature(poFeatNoSRS), OGRERR_NONE);
  delete poFeatNoSRS;

  // 2. Layer with projected SRS (EPSG:3857) to exercise coordinate
  // transformation
  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(3857);
  OGRLayer* poLayerSRS =
      poDS->CreateLayer("layer_srs", &oSRS, wkbPoint, nullptr);
  ASSERT_NE(poLayerSRS, nullptr);
  EXPECT_STREQ(poLayerSRS->GetName(), "layer_srs");
  EXPECT_EQ(poLayerSRS->GetDataset(), poDS);
  EXPECT_STREQ(poLayerSRS->GetFIDColumn(), "");

  OGRFeature* poFeatSRS = new OGRFeature(poLayerSRS->GetLayerDefn());
  OGRPoint ptSRS(100000.0, 200000.0);
  poFeatSRS->SetGeometry(&ptSRS);
  EXPECT_EQ(poLayerSRS->CreateFeature(poFeatSRS), OGRERR_NONE);
  delete poFeatSRS;

  // 3. Failed coordinate transformation (Local CS)
  {
    WithQuietHandler quiet_trans;
    OGRSpatialReference oBadSRS;
    oBadSRS.SetLocalCS("local");
    EXPECT_EQ(poDS->CreateLayer("layer_bad", &oBadSRS), nullptr);
    CPLErrorReset();
  }

  // 4. Test coordinate precision conversion
  {
    OGRGeomFieldDefn oGeomFieldDefn("geom", wkbPoint25D);
    oGeomFieldDefn.SetSpatialRef(&oSRS);
    OGRGeomCoordinatePrecision oCoordPrec;
    oCoordPrec.dfXYResolution = 0.01;
    oCoordPrec.dfZResolution = 0.05;
    oGeomFieldDefn.SetCoordinatePrecision(oCoordPrec);
    OGRLayer* poLayerPrec =
        poDS->CreateLayer("layer_prec", &oGeomFieldDefn, nullptr);
    ASSERT_NE(poLayerPrec, nullptr);
  }

  GDALClose(poDS);

  // 5. Creating layer on read-only dataset fails
  GDALDataset* poDSRONew = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDSRONew, nullptr);
  EXPECT_EQ(poDSRONew->CreateLayer("layer_fail"), nullptr);
  GDALClose(poDSRONew);

  // 6. Inline text (eGeoJSONSourceText)
  const char kInlineSeq[] = "\x1e{\"type\":\"Feature\",\"geometry\":null}\n";
  GDALDataset* poDSText = static_cast<GDALDataset*>(
      GDALOpenEx(kInlineSeq, GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDSText, nullptr);
  EXPECT_EQ(poDSText->GetLayerCount(), 1);
  GDALClose(poDSText);

  // Inline text with GA_Update fails
  EXPECT_EQ(GDALOpenEx(kInlineSeq, GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                       nullptr, nullptr),
            nullptr);
}

TEST_F(OGRGeoJSONSeqDriverTest, ReadingVariantsAndChunking) {
  const std::string filename = std::string(kTestDir) + "/reading.geojsonl";

  // RS-delimited GeoJSON Sequence records
  const char kData[] =
      "\x1e{\"type\":\"Feature\",\"properties\":{\"val\":10},\"geometry\":{"
      "\"type\":\"Point\",\"coordinates\":[1.0,2.0]}}\n"
      "\x1e{\"type\":\"Point\",\"coordinates\":[3.0,4.0]}\n"
      "\x1e{\"type\":\"Feature\",\"id\":99,\"properties\":{\"val\":20},"
      "\"geometry\":{\"type\":\"Point\",\"coordinates\":[5.0,6.0]}}\r\n";

  VSILFILE* fp = VSIFOpenL(filename.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(kData, 1, strlen(kData), fp);
  VSIFCloseL(fp);

  // Set small chunk size to exercise buffer growth and shifting
  CPLSetThreadLocalConfigOption("OGR_GEOJSONSEQ_CHUNK_SIZE", "16");
  absl::Cleanup cleanup = [] {
    CPLSetThreadLocalConfigOption("OGR_GEOJSONSEQ_CHUNK_SIZE", nullptr);
  };

  WithQuietHandler quiet;
  GDALDataset* poDS = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
  ASSERT_NE(poDS, nullptr);
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);

  // Read features: 1st (Feature), 2nd (standalone Point wrapped in Feature),
  // 3rd (Feature id 99)
  OGRFeature* f1 = poLayer->GetNextFeature();
  ASSERT_NE(f1, nullptr);
  EXPECT_EQ(f1->GetFieldAsInteger("val"), 10);
  delete f1;

  OGRFeature* f2 = poLayer->GetNextFeature();
  ASSERT_NE(f2, nullptr);
  EXPECT_NE(f2->GetGeometryRef(), nullptr);
  delete f2;

  OGRFeature* f3 = poLayer->GetNextFeature();
  ASSERT_NE(f3, nullptr);
  EXPECT_EQ(f3->GetFID(), 99);
  delete f3;

  EXPECT_EQ(poLayer->GetNextFeature(), nullptr);

  // Filters
  poLayer->SetSpatialFilterRect(0, 0, 2, 3);
  poLayer->ResetReading();
  OGRFeature* fFiltered = poLayer->GetNextFeature();
  ASSERT_NE(fFiltered, nullptr);
  delete fFiltered;
  EXPECT_EQ(poLayer->GetNextFeature(), nullptr);

  CPLErrorReset();
  GDALClose(poDS);
}

}  // namespace
}  // namespace autotest2
