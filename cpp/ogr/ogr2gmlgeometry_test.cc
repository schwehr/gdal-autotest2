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

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

class Ogr2GmlGeometryTest : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(Ogr2GmlGeometryTest, NullGeometryExport) {
  char* gml = OGR_G_ExportToGML(nullptr);
  ASSERT_THAT(gml, NotNull());
  EXPECT_STREQ(gml, "");
  CPLFree(gml);

  gml = OGR_G_ExportToGMLEx(nullptr, nullptr);
  ASSERT_THAT(gml, NotNull());
  EXPECT_STREQ(gml, "");
  CPLFree(gml);

  EXPECT_EQ(nullptr, OGR_G_ExportToGMLTree(nullptr));
}

TEST_F(Ogr2GmlGeometryTest, ExportEnvelopeToGMLTree) {
  OGRPoint pt(10.0, 20.0);
  CPLXMLNode* box = OGR_G_ExportEnvelopeToGMLTree(OGRGeometry::ToHandle(&pt));
  ASSERT_THAT(box, NotNull());
  char* serialized = CPLSerializeXMLTree(box);
  EXPECT_THAT(serialized, HasSubstr("<gml:Box>"));
  EXPECT_THAT(serialized, HasSubstr("<gml:coord>"));
  EXPECT_THAT(serialized, HasSubstr("<gml:X>10</gml:X>"));
  EXPECT_THAT(serialized, HasSubstr("<gml:Y>20</gml:Y>"));
  CPLFree(serialized);
  CPLDestroyXMLNode(box);
}

TEST_F(Ogr2GmlGeometryTest, ExportToGMLTree) {
  OGRPoint pt(1.0, 2.0);
  CPLXMLNode* tree = OGR_G_ExportToGMLTree(OGRGeometry::ToHandle(&pt));
  ASSERT_THAT(tree, NotNull());
  char* serialized = CPLSerializeXMLTree(tree);
  EXPECT_THAT(serialized, HasSubstr("<gml:Point>"));
  EXPECT_THAT(serialized, HasSubstr("<gml:coordinates>1,2</gml:coordinates>"));
  CPLFree(serialized);
  CPLDestroyXMLNode(tree);

  // Unsupported geometry in GML2 returns nullptr
  OGRCircularString cs;
  cs.addPoint(0, 0);
  cs.addPoint(1, 1);
  cs.addPoint(2, 0);
  EXPECT_EQ(nullptr, OGR_G_ExportToGMLTree(OGRGeometry::ToHandle(&cs)));
}

TEST_F(Ogr2GmlGeometryTest, Gml2ExportGeometries) {
  // Point 2D and 3D
  {
    OGRPoint pt(1.5, 2.5);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&pt));
    EXPECT_THAT(gml, HasSubstr("<gml:Point><gml:coordinates>1.5,2.5</"
                               "gml:coordinates></gml:Point>"));
    CPLFree(gml);

    OGRPoint pt3d(1.5, 2.5, 3.5);
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&pt3d));
    EXPECT_THAT(gml, HasSubstr("<gml:Point><gml:coordinates>1.5,2.5,3.5</"
                               "gml:coordinates></gml:Point>"));
    CPLFree(gml);

    OGRPoint empty_pt;
    empty_pt.empty();
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_pt));
    EXPECT_THAT(gml, HasSubstr("<gml:Point>"));
    CPLFree(gml);
  }

  // LineString 2D, 3D, and < 2 points
  {
    OGRLineString ls;
    ls.addPoint(0.0, 0.0);
    ls.addPoint(1.0, 2.0);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&ls));
    EXPECT_THAT(gml, HasSubstr("<gml:LineString><gml:coordinates>0,0 "
                               "1,2</gml:coordinates></gml:LineString>"));
    CPLFree(gml);

    OGRLineString ls3d;
    ls3d.addPoint(0.0, 0.0, 1.0);
    ls3d.addPoint(1.0, 2.0, 3.0);
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&ls3d));
    EXPECT_THAT(gml, HasSubstr("<gml:LineString><gml:coordinates>0,0,1 "
                               "1,2,3</gml:coordinates></gml:LineString>"));
    CPLFree(gml);

    OGRLineString empty_ls;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_ls));
    EXPECT_THAT(gml, HasSubstr("<gml:LineString>"));
    CPLFree(gml);

    OGRLineString single_pt_ls;
    single_pt_ls.addPoint(1.0, 1.0);
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&single_pt_ls));
    CPLFree(gml);
  }

  // LinearRing
  {
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(1, 0);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&ring));
    EXPECT_THAT(gml, HasSubstr("<gml:LinearRing><gml:coordinates>0,0 1,0 1,1 "
                               "0,0</gml:coordinates></gml:LinearRing>"));
    CPLFree(gml);

    OGRLinearRing empty_ring;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_ring));
    EXPECT_THAT(gml, HasSubstr("<gml:LinearRing>"));
    CPLFree(gml);

    OGRLinearRing invalid_ring;
    invalid_ring.addPoint(0, 0);
    invalid_ring.addPoint(1, 1);
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&invalid_ring));
    CPLFree(gml);
  }

  // Polygon
  {
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(10, 0);
    ring.addPoint(10, 10);
    ring.addPoint(0, 10);
    ring.addPoint(0, 0);
    poly.addRing(&ring);

    OGRLinearRing inner;
    inner.addPoint(2, 2);
    inner.addPoint(8, 2);
    inner.addPoint(8, 8);
    inner.addPoint(2, 8);
    inner.addPoint(2, 2);
    poly.addRing(&inner);

    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&poly));
    EXPECT_THAT(gml, HasSubstr("<gml:Polygon>"));
    EXPECT_THAT(gml, HasSubstr("<gml:outerBoundaryIs>"));
    EXPECT_THAT(gml, HasSubstr("<gml:innerBoundaryIs>"));
    CPLFree(gml);

    OGRPolygon empty_poly;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_poly));
    EXPECT_THAT(gml, HasSubstr("<gml:Polygon>"));
    CPLFree(gml);
  }

  // MultiPoint
  {
    OGRMultiPoint mp;
    OGRPoint p1(1, 2);
    OGRPoint p2(3, 4);
    mp.addGeometry(&p1);
    mp.addGeometry(&p2);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&mp));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiPoint>"));
    EXPECT_THAT(gml, HasSubstr("<gml:pointMember>"));
    CPLFree(gml);

    OGRMultiPoint empty_mp;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_mp));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiPoint>"));
    CPLFree(gml);
  }

  // MultiLineString
  {
    OGRMultiLineString mls;
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    mls.addGeometry(&ls);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&mls));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiLineString>"));
    EXPECT_THAT(gml, HasSubstr("<gml:lineStringMember>"));
    CPLFree(gml);

    OGRMultiLineString empty_mls;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_mls));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiLineString>"));
    CPLFree(gml);
  }

  // MultiPolygon
  {
    OGRMultiPolygon mpoly;
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(1, 0);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    mpoly.addGeometry(&poly);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&mpoly));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiPolygon>"));
    EXPECT_THAT(gml, HasSubstr("<gml:polygonMember>"));
    CPLFree(gml);

    OGRMultiPolygon empty_mpoly;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_mpoly));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiPolygon>"));
    CPLFree(gml);
  }

  // GeometryCollection
  {
    OGRGeometryCollection gc;
    OGRPoint p(1, 2);
    gc.addGeometry(&p);
    char* gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&gc));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiGeometry>"));
    EXPECT_THAT(gml, HasSubstr("<gml:geometryMember>"));
    CPLFree(gml);

    OGRGeometryCollection empty_gc;
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&empty_gc));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiGeometry>"));
    CPLFree(gml);

    // GC containing unsupported geometry in GML2
    OGRCircularString cs;
    cs.addPoint(0, 0);
    cs.addPoint(1, 1);
    cs.addPoint(2, 0);
    gc.addGeometry(&cs);
    gml = OGR_G_ExportToGML(OGRGeometry::ToHandle(&gc));
    EXPECT_EQ(gml, nullptr);
  }
}

TEST_F(Ogr2GmlGeometryTest, Gml2OptionsAndPrecision) {
  OGRPoint pt(1.1234567, 2.7654321, 3.9876543);
  OGRSpatialReference* srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  pt.assignSpatialReference(srs);
  srs->Release();

  const char* const options[] = {
      "FORMAT=GML2",
      "NAMESPACE_DECL=YES",
      "XY_COORD_RESOLUTION=1e-3",
      "Z_COORD_RESOLUTION=1e-2",
      nullptr,
  };
  char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                  const_cast<char**>(options));
  ASSERT_THAT(gml, NotNull());
  EXPECT_THAT(gml, HasSubstr("xmlns:gml=\"http://www.opengis.net/gml\""));
  EXPECT_THAT(gml, HasSubstr("srsName=\"EPSG:4326\""));
  EXPECT_THAT(gml, HasSubstr("1.123,2.765,3.99"));
  CPLFree(gml);

  // Projected SRS
  OGRSpatialReference* srsProj = new OGRSpatialReference();
  srsProj->importFromEPSG(32611);
  pt.assignSpatialReference(srsProj);
  srsProj->Release();
  gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                            const_cast<char**>(options));
  ASSERT_THAT(gml, NotNull());
  EXPECT_THAT(gml, HasSubstr("srsName=\"EPSG:32611\""));
  CPLFree(gml);
}

TEST_F(Ogr2GmlGeometryTest, Gml3OptionsAndFormats) {
  OGRPoint pt(10.0, 20.0, 30.0);
  OGRSpatialReference* srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  pt.assignSpatialReference(srs);
  srs->Release();

  // SRSNAME_FORMAT variations
  for (const char* fmt : {"SHORT", "OGC_URN", "OGC_URL", "INVALID"}) {
    std::string opt = std::string("SRSNAME_FORMAT=") + fmt;
    const char* const opts[] = {"FORMAT=GML3", "GMLID=pt1", opt.c_str(),
                                nullptr};
    WithQuietHandler q;
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    CPLFree(gml);
  }

  // GML3_LONGSRS=NO vs YES
  {
    const char* const opts[] = {"FORMAT=GML3", "GMLID=pt1", "GML3_LONGSRS=NO",
                                nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    EXPECT_THAT(gml, HasSubstr("srsName=\"EPSG:4326\""));
    CPLFree(gml);
  }
  {
    const char* const opts[] = {"FORMAT=GML3", "GMLID=pt1", "GML3_LONGSRS=YES",
                                "SRSNAME_FORMAT=SHORT", nullptr};
    WithQuietHandler q;
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    CPLFree(gml);
  }

  // SRSDIMENSION_LOC variations
  for (const char* loc :
       {"POSLIST", "GEOMETRY", "GEOMETRY,POSLIST", "INVALID"}) {
    std::string opt = std::string("SRSDIMENSION_LOC=") + loc;
    const char* const opts[] = {"FORMAT=GML3", "GMLID=pt1", opt.c_str(),
                                nullptr};
    WithQuietHandler q;
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    CPLFree(gml);
  }

  // COORD_SWAP=YES and NO
  {
    const char* const opts[] = {"FORMAT=GML3", "GMLID=pt1", "COORD_SWAP=NO",
                                nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    EXPECT_THAT(gml, HasSubstr("<gml:pos>10 20 30</gml:pos>"));
    CPLFree(gml);
  }
  {
    const char* const opts[] = {"FORMAT=GML3", "GMLID=pt1", "COORD_SWAP=YES",
                                nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    EXPECT_THAT(gml, HasSubstr("<gml:pos>20 10 30</gml:pos>"));
    CPLFree(gml);
  }

  // FORMAT=GML32 without GMLID (warning) vs with GMLID
  {
    WithQuietHandler q;
    const char* const opts[] = {"FORMAT=GML32", "NAMESPACE_DECL=YES", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    EXPECT_THAT(gml, HasSubstr("xmlns:gml=\"http://www.opengis.net/gml/3.2\""));
    CPLFree(gml);
  }
  {
    const char* const opts[] = {"FORMAT=GML3", "NAMESPACE_DECL=YES",
                                "GMLID=pt32", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    ASSERT_THAT(gml, NotNull());
    EXPECT_THAT(gml, HasSubstr("xmlns:gml=\"http://www.opengis.net/gml\""));
    EXPECT_THAT(gml, HasSubstr("gml:id=\"pt32\""));
    CPLFree(gml);
  }
}

TEST_F(Ogr2GmlGeometryTest, Gml3Geometries) {
  // Point (empty, 2D, 3D)
  {
    OGRPoint pt(1.0, 2.0);
    const char* const opts[] = {"FORMAT=GML3", "GMLID=p1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&pt),
                                    const_cast<char**>(opts));
    EXPECT_THAT(
        gml,
        HasSubstr(
            "<gml:Point gml:id=\"p1\"><gml:pos>1 2</gml:pos></gml:Point>"));
    CPLFree(gml);

    OGRPoint empty_pt;
    empty_pt.empty();
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_pt),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:Point gml:id=\"p1\">"));
    CPLFree(gml);
  }

  // LineString (normal, GML3_LINESTRING_ELEMENT=curve, empty)
  {
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    const char* const opts[] = {"FORMAT=GML3", "GMLID=ls1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&ls),
                                    const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:LineString gml:id=\"ls1\"><gml:posList>0 "
                               "0 1 1</gml:posList></gml:LineString>"));
    CPLFree(gml);

    const char* const opts_curve[] = {"FORMAT=GML3", "GMLID=ls2",
                                      "GML3_LINESTRING_ELEMENT=curve", nullptr};
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&ls),
                              const_cast<char**>(opts_curve));
    EXPECT_THAT(gml, HasSubstr("<gml:Curve "
                               "gml:id=\"ls2\"><gml:segments><gml:"
                               "LineStringSegment><gml:posList>0 0 1 "
                               "1</gml:posList></gml:LineStringSegment></"
                               "gml:segments></gml:Curve>"));
    CPLFree(gml);

    OGRLineString empty_ls;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_ls),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:LineString gml:id=\"ls1\">"));
    CPLFree(gml);
  }

  // LinearRing in GML3
  {
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(1, 0);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    const char* const opts[] = {"FORMAT=GML3", "GMLID=r1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&ring),
                                    const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:LinearRing><gml:posList>0 0 1 0 1 1 0 "
                               "0</gml:posList></gml:LinearRing>"));
    CPLFree(gml);
  }

  // CircularString: open arc vs closed circle (2D and 3D)
  {
    OGRCircularString cs_arc;
    cs_arc.addPoint(0, 0);
    cs_arc.addPoint(1, 1);
    cs_arc.addPoint(2, 0);
    const char* const opts[] = {"FORMAT=GML3", "GMLID=cs1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&cs_arc),
                                    const_cast<char**>(opts));
    EXPECT_THAT(
        gml,
        HasSubstr("<gml:Curve gml:id=\"cs1\"><gml:segments><gml:ArcString>"));
    CPLFree(gml);

    OGRCircularString cs_circle;
    cs_circle.addPoint(0, 0);
    cs_circle.addPoint(1, 1);
    cs_circle.addPoint(0, 0);
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&cs_circle),
                              const_cast<char**>(opts));
    EXPECT_THAT(
        gml, HasSubstr("<gml:Curve gml:id=\"cs1\"><gml:segments><gml:Circle>"));
    CPLFree(gml);

    OGRCircularString cs_circle_3d;
    cs_circle_3d.addPoint(0, 0, 1);
    cs_circle_3d.addPoint(1, 1, 2);
    cs_circle_3d.addPoint(0, 0, 1);
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&cs_circle_3d),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:Circle>"));
    CPLFree(gml);

    OGRCircularString empty_cs;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_cs),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:Curve gml:id=\"cs1\">"));
    CPLFree(gml);
  }

  // CompoundCurve
  {
    OGRCompoundCurve cc;
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    cs->addPoint(3, 1);
    cc.addCurveDirectly(ls.release());
    cc.addCurveDirectly(cs.release());

    const char* const opts[] = {"FORMAT=GML3", "GMLID=cc1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&cc),
                                    const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:CompositeCurve gml:id=\"cc1\">"));
    EXPECT_THAT(gml, HasSubstr("<gml:curveMember>"));
    CPLFree(gml);

    OGRCompoundCurve empty_cc;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_cc),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:CompositeCurve gml:id=\"cc1\">"));
    CPLFree(gml);
  }

  // Polygon and CurvePolygon
  {
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(10, 0);
    ring.addPoint(10, 10);
    ring.addPoint(0, 0);
    poly.addRing(&ring);

    const char* const opts[] = {"FORMAT=GML3", "GMLID=p1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&poly),
                                    const_cast<char**>(opts));
    EXPECT_THAT(
        gml,
        HasSubstr("<gml:Polygon gml:id=\"p1\"><gml:exterior><gml:LinearRing>"));
    CPLFree(gml);

    OGRCurvePolygon cp;
    auto cs_ring = std::make_unique<OGRCircularString>();
    cs_ring->addPoint(0, 0);
    cs_ring->addPoint(1, 1);
    cs_ring->addPoint(0, 0);
    cp.addRingDirectly(cs_ring.release());

    const char* const opts_cp[] = {"FORMAT=GML3", "GMLID=cp1", nullptr};
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&cp),
                              const_cast<char**>(opts_cp));
    EXPECT_THAT(gml, HasSubstr("<gml:Polygon "
                               "gml:id=\"cp1\"><gml:exterior><gml:Ring><gml:"
                               "curveMember><gml:Curve"));
    CPLFree(gml);

    OGRCurvePolygon empty_cp;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_cp),
                              const_cast<char**>(opts_cp));
    EXPECT_THAT(gml, HasSubstr("<gml:Polygon gml:id=\"cp1\">"));
    CPLFree(gml);
  }

  // Triangle
  {
    OGRTriangle tri(OGRPoint(0, 0), OGRPoint(1, 0), OGRPoint(0, 1));
    const char* const opts[] = {"FORMAT=GML3", "GMLID=tri1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&tri),
                                    const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:Triangle>"));
    CPLFree(gml);
  }

  // MultiPoint, MultiLineString, MultiCurve, MultiPolygon, MultiSurface
  {
    const char* const opts[] = {"FORMAT=GML3", "GMLID=m1", nullptr};

    OGRMultiPoint mp;
    OGRPoint p(1, 2);
    mp.addGeometry(&p);
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&mp),
                                    const_cast<char**>(opts));
    EXPECT_THAT(gml,
                HasSubstr("<gml:MultiPoint gml:id=\"m1\"><gml:pointMember>"));
    CPLFree(gml);

    OGRMultiLineString mls;
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(1, 1);
    mls.addGeometry(&ls);
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&mls),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml,
                HasSubstr("<gml:MultiCurve gml:id=\"m1\"><gml:curveMember>"));
    CPLFree(gml);

    OGRMultiCurve mc;
    mc.addGeometry(&ls);
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&mc),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml,
                HasSubstr("<gml:MultiCurve gml:id=\"m1\"><gml:curveMember>"));
    CPLFree(gml);

    OGRMultiCurve empty_mc;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_mc),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiCurve gml:id=\"m1\">"));
    CPLFree(gml);

    OGRMultiPolygon mpoly;
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(1, 0);
    ring.addPoint(1, 1);
    ring.addPoint(0, 0);
    poly.addRing(&ring);
    mpoly.addGeometry(&poly);
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&mpoly),
                              const_cast<char**>(opts));
    EXPECT_THAT(
        gml, HasSubstr("<gml:MultiSurface gml:id=\"m1\"><gml:surfaceMember>"));
    CPLFree(gml);

    OGRMultiSurface ms;
    ms.addGeometry(&poly);
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&ms),
                              const_cast<char**>(opts));
    EXPECT_THAT(
        gml, HasSubstr("<gml:MultiSurface gml:id=\"m1\"><gml:surfaceMember>"));
    CPLFree(gml);

    OGRMultiSurface empty_ms;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_ms),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:MultiSurface gml:id=\"m1\">"));
    CPLFree(gml);
  }

  // PolyhedralSurface and TriangulatedSurface
  {
    OGRPolyhedralSurface ps;
    auto poly = std::make_unique<OGRPolygon>();
    OGRLinearRing ring;
    ring.addPoint(0, 0, 0);
    ring.addPoint(1, 0, 0);
    ring.addPoint(1, 1, 0);
    ring.addPoint(0, 0, 0);
    poly->addRing(&ring);
    ps.addGeometryDirectly(poly.release());

    const char* const opts[] = {"FORMAT=GML3", "GMLID=ps1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&ps),
                                    const_cast<char**>(opts));
    EXPECT_THAT(
        gml, HasSubstr(
                 "<gml:PolyhedralSurface gml:id=\"ps1\"><gml:polygonPatches>"));
    CPLFree(gml);

    OGRPolyhedralSurface empty_ps;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_ps),
                              const_cast<char**>(opts));
    EXPECT_THAT(gml, HasSubstr("<gml:PolyhedralSurface gml:id=\"ps1\">"));
    CPLFree(gml);

    OGRTriangulatedSurface tin;
    auto tri = std::make_unique<OGRTriangle>(
        OGRPoint(0, 0, 0), OGRPoint(1, 0, 0), OGRPoint(0, 1, 0));
    tin.addGeometryDirectly(tri.release());

    const char* const opts_tin[] = {"FORMAT=GML3", "GMLID=tin1", nullptr};
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&tin),
                              const_cast<char**>(opts_tin));
    EXPECT_THAT(
        gml,
        HasSubstr("<gml:TriangulatedSurface gml:id=\"tin1\"><gml:patches>"));
    CPLFree(gml);

    OGRTriangulatedSurface empty_tin;
    gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&empty_tin),
                              const_cast<char**>(opts_tin));
    EXPECT_THAT(gml, HasSubstr("<gml:TriangulatedSurface gml:id=\"tin1\">"));
    CPLFree(gml);
  }

  // GeometryCollection in GML3
  {
    OGRGeometryCollection gc;
    OGRPoint p(1, 2);
    gc.addGeometry(&p);
    const char* const opts[] = {"FORMAT=GML3", "GMLID=gc1", nullptr};
    char* gml = OGR_G_ExportToGMLEx(OGRGeometry::ToHandle(&gc),
                                    const_cast<char**>(opts));
    EXPECT_THAT(
        gml,
        HasSubstr("<gml:MultiGeometry gml:id=\"gc1\"><gml:geometryMember>"));
    CPLFree(gml);
  }
}

}  // namespace
}  // namespace autotest2
