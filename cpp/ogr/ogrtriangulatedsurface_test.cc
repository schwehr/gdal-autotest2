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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestTriangulatedSurface : public OGRTriangulatedSurface {
 public:
  using OGRTriangulatedSurface::GetCasterToMultiPolygon;
  using OGRTriangulatedSurface::getSubGeometryName;
  using OGRTriangulatedSurface::getSubGeometryType;
  using OGRTriangulatedSurface::isCompatibleSubType;
};

std::unique_ptr<OGRTriangle> CreateTestTriangle() {
  OGRPoint p1(0, 0);
  OGRPoint p2(1, 0);
  OGRPoint p3(0, 1);
  return std::make_unique<OGRTriangle>(p1, p2, p3);
}

TEST(OgrTriangulatedSurfaceTest, BasicProperties) {
  TestTriangulatedSurface ts;
  EXPECT_STREQ("TIN", ts.getGeometryName());
  EXPECT_EQ(wkbTIN, ts.getGeometryType());

  ts.set3D(TRUE);
  EXPECT_EQ(wkbTINZ, ts.getGeometryType());

  ts.setMeasured(TRUE);
  EXPECT_EQ(wkbTINZM, ts.getGeometryType());

  ts.set3D(FALSE);
  EXPECT_EQ(wkbTINM, ts.getGeometryType());

  EXPECT_TRUE(ts.isCompatibleSubType(wkbTriangle));
  EXPECT_TRUE(ts.isCompatibleSubType(wkbTriangleZ));
  EXPECT_FALSE(ts.isCompatibleSubType(wkbPolygon));

  EXPECT_STREQ("TRIANGLE", ts.getSubGeometryName());
  EXPECT_EQ(wkbTriangle, ts.getSubGeometryType());
  EXPECT_NE(nullptr, ts.GetCasterToMultiPolygon());
}

TEST(OgrTriangulatedSurfaceTest, CopyAndClone) {
  OGRTriangulatedSurface ts;
  auto tri = CreateTestTriangle();
  ts.addGeometry(tri.get());

  // Copy constructor
  OGRTriangulatedSurface ts_copy(ts);
  EXPECT_EQ(1, ts_copy.getNumGeometries());

  // Assignment operator
  OGRTriangulatedSurface ts_assigned;
  ts_assigned = ts;
  EXPECT_EQ(1, ts_assigned.getNumGeometries());

  // Self assignment
  const auto* ptr = &ts_assigned;
  ts_assigned = *ptr;
  EXPECT_EQ(1, ts_assigned.getNumGeometries());

  // Clone
  std::unique_ptr<OGRTriangulatedSurface> ts_cloned(ts.clone());
  ASSERT_NE(nullptr, ts_cloned);
  EXPECT_EQ(1, ts_cloned->getNumGeometries());
}

TEST(OgrTriangulatedSurfaceTest, AddGeometry) {
  OGRTriangulatedSurface ts;

  // Add triangle directly
  auto tri = CreateTestTriangle();
  EXPECT_EQ(OGRERR_NONE, ts.addGeometry(tri.get()));
  EXPECT_EQ(1, ts.getNumGeometries());

  // Add valid triangle-shaped polygon (3 vertices + closing vertex = 4 points)
  OGRPolygon valid_poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(1, 0);
  ring.addPoint(0, 1);
  ring.addPoint(0, 0);
  valid_poly.addRing(&ring);
  EXPECT_EQ(OGRERR_NONE, ts.addGeometry(&valid_poly));
  EXPECT_EQ(2, ts.getNumGeometries());

  // Add invalid polygon (square with 5 points)
  OGRPolygon invalid_poly;
  OGRLinearRing square_ring;
  square_ring.addPoint(0, 0);
  square_ring.addPoint(1, 0);
  square_ring.addPoint(1, 1);
  square_ring.addPoint(0, 1);
  square_ring.addPoint(0, 0);
  invalid_poly.addRing(&square_ring);
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE, ts.addGeometry(&invalid_poly));
  EXPECT_EQ(2, ts.getNumGeometries());

  // Add incompatible non-polygon/non-triangle geometry (e.g. Point)
  OGRPoint pt(1, 2);
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE, ts.addGeometry(&pt));
  EXPECT_EQ(2, ts.getNumGeometries());
}

TEST(OgrTriangulatedSurfaceTest, CastToMultiPolygonAndPolyhedralSurface) {
  // CastToMultiPolygon
  {
    auto* ts = new OGRTriangulatedSurface();
    auto tri = CreateTestTriangle();
    ts->addGeometry(tri.get());

    OGRMultiPolygon* mp = OGRPolyhedralSurface::CastToMultiPolygon(ts);
    ASSERT_NE(nullptr, mp);
    EXPECT_STREQ("MULTIPOLYGON", mp->getGeometryName());
    EXPECT_EQ(1, mp->getNumGeometries());
    delete mp;
  }

  // CastToPolyhedralSurface
  {
    auto* ts = new OGRTriangulatedSurface();
    auto tri = CreateTestTriangle();
    ts->addGeometry(tri.get());

    OGRPolyhedralSurface* ps =
        OGRTriangulatedSurface::CastToPolyhedralSurface(ts);
    ASSERT_NE(nullptr, ps);
    EXPECT_STREQ("POLYHEDRALSURFACE", ps->getGeometryName());
    EXPECT_EQ(1, ps->getNumGeometries());
    delete ps;
  }
}

}  // namespace
}  // namespace autotest2
