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

#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_geometry.h"

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(MitabGeometryTest, OGRPointInRing) {
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(10, 0);
  ring.addPoint(10, 10);
  ring.addPoint(0, 10);
  ring.addPoint(0, 0);

  OGRPoint inside(5, 5);
  EXPECT_EQ(TRUE, OGRPointInRing(&inside, &ring));

  OGRPoint outside(15, 15);
  EXPECT_EQ(FALSE, OGRPointInRing(&outside, &ring));

  OGRPoint outside_x(-5, 5);
  EXPECT_EQ(FALSE, OGRPointInRing(&outside_x, &ring));
}

TEST(MitabGeometryTest, OGRIntersectPointPolygon) {
  OGRPolygon poly;
  OGRLinearRing ext;
  ext.addPoint(0, 0);
  ext.addPoint(10, 0);
  ext.addPoint(10, 10);
  ext.addPoint(0, 10);
  ext.addPoint(0, 0);
  poly.addRing(&ext);

  OGRLinearRing hole;
  hole.addPoint(3, 3);
  hole.addPoint(7, 3);
  hole.addPoint(7, 7);
  hole.addPoint(3, 7);
  hole.addPoint(3, 3);
  poly.addRing(&hole);

  // Inside exterior ring, outside hole
  OGRPoint pt_in_shell(1, 1);
  EXPECT_EQ(TRUE, OGRIntersectPointPolygon(&pt_in_shell, &poly));

  // Inside hole
  OGRPoint pt_in_hole(5, 5);
  EXPECT_EQ(FALSE, OGRIntersectPointPolygon(&pt_in_hole, &poly));

  // Outside polygon completely
  OGRPoint pt_outside(20, 20);
  EXPECT_EQ(FALSE, OGRIntersectPointPolygon(&pt_outside, &poly));
}

TEST(MitabGeometryTest, OGRPolygonLabelPointNullOrEmpty) {
  OGRPoint pt;
  EXPECT_EQ(OGRERR_FAILURE, OGRPolygonLabelPoint(nullptr, &pt));

  OGRPolygon empty_poly;
  OGRLinearRing empty_ring;
  empty_poly.addRing(&empty_ring);
  EXPECT_EQ(OGRERR_FAILURE, OGRPolygonLabelPoint(&empty_poly, &pt));
}

TEST(MitabGeometryTest, OGRPolygonLabelPointConvexFastPath) {
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(10, 0);
  ring.addPoint(10, 10);
  ring.addPoint(0, 10);
  ring.addPoint(0, 0);
  poly.addRing(&ring);

  OGRPoint label_pt;
  EXPECT_EQ(OGRERR_NONE, OGRPolygonLabelPoint(&poly, &label_pt));
  EXPECT_DOUBLE_EQ(5.0, label_pt.getX());
  EXPECT_DOUBLE_EQ(5.0, label_pt.getY());
}

TEST(MitabGeometryTest, OGRPolygonLabelPointScanlineWithHole) {
  // Polygon with hole centered at MBR center, forcing scanline calculation
  OGRPolygon poly;
  OGRLinearRing ext;
  ext.addPoint(0, 0);
  ext.addPoint(10, 0);
  ext.addPoint(10, 10);
  ext.addPoint(0, 10);
  ext.addPoint(0, 0);
  poly.addRing(&ext);

  OGRLinearRing hole;
  hole.addPoint(2, 2);
  hole.addPoint(8, 2);
  hole.addPoint(8, 8);
  hole.addPoint(2, 8);
  hole.addPoint(2, 2);
  poly.addRing(&hole);

  OGRPoint label_pt;
  EXPECT_EQ(OGRERR_NONE, OGRPolygonLabelPoint(&poly, &label_pt));
  // Label point should be inside the shell
  EXPECT_EQ(TRUE, OGRIntersectPointPolygon(&label_pt, &poly));
}

TEST(MitabGeometryTest, OGRPolygonLabelPointDegenerate) {
  // Flat collinear horizontal polygon where miny == maxy
  OGRPolygon flat_poly;
  OGRLinearRing ring;
  ring.addPoint(0, 5);
  ring.addPoint(10, 5);
  ring.addPoint(5, 5);
  ring.addPoint(0, 5);
  flat_poly.addRing(&ring);

  OGRPoint label_pt;
  EXPECT_EQ(OGRERR_FAILURE, OGRPolygonLabelPoint(&flat_poly, &label_pt));
}

TEST(MitabGeometryTest, OGRPolylineCenterPoint) {
  OGRPoint pt;
  EXPECT_EQ(OGRERR_FAILURE, OGRPolylineCenterPoint(nullptr, &pt));

  OGRLineString line_empty;
  EXPECT_EQ(OGRERR_FAILURE, OGRPolylineCenterPoint(&line_empty, &pt));

  OGRLineString line_one;
  line_one.addPoint(0, 0);
  EXPECT_EQ(OGRERR_FAILURE, OGRPolylineCenterPoint(&line_one, &pt));

  // Odd number of points: 3 points
  OGRLineString line_odd;
  line_odd.addPoint(0, 0);
  line_odd.addPoint(5, 10);
  line_odd.addPoint(20, 20);
  EXPECT_EQ(OGRERR_NONE, OGRPolylineCenterPoint(&line_odd, &pt));
  EXPECT_DOUBLE_EQ(5.0, pt.getX());
  EXPECT_DOUBLE_EQ(10.0, pt.getY());

  // Even number of points: 4 points
  OGRLineString line_even;
  line_even.addPoint(0, 0);
  line_even.addPoint(2, 4);
  line_even.addPoint(6, 8);
  line_even.addPoint(10, 12);
  EXPECT_EQ(OGRERR_NONE, OGRPolylineCenterPoint(&line_even, &pt));
  EXPECT_DOUBLE_EQ(4.0, pt.getX());  // (2 + 6) / 2
  EXPECT_DOUBLE_EQ(6.0, pt.getY());  // (4 + 8) / 2
}

TEST(MitabGeometryTest, OGRPolylineLabelPoint) {
  OGRPoint pt;
  EXPECT_EQ(OGRERR_FAILURE, OGRPolylineLabelPoint(nullptr, &pt));

  OGRLineString line_empty;
  EXPECT_EQ(OGRERR_FAILURE, OGRPolylineLabelPoint(&line_empty, &pt));

  OGRLineString line_one;
  line_one.addPoint(0, 0);
  EXPECT_EQ(OGRERR_FAILURE, OGRPolylineLabelPoint(&line_one, &pt));

  // Segment 1: (0,0)-(2,0), length 2
  // Segment 2: (2,0)-(12,0), length 10 (longest)
  // Segment 3: (12,0)-(15,0), length 3
  OGRLineString line;
  line.addPoint(0, 0);
  line.addPoint(2, 0);
  line.addPoint(12, 0);
  line.addPoint(15, 0);
  EXPECT_EQ(OGRERR_NONE, OGRPolylineLabelPoint(&line, &pt));
  EXPECT_DOUBLE_EQ(7.0, pt.getX());  // Center of segment 2: (2 + 12) / 2
  EXPECT_DOUBLE_EQ(0.0, pt.getY());
}

}  // namespace
}  // namespace autotest2
