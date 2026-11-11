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

// Unit tests for third_party/geos/src/operation/valid/RepeatedPointTester.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/RepeatedPointTester.cpp

#include "third_party/geos/include/geos/operation/valid/RepeatedPointTester.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/MultiSurface.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::MultiSurface;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::operation::valid::RepeatedPointTester;
using geos::util::UnsupportedOperationException;

class RepeatedPointTesterTest : public ::testing::Test {
 protected:
  RepeatedPointTesterTest() : factory_(GeometryFactory::create()) {}

  // RepeatedPointTester declares private overloads for Polygon, MultiPolygon,
  // MultiLineString, and GeometryCollection (including MultiPoint), so callers
  // must dispatch through the public Geometry* overload.
  static bool HasRepeatedPoint(RepeatedPointTester& tester,
                               const Geometry* geom) {
    return tester.hasRepeatedPoint(geom);
  }

  std::unique_ptr<LineString> MakeLine(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLineString(CoordinateSequence(coords));
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLinearRing(CoordinateSequence(coords));
  }

  std::unique_ptr<Polygon> MakePolygon(
      std::initializer_list<CoordinateXY> shell,
      std::initializer_list<std::initializer_list<CoordinateXY>> holes = {})
      const {
    std::vector<std::unique_ptr<LinearRing>> hole_rings;
    hole_rings.reserve(holes.size());
    for (const auto& hole : holes) {
      hole_rings.push_back(MakeRing(hole));
    }
    return factory_->createPolygon(MakeRing(shell), std::move(hole_rings));
  }

  GeometryFactory::Ptr factory_;
};

TEST_F(RepeatedPointTesterTest, DefaultCoordinateIsOriginAndRetainsLastHit) {
  RepeatedPointTester tester;
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 0.0);

  const auto dirty = MakeLine({{0.0, 0.0}, {2.5, -3.5}, {2.5, -3.5}});
  EXPECT_TRUE(HasRepeatedPoint(tester, dirty.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 2.5);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, -3.5);

  const auto clean = MakeLine({{0.0, 0.0}, {1.0, 1.0}});
  EXPECT_FALSE(HasRepeatedPoint(tester, clean.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 2.5);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, -3.5);

  const auto second_dirty = MakeLine({{4.0, 5.0}, {4.0, 5.0}});
  EXPECT_TRUE(HasRepeatedPoint(tester, second_dirty.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 5.0);
}

TEST_F(RepeatedPointTesterTest, CoordinateSequenceEdgeCases) {
  RepeatedPointTester tester;

  const CoordinateSequence empty_seq = CoordinateSequence::XY(0);
  EXPECT_FALSE(tester.hasRepeatedPoint(&empty_seq));

  const CoordinateSequence single_seq{CoordinateXY{3.0, 4.0}};
  EXPECT_FALSE(tester.hasRepeatedPoint(&single_seq));

  // Non-consecutive duplicates are not consecutive repeated points.
  const CoordinateSequence closed_seq{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{1.0, 0.0},
      CoordinateXY{1.0, 1.0},
      CoordinateXY{0.0, 0.0},
  };
  EXPECT_FALSE(tester.hasRepeatedPoint(&closed_seq));

  const CoordinateSequence repeat_at_start{
      CoordinateXY{1.5, 2.5},
      CoordinateXY{1.5, 2.5},
      CoordinateXY{3.0, 4.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&repeat_at_start));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 1.5);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 2.5);

  const CoordinateSequence repeat_in_middle{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{5.0, -6.0},
      CoordinateXY{5.0, -6.0},
      CoordinateXY{8.0, 9.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&repeat_in_middle));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, -6.0);

  const CoordinateSequence repeat_at_end{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{1.0, 1.0},
      CoordinateXY{9.0, 10.0},
      CoordinateXY{9.0, 10.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&repeat_at_end));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 9.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 10.0);

  const CoordinateSequence multiple_repeats{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 2.0}, CoordinateXY{1.0, 2.0},
      CoordinateXY{3.0, 4.0}, CoordinateXY{3.0, 4.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&multiple_repeats));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 1.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 2.0);
}

TEST_F(RepeatedPointTesterTest, CoordinateSequenceEqualityIgnoresZAndMAndNaN) {
  RepeatedPointTester tester;

  const CoordinateSequence xyz_same_xy{
      Coordinate{2.0, 3.0, 10.0},
      Coordinate{2.0, 3.0, 25.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&xyz_same_xy));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 3.0);

  const CoordinateSequence xym_same_xy{
      CoordinateXYM{6.0, -7.0, 1.0},
      CoordinateXYM{6.0, -7.0, 42.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&xym_same_xy));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 6.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, -7.0);

  const CoordinateSequence xyzm_same_xy{
      CoordinateXYZM{7.0, 8.0, 1.0, 2.0},
      CoordinateXYZM{7.0, 8.0, 9.0, 10.0},
  };
  EXPECT_TRUE(tester.hasRepeatedPoint(&xyzm_same_xy));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 7.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 8.0);

  // NaN never compares equal to NaN under 2D equality.
  const CoordinateSequence nan_seq{
      CoordinateXY{DoubleNotANumber, DoubleNotANumber},
      CoordinateXY{DoubleNotANumber, DoubleNotANumber},
  };
  EXPECT_FALSE(tester.hasRepeatedPoint(&nan_seq));
}

TEST_F(RepeatedPointTesterTest, EmptyGeometriesReturnFalse) {
  RepeatedPointTester tester;

  const auto empty_point = factory_->createPoint();
  const auto empty_line = factory_->createLineString();
  const auto empty_ring = factory_->createLinearRing();
  const auto empty_polygon = factory_->createPolygon();
  const auto empty_multipoint = factory_->createMultiPoint();
  const auto empty_multiline = factory_->createMultiLineString();
  const auto empty_multipolygon = factory_->createMultiPolygon();
  const auto empty_collection = factory_->createGeometryCollection();
  const auto empty_circular = factory_->createCircularString(false, false);
  const auto empty_compound = factory_->createCompoundCurve();
  const auto empty_curve_poly = factory_->createCurvePolygon(false, false);
  const auto empty_multicurve = factory_->createMultiCurve();
  const auto empty_multisurface = factory_->createMultiSurface();

  EXPECT_FALSE(HasRepeatedPoint(tester, empty_point.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_line.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_ring.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_polygon.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_multipoint.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_multiline.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_multipolygon.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_collection.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_circular.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_compound.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_curve_poly.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_multicurve.get()));
  EXPECT_FALSE(HasRepeatedPoint(tester, empty_multisurface.get()));
}

TEST_F(RepeatedPointTesterTest, PointAndMultiPointAlwaysReturnFalse) {
  RepeatedPointTester tester;

  const auto point = factory_->createPoint(CoordinateXY(1.0, 2.0));
  EXPECT_FALSE(HasRepeatedPoint(tester, point.get()));

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(3.0, 4.0)));
  const auto multipoint = factory_->createMultiPoint(std::move(pts));
  EXPECT_FALSE(HasRepeatedPoint(tester, multipoint.get()));
}

TEST_F(RepeatedPointTesterTest, LineStringAndLinearRingDetection) {
  RepeatedPointTester tester;

  const auto clean_line =
      MakeLine({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_line.get()));

  const auto repeated_line =
      MakeLine({{0.0, 0.0}, {2.0, 3.0}, {2.0, 3.0}, {4.0, 5.0}});
  EXPECT_TRUE(HasRepeatedPoint(tester, repeated_line.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 3.0);

  const auto clean_ring =
      MakeRing({{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}});
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_ring.get()));

  const auto repeated_ring =
      MakeRing({{0.0, 0.0}, {4.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 0.0}});
  EXPECT_TRUE(HasRepeatedPoint(tester, repeated_ring.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 0.0);

  const auto repeated_closure_ring =
      MakeRing({{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 0.0}, {0.0, 0.0}});
  EXPECT_TRUE(HasRepeatedPoint(tester, repeated_closure_ring.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 0.0);
}

TEST_F(RepeatedPointTesterTest, PolygonChecksExteriorAndInteriorRings) {
  RepeatedPointTester tester;

  const auto clean_no_holes =
      MakePolygon({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 5.0}, {0.0, 0.0}});
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_no_holes.get()));

  const auto clean_poly = MakePolygon(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
      {
          {{1.0, 1.0}, {3.0, 1.0}, {3.0, 3.0}, {1.0, 3.0}, {1.0, 1.0}},
          {{5.0, 5.0}, {8.0, 5.0}, {8.0, 8.0}, {5.0, 8.0}, {5.0, 5.0}},
      });
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_poly.get()));

  const auto bad_shell_poly = MakePolygon(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 0.0}},
      {{{1.0, 1.0}, {2.0, 1.0}, {2.0, 2.0}, {1.0, 1.0}}});
  EXPECT_TRUE(HasRepeatedPoint(tester, bad_shell_poly.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 0.0);

  const auto bad_hole_poly = MakePolygon(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
      {
          {{1.0, 1.0}, {3.0, 1.0}, {3.0, 3.0}, {1.0, 3.0}, {1.0, 1.0}},
          {{5.0, 5.0}, {7.0, 5.0}, {7.0, 7.0}, {7.0, 7.0}, {5.0, 5.0}},
      });
  EXPECT_TRUE(HasRepeatedPoint(tester, bad_hole_poly.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 7.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 7.0);
}

TEST_F(RepeatedPointTesterTest, MultiLineStringChecksEachComponent) {
  RepeatedPointTester tester;

  std::vector<std::unique_ptr<LineString>> clean_lines;
  clean_lines.push_back(factory_->createLineString());
  clean_lines.push_back(MakeLine({{0.0, 0.0}, {1.0, 1.0}}));
  clean_lines.push_back(MakeLine({{2.0, 2.0}, {3.0, 3.0}, {4.0, 2.0}}));
  const auto clean_mls =
      factory_->createMultiLineString(std::move(clean_lines));
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_mls.get()));

  std::vector<std::unique_ptr<LineString>> dirty_lines;
  dirty_lines.push_back(factory_->createLineString());
  dirty_lines.push_back(MakeLine({{0.0, 0.0}, {1.0, 1.0}}));
  dirty_lines.push_back(
      MakeLine({{2.0, 2.0}, {3.5, 4.5}, {3.5, 4.5}, {5.0, 5.0}}));
  const auto dirty_mls =
      factory_->createMultiLineString(std::move(dirty_lines));
  EXPECT_TRUE(HasRepeatedPoint(tester, dirty_mls.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 3.5);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 4.5);
}

TEST_F(RepeatedPointTesterTest, MultiPolygonChecksEachComponent) {
  RepeatedPointTester tester;

  std::vector<std::unique_ptr<Polygon>> clean_polys;
  clean_polys.push_back(factory_->createPolygon());
  clean_polys.push_back(
      MakePolygon({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}}));
  clean_polys.push_back(
      MakePolygon({{10.0, 0.0}, {12.0, 0.0}, {12.0, 2.0}, {10.0, 0.0}}));
  const auto clean_mp = factory_->createMultiPolygon(std::move(clean_polys));
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_mp.get()));

  std::vector<std::unique_ptr<Polygon>> dirty_polys;
  dirty_polys.push_back(
      MakePolygon({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}}));
  dirty_polys.push_back(MakePolygon(
      {{10.0, 0.0}, {20.0, 0.0}, {20.0, 20.0}, {10.0, 20.0}, {10.0, 0.0}},
      {{{12.0, 2.0}, {14.0, 2.0}, {14.0, 2.0}, {14.0, 4.0}, {12.0, 2.0}}}));
  const auto dirty_mp = factory_->createMultiPolygon(std::move(dirty_polys));
  EXPECT_TRUE(HasRepeatedPoint(tester, dirty_mp.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 14.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 2.0);
}

TEST_F(RepeatedPointTesterTest, GeometryCollectionChecksNestedGeometries) {
  RepeatedPointTester tester;

  std::vector<std::unique_ptr<Geometry>> clean_items;
  clean_items.push_back(factory_->createPoint());
  clean_items.push_back(factory_->createPoint(CoordinateXY(0.0, 0.0)));
  clean_items.push_back(MakeLine({{1.0, 1.0}, {2.0, 2.0}}));
  clean_items.push_back(
      MakePolygon({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 0.0}}));
  const auto clean_gc =
      factory_->createGeometryCollection(std::move(clean_items));
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_gc.get()));

  std::vector<std::unique_ptr<Geometry>> inner_dirty_items;
  inner_dirty_items.push_back(factory_->createPoint(CoordinateXY(9.0, 9.0)));
  inner_dirty_items.push_back(
      MakeLine({{1.0, 2.0}, {6.0, 7.0}, {6.0, 7.0}, {8.0, 9.0}}));

  std::vector<std::unique_ptr<Geometry>> outer_items;
  outer_items.push_back(
      MakePolygon({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 0.0}}));
  outer_items.push_back(
      factory_->createGeometryCollection(std::move(inner_dirty_items)));
  const auto dirty_gc =
      factory_->createGeometryCollection(std::move(outer_items));

  EXPECT_TRUE(HasRepeatedPoint(tester, dirty_gc.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 6.0);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 7.0);

  // MultiCurve and MultiSurface inherit from GeometryCollection, so linear and
  // polygonal components dispatch recursively through GeometryCollection.
  std::vector<std::unique_ptr<Curve>> clean_curves;
  clean_curves.push_back(MakeLine({{0.0, 0.0}, {1.0, 1.0}}));
  const auto clean_mc = factory_->createMultiCurve(std::move(clean_curves));
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_mc.get()));

  std::vector<std::unique_ptr<Curve>> dirty_curves;
  dirty_curves.push_back(MakeLine({{0.0, 0.0}, {2.2, 3.3}, {2.2, 3.3}}));
  const auto dirty_mc = factory_->createMultiCurve(std::move(dirty_curves));
  EXPECT_TRUE(HasRepeatedPoint(tester, dirty_mc.get()));
  EXPECT_DOUBLE_EQ(tester.getCoordinate().x, 2.2);
  EXPECT_DOUBLE_EQ(tester.getCoordinate().y, 3.3);

  std::vector<std::unique_ptr<Surface>> clean_surfaces;
  clean_surfaces.push_back(
      MakePolygon({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}}));
  const auto clean_ms = factory_->createMultiSurface(std::move(clean_surfaces));
  EXPECT_FALSE(HasRepeatedPoint(tester, clean_ms.get()));
}

TEST_F(RepeatedPointTesterTest, UnsupportedCurvedGeometriesThrow) {
  RepeatedPointTester tester;

  const auto circular = factory_->createCircularString(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 0.0}});
  EXPECT_THROW(HasRepeatedPoint(tester, circular.get()),
               UnsupportedOperationException);

  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(MakeLine({{0.0, 0.0}, {1.0, 0.0}}));
  const auto compound = factory_->createCompoundCurve(std::move(sections));
  EXPECT_THROW(HasRepeatedPoint(tester, compound.get()),
               UnsupportedOperationException);

  const auto curve_poly =
      factory_->createCurvePolygon(factory_->createCircularString(
          CoordinateSequence{CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0},
                             CoordinateXY{2.0, 0.0}, CoordinateXY{1.0, -1.0},
                             CoordinateXY{0.0, 0.0}}));
  EXPECT_THROW(HasRepeatedPoint(tester, curve_poly.get()),
               UnsupportedOperationException);

  std::vector<std::unique_ptr<Curve>> curved_items;
  curved_items.push_back(factory_->createCircularString(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 0.0}}));
  const auto multicurve = factory_->createMultiCurve(std::move(curved_items));
  EXPECT_THROW(HasRepeatedPoint(tester, multicurve.get()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
