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
// Tests for ogrgeojsonwriter.cpp.

#include "third_party/gdal/ogr/ogrgeojsonwriter.h"

#include <limits>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/json_c/json_object.h"
#include "third_party/json_c/json_types.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

class OgrGeoJsonWriterTest : public ::testing::Test {
 protected:
  void SetUp() override { CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "0"); }

  void TearDown() override {
    CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", nullptr);
  }
};

TEST_F(OgrGeoJsonWriterTest, Options) {
  OGRGeoJSONWriteOptions opts;
  EXPECT_FALSE(opts.bWriteBBOX);
  EXPECT_FALSE(opts.bBBOXRFC7946);
  EXPECT_EQ(-1, opts.nXYCoordPrecision);
  EXPECT_EQ(-1, opts.nZCoordPrecision);
  EXPECT_EQ(-1, opts.nSignificantFigures);
  EXPECT_FALSE(opts.bPolygonRightHandRule);
  EXPECT_TRUE(opts.bCanPatchCoordinatesWithNativeData);
  EXPECT_FALSE(opts.bHonourReservedRFC7946Members);

  opts.SetRFC7946Settings();
  EXPECT_TRUE(opts.bBBOXRFC7946);
  EXPECT_EQ(7, opts.nXYCoordPrecision);
  EXPECT_EQ(3, opts.nZCoordPrecision);
  EXPECT_TRUE(opts.bPolygonRightHandRule);
  EXPECT_FALSE(opts.bCanPatchCoordinatesWithNativeData);
  EXPECT_TRUE(opts.bHonourReservedRFC7946Members);

  // SetIDOptions
  CPLStringList idOpts;
  idOpts.AddNameValue("ID_FIELD", "custom_id");
  idOpts.AddNameValue("ID_TYPE", "String");
  idOpts.AddNameValue("ID_GENERATE", "YES");
  opts.SetIDOptions(idOpts.List());
  EXPECT_STREQ("custom_id", opts.osIDField.c_str());
  EXPECT_TRUE(opts.bForceIDFieldType);
  EXPECT_EQ(OFTString, opts.eForcedIDFieldType);
  EXPECT_TRUE(opts.bGenerateID);

  idOpts.Clear();
  idOpts.AddNameValue("ID_TYPE", "Integer");
  opts.SetIDOptions(idOpts.List());
  EXPECT_TRUE(opts.bForceIDFieldType);
  EXPECT_EQ(OFTInteger64, opts.eForcedIDFieldType);
}

TEST_F(OgrGeoJsonWriterTest, GetBBoxStandardAndRFC7946) {
  OGRGeoJSONWriteOptions opts;

  // 2D Point
  OGRPoint pt(10.0, 20.0);
  OGREnvelope3D env = OGRGeoJSONGetBBox(&pt, opts);
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(10.0, env.MaxX);
  EXPECT_DOUBLE_EQ(20.0, env.MinY);
  EXPECT_DOUBLE_EQ(20.0, env.MaxY);

  // 3D Point
  OGRPoint pt3d(10.0, 20.0, 30.0);
  env = OGRGeoJSONGetBBox(&pt3d, opts);
  EXPECT_DOUBLE_EQ(30.0, env.MinZ);
  EXPECT_DOUBLE_EQ(30.0, env.MaxZ);

  // Empty geometry
  OGRPoint ptEmpty;
  ptEmpty.empty();
  env = OGRGeoJSONGetBBox(&ptEmpty, opts);
  EXPECT_DOUBLE_EQ(0.0, env.MinX);

  // RFC7946 Heuristic 1: Multipart touching +-180
  opts.bBBOXRFC7946 = true;
  {
    OGRMultiPolygon mp;
    // Part 1: touching -180: [-180, -170] x [10, 20]
    auto poly1 = std::make_unique<OGRPolygon>();
    auto ring1 = std::make_unique<OGRLinearRing>();
    ring1->addPoint(-180.0, 10.0);
    ring1->addPoint(-170.0, 10.0);
    ring1->addPoint(-170.0, 20.0);
    ring1->addPoint(-180.0, 20.0);
    ring1->addPoint(-180.0, 10.0);
    poly1->addRingDirectly(ring1.release());
    mp.addGeometryDirectly(poly1.release());

    // Part 2: touching +180: [160, 180] x [10, 20]
    auto poly2 = std::make_unique<OGRPolygon>();
    auto ring2 = std::make_unique<OGRLinearRing>();
    ring2->addPoint(160.0, 10.0);
    ring2->addPoint(180.0, 10.0);
    ring2->addPoint(180.0, 20.0);
    ring2->addPoint(160.0, 20.0);
    ring2->addPoint(160.0, 10.0);
    poly2->addRingDirectly(ring2.release());
    mp.addGeometryDirectly(poly2.release());

    // Part 3: empty member
    mp.addGeometryDirectly(new OGRPolygon());

    // Part 4: positive interior part: [165, 175] x [10, 20]
    auto poly4 = std::make_unique<OGRPolygon>();
    auto ring4 = std::make_unique<OGRLinearRing>();
    ring4->addPoint(165.0, 10.0);
    ring4->addPoint(175.0, 10.0);
    ring4->addPoint(175.0, 20.0);
    ring4->addPoint(165.0, 20.0);
    ring4->addPoint(165.0, 10.0);
    poly4->addRingDirectly(ring4.release());
    mp.addGeometryDirectly(poly4.release());

    // Part 5: negative interior part: [-175, -165] x [10, 20]
    auto poly5 = std::make_unique<OGRPolygon>();
    auto ring5 = std::make_unique<OGRLinearRing>();
    ring5->addPoint(-175.0, 10.0);
    ring5->addPoint(-165.0, 10.0);
    ring5->addPoint(-165.0, 20.0);
    ring5->addPoint(-175.0, 20.0);
    ring5->addPoint(-175.0, 10.0);
    poly5->addRingDirectly(ring5.release());
    mp.addGeometryDirectly(poly5.release());

    env = OGRGeoJSONGetBBox(&mp, opts);
    EXPECT_DOUBLE_EQ(160.0, env.MinX);   // West limit
    EXPECT_DOUBLE_EQ(-165.0, env.MaxX);  // East limit
  }

  // RFC7946 Heuristic 2: Multipart spanning > 180 without touching +-180
  {
    OGRMultiPolygon mp;
    auto poly1 = std::make_unique<OGRPolygon>();
    auto ring1 = std::make_unique<OGRLinearRing>();
    ring1->addPoint(170.0, 10.0);
    ring1->addPoint(175.0, 10.0);
    ring1->addPoint(175.0, 20.0);
    ring1->addPoint(170.0, 20.0);
    ring1->addPoint(170.0, 10.0);
    poly1->addRingDirectly(ring1.release());
    mp.addGeometryDirectly(poly1.release());

    auto poly2 = std::make_unique<OGRPolygon>();
    auto ring2 = std::make_unique<OGRLinearRing>();
    ring2->addPoint(-175.0, 10.0);
    ring2->addPoint(-170.0, 10.0);
    ring2->addPoint(-170.0, 20.0);
    ring2->addPoint(-175.0, 20.0);
    ring2->addPoint(-175.0, 10.0);
    poly2->addRingDirectly(ring2.release());
    mp.addGeometryDirectly(poly2.release());

    env = OGRGeoJSONGetBBox(&mp, opts);
    EXPECT_DOUBLE_EQ(170.0, env.MinX);
    EXPECT_DOUBLE_EQ(-170.0, env.MaxX);

    // Add a part that aborts Heuristic 2 by having MinX > -120 and MaxX < 120
    auto poly3 = std::make_unique<OGRPolygon>();
    auto ring3 = std::make_unique<OGRLinearRing>();
    ring3->addPoint(0.0, 10.0);
    ring3->addPoint(10.0, 10.0);
    ring3->addPoint(10.0, 20.0);
    ring3->addPoint(0.0, 20.0);
    ring3->addPoint(0.0, 10.0);
    poly3->addRingDirectly(ring3.release());
    mp.addGeometryDirectly(poly3.release());

    env = OGRGeoJSONGetBBox(&mp, opts);
    EXPECT_DOUBLE_EQ(-175.0, env.MinX);
    EXPECT_DOUBLE_EQ(175.0, env.MaxX);
  }
}

TEST_F(OgrGeoJsonWriterTest, WriteBasicGeometries) {
  OGRGeoJSONWriteOptions opts;

  // nullptr geometry assert in OGRGeoJSONWriteGeometry
  // Point: empty returns nullptr
  OGRPoint ptEmpty;
  EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&ptEmpty, opts));

  // Point 2D, 3D, M, ZM
  {
    OGRPoint pt(1.5, 2.5);
    json_object* po = OGRGeoJSONWriteGeometry(&pt, opts);
    ASSERT_THAT(po, NotNull());
    const char* str = json_object_to_json_string(po);
    EXPECT_THAT(str, HasSubstr("\"type\": \"Point\""));
    EXPECT_THAT(str, HasSubstr("[ 1.5, 2.5 ]"));
    json_object_put(po);
  }
  {
    OGRPoint pt(1.0, 2.0, 3.0);
    json_object* po = OGRGeoJSONWriteGeometry(&pt, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("[ 1.0, 2.0, 3.0 ]"));
    json_object_put(po);
  }
  {
    opts.bAllowMeasure = true;
    OGRPoint pt(1.0, 2.0);
    pt.setM(4.0);
    json_object* po = OGRGeoJSONWriteGeometry(&pt, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("[ 1.0, 2.0, 4.0 ]"));
    json_object_put(po);

    OGRPoint pt3dm(1.0, 2.0, 3.0, 4.0);
    po = OGRGeoJSONWriteGeometry(&pt3dm, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ 1.0, 2.0, 3.0, 4.0 ]"));
    json_object_put(po);
    opts.bAllowMeasure = false;
  }

  // Non-finite point coordinate -> warning and returns nullptr
  {
    OGRPoint ptInf(std::numeric_limits<double>::infinity(), 2.0);
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&ptInf, opts));
  }

  // LineString 2D, 3D, empty
  {
    OGRLineString lsEmpty;
    json_object* po = OGRGeoJSONWriteGeometry(&lsEmpty, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("\"coordinates\": [ ]"));
    json_object_put(po);

    OGRLineString ls;
    ls.addPoint(1.0, 2.0);
    ls.addPoint(3.0, 4.0);
    po = OGRGeoJSONWriteGeometry(&ls, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ [ 1.0, 2.0 ], [ 3.0, 4.0 ] ]"));
    json_object_put(po);

    OGRLineString ls3d;
    ls3d.addPoint(1.0, 2.0, 5.0);
    ls3d.addPoint(3.0, 4.0, 6.0);
    po = OGRGeoJSONWriteGeometry(&ls3d, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ [ 1.0, 2.0, 5.0 ], [ 3.0, 4.0, 6.0 ] ]"));
    json_object_put(po);

    // Measured LineString (2D+M and 3D+ZM)
    opts.bAllowMeasure = true;
    OGRLineString ls2dm;
    ls2dm.addPoint(1.0, 2.0, 0.0, 10.0);
    ls2dm.addPoint(3.0, 4.0, 0.0, 20.0);
    ls2dm.set3D(false);
    po = OGRGeoJSONWriteGeometry(&ls2dm, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ [ 1.0, 2.0, 10.0 ], [ 3.0, 4.0, 20.0 ] ]"));
    json_object_put(po);

    OGRLineString ls3dzm;
    ls3dzm.addPoint(1.0, 2.0, 5.0, 10.0);
    ls3dzm.addPoint(3.0, 4.0, 6.0, 20.0);
    po = OGRGeoJSONWriteGeometry(&ls3dzm, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(
        json_object_to_json_string(po),
        HasSubstr("[ [ 1.0, 2.0, 5.0, 10.0 ], [ 3.0, 4.0, 6.0, 20.0 ] ]"));
    json_object_put(po);
    opts.bAllowMeasure = false;
  }

  // LineString with non-finite coord
  {
    OGRLineString lsNan;
    lsNan.addPoint(std::numeric_limits<double>::quiet_NaN(), 2.0);
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&lsNan, opts));
  }
}

TEST_F(OgrGeoJsonWriterTest, WritePolygonAndRightHandRule) {
  OGRGeoJSONWriteOptions opts;

  // Empty polygon
  OGRPolygon polyEmpty;
  json_object* po = OGRGeoJSONWriteGeometry(&polyEmpty, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po),
              HasSubstr("\"coordinates\": [ ]"));
  json_object_put(po);

  // Polygon with Clockwise exterior ring (standard GeoJSON/RFC7946 prefers CCW)
  // CW: (0,0) -> (0,10) -> (10,10) -> (10,0) -> (0,0)
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0.0, 0.0);
  ring->addPoint(0.0, 10.0);
  ring->addPoint(10.0, 10.0);
  ring->addPoint(10.0, 0.0);
  ring->addPoint(0.0, 0.0);
  EXPECT_TRUE(ring->isClockwise());
  poly->addRingDirectly(ring.release());

  // Without right hand rule: retains points in given order
  po = OGRGeoJSONWriteGeometry(poly.get(), opts);
  ASSERT_THAT(po, NotNull());
  json_object_put(po);

  // With right hand rule: exterior ring must be CCW, so order is reversed
  opts.bPolygonRightHandRule = true;
  po = OGRGeoJSONWriteGeometry(poly.get(), opts);
  ASSERT_THAT(po, NotNull());
  const char* str = json_object_to_json_string(po);
  EXPECT_THAT(str, HasSubstr("[ 0.0, 0.0 ], [ 10.0, 0.0 ], [ 10.0, 10.0 ]"));
  json_object_put(po);

  // Interior ring in CCW: should be inverted to CW under right hand rule
  auto inner = std::make_unique<OGRLinearRing>();
  inner->addPoint(2.0, 2.0);
  inner->addPoint(8.0, 2.0);
  inner->addPoint(8.0, 8.0);
  inner->addPoint(2.0, 8.0);
  inner->addPoint(2.0, 2.0);
  EXPECT_FALSE(inner->isClockwise());
  poly->addRingDirectly(inner.release());

  po = OGRGeoJSONWriteGeometry(poly.get(), opts);
  ASSERT_THAT(po, NotNull());
  str = json_object_to_json_string(po);
  // Interior ring was inverted to CW: (2,2) -> (2,8) -> (8,8) -> (8,2) -> (2,2)
  EXPECT_THAT(str, HasSubstr("[ 2.0, 2.0 ], [ 2.0, 8.0 ]"));
  json_object_put(po);

  // Measured LinearRing (2D+M and 3D+ZM) inside Polygon
  {
    opts.bAllowMeasure = true;
    auto polyM = std::make_unique<OGRPolygon>();
    auto ringM = std::make_unique<OGRLinearRing>();
    ringM->addPoint(0.0, 0.0, 0.0, 1.0);
    ringM->addPoint(0.0, 10.0, 0.0, 2.0);
    ringM->addPoint(10.0, 10.0, 0.0, 3.0);
    ringM->addPoint(0.0, 0.0, 0.0, 1.0);
    ringM->set3D(false);
    polyM->addRingDirectly(ringM.release());
    po = OGRGeoJSONWriteGeometry(polyM.get(), opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("[ 0.0, 0.0, 1.0 ]"));
    json_object_put(po);

    auto polyZM = std::make_unique<OGRPolygon>();
    auto ringZM = std::make_unique<OGRLinearRing>();
    ringZM->addPoint(0.0, 0.0, 5.0, 1.0);
    ringZM->addPoint(0.0, 10.0, 5.0, 2.0);
    ringZM->addPoint(10.0, 10.0, 5.0, 3.0);
    ringZM->addPoint(0.0, 0.0, 5.0, 1.0);
    polyZM->addRingDirectly(ringZM.release());
    po = OGRGeoJSONWriteGeometry(polyZM.get(), opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ 0.0, 0.0, 5.0, 1.0 ]"));
    json_object_put(po);
    opts.bAllowMeasure = false;
  }

  // Polygon with non-finite coord
  auto polyNan = std::make_unique<OGRPolygon>();
  auto ringNan = std::make_unique<OGRLinearRing>();
  ringNan->addPoint(std::numeric_limits<double>::infinity(), 0.0);
  polyNan->addRingDirectly(ringNan.release());
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWritePolygon(polyNan.get(), opts));
  }
}

TEST_F(OgrGeoJsonWriterTest, WriteMultiGeometriesAndCollections) {
  OGRGeoJSONWriteOptions opts;

  // MultiPoint
  {
    OGRMultiPoint mpEmpty;
    json_object* po = OGRGeoJSONWriteGeometry(&mpEmpty, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"MultiPoint\""));
    json_object_put(po);

    OGRMultiPoint mp;
    mp.addGeometryDirectly(new OGRPoint(1.0, 2.0));
    mp.addGeometryDirectly(new OGRPoint(3.0, 4.0));
    po = OGRGeoJSONWriteGeometry(&mp, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ [ 1.0, 2.0 ], [ 3.0, 4.0 ] ]"));
    json_object_put(po);

    OGRMultiPoint mpNan;
    mpNan.addGeometryDirectly(
        new OGRPoint(std::numeric_limits<double>::quiet_NaN(), 0.0));
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&mpNan, opts));
  }

  // MultiLineString
  {
    OGRMultiLineString mlsEmpty;
    json_object* po = OGRGeoJSONWriteGeometry(&mlsEmpty, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("\"MultiLineString\""));
    json_object_put(po);

    OGRMultiLineString mls;
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0.0, 0.0);
    ls->addPoint(1.0, 1.0);
    mls.addGeometryDirectly(ls.release());
    po = OGRGeoJSONWriteGeometry(&mls, opts);
    ASSERT_THAT(po, NotNull());
    json_object_put(po);

    OGRMultiLineString mlsNan;
    auto lsNan = std::make_unique<OGRLineString>();
    lsNan->addPoint(std::numeric_limits<double>::quiet_NaN(), 0.0);
    mlsNan.addGeometryDirectly(lsNan.release());
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&mlsNan, opts));
  }

  // MultiPolygon
  {
    OGRMultiPolygon mpEmpty;
    json_object* po = OGRGeoJSONWriteGeometry(&mpEmpty, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"MultiPolygon\""));
    json_object_put(po);

    OGRMultiPolygon mp;
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0.0, 0.0);
    ring->addPoint(1.0, 0.0);
    ring->addPoint(1.0, 1.0);
    ring->addPoint(0.0, 0.0);
    poly->addRingDirectly(ring.release());
    mp.addGeometryDirectly(poly.release());
    po = OGRGeoJSONWriteGeometry(&mp, opts);
    ASSERT_THAT(po, NotNull());
    json_object_put(po);

    OGRMultiPolygon mpNan;
    auto polyNan = std::make_unique<OGRPolygon>();
    auto ringNan = std::make_unique<OGRLinearRing>();
    ringNan->addPoint(std::numeric_limits<double>::infinity(), 0.0);
    polyNan->addRingDirectly(ringNan.release());
    mpNan.addGeometryDirectly(polyNan.release());
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&mpNan, opts));
  }

  // GeometryCollection
  {
    OGRGeometryCollection gcEmpty;
    json_object* po = OGRGeoJSONWriteGeometry(&gcEmpty, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("\"GeometryCollection\""));
    json_object_put(po);

    OGRGeometryCollection gc;
    gc.addGeometryDirectly(new OGRPoint(5.0, 6.0));
    po = OGRGeoJSONWriteGeometry(&gc, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("[ 5.0, 6.0 ]"));
    json_object_put(po);

    OGRGeometryCollection gcNan;
    gcNan.addGeometryDirectly(
        new OGRPoint(std::numeric_limits<double>::quiet_NaN(), 0.0));
    WithQuietHandler quiet;
    json_object* poNan = OGRGeoJSONWriteGeometry(&gcNan, opts);
    if (poNan != nullptr) {
      json_object_put(poNan);
    }
  }
}

TEST_F(OgrGeoJsonWriterTest, WriteCurvedGeometries) {
  OGRGeoJSONWriteOptions opts;

  // CircularString with <= 11 points (e.g. 3 points)
  OGRCircularString cs;
  cs.addPoint(0.0, 0.0);
  cs.addPoint(1.0, 1.0);
  cs.addPoint(2.0, 0.0);

  // Default: bAllowCurve = false -> linearized
  {
    json_object* po = OGRGeoJSONWriteGeometry(&cs, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"LineString\""));
    json_object_put(po);
  }

  // bAllowCurve = true
  opts.bAllowCurve = true;
  {
    json_object* po = OGRGeoJSONWriteGeometry(&cs, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("\"CircularString\""));
    json_object_put(po);
  }

  // CircularString with > 11 points (splits into CompoundCurve)
  {
    OGRCircularString csLarge;
    for (int i = 0; i < 15; ++i) {
      csLarge.addPoint(static_cast<double>(i), static_cast<double>(i % 3));
    }
    json_object* po = OGRGeoJSONWriteGeometry(&csLarge, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"CompoundCurve\""));
    json_object_put(po);
  }

  // CompoundCurve
  {
    OGRCompoundCurve cc;
    auto csSub = std::make_unique<OGRCircularString>();
    csSub->addPoint(0.0, 0.0);
    csSub->addPoint(1.0, 1.0);
    csSub->addPoint(2.0, 0.0);
    cc.addCurveDirectly(csSub.release());
    json_object* po = OGRGeoJSONWriteGeometry(&cc, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"CompoundCurve\""));
    json_object_put(po);

    opts.bAllowCurve = false;
    po = OGRGeoJSONWriteGeometry(&cc, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"LineString\""));
    json_object_put(po);
    opts.bAllowCurve = true;
  }

  // CurvePolygon
  {
    OGRCurvePolygon cp;
    auto csRing = std::make_unique<OGRCircularString>();
    csRing->addPoint(0.0, 0.0);
    csRing->addPoint(1.0, 1.0);
    csRing->addPoint(2.0, 0.0);
    csRing->addPoint(1.0, -1.0);
    csRing->addPoint(0.0, 0.0);
    cp.addRingDirectly(csRing.release());
    json_object* po = OGRGeoJSONWriteGeometry(&cp, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"CurvePolygon\""));
    json_object_put(po);

    opts.bAllowCurve = false;
    po = OGRGeoJSONWriteGeometry(&cp, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"Polygon\""));
    json_object_put(po);
    opts.bAllowCurve = true;
  }

  // MultiCurve and MultiSurface
  {
    OGRMultiCurve mc;
    mc.addGeometryDirectly(cs.clone());
    json_object* po = OGRGeoJSONWriteGeometry(&mc, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"MultiCurve\""));
    json_object_put(po);

    opts.bAllowCurve = false;
    po = OGRGeoJSONWriteGeometry(&mc, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("\"MultiLineString\""));
    json_object_put(po);
    opts.bAllowCurve = true;

    // Nested GeometryCollection containing curved geometry with bAllowCurve =
    // false
    opts.bAllowCurve = false;
    OGRGeometryCollection gcOuter;
    auto gcInner = std::make_unique<OGRGeometryCollection>();
    auto csCurv = std::make_unique<OGRCircularString>();
    csCurv->addPoint(0.0, 0.0);
    csCurv->addPoint(1.0, 1.0);
    csCurv->addPoint(2.0, 0.0);
    gcInner->addGeometryDirectly(csCurv.release());
    gcOuter.addGeometryDirectly(gcInner.release());
    po = OGRGeoJSONWriteGeometry(&gcOuter, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"LineString\""));
    json_object_put(po);
    opts.bAllowCurve = true;
  }
  {
    OGRMultiSurface ms;
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0.0, 0.0);
    ring->addPoint(1.0, 0.0);
    ring->addPoint(1.0, 1.0);
    ring->addPoint(0.0, 0.0);
    poly->addRingDirectly(ring.release());
    ms.addGeometryDirectly(poly.release());
    json_object* po = OGRGeoJSONWriteGeometry(&ms, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"MultiSurface\""));
    json_object_put(po);
  }

  // Unsupported geometry directly passed (e.g. Triangle)
  {
    OGRTriangle tri(OGRPoint(0.0, 0.0), OGRPoint(1.0, 0.0), OGRPoint(0.0, 1.0));
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGRGeoJSONWriteGeometry(&tri, opts));
  }
}

TEST_F(OgrGeoJsonWriterTest, PrecisionAndFormatting) {
  OGRGeoJSONWriteOptions opts;
  opts.nXYCoordPrecision = 2;
  opts.nZCoordPrecision = 1;

  OGRPoint pt(1.234567, 9.876543, 5.555);
  json_object* po = OGRGeoJSONWriteGeometry(&pt, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("[ 1.23, 9.88, 5.6 ]"));
  json_object_put(po);

  // Significant figures
  opts.nXYCoordPrecision = -1;
  opts.nZCoordPrecision = -1;
  opts.nSignificantFigures = 3;
  po = OGRGeoJSONWriteGeometry(&pt, opts);
  ASSERT_THAT(po, NotNull());
  json_object_put(po);
}

TEST_F(OgrGeoJsonWriterTest, WriteFeatureAndAttributes) {
  OGRFeatureDefn* defn = new OGRFeatureDefn("TestDefn");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };
  OGRFieldDefn fInt("f_int", OFTInteger);
  defn->AddFieldDefn(&fInt);
  OGRFieldDefn fBool("f_bool", OFTInteger);
  fBool.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&fBool);
  OGRFieldDefn fInt64("f_int64", OFTInteger64);
  defn->AddFieldDefn(&fInt64);
  OGRFieldDefn fReal("f_real", OFTReal);
  defn->AddFieldDefn(&fReal);
  OGRFieldDefn fFloat32("f_float32", OFTReal);
  fFloat32.SetSubType(OFSTFloat32);
  defn->AddFieldDefn(&fFloat32);
  OGRFieldDefn fStr("f_str", OFTString);
  defn->AddFieldDefn(&fStr);
  OGRFieldDefn fJson("f_json", OFTString);
  fJson.SetSubType(OFSTJSON);
  defn->AddFieldDefn(&fJson);
  OGRFieldDefn fIntList("f_int_list", OFTIntegerList);
  defn->AddFieldDefn(&fIntList);
  OGRFieldDefn fInt64List("f_int64_list", OFTInteger64List);
  defn->AddFieldDefn(&fInt64List);
  OGRFieldDefn fRealList("f_real_list", OFTRealList);
  defn->AddFieldDefn(&fRealList);
  OGRFieldDefn fStrList("f_str_list", OFTStringList);
  defn->AddFieldDefn(&fStrList);
  OGRFieldDefn fDate("f_date", OFTDate);
  defn->AddFieldDefn(&fDate);
  OGRFieldDefn fTime("f_time", OFTTime);
  defn->AddFieldDefn(&fTime);
  OGRFieldDefn fDateTime("f_datetime", OFTDateTime);
  defn->AddFieldDefn(&fDateTime);
  OGRFieldDefn fNull("f_null", OFTString);
  defn->AddFieldDefn(&fNull);
  OGRFieldDefn fUnset("f_unset", OFTString);
  defn->AddFieldDefn(&fUnset);
  OGRFieldDefn fBool64("f_bool64", OFTInteger64);
  fBool64.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&fBool64);
  OGRFieldDefn fBoolList("f_bool_list", OFTIntegerList);
  fBoolList.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&fBoolList);
  OGRFieldDefn fBool64List("f_bool64_list", OFTInteger64List);
  fBool64List.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&fBool64List);
  OGRFieldDefn fFloat32List("f_float32_list", OFTRealList);
  fFloat32List.SetSubType(OFSTFloat32);
  defn->AddFieldDefn(&fFloat32List);

  OGRFeature feat(defn);
  feat.SetFID(42);
  feat.SetField("f_int", 123);
  feat.SetField("f_bool", 1);
  feat.SetField("f_int64", static_cast<GIntBig>(9876543210LL));
  feat.SetField("f_bool64", static_cast<GIntBig>(1));
  feat.SetField("f_real", 12.3456);
  feat.SetField("f_float32", 3.14f);
  feat.SetField("f_str", "hello world");
  feat.SetField("f_json", "{\"key\":\"val\"}");
  int intList[] = {1, 2, 3};
  feat.SetField("f_int_list", 3, intList);
  int bList[] = {1, 0};
  feat.SetField("f_bool_list", 2, bList);
  GIntBig int64List[] = {10LL, 20LL};
  feat.SetField("f_int64_list", 2, int64List);
  GIntBig b64List[] = {0LL, 1LL};
  feat.SetField("f_bool64_list", 2, b64List);
  double realList[] = {1.1, 2.2};
  feat.SetField("f_real_list", 2, realList);
  double fltList[] = {1.5, 2.5};
  feat.SetField("f_float32_list", 2, fltList);
  const char* strList[] = {"a", "b", nullptr};
  feat.SetField(feat.GetFieldIndex("f_str_list"), const_cast<char**>(strList));
  feat.SetField("f_date", "2024-05-10");
  feat.SetField("f_time", "12:30:00");
  feat.SetField("f_datetime", "2024-05-10T12:30:00Z");
  feat.SetFieldNull(feat.GetFieldIndex("f_null"));
  // f_unset left unset

  auto pt = std::make_unique<OGRPoint>(10.0, 20.0, 5.0);
  feat.SetGeometryDirectly(pt.release());

  OGRGeoJSONWriteOptions opts;
  opts.bWriteBBOX = true;
  json_object* po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  const char* str = json_object_to_json_string(po);
  EXPECT_THAT(str, HasSubstr("\"id\": 42"));
  EXPECT_THAT(str, HasSubstr("\"bbox\": [ 10.0, 20.0, 5.0, 10.0, 20.0, 5.0 ]"));
  EXPECT_THAT(str, HasSubstr("\"f_int\": 123"));
  EXPECT_THAT(str, HasSubstr("\"f_bool\": true"));
  EXPECT_THAT(str, HasSubstr("\"f_bool64\": 1"));
  EXPECT_THAT(str, HasSubstr("\"f_int64\": 9876543210"));
  EXPECT_THAT(str, HasSubstr("\"f_str\": \"hello world\""));
  EXPECT_THAT(str, HasSubstr("\"f_json\": { \"key\": \"val\" }"));
  EXPECT_THAT(str, HasSubstr("\"f_int_list\": [ 1, 2, 3 ]"));
  EXPECT_THAT(str, HasSubstr("\"f_bool_list\": [ true, false ]"));
  EXPECT_THAT(str, HasSubstr("\"f_int64_list\": [ 10, 20 ]"));
  EXPECT_THAT(str, HasSubstr("\"f_bool64_list\": [ 0, 1 ]"));
  EXPECT_THAT(str, HasSubstr("\"f_real_list\": [ 1.1, 2.2 ]"));
  EXPECT_THAT(str, HasSubstr("\"f_float32_list\": [ 1.5, 2.5 ]"));
  EXPECT_THAT(str, HasSubstr("\"f_str_list\": [ \"a\", \"b\" ]"));
  EXPECT_THAT(str, HasSubstr("\"f_date\": \"2024-05-10\""));
  EXPECT_THAT(str, HasSubstr("\"f_null\": null"));
  json_object_put(po);

  // Non-finite real value: NaN / Inf
  feat.SetField("f_real", std::numeric_limits<double>::quiet_NaN());
  opts.bAllowNonFiniteValues = false;
  {
    WithQuietHandler quiet;
    po = OGRGeoJSONWriteFeature(&feat, opts);
  }
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po),
              ::testing::Not(HasSubstr("\"f_real\"")));
  json_object_put(po);

  opts.bAllowNonFiniteValues = true;
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"f_real\": NaN"));
  json_object_put(po);

  // Forced string ID
  opts.bForceIDFieldType = true;
  opts.eForcedIDFieldType = OFTString;
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"id\": \"42\""));
  json_object_put(po);

  // Custom ID field
  opts.osIDField = "f_str";
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po),
              HasSubstr("\"id\": \"hello world\""));
  json_object_put(po);

  // Custom ID field as integer
  opts.osIDField = "f_int";
  opts.bForceIDFieldType = false;
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"id\": 123"));
  json_object_put(po);

  opts.bForceIDFieldType = true;
  opts.eForcedIDFieldType = OFTInteger64;
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"id\": 123"));
  json_object_put(po);
}

TEST_F(OgrGeoJsonWriterTest, NativeMediaDataAndPatching) {
  OGRFeatureDefn* defn = new OGRFeatureDefn("NativeTestDefn");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };
  OGRFieldDefn fJsonProp("native_prop", OFTString);
  fJsonProp.SetSubType(OFSTJSON);
  defn->AddFieldDefn(&fJsonProp);
  OGRFieldDefn fIdStr("id", OFTString);
  defn->AddFieldDefn(&fIdStr);

  OGRFeature feat(defn);
  feat.SetFID(100);
  feat.SetField("native_prop", "\"native_val\"");
  feat.SetField("id", "native_id_100");

  // 3D Point
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0, 3.0);
  feat.SetGeometryDirectly(pt.release());

  feat.SetNativeMediaType("application/vnd.geo+json");
  const char* pszNative =
      "{\n"
      "  \"type\": \"Feature\",\n"
      "  \"id\": \"native_id_100\",\n"
      "  \"bbox\": [1.0, 2.0, 1.0, 2.0],\n"
      "  \"geometry\": {\n"
      "    \"type\": \"Point\",\n"
      "    \"coordinates\": [1.0, 2.0, 3.0, 4.0],\n"
      "    \"foreign_geom_member\": 12345\n"
      "  },\n"
      "  \"properties\": {\n"
      "    \"native_prop\": \"native_val\"\n"
      "  },\n"
      "  \"top_level_custom\": \"custom_val\"\n"
      "}";
  feat.SetNativeData(pszNative);

  OGRGeoJSONWriteOptions opts;
  opts.bCanPatchCoordinatesWithNativeData = true;
  json_object* po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  const char* str = json_object_to_json_string(po);
  EXPECT_THAT(str, HasSubstr("\"top_level_custom\": \"custom_val\""));
  EXPECT_THAT(str, HasSubstr("\"foreign_geom_member\": 12345"));
  // 4th coordinate patched from native coordinates [1.0, 2.0, 3.0, 4.0]
  EXPECT_THAT(str, HasSubstr("[ 1.0, 2.0, 3.0, 4.0 ]"));
  json_object_put(po);

  // Forced int ID from string native id
  opts.bForceIDFieldType = true;
  opts.eForcedIDFieldType = OFTInteger64;
  const char* pszNativeStrId =
      "{\"type\":\"Feature\",\"id\":\"999\",\"geometry\":null,\"properties\":{}"
      "}";
  feat.SetNativeData(pszNativeStrId);
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"id\": 999"));
  json_object_put(po);

  // Forced string ID from int native id
  opts.eForcedIDFieldType = OFTString;
  const char* pszNativeIntId =
      "{\"type\":\"Feature\",\"id\":888,\"geometry\":null,\"properties\":{}}";
  feat.SetNativeData(pszNativeIntId);
  po = OGRGeoJSONWriteFeature(&feat, opts);
  ASSERT_THAT(po, NotNull());
  EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"id\": \"888\""));
  json_object_put(po);

  // Native ID matching feature integer ID
  {
    OGRFeatureDefn* defnInt = new OGRFeatureDefn("DefnInt");
    defnInt->Reference();
    absl::Cleanup defn_int_cleanup = [defnInt] { defnInt->Release(); };
    OGRFieldDefn fIntId("id", OFTInteger);
    defnInt->AddFieldDefn(&fIntId);
    OGRFeature featInt(defnInt);
    featInt.SetField("id", 777);
    featInt.SetNativeMediaType("application/vnd.geo+json");
    const char* pszNativeMatchInt =
        "{\"type\":\"Feature\",\"id\":777,\"properties\":{\"id\":777}}";
    featInt.SetNativeData(pszNativeMatchInt);
    opts.bForceIDFieldType = false;
    po = OGRGeoJSONWriteFeature(&featInt, opts);
    ASSERT_THAT(po, NotNull());
    json_object_put(po);
  }

  // MultiPolygon native coordinate patching (depth 3)
  {
    OGRMultiPolygon mp;
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0.0, 0.0, 0.0);
    ring->addPoint(0.0, 1.0, 0.0);
    ring->addPoint(1.0, 1.0, 0.0);
    ring->addPoint(0.0, 0.0, 0.0);
    poly->addRingDirectly(ring.release());
    mp.addGeometryDirectly(poly.release());

    OGRFeature featMp(defn);
    featMp.SetGeometry(&mp);
    featMp.SetNativeMediaType("application/vnd.geo+json");
    const char* pszNativeMp =
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"MultiPolygon\","
        "\"coordinates\":[[[[0.0,0.0,0.0,1.0],[0.0,1.0,0.0,1.0],[1.0,1.0,0.0,1."
        "0],[0.0,0.0,0.0,1.0]]]]}}";
    featMp.SetNativeData(pszNativeMp);
    opts.bCanPatchCoordinatesWithNativeData = true;
    po = OGRGeoJSONWriteFeature(&featMp, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ 0.0, 0.0, 0.0, 1.0 ]"));
    json_object_put(po);
  }

  // GeometryCollection native geometry patching
  {
    OGRGeometryCollection gc;
    gc.addGeometryDirectly(new OGRPoint(1.0, 2.0, 3.0));
    OGRFeature featGc(defn);
    featGc.SetGeometry(&gc);
    featGc.SetNativeMediaType("application/vnd.geo+json");
    const char* pszNativeGc =
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"GeometryCollection\","
        "\"geometries\":[{\"type\":\"Point\",\"coordinates\":[1.0,2.0,3.0,4.0],"
        "\"extra\":100}]}}";
    featGc.SetNativeData(pszNativeGc);
    opts.bCanPatchCoordinatesWithNativeData = true;
    po = OGRGeoJSONWriteFeature(&featGc, opts);
    ASSERT_THAT(po, NotNull());
    EXPECT_THAT(json_object_to_json_string(po), HasSubstr("\"extra\": 100"));
    EXPECT_THAT(json_object_to_json_string(po),
                HasSubstr("[ 1.0, 2.0, 3.0, 4.0 ]"));
    json_object_put(po);

    // Mismatched length and mismatched child type
    const char* pszNativeGcMismatchLen =
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"GeometryCollection\","
        "\"geometries\":[]}}";
    featGc.SetNativeData(pszNativeGcMismatchLen);
    po = OGRGeoJSONWriteFeature(&featGc, opts);
    ASSERT_THAT(po, NotNull());
    json_object_put(po);

    const char* pszNativeGcMismatchType =
        "{\"type\":\"Feature\",\"geometry\":{\"type\":\"GeometryCollection\","
        "\"geometries\":[{\"type\":\"Polygon\",\"coordinates\":[]}]}}";
    featGc.SetNativeData(pszNativeGcMismatchType);
    po = OGRGeoJSONWriteFeature(&featGc, opts);
    ASSERT_THAT(po, NotNull());
    json_object_put(po);
  }

  // Compatible coordinates (2D vs 2D) and RFC7946 reserved member filtering
  {
    OGRPoint pt2d(10.0, 20.0);
    OGRFeature feat2d(defn);
    feat2d.SetGeometry(&pt2d);
    feat2d.SetNativeMediaType("application/vnd.geo+json");
    const char* pszNativeRfc =
        "{\"type\":\"Feature\",\"coordinates\":1,\"geometries\":2,\"features\":"
        "3,"
        "\"geometry\":{\"type\":\"Point\",\"coordinates\":[10.0,20.0],"
        "\"geometry\":1,\"properties\":2,\"features\":3}}";
    feat2d.SetNativeData(pszNativeRfc);
    opts.bHonourReservedRFC7946Members = true;
    opts.bCanPatchCoordinatesWithNativeData = false;
    po = OGRGeoJSONWriteFeature(&feat2d, opts);
    ASSERT_THAT(po, NotNull());
    json_object_put(po);
  }
}

TEST_F(OgrGeoJsonWriterTest, ExportToJsonCApi) {
  OGRPoint pt(12.3456, 45.6789);

  // Null geometry handle
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OGR_G_ExportToJson(nullptr));
    EXPECT_EQ(nullptr, OGR_G_ExportToJsonEx(nullptr, nullptr));
  }

  // Basic export
  char* pszJson = OGR_G_ExportToJson(OGRGeometry::ToHandle(&pt));
  ASSERT_THAT(pszJson, NotNull());
  EXPECT_THAT(pszJson, HasSubstr("\"type\": \"Point\""));
  EXPECT_THAT(pszJson, HasSubstr("[ 12.3456, 45.6789 ]"));
  CPLFree(pszJson);

  // ExportToJsonEx with options
  CPLStringList options;
  options.AddNameValue("COORDINATE_PRECISION", "1");
  pszJson = OGR_G_ExportToJsonEx(OGRGeometry::ToHandle(&pt), options.List());
  ASSERT_THAT(pszJson, NotNull());
  EXPECT_THAT(pszJson, HasSubstr("[ 12.3, 45.7 ]"));
  CPLFree(pszJson);

  // XY and Z precision
  options.Clear();
  options.AddNameValue("XY_COORD_PRECISION", "2");
  options.AddNameValue("Z_COORD_PRECISION", "1");
  OGRPoint pt3d(1.234, 5.678, 9.999);
  pszJson = OGR_G_ExportToJsonEx(OGRGeometry::ToHandle(&pt3d), options.List());
  ASSERT_THAT(pszJson, NotNull());
  EXPECT_THAT(pszJson, HasSubstr("[ 1.23, 5.68, 10.0 ]"));
  CPLFree(pszJson);

  // Significant figures
  options.Clear();
  options.AddNameValue("SIGNIFICANT_FIGURES", "2");
  pszJson = OGR_G_ExportToJsonEx(OGRGeometry::ToHandle(&pt), options.List());
  ASSERT_THAT(pszJson, NotNull());
  CPLFree(pszJson);

  // Axis order swapping on EPSG:4326 Lat/Long
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // {1, 2} mapping
    // But EPSGTreatsAsLatLong is true for EPSG:4326
    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    OGRPoint ptLatLong(10.0, 20.0);  // X=10 (lat), Y=20 (long)
    ptLatLong.assignSpatialReference(&srs4326);

    pszJson = OGR_G_ExportToJson(OGRGeometry::ToHandle(&ptLatLong));
    ASSERT_THAT(pszJson, NotNull());
    // Geometry coordinates before export was (10, 20).
    // swapXY should swap them to [ 20.0, 10.0 ] for GeoJSON (Long, Lat)
    EXPECT_THAT(pszJson, HasSubstr("[ 20.0, 10.0 ]"));
    // And verify geometry itself was unswapped back to (10, 20)
    EXPECT_DOUBLE_EQ(10.0, ptLatLong.getX());
    EXPECT_DOUBLE_EQ(20.0, ptLatLong.getY());
    CPLFree(pszJson);
  }
}

}  // namespace
}  // namespace autotest2
