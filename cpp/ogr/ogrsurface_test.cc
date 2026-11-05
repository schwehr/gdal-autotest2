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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"

namespace autotest2 {
namespace {

TEST(OgrSurfaceTest, PointOnSurface) {
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(10, 0);
  ring.addPoint(10, 10);
  ring.addPoint(0, 10);
  ring.addPoint(0, 0);
  poly.addRing(&ring);

  // PointOnSurface requires a non-empty point to populate
  OGRPoint pt(0.0, 0.0);
  EXPECT_EQ(OGRERR_NONE, poly.PointOnSurface(&pt));
  EXPECT_GT(pt.getX(), 0.0);
  EXPECT_LT(pt.getX(), 10.0);
  EXPECT_GT(pt.getY(), 0.0);
  EXPECT_LT(pt.getY(), 10.0);
}

TEST(OgrSurfaceTest, CastToPolygon) {
  // Cast from OGRPolygon
  auto* poly = new OGRPolygon();
  OGRPolygon* cast_poly = OGRSurface::CastToPolygon(poly);
  ASSERT_NE(nullptr, cast_poly);
  delete cast_poly;

  // Cast from OGRTriangle
  OGRPoint p1(0, 0);
  OGRPoint p2(1, 0);
  OGRPoint p3(0, 1);
  auto* tri = new OGRTriangle(p1, p2, p3);
  cast_poly = OGRSurface::CastToPolygon(tri);
  ASSERT_NE(nullptr, cast_poly);
  EXPECT_STREQ("POLYGON", cast_poly->getGeometryName());
  delete cast_poly;
}

TEST(OgrSurfaceTest, CastToCurvePolygon) {
  // Cast from OGRPolygon
  auto* poly = new OGRPolygon();
  OGRCurvePolygon* cast_cp = OGRSurface::CastToCurvePolygon(poly);
  ASSERT_NE(nullptr, cast_cp);
  delete cast_cp;

  // Cast from OGRCurvePolygon
  auto* cp = new OGRCurvePolygon();
  cast_cp = OGRSurface::CastToCurvePolygon(cp);
  ASSERT_NE(nullptr, cast_cp);
  delete cast_cp;
}

}  // namespace
}  // namespace autotest2
