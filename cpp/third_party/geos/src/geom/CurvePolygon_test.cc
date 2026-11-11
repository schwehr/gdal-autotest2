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

// Unit tests for third_party/geos/src/geom/CurvePolygon.cpp and
// third_party/geos/include/geos/geom/CurvePolygon.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/CurvePolygon.cpp

#include "third_party/geos/include/geos/geom/CurvePolygon.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateFilter;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_CURVEPOLYGON;
using geos::geom::LinearRing;
using geos::geom::MultiCurve;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::io::WKTReader;
using geos::io::WKTWriter;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;
using ::testing::HasSubstr;

std::unique_ptr<CoordinateSequence> MakeXySeq(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeXyzSeq(
    std::initializer_list<Coordinate> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeXyzmSeq(
    std::initializer_list<CoordinateXYZM> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CompoundCurve> MakeMixedHoleCompoundCurve(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(factory.createLineString(
      MakeXySeq({{4.0, 0.0}, {6.0, 0.0}, {6.0, 2.0}})));
  sections.push_back(factory.createCircularString(
      MakeXySeq({{6.0, 2.0}, {5.0, 3.0}, {4.0, 2.0}})));
  sections.push_back(
      factory.createLineString(MakeXySeq({{4.0, 2.0}, {4.0, 0.0}})));
  return factory.createCompoundCurve(std::move(sections));
}

std::unique_ptr<CompoundCurve> MakeLinearClosedCompoundCurve(
    const GeometryFactory& factory, double min_x, double min_y, double max_x,
    double max_y) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(factory.createLineString(
      MakeXySeq({{min_x, min_y}, {max_x, min_y}, {max_x, max_y}})));
  sections.push_back(factory.createLineString(
      MakeXySeq({{max_x, max_y}, {min_x, max_y}, {min_x, min_y}})));
  return factory.createCompoundCurve(std::move(sections));
}

std::unique_ptr<CurvePolygon> MakeMultiHoleCurvePolygon(
    const GeometryFactory& factory) {
  std::unique_ptr<Curve> shell = factory.createCircularString(MakeXySeq(
      {{-10.0, 0.0}, {0.0, 10.0}, {10.0, 0.0}, {0.0, -10.0}, {-10.0, 0.0}}));
  std::vector<std::unique_ptr<Curve>> holes;
  holes.push_back(factory.createLinearRing(MakeXySeq(
      {{-5.0, -5.0}, {-2.0, -5.0}, {-2.0, -2.0}, {-5.0, -2.0}, {-5.0, -5.0}})));
  holes.push_back(factory.createCircularString(MakeXySeq(
      {{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}, {2.0, -1.0}, {1.0, 0.0}})));
  holes.push_back(MakeMixedHoleCompoundCurve(factory));
  return factory.createCurvePolygon(std::move(shell), std::move(holes));
}

class ShiftXFilter final : public CoordinateFilter {
 public:
  explicit ShiftXFilter(double dx) : dx_(dx) {}
  void filter_rw(CoordinateXY* coord) const override { coord->x += dx_; }

 private:
  double dx_;
};

TEST(CurvePolygonTest, TypeMetadataAndEmptyFactoryVariants) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  struct EmptyDimCase {
    bool has_z;
    bool has_m;
    std::uint8_t expected_coord_dim;
  };
  constexpr EmptyDimCase kCases[] = {
      {false, false, 2},
      {true, false, 3},
      {false, true, 3},
      {true, true, 4},
  };

  for (const auto& test_case : kCases) {
    SCOPED_TRACE(test_case.has_m);
    SCOPED_TRACE(test_case.has_z);
    std::unique_ptr<CurvePolygon> empty_cp =
        factory->createCurvePolygon(test_case.has_z, test_case.has_m);
    ASSERT_NE(empty_cp, nullptr);
    EXPECT_TRUE(empty_cp->isEmpty());
    EXPECT_EQ(empty_cp->getGeometryType(), "CurvePolygon");
    EXPECT_EQ(empty_cp->getGeometryTypeId(), GEOS_CURVEPOLYGON);
    EXPECT_EQ(empty_cp->getDimension(), Dimension::A);
    EXPECT_EQ(empty_cp->getBoundaryDimension(), 1);
    EXPECT_EQ(empty_cp->getNumInteriorRing(), 0);
    EXPECT_EQ(empty_cp->getNumPoints(), 0);
    EXPECT_DOUBLE_EQ(empty_cp->getArea(), 0.0);
    EXPECT_DOUBLE_EQ(empty_cp->getLength(), 0.0);
    EXPECT_FALSE(empty_cp->hasCurvedComponents());
    EXPECT_EQ(empty_cp->hasZ(), test_case.has_z);
    EXPECT_EQ(empty_cp->hasM(), test_case.has_m);
    EXPECT_EQ(empty_cp->getCoordinateDimension(), test_case.expected_coord_dim);
    EXPECT_EQ(empty_cp->getCoordinate(), nullptr);
    EXPECT_TRUE(empty_cp->getCoordinates()->isEmpty());
  }

  // Passing a null shell creates an empty CurvePolygon backed by an empty ring.
  std::unique_ptr<CurvePolygon> null_shell_cp =
      factory->createCurvePolygon(std::unique_ptr<Curve>(nullptr));
  ASSERT_NE(null_shell_cp, nullptr);
  EXPECT_TRUE(null_shell_cp->isEmpty());
  EXPECT_EQ(null_shell_cp->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_DOUBLE_EQ(null_shell_cp->getArea(), 0.0);

  // Empty CompoundCurve shell creates an empty CurvePolygon; an empty
  // CompoundCurve is not closed, so getArea() throws IllegalArgumentException.
  std::unique_ptr<CurvePolygon> empty_compound_cp =
      factory->createCurvePolygon(factory->createCompoundCurve());
  ASSERT_NE(empty_compound_cp, nullptr);
  EXPECT_TRUE(empty_compound_cp->isEmpty());
  EXPECT_EQ(empty_compound_cp->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_FALSE(empty_compound_cp->hasCurvedComponents());
  EXPECT_TRUE(empty_compound_cp->getCoordinates()->isEmpty());
  EXPECT_THROW(empty_compound_cp->getArea(), IllegalArgumentException);
}

TEST(CurvePolygonTest, ConstructionAndAreaValidationErrors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty shell with a non-empty hole is rejected by SurfaceImpl.
  {
    std::unique_ptr<Curve> empty_shell =
        factory->createLinearRing(false, false);
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(factory->createLinearRing(MakeXySeq(
        {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}})));
    EXPECT_THROW(
        {
          try {
            factory->createCurvePolygon(std::move(empty_shell),
                                        std::move(holes));
          } catch (const IllegalArgumentException& e) {
            EXPECT_THAT(e.what(), HasSubstr("shell is empty"));
            throw;
          }
        },
        IllegalArgumentException);
  }

  // Null hole entry is rejected by SurfaceImpl.
  {
    std::unique_ptr<Curve> shell = factory->createLinearRing(MakeXySeq(
        {{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 5.0}, {0.0, 0.0}}));
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(nullptr);
    EXPECT_THROW(
        {
          try {
            factory->createCurvePolygon(std::move(shell), std::move(holes));
          } catch (const IllegalArgumentException& e) {
            EXPECT_THAT(e.what(), HasSubstr("null"));
            throw;
          }
        },
        IllegalArgumentException);
  }

  // Unclosed shell passes SurfaceImpl construction, so getArea() throws.
  {
    std::unique_ptr<Curve> unclosed_shell = factory->createLineString(
        MakeXySeq({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}}));
    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(unclosed_shell));
    EXPECT_THROW(cp->getArea(), IllegalArgumentException);
  }

  // Closed shell with an unclosed hole throws in getArea().
  {
    std::unique_ptr<Curve> shell = factory->createLinearRing(MakeXySeq(
        {{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 5.0}, {0.0, 0.0}}));
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(factory->createCircularString(
        MakeXySeq({{1.0, 1.0}, {2.0, 2.0}, {3.0, 1.0}})));
    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(shell), std::move(holes));
    EXPECT_THROW(cp->getArea(), IllegalArgumentException);
  }
}

TEST(CurvePolygonTest, GetCoordinatesSimpleAndCompoundHolesAndZ) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Shell only (no holes).
  std::unique_ptr<CurvePolygon> shell_only =
      factory->createCurvePolygon(factory->createCircularString(MakeXySeq(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  std::unique_ptr<CoordinateSequence> shell_coords =
      shell_only->getCoordinates();
  ASSERT_EQ(shell_coords->size(), 5);
  EXPECT_EQ(shell_coords->getAt<CoordinateXY>(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(shell_coords->getAt<CoordinateXY>(2), CoordinateXY(2.0, 0.0));

  // Shell + LinearRing hole (SimpleCurve) + CircularString hole (SimpleCurve)
  // + CompoundCurve hole (non-SimpleCurve branch in getCoordinates()).
  std::unique_ptr<CurvePolygon> multi_hole =
      MakeMultiHoleCurvePolygon(*factory);
  std::unique_ptr<CoordinateSequence> coords = multi_hole->getCoordinates();
  ASSERT_EQ(coords->size(), 23);
  EXPECT_EQ(multi_hole->getNumPoints(), 23);

  // Shell (0..4)
  EXPECT_EQ(coords->getAt<CoordinateXY>(0), CoordinateXY(-10.0, 0.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(4), CoordinateXY(-10.0, 0.0));
  // Hole 0 LinearRing (5..9)
  EXPECT_EQ(coords->getAt<CoordinateXY>(5), CoordinateXY(-5.0, -5.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(7), CoordinateXY(-2.0, -2.0));
  // Hole 1 CircularString (10..14)
  EXPECT_EQ(coords->getAt<CoordinateXY>(10), CoordinateXY(1.0, 0.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(12), CoordinateXY(3.0, 0.0));
  // Hole 2 CompoundCurve (15..22)
  EXPECT_EQ(coords->getAt<CoordinateXY>(15), CoordinateXY(4.0, 0.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(18), CoordinateXY(6.0, 2.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(19), CoordinateXY(5.0, 3.0));
  EXPECT_EQ(coords->getAt<CoordinateXY>(22), CoordinateXY(4.0, 0.0));

  // 3D XYZ coordinates are preserved across shell and hole.
  std::unique_ptr<Curve> shell_z =
      factory->createCircularString(MakeXyzSeq({{0.0, 0.0, 10.0},
                                                {1.0, 1.0, 20.0},
                                                {2.0, 0.0, 30.0},
                                                {1.0, -1.0, 40.0},
                                                {0.0, 0.0, 10.0}}));
  std::vector<std::unique_ptr<Curve>> holes_z;
  holes_z.push_back(factory->createLinearRing(MakeXyzSeq({{0.5, 0.0, 5.0},
                                                          {1.0, 0.5, 6.0},
                                                          {1.5, 0.0, 7.0},
                                                          {1.0, -0.5, 8.0},
                                                          {0.5, 0.0, 5.0}})));
  std::unique_ptr<CurvePolygon> cp_z =
      factory->createCurvePolygon(std::move(shell_z), std::move(holes_z));
  EXPECT_TRUE(cp_z->hasZ());
  EXPECT_FALSE(cp_z->hasM());
  EXPECT_EQ(cp_z->getCoordinateDimension(), 3);
  std::unique_ptr<CoordinateSequence> coords_z = cp_z->getCoordinates();
  ASSERT_EQ(coords_z->size(), 10);
  EXPECT_TRUE(coords_z->hasZ());
  EXPECT_DOUBLE_EQ(coords_z->getOrdinate(1, CoordinateSequence::Z), 20.0);
  EXPECT_DOUBLE_EQ(coords_z->getOrdinate(6, CoordinateSequence::Z), 6.0);

  // 4D XYZM coordinates are preserved across a CompoundCurve shell,
  // SimpleCurve hole, and CompoundCurve hole.
  std::vector<std::unique_ptr<SimpleCurve>> shell_xyzm_parts;
  shell_xyzm_parts.push_back(
      factory->createLineString(MakeXyzmSeq({{0.0, 0.0, 1.0, 100.0},
                                             {10.0, 0.0, 2.0, 200.0},
                                             {10.0, 10.0, 3.0, 300.0}})));
  shell_xyzm_parts.push_back(
      factory->createLineString(MakeXyzmSeq({{10.0, 10.0, 3.0, 300.0},
                                             {0.0, 10.0, 4.0, 400.0},
                                             {0.0, 0.0, 1.0, 100.0}})));
  std::unique_ptr<Curve> shell_xyzm =
      factory->createCompoundCurve(std::move(shell_xyzm_parts));

  std::vector<std::unique_ptr<SimpleCurve>> hole_xyzm_parts;
  hole_xyzm_parts.push_back(
      factory->createLineString(MakeXyzmSeq({{5.0, 5.0, 9.0, 900.0},
                                             {7.0, 5.0, 10.0, 1000.0},
                                             {7.0, 7.0, 11.0, 1100.0}})));
  hole_xyzm_parts.push_back(
      factory->createCircularString(MakeXyzmSeq({{7.0, 7.0, 11.0, 1100.0},
                                                 {6.0, 8.0, 12.0, 1200.0},
                                                 {5.0, 7.0, 13.0, 1300.0}})));
  hole_xyzm_parts.push_back(factory->createLineString(
      MakeXyzmSeq({{5.0, 7.0, 13.0, 1300.0}, {5.0, 5.0, 9.0, 900.0}})));

  std::vector<std::unique_ptr<Curve>> holes_xyzm;
  holes_xyzm.push_back(
      factory->createLinearRing(MakeXyzmSeq({{1.0, 1.0, 5.0, 500.0},
                                             {3.0, 1.0, 6.0, 600.0},
                                             {3.0, 3.0, 7.0, 700.0},
                                             {1.0, 3.0, 8.0, 800.0},
                                             {1.0, 1.0, 5.0, 500.0}})));
  holes_xyzm.push_back(
      factory->createCompoundCurve(std::move(hole_xyzm_parts)));

  std::unique_ptr<CurvePolygon> cp_xyzm =
      factory->createCurvePolygon(std::move(shell_xyzm), std::move(holes_xyzm));
  EXPECT_TRUE(cp_xyzm->hasZ());
  EXPECT_TRUE(cp_xyzm->hasM());
  EXPECT_EQ(cp_xyzm->getCoordinateDimension(), 4);
  std::unique_ptr<CoordinateSequence> coords_xyzm = cp_xyzm->getCoordinates();
  ASSERT_EQ(coords_xyzm->size(), 19);
  EXPECT_TRUE(coords_xyzm->hasZ());
  EXPECT_TRUE(coords_xyzm->hasM());
  EXPECT_DOUBLE_EQ(coords_xyzm->getOrdinate(1, CoordinateSequence::Z), 2.0);
  EXPECT_DOUBLE_EQ(coords_xyzm->getOrdinate(1, CoordinateSequence::M), 200.0);
  EXPECT_DOUBLE_EQ(coords_xyzm->getOrdinate(7, CoordinateSequence::Z), 6.0);
  EXPECT_DOUBLE_EQ(coords_xyzm->getOrdinate(7, CoordinateSequence::M), 600.0);
  EXPECT_DOUBLE_EQ(coords_xyzm->getOrdinate(15, CoordinateSequence::Z), 12.0);
  EXPECT_DOUBLE_EQ(coords_xyzm->getOrdinate(15, CoordinateSequence::M), 1200.0);
}

TEST(CurvePolygonTest, GetAreaCircularCompoundAndLinear) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Unit circle centered at (1, 0): area = pi regardless of orientation.
  std::unique_ptr<CurvePolygon> unit_circle =
      factory->createCurvePolygon(factory->createCircularString(MakeXySeq(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  EXPECT_NEAR(unit_circle->getArea(), std::numbers::pi, 1e-12);

  std::unique_ptr<CurvePolygon> ccw_unit_circle =
      factory->createCurvePolygon(factory->createCircularString(MakeXySeq(
          {{0.0, 0.0}, {1.0, -1.0}, {2.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}})));
  EXPECT_NEAR(ccw_unit_circle->getArea(), std::numbers::pi, 1e-12);

  // Stadium / D-shape CompoundCurve shell with linear and circular holes:
  // Rectangle [0, 4] x [-1, 1] (area 8) + right semicircle radius 1 (pi / 2)
  // minus square hole [1, 3] x [-0.5, 0.5] (area 2)
  // minus circular hole radius 0.5 centered at (4, 0) (area 0.25 * pi).
  std::vector<std::unique_ptr<SimpleCurve>> stadium_sections;
  stadium_sections.push_back(
      factory->createLineString(MakeXySeq({{0.0, -1.0}, {4.0, -1.0}})));
  stadium_sections.push_back(factory->createCircularString(
      MakeXySeq({{4.0, -1.0}, {5.0, 0.0}, {4.0, 1.0}})));
  stadium_sections.push_back(factory->createLineString(
      MakeXySeq({{4.0, 1.0}, {0.0, 1.0}, {0.0, -1.0}})));
  std::unique_ptr<Curve> stadium_shell =
      factory->createCompoundCurve(std::move(stadium_sections));

  std::vector<std::unique_ptr<Curve>> stadium_holes;
  stadium_holes.push_back(factory->createLinearRing(MakeXySeq(
      {{1.0, -0.5}, {3.0, -0.5}, {3.0, 0.5}, {1.0, 0.5}, {1.0, -0.5}})));
  stadium_holes.push_back(factory->createCircularString(MakeXySeq(
      {{3.5, 0.0}, {4.0, 0.5}, {4.5, 0.0}, {4.0, -0.5}, {3.5, 0.0}})));
  std::unique_ptr<CurvePolygon> stadium_cp = factory->createCurvePolygon(
      std::move(stadium_shell), std::move(stadium_holes));
  EXPECT_NEAR(stadium_cp->getArea(), 6.0 + 0.25 * std::numbers::pi, 1e-12);

  // Circle of radius 10 (100 * pi) minus square hole (9), circular hole of
  // radius 1 (pi), and mixed-curve CompoundCurve hole (4 + 0.5 * pi).
  std::unique_ptr<CurvePolygon> multi_hole =
      MakeMultiHoleCurvePolygon(*factory);
  EXPECT_NEAR(multi_hole->getArea(), 98.5 * std::numbers::pi - 13.0, 1e-10);

  // Purely linear CurvePolygon: 10x10 shell minus 3x3 hole = 91.
  std::unique_ptr<Curve> linear_shell = factory->createLinearRing(MakeXySeq(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  std::vector<std::unique_ptr<Curve>> linear_holes;
  linear_holes.push_back(factory->createLinearRing(
      MakeXySeq({{2.0, 2.0}, {5.0, 2.0}, {5.0, 5.0}, {2.0, 5.0}, {2.0, 2.0}})));
  std::unique_ptr<CurvePolygon> linear_cp = factory->createCurvePolygon(
      std::move(linear_shell), std::move(linear_holes));
  EXPECT_DOUBLE_EQ(linear_cp->getArea(), 91.0);
}

TEST(CurvePolygonTest, HasCurvedComponentsAllBranches) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Branch 1: curved shell -> returns true immediately.
  std::unique_ptr<CurvePolygon> curved_shell_cp =
      factory->createCurvePolygon(factory->createCircularString(MakeXySeq(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  EXPECT_TRUE(curved_shell_cp->hasCurvedComponents());

  // Branch 1b: curved CompoundCurve shell with no holes -> true.
  {
    std::vector<std::unique_ptr<SimpleCurve>> sections;
    sections.push_back(factory->createLineString(
        MakeXySeq({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}})));
    sections.push_back(factory->createCircularString(
        MakeXySeq({{2.0, 2.0}, {1.0, 3.0}, {0.0, 2.0}})));
    sections.push_back(
        factory->createLineString(MakeXySeq({{0.0, 2.0}, {0.0, 0.0}})));
    std::unique_ptr<CurvePolygon> cp = factory->createCurvePolygon(
        factory->createCompoundCurve(std::move(sections)));
    EXPECT_TRUE(cp->hasCurvedComponents());
  }

  // Branch 2: linear shell, one linear hole followed by a curved hole -> true.
  {
    std::unique_ptr<Curve> shell = factory->createLinearRing(MakeXySeq(
        {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(factory->createLinearRing(MakeXySeq(
        {{1.0, 1.0}, {2.0, 1.0}, {2.0, 2.0}, {1.0, 2.0}, {1.0, 1.0}})));
    holes.push_back(factory->createCircularString(MakeXySeq(
        {{5.0, 5.0}, {6.0, 6.0}, {7.0, 5.0}, {6.0, 4.0}, {5.0, 5.0}})));
    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(shell), std::move(holes));
    EXPECT_TRUE(cp->hasCurvedComponents());
  }

  // Branch 2b: linear shell + CompoundCurve hole containing a CircularString.
  {
    std::unique_ptr<Curve> shell = factory->createLinearRing(MakeXySeq(
        {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(MakeMixedHoleCompoundCurve(*factory));
    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(shell), std::move(holes));
    EXPECT_TRUE(cp->hasCurvedComponents());
  }

  // Branch 3: linear shell + linear hole (and linear CompoundCurve) -> false.
  {
    std::unique_ptr<Curve> shell =
        MakeLinearClosedCompoundCurve(*factory, 0.0, 0.0, 10.0, 10.0);
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(factory->createLinearRing(MakeXySeq(
        {{1.0, 1.0}, {2.0, 1.0}, {2.0, 2.0}, {1.0, 2.0}, {1.0, 1.0}})));
    holes.push_back(
        MakeLinearClosedCompoundCurve(*factory, 4.0, 4.0, 6.0, 6.0));
    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(shell), std::move(holes));
    EXPECT_FALSE(cp->hasCurvedComponents());
  }

  // Branch 3b: purely linear LinearRing shell with no holes -> false.
  {
    std::unique_ptr<Curve> shell = factory->createLinearRing(MakeXySeq(
        {{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}}));
    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(shell));
    EXPECT_FALSE(cp->hasCurvedComponents());
  }
}

TEST(CurvePolygonTest, GetBoundaryAndNormalizeThrowUnsupported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<CurvePolygon> empty_cp =
      factory->createCurvePolygon(false, false);
  EXPECT_THROW(empty_cp->getBoundary(), UnsupportedOperationException);
  EXPECT_THROW(empty_cp->normalize(), UnsupportedOperationException);

  std::unique_ptr<CurvePolygon> non_empty_cp =
      MakeMultiHoleCurvePolygon(*factory);
  EXPECT_THROW(non_empty_cp->getBoundary(), UnsupportedOperationException);
  EXPECT_THROW(non_empty_cp->normalize(), UnsupportedOperationException);
}

TEST(CurvePolygonTest, CloneDeepCopiesShellAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<CurvePolygon> orig = MakeMultiHoleCurvePolygon(*factory);
  std::unique_ptr<Geometry> cloned_geom = orig->clone();
  ASSERT_NE(cloned_geom, nullptr);
  EXPECT_NE(cloned_geom.get(), orig.get());

  const auto* cloned = dynamic_cast<const CurvePolygon*>(cloned_geom.get());
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_EQ(cloned->getNumInteriorRing(), 3);
  EXPECT_TRUE(cloned->hasCurvedComponents());
  EXPECT_TRUE(cloned->equalsExact(orig.get()));
  EXPECT_TRUE(cloned->equalsIdentical(orig.get()));
  EXPECT_NEAR(cloned->getArea(), orig->getArea(), 1e-12);

  // Mutating original coordinates and metadata does not affect the clone.
  const double expected_area = cloned->getArea();
  orig->setSRID(4326);
  ShiftXFilter shift(100.0);
  orig->apply_rw(&shift);
  orig->geometryChanged();

  EXPECT_EQ(cloned->getSRID(), 0);
  EXPECT_FALSE(cloned->equalsExact(orig.get()));
  EXPECT_NEAR(cloned->getArea(), expected_area, 1e-12);
  EXPECT_EQ(cloned->getCoordinates()->getAt<CoordinateXY>(0),
            CoordinateXY(-10.0, 0.0));
  EXPECT_EQ(orig->getCoordinates()->getAt<CoordinateXY>(0),
            CoordinateXY(90.0, 0.0));

  // Cloning an empty CurvePolygon creates a distinct empty CurvePolygon.
  std::unique_ptr<CurvePolygon> empty_cp =
      factory->createCurvePolygon(true, true);
  std::unique_ptr<Geometry> empty_clone = empty_cp->clone();
  ASSERT_NE(empty_clone, nullptr);
  EXPECT_NE(empty_clone.get(), empty_cp.get());
  EXPECT_EQ(empty_clone->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_TRUE(empty_clone->isEmpty());
  EXPECT_TRUE(empty_clone->equalsIdentical(empty_cp.get()));
}

TEST(CurvePolygonTest, ReverseReversesShellAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty CurvePolygon reverse remains empty CurvePolygon.
  std::unique_ptr<CurvePolygon> empty_cp =
      factory->createCurvePolygon(false, false);
  std::unique_ptr<Geometry> empty_rev = empty_cp->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_EQ(empty_rev->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_TRUE(empty_rev->isEmpty());

  // Multi-hole CurvePolygon reverse reverses shell and all holes.
  std::unique_ptr<CurvePolygon> orig = MakeMultiHoleCurvePolygon(*factory);
  std::unique_ptr<Geometry> rev_geom = orig->reverse();
  ASSERT_NE(rev_geom, nullptr);

  const auto* rev = dynamic_cast<const CurvePolygon*>(rev_geom.get());
  ASSERT_NE(rev, nullptr);
  EXPECT_EQ(rev->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_EQ(rev->getNumInteriorRing(), orig->getNumInteriorRing());
  EXPECT_TRUE(rev->hasCurvedComponents());
  EXPECT_NEAR(rev->getArea(), orig->getArea(), 1e-10);

  // Exterior CircularString vertices are reversed.
  std::unique_ptr<CoordinateSequence> orig_shell =
      orig->getExteriorRing()->getCoordinates();
  std::unique_ptr<CoordinateSequence> rev_shell =
      rev->getExteriorRing()->getCoordinates();
  ASSERT_EQ(rev_shell->size(), orig_shell->size());
  for (std::size_t i = 0; i < orig_shell->size(); ++i) {
    EXPECT_EQ(rev_shell->getAt<CoordinateXY>(i),
              orig_shell->getAt<CoordinateXY>(orig_shell->size() - 1 - i));
  }

  // Hole 0 (LinearRing) vertices are reversed.
  std::unique_ptr<CoordinateSequence> orig_hole0 =
      orig->getInteriorRingN(0)->getCoordinates();
  std::unique_ptr<CoordinateSequence> rev_hole0 =
      rev->getInteriorRingN(0)->getCoordinates();
  ASSERT_EQ(rev_hole0->size(), orig_hole0->size());
  for (std::size_t i = 0; i < orig_hole0->size(); ++i) {
    EXPECT_EQ(rev_hole0->getAt<CoordinateXY>(i),
              orig_hole0->getAt<CoordinateXY>(orig_hole0->size() - 1 - i));
  }

  // Hole 1 (CircularString) vertices are reversed.
  std::unique_ptr<CoordinateSequence> orig_hole1 =
      orig->getInteriorRingN(1)->getCoordinates();
  std::unique_ptr<CoordinateSequence> rev_hole1 =
      rev->getInteriorRingN(1)->getCoordinates();
  ASSERT_EQ(rev_hole1->size(), orig_hole1->size());
  for (std::size_t i = 0; i < orig_hole1->size(); ++i) {
    EXPECT_EQ(rev_hole1->getAt<CoordinateXY>(i),
              orig_hole1->getAt<CoordinateXY>(orig_hole1->size() - 1 - i));
  }

  // Hole 2 (CompoundCurve) reverses component order and vertices.
  const Curve* rev_hole2 = rev->getInteriorRingN(2);
  ASSERT_EQ(rev_hole2->getNumCurves(), 3);
  EXPECT_EQ(rev_hole2->getCurveN(0)->getGeometryType(), "LineString");
  EXPECT_EQ(rev_hole2->getCurveN(1)->getGeometryType(), "CircularString");
  EXPECT_EQ(rev_hole2->getCurveN(2)->getGeometryType(), "LineString");

  // Reversing twice restores exact original coordinates.
  std::unique_ptr<Geometry> double_rev = rev->reverse();
  EXPECT_TRUE(double_rev->equalsExact(orig.get(), 1e-12));
}

TEST(CurvePolygonTest, SortIndexAndCompareTo) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<Polygon> poly = factory->createPolygon();
  std::unique_ptr<CurvePolygon> empty_cp1 =
      factory->createCurvePolygon(false, false);
  std::unique_ptr<CurvePolygon> empty_cp2 =
      factory->createCurvePolygon(false, false);
  std::unique_ptr<MultiCurve> multi_curve = factory->createMultiCurve();

  // Polygon (5) < CurvePolygon (10) < MultiCurve (11).
  EXPECT_LT(poly->compareTo(empty_cp1.get()), 0);
  EXPECT_GT(empty_cp1->compareTo(poly.get()), 0);
  EXPECT_LT(empty_cp1->compareTo(multi_curve.get()), 0);
  EXPECT_GT(multi_curve->compareTo(empty_cp1.get()), 0);

  EXPECT_EQ(empty_cp1->compareTo(empty_cp2.get()), 0);

  std::unique_ptr<CurvePolygon> cp_unit =
      factory->createCurvePolygon(factory->createCircularString(MakeXySeq(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})));
  EXPECT_LT(empty_cp1->compareTo(cp_unit.get()), 0);
  EXPECT_GT(cp_unit->compareTo(empty_cp1.get()), 0);
  EXPECT_EQ(cp_unit->compareTo(cp_unit.get()), 0);

  // Same shell, different hole count.
  std::vector<std::unique_ptr<Curve>> one_hole;
  one_hole.push_back(factory->createLinearRing(MakeXySeq(
      {{0.5, 0.0}, {1.0, 0.5}, {1.5, 0.0}, {1.0, -0.5}, {0.5, 0.0}})));
  std::unique_ptr<CurvePolygon> cp_with_hole = factory->createCurvePolygon(
      factory->createCircularString(MakeXySeq(
          {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}})),
      std::move(one_hole));
  EXPECT_LT(cp_unit->compareTo(cp_with_hole.get()), 0);
  EXPECT_GT(cp_with_hole->compareTo(cp_unit.get()), 0);
}

TEST(CurvePolygonTest, WktRoundTripAndSurfaceProperties) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  WKTWriter writer;

  const std::string input_wkt =
      "CURVEPOLYGON (CIRCULARSTRING (-2 0, 0 2, 2 0, 0 -2, -2 0), "
      "(-1 0, 0 1, 1 0, 0 -1, -1 0))";
  std::unique_ptr<CurvePolygon> cp = reader.read<CurvePolygon>(input_wkt);
  ASSERT_NE(cp, nullptr);
  EXPECT_EQ(cp->getGeometryType(), "CurvePolygon");
  EXPECT_EQ(cp->getGeometryTypeId(), GEOS_CURVEPOLYGON);
  EXPECT_EQ(cp->getNumInteriorRing(), 1);
  EXPECT_NEAR(cp->getArea(), 4.0 * std::numbers::pi - 2.0, 1e-12);
  EXPECT_NEAR(cp->getLength(),
              4.0 * std::numbers::pi + 4.0 * std::numbers::sqrt2, 1e-12);

  const Envelope* env = cp->getEnvelopeInternal();
  ASSERT_NE(env, nullptr);
  EXPECT_DOUBLE_EQ(env->getMinX(), -2.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 2.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), -2.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 2.0);

  const std::string written_wkt = writer.write(cp.get());
  std::unique_ptr<CurvePolygon> reparsed =
      reader.read<CurvePolygon>(written_wkt);
  EXPECT_TRUE(reparsed->equalsExact(cp.get(), 1e-12));
}

}  // namespace
}  // namespace geos_google
