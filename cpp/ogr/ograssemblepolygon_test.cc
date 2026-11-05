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
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(OgrAssemblePolygonTest, NullAndInvalidInputs) {
  OGRErr err = OGRERR_FAILURE;
  EXPECT_EQ(nullptr,
            OGRBuildPolygonFromEdges(nullptr, FALSE, FALSE, 0.0, &err));
  EXPECT_EQ(OGRERR_NONE, err);

  EXPECT_EQ(nullptr,
            OGRBuildPolygonFromEdges(nullptr, FALSE, FALSE, 0.0, nullptr));

  WithQuietHandler quiet_handler;

  // Passing non-collection geometry
  OGRPoint pt(1, 2);
  EXPECT_EQ(nullptr, OGRBuildPolygonFromEdges(OGRGeometry::ToHandle(&pt), FALSE,
                                              FALSE, 0.0, &err));
  EXPECT_EQ(OGRERR_FAILURE, err);

  // Passing GeometryCollection containing non-linestring
  OGRGeometryCollection gc;
  gc.addGeometry(&pt);
  EXPECT_EQ(nullptr, OGRBuildPolygonFromEdges(OGRGeometry::ToHandle(&gc), FALSE,
                                              FALSE, 0.0, &err));
  EXPECT_EQ(OGRERR_FAILURE, err);
}

TEST(OgrAssemblePolygonTest, ValidExactClosedPolygon) {
  OGRMultiLineString mls;

  // Edge 1: (0, 0) -> (10, 0)
  auto ls1 = std::make_unique<OGRLineString>();
  ls1->addPoint(0, 0);
  ls1->addPoint(10, 0);
  mls.addGeometry(ls1.get());

  // Edge 2 (reversed): (10, 10) -> (10, 0)
  auto ls2 = std::make_unique<OGRLineString>();
  ls2->addPoint(10, 10);
  ls2->addPoint(10, 0);
  mls.addGeometry(ls2.get());

  // Edge 3: (10, 10) -> (0, 10)
  auto ls3 = std::make_unique<OGRLineString>();
  ls3->addPoint(10, 10);
  ls3->addPoint(0, 10);
  mls.addGeometry(ls3.get());

  // Edge 4 (reversed): (0, 0) -> (0, 10)
  auto ls4 = std::make_unique<OGRLineString>();
  ls4->addPoint(0, 0);
  ls4->addPoint(0, 10);
  mls.addGeometry(ls4.get());

  // Also add a degenerate 1-point linestring (should be ignored)
  auto ls_degenerate = std::make_unique<OGRLineString>();
  ls_degenerate->addPoint(5, 5);
  mls.addGeometry(ls_degenerate.get());

  OGRErr err = OGRERR_FAILURE;
  std::unique_ptr<OGRGeometry> poly_geom(
      OGRGeometry::FromHandle(OGRBuildPolygonFromEdges(
          OGRGeometry::ToHandle(&mls), FALSE, FALSE, 0.0, &err)));
  ASSERT_NE(nullptr, poly_geom);
  EXPECT_EQ(OGRERR_NONE, err);

  auto* poly = poly_geom->toPolygon();
  ASSERT_NE(nullptr, poly);
  EXPECT_FALSE(poly->IsEmpty());
  EXPECT_DOUBLE_EQ(100.0, poly->get_Area());
}

TEST(OgrAssemblePolygonTest, NearClosedWithAutoClose) {
  OGRGeometryCollection gc;

  // Edges with small gaps within tolerance 0.5
  auto ls1 = std::make_unique<OGRLineString>();
  ls1->addPoint(0, 0);
  ls1->addPoint(10, 0);
  gc.addGeometry(ls1.get());

  auto ls2 = std::make_unique<OGRLineString>();
  ls2->addPoint(10, 0.1);
  ls2->addPoint(10, 10);
  gc.addGeometry(ls2.get());

  auto ls3 = std::make_unique<OGRLineString>();
  ls3->addPoint(10, 10);
  ls3->addPoint(0, 10);
  gc.addGeometry(ls3.get());

  auto ls4 = std::make_unique<OGRLineString>();
  ls4->addPoint(0, 10);
  ls4->addPoint(0, 0.1);
  gc.addGeometry(ls4.get());

  OGRErr err = OGRERR_FAILURE;
  // bAutoClose = TRUE snaps the last point to the first
  std::unique_ptr<OGRGeometry> poly_geom(
      OGRGeometry::FromHandle(OGRBuildPolygonFromEdges(
          OGRGeometry::ToHandle(&gc), FALSE, TRUE, 0.5, &err)));
  ASSERT_NE(nullptr, poly_geom);
  EXPECT_EQ(OGRERR_NONE, err);

  auto* poly = poly_geom->toPolygon();
  ASSERT_NE(nullptr, poly);
  EXPECT_TRUE(poly->getExteriorRingCurve()->get_IsClosed());
}

TEST(OgrAssemblePolygonTest, UnclosedRing) {
  OGRMultiLineString mls;

  // 3 sides of a square, gap is 10 (much larger than tolerance 0.1)
  auto ls1 = std::make_unique<OGRLineString>();
  ls1->addPoint(0, 0);
  ls1->addPoint(10, 0);
  mls.addGeometry(ls1.get());

  auto ls2 = std::make_unique<OGRLineString>();
  ls2->addPoint(10, 0);
  ls2->addPoint(10, 10);
  mls.addGeometry(ls2.get());

  auto ls3 = std::make_unique<OGRLineString>();
  ls3->addPoint(10, 10);
  ls3->addPoint(0, 10);
  mls.addGeometry(ls3.get());

  WithQuietHandler quiet_handler;

  // bAutoClose = FALSE
  OGRErr err = OGRERR_NONE;
  OGRGeometryH hPoly = OGRBuildPolygonFromEdges(OGRGeometry::ToHandle(&mls),
                                                FALSE, FALSE, 0.1, &err);
  EXPECT_EQ(OGRERR_FAILURE, err);
  if (hPoly) OGR_G_DestroyGeometry(hPoly);

  // bAutoClose = TRUE
  err = OGRERR_NONE;
  hPoly = OGRBuildPolygonFromEdges(OGRGeometry::ToHandle(&mls), FALSE, TRUE,
                                   0.1, &err);
  EXPECT_EQ(OGRERR_FAILURE, err);
  if (hPoly) OGR_G_DestroyGeometry(hPoly);
}

}  // namespace
}  // namespace autotest2
