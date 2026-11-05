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

#include "third_party/gdal/ogr/ogr_api.h"

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(OgrApiTest, GEOSVersionAndNonLinearFlag) {
  int major = -1, minor = -1, patch = -1;
  OGRGetGEOSVersion(&major, &minor, &patch);
  // Also call with null pointers
  OGRGetGEOSVersion(nullptr, nullptr, nullptr);

  const int origFlag = OGRGetNonLinearGeometriesEnabledFlag();
  absl::Cleanup restoreFlag = [origFlag]() {
    OGRSetNonLinearGeometriesEnabledFlag(origFlag);
  };

  OGRSetNonLinearGeometriesEnabledFlag(FALSE);
  EXPECT_FALSE(OGRGetNonLinearGeometriesEnabledFlag());

  OGRSetNonLinearGeometriesEnabledFlag(TRUE);
  EXPECT_TRUE(OGRGetNonLinearGeometriesEnabledFlag());
}

TEST(OgrApiTest, GetAndSetPointCount) {
  WithQuietHandler quietHandler;

  EXPECT_EQ(0, OGR_G_GetPointCount(nullptr));

  OGRPoint pt(1.0, 2.0);
  EXPECT_EQ(1, OGR_G_GetPointCount(OGRGeometry::ToHandle(&pt)));

  OGRLineString ls;
  ls.addPoint(1.0, 2.0);
  ls.addPoint(3.0, 4.0);
  EXPECT_EQ(2, OGR_G_GetPointCount(OGRGeometry::ToHandle(&ls)));

  OGRPolygon poly;
  EXPECT_EQ(0, OGR_G_GetPointCount(OGRGeometry::ToHandle(&poly)));

  // OGR_G_SetPointCount
  OGR_G_SetPointCount(nullptr, 5);

  OGR_G_SetPointCount(OGRGeometry::ToHandle(&ls), 5);
  EXPECT_EQ(5, ls.getNumPoints());

  OGRCircularString cs;
  OGR_G_SetPointCount(OGRGeometry::ToHandle(&cs), 3);
  EXPECT_EQ(3, cs.getNumPoints());

  // Incompatible geometry
  OGR_G_SetPointCount(OGRGeometry::ToHandle(&pt), 2);
}

TEST(OgrApiTest, GetXYZM) {
  WithQuietHandler quietHandler;

  // Null geometry
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetX(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetY(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetZ(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetM(nullptr, 0));

  // OGRPoint
  OGRPoint pt(1.5, 2.5, 3.5, 4.5);
  OGRGeometryH hPt = OGRGeometry::ToHandle(&pt);
  EXPECT_DOUBLE_EQ(1.5, OGR_G_GetX(hPt, 0));
  EXPECT_DOUBLE_EQ(2.5, OGR_G_GetY(hPt, 0));
  EXPECT_DOUBLE_EQ(3.5, OGR_G_GetZ(hPt, 0));
  EXPECT_DOUBLE_EQ(4.5, OGR_G_GetM(hPt, 0));

  // Point index != 0
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetX(hPt, 1));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetY(hPt, 1));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetZ(hPt, 1));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetM(hPt, 1));

  // OGRLineString
  OGRLineString ls;
  ls.addPoint(10.0, 20.0, 30.0, 40.0);
  ls.addPoint(11.0, 21.0, 31.0, 41.0);
  OGRGeometryH hLs = OGRGeometry::ToHandle(&ls);
  EXPECT_DOUBLE_EQ(10.0, OGR_G_GetX(hLs, 0));
  EXPECT_DOUBLE_EQ(20.0, OGR_G_GetY(hLs, 0));
  EXPECT_DOUBLE_EQ(30.0, OGR_G_GetZ(hLs, 0));
  EXPECT_DOUBLE_EQ(40.0, OGR_G_GetM(hLs, 0));

  EXPECT_DOUBLE_EQ(11.0, OGR_G_GetX(hLs, 1));
  EXPECT_DOUBLE_EQ(21.0, OGR_G_GetY(hLs, 1));
  EXPECT_DOUBLE_EQ(31.0, OGR_G_GetZ(hLs, 1));
  EXPECT_DOUBLE_EQ(41.0, OGR_G_GetM(hLs, 1));

  // Out of bounds
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetX(hLs, -1));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetY(hLs, 5));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetZ(hLs, 5));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetM(hLs, 5));

  // Incompatible geometry
  OGRPolygon poly;
  OGRGeometryH hPoly = OGRGeometry::ToHandle(&poly);
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetX(hPoly, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetY(hPoly, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetZ(hPoly, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetM(hPoly, 0));
}

TEST(OgrApiTest, GetPointsAndGetPointsZM) {
  WithQuietHandler quietHandler;

  // Null geometry
  EXPECT_EQ(0, OGR_G_GetPoints(nullptr, nullptr, 0, nullptr, 0, nullptr, 0));
  EXPECT_EQ(0, OGR_G_GetPointsZM(nullptr, nullptr, 0, nullptr, 0, nullptr, 0,
                                 nullptr, 0));

  // Point geometry
  OGRPoint pt(1.0, 2.0, 3.0, 4.0);
  OGRGeometryH hPt = OGRGeometry::ToHandle(&pt);
  double x = 0, y = 0, z = 0, m = 0;
  EXPECT_EQ(1, OGR_G_GetPoints(hPt, &x, sizeof(double), &y, sizeof(double), &z,
                               sizeof(double)));
  EXPECT_DOUBLE_EQ(1.0, x);
  EXPECT_DOUBLE_EQ(2.0, y);
  EXPECT_DOUBLE_EQ(3.0, z);

  // Calling with null buffers
  EXPECT_EQ(1, OGR_G_GetPoints(hPt, nullptr, 0, nullptr, 0, nullptr, 0));

  EXPECT_EQ(1, OGR_G_GetPointsZM(hPt, &x, sizeof(double), &y, sizeof(double),
                                 &z, sizeof(double), &m, sizeof(double)));
  EXPECT_DOUBLE_EQ(4.0, m);
  EXPECT_EQ(1, OGR_G_GetPointsZM(hPt, nullptr, 0, nullptr, 0, nullptr, 0,
                                 nullptr, 0));

  // LineString geometry
  OGRLineString ls;
  ls.addPoint(10.0, 20.0, 30.0, 40.0);
  ls.addPoint(50.0, 60.0, 70.0, 80.0);
  OGRGeometryH hLs = OGRGeometry::ToHandle(&ls);

  double ax[2] = {0}, ay[2] = {0}, az[2] = {0}, am[2] = {0};
  EXPECT_EQ(2, OGR_G_GetPoints(hLs, ax, sizeof(double), ay, sizeof(double), az,
                               sizeof(double)));
  EXPECT_DOUBLE_EQ(10.0, ax[0]);
  EXPECT_DOUBLE_EQ(50.0, ax[1]);

  EXPECT_EQ(2, OGR_G_GetPointsZM(hLs, ax, sizeof(double), ay, sizeof(double),
                                 az, sizeof(double), am, sizeof(double)));
  EXPECT_DOUBLE_EQ(40.0, am[0]);
  EXPECT_DOUBLE_EQ(80.0, am[1]);

  // Incompatible geometry
  OGRPolygon poly;
  OGRGeometryH hPoly = OGRGeometry::ToHandle(&poly);
  EXPECT_EQ(-1, OGR_G_GetPoints(hPoly, ax, sizeof(double), ay, sizeof(double),
                                az, sizeof(double)));
  EXPECT_EQ(0, OGR_G_GetPointsZM(hPoly, ax, sizeof(double), ay, sizeof(double),
                                 az, sizeof(double), am, sizeof(double)));
}

TEST(OgrApiTest, GetPointAndGetPointZM) {
  WithQuietHandler quietHandler;

  double x = 0, y = 0, z = 0, m = 0;
  // Null geometry
  OGR_G_GetPoint(nullptr, 0, &x, &y, &z);
  OGR_G_GetPointZM(nullptr, 0, &x, &y, &z, &m);

  // Point geometry
  OGRPoint pt(1.0, 2.0, 3.0, 4.0);
  OGRGeometryH hPt = OGRGeometry::ToHandle(&pt);
  OGR_G_GetPoint(hPt, 0, &x, &y, &z);
  EXPECT_DOUBLE_EQ(1.0, x);
  EXPECT_DOUBLE_EQ(2.0, y);
  EXPECT_DOUBLE_EQ(3.0, z);

  // With null z
  OGR_G_GetPoint(hPt, 0, &x, &y, nullptr);
  EXPECT_DOUBLE_EQ(1.0, x);

  // Out of bounds for point
  OGR_G_GetPoint(hPt, 1, &x, &y, &z);

  OGR_G_GetPointZM(hPt, 0, &x, &y, &z, &m);
  EXPECT_DOUBLE_EQ(4.0, m);
  OGR_G_GetPointZM(hPt, 0, &x, &y, nullptr, nullptr);
  OGR_G_GetPointZM(hPt, 1, &x, &y, &z, &m);

  // LineString geometry
  OGRLineString ls;
  ls.addPoint(10.0, 20.0, 30.0, 40.0);
  OGRGeometryH hLs = OGRGeometry::ToHandle(&ls);

  OGR_G_GetPoint(hLs, 0, &x, &y, &z);
  EXPECT_DOUBLE_EQ(10.0, x);
  OGR_G_GetPoint(hLs, 0, &x, &y, nullptr);

  // Out of bounds for line string
  OGR_G_GetPoint(hLs, 5, &x, &y, &z);
  EXPECT_DOUBLE_EQ(0.0, x);
  OGR_G_GetPoint(hLs, -1, &x, &y, nullptr);

  OGR_G_GetPointZM(hLs, 0, &x, &y, &z, &m);
  EXPECT_DOUBLE_EQ(40.0, m);
  OGR_G_GetPointZM(hLs, 0, &x, &y, nullptr, nullptr);

  OGR_G_GetPointZM(hLs, 5, &x, &y, &z, &m);
  EXPECT_DOUBLE_EQ(0.0, m);
  OGR_G_GetPointZM(hLs, -1, &x, &y, nullptr, nullptr);

  // Incompatible geometry
  OGRPolygon poly;
  OGRGeometryH hPoly = OGRGeometry::ToHandle(&poly);
  OGR_G_GetPoint(hPoly, 0, &x, &y, &z);
  OGR_G_GetPointZM(hPoly, 0, &x, &y, &z, &m);
}

TEST(OgrApiTest, SetPointsAndSetPointsZM) {
  WithQuietHandler quietHandler;

  double ax[2] = {1.0, 2.0};
  double ay[2] = {3.0, 4.0};
  double az[2] = {5.0, 6.0};
  double am[2] = {7.0, 8.0};

  // Null geometry or null pointers
  OGR_G_SetPoints(nullptr, 2, ax, sizeof(double), ay, sizeof(double), az,
                  sizeof(double));
  OGR_G_SetPointsZM(nullptr, 2, ax, sizeof(double), ay, sizeof(double), az,
                    sizeof(double), am, sizeof(double));

  OGRPoint pt;
  OGRGeometryH hPt = OGRGeometry::ToHandle(&pt);
  OGR_G_SetPoints(hPt, 1, nullptr, sizeof(double), ay, sizeof(double), az,
                  sizeof(double));
  OGR_G_SetPoints(hPt, 1, ax, sizeof(double), nullptr, sizeof(double), az,
                  sizeof(double));
  OGR_G_SetPointsZM(hPt, 1, nullptr, sizeof(double), ay, sizeof(double), az,
                    sizeof(double), am, sizeof(double));
  OGR_G_SetPointsZM(hPt, 1, ax, sizeof(double), nullptr, sizeof(double), az,
                    sizeof(double), am, sizeof(double));

  // Point with and without Z
  OGR_G_SetPoints(hPt, 1, ax, sizeof(double), ay, sizeof(double), nullptr, 0);
  EXPECT_DOUBLE_EQ(1.0, pt.getX());
  EXPECT_DOUBLE_EQ(3.0, pt.getY());

  OGR_G_SetPoints(hPt, 1, ax, sizeof(double), ay, sizeof(double), az,
                  sizeof(double));
  EXPECT_DOUBLE_EQ(5.0, pt.getZ());

  // Point with SetPointsZM
  OGR_G_SetPointsZM(hPt, 1, ax, sizeof(double), ay, sizeof(double), az,
                    sizeof(double), am, sizeof(double));
  EXPECT_DOUBLE_EQ(7.0, pt.getM());

  // LineString SetPoints contiguous stride
  OGRLineString ls;
  OGRGeometryH hLs = OGRGeometry::ToHandle(&ls);

  // 2D contiguous
  OGR_G_SetPoints(hLs, 2, ax, sizeof(double), ay, sizeof(double), nullptr, 0);
  EXPECT_EQ(2, ls.getNumPoints());
  EXPECT_FALSE(ls.Is3D());

  // 3D contiguous
  OGR_G_SetPoints(hLs, 2, ax, sizeof(double), ay, sizeof(double), az,
                  sizeof(double));
  EXPECT_TRUE(ls.Is3D());
  EXPECT_DOUBLE_EQ(5.0, ls.getZ(0));

  // Non-contiguous stride (stride = 2 * sizeof(double))
  double interleaved2D[4] = {10.0, 20.0, 30.0, 40.0};
  OGR_G_SetPoints(hLs, 2, interleaved2D, 2 * sizeof(double), interleaved2D + 1,
                  2 * sizeof(double), nullptr, 0);
  EXPECT_DOUBLE_EQ(10.0, ls.getX(0));
  EXPECT_DOUBLE_EQ(20.0, ls.getY(0));

  double interleaved3D[6] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  OGR_G_SetPoints(hLs, 2, interleaved3D, 3 * sizeof(double), interleaved3D + 1,
                  3 * sizeof(double), interleaved3D + 2, 3 * sizeof(double));
  EXPECT_DOUBLE_EQ(30.0, ls.getZ(0));

  // SetPoints on incompatible geometry
  OGRPolygon poly;
  OGR_G_SetPoints(OGRGeometry::ToHandle(&poly), 2, ax, sizeof(double), ay,
                  sizeof(double), nullptr, 0);

  // LineString SetPointsZM contiguous strides
  // 1. !padfZ && !padfM
  OGR_G_SetPointsZM(hLs, 2, ax, sizeof(double), ay, sizeof(double), nullptr, 0,
                    nullptr, 0);
  EXPECT_FALSE(ls.Is3D());
  EXPECT_FALSE(ls.IsMeasured());

  // 2. pabyZ && !pabyM
  OGR_G_SetPointsZM(hLs, 2, ax, sizeof(double), ay, sizeof(double), az,
                    sizeof(double), nullptr, 0);
  EXPECT_TRUE(ls.Is3D());
  EXPECT_FALSE(ls.IsMeasured());

  // 3. !pabyZ && pabyM
  OGR_G_SetPointsZM(hLs, 2, ax, sizeof(double), ay, sizeof(double), nullptr, 0,
                    am, sizeof(double));
  EXPECT_TRUE(ls.IsMeasured());

  // 4. pabyZ && pabyM
  OGR_G_SetPointsZM(hLs, 2, ax, sizeof(double), ay, sizeof(double), az,
                    sizeof(double), am, sizeof(double));
  EXPECT_TRUE(ls.Is3D());
  EXPECT_TRUE(ls.IsMeasured());

  // Non-contiguous stride for SetPointsZM:
  // 1. !pabyM && !pabyZ
  OGR_G_SetPointsZM(hLs, 2, interleaved2D, 2 * sizeof(double),
                    interleaved2D + 1, 2 * sizeof(double), nullptr, 0, nullptr,
                    0);
  EXPECT_DOUBLE_EQ(10.0, ls.getX(0));

  // 2. !pabyM && pabyZ
  OGR_G_SetPointsZM(hLs, 2, interleaved3D, 3 * sizeof(double),
                    interleaved3D + 1, 3 * sizeof(double), interleaved3D + 2,
                    3 * sizeof(double), nullptr, 0);
  EXPECT_DOUBLE_EQ(30.0, ls.getZ(0));

  // 3. pabyM && !pabyZ
  OGR_G_SetPointsZM(hLs, 2, interleaved3D, 3 * sizeof(double),
                    interleaved3D + 1, 3 * sizeof(double), nullptr, 0,
                    interleaved3D + 2, 3 * sizeof(double));
  EXPECT_DOUBLE_EQ(30.0, ls.getM(0));

  // 4. pabyM && pabyZ
  double interleaved4D[8] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  OGR_G_SetPointsZM(hLs, 2, interleaved4D, 4 * sizeof(double),
                    interleaved4D + 1, 4 * sizeof(double), interleaved4D + 2,
                    4 * sizeof(double), interleaved4D + 3, 4 * sizeof(double));
  EXPECT_DOUBLE_EQ(4.0, ls.getM(0));

  // SetPointsZM on incompatible geometry
  OGR_G_SetPointsZM(OGRGeometry::ToHandle(&poly), 2, ax, sizeof(double), ay,
                    sizeof(double), nullptr, 0, nullptr, 0);
}

TEST(OgrApiTest, SetPointAndAddPointVariants) {
  WithQuietHandler quietHandler;

  // Null handles
  OGR_G_SetPoint(nullptr, 0, 1, 2, 3);
  OGR_G_SetPoint_2D(nullptr, 0, 1, 2);
  OGR_G_SetPointM(nullptr, 0, 1, 2, 4);
  OGR_G_SetPointZM(nullptr, 0, 1, 2, 3, 4);
  OGR_G_AddPoint(nullptr, 1, 2, 3);
  OGR_G_AddPoint_2D(nullptr, 1, 2);
  OGR_G_AddPointM(nullptr, 1, 2, 4);
  OGR_G_AddPointZM(nullptr, 1, 2, 3, 4);

  // Point geometry
  OGRPoint pt;
  OGRGeometryH hPt = OGRGeometry::ToHandle(&pt);

  OGR_G_SetPoint(hPt, 0, 10.0, 20.0, 30.0);
  EXPECT_DOUBLE_EQ(10.0, pt.getX());
  EXPECT_DOUBLE_EQ(30.0, pt.getZ());
  OGR_G_SetPoint(hPt, 1, 0, 0, 0);  // error

  OGR_G_SetPoint_2D(hPt, 0, 11.0, 21.0);
  EXPECT_DOUBLE_EQ(11.0, pt.getX());
  OGR_G_SetPoint_2D(hPt, 1, 0, 0);  // error

  OGR_G_SetPointM(hPt, 0, 12.0, 22.0, 42.0);
  EXPECT_DOUBLE_EQ(42.0, pt.getM());
  OGR_G_SetPointM(hPt, 1, 0, 0, 0);  // error

  OGR_G_SetPointZM(hPt, 0, 13.0, 23.0, 33.0, 43.0);
  EXPECT_DOUBLE_EQ(33.0, pt.getZ());
  EXPECT_DOUBLE_EQ(43.0, pt.getM());
  OGR_G_SetPointZM(hPt, 1, 0, 0, 0, 0);  // error

  OGR_G_AddPoint(hPt, 1.0, 2.0, 3.0);
  EXPECT_DOUBLE_EQ(1.0, pt.getX());
  OGR_G_AddPoint_2D(hPt, 4.0, 5.0);
  EXPECT_DOUBLE_EQ(4.0, pt.getX());
  OGR_G_AddPointM(hPt, 6.0, 7.0, 8.0);
  EXPECT_DOUBLE_EQ(8.0, pt.getM());
  OGR_G_AddPointZM(hPt, 9.0, 10.0, 11.0, 12.0);
  EXPECT_DOUBLE_EQ(12.0, pt.getM());

  // LineString geometry
  OGRLineString ls;
  OGRGeometryH hLs = OGRGeometry::ToHandle(&ls);

  OGR_G_AddPoint(hLs, 1.0, 2.0, 3.0);
  OGR_G_AddPoint_2D(hLs, 4.0, 5.0);
  OGR_G_AddPointM(hLs, 6.0, 7.0, 8.0);
  OGR_G_AddPointZM(hLs, 9.0, 10.0, 11.0, 12.0);
  EXPECT_EQ(4, ls.getNumPoints());

  OGR_G_SetPoint(hLs, 0, 100.0, 200.0, 300.0);
  EXPECT_DOUBLE_EQ(100.0, ls.getX(0));
  OGR_G_SetPoint(hLs, -1, 0, 0, 0);  // error

  OGR_G_SetPoint_2D(hLs, 1, 400.0, 500.0);
  EXPECT_DOUBLE_EQ(400.0, ls.getX(1));
  OGR_G_SetPoint_2D(hLs, -1, 0, 0);  // error

  OGR_G_SetPointM(hLs, 2, 600.0, 700.0, 800.0);
  EXPECT_DOUBLE_EQ(800.0, ls.getM(2));
  OGR_G_SetPointM(hLs, -1, 0, 0, 0);  // error

  OGR_G_SetPointZM(hLs, 3, 900.0, 1000.0, 1100.0, 1200.0);
  EXPECT_DOUBLE_EQ(1200.0, ls.getM(3));
  OGR_G_SetPointZM(hLs, -1, 0, 0, 0, 0);  // error

  // Incompatible geometry (OGRPolygon)
  OGRPolygon poly;
  OGRGeometryH hPoly = OGRGeometry::ToHandle(&poly);
  OGR_G_SetPoint(hPoly, 0, 1, 2, 3);
  OGR_G_SetPoint_2D(hPoly, 0, 1, 2);
  OGR_G_SetPointM(hPoly, 0, 1, 2, 4);
  OGR_G_SetPointZM(hPoly, 0, 1, 2, 3, 4);
  OGR_G_AddPoint(hPoly, 1, 2, 3);
  OGR_G_AddPoint_2D(hPoly, 1, 2);
  OGR_G_AddPointM(hPoly, 1, 2, 4);
  OGR_G_AddPointZM(hPoly, 1, 2, 3, 4);
}

TEST(OgrApiTest, GeometryContainerOperations) {
  WithQuietHandler quietHandler;

  // Null handles
  EXPECT_EQ(0, OGR_G_GetGeometryCount(nullptr));
  EXPECT_EQ(nullptr, OGR_G_GetGeometryRef(nullptr, 0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, OGR_G_AddGeometry(nullptr, nullptr));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_G_AddGeometryDirectly(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_RemoveGeometry(nullptr, 0, TRUE));

  // Polygon container
  OGRPolygon poly;
  OGRGeometryH hPoly = OGRGeometry::ToHandle(&poly);
  EXPECT_EQ(0, OGR_G_GetGeometryCount(hPoly));

  OGRLinearRing ring1;
  ring1.addPoint(0, 0);
  ring1.addPoint(10, 0);
  ring1.addPoint(10, 10);
  ring1.addPoint(0, 10);
  ring1.addPoint(0, 0);

  EXPECT_EQ(OGRERR_NONE,
            OGR_G_AddGeometry(hPoly, OGRGeometry::ToHandle(&ring1)));
  EXPECT_EQ(1, OGR_G_GetGeometryCount(hPoly));

  auto* ring2 = new OGRLinearRing();
  ring2->addPoint(2, 2);
  ring2->addPoint(8, 2);
  ring2->addPoint(8, 8);
  ring2->addPoint(2, 8);
  ring2->addPoint(2, 2);
  EXPECT_EQ(OGRERR_NONE,
            OGR_G_AddGeometryDirectly(hPoly, OGRGeometry::ToHandle(ring2)));
  EXPECT_EQ(2, OGR_G_GetGeometryCount(hPoly));

  EXPECT_NE(nullptr, OGR_G_GetGeometryRef(hPoly, 0));
  EXPECT_NE(nullptr, OGR_G_GetGeometryRef(hPoly, 1));

  // Incompatible subgeometry added to polygon
  OGRPoint invalidSubGeom(5, 5);
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGR_G_AddGeometry(hPoly, OGRGeometry::ToHandle(&invalidSubGeom)));
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGR_G_AddGeometryDirectly(
                hPoly, OGRGeometry::ToHandle(new OGRPoint(5, 5))));

  EXPECT_EQ(OGRERR_NONE, OGR_G_RemoveGeometry(hPoly, 1, TRUE));
  EXPECT_EQ(1, OGR_G_GetGeometryCount(hPoly));

  // CompoundCurve container
  OGRCompoundCurve cc;
  OGRGeometryH hCc = OGRGeometry::ToHandle(&cc);
  EXPECT_EQ(0, OGR_G_GetGeometryCount(hCc));

  OGRLineString ls1;
  ls1.addPoint(0, 0);
  ls1.addPoint(5, 5);
  EXPECT_EQ(OGRERR_NONE, OGR_G_AddGeometry(hCc, OGRGeometry::ToHandle(&ls1)));

  auto* ls2 = new OGRLineString();
  ls2->addPoint(5, 5);
  ls2->addPoint(10, 10);
  EXPECT_EQ(OGRERR_NONE,
            OGR_G_AddGeometryDirectly(hCc, OGRGeometry::ToHandle(ls2)));
  EXPECT_EQ(2, OGR_G_GetGeometryCount(hCc));
  EXPECT_NE(nullptr, OGR_G_GetGeometryRef(hCc, 0));

  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGR_G_AddGeometry(hCc, OGRGeometry::ToHandle(&invalidSubGeom)));
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGR_G_AddGeometryDirectly(
                hCc, OGRGeometry::ToHandle(new OGRPoint(5, 5))));

  // GeometryCollection container
  OGRGeometryCollection gc;
  OGRGeometryH hGc = OGRGeometry::ToHandle(&gc);
  EXPECT_EQ(OGRERR_NONE,
            OGR_G_AddGeometry(hGc, OGRGeometry::ToHandle(&invalidSubGeom)));
  EXPECT_EQ(OGRERR_NONE, OGR_G_AddGeometryDirectly(
                             hGc, OGRGeometry::ToHandle(new OGRPoint(1, 2))));
  EXPECT_EQ(2, OGR_G_GetGeometryCount(hGc));
  EXPECT_NE(nullptr, OGR_G_GetGeometryRef(hGc, 0));
  EXPECT_EQ(OGRERR_NONE, OGR_G_RemoveGeometry(hGc, 0, TRUE));
  EXPECT_EQ(1, OGR_G_GetGeometryCount(hGc));

  // PolyhedralSurface container
  OGRPolyhedralSurface ps;
  OGRGeometryH hPs = OGRGeometry::ToHandle(&ps);
  EXPECT_EQ(0, OGR_G_GetGeometryCount(hPs));

  OGRPolygon subPoly;
  auto* subRing = new OGRLinearRing();
  subRing->addPoint(0, 0);
  subRing->addPoint(1, 0);
  subRing->addPoint(1, 1);
  subRing->addPoint(0, 1);
  subRing->addPoint(0, 0);
  subPoly.addRingDirectly(subRing);

  EXPECT_EQ(OGRERR_NONE,
            OGR_G_AddGeometry(hPs, OGRGeometry::ToHandle(&subPoly)));

  auto* subPolyDirect = new OGRPolygon();
  auto* subRing2 = new OGRLinearRing();
  subRing2->addPoint(1, 0);
  subRing2->addPoint(2, 0);
  subRing2->addPoint(2, 1);
  subRing2->addPoint(1, 1);
  subRing2->addPoint(1, 0);
  subPolyDirect->addRingDirectly(subRing2);

  EXPECT_EQ(OGRERR_NONE, OGR_G_AddGeometryDirectly(
                             hPs, OGRGeometry::ToHandle(subPolyDirect)));
  EXPECT_EQ(2, OGR_G_GetGeometryCount(hPs));
  EXPECT_NE(nullptr, OGR_G_GetGeometryRef(hPs, 0));
  EXPECT_EQ(OGRERR_NONE, OGR_G_RemoveGeometry(hPs, 0, TRUE));
  EXPECT_EQ(1, OGR_G_GetGeometryCount(hPs));

  // Incompatible container (OGRPoint)
  OGRPoint pt;
  OGRGeometryH hPoint = OGRGeometry::ToHandle(&pt);
  EXPECT_EQ(0, OGR_G_GetGeometryCount(hPoint));
  EXPECT_EQ(nullptr, OGR_G_GetGeometryRef(hPoint, 0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGR_G_AddGeometry(hPoint, OGRGeometry::ToHandle(&pt)));
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            OGR_G_AddGeometryDirectly(
                hPoint, OGRGeometry::ToHandle(new OGRPoint(1, 2))));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_G_RemoveGeometry(hPoint, 0, TRUE));
}

TEST(OgrApiTest, LengthAreaAndClockwise) {
  WithQuietHandler quietHandler;

  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");

  // Null handles
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Length(nullptr));
  EXPECT_DOUBLE_EQ(-1.0, OGR_G_GeodesicLength(nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Area(nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetArea(nullptr));
  EXPECT_DOUBLE_EQ(-1.0, OGR_G_GeodesicArea(nullptr));
  EXPECT_FALSE(OGR_G_IsClockwise(nullptr));

  // Curve geometry (LineString)
  OGRLineString ls;
  ls.assignSpatialReference(&srs);
  ls.addPoint(0, 0);
  ls.addPoint(1, 0);
  OGRGeometryH hLs = OGRGeometry::ToHandle(&ls);

  EXPECT_GT(OGR_G_Length(hLs), 0.0);
  EXPECT_GT(OGR_G_GeodesicLength(hLs), 0.0);
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Area(hLs));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetArea(hLs));

  // Surface geometry (Polygon)
  OGRPolygon poly;
  poly.assignSpatialReference(&srs);
  auto* ring = new OGRLinearRing();
  ring->addPoint(0, 0);
  ring->addPoint(0, 1);
  ring->addPoint(1, 1);
  ring->addPoint(1, 0);
  ring->addPoint(0, 0);
  poly.addRingDirectly(ring);
  OGRGeometryH hPoly = OGRGeometry::ToHandle(&poly);

  EXPECT_GT(OGR_G_Length(hPoly), 0.0);
  EXPECT_GT(OGR_G_GeodesicLength(hPoly), 0.0);
  EXPECT_GT(OGR_G_Area(hPoly), 0.0);
  EXPECT_GT(OGR_G_GetArea(hPoly), 0.0);
  EXPECT_GT(OGR_G_GeodesicArea(hPoly), 0.0);

  // Closed curve (LinearRing) for Area and GeodesicArea and IsClockwise
  OGRLinearRing cwRing;
  cwRing.assignSpatialReference(&srs);
  cwRing.addPoint(0, 0);
  cwRing.addPoint(0, 1);
  cwRing.addPoint(1, 1);
  cwRing.addPoint(1, 0);
  cwRing.addPoint(0, 0);
  OGRGeometryH hRing = OGRGeometry::ToHandle(&cwRing);
  EXPECT_GT(OGR_G_Area(hRing), 0.0);
  EXPECT_GT(OGR_G_GeodesicArea(hRing), 0.0);
  EXPECT_TRUE(OGR_G_IsClockwise(hRing));

  // GeometryCollection
  OGRGeometryCollection gc;
  gc.assignSpatialReference(&srs);
  gc.addGeometry(&ls);
  gc.addGeometry(&poly);
  OGRGeometryH hGc = OGRGeometry::ToHandle(&gc);
  EXPECT_GT(OGR_G_Length(hGc), 0.0);
  EXPECT_GT(OGR_G_GeodesicLength(hGc), 0.0);
  EXPECT_GT(OGR_G_Area(hGc), 0.0);
  EXPECT_GT(OGR_G_GeodesicArea(hGc), 0.0);

  // Incompatible geometry (Point)
  OGRPoint pt(0, 0);
  OGRGeometryH hPt = OGRGeometry::ToHandle(&pt);
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Length(hPt));
  EXPECT_DOUBLE_EQ(-1.0, OGR_G_GeodesicLength(hPt));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Area(hPt));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetArea(hPt));
  EXPECT_DOUBLE_EQ(-1.0, OGR_G_GeodesicArea(hPt));
  EXPECT_FALSE(OGR_G_IsClockwise(hPt));
}

TEST(OgrApiTest, CurveLinearAndValue) {
  // Null handles
  EXPECT_FALSE(OGR_G_HasCurveGeometry(nullptr, FALSE));
  EXPECT_EQ(nullptr, OGR_G_GetLinearGeometry(nullptr, 0, nullptr));
  EXPECT_EQ(nullptr, OGR_G_GetCurveGeometry(nullptr, nullptr));
  EXPECT_EQ(nullptr, OGR_G_Value(nullptr, 0.0));

  // CircularString curve geometry
  OGRCircularString cs;
  cs.addPoint(0, 0);
  cs.addPoint(1, 1);
  cs.addPoint(2, 0);
  OGRGeometryH hCs = OGRGeometry::ToHandle(&cs);

  EXPECT_TRUE(OGR_G_HasCurveGeometry(hCs, TRUE));
  EXPECT_TRUE(OGR_G_HasCurveGeometry(hCs, FALSE));

  OGRGeometryH hLinear = OGR_G_GetLinearGeometry(hCs, 0, nullptr);
  ASSERT_NE(nullptr, hLinear);
  EXPECT_EQ(wkbLineString, wkbFlatten(OGR_G_GetGeometryType(hLinear)));
  OGR_G_DestroyGeometry(hLinear);

  OGRGeometryH hCurve = OGR_G_GetCurveGeometry(hCs, nullptr);
  ASSERT_NE(nullptr, hCurve);
  EXPECT_EQ(wkbCircularString, wkbFlatten(OGR_G_GetGeometryType(hCurve)));
  OGR_G_DestroyGeometry(hCurve);

  // OGR_G_Value on curve
  OGRGeometryH hValPt = OGR_G_Value(hCs, 0.5);
  ASSERT_NE(nullptr, hValPt);
  EXPECT_EQ(wkbPoint, wkbFlatten(OGR_G_GetGeometryType(hValPt)));
  OGR_G_DestroyGeometry(hValPt);

  // OGR_G_Value on non-curve
  OGRPolygon poly;
  EXPECT_EQ(nullptr, OGR_G_Value(OGRGeometry::ToHandle(&poly), 0.5));
}

}  // namespace
}  // namespace autotest2
