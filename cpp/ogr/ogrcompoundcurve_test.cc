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
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestCompoundCurve : public OGRCompoundCurve {
 public:
  using OGRCompoundCurve::CastToLinearRing;
  using OGRCompoundCurve::CastToLineString;
  using OGRCompoundCurve::GetCasterToLinearRing;
  using OGRCompoundCurve::GetCasterToLineString;
};

class SubGeometryVisitor : public OGRDefaultGeometryVisitor {
 public:
  bool visited = false;
  void visit(OGRCompoundCurve*) override { visited = true; }
};

class SubConstGeometryVisitor : public OGRDefaultConstGeometryVisitor {
 public:
  bool visited = false;
  void visit(const OGRCompoundCurve*) override { visited = true; }
};

std::unique_ptr<OGRCompoundCurve> FromWkt(const char* wkt) {
  if (wkt == nullptr) return nullptr;
  auto cc = std::make_unique<OGRCompoundCurve>();
  std::unique_ptr<char, autotest2::FreeDeleter> str(strdup(wkt));
  char* ptr = str.get();
  if (cc->importFromWkt(&ptr) != OGRERR_NONE) {
    return nullptr;
  }
  return cc;
}

std::unique_ptr<OGRLineString> MakeLine(
    const std::initializer_list<std::pair<double, double>>& pts) {
  auto ls = std::make_unique<OGRLineString>();
  for (const auto& p : pts) {
    ls->addPoint(p.first, p.second);
  }
  return ls;
}

TEST(OgrCompoundCurveTest, BasicPropertiesAndConstructors) {
  OGRCompoundCurve cc;
  EXPECT_STREQ("COMPOUNDCURVE", cc.getGeometryName());
  EXPECT_EQ(wkbCompoundCurve, cc.getGeometryType());
  EXPECT_TRUE(cc.IsEmpty());
  EXPECT_EQ(0, cc.getNumCurves());
  EXPECT_EQ(0, cc.getNumPoints());
  EXPECT_TRUE(cc.hasCurveGeometry(FALSE));

  // Coordinate dimensions
  cc.set3D(TRUE);
  EXPECT_EQ(wkbCompoundCurveZ, cc.getGeometryType());

  cc.set3D(FALSE);
  cc.setMeasured(TRUE);
  EXPECT_EQ(wkbCompoundCurveM, cc.getGeometryType());

  cc.set3D(TRUE);
  EXPECT_EQ(wkbCompoundCurveZM, cc.getGeometryType());

  cc.flattenTo2D();
  EXPECT_EQ(wkbCompoundCurve, cc.getGeometryType());

  // Add line curve
  EXPECT_EQ(OGRERR_NONE,
            cc.addCurveDirectly(MakeLine({{0, 0}, {1, 1}, {2, 0}}).release()));
  EXPECT_FALSE(cc.IsEmpty());
  EXPECT_EQ(1, cc.getNumCurves());
  EXPECT_EQ(3, cc.getNumPoints());
  EXPECT_THAT(cc.getCurve(0), testing::NotNull());
  EXPECT_THAT(std::as_const(cc).getCurve(0), testing::NotNull());

  // Copy constructor
  OGRCompoundCurve cc_copy(cc);
  EXPECT_TRUE(cc.Equals(&cc_copy));

  // Move constructor
  OGRCompoundCurve cc_moved(std::move(cc_copy));
  EXPECT_TRUE(cc.Equals(&cc_moved));

  // Copy assignment
  OGRCompoundCurve cc_assigned;
  cc_assigned = cc;
  EXPECT_TRUE(cc.Equals(&cc_assigned));

  // Self assignment
  const auto* cc_assigned_ptr = &cc_assigned;
  cc_assigned = *cc_assigned_ptr;
  EXPECT_TRUE(cc.Equals(&cc_assigned));

  // Move assignment
  OGRCompoundCurve cc_move_assigned;
  cc_move_assigned = std::move(cc_moved);
  EXPECT_TRUE(cc.Equals(&cc_move_assigned));

  // Clone
  std::unique_ptr<OGRCompoundCurve> cloned(cc.clone());
  ASSERT_THAT(cloned, testing::NotNull());
  EXPECT_TRUE(cc.Equals(cloned.get()));

  // Visitor
  SubGeometryVisitor v;
  cc.accept(&v);
  EXPECT_TRUE(v.visited);

  SubConstGeometryVisitor cv;
  std::as_const(cc).accept(&cv);
  EXPECT_TRUE(cv.visited);
}

TEST(OgrCompoundCurveTest, AddCurveVariantsAndValidation) {
  OGRCompoundCurve cc;

  // 1-point curve is rejected
  {
    auto one_pt = std::make_unique<OGRLineString>();
    one_pt->addPoint(0, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(one_pt.get()));
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(std::move(one_pt)));
  }

  // Linear ring is rejected
  {
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(1, 0);
    ring->addPoint(1, 1);
    ring->addPoint(0, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(ring.get()));
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(std::move(ring)));
  }

  // Nested compound curve is rejected
  {
    auto nested = std::make_unique<OGRCompoundCurve>();
    nested->addCurveDirectly(MakeLine({{0, 0}, {1, 1}}).release());
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(nested.get()));
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(std::move(nested)));
  }

  // Add first valid curve: line from (0, 0) to (2, 0)
  EXPECT_EQ(OGRERR_NONE,
            cc.addCurveDirectly(MakeLine({{0, 0}, {2, 0}}).release()));

  // Non-contiguous curve: starts at (5, 5) instead of (2, 0)
  {
    auto non_contig = MakeLine({{5, 5}, {6, 6}});
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(non_contig.get()));
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(std::move(non_contig)));
  }

  // Empty curve when nCurveCount > 0 is rejected
  {
    auto empty_curve = std::make_unique<OGRLineString>();
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, cc.addCurve(empty_curve.get()));
  }

  // Auto-reversing curve: line from (4, 0) to (2, 0)
  // End matches (2, 0), so it should auto-reverse to (2, 0) -> (4, 0)
  {
    auto rev_line = MakeLine({{4, 0}, {2, 0}});
    EXPECT_EQ(OGRERR_NONE, cc.addCurve(std::move(rev_line)));
    EXPECT_EQ(2, cc.getNumCurves());
    OGRPoint end_pt;
    cc.EndPoint(&end_pt);
    EXPECT_DOUBLE_EQ(4.0, end_pt.getX());
    EXPECT_DOUBLE_EQ(0.0, end_pt.getY());
  }

  // Steal curve
  OGRCurve* stolen = cc.stealCurve(1);
  ASSERT_THAT(stolen, testing::NotNull());
  delete stolen;
  EXPECT_EQ(1, cc.getNumCurves());
}

TEST(OgrCompoundCurveTest, WkbExportAndImport) {
  auto cc = FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 4 0))");
  ASSERT_THAT(cc, testing::NotNull());

  // Default export
  std::vector<unsigned char> wkb(cc->WkbSize());
  EXPECT_EQ(OGRERR_NONE, cc->exportToWkb(wkb.data(), nullptr));

  // Import into new compound curve
  OGRCompoundCurve cc_imp;
  size_t nConsumed = 0;
  EXPECT_EQ(OGRERR_NONE, cc_imp.importFromWkb(wkb.data(), wkb.size(),
                                              wkbVariantIso, nConsumed));
  EXPECT_TRUE(cc->Equals(&cc_imp));
  EXPECT_EQ(wkb.size(), nConsumed);

  // Export with wkbVariantOldOgc (automatically patched to wkbVariantIso)
  OGRwkbExportOptions opt_old_ogc(wkbNDR, wkbVariantOldOgc);
  EXPECT_EQ(OGRERR_NONE, cc->exportToWkb(wkb.data(), &opt_old_ogc));

  // Corrupt WKB
  {
    unsigned char bad_wkb[] = {0, 0, 0, 0, 9, 0, 0, 0, 1};
    OGRCompoundCurve cc_bad;
    size_t nBad = 0;
    WithQuietHandler quiet;
    EXPECT_NE(OGRERR_NONE, cc_bad.importFromWkb(bad_wkb, sizeof(bad_wkb),
                                                wkbVariantIso, nBad));
  }
}

TEST(OgrCompoundCurveTest, WktExportAndImport) {
  // Empty
  {
    auto cc_empty = FromWkt("COMPOUNDCURVE EMPTY");
    EXPECT_TRUE(cc_empty->IsEmpty());
    EXPECT_EQ("COMPOUNDCURVE EMPTY", cc_empty->exportToWkt());
  }

  // Linear only
  {
    auto cc_linear = FromWkt("COMPOUNDCURVE((0 0, 1 1), (1 1, 2 0))");
    ASSERT_THAT(cc_linear, testing::NotNull());
    EXPECT_FALSE(cc_linear->IsEmpty());
    EXPECT_TRUE(cc_linear->hasCurveGeometry(FALSE));
    EXPECT_FALSE(cc_linear->hasCurveGeometry(TRUE));
    EXPECT_FALSE(cc_linear->exportToWkt().empty());
  }

  // Mixed curve and linear
  {
    auto cc_mixed =
        FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 3 0))");
    ASSERT_THAT(cc_mixed, testing::NotNull());
    EXPECT_TRUE(cc_mixed->hasCurveGeometry(TRUE));
  }
}

TEST(OgrCompoundCurveTest, StartPointEndPointAndValue) {
  auto cc = FromWkt("COMPOUNDCURVE((0 0, 2 0), (2 0, 4 0))");

  OGRPoint start_pt;
  cc->StartPoint(&start_pt);
  EXPECT_DOUBLE_EQ(0.0, start_pt.getX());
  EXPECT_DOUBLE_EQ(0.0, start_pt.getY());

  OGRPoint end_pt;
  cc->EndPoint(&end_pt);
  EXPECT_DOUBLE_EQ(4.0, end_pt.getX());
  EXPECT_DOUBLE_EQ(0.0, end_pt.getY());

  // Value < 0 returns start point
  OGRPoint pt;
  cc->Value(-1.0, &pt);
  EXPECT_DOUBLE_EQ(0.0, pt.getX());

  // Value in first segment
  cc->Value(1.0, &pt);
  EXPECT_DOUBLE_EQ(1.0, pt.getX());

  // Value in second segment
  cc->Value(3.0, &pt);
  EXPECT_DOUBLE_EQ(3.0, pt.getX());

  // Value > length returns end point
  cc->Value(10.0, &pt);
  EXPECT_DOUBLE_EQ(4.0, pt.getX());
}

TEST(OgrCompoundCurveTest, PointIteratorAndReversePoints) {
  auto cc = FromWkt("COMPOUNDCURVE((0 0, 1 0), (1 0, 2 0), (2 0, 3 0))");
  EXPECT_EQ(4, cc->getNumPoints());

  std::unique_ptr<OGRPointIterator> iter(cc->getPointIterator());
  ASSERT_THAT(iter, testing::NotNull());

  std::vector<double> x_coords;
  OGRPoint pt;
  while (iter->getNextPoint(&pt)) {
    x_coords.push_back(pt.getX());
  }
  EXPECT_THAT(x_coords, testing::ElementsAre(0.0, 1.0, 2.0, 3.0));

  // reversePoints
  cc->reversePoints();
  OGRPoint start_after_rev;
  cc->StartPoint(&start_after_rev);
  EXPECT_DOUBLE_EQ(3.0, start_after_rev.getX());
}

TEST(OgrCompoundCurveTest, CurveToLineAndLinearGeometry) {
  auto cc = FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 0 0))");

  std::unique_ptr<OGRLineString> ls(cc->CurveToLine(5.0, nullptr));
  ASSERT_THAT(ls, testing::NotNull());
  EXPECT_GT(ls->getNumPoints(), 3);

  std::unique_ptr<OGRGeometry> linear(cc->getLinearGeometry(5.0, nullptr));
  ASSERT_THAT(linear, testing::NotNull());
  EXPECT_EQ(wkbLineString, wkbFlatten(linear->getGeometryType()));
}

TEST(OgrCompoundCurveTest, SpatialQueriesAndEnvelope) {
  auto cc = FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 0 0))");

  OGREnvelope env;
  cc->getEnvelope(&env);
  EXPECT_NEAR(0.0, env.MinX, 1e-6);
  EXPECT_NEAR(2.0, env.MaxX, 1e-6);
  EXPECT_NEAR(0.0, env.MinY, 1e-6);
  EXPECT_NEAR(1.0, env.MaxY, 1e-6);

  OGREnvelope3D env3d;
  cc->getEnvelope(&env3d);
  EXPECT_NEAR(0.0, env3d.MinX, 1e-6);
  EXPECT_NEAR(2.0, env3d.MaxX, 1e-6);

  // Equals
  EXPECT_TRUE(cc->Equals(cc.get()));
  auto other = FromWkt("COMPOUNDCURVE((0 0, 1 0), (1 0, 2 0))");
  EXPECT_FALSE(cc->Equals(other.get()));
  OGRPoint pt(0, 0);
  EXPECT_FALSE(cc->Equals(&pt));

  // swapXY
  cc->swapXY();
  EXPECT_FALSE(cc->IsEmpty());
  cc->swapXY();

  // segmentize
  EXPECT_TRUE(cc->segmentize(0.5));
}

TEST(OgrCompoundCurveTest, AreaAndGeodesic) {
  // Empty
  OGRCompoundCurve cc_empty;
  EXPECT_DOUBLE_EQ(0.0, cc_empty.get_Area());
  EXPECT_DOUBLE_EQ(0.0, cc_empty.get_Length());
  EXPECT_DOUBLE_EQ(0.0, cc_empty.get_GeodesicArea());
  EXPECT_DOUBLE_EQ(0.0, cc_empty.get_GeodesicLength());

  // Unclosed geometry
  {
    auto cc_unclosed = FromWkt("COMPOUNDCURVE((0 0, 1 1), (1 1, 2 0))");
    EXPECT_DOUBLE_EQ(0.0, cc_unclosed->get_Area());
    WithQuietHandler quiet;
    EXPECT_DOUBLE_EQ(-1.0, cc_unclosed->get_GeodesicArea());
  }

  // Convex closed compound curve (IsConvex() == true):
  // Half-circle arc from (0, 0) to (2, 0) via (1, 1), closed by line (2, 0) to
  // (0, 0)
  {
    auto cc_convex =
        FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 0 0))");
    EXPECT_GT(cc_convex->get_Area(), 0.0);
    EXPECT_GT(cc_convex->get_AreaOfCurveSegments(), 0.0);
    EXPECT_GT(cc_convex->get_Length(), 0.0);

    auto* srs = new OGRSpatialReference();
    srs->SetWellKnownGeogCS("WGS84");
    cc_convex->assignSpatialReference(srs);
    srs->Release();

    EXPECT_GT(cc_convex->get_GeodesicArea(), 0.0);
    EXPECT_GT(cc_convex->get_GeodesicLength(), 0.0);
    EXPECT_GT(cc_convex->get_GeodesicArea(cc_convex->getSpatialReference()),
              0.0);
    EXPECT_GT(cc_convex->get_GeodesicLength(cc_convex->getSpatialReference()),
              0.0);
  }

  // Non-convex closed compound curve: fallback to CurveToLine()->get_Area()
  {
    auto cc_non_convex = FromWkt(
        "COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 2, 2 0), (2 0, 1 1, 0 0))");
    EXPECT_GT(cc_non_convex->get_Area(), 0.0);
  }
}

TEST(OgrCompoundCurveTest, CastFunctionsAndCasters) {
  // CastToLineString: 1 curve fast path
  {
    auto cc1 = FromWkt("COMPOUNDCURVE((0 0, 1 1, 2 0))");
    auto* ls = TestCompoundCurve::CastToLineString(cc1.release());
    ASSERT_THAT(ls, testing::NotNull());
    EXPECT_STREQ("LINESTRING", ls->getGeometryName());
    delete ls;
  }

  // CastToLineString: multiple curves path
  {
    auto cc_multi = FromWkt("COMPOUNDCURVE((0 0, 1 1), (1 1, 2 0))");
    auto* ls = TestCompoundCurve::CastToLineString(cc_multi.release());
    ASSERT_THAT(ls, testing::NotNull());
    EXPECT_STREQ("LINESTRING", ls->getGeometryName());
    delete ls;
  }

  // CastToLineString: failure on non-linear circular curve
  {
    auto cc_circ =
        FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 3 0))");
    WithQuietHandler quiet;
    auto* ls = TestCompoundCurve::CastToLineString(cc_circ.release());
    EXPECT_EQ(nullptr, ls);
  }

  // CastToLinearRing: 1 curve fast path
  {
    auto cc_ring1 = FromWkt("COMPOUNDCURVE((0 0, 1 1, 2 0, 0 0))");
    auto* ring = TestCompoundCurve::CastToLinearRing(cc_ring1.release());
    ASSERT_THAT(ring, testing::NotNull());
    EXPECT_STREQ("LINEARRING", ring->getGeometryName());
    delete ring;
  }

  // CastToLinearRing: multiple curves path
  {
    auto cc_ring_multi =
        FromWkt("COMPOUNDCURVE((0 0, 1 1), (1 1, 2 0), (2 0, 0 0))");
    auto* ring = TestCompoundCurve::CastToLinearRing(cc_ring_multi.release());
    ASSERT_THAT(ring, testing::NotNull());
    EXPECT_STREQ("LINEARRING", ring->getGeometryName());
    delete ring;
  }

  // CastToLinearRing: failure on non-linear circular curve
  {
    auto cc_ring_circ =
        FromWkt("COMPOUNDCURVE(CIRCULARSTRING(0 0, 1 1, 2 0), (2 0, 0 0))");
    WithQuietHandler quiet;
    auto* ring = TestCompoundCurve::CastToLinearRing(cc_ring_circ.release());
    EXPECT_EQ(nullptr, ring);
  }

  // Caster function pointers
  TestCompoundCurve tcc;
  EXPECT_THAT(tcc.GetCasterToLineString(), testing::NotNull());
  EXPECT_THAT(tcc.GetCasterToLinearRing(), testing::NotNull());

  auto cc_for_ls = FromWkt("COMPOUNDCURVE((0 0, 1 1, 2 0))");
  auto* ls_from_caster = tcc.GetCasterToLineString()(cc_for_ls.release());
  ASSERT_THAT(ls_from_caster, testing::NotNull());
  delete ls_from_caster;

  auto cc_for_lr = FromWkt("COMPOUNDCURVE((0 0, 1 1, 2 0, 0 0))");
  auto* lr_from_caster = tcc.GetCasterToLinearRing()(cc_for_lr.release());
  ASSERT_THAT(lr_from_caster, testing::NotNull());
  delete lr_from_caster;
}

TEST(OgrCompoundCurveTest, EmptyPartsHandling) {
  OGRCompoundCurve cc;
  auto empty_ls = std::make_unique<OGRLineString>();
  EXPECT_EQ(OGRERR_NONE, cc.addCurveDirectly(empty_ls.release()));
  EXPECT_TRUE(cc.hasEmptyParts());
  cc.removeEmptyParts();
  EXPECT_FALSE(cc.hasEmptyParts());
  EXPECT_EQ(0, cc.getNumCurves());
}

}  // namespace
}  // namespace autotest2
