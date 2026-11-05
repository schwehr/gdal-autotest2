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
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(Ogr2KmlGeometryTest, NullGeometry) {
  char* kml = OGR_G_ExportToKML(nullptr, nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ("", kml);
  CPLFree(kml);
}

TEST(Ogr2KmlGeometryTest, CoordinateRangeEdgeCases) {
  // Test latitude clamping: near 90 and beyond 90
  {
    WithQuietHandler quiet_handler;
    OGRPoint pt(0.0, 90.000000001);
    char* kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("0,90"));
    CPLFree(kml);

    OGRPoint pt_neg(0.0, -90.000000001);
    kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_neg), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("0,-90"));
    CPLFree(kml);

    OGRPoint pt_large(0.0, 95.0);
    kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_large), nullptr);
    ASSERT_NE(nullptr, kml);
    CPLFree(kml);
  }

  // Test longitude wrapping and clamping
  {
    WithQuietHandler quiet_handler;
    OGRPoint pt_wrap(180.000000001, 0.0);
    char* kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_wrap), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("180,0"));
    CPLFree(kml);

    OGRPoint pt_wrap_neg(-180.000000001, 0.0);
    kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_wrap_neg), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("-180,0"));
    CPLFree(kml);

    OGRPoint pt_wrap_pos(190.0, 0.0);
    kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_wrap_pos), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("-170,0"));
    CPLFree(kml);

    OGRPoint pt_wrap_neg_large(-190.0, 0.0);
    kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_wrap_neg_large), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("170,0"));
    CPLFree(kml);

    OGRPoint pt_unreasonable(1e7, 0.0);
    kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt_unreasonable), nullptr);
    ASSERT_NE(nullptr, kml);
    EXPECT_THAT(kml, HasSubstr("0,0"));
    CPLFree(kml);
  }
}

class ZeroDimPoint : public OGRPoint {
 public:
  int getCoordinateDimension() const override { return 0; }
};

TEST(Ogr2KmlGeometryTest, Points2DAnd3D) {
  // Empty point with 0 coordinate dimension
  ZeroDimPoint zero_pt;
  char* kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&zero_pt), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ("<Point/>", kml);
  CPLFree(kml);

  // Default empty point (has 2D coordinate dimension, x=0, y=0)
  OGRPoint empty_pt;
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&empty_pt), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ("<Point><coordinates>0,0</coordinates></Point>", kml);
  CPLFree(kml);

  // 2D Point
  OGRPoint pt2d(1.5, 2.5);
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt2d), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ("<Point><coordinates>1.5,2.5</coordinates></Point>", kml);
  CPLFree(kml);

  // 3D Point without altitudeMode
  OGRPoint pt3d(1.0, 2.0, 3.0);
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt3d), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ("<Point><coordinates>1,2,3</coordinates></Point>", kml);
  CPLFree(kml);

  // 3D Point with altitudeMode
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt3d), "clampToGround");
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ(
      "<Point><altitudeMode>clampToGround</altitudeMode><coordinates>1,2,3</"
      "coordinates></Point>",
      kml);
  CPLFree(kml);

  // Very long altitudeMode (ignored / set to empty)
  std::string long_mode(128, 'a');
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&pt3d), long_mode.c_str());
  ASSERT_NE(nullptr, kml);
  EXPECT_STREQ("<Point><coordinates>1,2,3</coordinates></Point>", kml);
  CPLFree(kml);
}

TEST(Ogr2KmlGeometryTest, LineStringsAndRings) {
  // LineString 2D
  OGRLineString ls;
  ls.addPoint(0, 0);
  ls.addPoint(1, 1);
  char* kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&ls), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(
      kml,
      HasSubstr("<LineString><coordinates>0,0 1,1</coordinates></LineString>"));
  CPLFree(kml);

  // LineString 3D with altitudeMode
  OGRLineString ls3d;
  ls3d.addPoint(0, 0, 10);
  ls3d.addPoint(1, 1, 20);
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&ls3d), "relativeToGround");
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml, HasSubstr("<altitudeMode>relativeToGround</altitudeMode>"));
  EXPECT_THAT(kml, HasSubstr("<coordinates>0,0,10 1,1,20</coordinates>"));
  CPLFree(kml);

  // LinearRing
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(1, 0);
  ring.addPoint(1, 1);
  ring.addPoint(0, 0);
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&ring), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml, HasSubstr("<LinearRing><coordinates>0,0 1,0 1,1 "
                             "0,0</coordinates></LinearRing>"));
  CPLFree(kml);
}

TEST(Ogr2KmlGeometryTest, PolygonsAndCollections) {
  // Polygon with outer and inner rings
  OGRPolygon poly;
  auto outer = std::make_unique<OGRLinearRing>();
  outer->addPoint(0, 0);
  outer->addPoint(10, 0);
  outer->addPoint(10, 10);
  outer->addPoint(0, 0);
  poly.addRingDirectly(outer.release());

  auto inner = std::make_unique<OGRLinearRing>();
  inner->addPoint(2, 2);
  inner->addPoint(4, 2);
  inner->addPoint(4, 4);
  inner->addPoint(2, 2);
  poly.addRingDirectly(inner.release());

  char* kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&poly), "clampToGround");
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml,
              HasSubstr("<Polygon><altitudeMode>clampToGround</altitudeMode>"));
  EXPECT_THAT(kml, HasSubstr("<outerBoundaryIs><LinearRing>"));
  EXPECT_THAT(kml, HasSubstr("<innerBoundaryIs><LinearRing>"));
  CPLFree(kml);

  // MultiPoint
  OGRMultiPoint mp;
  auto pt = std::make_unique<OGRPoint>(1, 2);
  mp.addGeometry(pt.get());
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&mp), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml, HasSubstr("<MultiGeometry><Point>"));
  CPLFree(kml);

  // MultiLineString
  OGRMultiLineString mls;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mls.addGeometry(ls.get());
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&mls), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml, HasSubstr("<MultiGeometry><LineString>"));
  CPLFree(kml);

  // MultiPolygon
  OGRMultiPolygon mpoly;
  auto p = std::make_unique<OGRPolygon>();
  auto r = std::make_unique<OGRLinearRing>();
  r->addPoint(0, 0);
  r->addPoint(1, 0);
  r->addPoint(1, 1);
  r->addPoint(0, 0);
  p->addRingDirectly(r.release());
  mpoly.addGeometry(p.get());
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&mpoly), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml, HasSubstr("<MultiGeometry><Polygon>"));
  CPLFree(kml);

  // GeometryCollection
  OGRGeometryCollection gc;
  gc.addGeometry(pt.get());
  gc.addGeometry(ls.get());
  kml = OGR_G_ExportToKML(OGRGeometry::ToHandle(&gc), nullptr);
  ASSERT_NE(nullptr, kml);
  EXPECT_THAT(kml, HasSubstr("<MultiGeometry><Point>"));
  EXPECT_THAT(kml, HasSubstr("<LineString>"));
  CPLFree(kml);
}

TEST(Ogr2KmlGeometryTest, UnsupportedGeometries) {
  // Directly unsupported type
  OGRCircularString cs;
  cs.addPoint(0, 0);
  cs.addPoint(1, 1);
  cs.addPoint(2, 0);
  EXPECT_EQ(nullptr, OGR_G_ExportToKML(OGRGeometry::ToHandle(&cs), nullptr));

  // Unsupported type inside GeometryCollection
  OGRGeometryCollection gc;
  gc.addGeometry(&cs);
  EXPECT_EQ(nullptr, OGR_G_ExportToKML(OGRGeometry::ToHandle(&gc), nullptr));
}

}  // namespace
}  // namespace autotest2
