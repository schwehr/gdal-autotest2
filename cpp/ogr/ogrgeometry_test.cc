// Copyright 2024 Google LLC
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

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

TEST(OGRGeometryTest, SpatialReferenceManagement) {
  auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
  EXPECT_EQ(nullptr, pt->getSpatialReference());

  auto srs1 = new OGRSpatialReference();
  srs1->importFromEPSG(4326);
  pt->assignSpatialReference(srs1);
  srs1->Release();
  ASSERT_THAT(pt->getSpatialReference(), testing::NotNull());
  EXPECT_STREQ("WGS 84", pt->getSpatialReference()->GetName());

  // Replace SRS with another
  auto srs2 = new OGRSpatialReference();
  srs2->importFromEPSG(3857);
  pt->assignSpatialReference(srs2);
  srs2->Release();
  ASSERT_THAT(pt->getSpatialReference(), testing::NotNull());
  EXPECT_STREQ("WGS 84 / Pseudo-Mercator",
               pt->getSpatialReference()->GetName());

  // Clear SRS
  pt->assignSpatialReference(nullptr);
  EXPECT_EQ(nullptr, pt->getSpatialReference());
}

TEST(OGRGeometryTest, DimensionsAndFlags) {
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  EXPECT_EQ(0, pt->getDimension());
  EXPECT_EQ(2, pt->getCoordinateDimension());
  EXPECT_EQ(2, pt->CoordinateDimension());
  EXPECT_FALSE(pt->Is3D());
  EXPECT_FALSE(pt->IsMeasured());
  EXPECT_EQ(wkbPoint, pt->getIsoGeometryType());

  // Set 3D
  pt->set3D(TRUE);
  EXPECT_TRUE(pt->Is3D());
  EXPECT_EQ(3, pt->getCoordinateDimension());
  EXPECT_EQ(1001, static_cast<int>(pt->getIsoGeometryType()));

  // Set Measured
  pt->setMeasured(TRUE);
  EXPECT_TRUE(pt->IsMeasured());
  EXPECT_EQ(wkbPointZM, pt->getIsoGeometryType());

  // Set coordinate dimension
  pt->setCoordinateDimension(2);
  EXPECT_FALSE(pt->Is3D());
  EXPECT_FALSE(pt->IsMeasured());

  // FlattenTo2D
  pt->set3D(TRUE);
  pt->flattenTo2D();
  EXPECT_FALSE(pt->Is3D());

  // SwapXY
  pt->setX(10.0);
  pt->setY(20.0);
  pt->swapXY();
  EXPECT_DOUBLE_EQ(20.0, pt->getX());
  EXPECT_DOUBLE_EQ(10.0, pt->getY());

  // LineString dimensions
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(10, 10);
  EXPECT_EQ(1, ls->getDimension());

  // Segmentize
  ls->segmentize(5.0);
  EXPECT_GE(ls->getNumPoints(), 3);

  // Segmentize invalid threshold
  {
    CPLErrorReset();
    ls->segmentize(0.0);
    ls->segmentize(-1.0);
  }

  // Close rings on non-polygon (no-op)
  ls->closeRings();
}

TEST(OGRGeometryTest, WktExportImportAndFormatting) {
  auto pt = std::make_unique<OGRPoint>(12.34, 56.78);

  // exportToWkt
  char* pszWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, pt->exportToWkt(&pszWkt));
  ASSERT_THAT(pszWkt, testing::NotNull());
  EXPECT_THAT(pszWkt, testing::HasSubstr("POINT"));
  CPLFree(pszWkt);

  // exportToWkt with ISO variant
  pszWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, pt->exportToWkt(&pszWkt, wkbVariantIso));
  ASSERT_THAT(pszWkt, testing::NotNull());
  EXPECT_THAT(pszWkt, testing::HasSubstr("POINT"));
  CPLFree(pszWkt);

  // exportToWkt on Polygon with ISO variant
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(1, 0);
  ring->addPoint(1, 1);
  ring->addPoint(0, 1);
  ring->closeRings();
  poly->addRingDirectly(ring.release());

  pszWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, poly->exportToWkt(&pszWkt, wkbVariantIso));
  ASSERT_THAT(pszWkt, testing::NotNull());
  EXPECT_THAT(pszWkt, testing::HasSubstr("POLYGON"));
  CPLFree(pszWkt);

  // importFromWkt
  const char* wktInput = "POINT (5 10)";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wktInput));
  EXPECT_DOUBLE_EQ(5.0, pt->getX());
  EXPECT_DOUBLE_EQ(10.0, pt->getY());

  // exportToGML, KML, JSON
  char* pszGML = pt->exportToGML();
  ASSERT_THAT(pszGML, testing::NotNull());
  EXPECT_THAT(pszGML, testing::HasSubstr("<gml:Point"));
  CPLFree(pszGML);

  char* pszKML = pt->exportToKML();
  ASSERT_THAT(pszKML, testing::NotNull());
  EXPECT_THAT(pszKML, testing::HasSubstr("<Point>"));
  CPLFree(pszKML);

  char* pszJson = pt->exportToJson();
  ASSERT_THAT(pszJson, testing::NotNull());
  EXPECT_THAT(pszJson, testing::HasSubstr("Point"));
  CPLFree(pszJson);

  // dumpReadable
  FILE* fp = tmpfile();
  ASSERT_THAT(fp, testing::NotNull());
  pt->dumpReadable(fp, "PREFIX: ");
  fclose(fp);

  const std::string dumpStr = pt->dumpReadable();
  EXPECT_THAT(dumpStr, testing::HasSubstr("POINT"));
}

TEST(OGRGeometryTest, WkbExportImport) {
  auto pt = std::make_unique<OGRPoint>(10.0, 20.0);

  const size_t wkbSize = pt->WkbSize();
  EXPECT_EQ(21, wkbSize);

  std::vector<GByte> wkb(wkbSize);
  OGRGeometry* geom = pt.get();
  EXPECT_EQ(OGRERR_NONE, geom->exportToWkb(wkbNDR, wkb.data()));
  EXPECT_EQ(OGRERR_NONE, geom->exportToWkb(wkbXDR, wkb.data()));
  EXPECT_EQ(OGRERR_NONE, geom->exportToWkb(wkbNDR, wkb.data(), wkbVariantIso));

  // importFromWkb
  std::unique_ptr<OGRGeometry> geom2(new OGRPoint());
  EXPECT_EQ(OGRERR_NONE, geom2->importFromWkb(wkb.data(), wkb.size()));
  EXPECT_DOUBLE_EQ(10.0, geom2->toPoint()->getX());
  EXPECT_DOUBLE_EQ(20.0, geom2->toPoint()->getY());
}

TEST(OGRGeometryTest, DowncastingHelpers) {
  std::unique_ptr<OGRGeometry> pt = std::make_unique<OGRPoint>(1, 2);
  EXPECT_NE(nullptr, pt->toPoint());
  const OGRGeometry* cpt = pt.get();
  EXPECT_NE(nullptr, cpt->toPoint());

  std::unique_ptr<OGRGeometry> ls = std::make_unique<OGRLineString>();
  EXPECT_NE(nullptr, ls->toLineString());
  EXPECT_NE(nullptr, ls->toCurve());

  std::unique_ptr<OGRGeometry> poly = std::make_unique<OGRPolygon>();
  EXPECT_NE(nullptr, poly->toPolygon());
  EXPECT_NE(nullptr, poly->toCurvePolygon());
  EXPECT_NE(nullptr, poly->toSurface());

  std::unique_ptr<OGRGeometry> mp = std::make_unique<OGRMultiPoint>();
  EXPECT_NE(nullptr, mp->toMultiPoint());
  EXPECT_NE(nullptr, mp->toGeometryCollection());

  std::unique_ptr<OGRGeometry> mls = std::make_unique<OGRMultiLineString>();
  EXPECT_NE(nullptr, mls->toMultiLineString());
  EXPECT_NE(nullptr, mls->toMultiCurve());

  std::unique_ptr<OGRGeometry> mpoly = std::make_unique<OGRMultiPolygon>();
  EXPECT_NE(nullptr, mpoly->toMultiPolygon());
  EXPECT_NE(nullptr, mpoly->toMultiSurface());

  std::unique_ptr<OGRGeometry> cs = std::make_unique<OGRCircularString>();
  EXPECT_NE(nullptr, cs->toCircularString());
  EXPECT_NE(nullptr, cs->toCurve());

  std::unique_ptr<OGRGeometry> cc = std::make_unique<OGRCompoundCurve>();
  EXPECT_NE(nullptr, cc->toCompoundCurve());
  EXPECT_NE(nullptr, cc->toCurve());

  std::unique_ptr<OGRGeometry> cp = std::make_unique<OGRCurvePolygon>();
  EXPECT_NE(nullptr, cp->toCurvePolygon());
  EXPECT_NE(nullptr, cp->toSurface());

  std::unique_ptr<OGRGeometry> tri = std::make_unique<OGRTriangle>();
  EXPECT_NE(nullptr, tri->toTriangle());
  EXPECT_NE(nullptr, tri->toPolygon());

  std::unique_ptr<OGRGeometry> tin = std::make_unique<OGRTriangulatedSurface>();
  EXPECT_NE(nullptr, tin->toTriangulatedSurface());
  EXPECT_NE(nullptr, tin->toPolyhedralSurface());

  std::unique_ptr<OGRGeometry> ps = std::make_unique<OGRPolyhedralSurface>();
  EXPECT_NE(nullptr, ps->toPolyhedralSurface());
  EXPECT_NE(nullptr, ps->toSurface());
}

TEST(OGRGeometryTest, SpatialOperationsAndGEOS) {
  auto poly1 = std::make_unique<OGRPolygon>();
  auto ring1 = std::make_unique<OGRLinearRing>();
  ring1->addPoint(0, 0);
  ring1->addPoint(10, 0);
  ring1->addPoint(10, 10);
  ring1->addPoint(0, 10);
  ring1->closeRings();
  poly1->addRingDirectly(ring1.release());

  auto poly2 = std::make_unique<OGRPolygon>();
  auto ring2 = std::make_unique<OGRLinearRing>();
  ring2->addPoint(5, 5);
  ring2->addPoint(15, 5);
  ring2->addPoint(15, 15);
  ring2->addPoint(5, 15);
  ring2->closeRings();
  poly2->addRingDirectly(ring2.release());

  auto ptInside = std::make_unique<OGRPoint>(2, 2);
  auto ptOutside = std::make_unique<OGRPoint>(100, 100);

  // Intersects
  EXPECT_TRUE(poly1->Intersects(poly2.get()));
  EXPECT_TRUE(poly1->Intersect(poly2.get()));
  EXPECT_TRUE(poly1->Intersects(ptInside.get()));
  EXPECT_FALSE(poly1->Intersects(ptOutside.get()));

  // Equals
  EXPECT_TRUE(poly1->Equals(poly1.get()));
  EXPECT_FALSE(poly1->Equals(poly2.get()));

  // Disjoint
  EXPECT_FALSE(poly1->Disjoint(poly2.get()));
  EXPECT_TRUE(poly1->Disjoint(ptOutside.get()));

  // Touches
  auto ptEdge = std::make_unique<OGRPoint>(10, 5);
  EXPECT_TRUE(poly1->Touches(ptEdge.get()));

  // Crosses
  auto lsCross = std::make_unique<OGRLineString>();
  lsCross->addPoint(-5, 5);
  lsCross->addPoint(15, 5);
  EXPECT_TRUE(lsCross->Crosses(poly1.get()));

  // Within and Contains
  EXPECT_TRUE(ptInside->Within(poly1.get()));
  EXPECT_TRUE(poly1->Contains(ptInside.get()));
  EXPECT_FALSE(ptOutside->Within(poly1.get()));
  EXPECT_FALSE(poly1->Contains(ptOutside.get()));

  // Overlaps
  EXPECT_TRUE(poly1->Overlaps(poly2.get()));

  // Boundary
  std::unique_ptr<OGRGeometry> bnd(poly1->Boundary());
  ASSERT_THAT(bnd, testing::NotNull());
  EXPECT_TRUE(bnd->getGeometryType() == wkbLineString ||
              bnd->getGeometryType() == wkbMultiLineString);

  // ConvexHull
  std::unique_ptr<OGRGeometry> hull(poly1->ConvexHull());
  ASSERT_THAT(hull, testing::NotNull());

  // Buffer
  std::unique_ptr<OGRGeometry> buf(ptInside->Buffer(2.0, 8));
  ASSERT_THAT(buf, testing::NotNull());
  EXPECT_EQ(wkbPolygon, buf->getGeometryType());

  // Intersection
  std::unique_ptr<OGRGeometry> isect(poly1->Intersection(poly2.get()));
  ASSERT_THAT(isect, testing::NotNull());
  EXPECT_EQ(wkbPolygon, isect->getGeometryType());

  // Union
  std::unique_ptr<OGRGeometry> un(poly1->Union(poly2.get()));
  ASSERT_THAT(un, testing::NotNull());

  // Difference
  std::unique_ptr<OGRGeometry> diff(poly1->Difference(poly2.get()));
  ASSERT_THAT(diff, testing::NotNull());

  // SymDifference
  std::unique_ptr<OGRGeometry> symDiff(poly1->SymDifference(poly2.get()));
  ASSERT_THAT(symDiff, testing::NotNull());

  // Centroid
  OGRPoint centroid;
  EXPECT_EQ(OGRERR_NONE, poly1->Centroid(&centroid));
  EXPECT_DOUBLE_EQ(5.0, centroid.getX());
  EXPECT_DOUBLE_EQ(5.0, centroid.getY());

  // PointOnSurface
  OGRPoint posPoint(0, 0);
  EXPECT_EQ(OGRERR_NONE, poly1->PointOnSurface(&posPoint));
  EXPECT_TRUE(poly1->Intersects(&posPoint));

  // Simplify and SimplifyPreserveTopology
  std::unique_ptr<OGRGeometry> simp(lsCross->Simplify(1.0));
  ASSERT_THAT(simp, testing::NotNull());
  std::unique_ptr<OGRGeometry> simpPres(lsCross->SimplifyPreserveTopology(1.0));
  ASSERT_THAT(simpPres, testing::NotNull());

  // Distance and Distance3D
  EXPECT_DOUBLE_EQ(0.0, poly1->Distance(poly2.get()));
  EXPECT_GT(poly1->Distance(ptOutside.get()), 0.0);
  EXPECT_DOUBLE_EQ(-1.0, poly1->Distance3D(ptOutside.get()));

  // DelaunayTriangulation
  std::unique_ptr<OGRGeometry> dt(poly1->DelaunayTriangulation(0.0, TRUE));
  ASSERT_THAT(dt, testing::NotNull());

  // Polygonize
  auto mls = std::make_unique<OGRMultiLineString>();
  auto ringLine = std::make_unique<OGRLineString>();
  ringLine->addPoint(0, 0);
  ringLine->addPoint(1, 0);
  ringLine->addPoint(1, 1);
  ringLine->addPoint(0, 1);
  ringLine->addPoint(0, 0);
  mls->addGeometryDirectly(ringLine.release());
  std::unique_ptr<OGRGeometry> polyRes(mls->Polygonize());
  ASSERT_THAT(polyRes, testing::NotNull());

  // Round coordinates
  auto ptRound = std::make_unique<OGRPoint>(1.234567, 8.765432);
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.01;
  ptRound->roundCoordinates(prec);
  EXPECT_NEAR(1.23, ptRound->getX(), 1e-5);
  EXPECT_NEAR(8.77, ptRound->getY(), 1e-5);
}

TEST(OGRGeometryTest, Transformation) {
  auto pt = std::make_unique<OGRPoint>(2.3522, 48.8566);
  auto srsWGS84 = new OGRSpatialReference();
  srsWGS84->importFromEPSG(4326);
  srsWGS84->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  auto srs_cleaner1 = absl::MakeCleanup([srsWGS84] { srsWGS84->Release(); });
  pt->assignSpatialReference(srsWGS84);

  auto srs3857 = new OGRSpatialReference();
  srs3857->importFromEPSG(3857);
  srs3857->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  auto srs_cleaner = absl::MakeCleanup([srs3857] { srs3857->Release(); });

  EXPECT_EQ(OGRERR_NONE, pt->transformTo(srs3857));
  EXPECT_GT(pt->getX(), 100000.0);
  EXPECT_GT(pt->getY(), 100000.0);

  // Transform with OGRCoordinateTransformation
  OGRCoordinateTransformation* ct =
      OGRCreateCoordinateTransformation(pt->getSpatialReference(), srsWGS84);
  ASSERT_THAT(ct, testing::NotNull());
  auto ct_cleaner =
      absl::MakeCleanup([ct] { OCTDestroyCoordinateTransformation(ct); });
  EXPECT_EQ(OGRERR_NONE, pt->transform(ct));
  EXPECT_NEAR(2.3522, pt->getX(), 1e-3);
  EXPECT_NEAR(48.8566, pt->getY(), 1e-3);
}

TEST(OGRGeometryTest, CAPIWrappers) {
  // CreateGeometry & DestroyGeometry
  OGRGeometryH hPt = OGR_G_CreateGeometry(wkbPoint);
  ASSERT_THAT(hPt, testing::NotNull());
  EXPECT_EQ(wkbPoint, OGR_G_GetGeometryType(hPt));
  EXPECT_STREQ("POINT", OGR_G_GetGeometryName(hPt));

  // Point setting and getting
  OGR_G_SetPoint_2D(hPt, 0, 10.0, 20.0);
  EXPECT_DOUBLE_EQ(10.0, OGR_G_GetX(hPt, 0));
  EXPECT_DOUBLE_EQ(20.0, OGR_G_GetY(hPt, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetZ(hPt, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetM(hPt, 0));

  OGR_G_SetPoint(hPt, 0, 10.0, 20.0, 30.0);
  EXPECT_DOUBLE_EQ(30.0, OGR_G_GetZ(hPt, 0));

  OGR_G_SetPointM(hPt, 0, 10.0, 20.0, 40.0);
  EXPECT_DOUBLE_EQ(40.0, OGR_G_GetM(hPt, 0));

  OGR_G_SetPointZM(hPt, 0, 10.0, 20.0, 30.0, 40.0);
  double x = 0, y = 0, z = 0, m = 0;
  OGR_G_GetPointZM(hPt, 0, &x, &y, &z, &m);
  EXPECT_DOUBLE_EQ(10.0, x);
  EXPECT_DOUBLE_EQ(20.0, y);
  EXPECT_DOUBLE_EQ(30.0, z);
  EXPECT_DOUBLE_EQ(40.0, m);

  OGR_G_GetPoint(hPt, 0, &x, &y, &z);
  EXPECT_DOUBLE_EQ(10.0, x);
  EXPECT_DOUBLE_EQ(20.0, y);
  EXPECT_DOUBLE_EQ(30.0, z);

  // LineString points C API
  OGRGeometryH hLS = OGR_G_CreateGeometry(wkbLineString);
  OGR_G_AddPoint_2D(hLS, 0, 0);
  OGR_G_AddPoint(hLS, 1, 1, 2);
  OGR_G_AddPointM(hLS, 2, 2, 5);
  OGR_G_AddPointZM(hLS, 3, 3, 4, 6);
  EXPECT_EQ(4, OGR_G_GetPointCount(hLS));

  std::vector<double> padfX(4), padfY(4), padfZ(4), padfM(4);
  EXPECT_EQ(4, OGR_G_GetPointsZM(hLS, padfX.data(), sizeof(double),
                                 padfY.data(), sizeof(double), padfZ.data(),
                                 sizeof(double), padfM.data(), sizeof(double)));
  EXPECT_EQ(4, OGR_G_GetPoints(hLS, padfX.data(), sizeof(double), padfY.data(),
                               sizeof(double), padfZ.data(), sizeof(double)));

  // Spatial Reference
  auto srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  OGR_G_AssignSpatialReference(hLS, OGRSpatialReference::ToHandle(srs));
  srs->Release();
  ASSERT_THAT(OGR_G_GetSpatialReference(hLS), testing::NotNull());

  // Export / Import WKT
  char* pszWkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, OGR_G_ExportToWkt(hLS, &pszWkt));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  EXPECT_EQ(OGRERR_NONE, OGR_G_ExportToIsoWkt(hLS, &pszWkt));
  ASSERT_THAT(pszWkt, testing::NotNull());
  CPLFree(pszWkt);

  // Export / Import WKB
  const int nWkbSize = OGR_G_WkbSize(hLS);
  EXPECT_GT(nWkbSize, 0);
  EXPECT_EQ(nWkbSize, OGR_G_WkbSizeEx(hLS));
  std::vector<GByte> wkb(nWkbSize);
  EXPECT_EQ(OGRERR_NONE, OGR_G_ExportToWkb(hLS, wkbNDR, wkb.data()));
  EXPECT_EQ(OGRERR_NONE, OGR_G_ExportToIsoWkb(hLS, wkbNDR, wkb.data()));

  OGRGeometryH hImported = nullptr;
  EXPECT_EQ(OGRERR_NONE,
            OGR_G_CreateFromWkb(wkb.data(), nullptr, &hImported, nWkbSize));
  ASSERT_THAT(hImported, testing::NotNull());
  OGR_G_DestroyGeometry(hImported);

  // Clone
  OGRGeometryH hClone = OGR_G_Clone(hLS);
  ASSERT_THAT(hClone, testing::NotNull());
  EXPECT_TRUE(OGR_G_Equals(hLS, hClone));
  OGR_G_DestroyGeometry(hClone);

  // Geometry Collection C API
  OGRGeometryH hGC = OGR_G_CreateGeometry(wkbGeometryCollection);
  OGR_G_AddGeometry(hGC, hPt);
  EXPECT_EQ(1, OGR_G_GetGeometryCount(hGC));
  EXPECT_NE(nullptr, OGR_G_GetGeometryRef(hGC, 0));
  OGR_G_RemoveGeometry(hGC, 0, TRUE);
  EXPECT_EQ(0, OGR_G_GetGeometryCount(hGC));

  // Polygon and Rings C API
  OGRGeometryH hPoly = OGR_G_CreateGeometry(wkbPolygon);
  OGRGeometryH hRing = OGR_G_CreateGeometry(wkbLinearRing);
  OGR_G_AddPoint_2D(hRing, 0, 0);
  OGR_G_AddPoint_2D(hRing, 10, 0);
  OGR_G_AddPoint_2D(hRing, 10, 10);
  OGR_G_AddPoint_2D(hRing, 0, 10);
  OGR_G_CloseRings(hRing);
  OGR_G_AddGeometryDirectly(hPoly, hRing);

  EXPECT_TRUE(OGR_G_IsValid(hPoly));
  EXPECT_TRUE(OGR_G_IsSimple(hPoly));
  EXPECT_FALSE(OGR_G_IsEmpty(hPoly));
  EXPECT_DOUBLE_EQ(100.0, OGR_G_Area(hPoly));
  EXPECT_DOUBLE_EQ(40.0, OGR_G_Length(hPoly));

  // Segmentize, SwapXY, FlattenTo2D
  OGR_G_Segmentize(hPoly, 2.0);
  OGR_G_SwapXY(hPoly);
  OGR_G_FlattenTo2D(hPoly);
  EXPECT_EQ(2, OGR_G_GetCoordinateDimension(hPoly));

  // GEOS ops via C API
  OGRGeometryH hBuf = OGR_G_Buffer(hPoly, 1.0, 4);
  ASSERT_THAT(hBuf, testing::NotNull());
  OGR_G_DestroyGeometry(hBuf);

  OGRGeometryH hHull = OGR_G_ConvexHull(hPoly);
  ASSERT_THAT(hHull, testing::NotNull());
  OGR_G_DestroyGeometry(hHull);

  OGRGeometryH hBnd = OGR_G_Boundary(hPoly);
  ASSERT_THAT(hBnd, testing::NotNull());
  OGR_G_DestroyGeometry(hBnd);

  OGRGeometryH hCent = OGR_G_CreateGeometry(wkbPoint);
  EXPECT_EQ(OGRERR_NONE, OGR_G_Centroid(hPoly, hCent));
  EXPECT_DOUBLE_EQ(5.0, OGR_G_GetX(hCent, 0));
  EXPECT_DOUBLE_EQ(5.0, OGR_G_GetY(hCent, 0));
  OGR_G_DestroyGeometry(hCent);

  // Clean up
  OGR_G_DestroyGeometry(hPt);
  OGR_G_DestroyGeometry(hLS);
  OGR_G_DestroyGeometry(hGC);
  OGR_G_DestroyGeometry(hPoly);
}

TEST(OGRGeometryTest, CAPINullSafety) {
  EXPECT_EQ(nullptr, OGR_G_Clone(nullptr));
  EXPECT_EQ(0, OGR_G_GetDimension(nullptr));
  EXPECT_EQ(0, OGR_G_GetCoordinateDimension(nullptr));
  OGR_G_SetCoordinateDimension(nullptr, 2);
  EXPECT_EQ(0, OGR_G_WkbSize(nullptr));
  EXPECT_EQ(0, OGR_G_WkbSizeEx(nullptr));
  EXPECT_EQ(wkbUnknown, OGR_G_GetGeometryType(nullptr));
  EXPECT_STREQ("", OGR_G_GetGeometryName(nullptr));
  EXPECT_EQ(nullptr, OGR_G_GetSpatialReference(nullptr));
  OGR_G_AssignSpatialReference(nullptr, nullptr);
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_ExportToWkb(nullptr, wkbNDR, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_ExportToIsoWkb(nullptr, wkbNDR, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_ExportToWkt(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_ExportToIsoWkt(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_ImportFromWkb(nullptr, nullptr, 0));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_ImportFromWkt(nullptr, nullptr));
  OGR_G_CloseRings(nullptr);
  OGR_G_Segmentize(nullptr, 1.0);
  OGR_G_Empty(nullptr);
  EXPECT_TRUE(OGR_G_IsEmpty(nullptr));
  EXPECT_FALSE(OGR_G_IsValid(nullptr));
  EXPECT_TRUE(OGR_G_IsSimple(nullptr));
  EXPECT_FALSE(OGR_G_IsRing(nullptr));
  EXPECT_FALSE(OGR_G_Intersects(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Equals(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Disjoint(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Touches(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Crosses(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Within(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Contains(nullptr, nullptr));
  EXPECT_FALSE(OGR_G_Overlaps(nullptr, nullptr));
  EXPECT_EQ(nullptr, OGR_G_Boundary(nullptr));
  EXPECT_EQ(nullptr, OGR_G_ConvexHull(nullptr));
  EXPECT_EQ(nullptr, OGR_G_Buffer(nullptr, 1.0, 4));
  EXPECT_EQ(nullptr, OGR_G_Intersection(nullptr, nullptr));
  EXPECT_EQ(nullptr, OGR_G_Union(nullptr, nullptr));
  EXPECT_EQ(nullptr, OGR_G_UnionCascaded(nullptr));
  EXPECT_EQ(nullptr, OGR_G_PointOnSurface(nullptr));
  EXPECT_EQ(nullptr, OGR_G_Difference(nullptr, nullptr));
  EXPECT_EQ(nullptr, OGR_G_SymDifference(nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Distance(nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Distance3D(nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Length(nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Area(nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_Centroid(nullptr, nullptr));
  EXPECT_EQ(0, OGR_G_GetPointCount(nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetX(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetY(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetZ(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_G_GetM(nullptr, 0));
  EXPECT_EQ(0, OGR_G_GetGeometryCount(nullptr));
  EXPECT_EQ(nullptr, OGR_G_GetGeometryRef(nullptr, 0));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, OGR_G_AddGeometry(nullptr, nullptr));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            OGR_G_AddGeometryDirectly(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_RemoveGeometry(nullptr, 0, TRUE));
  EXPECT_EQ(nullptr, OGR_G_Polygonize(nullptr));
  OGR_G_SwapXY(nullptr);
  EXPECT_FALSE(OGR_G_HasCurveGeometry(nullptr, TRUE));
  EXPECT_EQ(nullptr, OGR_G_GetCurveGeometry(nullptr, nullptr));
  EXPECT_EQ(nullptr, OGR_G_GetLinearGeometry(nullptr, 0.0, nullptr));
  OGR_G_DestroyGeometry(nullptr);
}

TEST(OGRGeometryTest, TypeNamesAndConversions) {
  const std::vector<OGRwkbGeometryType> baseTypes = {
      wkbUnknown,
      wkbPoint,
      wkbLineString,
      wkbPolygon,
      wkbMultiPoint,
      wkbMultiLineString,
      wkbMultiPolygon,
      wkbGeometryCollection,
      wkbCircularString,
      wkbCompoundCurve,
      wkbCurvePolygon,
      wkbMultiCurve,
      wkbMultiSurface,
      wkbCurve,
      wkbSurface,
      wkbPolyhedralSurface,
      wkbTIN,
      wkbTriangle,
      wkbNone,
  };

  for (auto baseType : baseTypes) {
    for (int hasZ = 0; hasZ <= 1; ++hasZ) {
      for (int hasM = 0; hasM <= 1; ++hasM) {
        OGRwkbGeometryType t = OGR_GT_SetModifier(baseType, hasZ, hasM);
        const char* pszName = OGRGeometryTypeToName(t);
        EXPECT_NE(nullptr, pszName);

        for (bool camelCase : {false, true}) {
          for (bool addZM : {false, true}) {
            for (bool spaceZM : {false, true}) {
              const char* pszOgc =
                  OGRToOGCGeomType(t, camelCase, addZM, spaceZM);
              EXPECT_NE(nullptr, pszOgc);
            }
          }
        }
      }
    }
  }

  // OGRFromOGCGeomType
  const char* const ogcStrings[] = {
      "POINT",
      "POINT Z",
      "POINT M",
      "POINT ZM",
      "Point",
      "PointZ",
      "PointM",
      "PointZM",
      "LINESTRING",
      "LINESTRING Z",
      "LINESTRING M",
      "LINESTRING ZM",
      "POLYGON",
      "POLYGON Z",
      "POLYGON M",
      "POLYGON ZM",
      "MULTIPOINT",
      "MULTIPOINT Z",
      "MULTILINESTRING",
      "MULTIPOLYGON",
      "GEOMETRYCOLLECTION",
      "CIRCULARSTRING",
      "COMPOUNDCURVE",
      "CURVEPOLYGON",
      "MULTICURVE",
      "MULTISURFACE",
      "TRIANGLE",
      "POLYHEDRALSURFACE",
      "TIN",
      "CURVE",
      "SURFACE",
      "UNKNOWN",
      "INVALID_GEOM_TYPE",
      "",
  };

  for (const char* pszStr : ogcStrings) {
    OGRwkbGeometryType eType = OGRFromOGCGeomType(pszStr);
    EXPECT_TRUE(eType >= wkbUnknown);
  }
}

TEST(OGRGeometryTest, OGR_GT_Functions) {
  // Flatten, HasZ, HasM, SetZ, SetM
  EXPECT_EQ(wkbPoint, OGR_GT_Flatten(wkbPoint));
  EXPECT_EQ(wkbPoint, OGR_GT_Flatten(wkbPoint25D));
  EXPECT_EQ(wkbPoint, OGR_GT_Flatten(wkbPointM));
  EXPECT_EQ(wkbPoint, OGR_GT_Flatten(wkbPointZM));

  EXPECT_FALSE(OGR_GT_HasZ(wkbPoint));
  EXPECT_TRUE(OGR_GT_HasZ(wkbPoint25D));
  EXPECT_FALSE(OGR_GT_HasZ(wkbPointM));
  EXPECT_TRUE(OGR_GT_HasZ(wkbPointZM));

  EXPECT_FALSE(OGR_GT_HasM(wkbPoint));
  EXPECT_FALSE(OGR_GT_HasM(wkbPoint25D));
  EXPECT_TRUE(OGR_GT_HasM(wkbPointM));
  EXPECT_TRUE(OGR_GT_HasM(wkbPointZM));

  EXPECT_EQ(wkbPoint25D, OGR_GT_SetZ(wkbPoint));
  EXPECT_EQ(wkbPoint25D, OGR_GT_SetZ(wkbPoint25D));
  EXPECT_EQ(wkbPointZM, OGR_GT_SetZ(wkbPointM));
  EXPECT_EQ(wkbNone, OGR_GT_SetZ(wkbNone));

  EXPECT_EQ(wkbPointM, OGR_GT_SetM(wkbPoint));
  EXPECT_EQ(wkbPointM, OGR_GT_SetM(wkbPointM));
  EXPECT_EQ(wkbPointZM, OGR_GT_SetM(wkbPoint25D));
  EXPECT_EQ(wkbNone, OGR_GT_SetM(wkbNone));

  // SetModifier
  EXPECT_EQ(wkbPoint, OGR_GT_SetModifier(wkbPoint, 0, 0));
  EXPECT_EQ(wkbPoint25D, OGR_GT_SetModifier(wkbPoint, 1, 0));
  EXPECT_EQ(wkbPointM, OGR_GT_SetModifier(wkbPoint, 0, 1));
  EXPECT_EQ(wkbPointZM, OGR_GT_SetModifier(wkbPoint, 1, 1));

  // IsSubClassOf
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbPoint, wkbPoint));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbPoint, wkbUnknown));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiPoint, wkbGeometryCollection));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiLineString, wkbGeometryCollection));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiPolygon, wkbGeometryCollection));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiCurve, wkbGeometryCollection));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiSurface, wkbGeometryCollection));
  EXPECT_FALSE(OGR_GT_IsSubClassOf(wkbPoint, wkbGeometryCollection));

  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbPolygon, wkbCurvePolygon));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbTriangle, wkbCurvePolygon));
  EXPECT_FALSE(OGR_GT_IsSubClassOf(wkbPoint, wkbCurvePolygon));

  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiLineString, wkbMultiCurve));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbMultiPolygon, wkbMultiSurface));

  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbLineString, wkbCurve));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbCircularString, wkbCurve));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbCompoundCurve, wkbCurve));

  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbCurvePolygon, wkbSurface));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbPolygon, wkbSurface));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbTriangle, wkbSurface));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbPolyhedralSurface, wkbSurface));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbTIN, wkbSurface));

  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbTriangle, wkbPolygon));
  EXPECT_TRUE(OGR_GT_IsSubClassOf(wkbTIN, wkbPolyhedralSurface));
  EXPECT_FALSE(OGR_GT_IsSubClassOf(wkbPoint, wkbPolygon));

  // GetCollection
  EXPECT_EQ(wkbNone, OGR_GT_GetCollection(wkbNone));
  EXPECT_EQ(wkbMultiPoint, OGR_GT_GetCollection(wkbPoint));
  EXPECT_EQ(wkbMultiLineString, OGR_GT_GetCollection(wkbLineString));
  EXPECT_EQ(wkbMultiPolygon, OGR_GT_GetCollection(wkbPolygon));
  EXPECT_EQ(wkbTIN, OGR_GT_GetCollection(wkbTriangle));
  EXPECT_EQ(wkbMultiCurve, OGR_GT_GetCollection(wkbCircularString));
  EXPECT_EQ(wkbMultiSurface, OGR_GT_GetCollection(wkbCurvePolygon));
  EXPECT_EQ(wkbUnknown, OGR_GT_GetCollection(wkbGeometryCollection));

  // GetSingle
  EXPECT_EQ(wkbNone, OGR_GT_GetSingle(wkbNone));
  EXPECT_EQ(wkbPoint, OGR_GT_GetSingle(wkbMultiPoint));
  EXPECT_EQ(wkbLineString, OGR_GT_GetSingle(wkbMultiLineString));
  EXPECT_EQ(wkbPolygon, OGR_GT_GetSingle(wkbMultiPolygon));
  EXPECT_EQ(wkbCompoundCurve, OGR_GT_GetSingle(wkbMultiCurve));
  EXPECT_EQ(wkbCurvePolygon, OGR_GT_GetSingle(wkbMultiSurface));
  EXPECT_EQ(wkbUnknown, OGR_GT_GetSingle(wkbGeometryCollection));
  EXPECT_EQ(wkbPoint, OGR_GT_GetSingle(wkbPoint));

  // GetCurve
  EXPECT_EQ(wkbCompoundCurve, OGR_GT_GetCurve(wkbLineString));
  EXPECT_EQ(wkbCurvePolygon, OGR_GT_GetCurve(wkbPolygon));
  EXPECT_EQ(wkbCurvePolygon, OGR_GT_GetCurve(wkbTriangle));
  EXPECT_EQ(wkbMultiCurve, OGR_GT_GetCurve(wkbMultiLineString));
  EXPECT_EQ(wkbMultiSurface, OGR_GT_GetCurve(wkbMultiPolygon));
  EXPECT_EQ(wkbPoint, OGR_GT_GetCurve(wkbPoint));

  // GetLinear
  EXPECT_EQ(wkbLineString, OGR_GT_GetLinear(wkbCompoundCurve));
  EXPECT_EQ(wkbPolygon, OGR_GT_GetLinear(wkbCurvePolygon));
  EXPECT_EQ(wkbMultiLineString, OGR_GT_GetLinear(wkbMultiCurve));
  EXPECT_EQ(wkbMultiPolygon, OGR_GT_GetLinear(wkbMultiSurface));
  EXPECT_EQ(wkbPoint, OGR_GT_GetLinear(wkbPoint));

  // IsCurve, IsSurface, IsNonLinear
  EXPECT_TRUE(OGR_GT_IsCurve(wkbLineString));
  EXPECT_TRUE(OGR_GT_IsCurve(wkbCircularString));
  EXPECT_TRUE(OGR_GT_IsCurve(wkbCompoundCurve));
  EXPECT_FALSE(OGR_GT_IsCurve(wkbPoint));

  EXPECT_TRUE(OGR_GT_IsSurface(wkbPolygon));
  EXPECT_TRUE(OGR_GT_IsSurface(wkbCurvePolygon));
  EXPECT_FALSE(OGR_GT_IsSurface(wkbLineString));

  EXPECT_TRUE(OGR_GT_IsNonLinear(wkbCircularString));
  EXPECT_TRUE(OGR_GT_IsNonLinear(wkbCompoundCurve));
  EXPECT_TRUE(OGR_GT_IsNonLinear(wkbCurvePolygon));
  EXPECT_TRUE(OGR_GT_IsNonLinear(wkbMultiCurve));
  EXPECT_TRUE(OGR_GT_IsNonLinear(wkbMultiSurface));
  EXPECT_FALSE(OGR_GT_IsNonLinear(wkbPoint));
  EXPECT_FALSE(OGR_GT_IsNonLinear(wkbLineString));
}

TEST(OGRGeometryTest, MergeGeometryTypes) {
  EXPECT_EQ(wkbUnknown, OGRMergeGeometryTypes(wkbUnknown, wkbPoint));
  EXPECT_EQ(wkbUnknown, OGRMergeGeometryTypes(wkbPoint, wkbUnknown));
  EXPECT_EQ(wkbPoint, OGRMergeGeometryTypes(wkbNone, wkbPoint));
  EXPECT_EQ(wkbPoint, OGRMergeGeometryTypes(wkbPoint, wkbNone));
  EXPECT_EQ(wkbPoint, OGRMergeGeometryTypes(wkbPoint, wkbPoint));
  EXPECT_EQ(wkbPoint25D, OGRMergeGeometryTypes(wkbPoint, wkbPoint25D));
  EXPECT_EQ(wkbPointZM, OGRMergeGeometryTypes(wkbPoint25D, wkbPointM));

  EXPECT_EQ(wkbCompoundCurve,
            OGRMergeGeometryTypesEx(wkbLineString, wkbCircularString, TRUE));
  EXPECT_EQ(wkbCurvePolygon,
            OGRMergeGeometryTypesEx(wkbPolygon, wkbCurvePolygon, TRUE));
  EXPECT_EQ(wkbMultiSurface,
            OGRMergeGeometryTypesEx(wkbMultiPolygon, wkbMultiSurface, TRUE));
  EXPECT_EQ(wkbMultiCurve,
            OGRMergeGeometryTypesEx(wkbMultiCurve, wkbMultiLineString, TRUE));

  EXPECT_EQ(wkbCurvePolygon,
            OGRMergeGeometryTypesEx(wkbPolygon, wkbCurvePolygon, FALSE));
  EXPECT_EQ(wkbCurvePolygon,
            OGRMergeGeometryTypesEx(wkbCurvePolygon, wkbPolygon, FALSE));

  EXPECT_EQ(wkbUnknown, OGRMergeGeometryTypes(wkbPoint, wkbLineString));
}

TEST(OGRGeometryTest, DumpReadableOptions) {
  auto pt = std::make_unique<OGRPoint>(1.234567, 2.345678, 3.456789);
  pt->setM(4.56789);

  // WKT and precision options
  char** papszOpts = nullptr;
  papszOpts = CSLSetNameValue(papszOpts, "XY_COORD_PRECISION", "2");
  papszOpts = CSLSetNameValue(papszOpts, "Z_COORD_PRECISION", "1");
  papszOpts = CSLSetNameValue(papszOpts, "DISPLAY_GEOMETRY", "WKT");
  std::string dump = pt->dumpReadable("PT: ", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("PT: "));
  EXPECT_THAT(dump, testing::HasSubstr("POINT"));

  papszOpts = CSLSetNameValue(papszOpts, "DISPLAY_GEOMETRY", "ISO_WKT");
  dump = pt->dumpReadable(nullptr, papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("POINT"));

  papszOpts = CSLSetNameValue(papszOpts, "DISPLAY_GEOMETRY", "NO");
  dump = pt->dumpReadable("PREFIX: ", papszOpts);
  EXPECT_TRUE(dump.empty());

  // DISPLAY_GEOMETRY=SUMMARY on various geometry types
  papszOpts = CSLSetNameValue(papszOpts, "DISPLAY_GEOMETRY", "SUMMARY");

  dump = pt->dumpReadable("SUM: ", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("SUM: POINT"));

  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  dump = ls->dumpReadable("", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("2 points"));

  auto cs = std::make_unique<OGRCircularString>();
  cs->addPoint(0, 0);
  cs->addPoint(1, 1);
  cs->addPoint(2, 0);
  dump = cs->dumpReadable("", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("3 points"));

  auto poly = std::make_unique<OGRPolygon>();
  auto ringExt = std::make_unique<OGRLinearRing>();
  ringExt->addPoint(0, 0);
  ringExt->addPoint(10, 0);
  ringExt->addPoint(10, 10);
  ringExt->addPoint(0, 10);
  ringExt->addPoint(0, 0);
  poly->addRingDirectly(ringExt.release());

  auto ringInt1 = std::make_unique<OGRLinearRing>();
  ringInt1->addPoint(1, 1);
  ringInt1->addPoint(2, 1);
  ringInt1->addPoint(2, 2);
  ringInt1->addPoint(1, 2);
  ringInt1->addPoint(1, 1);
  poly->addRingDirectly(ringInt1.release());

  auto ringInt2 = std::make_unique<OGRLinearRing>();
  ringInt2->addPoint(3, 3);
  ringInt2->addPoint(4, 3);
  ringInt2->addPoint(4, 4);
  ringInt2->addPoint(3, 4);
  ringInt2->addPoint(3, 3);
  poly->addRingDirectly(ringInt2.release());

  dump = poly->dumpReadable("", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("2 inner rings"));

  // Empty polygon
  auto polyEmpty = std::make_unique<OGRPolygon>();
  dump = polyEmpty->dumpReadable("", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("empty"));

  // PolyhedralSurface
  auto ps = std::make_unique<OGRPolyhedralSurface>();
  ps->addGeometry(poly.get());
  dump = ps->dumpReadable("", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("1 geometries"));

  // GeometryCollection
  auto gc = std::make_unique<OGRGeometryCollection>();
  gc->addGeometry(pt.get());
  gc->addGeometry(ls.get());
  dump = gc->dumpReadable("", papszOpts);
  EXPECT_THAT(dump, testing::HasSubstr("2 geometries"));

  CSLDestroy(papszOpts);

  // OGR_G_DumpReadable C API
  FILE* fp = tmpfile();
  ASSERT_THAT(fp, testing::NotNull());
  OGR_G_DumpReadable(OGRGeometry::ToHandle(poly.get()), fp, "CAPI: ");
  OGR_G_DumpReadable(nullptr, fp, "NULL: ");
  fclose(fp);
}

TEST(OGRGeometryTest, PreparedGeometryAPI) {
  EXPECT_TRUE(OGRHasPreparedGeometrySupport());

  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(10, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring.release());

  OGRGeometryH hPoly = OGRGeometry::ToHandle(poly.get());
  OGRPreparedGeometryH hPrep = OGRCreatePreparedGeometry(hPoly);
  ASSERT_THAT(hPrep, testing::NotNull());

  auto ptInside = std::make_unique<OGRPoint>(5, 5);
  auto ptOutside = std::make_unique<OGRPoint>(15, 15);
  OGRGeometryH hPtIn = OGRGeometry::ToHandle(ptInside.get());
  OGRGeometryH hPtOut = OGRGeometry::ToHandle(ptOutside.get());

  EXPECT_TRUE(OGRPreparedGeometryIntersects(hPrep, hPtIn));
  EXPECT_FALSE(OGRPreparedGeometryIntersects(hPrep, hPtOut));

  EXPECT_TRUE(OGRPreparedGeometryContains(hPrep, hPtIn));
  EXPECT_FALSE(OGRPreparedGeometryContains(hPrep, hPtOut));

  // UniquePtr deleter
  {
    OGRPreparedGeometryUniquePtr up(OGRCreatePreparedGeometry(hPoly));
    EXPECT_NE(nullptr, up.get());
  }

  // Null safety
  EXPECT_FALSE(OGRPreparedGeometryIntersects(nullptr, hPtIn));
  EXPECT_FALSE(OGRPreparedGeometryIntersects(hPrep, nullptr));
  EXPECT_FALSE(OGRPreparedGeometryContains(nullptr, hPtIn));
  EXPECT_FALSE(OGRPreparedGeometryContains(hPrep, nullptr));
  OGRDestroyPreparedGeometry(hPrep);
  OGRDestroyPreparedGeometry(nullptr);
}

TEST(OGRGeometryTest, EWKBAndHexEWKB) {
  auto pt = std::make_unique<OGRPoint>(10.0, 20.0);

  // OGRGeometryToHexEWKB with SRID and PostGIS versions
  std::unique_ptr<char, CplFreeDeleter> pszHex1(
      OGRGeometryToHexEWKB(pt.get(), 4326, 2, 2));
  ASSERT_THAT(pszHex1, testing::NotNull());
  EXPECT_GT(strlen(pszHex1.get()), 0);

  int nSRID = 0;
  std::unique_ptr<OGRGeometry> poImported1(
      OGRGeometryFromHexEWKB(pszHex1.get(), &nSRID, FALSE));
  ASSERT_THAT(poImported1, testing::NotNull());
  EXPECT_EQ(4326, nSRID);
  EXPECT_EQ(wkbPoint, poImported1->getGeometryType());

  // Empty point with PostGIS >= 2.2
  auto ptEmpty = std::make_unique<OGRPoint>();
  std::unique_ptr<char, CplFreeDeleter> pszHexEmpty(
      OGRGeometryToHexEWKB(ptEmpty.get(), 0, 3, 0));
  ASSERT_THAT(pszHexEmpty, testing::NotNull());

  // PostGIS 1
  std::unique_ptr<char, CplFreeDeleter> pszHexPG1(
      OGRGeometryToHexEWKB(pt.get(), 0, 1, 5));
  ASSERT_THAT(pszHexPG1, testing::NotNull());
  std::unique_ptr<OGRGeometry> poImportedPG1(
      OGRGeometryFromHexEWKB(pszHexPG1.get(), &nSRID, TRUE));
  ASSERT_THAT(poImportedPG1, testing::NotNull());

  // Null safety
  EXPECT_EQ(nullptr, OGRGeometryFromHexEWKB(nullptr, nullptr, FALSE));
  EXPECT_EQ(nullptr, OGRGeometryFromEWKB(nullptr, 0, nullptr, FALSE));
}

namespace {
class CountingVisitor : public OGRDefaultGeometryVisitor {
 public:
  int nCount = 0;
  using OGRDefaultGeometryVisitor::visit;
  void visit(OGRPoint* poPoint) override {
    nCount++;
    OGRDefaultGeometryVisitor::visit(poPoint);
  }
};

class ConstCountingVisitor : public OGRDefaultConstGeometryVisitor {
 public:
  int nCount = 0;
  using OGRDefaultConstGeometryVisitor::visit;
  void visit(const OGRPoint* poPoint) override {
    nCount++;
    OGRDefaultConstGeometryVisitor::visit(poPoint);
  }
};
}  // namespace

TEST(OGRGeometryTest, GeometryVisitorPattern) {
  CountingVisitor visitor;
  ConstCountingVisitor constVisitor;

  auto pt = std::make_unique<OGRPoint>(1, 2);
  pt->accept(&visitor);
  static_cast<const OGRPoint*>(pt.get())->accept(&constVisitor);
  EXPECT_EQ(1, visitor.nCount);
  EXPECT_EQ(1, constVisitor.nCount);

  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  ls->accept(&visitor);
  static_cast<const OGRLineString*>(ls.get())->accept(&constVisitor);

  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(1, 0);
  ring->addPoint(1, 1);
  ring->addPoint(0, 0);
  ring->accept(&visitor);
  static_cast<const OGRLinearRing*>(ring.get())->accept(&constVisitor);

  auto cs = std::make_unique<OGRCircularString>();
  cs->addPoint(0, 0);
  cs->addPoint(1, 1);
  cs->addPoint(2, 0);
  cs->accept(&visitor);
  static_cast<const OGRCircularString*>(cs.get())->accept(&constVisitor);

  auto cc = std::make_unique<OGRCompoundCurve>();
  cc->addCurveDirectly(ls->clone());
  cc->accept(&visitor);
  static_cast<const OGRCompoundCurve*>(cc.get())->accept(&constVisitor);

  auto poly = std::make_unique<OGRPolygon>();
  poly->addRingDirectly(ring->clone());
  poly->accept(&visitor);
  static_cast<const OGRPolygon*>(poly.get())->accept(&constVisitor);

  auto cp = std::make_unique<OGRCurvePolygon>();
  auto csRing = std::make_unique<OGRCircularString>();
  csRing->addPoint(0, 0);
  csRing->addPoint(1, 1);
  csRing->addPoint(2, 0);
  csRing->addPoint(1, -1);
  csRing->addPoint(0, 0);
  cp->addRingDirectly(csRing.release());
  cp->accept(&visitor);
  static_cast<const OGRCurvePolygon*>(cp.get())->accept(&constVisitor);

  auto mp = std::make_unique<OGRMultiPoint>();
  mp->addGeometry(pt.get());
  mp->accept(&visitor);
  static_cast<const OGRMultiPoint*>(mp.get())->accept(&constVisitor);

  auto mls = std::make_unique<OGRMultiLineString>();
  mls->addGeometry(ls.get());
  mls->accept(&visitor);
  static_cast<const OGRMultiLineString*>(mls.get())->accept(&constVisitor);

  auto mpoly = std::make_unique<OGRMultiPolygon>();
  mpoly->addGeometry(poly.get());
  mpoly->accept(&visitor);
  static_cast<const OGRMultiPolygon*>(mpoly.get())->accept(&constVisitor);

  auto mc = std::make_unique<OGRMultiCurve>();
  mc->addGeometry(ls.get());
  mc->accept(&visitor);
  static_cast<const OGRMultiCurve*>(mc.get())->accept(&constVisitor);

  auto ms = std::make_unique<OGRMultiSurface>();
  ms->addGeometry(poly.get());
  ms->accept(&visitor);
  static_cast<const OGRMultiSurface*>(ms.get())->accept(&constVisitor);

  auto tri = std::make_unique<OGRTriangle>(OGRPoint(0, 0), OGRPoint(1, 0),
                                           OGRPoint(0, 1));
  tri->accept(&visitor);
  static_cast<const OGRTriangle*>(tri.get())->accept(&constVisitor);

  auto ps = std::make_unique<OGRPolyhedralSurface>();
  ps->addGeometry(poly.get());
  ps->accept(&visitor);
  static_cast<const OGRPolyhedralSurface*>(ps.get())->accept(&constVisitor);

  auto tin = std::make_unique<OGRTriangulatedSurface>();
  tin->addGeometry(tri.get());
  tin->accept(&visitor);
  static_cast<const OGRTriangulatedSurface*>(tin.get())->accept(&constVisitor);

  auto gc = std::make_unique<OGRGeometryCollection>();
  gc->addGeometry(pt.get());
  gc->accept(&visitor);
  static_cast<const OGRGeometryCollection*>(gc.get())->accept(&constVisitor);
}

template <typename T>
struct ExposeProtected : public T {
  using OGRGeometry::HomogenizeDimensionalityWith;
  using OGRGeometry::IsSFCGALCompatible;
  using T::T;
};

TEST(OGRGeometryTest, HomogenizeDimensionalityAndBinaryPrecision) {
  auto pt2D = std::make_unique<ExposeProtected<OGRPoint>>();
  pt2D->setX(1.0);
  pt2D->setY(2.0);
  auto pt3D = std::make_unique<OGRPoint>(1.0, 2.0, 3.0);
  pt2D->HomogenizeDimensionalityWith(pt3D.get());
  EXPECT_TRUE(pt2D->Is3D());

  auto ptM = std::make_unique<OGRPoint>(1.0, 2.0);
  ptM->setM(5.0);
  pt2D->HomogenizeDimensionalityWith(ptM.get());
  EXPECT_TRUE(pt2D->IsMeasured());

  // OGRGeomCoordinateBinaryPrecision
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.001;
  prec.dfZResolution = 0.01;
  prec.dfMResolution = 0.1;

  OGRGeomCoordinateBinaryPrecision binPrec;
  binPrec.SetFrom(prec);
  EXPECT_GT(binPrec.nXYBitPrecision, 0);
  EXPECT_GT(binPrec.nZBitPrecision, 0);
  EXPECT_GT(binPrec.nMBitPrecision, 0);

  // roundCoordinatesIEEE754
  auto ptRound = std::make_unique<OGRPoint>(1.234567, 8.765432, 5.555555);
  ptRound->setM(9.99999);
  ptRound->roundCoordinatesIEEE754(binPrec);
  EXPECT_NEAR(1.235, ptRound->getX(), 0.01);

  // OGRwkbExportOptions C API
  OGRwkbExportOptions* psOpts = OGRwkbExportOptionsCreate();
  ASSERT_THAT(psOpts, testing::NotNull());
  OGRwkbExportOptionsSetByteOrder(psOpts, wkbNDR);
  OGRwkbExportOptionsSetVariant(psOpts, wkbVariantIso);
  OGRwkbExportOptionsSetPrecision(psOpts, &prec);
  OGRwkbExportOptionsSetPrecision(psOpts, nullptr);
  OGRwkbExportOptionsDestroy(psOpts);

  // IsRectangle
  auto polyRect1 = std::make_unique<OGRPolygon>();
  auto ringRect1 = std::make_unique<OGRLinearRing>();
  ringRect1->addPoint(0, 0);
  ringRect1->addPoint(0, 10);
  ringRect1->addPoint(10, 10);
  ringRect1->addPoint(10, 0);
  ringRect1->addPoint(0, 0);
  polyRect1->addRingDirectly(ringRect1.release());
  EXPECT_TRUE(polyRect1->IsRectangle());

  auto polyRect2 = std::make_unique<OGRPolygon>();
  auto ringRect2 = std::make_unique<OGRLinearRing>();
  ringRect2->addPoint(0, 0);
  ringRect2->addPoint(10, 0);
  ringRect2->addPoint(10, 10);
  ringRect2->addPoint(0, 10);
  ringRect2->addPoint(0, 0);
  polyRect2->addRingDirectly(ringRect2.release());
  EXPECT_TRUE(polyRect2->IsRectangle());

  auto polyNonRect = std::make_unique<OGRPolygon>();
  auto ringNonRect = std::make_unique<OGRLinearRing>();
  ringNonRect->addPoint(0, 0);
  ringNonRect->addPoint(5, 10);
  ringNonRect->addPoint(10, 0);
  ringNonRect->addPoint(0, 0);
  polyNonRect->addRingDirectly(ringNonRect.release());
  EXPECT_FALSE(polyNonRect->IsRectangle());

  EXPECT_FALSE(pt2D->IsRectangle());

  // hasEmptyParts & removeEmptyParts
  EXPECT_FALSE(pt2D->hasEmptyParts());
  pt2D->removeEmptyParts();
}

TEST(OGRGeometryTest, AdditionalSpatialAndGEOSOperations) {
  // MakeValid and MakeValidEx
  auto bowTie = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(10, 0);
  ring->addPoint(0, 0);
  bowTie->addRingDirectly(ring.release());

  std::unique_ptr<OGRGeometry> valid(bowTie->MakeValid());
  ASSERT_THAT(valid, testing::NotNull());
  EXPECT_TRUE(valid->IsValid());

  std::unique_ptr<OGRGeometry> hValid(OGRGeometry::FromHandle(
      OGR_G_MakeValid(OGRGeometry::ToHandle(bowTie.get()))));
  ASSERT_THAT(hValid, testing::NotNull());

  char** papszMVOptions = nullptr;
  auto mv_cleaner =
      absl::MakeCleanup([&papszMVOptions] { CSLDestroy(papszMVOptions); });
  papszMVOptions = CSLSetNameValue(papszMVOptions, "METHOD", "STRUCTURED");
  papszMVOptions = CSLSetNameValue(papszMVOptions, "KEEP_COLLAPSED", "NO");
  std::unique_ptr<OGRGeometry> hValidEx(OGRGeometry::FromHandle(
      OGR_G_MakeValidEx(OGRGeometry::ToHandle(bowTie.get()), papszMVOptions)));
  ASSERT_THAT(hValidEx, testing::NotNull());

  // Normalize
  std::unique_ptr<OGRGeometry> norm(bowTie->Normalize());
  ASSERT_THAT(norm, testing::NotNull());
  OGRGeometryH hNorm = OGR_G_Normalize(OGRGeometry::ToHandle(bowTie.get()));
  ASSERT_THAT(hNorm, testing::NotNull());
  OGR_G_DestroyGeometry(hNorm);

  // ConcaveHull
  std::unique_ptr<OGRGeometry> concave(bowTie->ConcaveHull(0.5, FALSE));
  ASSERT_THAT(concave, testing::NotNull());
  OGRGeometryH hConcave =
      OGR_G_ConcaveHull(OGRGeometry::ToHandle(bowTie.get()), 0.5, FALSE);
  ASSERT_THAT(hConcave, testing::NotNull());
  OGR_G_DestroyGeometry(hConcave);

  // BufferEx
  char** papszBufOptions = nullptr;
  papszBufOptions = CSLSetNameValue(papszBufOptions, "QUADRANT_SEGMENTS", "8");
  papszBufOptions = CSLSetNameValue(papszBufOptions, "ENDCAP_STYLE", "FLAT");
  papszBufOptions = CSLSetNameValue(papszBufOptions, "JOIN_STYLE", "MITRE");
  papszBufOptions = CSLSetNameValue(papszBufOptions, "MITRE_LIMIT", "5.0");
  auto buf_cleaner =
      absl::MakeCleanup([&papszBufOptions] { CSLDestroy(papszBufOptions); });
  std::unique_ptr<OGRGeometry> bufEx(bowTie->BufferEx(1.0, papszBufOptions));
  ASSERT_THAT(bufEx, testing::NotNull());

  OGRGeometryH hBufEx =
      OGR_G_BufferEx(OGRGeometry::ToHandle(bowTie.get()), 1.0, papszBufOptions);
  ASSERT_THAT(hBufEx, testing::NotNull());
  OGR_G_DestroyGeometry(hBufEx);

  // UnionCascaded & UnaryUnion
  auto mp = std::make_unique<OGRMultiPolygon>();
  mp->addGeometry(valid.get());
  std::unique_ptr<OGRGeometry> unCasc(mp->UnionCascaded());
  ASSERT_THAT(unCasc, testing::NotNull());
  OGRGeometryH hUnCasc = OGR_G_UnionCascaded(OGRGeometry::ToHandle(mp.get()));
  ASSERT_THAT(hUnCasc, testing::NotNull());
  OGR_G_DestroyGeometry(hUnCasc);

  std::unique_ptr<OGRGeometry> unAry(mp->UnaryUnion());
  ASSERT_THAT(unAry, testing::NotNull());
  OGRGeometryH hUnAry = OGR_G_UnaryUnion(OGRGeometry::ToHandle(mp.get()));
  ASSERT_THAT(hUnAry, testing::NotNull());
  OGR_G_DestroyGeometry(hUnAry);

  // SetPrecision
  std::unique_ptr<OGRGeometry> precGeom(valid->SetPrecision(0.1, 0));
  ASSERT_THAT(precGeom, testing::NotNull());
  OGRGeometryH hPrec =
      OGR_G_SetPrecision(OGRGeometry::ToHandle(valid.get()), 0.1, 0);
  ASSERT_THAT(hPrec, testing::NotNull());
  OGR_G_DestroyGeometry(hPrec);

  // ConstrainedDelaunayTriangulation
  std::unique_ptr<OGRGeometry> cdt(valid->ConstrainedDelaunayTriangulation());
  ASSERT_THAT(cdt, testing::NotNull());
  OGRGeometryH hCdt = OGR_G_ConstrainedDelaunayTriangulation(
      OGRGeometry::ToHandle(valid.get()));
  ASSERT_THAT(hCdt, testing::NotNull());
  OGR_G_DestroyGeometry(hCdt);

  // BuildArea
  auto mls = std::make_unique<OGRMultiLineString>();
  auto ls1 = std::make_unique<OGRLineString>();
  ls1->addPoint(0, 0);
  ls1->addPoint(10, 0);
  ls1->addPoint(10, 10);
  ls1->addPoint(0, 10);
  ls1->addPoint(0, 0);
  mls->addGeometryDirectly(ls1.release());
  // BuildArea is disabled in Google modification
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, mls->BuildArea());
    EXPECT_EQ(nullptr, OGR_G_BuildArea(OGRGeometry::ToHandle(mls.get())));
  }

  // hasCurveGeometry, getLinearGeometry, getCurveGeometry
  EXPECT_FALSE(mls->hasCurveGeometry());
  EXPECT_FALSE(OGR_G_HasCurveGeometry(OGRGeometry::ToHandle(mls.get()), FALSE));

  std::unique_ptr<OGRGeometry> linGeom(mls->getLinearGeometry());
  ASSERT_THAT(linGeom, testing::NotNull());
  OGRGeometryH hLin =
      OGR_G_GetLinearGeometry(OGRGeometry::ToHandle(mls.get()), 0.0, nullptr);
  ASSERT_THAT(hLin, testing::NotNull());
  OGR_G_DestroyGeometry(hLin);

  std::unique_ptr<OGRGeometry> curvGeom(mls->getCurveGeometry());
  ASSERT_THAT(curvGeom, testing::NotNull());
  OGRGeometryH hCurv =
      OGR_G_GetCurveGeometry(OGRGeometry::ToHandle(mls.get()), nullptr);
  ASSERT_THAT(hCurv, testing::NotNull());
  OGR_G_DestroyGeometry(hCurv);

  // DB2 byte order flag
  OGRSetGenerate_DB2_V72_BYTE_ORDER(TRUE);
  EXPECT_TRUE(OGRGetGenerate_DB2_V72_BYTE_ORDER());
  OGRSetGenerate_DB2_V72_BYTE_ORDER(FALSE);
  EXPECT_FALSE(OGRGetGenerate_DB2_V72_BYTE_ORDER());

  // CastToError
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    OGRGeometry* castErr = OGRGeometry::CastToError(new OGRPoint(0, 0));
    EXPECT_EQ(nullptr, castErr);
  }
}

TEST(OGRGeometryTest, CopyMoveAndCAPIAdditional) {
  OGRPoint pt1(10.0, 20.0, 30.0);
  OGRPoint pt2(pt1);
  EXPECT_DOUBLE_EQ(10.0, pt2.getX());
  EXPECT_DOUBLE_EQ(20.0, pt2.getY());
  EXPECT_DOUBLE_EQ(30.0, pt2.getZ());

  OGRPoint pt3;
  pt3 = pt2;
  EXPECT_DOUBLE_EQ(10.0, pt3.getX());

  OGRPoint pt4(std::move(pt2));
  EXPECT_DOUBLE_EQ(10.0, pt4.getX());

  OGRPoint pt5;
  pt5 = std::move(pt3);
  EXPECT_DOUBLE_EQ(10.0, pt5.getX());

  // C API coordinate dimension and 3D / measured
  OGRGeometryH hPt = OGR_G_CreateGeometry(wkbPoint);
  OGR_G_SetCoordinateDimension(hPt, 3);
  EXPECT_EQ(3, OGR_G_GetCoordinateDimension(hPt));
  EXPECT_EQ(3, OGR_G_CoordinateDimension(hPt));

  OGR_G_Set3D(hPt, TRUE);
  EXPECT_TRUE(OGR_GT_HasZ(OGR_G_GetGeometryType(hPt)));
  OGR_G_Set3D(hPt, FALSE);
  EXPECT_FALSE(OGR_GT_HasZ(OGR_G_GetGeometryType(hPt)));

  OGR_G_SetMeasured(hPt, TRUE);
  EXPECT_TRUE(OGR_GT_HasM(OGR_G_GetGeometryType(hPt)));
  OGR_G_SetMeasured(hPt, FALSE);
  EXPECT_FALSE(OGR_GT_HasM(OGR_G_GetGeometryType(hPt)));

  // C API Transform and TransformTo
  auto srsWGS84 = new OGRSpatialReference();
  srsWGS84->importFromEPSG(4326);
  srsWGS84->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  auto srs3857 = new OGRSpatialReference();
  srs3857->importFromEPSG(3857);
  srs3857->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGR_G_AssignSpatialReference(hPt, OGRSpatialReference::ToHandle(srsWGS84));
  OGR_G_SetPoint_2D(hPt, 0, 2.35, 48.85);

  EXPECT_EQ(OGRERR_NONE,
            OGR_G_TransformTo(hPt, OGRSpatialReference::ToHandle(srs3857)));
  EXPECT_GT(OGR_G_GetX(hPt, 0), 10000.0);

  OGRCoordinateTransformation* ct =
      OGRCreateCoordinateTransformation(srs3857, srsWGS84);
  ASSERT_THAT(ct, testing::NotNull());
  EXPECT_EQ(
      OGRERR_NONE,
      OGR_G_Transform(hPt, reinterpret_cast<OGRCoordinateTransformationH>(ct)));
  EXPECT_NEAR(2.35, OGR_G_GetX(hPt, 0), 1e-2);
  OCTDestroyCoordinateTransformation(ct);

  srsWGS84->Release();
  srs3857->Release();

  // Null transform calls
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_TransformTo(nullptr, nullptr));
  EXPECT_EQ(OGRERR_FAILURE, OGR_G_Transform(nullptr, nullptr));

  // C API Envelope and Envelope3D
  OGREnvelope sEnv;
  OGR_G_GetEnvelope(hPt, &sEnv);
  EXPECT_NEAR(2.35, sEnv.MinX, 1e-2);
  OGREnvelope3D sEnv3D;
  OGR_G_GetEnvelope3D(hPt, &sEnv3D);
  EXPECT_NEAR(2.35, sEnv3D.MinX, 1e-2);

  OGR_G_GetEnvelope(nullptr, &sEnv);
  OGR_G_GetEnvelope3D(nullptr, &sEnv3D);

  OGR_G_DestroyGeometry(hPt);
}

TEST(OGRGeometryTest, CurveCollectionWktImport) {
  // OGRCompoundCurve importFromWkt
  auto cc = std::make_unique<OGRCompoundCurve>();
  const char* pszWktCC1 = "COMPOUNDCURVE ((0 0, 1 1))";
  EXPECT_EQ(OGRERR_NONE, cc->importFromWkt(&pszWktCC1));

  const char* pszWktCC2 =
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))";
  EXPECT_EQ(OGRERR_NONE, cc->importFromWkt(&pszWktCC2));

  const char* pszWktCC3 = "COMPOUNDCURVE EMPTY";
  EXPECT_EQ(OGRERR_NONE, cc->importFromWkt(&pszWktCC3));
  EXPECT_TRUE(cc->IsEmpty());

  const char* pszWktCC4 = "COMPOUNDCURVE (LINESTRING (0 0, 1 1))";
  EXPECT_EQ(OGRERR_NONE, cc->importFromWkt(&pszWktCC4));

  // Error cases in importFromWkt
  const char* pszWktErr1 = "COMPOUNDCURVE (POLYGON ((0 0, 1 0, 1 1, 0 0)))";
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_NE(OGRERR_NONE, cc->importFromWkt(&pszWktErr1));
  }

  // OGRMultiCurve importFromWkt
  auto mc = std::make_unique<OGRMultiCurve>();
  const char* pszWktMC1 = "MULTICURVE (EMPTY)";
  EXPECT_EQ(OGRERR_NONE, mc->importFromWkt(&pszWktMC1));

  const char* pszWktMC2 =
      "MULTICURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))";
  EXPECT_EQ(OGRERR_NONE, mc->importFromWkt(&pszWktMC2));

  const char* pszWktMC3 = "MULTICURVE Z ((0 0 1, 1 1 2))";
  EXPECT_EQ(OGRERR_NONE, mc->importFromWkt(&pszWktMC3));
  EXPECT_TRUE(mc->Is3D());

  const char* pszWktMC4 = "MULTICURVE M ((0 0 1, 1 1 2))";
  EXPECT_EQ(OGRERR_NONE, mc->importFromWkt(&pszWktMC4));
  EXPECT_TRUE(mc->IsMeasured());

  const char* pszWktMC5 = "MULTICURVE ZM ((0 0 1 2, 1 1 2 3))";
  EXPECT_EQ(OGRERR_NONE, mc->importFromWkt(&pszWktMC5));
  EXPECT_TRUE(mc->Is3D() && mc->IsMeasured());

  // OGRCurvePolygon importFromWkt with COMPOUNDCURVE
  auto cp = std::make_unique<OGRCurvePolygon>();
  const char* pszWktCP =
      "CURVEPOLYGON (COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 2 "
      "-1), (2 -1, 0 -1), (0 -1, 0 0)))";
  EXPECT_EQ(OGRERR_NONE, cp->importFromWkt(&pszWktCP));
  EXPECT_FALSE(cp->IsEmpty());
}

TEST(OGRGeometryTest, PreambleFromWktAndVariants) {
  auto pt = std::make_unique<OGRPoint>();

  const char* wkt1 = "POINT EMPTY";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt1));
  EXPECT_TRUE(pt->IsEmpty());

  const char* wkt2 = "POINT(EMPTY)";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt2));
  EXPECT_TRUE(pt->IsEmpty());

  const char* wkt3 = "POINT Z EMPTY";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt3));
  EXPECT_TRUE(pt->IsEmpty());
  EXPECT_TRUE(pt->Is3D());

  const char* wkt4 = "POINT M EMPTY";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt4));
  EXPECT_TRUE(pt->IsEmpty());
  EXPECT_TRUE(pt->IsMeasured());

  const char* wkt5 = "POINT ZM EMPTY";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt5));
  EXPECT_TRUE(pt->IsEmpty());
  EXPECT_TRUE(pt->Is3D() && pt->IsMeasured());

  const char* wkt6 = "POINTZ(1 2 3)";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt6));
  EXPECT_TRUE(pt->Is3D());

  const char* wkt7 = "POINTM(1 2 4)";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt7));
  EXPECT_TRUE(pt->IsMeasured());

  const char* wkt8 = "POINTZM(1 2 3 4)";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt8));
  EXPECT_TRUE(pt->Is3D() && pt->IsMeasured());

  const char* wkt9 = "POINT ( 5 6 )";
  EXPECT_EQ(OGRERR_NONE, pt->importFromWkt(&wkt9));
  EXPECT_DOUBLE_EQ(5.0, pt->getX());
  EXPECT_DOUBLE_EQ(6.0, pt->getY());

  // Corrupt WKT
  const char* wktCorrupt1 = "LINESTRING (0 0)";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, pt->importFromWkt(&wktCorrupt1));

  const char* wktCorrupt2 = "POINT INVALID";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, pt->importFromWkt(&wktCorrupt2));

  const char* wktCorrupt3 = "POINT (EMPTY INVALID)";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, pt->importFromWkt(&wktCorrupt3));
}

TEST(OGRGeometryTest, MakeValidAndGEOSConversions) {
  // MakeValid with METHOD=STRUCTURE and KEEP_COLLAPSED=YES
  auto bowTie = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(10, 0);
  ring->addPoint(0, 0);
  bowTie->addRingDirectly(ring.release());

  char** papszOpts = nullptr;
  papszOpts = CSLSetNameValue(papszOpts, "METHOD", "STRUCTURE");
  papszOpts = CSLSetNameValue(papszOpts, "KEEP_COLLAPSED", "YES");
  auto opts_cleaner =
      absl::MakeCleanup([&papszOpts] { CSLDestroy(papszOpts); });
  std::unique_ptr<OGRGeometry> validStruct(bowTie->MakeValid(papszOpts));
  ASSERT_THAT(validStruct, testing::NotNull());
  EXPECT_TRUE(validStruct->IsValid());

  // MakeValid on valid CurvePolygon
  auto cp = std::make_unique<OGRCurvePolygon>();
  auto csRing = std::make_unique<OGRCircularString>();
  csRing->addPoint(0, 0);
  csRing->addPoint(1, 1);
  csRing->addPoint(2, 0);
  csRing->addPoint(1, -1);
  csRing->addPoint(0, 0);
  cp->addRingDirectly(csRing.release());
  std::unique_ptr<OGRGeometry> validCP(cp->MakeValid());
  ASSERT_THAT(validCP, testing::NotNull());

  // IsRing on LineString
  auto lsRing = std::make_unique<OGRLineString>();
  lsRing->addPoint(0, 0);
  lsRing->addPoint(1, 0);
  lsRing->addPoint(1, 1);
  lsRing->addPoint(0, 1);
  lsRing->addPoint(0, 0);
  EXPECT_TRUE(lsRing->IsRing());
  EXPECT_TRUE(OGR_G_IsRing(OGRGeometry::ToHandle(lsRing.get())));

  auto lsOpen = std::make_unique<OGRLineString>();
  lsOpen->addPoint(0, 0);
  lsOpen->addPoint(1, 1);
  EXPECT_FALSE(lsOpen->IsRing());
  EXPECT_FALSE(OGR_G_IsRing(OGRGeometry::ToHandle(lsOpen.get())));

  // PolyhedralSurface and TIN exportToGEOS via Buffer
  auto poly = std::make_unique<OGRPolygon>();
  auto r = std::make_unique<OGRLinearRing>();
  r->addPoint(0, 0);
  r->addPoint(1, 0);
  r->addPoint(1, 1);
  r->addPoint(0, 0);
  poly->addRingDirectly(r.release());

  auto ps = std::make_unique<OGRPolyhedralSurface>();
  ps->addGeometry(poly.get());
  std::unique_ptr<OGRGeometry> psBuf(ps->Buffer(1.0));
  ASSERT_THAT(psBuf, testing::NotNull());

  auto tri = std::make_unique<OGRTriangle>(OGRPoint(0, 0), OGRPoint(1, 0),
                                           OGRPoint(0, 1));
  auto tin = std::make_unique<OGRTriangulatedSurface>();
  tin->addGeometry(tri.get());
  std::unique_ptr<OGRGeometry> tinBuf(tin->Buffer(1.0));
  ASSERT_THAT(tinBuf, testing::NotNull());

  // GeometryCollection containing PolyhedralSurface / Polygon
  auto gc = std::make_unique<OGRGeometryCollection>();
  gc->addGeometry(poly.get());
  std::unique_ptr<OGRGeometry> gcBuf(gc->Buffer(1.0));
  ASSERT_THAT(gcBuf, testing::NotNull());

  auto gcMixed = std::make_unique<OGRGeometryCollection>();
  auto pt = std::make_unique<OGRPoint>(0, 0);
  gcMixed->addGeometry(poly.get());
  gcMixed->addGeometry(pt.get());
  std::unique_ptr<OGRGeometry> gcMixedBuf(gcMixed->Buffer(1.0));
  ASSERT_THAT(gcMixedBuf, testing::NotNull());

  // Operations preserving SRS and rebuilding curves
  auto srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  auto srs_cleaner = absl::MakeCleanup([srs] { srs->Release(); });

  auto cs1 = std::make_unique<OGRCircularString>();
  cs1->addPoint(0, 0);
  cs1->addPoint(1, 1);
  cs1->addPoint(2, 0);
  cs1->assignSpatialReference(srs);

  auto cs2 = std::make_unique<OGRCircularString>();
  cs2->addPoint(0, 0);
  cs2->addPoint(1, -1);
  cs2->addPoint(2, 0);
  cs2->assignSpatialReference(srs);

  std::unique_ptr<OGRGeometry> csUnion(cs1->Union(cs2.get()));
  ASSERT_THAT(csUnion, testing::NotNull());
  EXPECT_NE(nullptr, csUnion->getSpatialReference());

  // Base OGRGeometry getLinearGeometry / getCurveGeometry
  OGRGeometry* geomBase = pt.get();
  std::unique_ptr<OGRGeometry> linClone(geomBase->getLinearGeometry());
  ASSERT_THAT(linClone, testing::NotNull());
  std::unique_ptr<OGRGeometry> curvClone(geomBase->getCurveGeometry());
  ASSERT_THAT(curvClone, testing::NotNull());

  // Base OGRGeometry operators
  OGRPoint ptA(1, 2);
  OGRPoint ptB(3, 4);
  OGRGeometry& refA = ptA;
  OGRGeometry& refB = ptB;
  refA = refB;
  EXPECT_DOUBLE_EQ(0.0, ptA.getX());
  refA = std::move(refB);
}

TEST(OGRGeometryTest, BufferExErrorHandling) {
  auto pt = std::make_unique<OGRPoint>(0, 0);

  // Invalid ENDCAP_STYLE
  char** opts = nullptr;
  opts = CSLSetNameValue(opts, "ENDCAP_STYLE", "INVALID");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pt->BufferEx(1.0, opts));
  }
  CSLDestroy(opts);

  // Invalid JOIN_STYLE
  opts = nullptr;
  opts = CSLSetNameValue(opts, "JOIN_STYLE", "INVALID");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pt->BufferEx(1.0, opts));
  }
  CSLDestroy(opts);

  // Invalid MITRE_LIMIT
  opts = nullptr;
  opts = CSLSetNameValue(opts, "MITRE_LIMIT", "NOT_A_NUMBER");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pt->BufferEx(1.0, opts));
  }
  CSLDestroy(opts);

  // Invalid QUADRANT_SEGMENTS
  opts = nullptr;
  opts = CSLSetNameValue(opts, "QUADRANT_SEGMENTS", "NOT_A_NUMBER");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pt->BufferEx(1.0, opts));
  }
  CSLDestroy(opts);

  // Unsupported option
  opts = nullptr;
  opts = CSLSetNameValue(opts, "UNKNOWN_OPTION", "VALUE");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pt->BufferEx(1.0, opts));
  }
  CSLDestroy(opts);
}

TEST(OGRGeometryTest, CompoundCurveSummaryDump) {
  auto cc = std::make_unique<OGRCompoundCurve>();
  auto ls1 = std::make_unique<OGRLineString>();
  ls1->addPoint(0, 0);
  ls1->addPoint(1, 0);
  cc->addCurveDirectly(ls1.release());

  auto ls2 = std::make_unique<OGRLineString>();
  ls2->addPoint(1, 0);
  ls2->addPoint(2, 0);
  cc->addCurveDirectly(ls2.release());

  char** opts = nullptr;
  opts = CSLSetNameValue(opts, "DISPLAY_GEOMETRY", "SUMMARY");
  auto opts_cleaner = absl::MakeCleanup([&opts] { CSLDestroy(opts); });

  std::string dump = cc->dumpReadable("", opts);
  EXPECT_THAT(
      dump, testing::HasSubstr("LINESTRING (2 points), LINESTRING (2 points)"));

  // CurvePolygon with compound curve ring in summary dump
  auto cp = std::make_unique<OGRCurvePolygon>();
  auto ccRing = std::make_unique<OGRCompoundCurve>();
  auto lsA = std::make_unique<OGRLineString>();
  lsA->addPoint(0, 0);
  lsA->addPoint(1, 0);
  ccRing->addCurveDirectly(lsA.release());
  auto lsB = std::make_unique<OGRLineString>();
  lsB->addPoint(1, 0);
  lsB->addPoint(1, 1);
  lsB->addPoint(0, 0);
  ccRing->addCurveDirectly(lsB.release());
  cp->addRingDirectly(ccRing.release());

  dump = cp->dumpReadable("", opts);
  EXPECT_THAT(dump, testing::HasSubstr("COMPOUNDCURVE"));
}

TEST(OGRGeometryTest, FinalCoverageBoost) {
  // IsSFCGALCompatible
  auto tri = std::make_unique<ExposeProtected<OGRTriangle>>(
      OGRPoint(0, 0), OGRPoint(1, 0), OGRPoint(0, 1));
  EXPECT_TRUE(tri->IsSFCGALCompatible());

  auto ps = std::make_unique<ExposeProtected<OGRPolyhedralSurface>>();
  EXPECT_TRUE(ps->IsSFCGALCompatible());

  auto tin = std::make_unique<ExposeProtected<OGRTriangulatedSurface>>();
  EXPECT_TRUE(tin->IsSFCGALCompatible());

  auto pt = std::make_unique<ExposeProtected<OGRPoint>>(1, 2);
  EXPECT_FALSE(pt->IsSFCGALCompatible());

  auto gcSFC = std::make_unique<ExposeProtected<OGRGeometryCollection>>();
  gcSFC->addGeometry(tin.get());
  EXPECT_TRUE(gcSFC->IsSFCGALCompatible());

  auto gcNonSFC = std::make_unique<ExposeProtected<OGRGeometryCollection>>();
  gcNonSFC->addGeometry(pt.get());
  EXPECT_FALSE(gcNonSFC->IsSFCGALCompatible());

  // Triangle exportToGEOS via Buffer
  std::unique_ptr<OGRGeometry> triBuf(tri->Buffer(1.0));
  ASSERT_THAT(triBuf, testing::NotNull());

  // C API Simplify, SimplifyPreserveTopology, DelaunayTriangulation, Intersect,
  // Equals, SymDifference
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(5, 1);
  ls->addPoint(10, 0);
  OGRGeometryH hLS = OGRGeometry::ToHandle(ls.get());

  OGRGeometryH hSimp = OGR_G_Simplify(hLS, 2.0);
  ASSERT_THAT(hSimp, testing::NotNull());
  OGR_G_DestroyGeometry(hSimp);

  OGRGeometryH hSimpPres = OGR_G_SimplifyPreserveTopology(hLS, 2.0);
  ASSERT_THAT(hSimpPres, testing::NotNull());
  OGR_G_DestroyGeometry(hSimpPres);

  OGRGeometryH hDT = OGR_G_DelaunayTriangulation(hLS, 0.0, TRUE);
  ASSERT_THAT(hDT, testing::NotNull());
  OGR_G_DestroyGeometry(hDT);

  EXPECT_TRUE(OGR_G_Intersect(hLS, hLS));
  EXPECT_TRUE(OGR_G_Equals(hLS, hLS));

  OGRGeometryH hSymDiff = OGR_G_SymDifference(hLS, hLS);
  ASSERT_THAT(hSymDiff, testing::NotNull());
  OGR_G_DestroyGeometry(hSymDiff);

  // OGR_G_PointOnSurface
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(10, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring.release());
  OGRGeometryH hPoly = OGRGeometry::ToHandle(poly.get());

  OGRGeometryH hPOS = OGR_G_PointOnSurface(hPoly);
  ASSERT_THAT(hPOS, testing::NotNull());
  OGR_G_DestroyGeometry(hPOS);

  // OGR_G_ExportToWkbEx
  OGRwkbExportOptions* psExportOpts = OGRwkbExportOptionsCreate();
  std::vector<GByte> wkbBuf(OGR_G_WkbSize(hPoly));
  EXPECT_EQ(OGRERR_NONE,
            OGR_G_ExportToWkbEx(hPoly, wkbBuf.data(), psExportOpts));
  EXPECT_EQ(OGRERR_FAILURE,
            OGR_G_ExportToWkbEx(nullptr, wkbBuf.data(), psExportOpts));
  OGRwkbExportOptionsDestroy(psExportOpts);

  // roundCoordinates with Z and M resolution
  auto ptZM = std::make_unique<OGRPoint>(1.2345, 2.3456, 3.4567);
  ptZM->setM(4.5678);
  OGRGeomCoordinatePrecision precZM;
  precZM.dfXYResolution = 0.01;
  precZM.dfZResolution = 0.01;
  precZM.dfMResolution = 0.01;
  ptZM->roundCoordinates(precZM);
  EXPECT_NEAR(1.23, ptZM->getX(), 1e-3);
  EXPECT_NEAR(3.46, ptZM->getZ(), 1e-3);
  EXPECT_NEAR(4.57, ptZM->getM(), 1e-3);

  // transformTo errors
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, ptZM->transformTo(nullptr));
    auto srs = new OGRSpatialReference();
    srs->importFromEPSG(4326);
    EXPECT_EQ(OGRERR_FAILURE, ptZM->transformTo(srs));
    srs->Release();
  }

  // MakeValid on already-valid geometry
  std::unique_ptr<OGRGeometry> mvValid(poly->MakeValid());
  ASSERT_THAT(mvValid, testing::NotNull());

  // BufferEx with out of range values
  char** badOpts = nullptr;
  badOpts = CSLSetNameValue(badOpts, "MITRE_LIMIT", "1e3000");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, ptZM->BufferEx(1.0, badOpts));
  }
  CSLDestroy(badOpts);

  badOpts = nullptr;
  badOpts = CSLSetNameValue(badOpts, "QUADRANT_SEGMENTS", "1e3000");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, ptZM->BufferEx(1.0, badOpts));
  }
  CSLDestroy(badOpts);

  // dumpReadable with empty compound curve and compound curve interior ring
  auto ccEmpty = std::make_unique<OGRCompoundCurve>();
  char** sumOpts = nullptr;
  sumOpts = CSLSetNameValue(sumOpts, "DISPLAY_GEOMETRY", "SUMMARY");
  auto sum_cleaner = absl::MakeCleanup([&sumOpts] { CSLDestroy(sumOpts); });
  std::string dumpCCEmpty = ccEmpty->dumpReadable("", sumOpts);
  EXPECT_THAT(dumpCCEmpty, testing::HasSubstr("empty"));

  auto cpWithHole = std::make_unique<OGRCurvePolygon>();
  auto ringExt = std::make_unique<OGRLineString>();
  ringExt->addPoint(0, 0);
  ringExt->addPoint(20, 0);
  ringExt->addPoint(20, 20);
  ringExt->addPoint(0, 20);
  ringExt->addPoint(0, 0);
  auto ccExt = std::make_unique<OGRCompoundCurve>();
  ccExt->addCurveDirectly(ringExt.release());
  cpWithHole->addRingDirectly(ccExt.release());

  auto ccHole = std::make_unique<OGRCompoundCurve>();
  auto lsHole = std::make_unique<OGRLineString>();
  lsHole->addPoint(5, 5);
  lsHole->addPoint(15, 5);
  lsHole->addPoint(15, 15);
  lsHole->addPoint(5, 15);
  lsHole->addPoint(5, 5);
  ccHole->addCurveDirectly(lsHole.release());
  cpWithHole->addRingDirectly(ccHole.release());

  std::string dumpHole = cpWithHole->dumpReadable("", sumOpts);
  EXPECT_THAT(dumpHole, testing::HasSubstr("inner rings"));
}

TEST(OGRGeometryTest, RemainingLinesCoverage) {
  auto pt = std::make_unique<OGRPoint>(1, 2);
  auto pt3D = std::make_unique<OGRPoint>(1, 2, 3);
  pt3D->setM(4);
  OGRGeometryH hPt = OGRGeometry::ToHandle(pt.get());
  OGRGeometryH hPt3D = OGRGeometry::ToHandle(pt3D.get());

  // OGR_G_Is3D, OGR_G_IsMeasured
  EXPECT_FALSE(OGR_G_Is3D(hPt));
  EXPECT_TRUE(OGR_G_Is3D(hPt3D));
  EXPECT_FALSE(OGR_G_IsMeasured(hPt));
  EXPECT_TRUE(OGR_G_IsMeasured(hPt3D));
  EXPECT_FALSE(OGR_G_Is3D(nullptr));
  EXPECT_FALSE(OGR_G_IsMeasured(nullptr));

  // OGR_G_Equal
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(OGR_G_Equal(nullptr, hPt));
    EXPECT_FALSE(OGR_G_Equal(hPt, nullptr));
  }
  EXPECT_TRUE(OGR_G_Equal(hPt, hPt));

  // OGR_G_Distance and Distance3D
  EXPECT_DOUBLE_EQ(0.0, OGR_G_Distance(hPt, hPt));
  EXPECT_DOUBLE_EQ(-1.0, OGR_G_Distance3D(hPt, hPt));

  // OGR_G_Boundary
  OGRGeometryH hBnd = OGR_G_Boundary(hPt);
  ASSERT_THAT(hBnd, testing::NotNull());
  OGR_G_DestroyGeometry(hBnd);

  // OGR_G_Intersects
  EXPECT_TRUE(OGR_G_Intersects(hPt, hPt));

  // OGR_G_ImportFromWkb & OGR_G_ImportFromWkt
  auto ptImport = std::make_unique<OGRPoint>();
  OGRGeometryH hImport = OGRGeometry::ToHandle(ptImport.get());
  std::vector<GByte> wkb(pt->WkbSize());
  pt->exportToWkb(wkb.data());
  EXPECT_EQ(OGRERR_NONE, OGR_G_ImportFromWkb(hImport, wkb.data(),
                                             static_cast<int>(wkb.size())));
  char szWkt[] = "POINT (7 8)";
  char* pszWkt = szWkt;
  EXPECT_EQ(OGRERR_NONE, OGR_G_ImportFromWkt(hImport, &pszWkt));
  EXPECT_DOUBLE_EQ(7.0, ptImport->getX());

  // SFCGAL not enabled error branches via IsSFCGALCompatible
  auto tri = std::make_unique<OGRTriangle>(OGRPoint(0, 0), OGRPoint(1, 0),
                                           OGRPoint(0, 1));
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_DOUBLE_EQ(-1.0, tri->Distance(pt.get()));
    EXPECT_DOUBLE_EQ(-1.0, tri->Distance3D(pt.get()));
    EXPECT_EQ(nullptr, tri->ConvexHull());
  }

  // MakeValid on SFCGAL compatible valid geometry
  std::unique_ptr<OGRGeometry> triValid(tri->MakeValid());
  ASSERT_THAT(triValid, testing::NotNull());

  // MakeValid on MultiPolygon with METHOD=STRUCTURE resulting in MultiPolygon
  auto mp = std::make_unique<OGRMultiPolygon>();
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0, 0);
  ring->addPoint(10, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->addPoint(0, 0);
  poly->addRingDirectly(ring.release());
  mp->addGeometry(poly.get());

  char** papszOpts = nullptr;
  papszOpts = CSLSetNameValue(papszOpts, "METHOD", "STRUCTURE");
  auto opts_cleaner =
      absl::MakeCleanup([&papszOpts] { CSLDestroy(papszOpts); });
  std::unique_ptr<OGRGeometry> mpValid(mp->MakeValid(papszOpts));
  ASSERT_THAT(mpValid, testing::NotNull());
  EXPECT_EQ(wkbMultiPolygon, mpValid->getGeometryType());

  // hasEmptyParts on MultiPolygon with empty part
  auto mpEmpty = std::make_unique<OGRMultiPolygon>();
  mpEmpty->addGeometryDirectly(new OGRPolygon());
  EXPECT_TRUE(mpEmpty->hasEmptyParts());
  std::unique_ptr<OGRGeometry> mpBuf(mpEmpty->Buffer(1.0));
  ASSERT_THAT(mpBuf, testing::NotNull());
}

}  // namespace
}  // namespace autotest2
