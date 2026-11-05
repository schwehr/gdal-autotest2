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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestMultiSurface : public OGRMultiSurface {
 public:
  using OGRMultiSurface::isCompatibleSubType;
};

std::unique_ptr<OGRPolygon> CreateSquarePolygon() {
  auto poly = std::make_unique<OGRPolygon>();
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(10, 0);
  ring.addPoint(10, 10);
  ring.addPoint(0, 10);
  ring.addPoint(0, 0);
  poly->addRing(&ring);
  return poly;
}

TEST(OgrMultiSurfaceTest, BasicProperties) {
  TestMultiSurface ms;
  EXPECT_STREQ("MULTISURFACE", ms.getGeometryName());
  EXPECT_EQ(2, ms.getDimension());
  EXPECT_EQ(wkbMultiSurface, ms.getGeometryType());

  ms.set3D(TRUE);
  EXPECT_EQ(wkbMultiSurfaceZ, ms.getGeometryType());

  ms.setMeasured(TRUE);
  EXPECT_EQ(wkbMultiSurfaceZM, ms.getGeometryType());

  ms.set3D(FALSE);
  EXPECT_EQ(wkbMultiSurfaceM, ms.getGeometryType());

  EXPECT_TRUE(ms.isCompatibleSubType(wkbPolygon));
  EXPECT_TRUE(ms.isCompatibleSubType(wkbCurvePolygon));
  EXPECT_FALSE(ms.isCompatibleSubType(wkbLineString));
}

TEST(OgrMultiSurfaceTest, HasCurveGeometry) {
  OGRMultiSurface ms;
  auto poly = CreateSquarePolygon();
  ms.addGeometry(poly.get());

  // Without looking for non-linear, always returns TRUE
  EXPECT_TRUE(ms.hasCurveGeometry(FALSE));
  // With only linear geometries, looking for non-linear returns FALSE
  EXPECT_FALSE(ms.hasCurveGeometry(TRUE));

  auto cp = std::make_unique<OGRCurvePolygon>();
  auto cs = std::make_unique<OGRCircularString>();
  cs->addPoint(0, 0);
  cs->addPoint(1, 0);
  cs->addPoint(0, 0);
  cp->addRingDirectly(cs.release());
  ms.addGeometry(cp.get());
  EXPECT_TRUE(ms.hasCurveGeometry(TRUE));
}

TEST(OgrMultiSurfaceTest, CopyAndClone) {
  OGRMultiSurface ms;
  auto poly = CreateSquarePolygon();
  ms.addGeometry(poly.get());

  // Copy constructor
  OGRMultiSurface ms_copy(ms);
  EXPECT_EQ(1, ms_copy.getNumGeometries());

  // Assignment operator
  OGRMultiSurface ms_assigned;
  ms_assigned = ms;
  EXPECT_EQ(1, ms_assigned.getNumGeometries());

  // Self assignment
  const auto* ptr = &ms_assigned;
  ms_assigned = *ptr;
  EXPECT_EQ(1, ms_assigned.getNumGeometries());

  // Clone
  std::unique_ptr<OGRMultiSurface> ms_cloned(ms.clone());
  ASSERT_NE(nullptr, ms_cloned);
  EXPECT_EQ(1, ms_cloned->getNumGeometries());
}

TEST(OgrMultiSurfaceTest, ImportExportWkt) {
  // Empty
  {
    const char* pszWkt = "MULTISURFACE EMPTY";
    OGRMultiSurface ms;
    EXPECT_EQ(OGRERR_NONE, ms.importFromWkt(&pszWkt));
    EXPECT_TRUE(ms.IsEmpty());
  }

  // WKT with polygon list
  {
    const char* pszWkt = "MULTISURFACE (((0 0, 10 0, 10 10, 0 10, 0 0)))";
    OGRMultiSurface ms;
    EXPECT_EQ(OGRERR_NONE, ms.importFromWkt(&pszWkt));
    EXPECT_EQ(1, ms.getNumGeometries());

    OGRWktOptions opts;
    OGRErr err = OGRERR_NONE;
    std::string exported = ms.exportToWkt(opts, &err);
    EXPECT_EQ(OGRERR_NONE, err);
    EXPECT_FALSE(exported.empty());
  }

  // WKT with EMPTY sub-surface and POLYGON keyword
  {
    const char* pszWkt =
        "MULTISURFACE (EMPTY, POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))";
    OGRMultiSurface ms;
    EXPECT_EQ(OGRERR_NONE, ms.importFromWkt(&pszWkt));
    EXPECT_EQ(2, ms.getNumGeometries());
  }

  // WKT with Z and M
  {
    const char* pszWkt =
        "MULTISURFACE ZM (((0 0 1 2, 10 0 1 2, 10 10 1 2, 0 10 1 2, 0 0 1 2)))";
    OGRMultiSurface ms;
    EXPECT_EQ(OGRERR_NONE, ms.importFromWkt(&pszWkt));
    EXPECT_TRUE(ms.Is3D());
    EXPECT_TRUE(ms.IsMeasured());
  }

  // Error branches
  {
    WithQuietHandler quiet_handler;
    OGRMultiSurface ms;

    // Unexpected token
    const char* pszBadToken = "MULTISURFACE (POINT (0 0))";
    EXPECT_EQ(OGRERR_CORRUPT_DATA, ms.importFromWkt(&pszBadToken));

    // Missing closing bracket
    const char* pszMissingBracket =
        "MULTISURFACE (((0 0, 10 0, 10 10, 0 10, 0 0))";
    EXPECT_EQ(OGRERR_CORRUPT_DATA, ms.importFromWkt(&pszMissingBracket));
  }
}

TEST(OgrMultiSurfaceTest, PointOnSurface) {
  OGRMultiSurface ms;
  auto poly = CreateSquarePolygon();
  ms.addGeometry(poly.get());

  OGRPoint pt(0.0, 0.0);
  EXPECT_EQ(OGRERR_NONE, ms.PointOnSurface(&pt));
  EXPECT_GT(pt.getX(), 0.0);
  EXPECT_LT(pt.getX(), 10.0);
}

TEST(OgrMultiSurfaceTest, CastToMultiPolygon) {
  // Successful cast (contains only OGRPolygon)
  {
    auto* ms = new OGRMultiSurface();
    auto poly = CreateSquarePolygon();
    ms->addGeometry(poly.get());

    OGRMultiPolygon* mp = OGRMultiSurface::CastToMultiPolygon(ms);
    ASSERT_NE(nullptr, mp);
    EXPECT_STREQ("MULTIPOLYGON", mp->getGeometryName());
    EXPECT_EQ(1, mp->getNumGeometries());
    delete mp;
  }

  // Failed cast (contains non-linear OGRCurvePolygon)
  {
    auto* ms = new OGRMultiSurface();
    auto cp = std::make_unique<OGRCurvePolygon>();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 0);
    cs->addPoint(0, 0);
    cp->addRingDirectly(cs.release());
    ms->addGeometry(cp.get());

    OGRMultiPolygon* mp = OGRMultiSurface::CastToMultiPolygon(ms);
    EXPECT_EQ(nullptr, mp);
  }
}

}  // namespace
}  // namespace autotest2
