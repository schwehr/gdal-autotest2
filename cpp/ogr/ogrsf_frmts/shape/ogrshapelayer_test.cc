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

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/shape/shape_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/ogrshape.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shp_vsi.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

// 1. Tests for CodePage, LDID, CPG, and layer date/EOF settings.
TEST(OGRShapeLayerTest, CodePageAndEncodingOptions) {
  constexpr const char* kDir = "/vsimem/osl_codepage";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  const struct {
    const char* encoding;
    const char* expectedMeta;
  } kEncodingTests[] = {
      {"LDID/87", "ISO-8859-1"}, {"LDID/3", "CP1252"},
      {"LDID/1", "CP437"},       {"LDID/2", "CP850"},
      {"LDID/4", "CP10000"},     {"LDID/8", "CP865"},
      {"LDID/19", "CP932"},      {"LDID/28", "CP863"},
      {"LDID/31", "CP852"},      {"LDID/36", "CP860"},
      {"LDID/38", "CP866"},      {"LDID/77", "CP936"},
      {"LDID/78", "CP949"},      {"LDID/79", "CP950"},
      {"LDID/80", "CP874"},      {"LDID/88", "CP1252"},
      {"LDID/103", "CP861"},     {"LDID/104", "CP895"},
      {"LDID/105", "CP620"},     {"LDID/106", "CP737"},
      {"LDID/107", "CP857"},     {"LDID/120", "CP950"},
      {"LDID/121", "CP949"},     {"LDID/122", "CP936"},
      {"LDID/123", "CP932"},     {"LDID/124", "CP874"},
      {"LDID/150", "CP10007"},   {"LDID/151", "CP10029"},
      {"LDID/200", "CP1250"},    {"LDID/201", "CP1251"},
      {"LDID/202", "CP1254"},    {"LDID/203", "CP1253"},
      {"LDID/204", "CP1257"},    {"LDID/10", "CP850"},
      {"LDID/11", "CP437"},      {"LDID/13", "CP437"},
      {"LDID/14", "CP850"},      {"LDID/15", "CP437"},
      {"LDID/16", "CP850"},      {"LDID/17", "CP437"},
      {"LDID/18", "CP850"},      {"LDID/20", "CP850"},
      {"LDID/21", "CP437"},      {"LDID/22", "CP850"},
      {"LDID/23", "CP865"},      {"LDID/24", "CP437"},
      {"LDID/25", "CP437"},      {"LDID/26", "CP850"},
      {"LDID/27", "CP437"},      {"LDID/29", "CP850"},
      {"LDID/34", "CP852"},      {"LDID/35", "CP852"},
      {"LDID/37", "CP850"},      {"LDID/55", "CP850"},
      {"LDID/64", "CP852"},      {"LDID/89", "CP1252"},
      {"LDID/100", "CP852"},     {"LDID/101", "CP866"},
      {"LDID/102", "CP865"},     {"LDID/108", "CP863"},
      {"LDID/134", "CP737"},     {"LDID/135", "CP852"},
      {"LDID/136", "CP857"},     {"LDID/999", nullptr},
      {"UTF-8", "UTF-8"},        {"UTF8", "UTF-8"},
      {"8859-1", "ISO-8859-1"},  {"88591", "ISO-8859-1"},
      {"ANSI 1251", "CP1251"},   {"1252", "CP1252"},
      {"936", "CP936"},          {"Big5", "Big5"},
  };

  for (size_t i = 0; i < std::size(kEncodingTests); ++i) {
    std::string lyrName = "enc_" + std::to_string(i);
    const char* const papszOpts[] = {
        CPLSPrintf("ENCODING=%s", kEncodingTests[i].encoding), nullptr};
    OGRGeomFieldDefn gf("geom", wkbPoint);
    OGRLayer* lyr = ds.ICreateLayer(lyrName.c_str(), &gf, papszOpts);
    ASSERT_THAT(lyr, NotNull());

    OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);
    // Test SetModificationDate with various date strings
    shapeLyr->SetModificationDate("2026-11-03");
    shapeLyr->SetModificationDate("2026/11/03");
    shapeLyr->SetModificationDate("invalid-date");
    shapeLyr->SetModificationDate("1899-01-01");  // out of range (< 1900)
    shapeLyr->SetModificationDate("2200-01-01");  // out of range (> 1900+255)
    shapeLyr->SetModificationDate("2026-13-01");  // invalid month
    shapeLyr->SetModificationDate("2026-11-32");  // invalid day

    shapeLyr->SetWriteDBFEOFChar(true);
    shapeLyr->SetWriteDBFEOFChar(false);
    shapeLyr->SetAutoRepack(true);
    shapeLyr->SetAutoRepack(false);
    shapeLyr->SetResizeAtClose(true);
    shapeLyr->SetResizeAtClose(false);
    shapeLyr->CreateSpatialIndexAtClose(1);
    shapeLyr->CreateSpatialIndexAtClose(0);

    const char* srcEnc =
        shapeLyr->GetMetadataItem("SOURCE_ENCODING", "SHAPEFILE");
    if (kEncodingTests[i].expectedMeta != nullptr) {
      EXPECT_THAT(srcEnc, NotNull());
    }
  }
}

// 2. Spatial reference and OGRShapeGeomFieldDefn handling (.prj, .qpj, epoch)
TEST(OGRShapeLayerTest, SpatialReferenceAndGeomFieldDefn) {
  constexpr const char* kDir = "/vsimem/osl_srs";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  // 1. Create with spatial reference
  OGRSpatialReference srs4326;
  srs4326.SetFromUserInput("EPSG:4326");
  srs4326.SetCoordinateEpoch(2026.5);
  OGRGeomFieldDefn gf("geom", wkbPoint);
  gf.SetSpatialRef(&srs4326);

  OGRLayer* lyr = ds.ICreateLayer("srs_layer", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  EXPECT_THAT(lyr->GetSpatialRef(), NotNull());

  // Test OGRShapeGeomFieldDefn methods directly
  const OGRGeomFieldDefn* gfd = lyr->GetLayerDefn()->GetGeomFieldDefn(0);
  ASSERT_THAT(gfd, NotNull());
  const OGRShapeGeomFieldDefn* shapeGfd =
      cpl::down_cast<const OGRShapeGeomFieldDefn*>(gfd);
  EXPECT_FALSE(shapeGfd->GetPrjFilename().empty());
  EXPECT_THAT(shapeGfd->GetSpatialRef(), NotNull());
  const_cast<OGRShapeGeomFieldDefn*>(shapeGfd)->SetSRSSet();

  // 2. Test AlterGeomFieldDefn (altering SRS, creating/updating/deleting .prj)
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);
  {
    OGRSpatialReference srs3857;
    srs3857.SetFromUserInput("EPSG:3857");
    OGRGeomFieldDefn newGfd("geom", wkbPoint);
    newGfd.SetSpatialRef(&srs3857);
    EXPECT_EQ(shapeLyr->AlterGeomFieldDefn(0, &newGfd,
                                           ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
              OGRERR_NONE);
    EXPECT_THAT(shapeLyr->GetSpatialRef(), NotNull());
  }

  // Clear SRS (should delete .prj file)
  {
    OGRGeomFieldDefn newGfd("geom", wkbPoint);
    newGfd.SetSpatialRef(nullptr);
    EXPECT_EQ(shapeLyr->AlterGeomFieldDefn(0, &newGfd,
                                           ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
              OGRERR_NONE);
    EXPECT_THAT(shapeLyr->GetSpatialRef(), Eq(nullptr));
  }

  // AlterGeomFieldDefn with same name, nullable flag, and on a layer without
  // initial PRJ
  {
    OGRGeomFieldDefn gfNoPrj("geom", wkbPoint);
    OGRLayer* lyrNoPrj = ds.ICreateLayer("no_prj_geom", &gfNoPrj, nullptr);
    ASSERT_THAT(lyrNoPrj, NotNull());
    OGRShapeLayer* shapeNoPrj = cpl::down_cast<OGRShapeLayer*>(lyrNoPrj);

    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    const std::string origName =
        shapeNoPrj->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef();
    OGRGeomFieldDefn newGeomDefn(origName.c_str(), wkbPoint);
    newGeomDefn.SetSpatialRef(&srs4326);
    newGeomDefn.SetNullable(FALSE);

    EXPECT_EQ(
        shapeNoPrj->AlterGeomFieldDefn(0, &newGeomDefn,
                                       ALTER_GEOM_FIELD_DEFN_NAME_FLAG |
                                           ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG |
                                           ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
        OGRERR_NONE);
    EXPECT_EQ(shapeNoPrj->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef(),
              origName);
    EXPECT_FALSE(shapeNoPrj->GetLayerDefn()->GetGeomFieldDefn(0)->IsNullable());
  }

  // Rename geometry field to a different name (not supported on shapefiles)
  {
    OGRGeomFieldDefn newGfd("new_geom", wkbPoint);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->AlterGeomFieldDefn(0, &newGfd,
                                           ALTER_GEOM_FIELD_DEFN_NAME_FLAG),
              OGRERR_FAILURE);
  }

  // Alter geometry type (not supported on shapefiles)
  {
    OGRGeomFieldDefn newGfd("geom", wkbPolygon);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->AlterGeomFieldDefn(0, &newGfd,
                                           ALTER_GEOM_FIELD_DEFN_TYPE_FLAG),
              OGRERR_FAILURE);
  }

  // Set coordinate epoch > 0 (not supported on shapefiles)
  {
    OGRSpatialReference srsEpoch;
    srsEpoch.SetFromUserInput("EPSG:4326");
    srsEpoch.SetCoordinateEpoch(2026.5);
    OGRGeomFieldDefn newGfd("geom", wkbPoint);
    newGfd.SetSpatialRef(&srsEpoch);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->AlterGeomFieldDefn(
                  0, &newGfd, ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG),
              OGRERR_FAILURE);
  }

  // Invalid geom field index
  {
    OGRGeomFieldDefn newGfd("geom", wkbPoint);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->AlterGeomFieldDefn(1, &newGfd,
                                           ALTER_GEOM_FIELD_DEFN_ALL_FLAG),
              OGRERR_FAILURE);
  }

  // 3. Test PRJ file loading with UTF-8 BOM, AXIS stripping, and match options
  const std::string prjPath = "/vsimem/osl_srs/bom_layer.prj";
  const std::string prjContent =
      "\xEF\xBB\xBF"  // UTF-8 BOM
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
      "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
      "0.0174532925199433],AXIS[\"Lat\",NORTH],AXIS[\"Lon\",EAST]]";
  VSILFILE* fpPrj = VSIFOpenL(prjPath.c_str(), "wt");
  ASSERT_THAT(fpPrj, NotNull());
  VSIFWriteL(prjContent.c_str(), 1, prjContent.size(), fpPrj);
  VSIFCloseL(fpPrj);

  {
    OGRShapeGeomFieldDefn lazyGfd("/vsimem/osl_srs/bom_layer.shp", wkbPoint,
                                  FALSE, nullptr);
    const OGRSpatialReference* poLazySRS = lazyGfd.GetSpatialRef();
    ASSERT_THAT(poLazySRS, NotNull());
    EXPECT_TRUE(poLazySRS->IsGeographic());
  }

  // Test with USE_OSR_FIND_MATCHES = NO
  {
    CPLSetConfigOption("USE_OSR_FIND_MATCHES", "NO");
    auto clMatches = absl::MakeCleanup(
        []() { CPLSetConfigOption("USE_OSR_FIND_MATCHES", nullptr); });
    OGRShapeGeomFieldDefn lazyGfd2("/vsimem/osl_srs/bom_layer.shp", wkbPoint,
                                   FALSE, nullptr);
    const OGRSpatialReference* poLazySRS = lazyGfd2.GetSpatialRef();
    ASSERT_THAT(poLazySRS, NotNull());
    EXPECT_TRUE(poLazySRS->IsGeographic());
  }

  // Test with uppercase .PRJ fallback
  const std::string upperPrjPath = "/vsimem/osl_srs/upper_layer.PRJ";
  VSILFILE* fpUpper = VSIFOpenL(upperPrjPath.c_str(), "wt");
  ASSERT_THAT(fpUpper, NotNull());
  VSIFWriteL(prjContent.c_str(), 1, prjContent.size(), fpUpper);
  VSIFCloseL(fpUpper);

  {
    OGRShapeGeomFieldDefn lazyUpper("/vsimem/osl_srs/upper_layer.shp", wkbPoint,
                                    FALSE, nullptr);
    const OGRSpatialReference* poSRS = lazyUpper.GetSpatialRef();
    ASSERT_THAT(poSRS, NotNull());
  }

  // Test with invalid PRJ content
  const std::string invalidPrjPath = "/vsimem/osl_srs/invalid_layer.prj";
  VSILFILE* fpInvalid = VSIFOpenL(invalidPrjPath.c_str(), "wt");
  ASSERT_THAT(fpInvalid, NotNull());
  const char* kBadWkt = "INVALID_WKT";
  VSIFWriteL(kBadWkt, 1, strlen(kBadWkt), fpInvalid);
  VSIFCloseL(fpInvalid);

  {
    OGRShapeGeomFieldDefn lazyInvalid("/vsimem/osl_srs/invalid_layer.shp",
                                      wkbPoint, FALSE, nullptr);
    WithQuietHandler quiet;
    const OGRSpatialReference* poSRS = lazyInvalid.GetSpatialRef();
    EXPECT_THAT(poSRS, Eq(nullptr));
  }
}

// 3. Feature CRUD operations, FID navigation, Repack, and Truncate
TEST(OGRShapeLayerTest, FeatureCRUDAndRepack) {
  constexpr const char* kDir = "/vsimem/osl_crud";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPoint);
  OGRLayer* lyr = ds.ICreateLayer("crud_lyr", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());

  OGRFieldDefn fldName("NAME", OFTString);
  EXPECT_EQ(lyr->CreateField(&fldName), OGRERR_NONE);
  OGRFieldDefn fldVal("VAL", OFTReal);
  EXPECT_EQ(lyr->CreateField(&fldVal), OGRERR_NONE);

  // 1. Create 5 features
  for (int i = 0; i < 5; ++i) {
    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetField("NAME", CPLSPrintf("Feature_%d", i));
    feat.SetField("VAL", i * 1.5);
    OGRPoint pt(10.0 + i, 20.0 + i);
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
    EXPECT_EQ(feat.GetFID(), i);
  }
  EXPECT_EQ(lyr->GetFeatureCount(), 5);

  // 2. FetchShape and GetFeature
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);
  std::unique_ptr<OGRFeature> f0(shapeLyr->GetFeature(0));
  ASSERT_THAT(f0, NotNull());
  EXPECT_STREQ(f0->GetFieldAsString("NAME"), "Feature_0");

  std::unique_ptr<OGRFeature> fNull(shapeLyr->GetFeature(100));
  EXPECT_THAT(fNull, Eq(nullptr));
  std::unique_ptr<OGRFeature> fNeg(shapeLyr->GetFeature(-1));
  EXPECT_THAT(fNeg, Eq(nullptr));

  // 3. ISetFeature: update middle feature and last feature
  {
    std::unique_ptr<OGRFeature> f2(shapeLyr->GetFeature(2));
    ASSERT_THAT(f2, NotNull());
    f2->SetField("NAME", "Updated_2_LongerName");
    OGRPoint ptNew(99.0, 99.0);
    f2->SetGeometry(&ptNew);
    EXPECT_EQ(shapeLyr->SetFeature(f2.get()), OGRERR_NONE);

    // Update last feature (FID 4)
    std::unique_ptr<OGRFeature> f4(shapeLyr->GetFeature(4));
    ASSERT_THAT(f4, NotNull());
    f4->SetField("NAME", "Updated_4");
    EXPECT_EQ(shapeLyr->SetFeature(f4.get()), OGRERR_NONE);

    // Non-existing FID
    f4->SetFID(999);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->SetFeature(f4.get()), OGRERR_NON_EXISTING_FEATURE);
  }

  // 4. DeleteFeature
  shapeLyr->SetAutoRepack(false);
  EXPECT_EQ(shapeLyr->DeleteFeature(1), OGRERR_NONE);
  EXPECT_EQ(shapeLyr->DeleteFeature(1),
            OGRERR_NON_EXISTING_FEATURE);  // already deleted
  EXPECT_EQ(shapeLyr->DeleteFeature(-1), OGRERR_NON_EXISTING_FEATURE);
  EXPECT_EQ(shapeLyr->DeleteFeature(100), OGRERR_NON_EXISTING_FEATURE);

  // 5. Feature iteration with deleted feature
  lyr->ResetReading();
  int count = 0;
  while (OGRFeature* f = lyr->GetNextFeature()) {
    EXPECT_NE(f->GetFID(), 1);
    delete f;
    count++;
  }
  EXPECT_EQ(count, 4);

  // SetNextByIndex
  EXPECT_EQ(lyr->SetNextByIndex(0), OGRERR_NONE);
  EXPECT_EQ(lyr->SetNextByIndex(3), OGRERR_NONE);
  EXPECT_EQ(lyr->SetNextByIndex(-1), OGRERR_NON_EXISTING_FEATURE);
  EXPECT_EQ(lyr->SetNextByIndex(10), OGRERR_NON_EXISTING_FEATURE);

  // 6. SyncToDisk, Repack, RecomputeExtent, ResizeDBF
  EXPECT_EQ(shapeLyr->SyncToDisk(), OGRERR_NONE);
  EXPECT_EQ(shapeLyr->RecomputeExtent(), OGRERR_NONE);
  EXPECT_EQ(shapeLyr->ResizeDBF(), OGRERR_NONE);
  EXPECT_EQ(shapeLyr->Repack(), OGRERR_NONE);
  EXPECT_EQ(shapeLyr->Repack(), OGRERR_NONE);  // 2nd call when repack is NO

  // Delete another feature and repack with OGR_SHAPE_PACK_IN_PLACE = YES
  EXPECT_EQ(shapeLyr->DeleteFeature(0), OGRERR_NONE);
  {
    CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", "YES");
    auto clPack = absl::MakeCleanup(
        []() { CPLSetConfigOption("OGR_SHAPE_PACK_IN_PLACE", nullptr); });
    EXPECT_EQ(shapeLyr->Repack(), OGRERR_NONE);
  }

  // 7. Auto geometry reset from wkbUnknown across geometry types
  {
    const struct {
      const char* lyrName;
      OGRwkbGeometryType expType;
      const char* wktGeom;
    } kAutoGeoms[] = {
        {"auto_pt", wkbPoint, "POINT(1 2)"},
        {"auto_pt25d", wkbPoint25D, "POINT Z(1 2 3)"},
        {"auto_ptm", wkbPointM, "POINT M(1 2 3)"},
        {"auto_ptzm", wkbPointZM, "POINT ZM(1 2 3 4)"},
        {"auto_ls", wkbLineString, "LINESTRING(1 2, 3 4)"},
        {"auto_ls25d", wkbLineString25D, "LINESTRING Z(1 2 3, 4 5 6)"},
        {"auto_lsm", wkbLineStringM, "LINESTRING M(1 2 3, 4 5 6)"},
        {"auto_lszm", wkbLineStringZM, "LINESTRING ZM(1 2 3 4, 5 6 7 8)"},
        {"auto_poly", wkbPolygon, "POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))"},
        {"auto_poly25d", wkbPolygon25D,
         "POLYGON Z((0 0 0, 1 0 0, 1 1 0, 0 1 0, 0 0 0))"},
        {"auto_polym", wkbPolygonM,
         "POLYGON M((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1))"},
        {"auto_polyzm", wkbPolygonZM,
         "POLYGON ZM((0 0 0 1, 1 0 0 1, 1 1 0 1, 0 1 0 1, 0 0 0 1))"},
        {"auto_mpt", wkbMultiPoint, "MULTIPOINT((1 2), (3 4))"},
        {"auto_mpt25d", wkbMultiPoint25D, "MULTIPOINT Z((1 2 3))"},
        {"auto_mptm", wkbMultiPointM, "MULTIPOINT M((1 2 3))"},
        {"auto_mptzm", wkbMultiPointM, "MULTIPOINT ZM((1 2 3 4))"},
        {"auto_tin", wkbUnknown, "TIN Z(((0 0 0, 0 1 0, 1 1 0, 0 0 0)))"},
        {"auto_polyhedralsurface", wkbUnknown,
         "POLYHEDRALSURFACE Z(((0 0 0, 0 1 0, 1 1 0, 1 0 0, 0 0 0)))"},
        {"auto_gc_tin", wkbUnknown,
         "GEOMETRYCOLLECTION Z(TIN Z(((0 0 0, 0 1 0, 1 1 0, 0 0 0))))"},
    };

    for (const auto& ag : kAutoGeoms) {
      OGRGeomFieldDefn gfUnk("geom", wkbUnknown);
      OGRLayer* lyrUnk = ds.ICreateLayer(ag.lyrName, &gfUnk, nullptr);
      ASSERT_THAT(lyrUnk, NotNull());

      OGRFeature feat(lyrUnk->GetLayerDefn());
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt(ag.wktGeom, nullptr, &geom);
      ASSERT_THAT(geom, NotNull());
      feat.SetGeometryDirectly(geom);
      EXPECT_EQ(lyrUnk->CreateFeature(&feat), OGRERR_NONE);
      EXPECT_EQ(lyrUnk->GetGeomType(), ag.expType);
    }

    // Unsupported geometry type for auto reset
    {
      OGRGeomFieldDefn gfUnk("geom", wkbUnknown);
      OGRLayer* lyrUnk = ds.ICreateLayer("auto_unsupported", &gfUnk, nullptr);
      ASSERT_THAT(lyrUnk, NotNull());
      OGRFeature feat(lyrUnk->GetLayerDefn());
      OGRGeometry* geom = nullptr;
      OGRGeometryFactory::createFromWkt("GEOMETRYCOLLECTION(POINT(1 2))",
                                        nullptr, &geom);
      ASSERT_THAT(geom, NotNull());
      feat.SetGeometryDirectly(geom);
      WithQuietHandler quiet;
      EXPECT_EQ(lyrUnk->CreateFeature(&feat), OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
    }
  }
}

// 4. Schema modifications: CreateField, DeleteField, ReorderFields,
// AlterFieldDefn
TEST(OGRShapeLayerTest, SchemaModifications) {
  constexpr const char* kDir = "/vsimem/osl_schema";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPolygon);
  OGRLayer* lyr = ds.ICreateLayer("schema_lyr", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

  // 1. CreateField with various types and subtypes
  const struct {
    const char* name;
    OGRFieldType type;
    OGRFieldSubType subType;
    int width;
    int precision;
  } kFieldDefs[] = {
      {"INT_FLD", OFTInteger, OFSTNone, 8, 0},
      {"BOOL_FLD", OFTInteger, OFSTBoolean, 1, 0},
      {"INT16_FLD", OFTInteger, OFSTInt16, 5, 0},
      {"INT64_FLD", OFTInteger64, OFSTNone, 18, 0},
      {"REAL_FLD", OFTReal, OFSTNone, 12, 4},
      {"FLOAT_FLD", OFTReal, OFSTFloat32, 10, 3},
      {"STR_FLD", OFTString, OFSTNone, 50, 0},
      {"DATE_FLD", OFTDate, OFSTNone, 10, 0},
      {"DATETIME", OFTDateTime, OFSTNone, 20, 0},
  };

  for (const auto& fd : kFieldDefs) {
    OGRFieldDefn fld(fd.name, fd.type);
    fld.SetSubType(fd.subType);
    fld.SetWidth(fd.width);
    fld.SetPrecision(fd.precision);
    EXPECT_EQ(shapeLyr->CreateField(&fld), OGRERR_NONE);
  }
  EXPECT_EQ(shapeLyr->GetLayerDefn()->GetFieldCount(), 9);

  // OFTTime is not supported on Shapefiles
  {
    OGRFieldDefn fldTime("TIME_FLD", OFTTime);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->CreateField(&fldTime), OGRERR_FAILURE);
  }

  // Duplicate name and name laundering
  {
    OGRFieldDefn fldLaunder("bad.name with spaces", OFTString);
    EXPECT_EQ(shapeLyr->CreateField(&fldLaunder), OGRERR_NONE);
    EXPECT_STREQ(shapeLyr->GetLayerDefn()->GetFieldDefn(9)->GetNameRef(),
                 "bad.name w");

    // Colliding truncated name
    OGRFieldDefn fldCollision("bad.name with more spaces", OFTString);
    EXPECT_EQ(shapeLyr->CreateField(&fldCollision), OGRERR_NONE);
    EXPECT_STREQ(shapeLyr->GetLayerDefn()->GetFieldDefn(10)->GetNameRef(),
                 "bad.name_1");
  }

  // Unsupported type with bApproxOK = FALSE
  {
    OGRFieldDefn fldList("INT_LIST", OFTIntegerList);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->CreateField(&fldList, FALSE), OGRERR_FAILURE);
  }

  // 2. AlterFieldDefn
  {
    // Change name
    OGRFieldDefn alteredName("NEW_INT", OFTInteger);
    EXPECT_EQ(shapeLyr->AlterFieldDefn(0, &alteredName, ALTER_NAME_FLAG),
              OGRERR_NONE);
    EXPECT_STREQ(shapeLyr->GetLayerDefn()->GetFieldDefn(0)->GetNameRef(),
                 "NEW_INT");

    // Change type from OFTInteger to OFTInteger64
    OGRFieldDefn alteredToInt64("NEW_INT", OFTInteger64);
    EXPECT_EQ(shapeLyr->AlterFieldDefn(0, &alteredToInt64, ALTER_TYPE_FLAG),
              OGRERR_NONE);
    EXPECT_EQ(shapeLyr->GetLayerDefn()->GetFieldDefn(0)->GetType(),
              OFTInteger64);

    // Change type from OFTInteger64 to OFTString and change width
    OGRFieldDefn alteredToStr("NEW_STR", OFTString);
    alteredToStr.SetWidth(30);
    EXPECT_EQ(shapeLyr->AlterFieldDefn(0, &alteredToStr,
                                       ALTER_NAME_FLAG | ALTER_TYPE_FLAG |
                                           ALTER_WIDTH_PRECISION_FLAG),
              OGRERR_NONE);
    EXPECT_EQ(shapeLyr->GetLayerDefn()->GetFieldDefn(0)->GetType(), OFTString);
    EXPECT_STREQ(shapeLyr->GetLayerDefn()->GetFieldDefn(0)->GetNameRef(),
                 "NEW_STR");

    // Invalid type conversion: OFTString to OFTReal (can only convert to
    // OFTString)
    OGRFieldDefn alteredToReal("NEW_STR", OFTReal);
    {
      WithQuietHandler quiet;
      EXPECT_EQ(shapeLyr->AlterFieldDefn(0, &alteredToReal, ALTER_TYPE_FLAG),
                OGRERR_FAILURE);
    }

    // Out of range index
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->AlterFieldDefn(100, &alteredToStr, ALTER_ALL_FLAG),
              OGRERR_FAILURE);
    EXPECT_EQ(shapeLyr->AlterFieldDefn(-1, &alteredToStr, ALTER_ALL_FLAG),
              OGRERR_FAILURE);
  }

  // 3. ReorderFields
  {
    const int nFields = shapeLyr->GetLayerDefn()->GetFieldCount();
    std::vector<int> map(nFields);
    for (int i = 0; i < nFields; ++i) map[i] = nFields - 1 - i;
    EXPECT_EQ(shapeLyr->ReorderFields(map.data()), OGRERR_NONE);

    // Invalid map
    std::vector<int> badMap(nFields, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->ReorderFields(badMap.data()), OGRERR_FAILURE);
  }

  // 4. DeleteField
  {
    const int countBefore = shapeLyr->GetLayerDefn()->GetFieldCount();
    EXPECT_EQ(shapeLyr->DeleteField(0), OGRERR_NONE);
    EXPECT_EQ(shapeLyr->GetLayerDefn()->GetFieldCount(), countBefore - 1);

    // Out of range index
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->DeleteField(-1), OGRERR_FAILURE);
    EXPECT_EQ(shapeLyr->DeleteField(100), OGRERR_FAILURE);
  }

  // 5. Encoding edge cases: invalid encoding, ASCII conversion failure, empty
  // encoding
  {
    const char* const papszInvalidEnc[] = {
        "ENCODING=INVALID_NONEXISTENT_ENCODING", nullptr};
    OGRGeomFieldDefn gf("geom", wkbPoint);
    OGRLayer* lyrInv = ds.ICreateLayer("inv_enc", &gf, papszInvalidEnc);
    ASSERT_THAT(lyrInv, NotNull());

    const char* const papszLatin1Enc[] = {"ENCODING=ISO-8859-1", nullptr};
    OGRLayer* lyrLatin1 = ds.ICreateLayer("latin1_lyr", &gf, papszLatin1Enc);
    ASSERT_THAT(lyrLatin1, NotNull());
    OGRShapeLayer* shapeLatin1 = cpl::down_cast<OGRShapeLayer*>(lyrLatin1);

    // Euro sign (\xE2\x82\xAC) cannot be represented in ISO-8859-1
    OGRFieldDefn fldNonLatin1("\xE2\x82\xAC", OFTString);
    {
      WithQuietHandler quiet;
      EXPECT_EQ(shapeLatin1->CreateField(&fldNonLatin1), OGRERR_FAILURE);
    }

    CPLErrorReset();
    OGRFieldDefn fldValid("VALID_NAME", OFTString);
    EXPECT_EQ(shapeLatin1->CreateField(&fldValid), OGRERR_NONE);

    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EXPECT_EQ(shapeLatin1->AlterFieldDefn(0, &fldNonLatin1, ALTER_NAME_FLAG),
                OGRERR_FAILURE);
    }

    const char* const papszEmptyEnc[] = {"ENCODING=", nullptr};
    OGRLayer* lyrNoEnc = ds.ICreateLayer("no_enc_lyr", &gf, papszEmptyEnc);
    ASSERT_THAT(lyrNoEnc, NotNull());
    OGRShapeLayer* shapeNoEnc = cpl::down_cast<OGRShapeLayer*>(lyrNoEnc);

    EXPECT_FALSE(shapeNoEnc->TestCapability(OLCStringsAsUTF8));

    OGRFieldDefn fldCol("SIMPLE_COL", OFTString);
    EXPECT_EQ(shapeNoEnc->CreateField(&fldCol), OGRERR_NONE);

    OGRFieldDefn fldRenamed("RENAMED_COL", OFTString);
    EXPECT_EQ(shapeNoEnc->AlterFieldDefn(0, &fldRenamed, ALTER_NAME_FLAG),
              OGRERR_NONE);
  }
}

// 5. Spatial and attribute filters, QIX and SBN spatial indices
TEST(OGRShapeLayerTest, SpatialAndAttributeFiltersWithIndices) {
  constexpr const char* kDir = "/vsimem/osl_idx";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPoint);
  OGRLayer* lyr = ds.ICreateLayer("idx_lyr", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

  OGRFieldDefn fldName("NAME", OFTString);
  EXPECT_EQ(lyr->CreateField(&fldName), OGRERR_NONE);
  OGRFieldDefn fldNum("NUM", OFTInteger);
  EXPECT_EQ(lyr->CreateField(&fldNum), OGRERR_NONE);

  for (int i = 0; i < 20; ++i) {
    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetField("NAME", (i % 2 == 0) ? "EVEN" : "ODD");
    feat.SetField("NUM", i);
    OGRPoint pt(static_cast<double>(i), static_cast<double>(i));
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
  }

  // 1. Attribute filter
  EXPECT_EQ(lyr->SetAttributeFilter("NAME = 'EVEN'"), OGRERR_NONE);
  EXPECT_EQ(lyr->GetFeatureCount(), 10);
  lyr->ResetReading();
  int evenCount = 0;
  while (OGRFeature* f = lyr->GetNextFeature()) {
    EXPECT_STREQ(f->GetFieldAsString("NAME"), "EVEN");
    delete f;
    evenCount++;
  }
  EXPECT_EQ(evenCount, 10);
  EXPECT_EQ(lyr->SetAttributeFilter(nullptr), OGRERR_NONE);

  // 2. Spatial filter without index
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(5, 0);
  ring.addPoint(5, 5);
  ring.addPoint(0, 5);
  ring.addPoint(0, 0);
  poly.addRing(&ring);

  lyr->SetSpatialFilter(&poly);
  EXPECT_EQ(lyr->GetFeatureCount(), 6);  // points (0,0) through (5,5)
  EXPECT_EQ(shapeLyr->GetFeatureCountWithSpatialFilterOnly(), 6);

  // Disjoint filter
  OGRPolygon polyDisjoint;
  OGRLinearRing ringDisjoint;
  ringDisjoint.addPoint(100, 100);
  ringDisjoint.addPoint(105, 100);
  ringDisjoint.addPoint(105, 105);
  ringDisjoint.addPoint(100, 105);
  ringDisjoint.addPoint(100, 100);
  polyDisjoint.addRing(&ringDisjoint);
  lyr->SetSpatialFilter(&polyDisjoint);
  EXPECT_EQ(lyr->GetFeatureCount(), 0);

  // Filter containing whole layer
  OGRPolygon polyAll;
  OGRLinearRing ringAll;
  ringAll.addPoint(-10, -10);
  ringAll.addPoint(100, -10);
  ringAll.addPoint(100, 100);
  ringAll.addPoint(-10, 100);
  ringAll.addPoint(-10, -10);
  polyAll.addRing(&ringAll);
  lyr->SetSpatialFilter(&polyAll);
  EXPECT_EQ(lyr->GetFeatureCount(), 20);

  lyr->SetSpatialFilter(nullptr);

  // 3. Create spatial index (.qix)
  EXPECT_EQ(shapeLyr->CreateSpatialIndex(0), OGRERR_NONE);
  EXPECT_EQ(shapeLyr->CreateSpatialIndex(4), OGRERR_NONE);

  // Spatial query with index active
  lyr->SetSpatialFilter(&poly);
  EXPECT_EQ(lyr->GetFeatureCount(), 6);
  lyr->ResetReading();
  int qixCount = 0;
  while (OGRFeature* f = lyr->GetNextFeature()) {
    delete f;
    qixCount++;
  }
  EXPECT_EQ(qixCount, 6);

  // Attribute filter + spatial filter together with index
  EXPECT_EQ(lyr->SetAttributeFilter("NAME = 'EVEN'"), OGRERR_NONE);
  EXPECT_EQ(lyr->GetFeatureCount(), 3);  // 0, 2, 4
  lyr->ResetReading();
  int comboCount = 0;
  while (OGRFeature* f = lyr->GetNextFeature()) {
    delete f;
    comboCount++;
  }
  EXPECT_EQ(comboCount, 3);
  EXPECT_EQ(lyr->SetAttributeFilter(nullptr), OGRERR_NONE);

  // Test FastFeatureCount capability with spatial index and attribute query
  EXPECT_TRUE(shapeLyr->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCFastSpatialFilter));
  lyr->SetAttributeFilter("NUM < 10");
  EXPECT_FALSE(shapeLyr->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(lyr->SetAttributeFilter(nullptr), OGRERR_NONE);
  lyr->SetSpatialFilter(nullptr);

  // Drop spatial index
  EXPECT_EQ(shapeLyr->DropSpatialIndex(), OGRERR_NONE);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->DropSpatialIndex(),
              OGRERR_FAILURE);  // 2nd drop fails (no index)
  }

  // SBN / SBX index testing
  {
    std::string sbnPath = std::string(kDir) + "/idx_lyr.sbn";
    std::string sbxPath = std::string(kDir) + "/idx_lyr.sbx";
    std::vector<unsigned char> sbn(108, 0);
    sbn[0] = 0x00;
    sbn[1] = 0x00;
    sbn[2] = 0x27;
    sbn[3] = 0x0A;
    sbn[4] = 0xFF;
    sbn[5] = 0xFF;
    sbn[6] = 0xFE;
    sbn[7] = 0x70;
    WriteMSBInt(sbn.data() + 28, 0);  // shape count = 0 (empty index)
    WriteMSBDouble(sbn.data() + 32, 0.0);
    WriteMSBDouble(sbn.data() + 40, 0.0);
    WriteMSBDouble(sbn.data() + 48, 100.0);
    WriteMSBDouble(sbn.data() + 56, 100.0);
    VSILFILE* fpSbn = VSIFOpenL(sbnPath.c_str(), "wb");
    ASSERT_THAT(fpSbn, NotNull());
    VSIFWriteL(sbn.data(), 1, sbn.size(), fpSbn);
    VSIFCloseL(fpSbn);

    VSILFILE* fpSbx = VSIFOpenL(sbxPath.c_str(), "wb");
    ASSERT_THAT(fpSbx, NotNull());
    VSIFCloseL(fpSbx);

    {
      OGRShapeDataSource dsSBN;
      dsSBN.SetDescription(kDir);
      GDALOpenInfo oiSBN(kDir, GA_Update);
      ASSERT_TRUE(dsSBN.Open(&oiSBN, false));
      OGRLayer* lyrSBN = dsSBN.GetLayerByName("idx_lyr");
      ASSERT_THAT(lyrSBN, NotNull());
      OGRShapeLayer* shapeSBN = cpl::down_cast<OGRShapeLayer*>(lyrSBN);

      lyrSBN->SetSpatialFilter(&poly);
      std::unique_ptr<OGRFeature> fSbn(lyrSBN->GetNextFeature());

      CPLStringList flSBN;
      shapeSBN->AddToFileList(flSBN);
      // Let dsSBN close so ~OGRShapeLayer() closes m_hSBN via
      // SBNCloseDiskTree(m_hSBN)
    }

    {
      OGRShapeDataSource dsSBN2;
      dsSBN2.SetDescription(kDir);
      GDALOpenInfo oiSBN2(kDir, GA_Update);
      ASSERT_TRUE(dsSBN2.Open(&oiSBN2, false));
      OGRLayer* lyrSBN2 = dsSBN2.GetLayerByName("idx_lyr");
      ASSERT_THAT(lyrSBN2, NotNull());
      OGRShapeLayer* shapeSBN2 = cpl::down_cast<OGRShapeLayer*>(lyrSBN2);

      EXPECT_TRUE(shapeSBN2->TestCapability(OLCFastSpatialFilter));
      EXPECT_EQ(shapeSBN2->DropSpatialIndex(), OGRERR_NONE);
    }
  }

  // 4. Test spatial filter on non-point (Polygon) geometries
  {
    OGRGeomFieldDefn gfPoly("geom", wkbPolygon);
    OGRLayer* polyLyr = ds.ICreateLayer("poly_idx_lyr", &gfPoly, nullptr);
    ASSERT_THAT(polyLyr, NotNull());
    OGRShapeLayer* polyShapeLyr = cpl::down_cast<OGRShapeLayer*>(polyLyr);

    // Create a polygon from (0,0) to (10,10)
    OGRFeature feat1(polyLyr->GetLayerDefn());
    OGRPolygon p1;
    OGRLinearRing r1;
    r1.addPoint(0, 0);
    r1.addPoint(10, 0);
    r1.addPoint(10, 10);
    r1.addPoint(0, 10);
    r1.addPoint(0, 0);
    p1.addRing(&r1);
    feat1.SetGeometry(&p1);
    EXPECT_EQ(polyLyr->CreateFeature(&feat1), OGRERR_NONE);

    // Create another polygon from (20,20) to (30,30)
    OGRFeature feat2(polyLyr->GetLayerDefn());
    OGRPolygon p2;
    OGRLinearRing r2;
    r2.addPoint(20, 20);
    r2.addPoint(30, 20);
    r2.addPoint(30, 30);
    r2.addPoint(20, 30);
    r2.addPoint(20, 20);
    p2.addRing(&r2);
    feat2.SetGeometry(&p2);
    EXPECT_EQ(polyLyr->CreateFeature(&feat2), OGRERR_NONE);

    polyLyr->SetSpatialFilterRect(5, 5, 15, 15);
    EXPECT_EQ(polyLyr->GetFeatureCount(), 1);
    EXPECT_EQ(polyShapeLyr->GetFeatureCountWithSpatialFilterOnly(), 1);

    polyLyr->SetSpatialFilterRect(100, 100, 110, 110);
    EXPECT_EQ(polyLyr->GetFeatureCount(), 0);

    polyLyr->SetSpatialFilter(nullptr);
    EXPECT_EQ(polyLyr->GetFeatureCount(), 2);
  }
}

// 6. Extents (2D and 3D), TestCapability, Rename, FileList, and Layer lifecycle
TEST(OGRShapeLayerTest, ExtentsCapabilitiesAndLifecycle) {
  constexpr const char* kDir = "/vsimem/osl_life";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPoint25D);
  OGRLayer* lyr = ds.ICreateLayer("life_lyr", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

  // 1. Extents on empty layer
  OGREnvelope env;
  EXPECT_EQ(shapeLyr->GetExtent(&env, FALSE), OGRERR_NONE);
  OGREnvelope3D env3D;
  EXPECT_EQ(shapeLyr->GetExtent3D(0, &env3D, FALSE), OGRERR_NONE);

  // Add 3D features
  OGRFeature f1(lyr->GetLayerDefn());
  OGRPoint pt1(10.0, 20.0, 30.0);
  f1.SetGeometry(&pt1);
  EXPECT_EQ(lyr->CreateFeature(&f1), OGRERR_NONE);

  OGRFeature f2(lyr->GetLayerDefn());
  OGRPoint pt2(50.0, 60.0, 70.0);
  f2.SetGeometry(&pt2);
  EXPECT_EQ(lyr->CreateFeature(&f2), OGRERR_NONE);

  EXPECT_EQ(shapeLyr->GetExtent(&env, FALSE), OGRERR_NONE);
  EXPECT_DOUBLE_EQ(env.MinX, 10.0);
  EXPECT_DOUBLE_EQ(env.MaxX, 50.0);
  EXPECT_DOUBLE_EQ(env.MinY, 20.0);
  EXPECT_DOUBLE_EQ(env.MaxY, 60.0);

  EXPECT_EQ(shapeLyr->GetExtent3D(0, &env3D, FALSE), OGRERR_NONE);
  EXPECT_DOUBLE_EQ(env3D.MinZ, 30.0);
  EXPECT_DOUBLE_EQ(env3D.MaxZ, 70.0);

  // 2. TestCapability
  EXPECT_TRUE(shapeLyr->TestCapability(OLCRandomRead));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCSequentialWrite));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCRandomWrite));
  EXPECT_FALSE(shapeLyr->TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCFastGetExtent));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCCreateField));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCDeleteField));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCReorderFields));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCAlterFieldDefn));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCAlterGeomFieldDefn));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCDeleteFeature));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCStringsAsUTF8));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCIgnoreFields));
  EXPECT_TRUE(shapeLyr->TestCapability(OLCZGeometries));
  EXPECT_FALSE(shapeLyr->TestCapability("NonExistentCap"));

  // 3. Rename
  EXPECT_EQ(shapeLyr->Rename("life_renamed"), OGRERR_NONE);
  EXPECT_STREQ(shapeLyr->GetName(), "life_renamed");

  // Rename collision
  {
    OGRGeomFieldDefn gfOther("geom", wkbPoint);
    ASSERT_THAT(ds.ICreateLayer("collision_target", &gfOther, nullptr),
                NotNull());
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->Rename("collision_target"), OGRERR_FAILURE);
  }

  // Rename with invalid characters (needs laundering)
  {
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->Rename("bad/name"), OGRERR_FAILURE);
  }

  // Rename collision with an existing file on disk
  {
    std::string existingFile = std::string(kDir) + "/file_exists.shp";
    VSILFILE* fp = VSIFOpenL(existingFile.c_str(), "wb");
    if (fp) VSIFCloseL(fp);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->Rename("file_exists"), OGRERR_FAILURE);
    VSIUnlink(existingFile.c_str());
  }

  // Rename layer that has a .prj file
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    OGRGeomFieldDefn gfd("geom", wkbPoint);
    gfd.SetSpatialRef(&srs);
    OGRLayer* lyrPrj = ds.ICreateLayer("prj_rename", &gfd, nullptr);
    ASSERT_THAT(lyrPrj, NotNull());
    OGRShapeLayer* shapePrj = cpl::down_cast<OGRShapeLayer*>(lyrPrj);
    EXPECT_EQ(shapePrj->Rename("prj_renamed_ok"), OGRERR_NONE);
  }

  // 4. FileList and GetDataset
  EXPECT_THAT(shapeLyr->GetDataset(), Eq(&ds));
  CPLStringList fl;
  shapeLyr->AddToFileList(fl);
  EXPECT_GT(fl.size(), 0);

  // 5. UpdateFollowingDeOrRecompression (requires zip dataset)
  {
    constexpr const char* kZipDir = "/vsimem/osl_life_zip";
    VSIMkdir(kZipDir, 0755);
    auto clZipDir =
        absl::MakeCleanup([kZipDir]() { VSIRmdirRecursive(kZipDir); });
    const std::string zipPath = "/vsimem/osl_life_zip/test.zip";
    OGRShapeDataSource dsZip;
    dsZip.SetDescription(zipPath.c_str());
    ASSERT_TRUE(dsZip.CreateZip(zipPath.c_str()));
    const char* const papszOpts[] = {"ENCODING=UTF-8", nullptr};
    OGRGeomFieldDefn gfZip("geom", wkbNone);
    OGRLayer* lyrZip = dsZip.ICreateLayer("zip_lyr", &gfZip, papszOpts);
    ASSERT_THAT(lyrZip, NotNull());
    OGRShapeLayer* shapeZipLyr = cpl::down_cast<OGRShapeLayer*>(lyrZip);
    shapeZipLyr->UpdateFollowingDeOrRecompression();
  }
}

// 7. Arrow Array Interface (optimized and fallback paths)
TEST(OGRShapeLayerTest, ArrowArrayStreamInterface) {
  constexpr const char* kDir = "/vsimem/osl_arrow";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gf("geom", wkbPoint);
  OGRLayer* lyr = ds.ICreateLayer("arrow_lyr", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

  OGRFieldDefn fldVal("VAL", OFTInteger);
  EXPECT_EQ(lyr->CreateField(&fldVal), OGRERR_NONE);

  for (int i = 0; i < 5; ++i) {
    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetField("VAL", i * 10);
    OGRPoint pt(static_cast<double>(i), static_cast<double>(i));
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
  }

  // 1. Fallback path (fields are not ignored)
  struct ArrowArrayStream stream;
  ASSERT_TRUE(lyr->GetArrowStream(&stream, nullptr));
  struct ArrowArray array;
  ASSERT_EQ(stream.get_next(&stream, &array), 0);
  EXPECT_EQ(array.length, 5);
  if (array.release) array.release(&array);

  // Second batch -> returns 0 (EOF)
  ASSERT_EQ(stream.get_next(&stream, &array), 0);
  EXPECT_EQ(array.length, 0);
  if (array.release) array.release(&array);
  if (stream.release) stream.release(&stream);

  const char* usedOpt = shapeLyr->GetMetadataItem(
      "LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH", "__DEBUG__");
  EXPECT_THAT(usedOpt, NotNull());
  EXPECT_STREQ(usedOpt, "NO");

  // Delete feature 1 to test optimized path skipping deleted shapes
  shapeLyr->SetAutoRepack(false);
  EXPECT_EQ(shapeLyr->DeleteFeature(1), OGRERR_NONE);

  // 2. Optimized path: ignore geometry and all fields
  lyr->ResetReading();
  lyr->GetLayerDefn()->GetGeomFieldDefn(0)->SetIgnored(TRUE);
  lyr->GetLayerDefn()->GetFieldDefn(0)->SetIgnored(TRUE);

  // Test with INCLUDE_FID=NO option (falls back to generic implementation)
  {
    const char* const papszArrowOpts[] = {"INCLUDE_FID=NO", nullptr};
    ASSERT_TRUE(lyr->GetArrowStream(&stream, papszArrowOpts));
    ASSERT_EQ(stream.get_next(&stream, &array), 0);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // Optimized path: skips deleted shape 1, gets 4 shapes
  lyr->ResetReading();
  ASSERT_TRUE(lyr->GetArrowStream(&stream, nullptr));
  ASSERT_EQ(stream.get_next(&stream, &array), 0);
  EXPECT_EQ(array.length, 4);
  if (array.release) array.release(&array);

  // Second call on optimized path -> nCount == 0 releases and zeroes array
  ASSERT_EQ(stream.get_next(&stream, &array), 0);
  EXPECT_EQ(array.length, 0);
  if (array.release) array.release(&array);
  if (stream.release) stream.release(&stream);

  usedOpt = shapeLyr->GetMetadataItem(
      "LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH", "__DEBUG__");
  EXPECT_THAT(usedOpt, NotNull());
  EXPECT_STREQ(usedOpt, "YES");

  // Fields ignored but geometry NOT ignored -> falls back
  lyr->ResetReading();
  lyr->GetLayerDefn()->GetGeomFieldDefn(0)->SetIgnored(FALSE);
  ASSERT_TRUE(lyr->GetArrowStream(&stream, nullptr));
  ASSERT_EQ(stream.get_next(&stream, &array), 0);
  if (array.release) array.release(&array);
  if (stream.release) stream.release(&stream);

  // MAX_FEATURES_IN_BATCH=1 hits batch boundary in optimized path (line 3934)
  lyr->ResetReading();
  lyr->GetLayerDefn()->GetGeomFieldDefn(0)->SetIgnored(TRUE);
  lyr->GetLayerDefn()->GetFieldDefn(0)->SetIgnored(TRUE);
  {
    const char* const papszBatch1Opts[] = {"MAX_FEATURES_IN_BATCH=1", nullptr};
    ASSERT_TRUE(lyr->GetArrowStream(&stream, papszBatch1Opts));
    ASSERT_EQ(stream.get_next(&stream, &array), 0);
    EXPECT_EQ(array.length, 1);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
  }

  // Active spatial filter falls back to generic GetNextArrowArray (lines
  // 3884-3886)
  lyr->ResetReading();
  lyr->GetLayerDefn()->GetGeomFieldDefn(0)->SetIgnored(FALSE);
  lyr->GetLayerDefn()->GetFieldDefn(0)->SetIgnored(FALSE);
  lyr->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);
  ASSERT_TRUE(lyr->GetArrowStream(&stream, nullptr));
  ASSERT_EQ(stream.get_next(&stream, &array), 0);
  EXPECT_GT(array.length, 0);
  if (array.release) array.release(&array);
  if (stream.release) stream.release(&stream);
  lyr->SetSpatialFilter(nullptr);
}

// 8. Read-only dataset operations failing gracefully via StartUpdate
TEST(OGRShapeLayerTest, ReadOnlyOperations) {
  constexpr const char* kDir = "/vsimem/osl_readonly";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  // Create a layer first
  {
    OGRShapeDataSource ds;
    ds.SetDescription(kDir);
    GDALOpenInfo oi(kDir, GA_Update);
    ASSERT_TRUE(ds.Open(&oi, false));
    OGRGeomFieldDefn gf("geom", wkbPoint);
    OGRLayer* lyr = ds.ICreateLayer("ro_lyr", &gf, nullptr);
    ASSERT_THAT(lyr, NotNull());
    OGRFieldDefn fld("ID", OFTInteger);
    EXPECT_EQ(lyr->CreateField(&fld), OGRERR_NONE);
    OGRFeature feat(lyr->GetLayerDefn());
    feat.SetField("ID", 1);
    OGRPoint pt(1.0, 2.0);
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
  }

  // Now open as ReadOnly
  {
    OGRShapeDataSource dsRO;
    dsRO.SetDescription(kDir);
    GDALOpenInfo oiRO(kDir, GA_ReadOnly);
    ASSERT_TRUE(dsRO.Open(&oiRO, false));
    OGRLayer* lyrRO = dsRO.GetLayerByName("ro_lyr");
    ASSERT_THAT(lyrRO, NotNull());
    OGRShapeLayer* shapeRO = cpl::down_cast<OGRShapeLayer*>(lyrRO);

    WithQuietHandler quiet;
    OGRFeature feat(lyrRO->GetLayerDefn());
    OGRPoint pt(3.0, 4.0);
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyrRO->CreateFeature(&feat), OGRERR_FAILURE);
    EXPECT_EQ(lyrRO->SetFeature(&feat), OGRERR_FAILURE);
    EXPECT_EQ(lyrRO->DeleteFeature(0), OGRERR_FAILURE);

    OGRFieldDefn fldNew("NEW_FLD", OFTString);
    EXPECT_EQ(lyrRO->CreateField(&fldNew), OGRERR_FAILURE);
    EXPECT_EQ(lyrRO->DeleteField(0), OGRERR_FAILURE);
    int panMap[] = {0};
    EXPECT_EQ(lyrRO->ReorderFields(panMap), OGRERR_FAILURE);

    EXPECT_EQ(shapeRO->AlterFieldDefn(0, &fldNew, ALTER_NAME_FLAG),
              OGRERR_FAILURE);
    OGRGeomFieldDefn gfdNew("geom", wkbPoint);
    EXPECT_EQ(
        shapeRO->AlterGeomFieldDefn(0, &gfdNew, ALTER_GEOM_FIELD_DEFN_SRS_FLAG),
        OGRERR_FAILURE);
    EXPECT_EQ(shapeRO->CreateSpatialIndex(0), OGRERR_FAILURE);
    EXPECT_EQ(shapeRO->DropSpatialIndex(), OGRERR_FAILURE);
    EXPECT_EQ(shapeRO->Repack(), OGRERR_FAILURE);
    EXPECT_EQ(shapeRO->ResizeDBF(), OGRERR_FAILURE);
  }
}

// 9. Field creation edge cases, late DBF creation, width truncation,
// collisions, and ResizeDBF
TEST(OGRShapeLayerTest, FieldCreationEdgeCasesAndResize) {
  constexpr const char* kDir = "/vsimem/osl_fld_edges";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  // 1. Create layer with geometry FIRST (no fields)
  OGRGeomFieldDefn gf("geom", wkbPoint);
  OGRLayer* lyr = ds.ICreateLayer("late_dbf", &gf, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

  // Add 3 features with no DBF fields existing
  for (int i = 0; i < 3; ++i) {
    OGRFeature feat(lyr->GetLayerDefn());
    OGRPoint pt(i, i);
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
  }

  // Now create the first field (triggers DBF creation and DBFWriteNULLAttribute
  // for existing shapes)
  OGRFieldDefn fldStr("STR_COL", OFTString);
  fldStr.SetWidth(50);
  EXPECT_EQ(lyr->CreateField(&fldStr), OGRERR_NONE);

  // Field with width > 255 (triggers warning and truncation to 255)
  {
    OGRFieldDefn fldWide("WIDE_COL", OFTString);
    fldWide.SetWidth(300);
    WithQuietHandler quiet;
    EXPECT_EQ(lyr->CreateField(&fldWide), OGRERR_NONE);
    EXPECT_EQ(lyr->GetLayerDefn()->GetFieldDefn(1)->GetWidth(), 254);
  }

  // Duplicate field name with bApproxOK = FALSE
  {
    OGRFieldDefn fldDup("STR_COL", OFTString);
    WithQuietHandler quiet;
    EXPECT_EQ(shapeLyr->CreateField(&fldDup, FALSE), OGRERR_FAILURE);
  }

  // Populate string values and test ResizeDBF
  {
    std::unique_ptr<OGRFeature> f0(shapeLyr->GetFeature(0));
    ASSERT_THAT(f0, NotNull());
    f0->SetField("STR_COL", "short");
    EXPECT_EQ(shapeLyr->SetFeature(f0.get()), OGRERR_NONE);
  }

  // ResizeDBF shrinks column width and truncates DBF
  EXPECT_EQ(shapeLyr->ResizeDBF(), OGRERR_NONE);
  EXPECT_LE(shapeLyr->GetLayerDefn()->GetFieldDefn(0)->GetWidth(), 50);

  // Colliding field names loop (> 10, > 100, error)
  {
    WithQuietHandler quiet;
    for (int i = 1; i <= 101; ++i) {
      OGRFieldDefn fldColl("COLLISION_LONG_NAME", OFTInteger);
      if (i <= 100) {
        EXPECT_EQ(shapeLyr->CreateField(&fldColl), OGRERR_NONE);
      } else {
        EXPECT_EQ(shapeLyr->CreateField(&fldColl), OGRERR_FAILURE);
      }
    }
  }

  // Create up to 256 fields (triggers 256th field warning)
  {
    WithQuietHandler quiet;
    int currentFields = shapeLyr->GetLayerDefn()->GetFieldCount();
    for (int i = currentFields; i < 256; ++i) {
      OGRFieldDefn fldN(CPLSPrintf("F_%d", i), OFTInteger);
      shapeLyr->CreateField(&fldN);
    }
  }

  // ResizeDBF on layer with no string/int columns (only OFTReal)
  {
    OGRGeomFieldDefn gfReal("geom", wkbPoint);
    OGRLayer* lyrReal = ds.ICreateLayer("real_lyr", &gfReal, nullptr);
    ASSERT_THAT(lyrReal, NotNull());
    OGRShapeLayer* shapeReal = cpl::down_cast<OGRShapeLayer*>(lyrReal);
    OGRFieldDefn fldReal("VAL", OFTReal);
    EXPECT_EQ(lyrReal->CreateField(&fldReal), OGRERR_NONE);
    EXPECT_EQ(shapeReal->ResizeDBF(), OGRERR_NONE);
  }

  // ResizeDBF on layer with deleted records
  {
    OGRGeomFieldDefn gfDel("geom", wkbPoint);
    OGRLayer* lyrDel = ds.ICreateLayer("del_resize", &gfDel, nullptr);
    ASSERT_THAT(lyrDel, NotNull());
    OGRShapeLayer* shapeDel = cpl::down_cast<OGRShapeLayer*>(lyrDel);
    OGRFieldDefn fldStr("STR", OFTString);
    EXPECT_EQ(lyrDel->CreateField(&fldStr), OGRERR_NONE);

    for (int i = 0; i < 2; ++i) {
      OGRFeature f(lyrDel->GetLayerDefn());
      f.SetField("STR", "hello");
      OGRPoint pt(i, i);
      f.SetGeometry(&pt);
      EXPECT_EQ(lyrDel->CreateFeature(&f), OGRERR_NONE);
    }
    shapeDel->SetAutoRepack(false);
    EXPECT_EQ(shapeDel->DeleteFeature(0), OGRERR_NONE);
    EXPECT_EQ(shapeDel->ResizeDBF(), OGRERR_NONE);
  }

  // RecomputeExtent on PointZM layer with 2 points
  {
    OGRGeomFieldDefn gfZM("geom", wkbPointZM);
    OGRLayer* lyrZM = ds.ICreateLayer("zm_lyr", &gfZM, nullptr);
    ASSERT_THAT(lyrZM, NotNull());
    OGRShapeLayer* shapeZM = cpl::down_cast<OGRShapeLayer*>(lyrZM);

    OGRFeature feat1(lyrZM->GetLayerDefn());
    OGRPoint pt1(1.0, 2.0, 3.0, 4.0);
    feat1.SetGeometry(&pt1);
    EXPECT_EQ(lyrZM->CreateFeature(&feat1), OGRERR_NONE);

    OGRFeature feat2(lyrZM->GetLayerDefn());
    OGRPoint pt2(10.0, 20.0, 30.0, 40.0);
    feat2.SetGeometry(&pt2);
    EXPECT_EQ(lyrZM->CreateFeature(&feat2), OGRERR_NONE);

    shapeZM->SetAutoRepack(false);
    EXPECT_EQ(shapeZM->DeleteFeature(1), OGRERR_NONE);
    EXPECT_EQ(shapeZM->RecomputeExtent(), OGRERR_NONE);
  }
}

// 10. Layer destruction lifecycle flags (auto repack, resize, create spatial
// index at close)
TEST(OGRShapeLayerTest, LayerCloseAndDestructorFlags) {
  constexpr const char* kDir = "/vsimem/osl_dtor_flags";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  {
    OGRShapeDataSource ds;
    ds.SetDescription(kDir);
    GDALOpenInfo oi(kDir, GA_Update);
    ASSERT_TRUE(ds.Open(&oi, false));

    OGRGeomFieldDefn gf("geom", wkbPoint);
    OGRLayer* lyr = ds.ICreateLayer("dtor_lyr", &gf, nullptr);
    ASSERT_THAT(lyr, NotNull());
    OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

    OGRFieldDefn fldName("NAME", OFTString);
    fldName.SetWidth(60);
    EXPECT_EQ(lyr->CreateField(&fldName), OGRERR_NONE);

    for (int i = 0; i < 5; ++i) {
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetField("NAME", "abc");
      OGRPoint pt(i, i);
      feat.SetGeometry(&pt);
      EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
    }

    shapeLyr->SetAutoRepack(false);
    EXPECT_EQ(shapeLyr->DeleteFeature(2), OGRERR_NONE);

    // Set destructor flags
    shapeLyr->SetAutoRepack(true);
    shapeLyr->SetResizeAtClose(true);
    shapeLyr->CreateSpatialIndexAtClose(1);

    // Also create QIX and keep open so destructor closes it
    EXPECT_EQ(shapeLyr->CreateSpatialIndex(0), OGRERR_NONE);
  }

  // Reopen and verify feature 2 was repacked away (count = 4)
  {
    OGRShapeDataSource ds;
    ds.SetDescription(kDir);
    GDALOpenInfo oi(kDir, GA_ReadOnly);
    ASSERT_TRUE(ds.Open(&oi, false));
    OGRLayer* lyr = ds.GetLayerByName("dtor_lyr");
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(lyr->GetFeatureCount(), 4);
  }
}

// 11. Open options: ADJUST_GEOM_TYPE (FIRST_SHAPE and ALL_SHAPES)
TEST(OGRShapeLayerTest, OpenOptionsAdjustGeomType) {
  constexpr const char* kDir = "/vsimem/osl_adjust_geom";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  // Create PointM layer with unused M (-1e38)
  {
    OGRShapeDataSource ds;
    ds.SetDescription(kDir);
    GDALOpenInfo oi(kDir, GA_Update);
    ASSERT_TRUE(ds.Open(&oi, false));

    OGRGeomFieldDefn gf("geom", wkbPointM);
    OGRLayer* lyr = ds.ICreateLayer("m_lyr", &gf, nullptr);
    ASSERT_THAT(lyr, NotNull());

    OGRFeature feat(lyr->GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    pt.setM(-1e38);  // Unused M
    feat.SetGeometry(&pt);
    EXPECT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);

    // Create a second layer with a valid M value to trigger bMIsUsed = true
    OGRLayer* lyrUsed = ds.ICreateLayer("m_used_lyr", &gf, nullptr);
    ASSERT_THAT(lyrUsed, NotNull());
    OGRFeature featUsed(lyrUsed->GetLayerDefn());
    OGRPoint ptUsed(3.0, 4.0);
    ptUsed.setM(10.0);  // Valid M
    featUsed.SetGeometry(&ptUsed);
    EXPECT_EQ(lyrUsed->CreateFeature(&featUsed), OGRERR_NONE);
  }

  const std::string shpPath = std::string(kDir) + "/m_lyr.shp";
  const std::string shpUsedPath = std::string(kDir) + "/m_used_lyr.shp";

  // Reopen with ADJUST_GEOM_TYPE=FIRST_SHAPE
  {
    const char* const papszOpenOpts[] = {"ADJUST_GEOM_TYPE=FIRST_SHAPE",
                                         nullptr};
    OGRShapeDataSource ds;
    ds.SetDescription(shpPath.c_str());
    GDALOpenInfo oi(shpPath.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(papszOpenOpts);
    ASSERT_TRUE(ds.Open(&oi, false));
    OGRLayer* lyr = ds.GetLayerByName("m_lyr");
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(lyr->GetGeomType(),
              wkbPoint);  // adjusted from wkbPointM to wkbPoint
  }

  // Reopen with ADJUST_GEOM_TYPE=ALL_SHAPES on m_lyr (M unused -> adjusted to
  // wkbPoint)
  {
    const char* const papszOpenOpts[] = {"ADJUST_GEOM_TYPE=ALL_SHAPES",
                                         nullptr};
    OGRShapeDataSource ds;
    ds.SetDescription(shpPath.c_str());
    GDALOpenInfo oi(shpPath.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(papszOpenOpts);
    ASSERT_TRUE(ds.Open(&oi, false));
    OGRLayer* lyr = ds.GetLayerByName("m_lyr");
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(lyr->GetGeomType(), wkbPoint);
  }

  // Reopen with ADJUST_GEOM_TYPE=ALL_SHAPES on m_used_lyr (M is used -> kept as
  // wkbPointM)
  {
    const char* const papszOpenOpts[] = {"ADJUST_GEOM_TYPE=ALL_SHAPES",
                                         nullptr};
    OGRShapeDataSource ds;
    ds.SetDescription(shpUsedPath.c_str());
    GDALOpenInfo oi(shpUsedPath.c_str(), GA_ReadOnly);
    oi.papszOpenOptions = const_cast<char**>(papszOpenOpts);
    ASSERT_TRUE(ds.Open(&oi, false));
    OGRLayer* lyr = ds.GetLayerByName("m_used_lyr");
    ASSERT_THAT(lyr, NotNull());
    EXPECT_EQ(lyr->GetGeomType(), wkbPointM);
  }
}

// 12. Extent edge cases and DBF-only layer (wkbNone)
TEST(OGRShapeLayerTest, ExtentEdgeCasesAndDBFOnlyLayer) {
  constexpr const char* kDir = "/vsimem/osl_extent_dbf";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  // 1. Layer with wkbNone (DBF only, no SHP)
  OGRGeomFieldDefn gfNone("geom", wkbNone);
  OGRLayer* lyrNone = ds.ICreateLayer("dbf_only", &gfNone, nullptr);
  ASSERT_THAT(lyrNone, NotNull());
  OGRShapeLayer* shapeNone = cpl::down_cast<OGRShapeLayer*>(lyrNone);

  OGRFieldDefn fld("TEXT", OFTString);
  EXPECT_EQ(lyrNone->CreateField(&fld), OGRERR_NONE);

  for (int i = 0; i < 3; ++i) {
    OGRFeature feat(lyrNone->GetLayerDefn());
    feat.SetField("TEXT", CPLSPrintf("val_%d", i));
    EXPECT_EQ(lyrNone->CreateFeature(&feat), OGRERR_NONE);
  }
  EXPECT_EQ(lyrNone->GetFeatureCount(), 3);
  std::unique_ptr<OGRFeature> f(shapeNone->GetFeature(0));
  ASSERT_THAT(f, NotNull());
  EXPECT_STREQ(f->GetFieldAsString("TEXT"), "val_0");

  // DBF-only layer extent operations failing
  {
    WithQuietHandler quiet;
    EXPECT_EQ(shapeNone->RecomputeExtent(), OGRERR_FAILURE);
    OGREnvelope envNone;
    EXPECT_EQ(shapeNone->GetExtent(&envNone, FALSE), OGRERR_FAILURE);
    EXPECT_EQ(shapeNone->GetExtent(1, &envNone, FALSE), OGRERR_FAILURE);
    OGREnvelope3D env3DNone;
    EXPECT_EQ(shapeNone->GetExtent3D(0, &env3DNone, FALSE), OGRERR_FAILURE);
    EXPECT_EQ(shapeNone->GetExtent3D(1, &env3DNone, FALSE), OGRERR_FAILURE);
  }

  // 2. 2D layer GetExtent3D MinZ/MaxZ infinity check
  OGRGeomFieldDefn gf2D("geom", wkbPoint);
  OGRLayer* lyr2D = ds.ICreateLayer("layer_2d", &gf2D, nullptr);
  ASSERT_THAT(lyr2D, NotNull());
  OGRShapeLayer* shape2D = cpl::down_cast<OGRShapeLayer*>(lyr2D);

  OGRFeature feat2D(lyr2D->GetLayerDefn());
  OGRPoint pt2D(10.0, 20.0);
  feat2D.SetGeometry(&pt2D);
  EXPECT_EQ(lyr2D->CreateFeature(&feat2D), OGRERR_NONE);

  OGREnvelope3D env3D;
  EXPECT_EQ(shape2D->GetExtent3D(0, &env3D, FALSE), OGRERR_NONE);
  EXPECT_TRUE(std::isinf(env3D.MinZ));
  EXPECT_TRUE(std::isinf(env3D.MaxZ));

  // GetExtent3D with active spatial filter
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(50, 0);
  ring.addPoint(50, 50);
  ring.addPoint(0, 50);
  ring.addPoint(0, 0);
  poly.addRing(&ring);
  lyr2D->SetSpatialFilter(&poly);
  EXPECT_EQ(shape2D->GetExtent3D(0, &env3D, TRUE), OGRERR_NONE);
  lyr2D->SetSpatialFilter(nullptr);

  // 3. NaN in header bounds triggers fallback in GetExtent and GetExtent3D
  {
    OGRGeomFieldDefn gfNan("geom", wkbPoint);
    OGRLayer* lyrNan = ds.ICreateLayer("nan_lyr", &gfNan, nullptr);
    ASSERT_THAT(lyrNan, NotNull());
    OGRFeature featNan(lyrNan->GetLayerDefn());
    OGRPoint ptNan(10.0, 20.0);
    featNan.SetGeometry(&ptNan);
    EXPECT_EQ(lyrNan->CreateFeature(&featNan), OGRERR_NONE);
    lyrNan->SyncToDisk();

    std::string nanShpPath = std::string(kDir) + "/nan_lyr.shp";
    VSILFILE* fp = VSIFOpenL(nanShpPath.c_str(), "r+b");
    ASSERT_THAT(fp, NotNull());
    double nanVal = std::numeric_limits<double>::quiet_NaN();
    VSIFSeekL(fp, 36, SEEK_SET);
    VSIFWriteL(&nanVal, sizeof(double), 1, fp);
    VSIFCloseL(fp);

    OGRShapeDataSource dsNan;
    dsNan.SetDescription(kDir);
    GDALOpenInfo oiNan(kDir, GA_ReadOnly);
    ASSERT_TRUE(dsNan.Open(&oiNan, false));
    OGRLayer* lyrReopen = dsNan.GetLayerByName("nan_lyr");
    ASSERT_THAT(lyrReopen, NotNull());
    OGREnvelope envNan;
    EXPECT_EQ(lyrReopen->GetExtent(&envNan, FALSE), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(envNan.MinX, 10.0);
    EXPECT_DOUBLE_EQ(envNan.MinY, 20.0);
    OGREnvelope3D env3DNan;
    EXPECT_EQ(lyrReopen->GetExtent3D(0, &env3DNan, FALSE), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(env3DNan.MinX, 10.0);
    EXPECT_DOUBLE_EQ(env3DNan.MinY, 20.0);
  }
}

// 13. Shapefile without DBF and inconsistent record counts
TEST(OGRShapeLayerTest, ShapefileWithoutDBFAndInconsistentRecords) {
  constexpr const char* kDir = "/vsimem/osl_no_dbf";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  // Create no_dbf.shp with 2 points and NO .dbf file
  {
    std::string shpPath = std::string(kDir) + "/no_dbf.shp";
    SHPHandle hSHP = SHPCreate(shpPath.c_str(), SHPT_POINT);
    ASSERT_THAT(hSHP, NotNull());
    double x = 1.0, y = 2.0;
    SHPObject* obj = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
    SHPWriteObject(hSHP, -1, obj);
    SHPDestroyObject(obj);
    double x2 = 3.0, y2 = 4.0;
    obj = SHPCreateSimpleObject(SHPT_POINT, 1, &x2, &y2, nullptr);
    SHPWriteObject(hSHP, -1, obj);
    SHPDestroyObject(obj);
    SHPClose(hSHP);
  }

  // Open layer with no DBF
  {
    OGRShapeDataSource ds;
    ds.SetDescription(kDir);
    GDALOpenInfo oi(kDir, GA_Update);
    ASSERT_TRUE(ds.Open(&oi, false));
    OGRLayer* lyr = ds.GetLayerByName("no_dbf");
    ASSERT_THAT(lyr, NotNull());
    OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

    // DeleteFeature fails without DBF
    {
      WithQuietHandler quiet;
      EXPECT_EQ(shapeLyr->DeleteFeature(0), OGRERR_FAILURE);
      EXPECT_EQ(shapeLyr->ResizeDBF(), OGRERR_FAILURE);
    }

    // GetNextFeature without DBF hits FetchShape and m_nFeaturesRead (lines
    // 995-996, 1001-1010)
    {
      std::unique_ptr<OGRFeature> fNoDbf(shapeLyr->GetNextFeature());
      ASSERT_THAT(fNoDbf, NotNull());
    }

    // CreateField when m_hDBF is nullptr creates DBF and writes nulls
    OGRFieldDefn fld("NEW_COL", OFTString);
    EXPECT_EQ(shapeLyr->CreateField(&fld), OGRERR_NONE);
    EXPECT_EQ(shapeLyr->GetLayerDefn()->GetFieldCount(), 1);
  }

  // Inconsistent record count: 1 shape in .shp, 2 records in .dbf
  {
    std::string incShpPath = std::string(kDir) + "/inc.shp";
    SHPHandle hIncSHP = SHPCreate(incShpPath.c_str(), SHPT_POINT);
    ASSERT_THAT(hIncSHP, NotNull());
    double x = 1.0, y = 2.0;
    SHPObject* obj = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
    SHPWriteObject(hIncSHP, -1, obj);
    SHPDestroyObject(obj);
    SHPClose(hIncSHP);

    std::string incDbfPath = std::string(kDir) + "/inc.dbf";
    DBFHandle hIncDBF = DBFCreate(incDbfPath.c_str());
    ASSERT_THAT(hIncDBF, NotNull());
    int fldIdx = DBFAddField(hIncDBF, "VAL", FTInteger, 10, 0);
    DBFWriteIntegerAttribute(hIncDBF, 0, fldIdx, 100);
    DBFWriteIntegerAttribute(hIncDBF, 1, fldIdx, 200);
    DBFClose(hIncDBF);

    OGRShapeDataSource dsInc;
    dsInc.SetDescription(kDir);
    GDALOpenInfo oiInc(kDir, GA_ReadOnly);
    ASSERT_TRUE(dsInc.Open(&oiInc, false));
    OGRLayer* lyrInc = dsInc.GetLayerByName("inc");
    ASSERT_THAT(lyrInc, NotNull());
    EXPECT_EQ(lyrInc->GetFeatureCount(), 1);
  }
}

// 14. Degenerate bounding boxes and non-envelope spatial filtering on
// LineString layer
TEST(OGRShapeLayerTest, DegenerateBoundingBoxesAndSpatialFiltering) {
  constexpr const char* kDir = "/vsimem/osl_degen";
  VSIMkdir(kDir, 0755);
  auto clDir = absl::MakeCleanup([kDir]() { VSIRmdirRecursive(kDir); });

  OGRShapeDataSource ds;
  ds.SetDescription(kDir);
  GDALOpenInfo oi(kDir, GA_Update);
  ASSERT_TRUE(ds.Open(&oi, false));

  OGRGeomFieldDefn gfLine("geom", wkbLineString);
  OGRLayer* lyr = ds.ICreateLayer("lines", &gfLine, nullptr);
  ASSERT_THAT(lyr, NotNull());
  OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

  // Feature 0: horizontal line (dfYMin == dfYMax == 5.0)
  {
    OGRFeature f(lyr->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0.0, 5.0);
    ls.addPoint(10.0, 5.0);
    f.SetGeometry(&ls);
    EXPECT_EQ(lyr->CreateFeature(&f), OGRERR_NONE);
  }
  // Feature 1: vertical line (dfXMin == dfXMax == 5.0)
  {
    OGRFeature f(lyr->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(5.0, 0.0);
    ls.addPoint(5.0, 10.0);
    f.SetGeometry(&ls);
    EXPECT_EQ(lyr->CreateFeature(&f), OGRERR_NONE);
  }
  // Feature 2: diagonal line
  {
    OGRFeature f(lyr->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0.0, 0.0);
    ls.addPoint(10.0, 10.0);
    f.SetGeometry(&ls);
    EXPECT_EQ(lyr->CreateFeature(&f), OGRERR_NONE);
  }
  // Feature 3: null geometry
  {
    OGRFeature f(lyr->GetLayerDefn());
    EXPECT_EQ(lyr->CreateFeature(&f), OGRERR_NONE);
  }
  // Feature 4: deleted line
  {
    OGRFeature f(lyr->GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(20.0, 20.0);
    ls.addPoint(30.0, 30.0);
    f.SetGeometry(&ls);
    EXPECT_EQ(lyr->CreateFeature(&f), OGRERR_NONE);
  }
  shapeLyr->SetAutoRepack(false);
  EXPECT_EQ(shapeLyr->DeleteFeature(4), OGRERR_NONE);

  // Non-envelope triangle spatial filter
  OGRPolygon tri;
  OGRLinearRing ring;
  ring.addPoint(0.0, 0.0);
  ring.addPoint(6.0, 0.0);
  ring.addPoint(0.0, 6.0);
  ring.addPoint(0.0, 0.0);
  tri.addRing(&ring);

  lyr->SetSpatialFilter(&tri);
  EXPECT_GT(lyr->GetFeatureCount(TRUE), 0);

  lyr->ResetReading();
  int count = 0;
  while (OGRFeature* f = lyr->GetNextFeature()) {
    delete f;
    count++;
  }
  EXPECT_GT(count, 0);

  // Spatial filter smaller than layer extent triggers bHasTrivialSpatialFilter
  // = false (line 1625) and non-prepared Intersects in
  // GetFeatureCountWithSpatialFilterOnly (line 1574)
  lyr->SetSpatialFilterRect(0.0, 0.0, 6.0, 6.0);
  EXPECT_GT(lyr->GetFeatureCount(), 0);
  lyr->SetSpatialFilter(nullptr);

  // GetExtent and GetExtent3D with invalid geom field index (lines 1681, 1730)
  {
    OGREnvelope envBad;
    EXPECT_EQ(shapeLyr->GetExtent(1, &envBad, TRUE), OGRERR_FAILURE);
    OGREnvelope3D env3DBad;
    EXPECT_EQ(shapeLyr->GetExtent3D(1, &env3DBad, TRUE), OGRERR_FAILURE);
  }
}

// 15. Direct construction with ENCODING create option, NaN bounds fallback,
// in-place truncation vs repack, spatial index drops, and SetNextByIndex filter
TEST(OGRShapeLayerTest, NaNHeaderBoundsRepackAndEncodingEdgeCases) {
  constexpr const char* kRepackDir = "/vsimem/osl_nan_repack";
  VSIMkdir(kRepackDir, 0755);
  auto clRepackDir =
      absl::MakeCleanup([kRepackDir]() { VSIRmdirRecursive(kRepackDir); });

  // 1. Direct OGRShapeLayer construction with ENCODING=INVALID_CHARSET and NaN
  // bounds
  {
    std::string directShp = std::string(kRepackDir) + "/direct.shp";
    std::string directDbf = std::string(kRepackDir) + "/direct.dbf";
    SHPHandle hSHP = SHPCreate(directShp.c_str(), SHPT_POINTZ);
    ASSERT_THAT(hSHP, NotNull());
    DBFHandle hDBF = DBFCreate(directDbf.c_str());
    ASSERT_THAT(hDBF, NotNull());
    int fld = DBFAddField(hDBF, "NUM", FTInteger, 10, 0);
    DBFWriteIntegerAttribute(hDBF, 0, fld, 42);

    double x = 10.0, y = 20.0, z = 30.0, m = 0.0;
    SHPObject* obj = SHPCreateObject(SHPT_POINTZ, -1, 0, nullptr, nullptr, 1,
                                     &x, &y, &z, &m);
    ASSERT_THAT(obj, NotNull());
    SHPWriteObject(hSHP, -1, obj);
    SHPDestroyObject(obj);

    // Corrupt bounds to NaN
    hSHP->adBoundsMin[0] = std::numeric_limits<double>::quiet_NaN();

    OGRShapeDataSource dsDirect;
    dsDirect.SetDescription(kRepackDir);
    GDALOpenInfo oiDirect(kRepackDir, GA_Update);
    ASSERT_TRUE(dsDirect.Open(&oiDirect, false));

    const char* const createOpts[] = {"ENCODING=INVALID_CHARSET", nullptr};
    OGRShapeLayer directLyr(&dsDirect, directShp.c_str(), hSHP, hDBF, nullptr,
                            false, "", true, wkbPoint25D,
                            const_cast<char**>(createOpts));

    OGREnvelope env;
    EXPECT_EQ(directLyr.GetExtent(&env, TRUE), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(env.MinX, 10.0);
    EXPECT_DOUBLE_EQ(env.MinY, 20.0);

    OGREnvelope3D env3D;
    EXPECT_EQ(directLyr.GetExtent3D(0, &env3D, TRUE), OGRERR_NONE);
    EXPECT_DOUBLE_EQ(env3D.MinX, 10.0);
    EXPECT_DOUBLE_EQ(env3D.MinY, 20.0);
    EXPECT_DOUBLE_EQ(env3D.MinZ, 30.0);

    // Delete feature 0 with auto-repack true so directLyr destructor runs
    // Repack() (line 226)
    directLyr.SetAutoRepack(false);
    EXPECT_EQ(directLyr.DeleteFeature(0), OGRERR_NONE);
    directLyr.SetAutoRepack(true);

    // Direct SHP-only construction (hDBF == nullptr) with
    // ENCODING=INVALID_CHARSET
    {
      std::string directNoDbf = std::string(kRepackDir) + "/direct_nodbf.shp";
      SHPHandle hShpNoDbf = SHPCreate(directNoDbf.c_str(), SHPT_POINT);
      ASSERT_THAT(hShpNoDbf, NotNull());
      const char* const badEncOpts[] = {"ENCODING=INVALID_CHARSET", nullptr};
      OGRShapeLayer lyrNoDbf(&dsDirect, directNoDbf.c_str(), hShpNoDbf, nullptr,
                             nullptr, false, "", true, wkbPoint,
                             const_cast<char**>(badEncOpts));
    }

    // Direct SHP+DBF construction where hDBF has a field and pszCodePage is
    // null
    // -> m_osEncoding is initialized to "" from ConvertCodePage(nullptr)
    // -> ENCODING=INVALID_CHARSET is accepted from papszCreateOptions
    // -> TestCapability(OLCStringsAsUTF8) checks DBF field name and fails
    // recode (lines 1864-1866)
    // -> triggers warning/debug and m_osEncoding = "" (lines 128-132)
    {
      std::string encShp = std::string(kRepackDir) + "/enc.shp";
      std::string encDbf = std::string(kRepackDir) + "/enc.dbf";
      SHPHandle hEncSHP = SHPCreate(encShp.c_str(), SHPT_POINT);
      ASSERT_THAT(hEncSHP, NotNull());
      DBFHandle hEncDBF = DBFCreate(encDbf.c_str());
      ASSERT_THAT(hEncDBF, NotNull());
      DBFAddField(hEncDBF, "\x80\x81", FTString, 20, 0);
      hEncDBF->iLanguageDriver = 250;
      free(hEncDBF->pszCodePage);
      hEncDBF->pszCodePage = nullptr;

      const char* const badEncOpts[] = {"ENCODING=INVALID_CHARSET", nullptr};
      OGRShapeLayer encLyr(&dsDirect, encShp.c_str(), hEncSHP, hEncDBF, nullptr,
                           false, "", true, wkbPoint,
                           const_cast<char**>(badEncOpts));
      EXPECT_FALSE(encLyr.TestCapability(OLCStringsAsUTF8));

      // ReorderFields with 0 fields on directNoDbf layer -> hits line 2196
      std::string zeroFldShp = std::string(kRepackDir) + "/zero_fld.shp";
      SHPHandle hZeroSHP = SHPCreate(zeroFldShp.c_str(), SHPT_POINT);
      ASSERT_THAT(hZeroSHP, NotNull());
      OGRShapeLayer zeroFldLyr(&dsDirect, zeroFldShp.c_str(), hZeroSHP, nullptr,
                               nullptr, false, "", true, wkbPoint, nullptr);
      EXPECT_EQ(zeroFldLyr.ReorderFields(nullptr), OGRERR_NONE);
    }
  }

  // 2. Line layer for spatial index drops, shrink truncation, non-last growth
  // repack, and CPG repack
  {
    OGRShapeDataSource dsLine;
    dsLine.SetDescription(kRepackDir);
    GDALOpenInfo oiLine(kRepackDir, GA_Update);
    ASSERT_TRUE(dsLine.Open(&oiLine, false));

    const char* const createOpts[] = {"ENCODING=UTF-8", nullptr};
    OGRGeomFieldDefn gf("geom", wkbLineString);
    OGRLayer* lyr =
        dsLine.ICreateLayer("line_repack", &gf, const_cast<char**>(createOpts));
    ASSERT_THAT(lyr, NotNull());
    OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

    // Repack() on fresh layer with UNKNOWN need -> hits lines 2852-2855
    // (nothing to do)
    EXPECT_EQ(shapeLyr->Repack(), OGRERR_NONE);

    // Create OFTInteger64 field with default width 0 -> triggers nWidth = 18
    // (line 2072)
    OGRFieldDefn fld64("BIG_ID", OFTInteger64);
    EXPECT_EQ(shapeLyr->CreateField(&fld64), OGRERR_NONE);
    EXPECT_EQ(shapeLyr->GetLayerDefn()->GetFieldDefn(0)->GetWidth(), 18);

    auto makeLine = [](double startX, int numPoints) {
      OGRLineString ls;
      for (int i = 0; i < numPoints; ++i) {
        ls.addPoint(startX + i, startX + i);
      }
      return ls;
    };

    // Feature 0: 5 points
    OGRFeature feat0(lyr->GetLayerDefn());
    OGRLineString ls0 = makeLine(0.0, 5);
    feat0.SetGeometry(&ls0);
    feat0.SetField("BIG_ID", static_cast<GIntBig>(100));
    EXPECT_EQ(lyr->CreateFeature(&feat0), OGRERR_NONE);

    // Feature 1: 5 points
    OGRFeature feat1(lyr->GetLayerDefn());
    OGRLineString ls1 = makeLine(10.0, 5);
    feat1.SetGeometry(&ls1);
    feat1.SetField("BIG_ID", static_cast<GIntBig>(200));
    EXPECT_EQ(lyr->CreateFeature(&feat1), OGRERR_NONE);

    // Create spatial index (.qix)
    EXPECT_EQ(shapeLyr->CreateSpatialIndex(0), OGRERR_NONE);

    // AddToFileList while both .cpg and .qix are present
    CPLStringList filesWithCpgAndQix;
    shapeLyr->AddToFileList(filesWithCpgAndQix);

    // CreateFeature (Feature 2: 5 points) -> drops active spatial index (line
    // 1197)
    OGRFeature feat2(lyr->GetLayerDefn());
    OGRLineString ls2 = makeLine(20.0, 5);
    feat2.SetGeometry(&ls2);
    feat2.SetField("BIG_ID", static_cast<GIntBig>(300));
    EXPECT_EQ(lyr->CreateFeature(&feat2), OGRERR_NONE);

    // Recreate spatial index
    EXPECT_EQ(shapeLyr->CreateSpatialIndex(0), OGRERR_NONE);

    // Update Feature 2 (the LAST record in .shp) with smaller geometry (2
    // points)
    // -> drops spatial index (line 1095) and shrinks file with VSIFTruncateL
    // (lines 1122-1125)
    std::unique_ptr<OGRFeature> f2Update(shapeLyr->GetFeature(2));
    ASSERT_THAT(f2Update, NotNull());
    OGRLineString ls2Small = makeLine(20.0, 2);
    f2Update->SetGeometry(&ls2Small);
    EXPECT_EQ(shapeLyr->SetFeature(f2Update.get()), OGRERR_NONE);

    // Recreate spatial index
    EXPECT_EQ(shapeLyr->CreateSpatialIndex(0), OGRERR_NONE);

    // Delete Feature 2 with auto-repack false -> drops spatial index (line
    // 1173)
    shapeLyr->SetAutoRepack(false);
    EXPECT_EQ(shapeLyr->DeleteFeature(2), OGRERR_NONE);

    // Recreate spatial index
    EXPECT_EQ(shapeLyr->CreateSpatialIndex(0), OGRERR_NONE);

    // Call Repack() while .cpg exists and record 2 is deleted
    // -> hits lines 2892-2894 (cpg), 2934 (DropSpatialIndex), 2962-2967
    // (oCPGTempFile)
    EXPECT_EQ(shapeLyr->Repack(), OGRERR_NONE);

    // Call Repack() again when no records deleted and !m_bSHPNeedsRepack
    // -> hits lines 2852-2855 (REPACK: nothing to do)
    EXPECT_EQ(shapeLyr->Repack(), OGRERR_NONE);

    // Now update Feature 0 (a NON-LAST record!) with a larger geometry (12
    // points)
    // -> triggers m_bSHPNeedsRepack = true and m_eNeedRepack = YES (lines
    // 1129-1132)
    std::unique_ptr<OGRFeature> f0Update(shapeLyr->GetFeature(0));
    ASSERT_THAT(f0Update, NotNull());
    OGRLineString ls0Big = makeLine(0.0, 12);
    f0Update->SetGeometry(&ls0Big);
    EXPECT_EQ(shapeLyr->SetFeature(f0Update.get()), OGRERR_NONE);

    // Enable AutoRepack, and let dsLine close without SyncToDisk or
    // ResetReading
    // -> ~OGRShapeLayer() hits line 226: Repack()!
    shapeLyr->SetAutoRepack(true);

    // Layer created with wkbUnknown -> first CreateFeature triggers
    // ResetGeomType (lines 1205-1215, 2550-2589)
    {
      OGRGeomFieldDefn gfUnknown("geom", wkbUnknown);
      OGRLayer* lyrUnknown =
          dsLine.ICreateLayer("unk_lyr", &gfUnknown, nullptr);
      ASSERT_THAT(lyrUnknown, NotNull());
      OGRFeature fUnk(lyrUnknown->GetLayerDefn());
      OGRPoint pt(1.0, 2.0);
      fUnk.SetGeometry(&pt);
      EXPECT_EQ(lyrUnknown->CreateFeature(&fUnk), OGRERR_NONE);
      EXPECT_EQ(lyrUnknown->GetGeomType(), wkbPoint);

      struct UnknownGeomCase {
        const char* name;
        std::unique_ptr<OGRGeometry> geom;
      };
      auto makePt25D = []() {
        return std::make_unique<OGRPoint>(1.0, 2.0, 3.0);
      };
      auto makePtM = []() {
        auto p = std::make_unique<OGRPoint>(1.0, 2.0);
        p->setM(5.0);
        return p;
      };
      auto makePtZM = []() {
        auto p = std::make_unique<OGRPoint>(1.0, 2.0, 3.0);
        p->setM(5.0);
        return p;
      };
      auto makeMPt = []() {
        auto mp = std::make_unique<OGRMultiPoint>();
        OGRPoint p(1.0, 2.0);
        mp->addGeometry(&p);
        return mp;
      };
      auto makeMPt25D = []() {
        auto mp = std::make_unique<OGRMultiPoint>();
        OGRPoint p(1.0, 2.0, 3.0);
        mp->addGeometry(&p);
        return mp;
      };
      auto makeMPtM = []() {
        auto mp = std::make_unique<OGRMultiPoint>();
        OGRPoint p(1.0, 2.0);
        p.setM(4.0);
        mp->addGeometry(&p);
        return mp;
      };
      auto makeMPtZM = []() {
        auto mp = std::make_unique<OGRMultiPoint>();
        OGRPoint p(1.0, 2.0, 3.0);
        p.setM(4.0);
        mp->addGeometry(&p);
        return mp;
      };
      auto makeLS = []() {
        auto ls = std::make_unique<OGRLineString>();
        ls->addPoint(0, 0);
        ls->addPoint(1, 1);
        return ls;
      };
      auto makeLS25D = []() {
        auto ls = std::make_unique<OGRLineString>();
        ls->addPoint(0, 0, 1);
        ls->addPoint(1, 1, 2);
        return ls;
      };
      auto makeLSM = []() {
        auto ls = std::make_unique<OGRLineString>();
        ls->addPoint(0, 0, 0, 1);
        ls->addPoint(1, 1, 0, 2);
        return ls;
      };
      auto makeLSZM = []() {
        auto ls = std::make_unique<OGRLineString>();
        ls->addPoint(0, 0, 1, 1);
        ls->addPoint(1, 1, 2, 2);
        return ls;
      };
      auto makePoly = []() {
        auto poly = std::make_unique<OGRPolygon>();
        OGRLinearRing ring;
        ring.addPoint(0, 0);
        ring.addPoint(1, 0);
        ring.addPoint(1, 1);
        ring.addPoint(0, 0);
        poly->addRing(&ring);
        return poly;
      };
      auto makePoly25D = []() {
        auto poly = std::make_unique<OGRPolygon>();
        OGRLinearRing ring;
        ring.addPoint(0, 0, 1);
        ring.addPoint(1, 0, 1);
        ring.addPoint(1, 1, 1);
        ring.addPoint(0, 0, 1);
        poly->addRing(&ring);
        return poly;
      };
      auto makePolyM = []() {
        auto poly = std::make_unique<OGRPolygon>();
        OGRLinearRing ring;
        ring.addPoint(0, 0, 0, 1);
        ring.addPoint(1, 0, 0, 1);
        ring.addPoint(1, 1, 0, 1);
        ring.addPoint(0, 0, 0, 1);
        poly->addRing(&ring);
        return poly;
      };
      auto makePolyZM = []() {
        auto poly = std::make_unique<OGRPolygon>();
        OGRLinearRing ring;
        ring.addPoint(0, 0, 1, 1);
        ring.addPoint(1, 0, 1, 1);
        ring.addPoint(1, 1, 1, 1);
        ring.addPoint(0, 0, 1, 1);
        poly->addRing(&ring);
        return poly;
      };

      UnknownGeomCase cases[] = {
          {"pt25d", makePt25D()},     {"ptm", makePtM()},
          {"ptzm", makePtZM()},       {"mpt", makeMPt()},
          {"mpt25d", makeMPt25D()},   {"mptm", makeMPtM()},
          {"mptzm", makeMPtZM()},     {"ls", makeLS()},
          {"ls25d", makeLS25D()},     {"lsm", makeLSM()},
          {"lszm", makeLSZM()},       {"poly", makePoly()},
          {"poly25d", makePoly25D()}, {"polym", makePolyM()},
          {"polyzm", makePolyZM()},
      };

      for (auto& c : cases) {
        OGRGeomFieldDefn gf(c.name, wkbUnknown);
        OGRLayer* lyr = dsLine.ICreateLayer(c.name, &gf, nullptr);
        ASSERT_THAT(lyr, NotNull());
        OGRFeature f(lyr->GetLayerDefn());
        f.SetGeometry(c.geom.get());
        EXPECT_EQ(lyr->CreateFeature(&f), OGRERR_NONE);
      }

      // wkbPolyhedralSurface on wkbUnknown layer -> hits lines 1308-1313
      {
        OGRGeomFieldDefn gfPs("ps", wkbUnknown);
        OGRLayer* lyrPs = dsLine.ICreateLayer("unk_ps", &gfPs, nullptr);
        ASSERT_THAT(lyrPs, NotNull());
        OGRFeature fPs(lyrPs->GetLayerDefn());
        OGRPolyhedralSurface ps;
        fPs.SetGeometry(&ps);
        EXPECT_EQ(lyrPs->CreateFeature(&fPs), OGRERR_NONE);
      }
      // wkbGeometryCollection with PolyhedralSurface + Point (incompatible) ->
      // hits lines 1315-1333
      {
        OGRGeomFieldDefn gfGcBad("gc_bad", wkbUnknown);
        OGRLayer* lyrGcBad =
            dsLine.ICreateLayer("unk_gc_bad", &gfGcBad, nullptr);
        ASSERT_THAT(lyrGcBad, NotNull());
        OGRFeature fGcBad(lyrGcBad->GetLayerDefn());
        OGRGeometryCollection gcBad;
        OGRPolyhedralSurface psSub;
        OGRPoint ptSub(1.0, 2.0);
        gcBad.addGeometry(&psSub);
        gcBad.addGeometry(&ptSub);
        fGcBad.SetGeometry(&gcBad);
        WithQuietHandler quiet;
        CPL_IGNORE_RET_VAL(lyrGcBad->CreateFeature(&fGcBad));
      }
      // wkbGeometryCollection with PolyhedralSurface only (compatible) -> hits
      // lines 1334-1338
      {
        OGRGeomFieldDefn gfGcOk("gc_ok", wkbUnknown);
        OGRLayer* lyrGcOk = dsLine.ICreateLayer("unk_gc_ok", &gfGcOk, nullptr);
        ASSERT_THAT(lyrGcOk, NotNull());
        OGRFeature fGcOk(lyrGcOk->GetLayerDefn());
        OGRGeometryCollection gcOk;
        OGRPolyhedralSurface psSub;
        gcOk.addGeometry(&psSub);
        fGcOk.SetGeometry(&gcOk);
        EXPECT_EQ(lyrGcOk->CreateFeature(&fGcOk), OGRERR_NONE);
      }
    }
  }

  // 3. Reopen dataset and test CreateField on reopened layer, TestCapability
  // variations, and SetNextByIndex
  {
    OGRShapeDataSource dsLine;
    dsLine.SetDescription(kRepackDir);
    GDALOpenInfo oiLine(kRepackDir, GA_Update);
    ASSERT_TRUE(dsLine.Open(&oiLine, false));

    OGRLayer* lyr = dsLine.GetLayerByName("line_repack");
    ASSERT_THAT(lyr, NotNull());
    OGRShapeLayer* shapeLyr = cpl::down_cast<OGRShapeLayer*>(lyr);

    // CreateField on reopened layer that already has fields
    // -> hits lines 1977-1981 (populating m_oSetUCFieldName from existing
    // fields)
    OGRFieldDefn fldStr("EXTRA_TXT", OFTString);
    EXPECT_EQ(shapeLyr->CreateField(&fldStr), OGRERR_NONE);

    // Capabilities with no filter
    EXPECT_TRUE(shapeLyr->TestCapability(OLCFastGetExtent3D));
    EXPECT_TRUE(shapeLyr->TestCapability(OLCFastSetNextByIndex));

    // Set spatial filter (no QIX or SBN index present)
    shapeLyr->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);

    // FastFeatureCount returns FALSE when spatial filter is set and no QIX/SBN
    // -> hits lines 1796-1797
    EXPECT_FALSE(shapeLyr->TestCapability(OLCFastFeatureCount));

    // FastGetExtent3D returns FALSE with spatial filter
    // -> hits line 1820
    EXPECT_FALSE(shapeLyr->TestCapability(OLCFastGetExtent3D));

    // FastSetNextByIndex returns FALSE with spatial filter
    // -> hits line 1823
    EXPECT_FALSE(shapeLyr->TestCapability(OLCFastSetNextByIndex));

    // SetNextByIndex with spatial filter
    // -> hits line 874: return OGRLayer::SetNextByIndex(nIndex);
    EXPECT_EQ(shapeLyr->SetNextByIndex(0), OGRERR_NONE);

    shapeLyr->SetSpatialFilter(nullptr);
  }

  // 4. UpdateFollowingDeOrRecompression with SRS & QIX, CloseUnderlyingLayer,
  // and ReopenFileDescriptors failure when .dbf or .shp is removed
  {
    const std::string zipPath = std::string(kRepackDir) + "/reopen_test.zip";
    OGRShapeDataSource dsZip;
    dsZip.SetDescription(zipPath.c_str());
    ASSERT_TRUE(dsZip.CreateZip(zipPath.c_str()));

    // Create a standalone OGRShapeLayer on dsZip with SRS and QIX
    std::string standaloneShp = std::string(kRepackDir) + "/standalone.shp";
    std::string standaloneDbf = std::string(kRepackDir) + "/standalone.dbf";
    SHPHandle hSHP = SHPCreate(standaloneShp.c_str(), SHPT_POINT);
    ASSERT_THAT(hSHP, NotNull());
    DBFHandle hDBF = DBFCreate(standaloneDbf.c_str());
    ASSERT_THAT(hDBF, NotNull());
    DBFAddField(hDBF, "ID", FTInteger, 5, 0);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    std::string prjFile = std::string(kRepackDir) + "/standalone.prj";
    OGRShapeLayer standaloneLyr(&dsZip, standaloneShp.c_str(), hSHP, hDBF, &srs,
                                true, prjFile, true, wkbPoint, nullptr);
    EXPECT_EQ(standaloneLyr.CreateSpatialIndex(0), OGRERR_NONE);

    struct ArrowArrayStream streamClosed;
    ASSERT_TRUE(standaloneLyr.GetArrowStream(&streamClosed, nullptr));

    // UpdateFollowingDeOrRecompression updates prj path (lines 3754-3764) and
    // calls CloseUnderlyingLayer(), which closes m_hQIX (line 3665)
    standaloneLyr.UpdateFollowingDeOrRecompression();

    // Since UpdateFollowingDeOrRecompression pointed m_osFullName into the zip
    // dir where standalone.shp/.dbf do not exist, TouchLayer() will fail in
    // ReopenFileDescriptors() on m_hSHP (lines 3620-3623) and set
    // FD_CANNOT_REOPEN!
    WithQuietHandler quiet;
    standaloneLyr.SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);
    EXPECT_EQ(standaloneLyr.GetFeatureCount(TRUE), 0);
    standaloneLyr.ResetReading();
    struct ArrowArray arrClosed;
    EXPECT_EQ(streamClosed.get_next(&streamClosed, &arrClosed), EIO);
    if (streamClosed.release) streamClosed.release(&streamClosed);

    // Second call hits m_eFileDescriptorsState == FD_CANNOT_REOPEN (line 3597)
    CPLStringList flClosed;
    standaloneLyr.AddToFileList(flClosed);
    OGREnvelope envClosed;
    EXPECT_EQ(standaloneLyr.GetExtent(&envClosed, TRUE), OGRERR_FAILURE);
    OGREnvelope3D env3DClosed;
    EXPECT_EQ(standaloneLyr.GetExtent3D(0, &env3DClosed, TRUE), OGRERR_FAILURE);
    EXPECT_FALSE(standaloneLyr.TestCapability(OLCRandomRead));
    EXPECT_EQ(standaloneLyr.SetNextByIndex(0), OGRERR_FAILURE);
    EXPECT_THAT(standaloneLyr.GetNextFeature(), IsNull());
    EXPECT_THAT(standaloneLyr.GetFeature(0), IsNull());
    EXPECT_EQ(standaloneLyr.SyncToDisk(), OGRERR_FAILURE);

    // Also test ReopenFileDescriptors failing on .dbf only (m_bHSHPWasNonNULL
    // == false)
    std::string dbfOnlyPath = std::string(kRepackDir) + "/dbfonly_reopen.dbf";
    DBFHandle hDBF2 = DBFCreate(dbfOnlyPath.c_str());
    ASSERT_THAT(hDBF2, NotNull());
    DBFAddField(hDBF2, "ID", FTInteger, 5, 0);
    OGRShapeLayer dbfOnlyLyr(&dsZip, dbfOnlyPath.c_str(), nullptr, hDBF2,
                             nullptr, false, "", true, wkbNone, nullptr);
    EXPECT_EQ(dbfOnlyLyr.IGetExtent(0, &envClosed, true), OGRERR_FAILURE);
    EXPECT_EQ(dbfOnlyLyr.IGetExtent3D(0, &env3DClosed, true), OGRERR_FAILURE);
    dbfOnlyLyr.UpdateFollowingDeOrRecompression();
    // TouchLayer fails to reopen .dbf (lines 3632-3638)
    EXPECT_FALSE(dbfOnlyLyr.TestCapability(OLCRandomRead));
  }
}

}  // namespace
}  // namespace autotest2
