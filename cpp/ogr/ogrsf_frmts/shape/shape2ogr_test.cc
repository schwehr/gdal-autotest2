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
// Tests for shape2ogr.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/shape/shape2ogr.cpp

#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/ogrshape.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

TEST(Shape2OgrTest, FeatureDefnFromShpAndDbf) {
  constexpr const char* kBase = "/vsimem/s2o_defn";
  constexpr const char* kShp = "/vsimem/s2o_defn.shp";
  constexpr const char* kShx = "/vsimem/s2o_defn.shx";
  constexpr const char* kDbf = "/vsimem/s2o_defn.dbf";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
    VSIUnlink(kDbf);
  });

  // 1. hSHP == nullptr gives wkbNone
  OGRFeatureDefn* poDefnNone =
      SHPReadOGRFeatureDefn("none_layer", nullptr, nullptr, "", FALSE);
  ASSERT_THAT(poDefnNone, NotNull());
  EXPECT_EQ(poDefnNone->GetGeomType(), wkbNone);
  poDefnNone->Release();

  // 2. All 13 SHPT types to OGR geometry types
  constexpr int kShapeTypes[] = {
      SHPT_POINT,       SHPT_POINTZ,  SHPT_POINTM,     SHPT_ARC,
      SHPT_ARCZ,        SHPT_ARCM,    SHPT_MULTIPOINT, SHPT_MULTIPOINTZ,
      SHPT_MULTIPOINTM, SHPT_POLYGON, SHPT_POLYGONZ,   SHPT_POLYGONM,
      SHPT_MULTIPATCH,
  };
  constexpr OGRwkbGeometryType kExpectedGeomTypes[] = {
      wkbPoint,        wkbPointZM,     wkbPointM,     wkbLineString,
      wkbLineStringZM, wkbLineStringM, wkbMultiPoint, wkbMultiPointZM,
      wkbMultiPointM,  wkbPolygon,     wkbPolygonZM,  wkbPolygonM,
      wkbUnknown,
  };

  for (size_t i = 0; i < std::size(kShapeTypes); ++i) {
    SHPHandle h = SHPCreate(kBase, kShapeTypes[i]);
    ASSERT_THAT(h, NotNull());
    OGRFeatureDefn* poDefn =
        SHPReadOGRFeatureDefn("test_layer", h, nullptr, "", FALSE);
    ASSERT_THAT(poDefn, NotNull());
    EXPECT_EQ(poDefn->GetGeomType(), kExpectedGeomTypes[i]);
    poDefn->Release();
    SHPClose(h);
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  }

  // 3. DBF field types, encoding, and bAdjustType
  DBFHandle hDBF = DBFCreate(kDbf);
  ASSERT_THAT(hDBF, NotNull());
  EXPECT_EQ(DBFAddField(hDBF, "DATE_FLD", FTDate, 8, 0), 0);
  EXPECT_EQ(DBFAddField(hDBF, "INT64_FLD", FTDouble, 15, 0), 1);
  EXPECT_EQ(DBFAddField(hDBF, "REAL_W20", FTDouble, 20, 0), 2);
  EXPECT_EQ(DBFAddField(hDBF, "REAL_PREC", FTDouble, 10, 2), 3);
  EXPECT_EQ(DBFAddField(hDBF, "INT_FLD", FTInteger, 9, 0), 4);
  EXPECT_EQ(DBFAddField(hDBF, "BOOL_FLD", FTLogical, 1, 0), 5);
  EXPECT_EQ(DBFAddField(hDBF, "STR_FLD", FTString, 30, 0), 6);
  EXPECT_EQ(DBFAddField(hDBF, "ADJ_OVF", FTDouble, 25, 0), 7);
  EXPECT_EQ(DBFAddField(hDBF, "ADJ_INT", FTDouble, 15, 0), 8);

  // Add a row to test bAdjustType demotion / promotion
  DBFWriteAttributeDirectly(hDBF, 0, 0, "20261103");
  DBFWriteAttributeDirectly(hDBF, 0, 1, "12345678901");          // Int64
  DBFWriteAttributeDirectly(hDBF, 0, 2, "1234567890123456789");  // Real
  DBFWriteDoubleAttribute(hDBF, 0, 3, 123.45);
  DBFWriteIntegerAttribute(hDBF, 0, 4, 42);
  DBFWriteLogicalAttribute(hDBF, 0, 5, 'T');
  DBFWriteStringAttribute(hDBF, 0, 6, "Hello");
  DBFWriteAttributeDirectly(hDBF, 0, 7,
                            "99999999999999999999");  // Overflow -> Real
  DBFWriteAttributeDirectly(hDBF, 0, 8, "12345");     // Fits in Int32

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(hSHP, NotNull());

  OGRFeatureDefn* poDefnAdjust =
      SHPReadOGRFeatureDefn("adj_layer", hSHP, hDBF, "UTF-8", TRUE);
  ASSERT_THAT(poDefnAdjust, NotNull());
  EXPECT_EQ(poDefnAdjust->GetFieldCount(), 9);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(0)->GetType(), OFTDate);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(1)->GetType(), OFTInteger64);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(2)->GetType(), OFTInteger64);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(3)->GetType(), OFTReal);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(4)->GetType(), OFTInteger);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(5)->GetType(), OFTInteger);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(5)->GetSubType(), OFSTBoolean);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(6)->GetType(), OFTString);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(7)->GetType(), OFTReal);
  EXPECT_EQ(poDefnAdjust->GetFieldDefn(8)->GetType(), OFTInteger);
  poDefnAdjust->Release();

  DBFClose(hDBF);
  SHPClose(hSHP);
}

TEST(Shape2OgrTest, WriteAndReadEmptyAndNullGeometries) {
  constexpr const char* kBase = "/vsimem/s2o_null";
  constexpr const char* kShp = "/vsimem/s2o_null.shp";
  constexpr const char* kShx = "/vsimem/s2o_null.shx";
  constexpr const char* kDbf = "/vsimem/s2o_null.dbf";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
    VSIUnlink(kDbf);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(hSHP, NotNull());
  DBFHandle hDBF = DBFCreate(kDbf);
  ASSERT_THAT(hDBF, NotNull());

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("null_test");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };
  poDefn->SetGeomType(wkbPoint);

  // 1. Write null geometry
  OGRFeature fNull(poDefn);
  fNull.SetGeometry(nullptr);
  bool bTrunc = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fNull, "", &bTrunc, false),
            OGRERR_NONE);

  // 2. Write empty geometry
  OGRFeature fEmpty(poDefn);
  OGRPoint emptyPt;
  fEmpty.SetGeometry(&emptyPt);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fEmpty, "", &bTrunc, false),
            OGRERR_NONE);

  // 3. Read back
  bool bWarn = false;
  OGRGeometry* g0 = SHPReadOGRObject(hSHP, 0, nullptr, bWarn);
  EXPECT_EQ(g0, nullptr);

  OGRGeometry* g1 = SHPReadOGRObject(hSHP, 1, nullptr, bWarn);
  EXPECT_EQ(g1, nullptr);

  // Out of range read
  EXPECT_EQ(SHPReadOGRObject(hSHP, -1, nullptr, bWarn), nullptr);
  EXPECT_EQ(SHPReadOGRObject(hSHP, 10, nullptr, bWarn), nullptr);

  DBFClose(hDBF);
  SHPClose(hSHP);
}

TEST(Shape2OgrTest, WriteAndReadPointGeometries) {
  constexpr const char* kBase = "/vsimem/s2o_point";
  constexpr const char* kShp = "/vsimem/s2o_point.shp";
  constexpr const char* kShx = "/vsimem/s2o_point.shx";
  constexpr const char* kDbf = "/vsimem/s2o_point.dbf";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
    VSIUnlink(kDbf);
  });

  // 1. SHPT_POINT round trip
  SHPHandle hSHP = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(hSHP, NotNull());
  DBFHandle hDBF = DBFCreate(kDbf);
  ASSERT_THAT(hDBF, NotNull());

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("pt_test");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };
  poDefn->SetGeomType(wkbPoint);

  OGRFeature fPoint(poDefn);
  OGRPoint pt(10.5, 20.25);
  fPoint.SetGeometry(&pt);
  bool bTrunc = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fPoint, "", &bTrunc, false),
            OGRERR_NONE);

  bool bWarn = false;
  std::unique_ptr<OGRGeometry> gPoint(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPoint, NotNull());
  EXPECT_EQ(gPoint->getGeometryType(), wkbPoint);
  EXPECT_DOUBLE_EQ(gPoint->toPoint()->getX(), 10.5);
  EXPECT_DOUBLE_EQ(gPoint->toPoint()->getY(), 20.25);

  // Non-point geometry error
  OGRLineString line;
  line.addPoint(0, 0);
  line.addPoint(1, 1);
  fPoint.SetGeometry(&line);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fPoint, "", &bTrunc, false),
        OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  }

  // Non-finite coordinate error
  OGRPoint nanPt(std::numeric_limits<double>::infinity(), 0.0);
  fPoint.SetFID(OGRNullFID);
  fPoint.SetGeometry(&nanPt);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fPoint, "", &bTrunc, false),
        OGRERR_FAILURE);
  }

  // Allow non-finite coordinates via config option
  {
    const CPLConfigOptionSetter setter("OGR_SHAPE_ALLOW_NON_FINITE_COORDINATES",
                                       "YES", false);
    fPoint.SetFID(OGRNullFID);
    EXPECT_EQ(
        SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fPoint, "", &bTrunc, false),
        OGRERR_NONE);
  }

  DBFClose(hDBF);
  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);
  VSIUnlink(kDbf);

  // 2. SHPT_POINTZ and SHPT_POINTM
  hSHP = SHPCreate(kBase, SHPT_POINTZ);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbPointZM);

  OGRFeature fPointZ(poDefn);
  OGRPoint ptZ(1.0, 2.0, 3.0, 4.0);
  fPointZ.SetGeometry(&ptZ);
  EXPECT_EQ(
      SHPWriteOGRFeature(hSHP, nullptr, poDefn, &fPointZ, "", &bTrunc, false),
      OGRERR_NONE);

  OGRPoint ptZNoM(10.0, 20.0, 30.0);
  fPointZ.SetFID(OGRNullFID);
  fPointZ.SetGeometry(&ptZNoM);
  EXPECT_EQ(
      SHPWriteOGRFeature(hSHP, nullptr, poDefn, &fPointZ, "", &bTrunc, false),
      OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gPointZ(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPointZ, NotNull());
  EXPECT_DOUBLE_EQ(gPointZ->toPoint()->getZ(), 3.0);
  EXPECT_DOUBLE_EQ(gPointZ->toPoint()->getM(), 4.0);

  std::unique_ptr<OGRGeometry> gPointZNoM(
      SHPReadOGRObject(hSHP, 1, nullptr, bWarn));
  ASSERT_THAT(gPointZNoM, NotNull());
  EXPECT_DOUBLE_EQ(gPointZNoM->toPoint()->getZ(), 30.0);

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // SHPT_POINTM
  hSHP = SHPCreate(kBase, SHPT_POINTM);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbPointM);
  OGRPoint ptM(5.0, 6.0, 0.0, 7.0);
  fPointZ.SetFID(OGRNullFID);
  fPointZ.SetGeometry(&ptM);
  EXPECT_EQ(
      SHPWriteOGRFeature(hSHP, nullptr, poDefn, &fPointZ, "", &bTrunc, false),
      OGRERR_NONE);
  std::unique_ptr<OGRGeometry> gPointM(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPointM, NotNull());
  EXPECT_DOUBLE_EQ(gPointM->toPoint()->getM(), 7.0);
  EXPECT_FALSE(gPointM->Is3D());

  SHPClose(hSHP);

  // Read POINTZ where bMeasureIsUsed is false
  double ptZ_X = 1.0, ptZ_Y = 2.0, ptZ_Z = 3.0;
  SHPObject* psPointZNoM = SHPCreateObject(SHPT_POINTZ, -1, 0, nullptr, nullptr,
                                           1, &ptZ_X, &ptZ_Y, &ptZ_Z, nullptr);
  psPointZNoM->bMeasureIsUsed = FALSE;
  std::unique_ptr<OGRGeometry> gPtZNoM(
      SHPReadOGRObject(nullptr, 0, psPointZNoM, bWarn));
  ASSERT_THAT(gPtZNoM, NotNull());
  EXPECT_FALSE(gPtZNoM->IsMeasured());

  // Unsupported shape type in SHPReadOGRObject
  SHPObject* psInvalidType = SHPCreateObject(
      999, -1, 0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(SHPReadOGRObject(nullptr, 0, psInvalidType, bWarn), nullptr);
}

TEST(Shape2OgrTest, WriteAndReadMultiPointGeometries) {
  constexpr const char* kBase = "/vsimem/s2o_mp";
  constexpr const char* kShp = "/vsimem/s2o_mp.shp";
  constexpr const char* kShx = "/vsimem/s2o_mp.shx";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_MULTIPOINT);
  ASSERT_THAT(hSHP, NotNull());

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("mp_test");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };
  poDefn->SetGeomType(wkbMultiPoint);

  // 1. Write MultiPoint with valid and empty points
  OGRMultiPoint mp;
  OGRPoint p1(1.0, 2.0);
  OGRPoint pEmpty;
  OGRPoint p2(3.0, 4.0);
  mp.addGeometry(&p1);
  mp.addGeometry(&pEmpty);  // ignored in shapefile writer
  mp.addGeometry(&p2);

  OGRFeature f(poDefn);
  f.SetGeometry(&mp);
  bool bTrunc = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  bool bWarn = false;
  std::unique_ptr<OGRGeometry> gMP(SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gMP, NotNull());
  EXPECT_EQ(gMP->getGeometryType(), wkbMultiPoint);
  EXPECT_EQ(gMP->toMultiPoint()->getNumGeometries(), 2);

  // Non-multipoint error
  f.SetGeometry(&p1);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
              OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  }

  // Non-finite coordinate error
  OGRMultiPoint nanMp;
  OGRPoint nanP(std::numeric_limits<double>::infinity(), 0.0);
  nanMp.addGeometry(&nanP);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&nanMp);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
              OGRERR_FAILURE);
  }

  // Read empty multipoint (0 vertices)
  SHPObject* psEmptyMp =
      SHPCreateSimpleObject(SHPT_MULTIPOINT, 0, nullptr, nullptr, nullptr);
  EXPECT_EQ(SHPReadOGRObject(hSHP, 0, psEmptyMp, bWarn), nullptr);

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // 2. SHPT_MULTIPOINTZ and SHPT_MULTIPOINTM
  hSHP = SHPCreate(kBase, SHPT_MULTIPOINTZ);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbMultiPointZM);

  OGRMultiPoint mpZ;
  OGRPoint pz(1.0, 2.0, 3.0, 4.0);
  mpZ.addGeometry(&pz);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mpZ);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);
  std::unique_ptr<OGRGeometry> gMpZ(SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gMpZ, NotNull());
  EXPECT_DOUBLE_EQ(gMpZ->toMultiPoint()->getGeometryRef(0)->toPoint()->getZ(),
                   3.0);
  EXPECT_DOUBLE_EQ(gMpZ->toMultiPoint()->getGeometryRef(0)->toPoint()->getM(),
                   4.0);

  // Write 3D point without M into wkbMultiPointZM layer (adfM fallback)
  OGRMultiPoint mp3DNoM;
  OGRPoint p3D(1.0, 2.0, 3.0);
  mp3DNoM.addGeometry(&p3D);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mp3DNoM);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // SHPT_MULTIPOINTZ read where padfM == nullptr
  double rawMpX[1] = {1.0}, rawMpY[1] = {2.0}, rawMpZ[1] = {3.0};
  SHPObject* psMpZNoM =
      SHPCreateObject(SHPT_MULTIPOINTZ, -1, 0, nullptr, nullptr, 1, rawMpX,
                      rawMpY, rawMpZ, nullptr);
  free(psMpZNoM->padfM);
  psMpZNoM->padfM = nullptr;
  std::unique_ptr<OGRGeometry> gMpZNoM(
      SHPReadOGRObject(nullptr, 0, psMpZNoM, bWarn));
  ASSERT_THAT(gMpZNoM, NotNull());

  // SHPT_MULTIPOINTM
  hSHP = SHPCreate(kBase, SHPT_MULTIPOINTM);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbMultiPointM);
  OGRMultiPoint mpM;
  OGRPoint pm(1.0, 2.0, 0.0, 5.0);
  mpM.addGeometry(&pm);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mpM);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);
  std::unique_ptr<OGRGeometry> gMpM(SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gMpM, NotNull());
  EXPECT_DOUBLE_EQ(gMpM->toMultiPoint()->getGeometryRef(0)->toPoint()->getM(),
                   5.0);
  EXPECT_FALSE(gMpM->toMultiPoint()->getGeometryRef(0)->toPoint()->Is3D());

  SHPClose(hSHP);
}

TEST(Shape2OgrTest, WriteAndReadArcGeometries) {
  constexpr const char* kBase = "/vsimem/s2o_arc";
  constexpr const char* kShp = "/vsimem/s2o_arc.shp";
  constexpr const char* kShx = "/vsimem/s2o_arc.shx";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_ARC);
  ASSERT_THAT(hSHP, NotNull());

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("arc_test");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };
  poDefn->SetGeomType(wkbLineString);

  // 1. Single OGRLineString
  OGRLineString line;
  line.addPoint(0, 0);
  line.addPoint(5, 5);
  OGRFeature f(poDefn);
  f.SetGeometry(&line);
  bool bTrunc = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  bool bWarn = false;
  std::unique_ptr<OGRGeometry> gLine(SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gLine, NotNull());
  EXPECT_EQ(gLine->getGeometryType(), wkbLineString);
  EXPECT_EQ(gLine->toLineString()->getNumPoints(), 2);

  // 2. OGRMultiLineString with 2 parts and an empty line
  OGRMultiLineString mls;
  OGRLineString l1;
  l1.addPoint(1, 1);
  l1.addPoint(2, 2);
  OGRLineString lEmpty;
  OGRLineString l2;
  l2.addPoint(3, 3);
  l2.addPoint(4, 4);
  mls.addGeometry(&l1);
  mls.addGeometry(&lEmpty);  // ignored
  mls.addGeometry(&l2);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mls);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gMls(SHPReadOGRObject(hSHP, 1, nullptr, bWarn));
  ASSERT_THAT(gMls, NotNull());
  EXPECT_EQ(gMls->getGeometryType(), wkbMultiLineString);
  EXPECT_EQ(gMls->toMultiLineString()->getNumGeometries(), 2);

  // 3. Curved geometry forced to MultiLineString
  OGRCircularString cs;
  cs.addPoint(0, 0);
  cs.addPoint(1, 1);
  cs.addPoint(2, 0);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&cs);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // 4. Non-linestring error
  OGRPoint pt(1, 1);
  f.SetGeometry(&pt);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
              OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  }

  // 5. Read ARC with nParts == 0
  SHPObject* psArcZeroParts = SHPCreateObject(
      SHPT_ARC, -1, 0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
  psArcZeroParts->nParts = 0;
  EXPECT_EQ(SHPReadOGRObject(hSHP, 0, psArcZeroParts, bWarn), nullptr);

  // 6. Read ARC with panPartStart == nullptr
  double px[2] = {0, 1};
  double py[2] = {0, 1};
  SHPObject* psArcNoPartStart = SHPCreateObject(
      SHPT_ARC, -1, 1, nullptr, nullptr, 2, px, py, nullptr, nullptr);
  free(psArcNoPartStart->panPartStart);
  psArcNoPartStart->panPartStart = nullptr;
  std::unique_ptr<OGRGeometry> gArcNoPartStart(
      SHPReadOGRObject(hSHP, 0, psArcNoPartStart, bWarn));
  ASSERT_THAT(gArcNoPartStart, NotNull());

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // 7. ARCZ and ARCM
  hSHP = SHPCreate(kBase, SHPT_ARCZ);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbLineStringZM);

  OGRLineString lineZ;
  lineZ.addPoint(0, 0, 10, 100);
  lineZ.addPoint(1, 1, 20, 200);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&lineZ);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // MultiLineString with ARCZ
  OGRMultiLineString mlsZ;
  mlsZ.addGeometry(&lineZ);
  mlsZ.addGeometry(&lineZ);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mlsZ);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gLineZ(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gLineZ, NotNull());
  EXPECT_DOUBLE_EQ(gLineZ->toLineString()->getZ(0), 10.0);
  EXPECT_DOUBLE_EQ(gLineZ->toLineString()->getM(0), 100.0);

  std::unique_ptr<OGRGeometry> gMlsZ(SHPReadOGRObject(hSHP, 1, nullptr, bWarn));
  ASSERT_THAT(gMlsZ, NotNull());
  EXPECT_EQ(gMlsZ->getGeometryType(), wkbMultiLineStringZM);

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // ARCM
  hSHP = SHPCreate(kBase, SHPT_ARCM);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbLineStringM);

  OGRLineString lineM;
  lineM.setNumPoints(2);
  lineM.setPointM(0, 0, 0, 50);
  lineM.setPointM(1, 1, 1, 60);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&lineM);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  OGRMultiLineString mlsM;
  mlsM.addGeometry(&lineM);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mlsM);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gLineM(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gLineM, NotNull());
  EXPECT_DOUBLE_EQ(gLineM->toLineString()->getM(0), 50.0);

  std::unique_ptr<OGRGeometry> gMlsM(SHPReadOGRObject(hSHP, 1, nullptr, bWarn));
  ASSERT_THAT(gMlsM, NotNull());

  SHPClose(hSHP);
}

TEST(Shape2OgrTest, WriteAndReadPolygonGeometries) {
  constexpr const char* kBase = "/vsimem/s2o_poly";
  constexpr const char* kShp = "/vsimem/s2o_poly.shp";
  constexpr const char* kShx = "/vsimem/s2o_poly.shx";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POLYGON);
  ASSERT_THAT(hSHP, NotNull());

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("poly_test");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };
  poDefn->SetGeomType(wkbPolygon);

  // 1. Single OGRPolygon
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);
  poly.addRing(&ring);

  OGRFeature f(poDefn);
  f.SetGeometry(&poly);
  bool bTrunc = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  bool bWarn = false;
  std::unique_ptr<OGRGeometry> gPoly(SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPoly, NotNull());
  EXPECT_EQ(gPoly->getGeometryType(), wkbPolygon);

  // 2. Polygon with empty exterior ring -> writes NULL shape
  OGRPolygon emptyPoly;
  f.SetFID(OGRNullFID);
  f.SetGeometry(&emptyPoly);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);
  EXPECT_EQ(SHPReadOGRObject(hSHP, 1, nullptr, bWarn), nullptr);

  // Polygon with empty interior ring
  OGRPolygon polyWithEmptyInner;
  polyWithEmptyInner.addRing(&ring);
  OGRLinearRing emptyInner;
  polyWithEmptyInner.addRing(&emptyInner);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&polyWithEmptyInner);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // 3. MultiPolygon
  OGRMultiPolygon mp;
  mp.addGeometry(&poly);
  OGRPolygon polyEmpty;
  mp.addGeometry(&polyEmpty);  // empty child ignored
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mp);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // MultiPolygon with only empty polygons
  OGRMultiPolygon mpAllEmpty;
  OGRPolygon pe;
  mpAllEmpty.addGeometry(&pe);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mpAllEmpty);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // MultiPolygon with empty interior ring
  OGRMultiPolygon mpInner;
  mpInner.addGeometry(&polyWithEmptyInner);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&mpInner);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gMp(SHPReadOGRObject(hSHP, 2, nullptr, bWarn));
  ASSERT_THAT(gMp, NotNull());

  // 4. OGRGeometryCollection of polygons
  OGRGeometryCollection gc;
  gc.addGeometry(&poly);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&gc);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // OGRGeometryCollection with non-polygon child error
  OGRPoint pt(1, 1);
  gc.addGeometry(&pt);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&gc);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
              OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  }

  // 5. OGRTriangle
  OGRTriangle tri(OGRPoint(0, 0), OGRPoint(0, 5), OGRPoint(5, 0));
  f.SetFID(OGRNullFID);
  f.SetGeometry(&tri);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // 6. OGRPolyhedralSurface
  OGRPolyhedralSurface ps;
  ps.addGeometry(&poly);
  f.SetFID(OGRNullFID);
  f.SetGeometry(&ps);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  // 7. Non-polygon error
  f.SetFID(OGRNullFID);
  f.SetGeometry(&pt);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
              OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  }

  // 8. Read polygon with nParts == 0
  SHPObject* psPolyZeroParts =
      SHPCreateObject(SHPT_POLYGON, -1, 0, nullptr, nullptr, 0, nullptr,
                      nullptr, nullptr, nullptr);
  psPolyZeroParts->nParts = 0;
  EXPECT_EQ(SHPReadOGRObject(hSHP, 0, psPolyZeroParts, bWarn), nullptr);

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // 9. POLYGONZ and POLYGONM
  hSHP = SHPCreate(kBase, SHPT_POLYGONZ);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbPolygonZM);

  OGRPolygon polyZ;
  OGRLinearRing ringZ;
  ringZ.addPoint(0, 0, 10, 100);
  ringZ.addPoint(0, 10, 20, 200);
  ringZ.addPoint(10, 10, 30, 300);
  ringZ.addPoint(10, 0, 40, 400);
  ringZ.addPoint(0, 0, 10, 100);
  polyZ.addRing(&ringZ);

  f.SetFID(OGRNullFID);
  f.SetGeometry(&polyZ);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, true),
            OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gPolyZ(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPolyZ, NotNull());
  EXPECT_DOUBLE_EQ(gPolyZ->toPolygon()->getExteriorRing()->getZ(0), 10.0);
  EXPECT_DOUBLE_EQ(gPolyZ->toPolygon()->getExteriorRing()->getM(0), 100.0);

  SHPClose(hSHP);
  VSIUnlink(kShp);
  VSIUnlink(kShx);

  // POLYGONM
  hSHP = SHPCreate(kBase, SHPT_POLYGONM);
  ASSERT_THAT(hSHP, NotNull());
  poDefn->SetGeomType(wkbPolygonM);

  OGRPolygon polyM;
  OGRLinearRing ringM;
  ringM.setNumPoints(5);
  ringM.setPointM(0, 0, 0, 10);
  ringM.setPointM(1, 0, 10, 20);
  ringM.setPointM(2, 10, 10, 30);
  ringM.setPointM(3, 10, 0, 40);
  ringM.setPointM(4, 0, 0, 10);
  polyM.addRing(&ringM);

  f.SetFID(OGRNullFID);
  f.SetGeometry(&polyM);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
            OGRERR_NONE);

  std::unique_ptr<OGRGeometry> gPolyM(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPolyM, NotNull());
  EXPECT_DOUBLE_EQ(gPolyM->toPolygon()->getExteriorRing()->getM(0), 10.0);

  // Polygon read where panPartStart == nullptr
  double polyX[5] = {0, 0, 10, 10, 0};
  double polyY[5] = {0, 10, 10, 0, 0};
  SHPObject* psPolyNoPartStart = SHPCreateObject(
      SHPT_POLYGON, -1, 1, nullptr, nullptr, 5, polyX, polyY, nullptr, nullptr);
  free(psPolyNoPartStart->panPartStart);
  psPolyNoPartStart->panPartStart = nullptr;
  std::unique_ptr<OGRGeometry> gPolyNoPartStart(
      SHPReadOGRObject(hSHP, 0, psPolyNoPartStart, bWarn));
  ASSERT_THAT(gPolyNoPartStart, NotNull());

  SHPClose(hSHP);
}

TEST(Shape2OgrTest, PolygonWindingOrderAutocorrection) {
  constexpr const char* kBase = "/vsimem/s2o_winding";
  constexpr const char* kShp = "/vsimem/s2o_winding.shp";
  constexpr const char* kShx = "/vsimem/s2o_winding.shx";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POLYGON);
  ASSERT_THAT(hSHP, NotNull());

  // 1. Two disjoint CCW rings (!sFirstEnvelope.Intersects(sCurEnvelope))
  // CCW ring 0: (0,0)-(10,0)-(10,10)-(0,10)-(0,0)
  // CCW ring 1: (20,20)-(30,20)-(30,30)-(20,30)-(20,20)
  const int panPartStartDisjoint[2] = {0, 5};
  const double padfXDisjoint[10] = {0, 10, 10, 0, 0, 20, 30, 30, 20, 20};
  const double padfYDisjoint[10] = {0, 0, 10, 10, 0, 20, 20, 30, 30, 20};
  SHPObject* psDisjoint =
      SHPCreateObject(SHPT_POLYGON, -1, 2, panPartStartDisjoint, nullptr, 10,
                      padfXDisjoint, padfYDisjoint, nullptr, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, psDisjoint), 0);
  SHPDestroyObject(psDisjoint);

  // 2. Intersecting envelopes with extreme points outside exterior ring (lines
  // 350-416) Ring 0: L-shaped CCW ring
  // (0,0)-(10,0)-(10,2)-(2,2)-(2,10)-(0,10)-(0,0) Ring 1: CCW ring in corner
  // (5,5)-(9,5)-(9,9)-(5,9)-(5,5)
  const int panPartStartL[2] = {0, 7};
  const double padfXL[12] = {0, 10, 10, 2, 2, 0, 0, 5, 9, 9, 5, 5};
  const double padfYL[12] = {0, 0, 2, 2, 10, 10, 0, 5, 5, 9, 9, 5};
  SHPObject* psL = SHPCreateObject(SHPT_POLYGON, -1, 2, panPartStartL, nullptr,
                                   12, padfXL, padfYL, nullptr, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, psL), 1);
  SHPDestroyObject(psL);

  bool bWarned = false;
  {
    WithQuietHandler quiet;
    std::unique_ptr<OGRGeometry> g1(
        SHPReadOGRObject(hSHP, 0, nullptr, bWarned));
    ASSERT_THAT(g1, NotNull());
    EXPECT_TRUE(bWarned);
  }

  // Second read should not warn again because bWarned is already true
  {
    WithQuietHandler quiet;
    std::unique_ptr<OGRGeometry> g2(
        SHPReadOGRObject(hSHP, 1, nullptr, bWarned));
    ASSERT_THAT(g2, NotNull());
  }

  SHPClose(hSHP);
}

TEST(Shape2OgrTest, WriteAndReadMultiPatchGeometries) {
  constexpr const char* kBase = "/vsimem/s2o_patch";
  constexpr const char* kShp = "/vsimem/s2o_patch.shp";
  constexpr const char* kShx = "/vsimem/s2o_patch.shx";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_MULTIPATCH);
  ASSERT_THAT(hSHP, NotNull());

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("patch_test");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };
  poDefn->SetGeomType(wkbUnknown);

  // Write a polygon to MultiPatch
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0, 1);
  ring.addPoint(0, 10, 2);
  ring.addPoint(10, 10, 3);
  ring.addPoint(10, 0, 4);
  ring.addPoint(0, 0, 1);
  poly.addRing(&ring);

  OGRFeature f(poDefn);
  f.SetGeometry(&poly);
  bool bTrunc = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, true),
            OGRERR_NONE);

  bool bWarn = false;
  std::unique_ptr<OGRGeometry> gPatch(
      SHPReadOGRObject(hSHP, 0, nullptr, bWarn));
  ASSERT_THAT(gPatch, NotNull());

  // Non-patch geometry error
  OGRPoint pt(1, 1);
  f.SetGeometry(&pt);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, nullptr, poDefn, &f, "", &bTrunc, false),
              OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  }

  SHPClose(hSHP);
}

TEST(Shape2OgrTest, ReadAndWriteFeaturesWithAttributes) {
  constexpr const char* kBase = "/vsimem/s2o_attr";
  constexpr const char* kShp = "/vsimem/s2o_attr.shp";
  constexpr const char* kShx = "/vsimem/s2o_attr.shx";
  constexpr const char* kDbf = "/vsimem/s2o_attr.dbf";
  auto cl = absl::MakeCleanup([]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
    VSIUnlink(kDbf);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(hSHP, NotNull());
  DBFHandle hDBF = DBFCreate(kDbf);
  ASSERT_THAT(hDBF, NotNull());

  // Fields: STR (width 5), INT (width 5), REAL (width 8, prec 0), BOOL, DATE
  EXPECT_EQ(DBFAddField(hDBF, "STR_FLD", FTString, 5, 0), 0);
  EXPECT_EQ(DBFAddField(hDBF, "INT_FLD", FTInteger, 5, 0), 1);
  EXPECT_EQ(DBFAddField(hDBF, "REAL_FLD", FTDouble, 8, 0), 2);
  EXPECT_EQ(DBFAddField(hDBF, "BOOL_FLD", FTLogical, 1, 0), 3);
  EXPECT_EQ(DBFAddField(hDBF, "DATE_FLD", FTDate, 10, 0), 4);

  OGRFeatureDefn* poDefn = new OGRFeatureDefn("attr_layer");
  poDefn->Reference();
  absl::Cleanup clDefn = [poDefn]() { poDefn->Release(); };

  OGRFieldDefn fStr("STR_FLD", OFTString);
  fStr.SetWidth(5);
  poDefn->AddFieldDefn(&fStr);

  OGRFieldDefn fInt("INT_FLD", OFTInteger);
  fInt.SetWidth(5);
  poDefn->AddFieldDefn(&fInt);

  OGRFieldDefn fReal("REAL_FLD", OFTReal);
  fReal.SetWidth(8);
  fReal.SetPrecision(0);
  poDefn->AddFieldDefn(&fReal);

  OGRFieldDefn fBool("BOOL_FLD", OFTInteger);
  fBool.SetSubType(OFSTBoolean);
  poDefn->AddFieldDefn(&fBool);

  OGRFieldDefn fDate("DATE_FLD", OFTDate);
  poDefn->AddFieldDefn(&fDate);

  poDefn->SetGeomType(wkbPoint);

  // 1. Write feature with GrowField on String and Integer
  OGRFeature f1(poDefn);
  OGRPoint pt(1.0, 2.0);
  f1.SetGeometry(&pt);
  f1.SetField(0, "LongStringExceeding5");  // Width 21 > 5 -> GrowField
  f1.SetField(1, 1234567);                 // Width 7 > 5 -> GrowField
  f1.SetField(2, 42.0);
  f1.SetField(3, 1);
  f1.SetField(4, 2026, 11, 3);
  bool bTrunc = false;
  EXPECT_EQ(
      SHPWriteOGRFeature(hSHP, hDBF, poDefn, &f1, "UTF-8", &bTrunc, false),
      OGRERR_NONE);

  // 2. Write feature with Null / un-set attributes
  OGRFeature f2(poDefn);
  f2.SetGeometry(&pt);
  f2.SetFieldNull(0);
  f2.SetFieldNull(1);
  f2.SetFieldNull(2);
  f2.SetFieldNull(3);
  f2.SetFieldNull(4);
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, hDBF, poDefn, &f2, "", &bTrunc, false),
            OGRERR_NONE);

  // 3. Truncation warning on String > 254 chars with UTF-8 character
  std::string longStr(253, 'A');
  longStr += "\xC3\xA9";  // 2-byte UTF-8 é straddling byte 254
  longStr += "extra";
  OGRFeature fTrunc(poDefn);
  fTrunc.SetGeometry(&pt);
  fTrunc.SetField(0, longStr.c_str());
  fTrunc.SetField(1, 0);
  fTrunc.SetField(2, 0.0);
  fTrunc.SetField(3, 0);
  fTrunc.SetField(4, 0, 0, 0);  // All zero date -> null
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fTrunc, "UTF-8", &bTrunc,
                                 false),
              OGRERR_NONE);
    EXPECT_TRUE(bTrunc);
  }

  // 4. Real > 2^53 warning loop (11 times)
  const double hugeVal = 1e16;  // > 2^53 (~9e15)
  for (int i = 0; i < 11; ++i) {
    OGRFeature fHuge(poDefn);
    fHuge.SetGeometry(&pt);
    fHuge.SetField(2, hugeVal);
    WithQuietHandler quiet;
    EXPECT_EQ(
        SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fHuge, "", &bTrunc, false),
        OGRERR_NONE);
  }

  // 5. Invalid date year < 0
  OGRFeature fBadDate(poDefn);
  fBadDate.SetGeometry(&pt);
  fBadDate.SetField(4, -1, 1, 1);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        SHPWriteOGRFeature(hSHP, hDBF, poDefn, &fBadDate, "", &bTrunc, false),
        OGRERR_NONE);
  }

  // 6. Read back features
  bool bWarn = false;
  std::unique_ptr<OGRFeature> rf1(
      SHPReadOGRFeature(hSHP, hDBF, poDefn, 0, nullptr, "UTF-8", bWarn));
  ASSERT_THAT(rf1, NotNull());
  EXPECT_STREQ(rf1->GetFieldAsString(0), "LongStringExceeding5");
  EXPECT_EQ(rf1->GetFieldAsInteger(1), 1234567);
  EXPECT_DOUBLE_EQ(rf1->GetFieldAsDouble(2), 42.0);
  EXPECT_EQ(rf1->GetFieldAsInteger(3), 1);

  // Read feature with MM/DD/YYYY date format
  DBFWriteAttributeDirectly(hDBF, 1, 4, "11/03/2026");
  std::unique_ptr<OGRFeature> rf2(
      SHPReadOGRFeature(hSHP, hDBF, poDefn, 1, nullptr, "", bWarn));
  ASSERT_THAT(rf2, NotNull());
  int y = 0, m = 0, d = 0, hr = 0, min = 0, sec = 0, tz = 0;
  rf2->GetFieldAsDateTime(4, &y, &m, &d, &hr, &min, &sec, &tz);
  EXPECT_EQ(y, 2026);
  EXPECT_EQ(m, 11);
  EXPECT_EQ(d, 3);

  // Read out of bounds feature
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPReadOGRFeature(hSHP, hDBF, poDefn, -1, nullptr, "", bWarn),
              nullptr);
    EXPECT_EQ(SHPReadOGRFeature(hSHP, hDBF, poDefn, 1000, nullptr, "", bWarn),
              nullptr);
  }

  // Deleted DBF record
  DBFMarkRecordDeleted(hDBF, 0, TRUE);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(SHPReadOGRFeature(hSHP, hDBF, poDefn, 0, nullptr, "", bWarn),
              nullptr);
  }

  // Ignored geometry and ignored field
  poDefn->SetGeometryIgnored(TRUE);
  poDefn->GetFieldDefn(0)->SetIgnored(TRUE);
  std::unique_ptr<OGRFeature> rfIgnored(
      SHPReadOGRFeature(hSHP, hDBF, poDefn, 1, nullptr, "", bWarn));
  ASSERT_THAT(rfIgnored, NotNull());
  EXPECT_EQ(rfIgnored->GetGeometryRef(), nullptr);

  // Test SHPReadOGRFeatureDefn with empty encoding
  OGRFeatureDefn* poDefnEmptyEnc =
      SHPReadOGRFeatureDefn("no_enc", hSHP, hDBF, "", FALSE);
  ASSERT_THAT(poDefnEmptyEnc, NotNull());
  poDefnEmptyEnc->Release();

  // Test geometry dimension adjustments in SHPReadOGRFeature (lines 1341-1355)
  // 1. Promote 2D point to 3D and Measured
  poDefn->SetGeometryIgnored(FALSE);
  poDefn->SetGeomType(wkbPoint25D);
  std::unique_ptr<OGRFeature> rf3D(
      SHPReadOGRFeature(hSHP, hDBF, poDefn, 1, nullptr, "", bWarn));
  ASSERT_THAT(rf3D, NotNull());
  ASSERT_THAT(rf3D->GetGeometryRef(), NotNull());
  EXPECT_TRUE(rf3D->GetGeometryRef()->Is3D());

  poDefn->SetGeomType(wkbPointM);
  std::unique_ptr<OGRFeature> rfM(
      SHPReadOGRFeature(hSHP, hDBF, poDefn, 1, nullptr, "", bWarn));
  ASSERT_THAT(rfM, NotNull());
  ASSERT_THAT(rfM->GetGeometryRef(), NotNull());
  EXPECT_TRUE(rfM->GetGeometryRef()->IsMeasured());

  // 2. Demote 3D point to 2D
  OGRPoint pt3D(1.0, 2.0, 3.0);
  OGRFeature f3D(poDefn);
  f3D.SetGeometry(&pt3D);
  f3D.SetFID(OGRNullFID);
  bool bTrunc2 = false;
  EXPECT_EQ(SHPWriteOGRFeature(hSHP, hDBF, poDefn, &f3D, "", &bTrunc2, false),
            OGRERR_NONE);
  const int n3DId = hSHP->nRecords - 1;
  poDefn->SetGeomType(wkbPoint);
  std::unique_ptr<OGRFeature> rf2D(
      SHPReadOGRFeature(hSHP, hDBF, poDefn, n3DId, nullptr, "", bWarn));
  ASSERT_THAT(rf2D, NotNull());
  ASSERT_THAT(rf2D->GetGeometryRef(), NotNull());
  EXPECT_FALSE(rf2D->GetGeometryRef()->Is3D());

  DBFClose(hDBF);
  SHPClose(hSHP);
}

}  // namespace
}  // namespace autotest2
