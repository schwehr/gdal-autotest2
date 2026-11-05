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

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestCurvePolygon : public OGRCurvePolygon {
 public:
  using OGRCurvePolygon::CastToPolygon;
  using OGRCurvePolygon::GetCasterToCurvePolygon;
  using OGRCurvePolygon::GetCasterToPolygon;
};

class SubGeometryVisitor : public OGRDefaultGeometryVisitor {
 public:
  bool visited = false;
  void visit(OGRCurvePolygon*) override { visited = true; }
};

class SubConstGeometryVisitor : public OGRDefaultConstGeometryVisitor {
 public:
  bool visited = false;
  void visit(const OGRCurvePolygon*) override { visited = true; }
};

std::unique_ptr<OGRCurvePolygon> FromWkt(const char* wkt) {
  if (wkt == nullptr) return nullptr;
  auto cp = std::make_unique<OGRCurvePolygon>();
  std::unique_ptr<char, autotest2::FreeDeleter> str(strdup(wkt));
  char* ptr = str.get();
  if (cp->importFromWkt(&ptr) != OGRERR_NONE) {
    return nullptr;
  }
  return cp;
}

std::unique_ptr<OGRLineString> MakeClosedLine(
    const std::initializer_list<std::pair<double, double>>& pts) {
  auto ls = std::make_unique<OGRLineString>();
  for (const auto& p : pts) {
    ls->addPoint(p.first, p.second);
  }
  return ls;
}

TEST(OgrCurvePolygonTest, BasicPropertiesAndConstructors) {
  OGRCurvePolygon cp;
  EXPECT_STREQ("CURVEPOLYGON", cp.getGeometryName());
  EXPECT_EQ(wkbCurvePolygon, cp.getGeometryType());
  EXPECT_EQ(2, cp.getDimension());
  EXPECT_TRUE(cp.IsEmpty());
  EXPECT_EQ(0, cp.getNumInteriorRings());
  EXPECT_EQ(nullptr, cp.getExteriorRingCurve());
  EXPECT_EQ(nullptr, std::as_const(cp).getExteriorRingCurve());

  // Coordinate dimensions
  cp.set3D(TRUE);
  EXPECT_EQ(wkbCurvePolygonZ, cp.getGeometryType());

  cp.set3D(FALSE);
  cp.setMeasured(TRUE);
  EXPECT_EQ(wkbCurvePolygonM, cp.getGeometryType());

  cp.set3D(TRUE);
  EXPECT_EQ(wkbCurvePolygonZM, cp.getGeometryType());

  cp.flattenTo2D();
  EXPECT_EQ(wkbCurvePolygon, cp.getGeometryType());

  // Add outer ring
  EXPECT_EQ(OGRERR_NONE,
            cp.addRingDirectly(
                MakeClosedLine({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}})
                    .release()));
  EXPECT_FALSE(cp.IsEmpty());
  EXPECT_THAT(cp.getExteriorRingCurve(), testing::NotNull());
  EXPECT_THAT(std::as_const(cp).getExteriorRingCurve(), testing::NotNull());
  EXPECT_EQ(0, cp.getNumInteriorRings());

  // Copy constructor
  OGRCurvePolygon cp_copy(cp);
  EXPECT_TRUE(cp.Equals(&cp_copy));

  // Move constructor
  OGRCurvePolygon cp_moved(std::move(cp_copy));
  EXPECT_TRUE(cp.Equals(&cp_moved));

  // Copy assignment
  OGRCurvePolygon cp_assigned;
  cp_assigned = cp;
  EXPECT_TRUE(cp.Equals(&cp_assigned));

  // Self assignment
  const auto* cp_assigned_ptr = &cp_assigned;
  cp_assigned = *cp_assigned_ptr;
  EXPECT_TRUE(cp.Equals(&cp_assigned));

  // Move assignment
  OGRCurvePolygon cp_move_assigned;
  cp_move_assigned = std::move(cp_moved);
  EXPECT_TRUE(cp.Equals(&cp_move_assigned));

  // Incompatible assignment: assigning OGRPolygon containing OGRLinearRing
  {
    OGRPolygon poly;
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(10, 0);
    ring->addPoint(10, 10);
    ring->addPoint(0, 0);
    poly.addRingDirectly(ring.release());

    OGRCurvePolygon cp_bad;
    WithQuietHandler quiet;
    cp_bad = poly;  // triggers incompatible sub-geometry error
    EXPECT_TRUE(cp_bad.IsEmpty());
  }

  // Clone
  std::unique_ptr<OGRCurvePolygon> cloned(cp.clone());
  ASSERT_THAT(cloned, testing::NotNull());
  EXPECT_TRUE(cp.Equals(cloned.get()));

  // Visitor
  SubGeometryVisitor v;
  cp.accept(&v);
  EXPECT_TRUE(v.visited);

  SubConstGeometryVisitor cv;
  std::as_const(cp).accept(&cv);
  EXPECT_TRUE(cv.visited);
}

TEST(OgrCurvePolygonTest, RingValidationAndManagement) {
  OGRCurvePolygon cp;

  // Linear ring is rejected
  {
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(10, 0);
    ring->addPoint(10, 10);
    ring->addPoint(0, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE, cp.addRing(ring.get()));
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE, cp.addRing(std::move(ring)));
  }

  // LineString with < 4 points is rejected
  {
    auto short_ls = MakeClosedLine({{0, 0}, {1, 0}, {0, 0}});
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
              cp.addRing(std::move(short_ls)));
  }

  // Unclosed ring with OGR_GEOMETRY_ACCEPT_UNCLOSED_RING = NO
  {
    CPLSetConfigOption("OGR_GEOMETRY_ACCEPT_UNCLOSED_RING", "NO");
    auto unclosed_ls = MakeClosedLine({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE, cp.addRing(unclosed_ls.get()));
    CPLSetConfigOption("OGR_GEOMETRY_ACCEPT_UNCLOSED_RING", nullptr);
  }

  // Unclosed ring with default (warning only, auto-closed by
  // HomogenizeDimensionalityWith / addCurveDirectly)
  {
    WithQuietHandler quiet;
    auto unclosed_ls = MakeClosedLine({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    EXPECT_EQ(OGRERR_NONE, cp.addRing(unclosed_ls.get()));
    EXPECT_FALSE(cp.IsEmpty());
  }

  cp.empty();
  EXPECT_TRUE(cp.IsEmpty());

  // Add outer ring and interior rings
  EXPECT_EQ(OGRERR_NONE,
            cp.addRingDirectly(
                MakeClosedLine({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}})
                    .release()));
  EXPECT_EQ(
      OGRERR_NONE,
      cp.addRingDirectly(
          MakeClosedLine({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}).release()));
  EXPECT_EQ(
      OGRERR_NONE,
      cp.addRingDirectly(
          MakeClosedLine({{3, 3}, {4, 3}, {4, 4}, {3, 4}, {3, 3}}).release()));

  EXPECT_EQ(2, cp.getNumInteriorRings());
  EXPECT_THAT(cp.getInteriorRingCurve(0), testing::NotNull());
  EXPECT_THAT(std::as_const(cp).getInteriorRingCurve(0), testing::NotNull());
  EXPECT_THAT(cp.getInteriorRingCurve(1), testing::NotNull());
  EXPECT_EQ(nullptr, cp.getInteriorRingCurve(2));

  // Steal exterior ring
  {
    OGRCurvePolygon cp_for_steal;
    EXPECT_EQ(nullptr, cp_for_steal.stealExteriorRingCurve());

    cp_for_steal.addRingDirectly(
        MakeClosedLine({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}).release());
    OGRCurve* stolen = cp_for_steal.stealExteriorRingCurve();
    ASSERT_THAT(stolen, testing::NotNull());
    delete stolen;
  }

  // Remove ring
  EXPECT_EQ(OGRERR_NONE, cp.removeRing(1, true));
  EXPECT_EQ(1, cp.getNumInteriorRings());
  EXPECT_EQ(OGRERR_FAILURE, cp.removeRing(10, true));
  EXPECT_EQ(OGRERR_NONE, cp.removeRing(-1, true));
  EXPECT_TRUE(cp.IsEmpty());
}

TEST(OgrCurvePolygonTest, WkbExportAndImport) {
  auto cp = FromWkt(
      "CURVEPOLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))");
  ASSERT_THAT(cp, testing::NotNull());

  // Default export
  std::vector<unsigned char> wkb(cp->WkbSize());
  EXPECT_EQ(OGRERR_NONE, cp->exportToWkb(wkb.data(), nullptr));

  // Import into new curve polygon
  OGRCurvePolygon cp_imp;
  size_t nConsumed = 0;
  EXPECT_EQ(OGRERR_NONE, cp_imp.importFromWkb(wkb.data(), wkb.size(),
                                              wkbVariantIso, nConsumed));
  EXPECT_TRUE(cp->Equals(&cp_imp));
  EXPECT_EQ(wkb.size(), nConsumed);

  // Export with wkbVariantOldOgc (automatically patched to wkbVariantIso)
  OGRwkbExportOptions opt_old_ogc(wkbNDR, wkbVariantOldOgc);
  EXPECT_EQ(OGRERR_NONE, cp->exportToWkb(wkb.data(), &opt_old_ogc));

  // Corrupt WKB
  {
    unsigned char bad_wkb[] = {0, 0, 0, 0, 10, 0, 0, 0, 1};
    OGRCurvePolygon cp_bad;
    size_t nBadConsumed = 0;
    WithQuietHandler quiet;
    EXPECT_NE(OGRERR_NONE, cp_bad.importFromWkb(bad_wkb, sizeof(bad_wkb),
                                                wkbVariantIso, nBadConsumed));
  }
}

TEST(OgrCurvePolygonTest, WktExportAndImport) {
  // Empty
  {
    auto cp_empty = FromWkt("CURVEPOLYGON EMPTY");
    EXPECT_TRUE(cp_empty->IsEmpty());
    EXPECT_EQ("CURVEPOLYGON EMPTY", cp_empty->exportToWkt());
  }

  // Curve polygon with CircularString
  {
    auto cp_circ =
        FromWkt("CURVEPOLYGON(CIRCULARSTRING(0 0, 1 1, 2 0, 1 -1, 0 0))");
    ASSERT_THAT(cp_circ, testing::NotNull());
    EXPECT_FALSE(cp_circ->IsEmpty());
    EXPECT_TRUE(cp_circ->hasCurveGeometry(FALSE));
    EXPECT_TRUE(cp_circ->hasCurveGeometry(TRUE));
    EXPECT_FALSE(cp_circ->exportToWkt().empty());
  }

  // Curve polygon with CompoundCurve ring
  {
    auto cp_compound = FromWkt(
        "CURVEPOLYGON(COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 0 "
        "0)))");
    ASSERT_THAT(cp_compound, testing::NotNull());
    EXPECT_FALSE(cp_compound->IsEmpty());
  }

  // Linear geometry in curve polygon
  {
    auto cp_linear = FromWkt("CURVEPOLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
    EXPECT_TRUE(cp_linear->hasCurveGeometry(FALSE));
    EXPECT_FALSE(cp_linear->hasCurveGeometry(TRUE));
  }
}

TEST(OgrCurvePolygonTest, CurvePolyToPolyAndLinearGeometry) {
  auto cp = FromWkt("CURVEPOLYGON(CIRCULARSTRING(0 0, 1 1, 2 0, 1 -1, 0 0))");

  std::unique_ptr<OGRPolygon> poly(cp->CurvePolyToPoly(5.0, nullptr));
  ASSERT_THAT(poly, testing::NotNull());
  EXPECT_STREQ("POLYGON", poly->getGeometryName());
  EXPECT_FALSE(poly->IsEmpty());

  std::unique_ptr<OGRGeometry> linear(cp->getLinearGeometry(5.0, nullptr));
  ASSERT_THAT(linear, testing::NotNull());
  EXPECT_EQ(wkbPolygon, wkbFlatten(linear->getGeometryType()));
}

TEST(OgrCurvePolygonTest, SpatialQueriesAndEnvelope) {
  auto cp = FromWkt("CURVEPOLYGON(CIRCULARSTRING(0 0, 2 0, 0 0))");
  // Circle center (1, 0), R=1

  OGREnvelope env;
  cp->getEnvelope(&env);
  EXPECT_NEAR(0.0, env.MinX, 1e-6);
  EXPECT_NEAR(2.0, env.MaxX, 1e-6);

  OGREnvelope3D env3d;
  cp->getEnvelope(&env3d);
  EXPECT_NEAR(0.0, env3d.MinX, 1e-6);
  EXPECT_NEAR(2.0, env3d.MaxX, 1e-6);

  // Contains and Intersects against Point
  OGRPoint pt_inside(1.0, 0.0);
  EXPECT_TRUE(cp->Contains(&pt_inside));
  EXPECT_TRUE(cp->Intersects(&pt_inside));

  OGRPoint pt_boundary(2.0, 0.0);
  EXPECT_FALSE(cp->Contains(&pt_boundary));
  EXPECT_TRUE(cp->Intersects(&pt_boundary));

  OGRPoint pt_outside(5.0, 5.0);
  EXPECT_FALSE(cp->Contains(&pt_outside));
  EXPECT_FALSE(cp->Intersects(&pt_outside));

  // Non-point geometry
  auto ls_crossing = MakeClosedLine({{1, -2}, {1, 2}});
  EXPECT_TRUE(cp->Intersects(ls_crossing.get()));

  // Empty geometry check
  OGRCurvePolygon cp_empty;
  EXPECT_FALSE(cp_empty.Contains(&pt_inside));
  EXPECT_FALSE(cp_empty.Intersects(&pt_inside));

  // Linear curve polygon falling back to OGRGeometry::Contains/Intersects
  {
    auto cp_linear = FromWkt("CURVEPOLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
    OGRPoint pt_in_linear(5.0, 5.0);
    EXPECT_TRUE(cp_linear->Contains(&pt_in_linear));
    EXPECT_TRUE(cp_linear->Intersects(&pt_in_linear));
  }

  // swapXY
  cp->swapXY();
  EXPECT_FALSE(cp->IsEmpty());
  cp->swapXY();

  // segmentize
  EXPECT_TRUE(cp->segmentize(0.5));
}

TEST(OgrCurvePolygonTest, AreaAndLength) {
  // Empty
  OGRCurvePolygon cp_empty;
  EXPECT_DOUBLE_EQ(0.0, cp_empty.get_Area());
  EXPECT_DOUBLE_EQ(0.0, cp_empty.get_Length());
  EXPECT_DOUBLE_EQ(0.0, cp_empty.get_GeodesicArea());
  EXPECT_DOUBLE_EQ(0.0, cp_empty.get_GeodesicLength());

  // Circle with hole
  auto cp = FromWkt(
      "CURVEPOLYGON(CIRCULARSTRING(0 0, 10 0, 0 0), (2 0, 4 0, 4 2, 2 2, 2 "
      "0))");
  EXPECT_GT(cp->get_Area(), 0.0);
  EXPECT_GT(cp->get_Length(), 0.0);

  auto* srs = new OGRSpatialReference();
  srs->SetWellKnownGeogCS("WGS84");
  cp->assignSpatialReference(srs);
  srs->Release();

  EXPECT_GT(cp->get_GeodesicArea(), 0.0);
  EXPECT_GT(cp->get_GeodesicLength(), 0.0);
  EXPECT_GT(cp->get_GeodesicArea(cp->getSpatialReference()), 0.0);
  EXPECT_GT(cp->get_GeodesicLength(cp->getSpatialReference()), 0.0);
}

TEST(OgrCurvePolygonTest, CastToPolygonAndCasters) {
  // Linear curve polygon converts cleanly to OGRPolygon
  {
    auto cp_linear = FromWkt("CURVEPOLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
    auto* poly = TestCurvePolygon::CastToPolygon(cp_linear.release());
    ASSERT_THAT(poly, testing::NotNull());
    EXPECT_STREQ("POLYGON", poly->getGeometryName());
    delete poly;
  }

  // Non-linear curve polygon fails CastToPolygon
  {
    auto cp_circ = FromWkt("CURVEPOLYGON(CIRCULARSTRING(0 0, 2 0, 0 0))");
    auto* poly = TestCurvePolygon::CastToPolygon(cp_circ.release());
    EXPECT_EQ(nullptr, poly);
  }

  // Caster function pointers
  TestCurvePolygon tcp;
  EXPECT_THAT(tcp.GetCasterToPolygon(), testing::NotNull());
  EXPECT_THAT(tcp.GetCasterToCurvePolygon(), testing::NotNull());

  auto cp_cast = FromWkt("CURVEPOLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto* poly_from_caster = tcp.GetCasterToPolygon()(cp_cast.release());
  ASSERT_THAT(poly_from_caster, testing::NotNull());
  delete poly_from_caster;

  auto cp_cast2 = FromWkt("CURVEPOLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
  OGRSurface* surface = cp_cast2.get();
  EXPECT_EQ(cp_cast2.get(), tcp.GetCasterToCurvePolygon()(surface));
}

TEST(OgrCurvePolygonTest, EmptyPartsHandling) {
  // Polygon with empty outer ring
  {
    OGRCurvePolygon cp;
    auto cs_empty = std::make_unique<OGRCircularString>();
    EXPECT_EQ(OGRERR_NONE, cp.addRingDirectly(cs_empty.release()));
    EXPECT_TRUE(cp.hasEmptyParts());
    cp.removeEmptyParts();
    EXPECT_TRUE(cp.IsEmpty());
  }

  // Polygon with empty interior ring
  {
    OGRCurvePolygon cp;
    EXPECT_EQ(OGRERR_NONE,
              cp.addRingDirectly(
                  MakeClosedLine({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}})
                      .release()));
    auto inner_empty = std::make_unique<OGRCircularString>();
    EXPECT_EQ(OGRERR_NONE, cp.addRingDirectly(inner_empty.release()));
    EXPECT_TRUE(cp.hasEmptyParts());
    cp.removeEmptyParts();
    EXPECT_FALSE(cp.hasEmptyParts());
    EXPECT_EQ(0, cp.getNumInteriorRings());
  }
}

}  // namespace
}  // namespace autotest2
