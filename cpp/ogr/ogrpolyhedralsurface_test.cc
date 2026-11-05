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

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class DummyCT : public OGRCoordinateTransformation {
 public:
  const OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  const OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  int Transform(size_t nCount, double* x, double* y, double* z, double* t,
                int* pabSuccess) override {
    if (pabSuccess) {
      for (size_t i = 0; i < nCount; ++i) pabSuccess[i] = TRUE;
    }
    return TRUE;
  }
  OGRCoordinateTransformation* Clone() const override { return new DummyCT(); }
  OGRCoordinateTransformation* GetInverse() const override {
    return new DummyCT();
  }
};

class TestPolyhedralSurface : public OGRPolyhedralSurface {
 public:
  using OGRPolyhedralSurface::CastToMultiPolygonImpl;
  using OGRPolyhedralSurface::GetCasterToCurvePolygon;
  using OGRPolyhedralSurface::GetCasterToMultiPolygon;
  using OGRPolyhedralSurface::GetCasterToPolygon;
  using OGRPolyhedralSurface::getSubGeometryName;
  using OGRPolyhedralSurface::getSubGeometryType;
  using OGRPolyhedralSurface::isCompatibleSubType;
};

std::unique_ptr<OGRPolygon> CreateSquarePolygon(double x0, double y0,
                                                double size) {
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(x0, y0);
  ring->addPoint(x0 + size, y0);
  ring->addPoint(x0 + size, y0 + size);
  ring->addPoint(x0, y0 + size);
  ring->addPoint(x0, y0);
  poly->addRingDirectly(ring.release());
  return poly;
}

std::unique_ptr<OGRPolygon> CreateSquarePolygon3D(double x0, double y0,
                                                  double z, double size) {
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(x0, y0, z);
  ring->addPoint(x0 + size, y0, z);
  ring->addPoint(x0 + size, y0 + size, z);
  ring->addPoint(x0, y0 + size, z);
  ring->addPoint(x0, y0, z);
  poly->addRingDirectly(ring.release());
  return poly;
}

std::unique_ptr<OGRPolygon> CreateSquarePolygonZM(double x0, double y0,
                                                  double z, double m,
                                                  double size) {
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(x0, y0, z, m);
  ring->addPoint(x0 + size, y0, z, m);
  ring->addPoint(x0 + size, y0 + size, z, m);
  ring->addPoint(x0, y0 + size, z, m);
  ring->addPoint(x0, y0, z, m);
  poly->addRingDirectly(ring.release());
  return poly;
}

TEST(OgrPolyhedralSurfaceTest, BasicPropertiesAndConstruction) {
  OGRPolyhedralSurface ps;
  EXPECT_TRUE(ps.IsEmpty());
  EXPECT_EQ(ps.getDimension(), 2);
  EXPECT_STREQ(ps.getGeometryName(), "POLYHEDRALSURFACE");
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurface);
  EXPECT_FALSE(ps.hasCurveGeometry());
  EXPECT_EQ(ps.getNumGeometries(), 0);
  EXPECT_EQ(ps.WkbSize(), 9);

  OGREnvelope env;
  ps.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(env.MinX, 0);
  EXPECT_DOUBLE_EQ(env.MaxX, 0);
  EXPECT_DOUBLE_EQ(env.MinY, 0);
  EXPECT_DOUBLE_EQ(env.MaxY, 0);

  OGREnvelope3D env3d;
  ps.getEnvelope(&env3d);
  EXPECT_DOUBLE_EQ(env3d.MinX, 0);
  EXPECT_DOUBLE_EQ(env3d.MaxX, 0);
  EXPECT_DOUBLE_EQ(env3d.MinY, 0);
  EXPECT_DOUBLE_EQ(env3d.MaxY, 0);
  EXPECT_DOUBLE_EQ(env3d.MinZ, 0);
  EXPECT_DOUBLE_EQ(env3d.MaxZ, 0);

  auto poly = CreateSquarePolygon(0, 0, 10);
  EXPECT_EQ(ps.addGeometry(poly.get()), OGRERR_NONE);
  EXPECT_FALSE(ps.IsEmpty());
  EXPECT_EQ(ps.getNumGeometries(), 1);
  EXPECT_GT(ps.WkbSize(), 9);

  ps.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(env.MinX, 0);
  EXPECT_DOUBLE_EQ(env.MaxX, 10);
  EXPECT_DOUBLE_EQ(env.MinY, 0);
  EXPECT_DOUBLE_EQ(env.MaxY, 10);

  // Clone
  std::unique_ptr<OGRPolyhedralSurface> cloned(ps.clone());
  ASSERT_NE(cloned, nullptr);
  EXPECT_TRUE(cloned->Equals(&ps));

  // Copy constructor
  OGRPolyhedralSurface copy_constructed(ps);
  EXPECT_TRUE(copy_constructed.Equals(&ps));

  // Move constructor
  OGRPolyhedralSurface move_constructed(std::move(copy_constructed));
  EXPECT_TRUE(move_constructed.Equals(&ps));

  // Copy assignment & self assignment
  OGRPolyhedralSurface assigned;
  assigned = ps;
  EXPECT_TRUE(assigned.Equals(&ps));
  assigned = assigned;
  EXPECT_TRUE(assigned.Equals(&ps));

  // Move assignment
  OGRPolyhedralSurface move_assigned;
  move_assigned = std::move(assigned);
  EXPECT_TRUE(move_assigned.Equals(&ps));

  // Empty
  ps.empty();
  EXPECT_TRUE(ps.IsEmpty());
  EXPECT_EQ(ps.getNumGeometries(), 0);
}

TEST(OgrPolyhedralSurfaceTest, DimensionAndGeometryTypes) {
  OGRPolyhedralSurface ps;
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurface);

  ps.set3D(TRUE);
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurfaceZ);

  ps.setMeasured(TRUE);
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurfaceZM);

  ps.set3D(FALSE);
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurfaceM);

  ps.setMeasured(FALSE);
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurface);

  // setCoordinateDimension
  ps.setCoordinateDimension(3);
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurfaceZ);
  ps.setCoordinateDimension(2);
  EXPECT_EQ(ps.getGeometryType(), wkbPolyhedralSurface);
}

TEST(OgrPolyhedralSurfaceTest, GeometryManagementAndIteration) {
  OGRPolyhedralSurface ps;

  // Add invalid geometry type (e.g. OGRPoint)
  OGRPoint pt(1, 2);
  EXPECT_EQ(ps.addGeometry(&pt), OGRERR_UNSUPPORTED_GEOMETRY_TYPE);

  auto pt_unowned = new OGRPoint(1, 2);
  EXPECT_EQ(ps.addGeometryDirectly(pt_unowned),
            OGRERR_UNSUPPORTED_GEOMETRY_TYPE);
  delete pt_unowned;

  EXPECT_EQ(ps.addGeometry(std::make_unique<OGRPoint>(1, 2)),
            OGRERR_UNSUPPORTED_GEOMETRY_TYPE);

  // Add valid geometries
  auto p1 = CreateSquarePolygon(0, 0, 10);
  auto p2 = CreateSquarePolygon(10, 0, 10);
  EXPECT_EQ(ps.addGeometryDirectly(p1.release()), OGRERR_NONE);
  EXPECT_EQ(ps.addGeometry(std::move(p2)), OGRERR_NONE);
  EXPECT_EQ(ps.getNumGeometries(), 2);

  // getGeometryRef non-const and const
  EXPECT_NE(ps.getGeometryRef(0), nullptr);
  const OGRPolyhedralSurface& const_ps = ps;
  EXPECT_NE(const_ps.getGeometryRef(1), nullptr);

  // Range-based for loop
  int count = 0;
  for (auto* poly : ps) {
    ASSERT_NE(poly, nullptr);
    count++;
  }
  EXPECT_EQ(count, 2);

  count = 0;
  for (const auto* poly : const_ps) {
    ASSERT_NE(poly, nullptr);
    count++;
  }
  EXPECT_EQ(count, 2);

  // hasEmptyParts and removeEmptyParts
  EXPECT_FALSE(ps.hasEmptyParts());
  ps.addGeometryDirectly(new OGRPolygon());
  EXPECT_EQ(ps.getNumGeometries(), 3);
  EXPECT_TRUE(ps.hasEmptyParts());
  ps.removeEmptyParts();
  EXPECT_EQ(ps.getNumGeometries(), 2);
  EXPECT_FALSE(ps.hasEmptyParts());

  // removeGeometry
  EXPECT_EQ(ps.removeGeometry(0, TRUE), OGRERR_NONE);
  EXPECT_EQ(ps.getNumGeometries(), 1);
  EXPECT_EQ(ps.removeGeometry(-1, TRUE), OGRERR_NONE);
  EXPECT_EQ(ps.getNumGeometries(), 0);
  EXPECT_EQ(ps.removeGeometry(5, TRUE), OGRERR_FAILURE);
}

TEST(OgrPolyhedralSurfaceTest, WkbImportExport) {
  OGRPolyhedralSurface ps;
  ps.addGeometryDirectly(CreateSquarePolygon(0, 0, 5).release());
  ps.addGeometryDirectly(CreateSquarePolygon(5, 5, 5).release());

  size_t wkb_size = ps.WkbSize();
  std::vector<unsigned char> buffer(wkb_size);
  EXPECT_EQ(ps.exportToWkb(buffer.data()), OGRERR_NONE);

  OGRPolyhedralSurface imported;
  size_t bytes_consumed = 0;
  EXPECT_EQ(imported.importFromWkb(buffer.data(), buffer.size(), wkbVariantIso,
                                   bytes_consumed),
            OGRERR_NONE);
  EXPECT_EQ(bytes_consumed, wkb_size);
  EXPECT_TRUE(imported.Equals(&ps));

  // Export with explicit options
  OGRwkbExportOptions options;
  options.eByteOrder = wkbNDR;
  EXPECT_EQ(ps.exportToWkb(buffer.data(), &options), OGRERR_NONE);
  imported.empty();
  EXPECT_EQ(imported.importFromWkb(buffer.data(), buffer.size(), wkbVariantIso,
                                   bytes_consumed),
            OGRERR_NONE);
  EXPECT_TRUE(imported.Equals(&ps));

  // 3D PolyhedralSurface WKB
  OGRPolyhedralSurface ps3d;
  ps3d.addGeometryDirectly(CreateSquarePolygon3D(0, 0, 1, 5).release());
  std::vector<unsigned char> buf3d(ps3d.WkbSize());
  EXPECT_EQ(ps3d.exportToWkb(buf3d.data()), OGRERR_NONE);
  OGRPolyhedralSurface imported3d;
  EXPECT_EQ(imported3d.importFromWkb(buf3d.data(), buf3d.size(), wkbVariantIso,
                                     bytes_consumed),
            OGRERR_NONE);
  EXPECT_TRUE(imported3d.Is3D());
  EXPECT_TRUE(imported3d.Equals(&ps3d));

  // WKB Errors
  OGRPolyhedralSurface err_ps;
  // Truncated buffer
  EXPECT_NE(
      err_ps.importFromWkb(buffer.data(), 4, wkbVariantIso, bytes_consumed),
      OGRERR_NONE);
  // Truncated subgeometry (<9 bytes remaining)
  EXPECT_EQ(
      err_ps.importFromWkb(buffer.data(), 10, wkbVariantIso, bytes_consumed),
      OGRERR_NOT_ENOUGH_DATA);
}

TEST(OgrPolyhedralSurfaceTest, WktImportExport) {
  OGRPolyhedralSurface ps;
  EXPECT_EQ(ps.exportToWkt(), "POLYHEDRALSURFACE EMPTY");

  ps.addGeometryDirectly(CreateSquarePolygon(0, 0, 1).release());
  std::string wkt = ps.exportToWkt();
  EXPECT_THAT(wkt, HasSubstr("POLYHEDRALSURFACE (((0 0,1 0,1 1,0 1,0 0)))"));

  OGRPolyhedralSurface imported;
  const char* pszWkt = wkt.c_str();
  EXPECT_EQ(imported.importFromWkt(&pszWkt), OGRERR_NONE);
  EXPECT_TRUE(imported.Equals(&ps));

  // 3D and ZM WKT
  OGRPolyhedralSurface ps_zm;
  ps_zm.addGeometryDirectly(CreateSquarePolygonZM(0, 0, 5, 10, 1).release());
  std::string wkt_zm = ps_zm.exportToWkt();
  EXPECT_THAT(wkt_zm, HasSubstr("POLYHEDRALSURFACE ZM"));
  const char* pszWktZM = wkt_zm.c_str();
  OGRPolyhedralSurface imported_zm;
  EXPECT_EQ(imported_zm.importFromWkt(&pszWktZM), OGRERR_NONE);
  EXPECT_TRUE(imported_zm.Is3D());
  EXPECT_TRUE(imported_zm.IsMeasured());

  // WKT with empty child polygon (should be skipped in exportToWkt)
  OGRPolyhedralSurface ps_with_empty;
  ps_with_empty.addGeometryDirectly(new OGRPolygon());
  EXPECT_EQ(ps_with_empty.exportToWkt(), "POLYHEDRALSURFACE EMPTY");

  // WKT errors
  {
    WithQuietHandler quiet_handler;
    const char* bad_token = "POLYHEDRALSURFACE (POINT (0 0))";
    EXPECT_NE(imported.importFromWkt(&bad_token), OGRERR_NONE);

    const char* bad_geom = "POLYHEDRALSURFACE (((0 0,1 0,1 1,0 1,0 0)) FOO";
    EXPECT_NE(imported.importFromWkt(&bad_geom), OGRERR_NONE);
  }
}

TEST(OgrPolyhedralSurfaceTest, TransformationsAndCalculations) {
  OGRPolyhedralSurface ps;
  ps.addGeometryDirectly(CreateSquarePolygon3D(0, 0, 10, 5).release());

  EXPECT_TRUE(ps.Is3D());
  ps.flattenTo2D();
  EXPECT_FALSE(ps.Is3D());

  ps.swapXY();
  OGREnvelope env;
  ps.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(env.MinX, 0);
  EXPECT_DOUBLE_EQ(env.MaxX, 5);

  // assignSpatialReference
  auto srs = std::make_unique<OGRSpatialReference>();
  ps.assignSpatialReference(srs.get());
  EXPECT_EQ(ps.getSpatialReference(), srs.get());
  EXPECT_EQ(ps.getGeometryRef(0)->getSpatialReference(), srs.get());
  ps.assignSpatialReference(nullptr);
  srs.reset();
  OSRCleanup();

  // transform
  DummyCT dummy_ct;
  EXPECT_EQ(ps.transform(&dummy_ct), OGRERR_NONE);

  // PointOnSurface requires a non-empty point to populate
  OGRPoint pt(0.0, 0.0);
  EXPECT_EQ(ps.PointOnSurface(&pt), OGRERR_NONE);
  EXPECT_FALSE(pt.IsEmpty());

  // Calculations: get_Area, get_GeodesicArea, get_Length, get_GeodesicLength
  {
    WithQuietHandler quiet_handler;
    ps.get_Area();
    EXPECT_EQ(ps.get_GeodesicArea(nullptr), -1.0);
    EXPECT_DOUBLE_EQ(ps.get_Length(), 0.0);
    EXPECT_EQ(ps.get_GeodesicLength(nullptr), -1.0);

    OGRPolyhedralSurface empty_ps;
    EXPECT_DOUBLE_EQ(empty_ps.get_GeodesicArea(nullptr), 0.0);
    EXPECT_DOUBLE_EQ(empty_ps.get_Length(), 0.0);
    EXPECT_DOUBLE_EQ(empty_ps.get_GeodesicLength(nullptr), 0.0);
  }
}

TEST(OgrPolyhedralSurfaceTest, EqualsComparison) {
  OGRPolyhedralSurface ps1;
  OGRPolyhedralSurface ps2;

  EXPECT_TRUE(ps1.Equals(&ps1));
  EXPECT_TRUE(ps1.Equals(&ps2));

  OGRPoint pt(0, 0);
  EXPECT_FALSE(ps1.Equals(&pt));

  ps1.addGeometryDirectly(CreateSquarePolygon(0, 0, 10).release());
  EXPECT_FALSE(ps1.Equals(&ps2));

  ps2.addGeometryDirectly(CreateSquarePolygon(0, 0, 10).release());
  EXPECT_TRUE(ps1.Equals(&ps2));

  ps2.empty();
  ps2.addGeometryDirectly(CreateSquarePolygon(1, 1, 10).release());
  EXPECT_FALSE(ps1.Equals(&ps2));
}

TEST(OgrPolyhedralSurfaceTest, CastersAndSubGeometryProperties) {
  TestPolyhedralSurface ps;
  EXPECT_TRUE(ps.isCompatibleSubType(wkbPolygon));
  EXPECT_FALSE(ps.isCompatibleSubType(wkbLineString));
  EXPECT_STREQ(ps.getSubGeometryName(), "POLYGON");
  EXPECT_EQ(ps.getSubGeometryType(), wkbPolygon);

  // CastToMultiPolygon
  auto ps_to_cast = std::make_unique<OGRPolyhedralSurface>();
  ps_to_cast->addGeometryDirectly(CreateSquarePolygon(0, 0, 1).release());
  OGRMultiPolygon* mp =
      OGRPolyhedralSurface::CastToMultiPolygon(ps_to_cast.release());
  ASSERT_NE(mp, nullptr);
  EXPECT_EQ(mp->getNumGeometries(), 1);
  delete mp;

  // GetCasterToPolygon and GetCasterToCurvePolygon (both return caster
  // functions that fail and delete input)
  {
    WithQuietHandler quiet_handler;
    auto caster_to_poly = ps.GetCasterToPolygon();
    EXPECT_EQ(caster_to_poly(new OGRPolyhedralSurface()), nullptr);

    auto caster_to_curve_poly = ps.GetCasterToCurvePolygon();
    EXPECT_EQ(caster_to_curve_poly(new OGRPolyhedralSurface()), nullptr);
  }
}

}  // namespace
}  // namespace autotest2
