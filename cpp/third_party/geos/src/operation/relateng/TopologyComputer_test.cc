// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/src/operation/relateng/TopologyComputer.cpp.

#include "third_party/geos/include/geos/operation/relateng/TopologyComputer.h"

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/NodeSection.h"
#include "third_party/geos/include/geos/operation/relateng/RelateGeometry.h"
#include "third_party/geos/include/geos/operation/relateng/RelateMatrixPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::NodeSection;
using geos::operation::relateng::RelateGeometry;
using geos::operation::relateng::RelateMatrixPredicate;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyComputer;
using geos::operation::relateng::TopologyPredicate;
using geos::util::IllegalStateException;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr bool kGeomA = true;
constexpr bool kGeomB = false;

constexpr int kDimFalse = Dimension::False;
constexpr int kDimP = Dimension::P;
constexpr int kDimL = Dimension::L;
constexpr int kDimA = Dimension::A;

constexpr Location kInt = Location::INTERIOR;
constexpr Location kBdy = Location::BOUNDARY;
constexpr Location kExt = Location::EXTERIOR;

struct DimUpdate {
  Location loc_a;
  Location loc_b;
  int dim;

  bool operator==(const DimUpdate& other) const = default;
};

std::ostream& operator<<(std::ostream& os, const DimUpdate& u) {
  return os << "{" << u.loc_a << ", " << u.loc_b << ", " << u.dim << "}";
}

// Predicate double that records every updateDimension call and can trigger
// early-known termination when a target dimension is updated.
class RecordingTopologyPredicate final : public TopologyPredicate {
 public:
  std::string name() const override { return "recording"; }

  void finish() override { known_ = true; }

  bool isKnown() const override { return known_; }

  bool value() const override { return value_; }

  void updateDimension(geos::geom::Location loc_a, geos::geom::Location loc_b,
                       int dimension) override {
    updates_.push_back({loc_a, loc_b, dimension});
    if (know_on_dim_ >= 0 && dimension == know_on_dim_) {
      known_ = true;
      value_ = true;
    }
  }

  void ClearUpdates() { updates_.clear(); }
  void set_know_on_dim(int dim) { know_on_dim_ = dim; }

  const std::vector<DimUpdate>& updates() const { return updates_; }

 private:
  std::vector<DimUpdate> updates_;
  bool known_ = false;
  bool value_ = false;
  int know_on_dim_ = -1;
};

class TopologyComputerTest : public ::testing::Test {
 protected:
  TopologyComputerTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::vector<DimUpdate> CaptureInitUpdates(
      const std::string& wkt_a, const std::string& wkt_b,
      const BoundaryNodeRule& bn_rule =
          BoundaryNodeRule::getBoundaryRuleMod2()) const {
    auto geom_a = ReadWkt(wkt_a);
    auto geom_b = ReadWkt(wkt_b);
    RelateGeometry rel_a(geom_a.get(), bn_rule);
    RelateGeometry rel_b(geom_b.get(), bn_rule);
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    return pred.updates();
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(TopologyComputerTest, InitExteriorDimsNonEmptyDimensionPairs) {
  // P/P, L/L, A/A leave exterior dimensions undetermined until geometry tests.
  EXPECT_THAT(CaptureInitUpdates("POINT (0 0)", "POINT (1 1)"), IsEmpty());
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 1 1)", "LINESTRING (1 0, 0 1)"),
      IsEmpty());
  EXPECT_THAT(CaptureInitUpdates("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                                 "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))"),
              IsEmpty());

  // P/L and L/P.
  EXPECT_THAT(CaptureInitUpdates("POINT (0 0)", "LINESTRING (0 0, 1 1)"),
              ElementsAre(DimUpdate{kExt, kInt, kDimL}));
  EXPECT_THAT(CaptureInitUpdates("LINESTRING (0 0, 1 1)", "POINT (0 0)"),
              ElementsAre(DimUpdate{kInt, kExt, kDimL}));

  // P/A and A/P.
  EXPECT_THAT(
      CaptureInitUpdates("POINT (0 0)", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"),
      ElementsAre(DimUpdate{kExt, kInt, kDimA}, DimUpdate{kExt, kBdy, kDimL}));
  EXPECT_THAT(
      CaptureInitUpdates("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", "POINT (0 0)"),
      ElementsAre(DimUpdate{kInt, kExt, kDimA}, DimUpdate{kBdy, kExt, kDimL}));

  // L/A and A/L.
  EXPECT_THAT(CaptureInitUpdates("LINESTRING (0 0, 1 1)",
                                 "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"),
              ElementsAre(DimUpdate{kExt, kInt, kDimA}));
  EXPECT_THAT(CaptureInitUpdates("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                                 "LINESTRING (0 0, 1 1)"),
              ElementsAre(DimUpdate{kInt, kExt, kDimA}));

  // Zero-length LineStrings have real dimension P, triggering P/L and P/A
  // rules.
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 0 0)", "LINESTRING (0 0, 1 1)"),
      ElementsAre(DimUpdate{kExt, kInt, kDimL}));
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 1 1)", "LINESTRING (0 0, 0 0)"),
      ElementsAre(DimUpdate{kInt, kExt, kDimL}));
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 0 0)",
                         "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"),
      ElementsAre(DimUpdate{kExt, kInt, kDimA}, DimUpdate{kExt, kBdy, kDimL}));
}

TEST_F(TopologyComputerTest, InitExteriorDimsWithEmptyGeometries) {
  // Both geometries empty -> no updates.
  EXPECT_THAT(CaptureInitUpdates("POINT EMPTY", "POINT EMPTY"), IsEmpty());
  EXPECT_THAT(CaptureInitUpdates("LINESTRING EMPTY", "POLYGON EMPTY"),
              IsEmpty());

  // Non-empty Point vs Empty.
  EXPECT_THAT(CaptureInitUpdates("POINT (1 1)", "POINT EMPTY"),
              ElementsAre(DimUpdate{kInt, kExt, kDimP}));
  EXPECT_THAT(CaptureInitUpdates("POINT EMPTY", "POINT (1 1)"),
              ElementsAre(DimUpdate{kExt, kInt, kDimP}));

  // Non-empty open LineString (has boundary) vs Empty.
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 1 1)", "POINT EMPTY"),
      ElementsAre(DimUpdate{kBdy, kExt, kDimP}, DimUpdate{kInt, kExt, kDimL}));
  EXPECT_THAT(
      CaptureInitUpdates("POINT EMPTY", "LINESTRING (0 0, 1 1)"),
      ElementsAre(DimUpdate{kExt, kBdy, kDimP}, DimUpdate{kExt, kInt, kDimL}));

  // Non-empty closed LineString under Mod2 (no boundary) vs Empty.
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 1 0, 1 1, 0 0)", "POLYGON EMPTY"),
      ElementsAre(DimUpdate{kInt, kExt, kDimL}));
  EXPECT_THAT(
      CaptureInitUpdates("POLYGON EMPTY", "LINESTRING (0 0, 1 0, 1 1, 0 0)"),
      ElementsAre(DimUpdate{kExt, kInt, kDimL}));

  // Closed LineString under EndPoint boundary rule has a boundary vertex.
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 1 0, 1 1, 0 0)", "POINT EMPTY",
                         BoundaryNodeRule::getBoundaryEndPoint()),
      ElementsAre(DimUpdate{kBdy, kExt, kDimP}, DimUpdate{kInt, kExt, kDimL}));

  // Zero-length LineString vs EMPTY: real dim is P, getDimension() is L,
  // Mod2 treats coincident endpoints (degree 2) as non-boundary while
  // EndPoint treats them as boundary.
  EXPECT_THAT(CaptureInitUpdates("LINESTRING (0 0, 0 0)", "POINT EMPTY"),
              ElementsAre(DimUpdate{kInt, kExt, kDimL}));
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING (0 0, 0 0)", "POINT EMPTY",
                         BoundaryNodeRule::getBoundaryEndPoint()),
      ElementsAre(DimUpdate{kBdy, kExt, kDimP}, DimUpdate{kInt, kExt, kDimL}));

  // Non-empty Polygon vs Empty.
  EXPECT_THAT(
      CaptureInitUpdates("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                         "LINESTRING EMPTY"),
      ElementsAre(DimUpdate{kBdy, kExt, kDimL}, DimUpdate{kInt, kExt, kDimA}));
  EXPECT_THAT(
      CaptureInitUpdates("LINESTRING EMPTY",
                         "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"),
      ElementsAre(DimUpdate{kExt, kBdy, kDimL}, DimUpdate{kExt, kInt, kDimA}));
}

TEST_F(TopologyComputerTest, GetDimensionAndIsAreaArea) {
  auto pt = ReadWkt("POINT (0 0)");
  auto line = ReadWkt("LINESTRING (0 0, 1 1)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto gc_mixed =
      ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (1 1, 2 2))");
  auto gc_area = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), POLYGON ((0 0, 1 0, 1 1, 0 0)))");
  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");

  RecordingTopologyPredicate pred;

  {
    RelateGeometry rel_a(pt.get());
    RelateGeometry rel_b(line.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_EQ(tc.getDimension(kGeomA), kDimP);
    EXPECT_EQ(tc.getDimension(kGeomB), kDimL);
    EXPECT_FALSE(tc.isAreaArea());
  }
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(gc_mixed.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_EQ(tc.getDimension(kGeomA), kDimA);
    EXPECT_EQ(tc.getDimension(kGeomB), kDimL);
    EXPECT_FALSE(tc.isAreaArea());
  }
  {
    RelateGeometry rel_a(gc_mixed.get());
    RelateGeometry rel_b(poly.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_EQ(tc.getDimension(kGeomA), kDimL);
    EXPECT_EQ(tc.getDimension(kGeomB), kDimA);
    EXPECT_FALSE(tc.isAreaArea());
  }
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(gc_area.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_EQ(tc.getDimension(kGeomA), kDimA);
    EXPECT_EQ(tc.getDimension(kGeomB), kDimA);
    EXPECT_TRUE(tc.isAreaArea());
  }
  {
    RelateGeometry rel_a(empty_gc.get());
    RelateGeometry rel_b(poly.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_EQ(tc.getDimension(kGeomA), kDimFalse);
    EXPECT_EQ(tc.getDimension(kGeomB), kDimA);
    EXPECT_FALSE(tc.isAreaArea());
  }
}

TEST_F(TopologyComputerTest, IsSelfNodingRequiredBranches) {
  auto pt = ReadWkt("POINT (0 0)");
  auto line = ReadWkt("LINESTRING (0 0, 2 2, 0 2, 2 0)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto gc_single_poly =
      ReadWkt("GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))");
  auto gc_two_polys = ReadWkt(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1)))");
  auto gc_area_and_line = ReadWkt(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "LINESTRING (0 0, 3 3))");

  // 1. Predicate does not require self-noding -> always false.
  {
    auto intersects = RelatePredicate::intersects();
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(gc_area_and_line.get());
    TopologyComputer tc(*intersects, rel_a, rel_b);
    EXPECT_FALSE(tc.isSelfNodingRequired());
  }

  // 2. Predicate requires self-noding and geomA requires it (LineString /
  // multi-element GC).
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(pt.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_TRUE(tc.isSelfNodingRequired());
  }
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(gc_two_polys.get());
    RelateGeometry rel_b(pt.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_TRUE(tc.isSelfNodingRequired());
  }

  // 3. geomA does NOT require self-noding, geomB has both area and line ->
  // true.
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(gc_area_and_line.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_TRUE(tc.isSelfNodingRequired());
  }

  // 3b. geomA has area + line -> geomA.isSelfNodingRequired() is true.
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(gc_area_and_line.get());
    RelateGeometry rel_b(pt.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_TRUE(tc.isSelfNodingRequired());
  }

  // 4. geomA does NOT require self-noding and geomB does NOT have area+line ->
  // false.
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(line.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_FALSE(tc.isSelfNodingRequired());
  }
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(gc_two_polys.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_FALSE(tc.isSelfNodingRequired());
  }
  {
    RelateMatrixPredicate pred;
    RelateGeometry rel_a(gc_single_poly.get());
    RelateGeometry rel_b(poly.get());
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_FALSE(tc.isSelfNodingRequired());
  }
}

TEST_F(TopologyComputerTest, ExteriorCheckResultKnownFinishAndGetResult) {
  auto pt_a = ReadWkt("POINT (0 0)");
  auto poly_b = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  {
    auto contains = RelatePredicate::contains();
    RelateGeometry rel_a(poly_b.get());
    RelateGeometry rel_b(pt_a.get());
    contains->init(rel_a.getDimensionReal(), rel_b.getDimensionReal());
    TopologyComputer tc(*contains, rel_a, rel_b);
    EXPECT_FALSE(tc.isExteriorCheckRequired(kGeomA));
    EXPECT_TRUE(tc.isExteriorCheckRequired(kGeomB));
  }
  {
    auto within = RelatePredicate::within();
    RelateGeometry rel_a(pt_a.get());
    RelateGeometry rel_b(poly_b.get());
    within->init(rel_a.getDimensionReal(), rel_b.getDimensionReal());
    TopologyComputer tc(*within, rel_a, rel_b);
    EXPECT_TRUE(tc.isExteriorCheckRequired(kGeomA));
    EXPECT_FALSE(tc.isExteriorCheckRequired(kGeomB));
  }
  {
    auto intersects = RelatePredicate::intersects();
    RelateGeometry rel_a(pt_a.get());
    RelateGeometry rel_b(poly_b.get());
    TopologyComputer tc(*intersects, rel_a, rel_b);
    EXPECT_FALSE(tc.isExteriorCheckRequired(kGeomA));
    EXPECT_FALSE(tc.isExteriorCheckRequired(kGeomB));
    EXPECT_FALSE(tc.isResultKnown());

    const CoordinateXY c(1.0, 1.0);
    tc.addPointOnPointInterior(&c);
    EXPECT_TRUE(tc.isResultKnown());
    EXPECT_TRUE(tc.getResult());
  }
  {
    auto intersects = RelatePredicate::intersects();
    RelateGeometry rel_a(pt_a.get());
    RelateGeometry rel_b(poly_b.get());
    TopologyComputer tc(*intersects, rel_a, rel_b);
    EXPECT_FALSE(tc.isResultKnown());
    tc.finish();
    EXPECT_TRUE(tc.isResultKnown());
    EXPECT_FALSE(tc.getResult());
  }
  {
    RelateMatrixPredicate matrix_pred;
    RelateGeometry rel_a(pt_a.get());
    RelateGeometry rel_b(poly_b.get());
    matrix_pred.init(rel_a.getDimensionReal(), rel_b.getDimensionReal());
    TopologyComputer tc(matrix_pred, rel_a, rel_b);
    EXPECT_TRUE(tc.isExteriorCheckRequired(kGeomA));
    EXPECT_TRUE(tc.isExteriorCheckRequired(kGeomB));

    const CoordinateXY c(0.0, 0.0);
    tc.addPointOnGeometry(kGeomA, kBdy, kDimA, &c);
    tc.finish();
    EXPECT_TRUE(tc.isResultKnown());
    EXPECT_FALSE(tc.getResult());
    EXPECT_EQ(matrix_pred.getIM()->toString(), "F0FFFF212");
  }
}

TEST_F(TopologyComputerTest, PointOnPointInteriorAndExterior) {
  auto pt_a = ReadWkt("POINT (0 0)");
  auto pt_b = ReadWkt("POINT (1 1)");
  RelateGeometry rel_a(pt_a.get());
  RelateGeometry rel_b(pt_b.get());
  RecordingTopologyPredicate pred;
  TopologyComputer tc(pred, rel_a, rel_b);
  pred.ClearUpdates();

  const CoordinateXY c(0.0, 0.0);
  tc.addPointOnPointInterior(&c);
  tc.addPointOnPointExterior(kGeomA, &c);
  tc.addPointOnPointExterior(kGeomB, nullptr);

  EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimP},
                                          DimUpdate{kInt, kExt, kDimP},
                                          DimUpdate{kExt, kInt, kDimP}));
}

TEST_F(TopologyComputerTest, AddPointOnGeometryAllBranches) {
  const CoordinateXY c(1.0, 1.0);
  auto pt = ReadWkt("POINT (1 1)");
  auto line = ReadWkt("LINESTRING (0 0, 2 2)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto empty_poly = ReadWkt("POLYGON EMPTY");

  // Target is empty: returns early after recording Point interior location.
  {
    RelateGeometry rel_a(pt.get());
    RelateGeometry rel_b(empty_poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addPointOnGeometry(kGeomA, kExt, kDimA, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kExt, kDimP}));
  }
  {
    RelateGeometry rel_a(empty_poly.get());
    RelateGeometry rel_b(pt.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addPointOnGeometry(kGeomB, kExt, kDimA, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kExt, kInt, kDimP}));
  }

  // Non-empty target dimension P and L (both isPointA == true and false).
  {
    RelateGeometry rel_a(pt.get());
    RelateGeometry rel_b(line.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addPointOnGeometry(kGeomA, kInt, kDimP, &c);
    tc.addPointOnGeometry(kGeomB, kBdy, kDimL, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimP},
                                            DimUpdate{kBdy, kInt, kDimP}));
  }

  // Non-empty target dimension A: infers E/I=A and E/B=L on the target side.
  {
    RelateGeometry rel_a(pt.get());
    RelateGeometry rel_b(poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addPointOnGeometry(kGeomA, kInt, kDimA, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimP},
                                            DimUpdate{kExt, kInt, kDimA},
                                            DimUpdate{kExt, kBdy, kDimL}));
  }
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(pt.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addPointOnGeometry(kGeomB, kBdy, kDimA, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kBdy, kInt, kDimP},
                                            DimUpdate{kInt, kExt, kDimA},
                                            DimUpdate{kBdy, kExt, kDimL}));
  }

  // Unknown dimTarget on non-empty target throws IllegalStateException.
  {
    RelateGeometry rel_a(pt.get());
    RelateGeometry rel_b(poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_THROW(tc.addPointOnGeometry(kGeomA, kInt, kDimFalse, &c),
                 IllegalStateException);
    EXPECT_THROW(tc.addPointOnGeometry(kGeomB, kInt, 99, &c),
                 IllegalStateException);
  }
}

TEST_F(TopologyComputerTest, AddLineEndOnGeometryAllBranches) {
  const CoordinateXY c(1.0, 1.0);
  auto pt = ReadWkt("POINT (1 1)");
  auto line = ReadWkt("LINESTRING (0 0, 2 2)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto empty_line = ReadWkt("LINESTRING EMPTY");

  // Target empty -> returns after recording line end point interaction.
  {
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(empty_line.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addLineEndOnGeometry(kGeomA, kBdy, kExt, kDimA, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kBdy, kExt, kDimP}));
  }
  {
    RelateGeometry rel_a(empty_line.get());
    RelateGeometry rel_b(line.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addLineEndOnGeometry(kGeomB, kInt, kExt, kDimL, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kExt, kInt, kDimP}));
  }

  // Target dimension P -> only records point interaction.
  {
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(pt.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addLineEndOnGeometry(kGeomA, kBdy, kInt, kDimP, &c);
    tc.addLineEndOnGeometry(kGeomB, kBdy, kExt, kDimP, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kBdy, kInt, kDimP},
                                            DimUpdate{kExt, kBdy, kDimP}));
  }

  // Target dimension L:
  // - locTarget == EXTERIOR adds (INTERIOR, EXTERIOR, L)
  // - locTarget != EXTERIOR adds only the endpoint entry
  {
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(line.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addLineEndOnGeometry(kGeomA, kBdy, kExt, kDimL, &c);
    tc.addLineEndOnGeometry(kGeomB, kBdy, kExt, kDimL, &c);
    tc.addLineEndOnGeometry(kGeomA, kBdy, kInt, kDimL, &c);
    tc.addLineEndOnGeometry(kGeomB, kInt, kBdy, kDimL, &c);

    EXPECT_THAT(
        pred.updates(),
        ElementsAre(DimUpdate{kBdy, kExt, kDimP}, DimUpdate{kInt, kExt, kDimL},
                    DimUpdate{kExt, kBdy, kDimP}, DimUpdate{kExt, kInt, kDimL},
                    DimUpdate{kBdy, kInt, kDimP},
                    DimUpdate{kBdy, kInt, kDimP}));
  }

  // Target dimension A:
  // - locTarget == BOUNDARY adds only endpoint entry
  // - locTarget == INTERIOR / EXTERIOR also adds (I, locArea, L) & (E, locArea,
  // A)
  {
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addLineEndOnGeometry(kGeomA, kBdy, kBdy, kDimA, &c);
    tc.addLineEndOnGeometry(kGeomA, kBdy, kInt, kDimA, &c);
    tc.addLineEndOnGeometry(kGeomA, kInt, kExt, kDimA, &c);

    EXPECT_THAT(
        pred.updates(),
        ElementsAre(DimUpdate{kBdy, kBdy, kDimP}, DimUpdate{kBdy, kInt, kDimP},
                    DimUpdate{kInt, kInt, kDimL}, DimUpdate{kExt, kInt, kDimA},
                    DimUpdate{kInt, kExt, kDimP}, DimUpdate{kInt, kExt, kDimL},
                    DimUpdate{kExt, kExt, kDimA}));
  }
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(line.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addLineEndOnGeometry(kGeomB, kBdy, kInt, kDimA, &c);
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kBdy, kDimP},
                                            DimUpdate{kInt, kInt, kDimL},
                                            DimUpdate{kInt, kExt, kDimA}));
  }

  // Unknown dimTarget throws IllegalStateException.
  {
    RelateGeometry rel_a(line.get());
    RelateGeometry rel_b(poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_THROW(tc.addLineEndOnGeometry(kGeomA, kBdy, kInt, kDimFalse, &c),
                 IllegalStateException);
    EXPECT_THROW(tc.addLineEndOnGeometry(kGeomB, kBdy, kInt, 42, &c),
                 IllegalStateException);
  }
}

TEST_F(TopologyComputerTest, AddAreaVertexExteriorAndPointTargets) {
  const CoordinateXY c(1.0, 1.0);
  auto pt = ReadWkt("POINT (1 1)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  // locTarget == EXTERIOR returns immediately before dimension switch.
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(pt.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addAreaVertex(kGeomA, kBdy, kExt, kDimFalse, &c);
    tc.addAreaVertex(kGeomA, kInt, kExt, kDimP, &c);
    tc.addAreaVertex(kGeomB, kBdy, kExt, kDimA, &c);
    tc.addAreaVertex(kGeomB, kInt, kExt, kDimL, &c);

    EXPECT_THAT(
        pred.updates(),
        ElementsAre(DimUpdate{kInt, kExt, kDimA}, DimUpdate{kBdy, kExt, kDimL},
                    DimUpdate{kExt, kExt, kDimA}, DimUpdate{kInt, kExt, kDimA},
                    DimUpdate{kExt, kInt, kDimA}, DimUpdate{kExt, kBdy, kDimL},
                    DimUpdate{kExt, kExt, kDimA},
                    DimUpdate{kExt, kInt, kDimA}));
  }

  // Target dimension P (locTarget == INTERIOR).
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(pt.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addAreaVertex(kGeomA, kBdy, kInt, kDimP, &c);
    tc.addAreaVertex(kGeomA, kInt, kInt, kDimP, &c);
    tc.addAreaVertex(kGeomB, kBdy, kInt, kDimP, &c);
    tc.addAreaVertex(kGeomB, kInt, kInt, kDimP, &c);

    EXPECT_THAT(
        pred.updates(),
        ElementsAre(
            // kGeomA, locArea=BOUNDARY
            DimUpdate{kBdy, kInt, kDimP}, DimUpdate{kInt, kExt, kDimA},
            DimUpdate{kBdy, kExt, kDimL}, DimUpdate{kExt, kExt, kDimA},
            // kGeomA, locArea=INTERIOR
            DimUpdate{kInt, kInt, kDimP}, DimUpdate{kInt, kExt, kDimA},
            // kGeomB, locArea=BOUNDARY
            DimUpdate{kInt, kBdy, kDimP}, DimUpdate{kExt, kInt, kDimA},
            DimUpdate{kExt, kBdy, kDimL}, DimUpdate{kExt, kExt, kDimA},
            // kGeomB, locArea=INTERIOR
            DimUpdate{kInt, kInt, kDimP}, DimUpdate{kExt, kInt, kDimA}));
  }
}

TEST_F(TopologyComputerTest, AddAreaVertexLineAndAreaTargetsAndErrors) {
  const CoordinateXY c(1.0, 1.0);
  auto line = ReadWkt("LINESTRING (0 0, 2 2)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  // Target dimension L.
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(line.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addAreaVertex(kGeomA, kBdy, kBdy, kDimL, &c);
    tc.addAreaVertex(kGeomA, kInt, kInt, kDimL, &c);
    tc.addAreaVertex(kGeomB, kBdy, kInt, kDimL, &c);
    tc.addAreaVertex(kGeomB, kInt, kBdy, kDimL, &c);

    EXPECT_THAT(
        pred.updates(),
        ElementsAre(
            // kGeomA, locArea=BOUNDARY -> only P
            DimUpdate{kBdy, kBdy, kDimP},
            // kGeomA, locArea=INTERIOR -> P plus (I, E, A)
            DimUpdate{kInt, kInt, kDimP}, DimUpdate{kInt, kExt, kDimA},
            // kGeomB, locArea=BOUNDARY
            DimUpdate{kInt, kBdy, kDimP},
            // kGeomB, locArea=INTERIOR
            DimUpdate{kBdy, kInt, kDimP}, DimUpdate{kExt, kInt, kDimA}));
  }

  // Target dimension A via addAreaVertex and direct addAreaVertexOnArea.
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    // locTarget == BOUNDARY: locArea == BOUNDARY vs INTERIOR.
    tc.addAreaVertex(kGeomA, kBdy, kBdy, kDimA, &c);
    tc.addAreaVertex(kGeomA, kInt, kBdy, kDimA, &c);
    // locTarget == INTERIOR: locArea == BOUNDARY vs INTERIOR.
    tc.addAreaVertex(kGeomA, kBdy, kInt, kDimA, &c);
    tc.addAreaVertex(kGeomA, kInt, kInt, kDimA, &c);
    // Direct addAreaVertexOnArea with locTarget == EXTERIOR and isAreaA == B.
    tc.addAreaVertexOnArea(kGeomB, kBdy, kExt, &c);
    tc.addAreaVertexOnArea(kGeomB, kInt, kExt, &c);

    EXPECT_THAT(pred.updates(),
                ElementsAre(
                    // (B, B)
                    DimUpdate{kBdy, kBdy, kDimP},
                    // (I, B)
                    DimUpdate{kInt, kInt, kDimA}, DimUpdate{kInt, kBdy, kDimL},
                    DimUpdate{kInt, kExt, kDimA},
                    // (B, I)
                    DimUpdate{kInt, kInt, kDimA}, DimUpdate{kBdy, kInt, kDimL},
                    DimUpdate{kExt, kInt, kDimA},
                    // (I, I)
                    DimUpdate{kInt, kInt, kDimA},
                    // B on A exterior (BOUNDARY)
                    DimUpdate{kExt, kInt, kDimA}, DimUpdate{kExt, kBdy, kDimL},
                    DimUpdate{kExt, kExt, kDimA},
                    // B on A exterior (INTERIOR)
                    DimUpdate{kExt, kInt, kDimA}));
  }

  // Unknown dimTarget when locTarget != EXTERIOR throws IllegalStateException.
  {
    RelateGeometry rel_a(poly.get());
    RelateGeometry rel_b(poly.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    EXPECT_THROW(tc.addAreaVertex(kGeomA, kBdy, kInt, kDimFalse, &c),
                 IllegalStateException);
    EXPECT_THROW(tc.addAreaVertex(kGeomB, kInt, kBdy, 7, &c),
                 IllegalStateException);
  }
}

TEST_F(TopologyComputerTest, AddIntersectionSameGeometryAndDimensionBranches) {
  auto line_a = ReadWkt("LINESTRING (0 1, 2 1)");
  auto line_b = ReadWkt("LINESTRING (1 0, 1 2)");
  auto poly_a = ReadWkt("POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))");
  auto poly_b = ReadWkt("POLYGON ((1 1, 1 3, 3 3, 3 1, 1 1))");

  const CoordinateXY node_pt(1.0, 1.0);
  const CoordinateXY a_v0(0.0, 1.0);
  const CoordinateXY a_v1(2.0, 1.0);
  const CoordinateXY b_v0(1.0, 0.0);
  const CoordinateXY b_v1(1.0, 2.0);

  // 1. Same geometry (A/A or B/B) skips updateIntersectionAB.
  {
    RelateGeometry rel_a(line_a.get());
    RelateGeometry rel_b(line_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomA, kDimL, 2, 0, nullptr, false,
                                       &b_v0, node_pt, &b_v1));
    tc.addIntersection(new NodeSection(kGeomB, kDimL, 1, 0, nullptr, false,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimL, 2, 0, nullptr, false,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), IsEmpty());
  }

  // 2. Line/Line AB intersection updates node location only (no Area/Area
  // cross).
  {
    RelateGeometry rel_a(line_a.get());
    RelateGeometry rel_b(line_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimL, 1, 0, nullptr, false,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimP}));
  }

  // 3. Line/Area AB intersection skips updateAreaAreaCross.
  {
    RelateGeometry rel_a(line_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), true,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kBdy, kDimP}));
  }

  // 4. Area/Area proper crossing sets I/I = A and node location.
  {
    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), false,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), false,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimA},
                                            DimUpdate{kBdy, kBdy, kDimP}));
  }
}

TEST_F(TopologyComputerTest, AddIntersectionAreaAreaAtVertexCrossingVsTouch) {
  auto poly_a = ReadWkt("POLYGON ((0 0, 0 1, 2 1, 2 0, 0 0))");
  auto poly_b = ReadWkt("POLYGON ((1 0, 1 2, 3 2, 3 0, 1 0))");

  const CoordinateXY node_pt(1.0, 1.0);
  // Horizontal edge through (1,1): a0=(0,1) -> (1,1) -> a1=(2,1).
  const CoordinateXY a_v0(0.0, 1.0);
  const CoordinateXY a_v1(2.0, 1.0);

  // Vertex intersection where B crosses A: b0=(1,0) -> (1,1) -> b1=(1,2).
  {
    const CoordinateXY b_v0(1.0, 0.0);
    const CoordinateXY b_v1(1.0, 2.0);
    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), true,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), true,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimA},
                                            DimUpdate{kBdy, kBdy, kDimP}));
  }

  // Vertex touch where B does not cross A: b0=(0,2) -> (1,1) -> b1=(2,2).
  {
    const CoordinateXY b_v0(0.0, 2.0);
    const CoordinateXY b_v1(2.0, 2.0);
    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), true,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), true,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kBdy, kBdy, kDimP}));
  }

  // Collinear vertex touch: b0=(0,1) -> (1,1) -> b1=(1,2) is not a crossing.
  {
    const CoordinateXY b_v0(0.0, 1.0);
    const CoordinateXY b_v1(1.0, 2.0);
    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), false,
                                       &a_v0, node_pt, &a_v1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), true,
                                       &b_v0, node_pt, &b_v1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kBdy, kBdy, kDimP}));
  }

  // Asymmetric proper flag (A is at vertex, B is not at vertex): crossing vs
  // touch.
  {
    const CoordinateXY va0(1.0, 0.0);
    const CoordinateXY va1(1.0, 2.0);
    const CoordinateXY vb0(0.0, 1.0);
    const CoordinateXY vb1(2.0, 1.0);
    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), true,
                                       &va0, node_pt, &va1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), false,
                                       &vb0, node_pt, &vb1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimA},
                                            DimUpdate{kBdy, kBdy, kDimP}));
  }
  {
    const CoordinateXY va0(0.0, 2.0);
    const CoordinateXY va1(2.0, 2.0);
    const CoordinateXY vb0(0.0, 1.0);
    const CoordinateXY vb1(2.0, 1.0);
    RelateGeometry rel_a(poly_a.get());
    RelateGeometry rel_b(poly_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), true,
                                       &va0, node_pt, &va1),
                       new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), false,
                                       &vb0, node_pt, &vb1));
    EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kBdy, kBdy, kDimP}));
  }
}

TEST_F(TopologyComputerTest, EvaluateNodesEmptyAndNonInteractingSkipped) {
  auto line_a = ReadWkt("LINESTRING (0 0, 2 2, 0 2, 2 0)");
  auto line_b = ReadWkt("LINESTRING (10 10, 12 12)");

  // Empty node map: evaluateNodes is a no-op.
  {
    RelateGeometry rel_a(line_a.get());
    RelateGeometry rel_b(line_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);
    pred.ClearUpdates();

    tc.evaluateNodes();
    EXPECT_THAT(pred.updates(), IsEmpty());
  }

  // Self-intersection node (A with A only) has hasInteractionAB() == false,
  // while a subsequent AB node in the same nodeMap IS evaluated.
  {
    const CoordinateXY self_pt(0.5, 0.5);
    const CoordinateXY sv0(0.0, 0.0);
    const CoordinateXY sv1(1.0, 1.0);
    const CoordinateXY sv2(0.0, 1.0);
    const CoordinateXY sv3(1.0, 0.0);

    const CoordinateXY ab_pt(2.0, 2.0);
    const CoordinateXY av0(1.0, 2.0);
    const CoordinateXY av1(3.0, 2.0);
    const CoordinateXY bv0(2.0, 1.0);
    const CoordinateXY bv1(2.0, 3.0);

    RelateGeometry rel_a(line_a.get());
    RelateGeometry rel_b(line_b.get());
    RecordingTopologyPredicate pred;
    TopologyComputer tc(pred, rel_a, rel_b);

    tc.addIntersection(new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false,
                                       &sv0, self_pt, &sv1),
                       new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false,
                                       &sv2, self_pt, &sv3));
    pred.ClearUpdates();
    tc.evaluateNodes();
    EXPECT_THAT(pred.updates(), IsEmpty());

    // Now add an AB node and confirm evaluateNodes skips self_pt and evaluates
    // ab_pt.
    tc.addIntersection(
        new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false, &av0, ab_pt, &av1),
        new NodeSection(kGeomB, kDimL, 1, 0, nullptr, false, &bv0, ab_pt,
                        &bv1));
    pred.ClearUpdates();
    tc.evaluateNodes();
    EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kExt, kDimL}));
    EXPECT_THAT(pred.updates(), Contains(DimUpdate{kExt, kInt, kDimL}));
  }
}

TEST_F(TopologyComputerTest, EvaluateNodesLineLineReusesNodeAndSkipsAreaEdges) {
  auto line_a = ReadWkt("LINESTRING (0 1, 2 1)");
  auto line_b = ReadWkt("LINESTRING (1 0, 1 2)");
  const CoordinateXY node_pt(1.0, 1.0);
  const CoordinateXY a_v0(0.0, 1.0);
  const CoordinateXY a_v1(2.0, 1.0);
  const CoordinateXY b_v0(1.0, 0.0);
  const CoordinateXY b_v1(1.0, 2.0);
  const CoordinateXY b_extra(1.5, 1.5);

  RelateGeometry rel_a(line_a.get());
  RelateGeometry rel_b(line_b.get());
  RecordingTopologyPredicate pred;
  TopologyComputer tc(pred, rel_a, rel_b);

  // Add two intersections at the exact same node_pt to exercise getNodeSections
  // lookup on existing nodeMap entries, plus verify evaluateNodeEdges skips
  // Dimension::A updates for Line/Line.
  tc.addIntersection(new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false, &a_v0,
                                     node_pt, &a_v1),
                     new NodeSection(kGeomB, kDimL, 1, 0, nullptr, false, &b_v0,
                                     node_pt, &b_v1));
  tc.addIntersection(new NodeSection(kGeomB, kDimL, 1, 0, nullptr, true, &b_v1,
                                     node_pt, &b_extra),
                     new NodeSection(kGeomB, kDimL, 2, 0, nullptr, true,
                                     &b_extra, node_pt, &b_v0));

  pred.ClearUpdates();
  tc.evaluateNodes();

  EXPECT_THAT(pred.updates(), Not(IsEmpty()));
  for (const DimUpdate& u : pred.updates()) {
    EXPECT_EQ(u.dim, kDimL);
  }
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kExt, kDimL}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kExt, kInt, kDimL}));
}

TEST_F(TopologyComputerTest, EvaluateNodesAreaAreaEvaluatesSidesAndOnEdges) {
  // CW squares [0,2]x[0,2] and [1,3]x[1,3] crossing at (1, 2).
  auto poly_a = ReadWkt("POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))");
  auto poly_b = ReadWkt("POLYGON ((1 1, 1 3, 3 3, 3 1, 1 1))");

  const CoordinateXY node_pt(1.0, 2.0);
  // A top edge: (0,2) -> (1,2) -> (2,2), interior below (on right).
  const CoordinateXY a_v0(0.0, 2.0);
  const CoordinateXY a_v1(2.0, 2.0);
  // B left edge: (1,1) -> (1,2) -> (1,3), interior right (on right).
  const CoordinateXY b_v0(1.0, 1.0);
  const CoordinateXY b_v1(1.0, 3.0);

  RelateGeometry rel_a(poly_a.get());
  RelateGeometry rel_b(poly_b.get());
  RecordingTopologyPredicate pred;
  TopologyComputer tc(pred, rel_a, rel_b);

  tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_a.get(), false,
                                     &a_v0, node_pt, &a_v1),
                     new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), false,
                                     &b_v0, node_pt, &b_v1));

  pred.ClearUpdates();
  tc.evaluateNodes();

  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kExt, kInt, kDimA}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kInt, kDimA}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kExt, kDimA}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kExt, kExt, kDimA}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kBdy, kInt, kDimL}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kExt, kBdy, kDimL}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kBdy, kExt, kDimL}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kBdy, kDimL}));
}

TEST_F(TopologyComputerTest, EvaluateNodesWithOverlappingPolygonCollection) {
  // Overlapping polygons in a GeometryCollection make the inner polygon's
  // boundary vertex lie in Location::INTERIOR of the collection.
  auto gc_a = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0)), "
      "POLYGON ((1 1, 1 3, 3 3, 3 1, 1 1)))");
  const Geometry* inner_poly_a = gc_a->getGeometryN(1);
  auto poly_b = ReadWkt("POLYGON ((2 2, 2 5, 5 5, 5 2, 2 2))");

  const CoordinateXY node_pt(2.0, 3.0);
  // Inner A top edge (1,3)->(2,3)->(3,3) (CW); B left edge (2,2)->(2,3)->(2,5)
  // (CW).
  const CoordinateXY a_v0(1.0, 3.0);
  const CoordinateXY a_v1(3.0, 3.0);
  const CoordinateXY b_v0(2.0, 2.0);
  const CoordinateXY b_v1(2.0, 5.0);

  RelateGeometry rel_a(gc_a.get());
  RelateGeometry rel_b(poly_b.get());
  RecordingTopologyPredicate pred;
  TopologyComputer tc(pred, rel_a, rel_b);
  pred.ClearUpdates();

  tc.addIntersection(new NodeSection(kGeomA, kDimA, 2, 0, inner_poly_a, false,
                                     &a_v0, node_pt, &a_v1),
                     new NodeSection(kGeomB, kDimA, 1, 0, poly_b.get(), false,
                                     &b_v0, node_pt, &b_v1));
  EXPECT_THAT(pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimA},
                                          DimUpdate{kInt, kBdy, kDimP}));

  pred.ClearUpdates();
  tc.evaluateNodes();

  // Because isAreaInteriorA is true at (2,3), all A locations around the node
  // become INTERIOR, so no A=BOUNDARY or A=EXTERIOR updates appear from this
  // node.
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kInt, kDimA}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kExt, kDimA}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kBdy, kDimL}));
  EXPECT_THAT(pred.updates(), Contains(DimUpdate{kInt, kExt, kDimL}));
  for (const DimUpdate& u : pred.updates()) {
    EXPECT_EQ(u.loc_a, kInt);
  }

  // Symmetric case: overlapping polygon collection is B (isAreaInteriorB true).
  {
    RelateGeometry rel_sym_a(poly_b.get());
    RelateGeometry rel_sym_b(gc_a.get());
    RecordingTopologyPredicate sym_pred;
    TopologyComputer sym_tc(sym_pred, rel_sym_a, rel_sym_b);
    sym_pred.ClearUpdates();

    sym_tc.addIntersection(new NodeSection(kGeomA, kDimA, 1, 0, poly_b.get(),
                                           false, &b_v0, node_pt, &b_v1),
                           new NodeSection(kGeomB, kDimA, 2, 0, inner_poly_a,
                                           false, &a_v0, node_pt, &a_v1));
    EXPECT_THAT(sym_pred.updates(), ElementsAre(DimUpdate{kInt, kInt, kDimA},
                                                DimUpdate{kBdy, kInt, kDimP}));

    sym_pred.ClearUpdates();
    sym_tc.evaluateNodes();
    EXPECT_THAT(sym_pred.updates(), Contains(DimUpdate{kInt, kInt, kDimA}));
    EXPECT_THAT(sym_pred.updates(), Contains(DimUpdate{kExt, kInt, kDimA}));
    EXPECT_THAT(sym_pred.updates(), Contains(DimUpdate{kBdy, kInt, kDimL}));
    EXPECT_THAT(sym_pred.updates(), Contains(DimUpdate{kExt, kInt, kDimL}));
    for (const DimUpdate& u : sym_pred.updates()) {
      EXPECT_EQ(u.loc_b, kInt);
    }
  }
}

TEST_F(TopologyComputerTest, EvaluateNodesShortCircuitsWhenResultKnown) {
  auto line_a = ReadWkt("LINESTRING (0 1, 2 1, 6 5)");
  auto line_b = ReadWkt("LINESTRING (1 0, 1 2, 4 5, 6 5)");

  // Node 1 at (1, 1): crossing lines -> ON updates are I/E and E/I (no I/I).
  const CoordinateXY pt1(1.0, 1.0);
  const CoordinateXY a1_v0(0.0, 1.0);
  const CoordinateXY a1_v1(2.0, 1.0);
  const CoordinateXY b1_v0(1.0, 0.0);
  const CoordinateXY b1_v1(1.0, 2.0);

  // Node 2 at (5, 5): collinear overlapping lines -> ON updates include I/I.
  const CoordinateXY pt2(5.0, 5.0);
  const CoordinateXY a2_v0(4.0, 5.0);
  const CoordinateXY a2_v1(6.0, 5.0);
  const CoordinateXY b2_v0(4.0, 5.0);
  const CoordinateXY b2_v1(6.0, 5.0);

  RelateGeometry rel_a(line_a.get());
  RelateGeometry rel_b(line_b.get());
  RecordingTopologyPredicate pred;
  TopologyComputer tc(pred, rel_a, rel_b);

  tc.addIntersection(
      new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false, &a1_v0, pt1, &a1_v1),
      new NodeSection(kGeomB, kDimL, 1, 0, nullptr, false, &b1_v0, pt1,
                      &b1_v1));
  tc.addIntersection(
      new NodeSection(kGeomA, kDimL, 1, 0, nullptr, false, &a2_v0, pt2, &a2_v1),
      new NodeSection(kGeomB, kDimL, 1, 0, nullptr, false, &b2_v0, pt2,
                      &b2_v1));

  // Become known as soon as node 1 records any Dimension::L update.
  pred.ClearUpdates();
  pred.set_know_on_dim(kDimL);
  tc.evaluateNodes();

  EXPECT_TRUE(tc.isResultKnown());
  EXPECT_TRUE(tc.getResult());
  // Node 2 (at (5,5), ordered after (1,1) in nodeMap) would emit {I, I, L};
  // early return prevents it from being evaluated.
  EXPECT_THAT(pred.updates(), Not(Contains(DimUpdate{kInt, kInt, kDimL})));
}

TEST_F(TopologyComputerTest, RelateNGIntegrationExercisesEndToEndMatrices) {
  // Mixed GeometryCollection with overlapping polygons and an interior line.
  auto a = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0)), "
      "POLYGON ((1 1, 1 3, 3 3, 3 1, 1 1)), "
      "LINESTRING (0 0, 4 4))");
  auto b = ReadWkt("LINESTRING (2 0, 2 5)");
  std::unique_ptr<IntersectionMatrix> im = RelateNG::relate(a.get(), b.get());
  EXPECT_EQ(im->toString(), "1F2001102");

  // Self-crossing line A tested against a segment coincident with one arm of A.
  auto self_cross = ReadWkt("LINESTRING (0 0, 2 2, 0 2, 2 0)");
  auto single_seg = ReadWkt("LINESTRING (0 0, 2 2)");
  std::unique_ptr<IntersectionMatrix> im_cross =
      RelateNG::relate(self_cross.get(), single_seg.get());
  EXPECT_EQ(im_cross->toString(), "101F00FF2");

  // Zero-length LineString inside Polygon.
  auto zero_line = ReadWkt("LINESTRING (2 2, 2 2)");
  auto box = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  EXPECT_TRUE(RelateNG::within(zero_line.get(), box.get()));
  EXPECT_TRUE(RelateNG::contains(box.get(), zero_line.get()));
  EXPECT_TRUE(RelateNG::intersects(zero_line.get(), box.get()));
}

}  // namespace
}  // namespace geos_google
