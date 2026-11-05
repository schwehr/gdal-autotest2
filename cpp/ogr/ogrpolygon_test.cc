// Copyright 2014 Google Inc. All Rights Reserved.
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

// Test ogr/ogrpolygon.cpp.
//
// GDAL's OGRPolygons consist of an outer shell (loop) followed by zero or more
// loops inside the outer shell that are holes.  No edges may overlap or cross.
//
// GDAL does not care about the direction of the loops.  The planer inside of
// polygon is always taken as the positive area for the outer loop.  The planer
// inside of inner loops are always negative area.
//
// It is possible to construct an invalid polygon by adding non-sensical rings.
// You should test the final polygon with the IsValid method before doing any
// operations on it unless you know beforehand that it is valid.
//
// If you have more than one out shell for the polygon, you must use
// OGRMultiPolygon.
//
// See also:
//   https://en.wikipedia.org/wiki/Well-known_text#Well-known_binary

#include <string.h>

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestPolygon : public OGRPolygon {
 public:
  using OGRPolygon::CastToCurvePolygon;
  using OGRPolygon::checkRing;
  using OGRPolygon::GetCasterToCurvePolygon;
  using OGRPolygon::GetCasterToPolygon;
  using OGRPolygon::isRingCorrectType;
};

std::unique_ptr<OGRGeometry> GeomFromWkt(const char* wkt) {
  if (wkt == nullptr) return nullptr;
  OGRGeometry* geom = nullptr;
  std::unique_ptr<char, autotest2::FreeDeleter> wkt_copy(strdup(wkt));
  char* wkt_end = wkt_copy.get();
  if (OGRGeometryFactory::createFromWkt(&wkt_end, nullptr, &geom) !=
      OGRERR_NONE) {
    return nullptr;
  }
  return std::unique_ptr<OGRGeometry>(geom);
}

std::unique_ptr<OGRPolygon> FromWkt(const char* wkt) {
  if (wkt == nullptr) return nullptr;
  auto p = std::make_unique<OGRPolygon>();
  std::unique_ptr<char, autotest2::FreeDeleter> str(strdup(wkt));
  char* ptr = str.get();
  if (p->importFromWkt(&ptr) != OGRERR_NONE) {
    return nullptr;
  }
  return p;
}

OGRLinearRing* MakeRing(
    const std::initializer_list<std::pair<double, double>>& points) {
  auto ring = new OGRLinearRing();
  for (const auto& point : points) {
    ring->addPoint(point.first, point.second);
  }
  return ring;
}

TEST(OgrPolygonTest, DefaultConstructor) {
  OGRPolygon p;
  EXPECT_EQ(2, p.getDimension());
  EXPECT_EQ(2, p.getCoordinateDimension());
  EXPECT_EQ(2, p.CoordinateDimension());
  EXPECT_TRUE(p.IsEmpty());
  EXPECT_TRUE(p.IsValid());
  EXPECT_TRUE(p.IsSimple());
  EXPECT_FALSE(p.Is3D());
  EXPECT_FALSE(p.IsMeasured());
  EXPECT_FALSE(p.IsRing());

  EXPECT_STREQ("POLYGON", p.getGeometryName());
  EXPECT_EQ(wkbPolygon, p.getGeometryType());

  EXPECT_FALSE(p.hasCurveGeometry(FALSE));
  EXPECT_FALSE(p.hasCurveGeometry(TRUE));

  EXPECT_EQ(9, p.WkbSize());

  std::vector<unsigned char> wkb(p.WkbSize(), 0);
  {
    const OGRwkbExportOptions options(wkbXDR);
    EXPECT_EQ(OGRERR_NONE, p.exportToWkb(wkb.data(), &options));
  }
  // 0x00 for big endian
  // 0x0004 for a polygon
  // 0x0000 for no rings
  EXPECT_THAT(wkb, testing::ElementsAre(0, 0, 0, 0, 3, 0, 0, 0, 0));

  EXPECT_EQ(OGRERR_NONE, p.exportToWkb(wkb.data()));
  // 0x01 for little endian
  EXPECT_THAT(wkb, testing::ElementsAre(1, 3, 0, 0, 0, 0, 0, 0, 0));
}

TEST(OgrPolygonTest, Empty) {
  auto p = FromWkt("POLYGON EMPTY");
  OGRPolygon expect;
  EXPECT_TRUE(p->IsValid());
  EXPECT_TRUE(p->IsEmpty());
  EXPECT_TRUE(expect.Equals(p.get()));
  EXPECT_DOUBLE_EQ(0.0, p->get_Area());

  std::unique_ptr<OGRGeometry> boundary(p->Boundary());
  auto expected_boundary = GeomFromWkt("MULTILINESTRING EMPTY");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));
}

TEST(OgrPolygonTest, RectangleWtkCcw) {
  auto p = FromWkt("POLYGON((0 0, 7 0, 7 6, 0 6, 0 0))");
  EXPECT_TRUE(p->IsValid());
  EXPECT_FALSE(p->IsEmpty());
  EXPECT_FALSE(p->IsRing());
  // Units of area are undefined as there is no SRS attached to the geometry.
  EXPECT_DOUBLE_EQ(42.0, p->get_Area());

  std::unique_ptr<OGRGeometry> boundary(p->Boundary());
  auto expected_boundary = GeomFromWkt("LINESTRING (0 0,7 0,7 6,0 6,0 0)");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));
}

TEST(OgrPolygonTest, RectangleWtkCw) {
  auto p = FromWkt("POLYGON((0 0, 0 6, 7 6, 7 0, 0 0))");
  EXPECT_TRUE(p->IsValid());
  EXPECT_DOUBLE_EQ(42.0, p->get_Area());

  std::unique_ptr<OGRGeometry> boundary(p->Boundary());
  auto expected_boundary = GeomFromWkt("LINESTRING (0 0,0 6,7 6,7 0,0 0)");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));
}

TEST(OgrPolygonTest, RectangleCcw) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {7, 0}, {7, 6}, {0, 6}, {0, 0}}));
  EXPECT_TRUE(p.IsValid());
  EXPECT_DOUBLE_EQ(42.0, p.get_Area());

  std::unique_ptr<OGRGeometry> boundary(p.Boundary());
  auto expected_boundary = GeomFromWkt("LINESTRING (0 0,7 0,7 6,0 6,0 0)");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));
}

TEST(OgrPolygonTest, RectangleCw) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {0, 6}, {7, 6}, {7, 0}, {0, 0}}));
  EXPECT_TRUE(p.IsValid());
  EXPECT_DOUBLE_EQ(42.0, p.get_Area());

  std::unique_ptr<OGRGeometry> boundary(p.Boundary());
  auto expected_boundary = GeomFromWkt("LINESTRING (0 0,0 6,7 6,7 0,0 0)");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));
}

TEST(OgrPolygonTest, RectangleCcwHoleCcw) {
  auto p =
      FromWkt("POLYGON((0 0, 7 0, 7 6, 0 6, 0 0),(4 3, 6 3, 6 5, 4 5, 4 3))");
  EXPECT_TRUE(p->IsValid());
  EXPECT_DOUBLE_EQ(38.0, p->get_Area());

  std::unique_ptr<OGRGeometry> boundary(p->Boundary());
  auto expected_boundary = GeomFromWkt(
      "MULTILINESTRING ((0 0,7 0,7 6,0 6,0 0),(4 3,6 3,6 5,4 5,4 3))");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));

  auto p1 = FromWkt("POLYGON((0 0, 7 0, 7 6, 0 6, 0 0))");
  auto p2 = FromWkt("POLYGON((4 3, 6 3, 6 5, 4 5, 4 3))");
  EXPECT_DOUBLE_EQ(p->get_Area(), p1->get_Area() - p2->get_Area());
}

TEST(OgrPolygonTest, RectangleCcwHoleCw) {
  auto p =
      FromWkt("POLYGON((0 0, 7 0, 7 6, 0 6, 0 0),(4 3, 4 5, 6 5, 6 3, 4 3))");
  EXPECT_TRUE(p->IsValid());
  EXPECT_DOUBLE_EQ(38.0, p->get_Area());

  std::unique_ptr<OGRGeometry> boundary(p->Boundary());
  auto expected_boundary = GeomFromWkt(
      "MULTILINESTRING ((0 0,7 0,7 6,0 6,0 0),(4 3,4 5,6 5,6 3,4 3))");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));

  auto p1 = FromWkt("POLYGON((0 0, 7 0, 7 6, 0 6, 0 0))");
  auto p2 = FromWkt("POLYGON((4 3, 4 5, 6 5, 6 3, 4 3))");
  EXPECT_DOUBLE_EQ(p->get_Area(), p1->get_Area() - p2->get_Area());
}

TEST(OgrPolygonTest, TwoHoles) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {7, 0}, {7, 6}, {0, 6}, {0, 0}}));
  p.addRingDirectly(MakeRing({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}));
  p.addRingDirectly(MakeRing({{3, 1}, {5, 1}, {5, 2}, {3, 2}, {3, 1}}));
  EXPECT_TRUE(p.IsValid());
  EXPECT_DOUBLE_EQ(39.0, p.get_Area());

  std::unique_ptr<OGRGeometry> boundary(p.Boundary());
  auto expected_boundary = GeomFromWkt(
      "MULTILINESTRING ("
      "(0 0,7 0,7 6,0 6,0 0),(1 1,2 1,2 2,1 2,1 1),(3 1,5 1,5 2,3 2,3 1))");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));

  auto p1 = FromWkt("POLYGON((0 0, 7 0, 7 6, 0 6, 0 0))");
  auto p2 = FromWkt("POLYGON((1 1, 2 1, 2 2, 1 2, 1 1))");
  auto p3 = FromWkt("POLYGON((3 1, 5 1, 5 2, 3 2, 3 1))");
  EXPECT_DOUBLE_EQ(p.get_Area(),
                   p1->get_Area() - p2->get_Area() - p3->get_Area());
}

TEST(OgrPolygonTest, TwoHolesSharingAnEdge) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {7, 0}, {7, 6}, {0, 6}, {0, 0}}));
  p.addRingDirectly(MakeRing({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}));
  p.addRingDirectly(MakeRing({{2, 1}, {5, 1}, {5, 2}, {2, 2}, {2, 1}}));

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Self-intersection"));
}

TEST(OgrPolygonTest, IslandWkt) {
  // Three concentric loops not allowed with WKT.
  constexpr char kWkt[] =
      "POLYGON("
      "{0 0, 7 0, 7 6, 0 6, 0 0),"
      "(1 1, 6 1, 6 5, 1 5, 1 1),"
      "(2 2, 3 2, 3 3, 2 3, 2 2)";
  std::unique_ptr<char, autotest2::FreeDeleter> str(strdup(kWkt));
  char* ptr = str.get();
  OGRPolygon p;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, p.importFromWkt(&ptr));
}

TEST(OgrPolygonTest, IslandWktCwInnerLoop) {
  // Three concentric loops not allowed with WKT.
  constexpr char kWkt[] =
      "POLYGON("
      "{0 0, 7 0, 7 6, 0 6, 0 0),"
      "(1 1, 6 1, 6 5, 1 5, 1 1),"
      "(2 2, 2 3, 3 3, 3 2, 2 2)";
  std::unique_ptr<char, autotest2::FreeDeleter> str(strdup(kWkt));
  char* ptr = str.get();
  OGRPolygon p;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, p.importFromWkt(&ptr));
}

TEST(OgrPolygonTest, Island) {
  // Three concentric loops.
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {7, 0}, {7, 6}, {0, 6}, {0, 0}}));
  p.addRingDirectly(MakeRing({{1, 1}, {6, 1}, {6, 5}, {1, 5}, {1, 1}}));
  EXPECT_TRUE(p.IsValid());
  p.addRingDirectly(MakeRing({{2, 2}, {3, 2}, {3, 3}, {2, 3}, {2, 2}}));

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Holes are nested"));
}

TEST(OgrPolygonTest, IslandCwInnerLoop) {
  // Three concentric loops.
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {7, 0}, {7, 6}, {0, 6}, {0, 0}}));
  p.addRingDirectly(MakeRing({{1, 1}, {6, 1}, {6, 5}, {1, 5}, {1, 1}}));
  p.addRingDirectly(MakeRing({{2, 2}, {2, 3}, {3, 3}, {3, 2}, {2, 2}}));  // CW

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Holes are nested"));
}

TEST(OgrPolygonTest, Bowtie) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {3, 3}, {3, 0}, {0, 3}, {0, 0}}));

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Self-intersection"));
}

TEST(OgrPolygonTest, BowtieMiddleNode) {
  // Goes around the outside without crossing in the middle, but does have a
  // node at the center.
  OGRPolygon p;
  p.addRingDirectly(
      MakeRing({{0, 0}, {1, 1}, {2, 0}, {2, 2}, {1, 1}, {0, 2}, {0, 0}}));

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Self-intersection"));
}

TEST(OgrPolygonTest, NoArea) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {3, 0}, {6, 0}, {0, 0}}));

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Self-intersection"));
}

TEST(OgrPolygonTest, NonOverlappingRings) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {7, 0}, {7, 6}, {0, 0}}));
  p.addRingDirectly(MakeRing({{10, 10}, {17, 10}, {17, 16}, {10, 10}}));

  WithQuietHandler error_handler;
  EXPECT_FALSE(p.IsValid());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              testing::HasSubstr("Hole lies outside shell"));
}

TEST(OgrPolygonTest, RectangleDoubleVertex) {
  OGRPolygon p;
  p.addRingDirectly(MakeRing({{0, 0}, {1, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}}));

  WithQuietHandler error_handler;
  EXPECT_TRUE(p.IsValid());
  EXPECT_DOUBLE_EQ(1.0, p.get_Area());

  std::unique_ptr<OGRGeometry> boundary(p.Boundary());
  auto expected_boundary = GeomFromWkt("LINESTRING (0 0,1 0,1 0,1 1,0 1,0 0)");
  EXPECT_TRUE(expected_boundary->Equals(boundary.get()));
  boundary->dumpReadable(stderr);
}

TEST(OgrPolygonTest, ConstructorsAndAssignment) {
  // Bounding box constructor
  OGRPolygon p_box(1.0, 2.0, 5.0, 6.0);
  EXPECT_DOUBLE_EQ(16.0, p_box.get_Area());

  // OGREnvelope constructor
  OGREnvelope env;
  env.MinX = 0.0;
  env.MinY = 0.0;
  env.MaxX = 10.0;
  env.MaxY = 10.0;
  OGRPolygon p_env(env);
  EXPECT_DOUBLE_EQ(100.0, p_env.get_Area());

  // Copy constructor
  OGRPolygon p_copy(p_env);
  EXPECT_TRUE(p_copy.Equals(&p_env));

  // Copy assignment
  OGRPolygon p_assign;
  p_assign = p_box;
  EXPECT_TRUE(p_assign.Equals(&p_box));

  // Self assignment
  p_assign = p_assign;
  EXPECT_TRUE(p_assign.Equals(&p_box));

  // Clone
  std::unique_ptr<OGRPolygon> cloned(p_box.clone());
  ASSERT_THAT(cloned, testing::NotNull());
  EXPECT_TRUE(cloned->Equals(&p_box));
}

TEST(OgrPolygonTest, GeometryTypesAndNames) {
  OGRPolygon p2d;
  EXPECT_EQ(wkbPolygon, p2d.getGeometryType());
  EXPECT_STREQ("POLYGON", p2d.getGeometryName());

  OGRPolygon p3d;
  p3d.set3D(TRUE);
  EXPECT_EQ(wkbPolygon25D, p3d.getGeometryType());

  OGRPolygon pm;
  pm.setMeasured(TRUE);
  EXPECT_EQ(wkbPolygonM, pm.getGeometryType());

  OGRPolygon pzm;
  pzm.set3D(TRUE);
  pzm.setMeasured(TRUE);
  EXPECT_EQ(wkbPolygonZM, pzm.getGeometryType());
}

TEST(OgrPolygonTest, RingsAccessorsAndStealing) {
  OGRPolygon p;
  EXPECT_EQ(nullptr, p.getExteriorRing());
  EXPECT_EQ(nullptr, std::as_const(p).getExteriorRing());
  EXPECT_EQ(nullptr, p.getInteriorRing(0));
  EXPECT_EQ(nullptr, std::as_const(p).getInteriorRing(0));
  EXPECT_EQ(nullptr, p.getInteriorRing(-1));
  EXPECT_EQ(nullptr, p.stealInteriorRing(0));
  EXPECT_EQ(nullptr, p.stealInteriorRing(-1));

  // Add outer ring and inner rings
  p.addRingDirectly(MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}));
  p.addRingDirectly(MakeRing({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}));
  p.addRingDirectly(MakeRing({{3, 3}, {4, 3}, {4, 4}, {3, 4}, {3, 3}}));

  EXPECT_THAT(p.getExteriorRing(), testing::NotNull());
  EXPECT_THAT(std::as_const(p).getExteriorRing(), testing::NotNull());
  EXPECT_THAT(p.getInteriorRing(0), testing::NotNull());
  EXPECT_THAT(std::as_const(p).getInteriorRing(0), testing::NotNull());
  EXPECT_THAT(p.getInteriorRing(1), testing::NotNull());
  EXPECT_EQ(nullptr, p.getInteriorRing(2));   // out of range
  EXPECT_EQ(nullptr, p.getInteriorRing(-1));  // negative

  // Steal exterior ring on a dedicated polygon
  {
    OGRPolygon p_ext;
    p_ext.addRingDirectly(
        MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}));
    OGRLinearRing* stolen_exterior = p_ext.stealExteriorRing();
    ASSERT_THAT(stolen_exterior, testing::NotNull());
    delete stolen_exterior;
  }

  // Steal interior ring on a dedicated polygon
  {
    OGRPolygon p_int;
    p_int.addRingDirectly(
        MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}));
    p_int.addRingDirectly(MakeRing({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}));
    EXPECT_EQ(nullptr, p_int.stealInteriorRing(-1));
    EXPECT_EQ(nullptr, p_int.stealInteriorRing(5));
    OGRLinearRing* stolen_interior = p_int.stealInteriorRing(0);
    ASSERT_THAT(stolen_interior, testing::NotNull());
    delete stolen_interior;
  }
}

TEST(OgrPolygonTest, CheckRingValidation) {
  OGRPolygon p;

  // Non-linear-ring type
  {
    WithQuietHandler error_handler;
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    EXPECT_NE(OGRERR_NONE, p.addRing(ls.get()));
  }

  // Unclosed ring with OGR_GEOMETRY_ACCEPT_UNCLOSED_RING = NO
  {
    CPLSetConfigOption("OGR_GEOMETRY_ACCEPT_UNCLOSED_RING", "NO");
    WithQuietHandler error_handler;
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(1, 0);
    ring->addPoint(1, 1);
    EXPECT_NE(OGRERR_NONE, p.addRing(ring.get()));
    CPLSetConfigOption("OGR_GEOMETRY_ACCEPT_UNCLOSED_RING", nullptr);
  }

  // Unclosed ring with default (warning only)
  {
    WithQuietHandler error_handler;
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(1, 0);
    ring->addPoint(1, 1);
    EXPECT_EQ(OGRERR_NONE, p.addRing(ring.get()));
  }
}

TEST(OgrPolygonTest, WkbExportAndImportVariants) {
  auto p = FromWkt(
      "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))");

  // Export to WKB with default options (psOptions == nullptr)
  std::vector<unsigned char> wkb(p->WkbSize());
  EXPECT_EQ(OGRERR_NONE, p->exportToWkb(wkb.data(), nullptr));

  // Import into new polygon
  OGRPolygon p_imp;
  size_t nBytesConsumed = 0;
  EXPECT_EQ(OGRERR_NONE, p_imp.importFromWkb(wkb.data(), wkb.size(),
                                             wkbVariantOldOgc, nBytesConsumed));
  EXPECT_TRUE(p->Equals(&p_imp));

  // Export to PostGIS 1 variant (2D, 3D, M, ZM)
  {
    OGRwkbExportOptions opt_postgis(wkbNDR, wkbVariantPostGIS1);
    std::vector<unsigned char> wkb_pg(p->WkbSize());
    EXPECT_EQ(OGRERR_NONE, p->exportToWkb(wkb_pg.data(), &opt_postgis));

    // 3D PostGIS
    OGRPolygon p3d(*p);
    p3d.set3D(TRUE);
    std::vector<unsigned char> wkb_3d(p3d.WkbSize());
    EXPECT_EQ(OGRERR_NONE, p3d.exportToWkb(wkb_3d.data(), &opt_postgis));

    // Measured PostGIS
    OGRPolygon pm(*p);
    pm.setMeasured(TRUE);
    std::vector<unsigned char> wkb_m(pm.WkbSize());
    EXPECT_EQ(OGRERR_NONE, pm.exportToWkb(wkb_m.data(), &opt_postgis));
  }

  // Export with byte swapping (XDR)
  {
    OGRwkbExportOptions opt_xdr(wkbXDR, wkbVariantIso);
    std::vector<unsigned char> wkb_xdr(p->WkbSize());
    EXPECT_EQ(OGRERR_NONE, p->exportToWkb(wkb_xdr.data(), &opt_xdr));

    OGRPolygon p_from_xdr;
    size_t nBytesConsumedXdr = 0;
    EXPECT_EQ(OGRERR_NONE,
              p_from_xdr.importFromWkb(wkb_xdr.data(), wkb_xdr.size(),
                                       wkbVariantIso, nBytesConsumedXdr));
    EXPECT_TRUE(p->Equals(&p_from_xdr));
  }

  // Fast path for 1-ring NDR polygon onto existing 1-ring polygon
  {
    auto p1 = FromWkt("POLYGON((0 0, 5 0, 5 5, 0 5, 0 0))");
    auto p2 = FromWkt("POLYGON((1 1, 6 1, 6 6, 1 6, 1 1))");
    std::vector<unsigned char> wkb1(p1->WkbSize());
    EXPECT_EQ(OGRERR_NONE, p1->exportToWkb(wkb1.data()));

    // Import p1's WKB on top of p2
    size_t nBytesConsumedFast = 0;
    EXPECT_EQ(OGRERR_NONE,
              p2->importFromWkb(wkb1.data(), wkb1.size(), wkbVariantOldOgc,
                                nBytesConsumedFast));
    EXPECT_TRUE(p1->Equals(p2.get()));

    // Corrupt data for fast path
    unsigned char corrupt_wkb[9 + 4] = {
        1,    3,    0,    0,   0, 1, 0, 0, 0,  // header for 1-ring polygon
        0xFF, 0xFF, 0xFF, 0x7F  // gigantic point count causing corrupt data
    };
    size_t nBytesCorrupt = 0;
    EXPECT_NE(OGRERR_NONE, p2->importFromWkb(corrupt_wkb, sizeof(corrupt_wkb),
                                             wkbVariantOldOgc, nBytesCorrupt));
  }

  // General corrupt WKB
  {
    unsigned char bad_wkb[] = {0, 0, 0, 0, 3,
                               0, 0, 0, 2};  // 2 rings but truncated
    OGRPolygon bad_poly;
    size_t nBytesBad = 0;
    EXPECT_NE(OGRERR_NONE, bad_poly.importFromWkb(bad_wkb, sizeof(bad_wkb),
                                                  wkbVariantOldOgc, nBytesBad));
  }
}

TEST(OgrPolygonTest, WktImportAndExportVariants) {
  // Empty rings in WKT list: POLYGON(EMPTY, (0 0, 1 0, 1 1, 0 0))
  {
    OGRPolygon p;
    const char* wkt = "POLYGON(EMPTY, (0 0, 1 0, 1 1, 0 0))";
    EXPECT_EQ(OGRERR_NONE, p.importFromWkt(&wkt));
  }

  // Corrupt WKT tests
  {
    OGRPolygon p;
    const char* bad1 = "POLYGON NON_PAREN";
    EXPECT_EQ(OGRERR_CORRUPT_DATA, p.importFromWkt(&bad1));

    const char* bad2 = "POLYGON ((0 0, 1 1)";  // missing closing paren
    EXPECT_EQ(OGRERR_CORRUPT_DATA, p.importFromWkt(&bad2));

    const char* bad3 = "POLYGON ((invalid))";
    EXPECT_EQ(OGRERR_CORRUPT_DATA, p.importFromWkt(&bad3));
  }

  // 3D / M / ZM coordinates in WKT
  {
    auto p_z = FromWkt("POLYGON Z ((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1))");
    EXPECT_TRUE(p_z->Is3D());

    auto p_m = FromWkt("POLYGON M ((0 0 5, 10 0 5, 10 10 5, 0 10 5, 0 0 5))");
    EXPECT_TRUE(p_m->IsMeasured());

    auto p_zm = FromWkt(
        "POLYGON ZM ((0 0 1 5, 10 0 1 5, 10 10 1 5, 0 10 1 5, 0 0 1 5))");
    EXPECT_TRUE(p_zm->Is3D());
    EXPECT_TRUE(p_zm->IsMeasured());

    // Export to WKT
    EXPECT_FALSE(p_zm->exportToWkt().empty());
  }

  // Export to WKT when empty
  {
    OGRPolygon p_empty;
    EXPECT_EQ("POLYGON EMPTY", p_empty.exportToWkt());
  }
}

TEST(OgrPolygonTest, IsPointOnSurface) {
  auto p = FromWkt(
      "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))");

  // Null point
  EXPECT_FALSE(p->IsPointOnSurface(nullptr));

  // Point inside outer ring, outside hole
  OGRPoint pt_inside(1, 1);
  EXPECT_TRUE(p->IsPointOnSurface(&pt_inside));

  // Point inside hole
  OGRPoint pt_hole(3, 3);
  EXPECT_FALSE(p->IsPointOnSurface(&pt_hole));

  // Point outside outer ring
  OGRPoint pt_outside(20, 20);
  EXPECT_FALSE(p->IsPointOnSurface(&pt_outside));
}

TEST(OgrPolygonTest, OperationsAndCasters) {
  auto p = FromWkt("POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");

  // closeRings
  p->closeRings();

  // CurvePolyToPoly
  std::unique_ptr<OGRPolygon> poly_from_curve(p->CurvePolyToPoly(0.0, nullptr));
  ASSERT_THAT(poly_from_curve, testing::NotNull());
  EXPECT_TRUE(p->Equals(poly_from_curve.get()));

  // hasCurveGeometry
  EXPECT_FALSE(p->hasCurveGeometry(FALSE));
  EXPECT_FALSE(p->hasCurveGeometry(TRUE));

  // getLinearGeometry
  std::unique_ptr<OGRGeometry> linear(p->getLinearGeometry(0.0, nullptr));
  ASSERT_THAT(linear, testing::NotNull());
  EXPECT_TRUE(p->Equals(linear.get()));

  // getCurveGeometry without curve parts returns clone()
  std::unique_ptr<OGRGeometry> curve_geom(p->getCurveGeometry(nullptr));
  ASSERT_THAT(curve_geom, testing::NotNull());
  EXPECT_TRUE(p->Equals(curve_geom.get()));

  // CastToCurvePolygon
  auto p_for_cast = FromWkt("POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto* curve_poly = TestPolygon::CastToCurvePolygon(p_for_cast.release());
  ASSERT_THAT(curve_poly, testing::NotNull());
  EXPECT_STREQ("CURVEPOLYGON", curve_poly->getGeometryName());
  delete curve_poly;

  // Caster function pointers
  TestPolygon p_caster;
  EXPECT_THAT(p_caster.GetCasterToPolygon(), testing::NotNull());
  EXPECT_THAT(p_caster.GetCasterToCurvePolygon(), testing::NotNull());

  EXPECT_FALSE(p_caster.isRingCorrectType(nullptr));
  EXPECT_FALSE(p_caster.checkRing(nullptr));

  auto p_cast_target = FromWkt("POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");
  OGRSurface* surface = p_cast_target.get();
  EXPECT_EQ(surface, p_caster.GetCasterToPolygon()(surface));

  auto* cp_from_surface =
      p_caster.GetCasterToCurvePolygon()(p_cast_target.release());
  ASSERT_THAT(cp_from_surface, testing::NotNull());
  delete cp_from_surface;
}

}  // namespace
}  // namespace autotest2
