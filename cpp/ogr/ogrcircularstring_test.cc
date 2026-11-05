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
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestCircularString : public OGRCircularString {
 public:
  using OGRCircularString::ContainsPoint;
  using OGRCircularString::get_AreaOfCurveSegments;
  using OGRCircularString::GetCasterToLinearRing;
  using OGRCircularString::GetCasterToLineString;
  using OGRCircularString::IntersectsPoint;
};

class SubGeometryVisitor : public OGRDefaultGeometryVisitor {
 public:
  bool visited = false;
  void visit(OGRCircularString*) override { visited = true; }
};

class SubConstGeometryVisitor : public OGRDefaultConstGeometryVisitor {
 public:
  bool visited = false;
  void visit(const OGRCircularString*) override { visited = true; }
};

std::unique_ptr<OGRCircularString> FromWkt(const char* wkt) {
  if (wkt == nullptr) return nullptr;
  auto cs = std::make_unique<OGRCircularString>();
  std::unique_ptr<char, autotest2::FreeDeleter> str(strdup(wkt));
  char* ptr = str.get();
  if (cs->importFromWkt(&ptr) != OGRERR_NONE) {
    return nullptr;
  }
  return cs;
}

TEST(OgrCircularStringTest, BasicPropertiesAndConstructors) {
  OGRCircularString cs;
  EXPECT_STREQ("CIRCULARSTRING", cs.getGeometryName());
  EXPECT_EQ(wkbCircularString, cs.getGeometryType());
  EXPECT_TRUE(cs.IsEmpty());
  EXPECT_EQ(0, cs.getNumPoints());
  EXPECT_TRUE(cs.hasCurveGeometry(FALSE));
  EXPECT_TRUE(cs.hasCurveGeometry(TRUE));

  // Coordinate dimensions
  cs.set3D(TRUE);
  EXPECT_EQ(wkbCircularStringZ, cs.getGeometryType());

  cs.set3D(FALSE);
  cs.setMeasured(TRUE);
  EXPECT_EQ(wkbCircularStringM, cs.getGeometryType());

  cs.set3D(TRUE);
  EXPECT_EQ(wkbCircularStringZM, cs.getGeometryType());

  // Add points
  cs.empty();
  cs.addPoint(0, 0, 1, 10);
  cs.addPoint(1, 1, 2, 20);
  cs.addPoint(2, 0, 3, 30);
  EXPECT_EQ(3, cs.getNumPoints());
  EXPECT_FALSE(cs.IsEmpty());

  // Copy constructor
  OGRCircularString cs_copy(cs);
  EXPECT_TRUE(cs.Equals(&cs_copy));
  EXPECT_EQ(wkbCircularStringZM, cs_copy.getGeometryType());

  // Move constructor
  OGRCircularString cs_moved(std::move(cs_copy));
  EXPECT_TRUE(cs.Equals(&cs_moved));

  // Copy assignment
  OGRCircularString cs_assigned;
  cs_assigned = cs;
  EXPECT_TRUE(cs.Equals(&cs_assigned));

  // Self assignment
  const auto* cs_assigned_ptr = &cs_assigned;
  cs_assigned = *cs_assigned_ptr;
  EXPECT_TRUE(cs.Equals(&cs_assigned));

  // Move assignment
  OGRCircularString cs_move_assigned;
  cs_move_assigned = std::move(cs_moved);
  EXPECT_TRUE(cs.Equals(&cs_move_assigned));

  // Clone
  std::unique_ptr<OGRCircularString> cloned(cs.clone());
  ASSERT_THAT(cloned, testing::NotNull());
  EXPECT_TRUE(cs.Equals(cloned.get()));

  // toUpperClass
  EXPECT_EQ(&cs, cs.toUpperClass());
  EXPECT_EQ(&std::as_const(cs), std::as_const(cs).toUpperClass());

  // Visitor
  SubGeometryVisitor v;
  cs.accept(&v);
  EXPECT_TRUE(v.visited);

  SubConstGeometryVisitor cv;
  std::as_const(cs).accept(&cv);
  EXPECT_TRUE(cv.visited);
}

TEST(OgrCircularStringTest, Validity) {
  OGRCircularString cs;
  EXPECT_TRUE(cs.IsValid());  // 0 points: valid

  WithQuietHandler quiet;
  cs.addPoint(0, 0);
  EXPECT_FALSE(cs.IsValid());  // 1 point: invalid

  cs.addPoint(1, 1);
  EXPECT_FALSE(cs.IsValid());  // 2 points: invalid

  cs.addPoint(2, 0);
  EXPECT_TRUE(cs.IsValid());  // 3 points: valid

  cs.addPoint(3, 1);
  EXPECT_FALSE(cs.IsValid());  // 4 points: invalid

  cs.addPoint(4, 0);
  EXPECT_TRUE(cs.IsValid());  // 5 points: valid
}

TEST(OgrCircularStringTest, WkbExportAndImport) {
  auto cs = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0)");
  ASSERT_THAT(cs, testing::NotNull());

  // Default export options (nullptr)
  std::vector<unsigned char> wkb(cs->WkbSize());
  EXPECT_EQ(OGRERR_NONE, cs->exportToWkb(wkb.data(), nullptr));

  // Import into new circular string
  OGRCircularString cs_imp;
  size_t nBytesConsumed = 0;
  EXPECT_EQ(OGRERR_NONE, cs_imp.importFromWkb(wkb.data(), wkb.size(),
                                              wkbVariantIso, nBytesConsumed));
  EXPECT_TRUE(cs->Equals(&cs_imp));
  EXPECT_EQ(wkb.size(), nBytesConsumed);

  // Export with wkbVariantOldOgc (automatically patched to wkbVariantIso)
  OGRwkbExportOptions opt_old_ogc(wkbNDR, wkbVariantOldOgc);
  EXPECT_EQ(OGRERR_NONE, cs->exportToWkb(wkb.data(), &opt_old_ogc));

  // Export with XDR
  OGRwkbExportOptions opt_xdr(wkbXDR, wkbVariantIso);
  std::vector<unsigned char> wkb_xdr(cs->WkbSize());
  EXPECT_EQ(OGRERR_NONE, cs->exportToWkb(wkb_xdr.data(), &opt_xdr));

  OGRCircularString cs_from_xdr;
  size_t nBytesXdr = 0;
  EXPECT_EQ(OGRERR_NONE,
            cs_from_xdr.importFromWkb(wkb_xdr.data(), wkb_xdr.size(),
                                      wkbVariantIso, nBytesXdr));
  EXPECT_TRUE(cs->Equals(&cs_from_xdr));

  // Export invalid circular string (e.g. 2 points)
  {
    OGRCircularString invalid_cs;
    invalid_cs.addPoint(0, 0);
    invalid_cs.addPoint(1, 1);
    std::vector<unsigned char> buf(100);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, invalid_cs.exportToWkb(buf.data()));
  }

  // Import corrupt WKB (2 points in header)
  {
    // WKB for 2-point circular string: byte order (1), type (8 for circular
    // string), count (2), 2 points
    unsigned char corrupt_wkb[] = {
        1,                          // byte order NDR
        8, 0, 0, 0,                 // type wkbCircularString
        2, 0, 0, 0,                 // count = 2 (invalid for circular string)
        0, 0, 0, 0, 0, 0, 0,   0,   // x0
        0, 0, 0, 0, 0, 0, 0,   0,   // y0
        0, 0, 0, 0, 0, 0, 240, 63,  // x1 = 1.0
        0, 0, 0, 0, 0, 0, 240, 63   // y1 = 1.0
    };
    OGRCircularString cs_corrupt;
    size_t nConsumedCorrupt = 0;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_CORRUPT_DATA,
              cs_corrupt.importFromWkb(corrupt_wkb, sizeof(corrupt_wkb),
                                       wkbVariantIso, nConsumedCorrupt));
    EXPECT_TRUE(cs_corrupt.IsEmpty());
  }
}

TEST(OgrCircularStringTest, WktExportAndImport) {
  // Empty
  {
    auto cs_empty = FromWkt("CIRCULARSTRING EMPTY");
    EXPECT_TRUE(cs_empty->IsEmpty());
    EXPECT_EQ("CIRCULARSTRING EMPTY", cs_empty->exportToWkt());
  }

  // 2D, Z, M, ZM
  {
    auto cs2d = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0)");
    EXPECT_FALSE(cs2d->Is3D());
    EXPECT_FALSE(cs2d->IsMeasured());
    EXPECT_FALSE(cs2d->exportToWkt().empty());

    auto csz = FromWkt("CIRCULARSTRING Z (0 0 1, 1 1 2, 2 0 3)");
    EXPECT_TRUE(csz->Is3D());
    EXPECT_FALSE(csz->IsMeasured());
    EXPECT_FALSE(csz->exportToWkt().empty());

    auto csm = FromWkt("CIRCULARSTRING M (0 0 10, 1 1 20, 2 0 30)");
    EXPECT_FALSE(csm->Is3D());
    EXPECT_TRUE(csm->IsMeasured());
    EXPECT_FALSE(csm->exportToWkt().empty());

    auto cszm = FromWkt("CIRCULARSTRING ZM (0 0 1 10, 1 1 2 20, 2 0 3 30)");
    EXPECT_TRUE(cszm->Is3D());
    EXPECT_TRUE(cszm->IsMeasured());
    EXPECT_FALSE(cszm->exportToWkt().empty());
  }

  // Invalid WKT (2 points)
  {
    OGRCircularString cs_bad;
    const char* bad_wkt = "CIRCULARSTRING(0 0, 1 1)";
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, cs_bad.importFromWkt(&bad_wkt));
    EXPECT_TRUE(cs_bad.IsEmpty());
  }

  // Export invalid (2 points)
  {
    OGRCircularString invalid_cs;
    invalid_cs.addPoint(0, 0);
    invalid_cs.addPoint(1, 1);
    OGRErr err = OGRERR_NONE;
    WithQuietHandler quiet;
    EXPECT_TRUE(invalid_cs.exportToWkt(OGRWktOptions(), &err).empty());
    EXPECT_EQ(OGRERR_FAILURE, err);
  }
}

TEST(OgrCircularStringTest, LengthCalculation) {
  // Empty
  OGRCircularString empty_cs;
  EXPECT_DOUBLE_EQ(0.0, empty_cs.get_Length());

  // Half-circle of radius 1: length should be PI
  auto semi_circle = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0)");
  EXPECT_NEAR(M_PI, semi_circle->get_Length(), 1e-6);

  // Collinear 3 points: straight line distance
  auto straight = FromWkt("CIRCULARSTRING(0 0, 1 0, 2 0)");
  EXPECT_NEAR(2.0, straight->get_Length(), 1e-6);
}

TEST(OgrCircularStringTest, EnvelopeCalculation) {
  // Empty and invalid
  {
    OGRCircularString cs_empty;
    OGREnvelope env;
    cs_empty.getEnvelope(&env);

    OGRCircularString cs_invalid;
    cs_invalid.addPoint(0, 0);
    cs_invalid.addPoint(1, 1);
    WithQuietHandler quiet;
    cs_invalid.getEnvelope(&env);
  }

  // Arc traversing quadrants CCW:
  // Circle of center (1, 0), radius 1. Full circle (0 0, 1 1, 2 0, 1 -1, 0 0)
  {
    auto full_circle = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0, 1 -1, 0 0)");
    OGREnvelope env;
    full_circle->getEnvelope(&env);
    EXPECT_NEAR(0.0, env.MinX, 1e-6);
    EXPECT_NEAR(2.0, env.MaxX, 1e-6);
    EXPECT_NEAR(-1.0, env.MinY, 1e-6);
    EXPECT_NEAR(1.0, env.MaxY, 1e-6);

    OGREnvelope3D env3d;
    full_circle->getEnvelope(&env3d);
    EXPECT_NEAR(0.0, env3d.MinX, 1e-6);
    EXPECT_NEAR(2.0, env3d.MaxX, 1e-6);
  }

  // Arc traversing quadrants CW (quadrantStart > quadrantEnd):
  {
    auto full_circle_cw = FromWkt("CIRCULARSTRING(0 0, 1 -1, 2 0, 1 1, 0 0)");
    OGREnvelope env;
    full_circle_cw->getEnvelope(&env);
    EXPECT_NEAR(0.0, env.MinX, 1e-6);
    EXPECT_NEAR(2.0, env.MaxX, 1e-6);
    EXPECT_NEAR(-1.0, env.MinY, 1e-6);
    EXPECT_NEAR(1.0, env.MaxY, 1e-6);
  }

  // Small arcs in various quadrants to test quadrant switches (cases 0, 1, 2,
  // 3)
  {
    // Crossing quadrant 0 (MaxX = cx + R): center (0, 0), R=1, angles -45 to
    // +45 deg
    auto arc0 = FromWkt("CIRCULARSTRING(0.7071 -0.7071, 1 0, 0.7071 0.7071)");
    OGREnvelope env0;
    arc0->getEnvelope(&env0);
    EXPECT_NEAR(1.0, env0.MaxX, 1e-3);

    // Crossing quadrant 1 (MaxY = cy + R): angles 45 to 135 deg
    auto arc1 = FromWkt("CIRCULARSTRING(0.7071 0.7071, 0 1, -0.7071 0.7071)");
    OGREnvelope env1;
    arc1->getEnvelope(&env1);
    EXPECT_NEAR(1.0, env1.MaxY, 1e-3);

    // Crossing quadrant 2 (MinX = cx - R): angles 135 to 225 deg
    auto arc2 =
        FromWkt("CIRCULARSTRING(-0.7071 0.7071, -1 0, -0.7071 -0.7071)");
    OGREnvelope env2;
    arc2->getEnvelope(&env2);
    EXPECT_NEAR(-1.0, env2.MinX, 1e-3);

    // Crossing quadrant 3 (MinY = cy - R): angles 225 to 315 deg
    auto arc3 =
        FromWkt("CIRCULARSTRING(-0.7071 -0.7071, 0 -1, 0.7071 -0.7071)");
    OGREnvelope env3;
    arc3->getEnvelope(&env3);
    EXPECT_NEAR(-1.0, env3.MinY, 1e-3);
  }
}

TEST(OgrCircularStringTest, Segmentize) {
  // Empty
  OGRCircularString cs_empty;
  EXPECT_TRUE(cs_empty.segmentize(1.0));

  // Invalid
  {
    OGRCircularString cs_invalid;
    cs_invalid.addPoint(0, 0);
    cs_invalid.addPoint(1, 1);
    WithQuietHandler quiet;
    EXPECT_FALSE(cs_invalid.segmentize(1.0));
  }

  // Large dfMaxLength: no points added
  {
    auto cs = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0)");
    int count_before = cs->getNumPoints();
    EXPECT_TRUE(cs->segmentize(100.0));
    EXPECT_EQ(count_before, cs->getNumPoints());
  }

  // 3D circular arc segmentized with intermediate points:
  // x0 < x2 (triggers reversePoints recursion)
  {
    auto cs = FromWkt("CIRCULARSTRING Z (0 0 0, 1 1 5, 2 0 10)");
    EXPECT_TRUE(cs->segmentize(0.2));
    EXPECT_GT(cs->getNumPoints(), 3);
    EXPECT_TRUE(cs->Is3D());
  }

  // 3D circular arc where x0 > x2 (direct segmentize loop)
  {
    auto cs = FromWkt("CIRCULARSTRING Z (2 0 10, 1 1 5, 0 0 0)");
    EXPECT_TRUE(cs->segmentize(0.2));
    EXPECT_GT(cs->getNumPoints(), 3);
  }

  // x0 == x2 and y0 < y2 (triggers reversePoints recursion)
  {
    auto cs = FromWkt("CIRCULARSTRING(0 0, 1 1, 0 2)");
    EXPECT_TRUE(cs->segmentize(0.2));
    EXPECT_GT(cs->getNumPoints(), 3);
  }

  // 3D collinear line segmentized (else branch in segmentize)
  {
    auto cs_straight = FromWkt("CIRCULARSTRING Z (10 0 0, 5 0 5, 0 0 10)");
    EXPECT_TRUE(cs_straight->segmentize(0.5));
    EXPECT_GT(cs_straight->getNumPoints(), 3);
  }

  // Negative dfMaxLength triggering error checks
  {
    WithQuietHandler quiet;
    auto cs_arc = FromWkt("CIRCULARSTRING(2 0, 1 1, 0 0)");
    EXPECT_FALSE(cs_arc->segmentize(-1.0));

    auto cs_line = FromWkt("CIRCULARSTRING(10 0, 5 0, 0 0)");
    EXPECT_FALSE(cs_line->segmentize(-1.0));
  }
}

TEST(OgrCircularStringTest, Value) {
  auto cs = FromWkt("CIRCULARSTRING Z (0 0 0, 1 1 5, 2 0 10)");

  // Distance < 0 returns start point
  OGRPoint pt;
  cs->Value(-1.0, &pt);
  EXPECT_DOUBLE_EQ(0.0, pt.getX());
  EXPECT_DOUBLE_EQ(0.0, pt.getY());
  EXPECT_DOUBLE_EQ(0.0, pt.getZ());

  // Intermediate distance on arc
  cs->Value(cs->get_Length() / 2.0, &pt);
  EXPECT_NEAR(1.0, pt.getX(), 1e-3);
  EXPECT_NEAR(1.0, pt.getY(), 1e-3);
  EXPECT_NEAR(5.0, pt.getZ(), 1e-3);

  // Distance > length returns end point
  cs->Value(cs->get_Length() + 5.0, &pt);
  EXPECT_DOUBLE_EQ(2.0, pt.getX());
  EXPECT_DOUBLE_EQ(0.0, pt.getY());
  EXPECT_DOUBLE_EQ(10.0, pt.getZ());

  // Intermediate distance on straight line
  auto cs_straight = FromWkt("CIRCULARSTRING Z (0 0 0, 1 0 5, 2 0 10)");
  cs_straight->Value(1.0, &pt);
  EXPECT_NEAR(1.0, pt.getX(), 1e-3);
  EXPECT_NEAR(0.0, pt.getY(), 1e-3);
  EXPECT_NEAR(5.0, pt.getZ(), 1e-3);
}

TEST(OgrCircularStringTest, LinearGeometryAndCurveToLine) {
  auto cs = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0)");

  std::unique_ptr<OGRLineString> ls(cs->CurveToLine(5.0, nullptr));
  ASSERT_THAT(ls, testing::NotNull());
  EXPECT_GT(ls->getNumPoints(), 3);

  std::unique_ptr<OGRGeometry> geom(cs->getLinearGeometry(5.0, nullptr));
  ASSERT_THAT(geom, testing::NotNull());
  EXPECT_EQ(wkbLineString, wkbFlatten(geom->getGeometryType()));
}

TEST(OgrCircularStringTest, AreaAndGeodesic) {
  // Empty
  OGRCircularString cs_empty;
  EXPECT_DOUBLE_EQ(0.0, cs_empty.get_Area());
  EXPECT_DOUBLE_EQ(0.0, cs_empty.get_GeodesicArea());
  EXPECT_DOUBLE_EQ(0.0, cs_empty.get_GeodesicLength());

  // Unclosed geometry
  {
    auto cs_unclosed = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0)");
    EXPECT_DOUBLE_EQ(0.0, cs_unclosed->get_Area());
    WithQuietHandler quiet;
    EXPECT_DOUBLE_EQ(-1.0, cs_unclosed->get_GeodesicArea());
  }

  // 3-point full circle: (0 0, 2 0, 0 0)
  // Center (1, 0), radius 1: Area should be PI * 1^2 = PI
  {
    auto circle3 = FromWkt("CIRCULARSTRING(0 0, 2 0, 0 0)");
    EXPECT_NEAR(M_PI, circle3->get_Area(), 1e-6);
  }

  // 5-point full circle: (0 0, 1 1, 2 0, 1 -1, 0 0)
  // Center (1, 0), radius 1: Area should be PI
  {
    auto circle5 = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0, 1 -1, 0 0)");
    EXPECT_NEAR(M_PI, circle5->get_Area(), 1e-6);

    auto* srs = new OGRSpatialReference();
    srs->SetWellKnownGeogCS("WGS84");
    circle5->assignSpatialReference(srs);
    srs->Release();

    EXPECT_GT(circle5->get_GeodesicArea(), 0.0);
    EXPECT_GT(circle5->get_GeodesicLength(), 0.0);
    EXPECT_GT(circle5->get_GeodesicArea(circle5->getSpatialReference()), 0.0);
    EXPECT_GT(circle5->get_GeodesicLength(circle5->getSpatialReference()), 0.0);
  }

  // Convex closed non-full-circle: uses get_LinearArea() +
  // get_AreaOfCurveSegments()
  {
    auto convex = FromWkt("CIRCULARSTRING(0 0, 1 1, 2 0, 1 -0.5, 0 0)");
    EXPECT_GT(convex->get_Area(), 0.0);
  }

  // Non-convex closed curve: fallback to CurveToLine()->get_Area()
  {
    auto non_convex = FromWkt("CIRCULARSTRING(0 0, 1 2, 2 0, 1 0.5, 0 0)");
    EXPECT_GT(non_convex->get_Area(), 0.0);
  }
}

TEST(OgrCircularStringTest, ContainsAndIntersectsPoint) {
  TestCircularString cs;
  const char* wkt = "CIRCULARSTRING(0 0, 2 0, 0 0)";
  EXPECT_EQ(OGRERR_NONE, cs.importFromWkt(&wkt));

  // Center is (1, 0), R=1
  OGRPoint pt_inside(1.0, 0.0);
  EXPECT_EQ(1, cs.ContainsPoint(&pt_inside));
  EXPECT_EQ(1, cs.IntersectsPoint(&pt_inside));

  OGRPoint pt_boundary(2.0, 0.0);
  EXPECT_EQ(0, cs.ContainsPoint(&pt_boundary));
  EXPECT_EQ(1, cs.IntersectsPoint(&pt_boundary));

  OGRPoint pt_outside(5.0, 5.0);
  EXPECT_EQ(0, cs.ContainsPoint(&pt_outside));
  EXPECT_EQ(0, cs.IntersectsPoint(&pt_outside));

  // Non-circle returns -1
  TestCircularString cs_arc;
  const char* arc_wkt = "CIRCULARSTRING(0 0, 1 1, 2 0)";
  EXPECT_EQ(OGRERR_NONE, cs_arc.importFromWkt(&arc_wkt));
  EXPECT_EQ(-1, cs_arc.ContainsPoint(&pt_inside));
  EXPECT_EQ(-1, cs_arc.IntersectsPoint(&pt_inside));
}

TEST(OgrCircularStringTest, Casters) {
  TestCircularString cs;

  auto pfnLS = cs.GetCasterToLineString();
  ASSERT_THAT(pfnLS, testing::NotNull());
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pfnLS(new OGRCircularString()));
  }

  auto pfnLR = cs.GetCasterToLinearRing();
  ASSERT_THAT(pfnLR, testing::NotNull());
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, pfnLR(new OGRCircularString()));
  }
}

}  // namespace
}  // namespace autotest2
