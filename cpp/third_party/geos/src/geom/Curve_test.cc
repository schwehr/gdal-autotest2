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

// Unit tests for third_party/geos/src/geom/Curve.cpp and
// third_party/geos/include/geos/geom/Curve.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/Curve.cpp

#include "third_party/geos/include/geos/geom/Curve.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Curve;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryComponentFilter;
using geos::geom::GeometryFactory;
using geos::geom::GeometryFilter;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::SimpleCurve;
using geos::util::UnsupportedOperationException;
using ::testing::ElementsAre;

std::unique_ptr<CoordinateSequence> MakeXySeq(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CompoundCurve> MakeOpenCompoundCurve(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(
      factory.createLineString(MakeXySeq({{0.0, 0.0}, {2.0, 0.0}})));
  sections.push_back(factory.createCircularString(
      MakeXySeq({{2.0, 0.0}, {3.0, 1.0}, {2.0, 2.0}})));
  return factory.createCompoundCurve(std::move(sections));
}

std::unique_ptr<CompoundCurve> MakeClosedCompoundCurve(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(
      factory.createLineString(MakeXySeq({{0.0, 0.0}, {2.0, 0.0}})));
  sections.push_back(factory.createCircularString(
      MakeXySeq({{2.0, 0.0}, {3.0, 1.0}, {2.0, 2.0}})));
  sections.push_back(factory.createLineString(
      MakeXySeq({{2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}})));
  return factory.createCompoundCurve(std::move(sections));
}

class RecordingGeometryFilterRO final : public GeometryFilter {
 public:
  void filter_ro(const Geometry* geom) override { visited.push_back(geom); }
  std::vector<const Geometry*> visited;
};

class RecordingGeometryFilterRW final : public GeometryFilter {
 public:
  explicit RecordingGeometryFilterRW(int new_srid) : srid(new_srid) {}

  void filter_rw(Geometry* geom) override {
    visited.push_back(geom);
    geom->setSRID(srid);
  }

  int srid;
  std::vector<Geometry*> visited;
};

class RecordingComponentFilterRO final : public GeometryComponentFilter {
 public:
  void filter_ro(const Geometry* geom) override { visited.push_back(geom); }
  std::vector<const Geometry*> visited;
};

class RecordingComponentFilterRW final : public GeometryComponentFilter {
 public:
  explicit RecordingComponentFilterRW(int new_srid) : srid(new_srid) {}

  void filter_rw(Geometry* geom) override {
    visited.push_back(geom);
    geom->setSRID(srid);
  }

  int srid;
  std::vector<Geometry*> visited;
};

TEST(CurveTest, DimensionAndBoundaryDimension) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> open_line =
      factory->createLineString(MakeXySeq({{0.0, 0.0}, {3.0, 4.0}}));
  const Curve& open_line_curve = *open_line;
  EXPECT_EQ(open_line_curve.getDimension(), Dimension::L);
  EXPECT_FALSE(open_line_curve.isClosed());
  EXPECT_EQ(open_line_curve.getBoundaryDimension(), 0);

  std::unique_ptr<LineString> closed_line = factory->createLineString(
      MakeXySeq({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}}));
  const Curve& closed_line_curve = *closed_line;
  EXPECT_EQ(closed_line_curve.getDimension(), Dimension::L);
  EXPECT_TRUE(closed_line_curve.isClosed());
  EXPECT_EQ(closed_line_curve.getBoundaryDimension(), Dimension::False);

  std::unique_ptr<LineString> empty_line = factory->createLineString();
  const Curve& empty_line_curve = *empty_line;
  EXPECT_EQ(empty_line_curve.getDimension(), Dimension::L);
  EXPECT_FALSE(empty_line_curve.isClosed());
  EXPECT_EQ(empty_line_curve.getBoundaryDimension(), 0);

  std::unique_ptr<CircularString> open_arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  const Curve& open_arc_curve = *open_arc;
  EXPECT_EQ(open_arc_curve.getDimension(), Dimension::L);
  EXPECT_FALSE(open_arc_curve.isClosed());
  EXPECT_EQ(open_arc_curve.getBoundaryDimension(), 0);

  std::unique_ptr<CircularString> closed_arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}));
  const Curve& closed_arc_curve = *closed_arc;
  EXPECT_EQ(closed_arc_curve.getDimension(), Dimension::L);
  EXPECT_TRUE(closed_arc_curve.isClosed());
  EXPECT_EQ(closed_arc_curve.getBoundaryDimension(), Dimension::False);

  std::unique_ptr<CompoundCurve> open_compound =
      MakeOpenCompoundCurve(*factory);
  const Curve& open_compound_curve = *open_compound;
  EXPECT_EQ(open_compound_curve.getDimension(), Dimension::L);
  EXPECT_FALSE(open_compound_curve.isClosed());
  EXPECT_EQ(open_compound_curve.getBoundaryDimension(), 0);

  std::unique_ptr<CompoundCurve> closed_compound =
      MakeClosedCompoundCurve(*factory);
  const Curve& closed_compound_curve = *closed_compound;
  EXPECT_EQ(closed_compound_curve.getDimension(), Dimension::L);
  EXPECT_TRUE(closed_compound_curve.isClosed());
  EXPECT_EQ(closed_compound_curve.getBoundaryDimension(), Dimension::False);

  std::unique_ptr<CompoundCurve> empty_compound =
      factory->createCompoundCurve();
  const Curve& empty_compound_curve = *empty_compound;
  EXPECT_EQ(empty_compound_curve.getDimension(), Dimension::L);
  EXPECT_FALSE(empty_compound_curve.isClosed());
  EXPECT_EQ(empty_compound_curve.getBoundaryDimension(), 0);
}

TEST(CurveTest, IsRingForLinearCurves) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Open simple LineString: not closed -> not a ring.
  std::unique_ptr<LineString> open_simple = factory->createLineString(
      MakeXySeq({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}}));
  EXPECT_FALSE(open_simple->isClosed());
  EXPECT_TRUE(open_simple->isSimple());
  EXPECT_FALSE(open_simple->isRing());

  // Closed simple LineString: closed and simple -> ring.
  std::unique_ptr<LineString> closed_simple = factory->createLineString(
      MakeXySeq({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}}));
  EXPECT_TRUE(closed_simple->isClosed());
  EXPECT_TRUE(closed_simple->isSimple());
  EXPECT_TRUE(closed_simple->isRing());

  // Closed self-intersecting LineString (bowtie): closed but not simple.
  std::unique_ptr<LineString> closed_bowtie = factory->createLineString(
      MakeXySeq({{0.0, 0.0}, {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0}, {0.0, 0.0}}));
  EXPECT_TRUE(closed_bowtie->isClosed());
  EXPECT_FALSE(closed_bowtie->isSimple());
  EXPECT_FALSE(closed_bowtie->isRing());

  // Open self-intersecting LineString: neither closed nor simple.
  std::unique_ptr<LineString> open_crossing = factory->createLineString(
      MakeXySeq({{0.0, 0.0}, {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0}}));
  EXPECT_FALSE(open_crossing->isClosed());
  EXPECT_FALSE(open_crossing->isRing());

  // Simple and self-intersecting LinearRings.
  std::unique_ptr<LinearRing> simple_ring = factory->createLinearRing(
      MakeXySeq({{0.0, 0.0}, {3.0, 0.0}, {3.0, 3.0}, {0.0, 3.0}, {0.0, 0.0}}));
  EXPECT_TRUE(simple_ring->isRing());

  std::unique_ptr<LinearRing> bowtie_ring = factory->createLinearRing(
      MakeXySeq({{0.0, 0.0}, {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0}, {0.0, 0.0}}));
  EXPECT_TRUE(bowtie_ring->isClosed());
  EXPECT_FALSE(bowtie_ring->isSimple());
  EXPECT_FALSE(bowtie_ring->isRing());

  // Empty LineString is not closed, while empty LinearRing is closed & simple.
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  EXPECT_FALSE(empty_line->isRing());

  std::unique_ptr<LinearRing> empty_ring = factory->createLinearRing();
  EXPECT_TRUE(empty_ring->isClosed());
  EXPECT_TRUE(empty_ring->isRing());
}

TEST(CurveTest, IsRingForCurvedSubclasses) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Open curved types short-circuit isRing() to false via !isClosed().
  std::unique_ptr<CircularString> open_arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  EXPECT_FALSE(open_arc->isRing());

  std::unique_ptr<CompoundCurve> open_compound =
      MakeOpenCompoundCurve(*factory);
  EXPECT_FALSE(open_compound->isRing());

  std::unique_ptr<CircularString> empty_arc =
      factory->createCircularString(false, false);
  EXPECT_FALSE(empty_arc->isRing());

  std::unique_ptr<CompoundCurve> empty_compound =
      factory->createCompoundCurve();
  EXPECT_FALSE(empty_compound->isRing());

  // Closed non-empty curved types reach isSimple(), which throws.
  std::unique_ptr<CircularString> closed_arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}));
  EXPECT_THROW(closed_arc->isRing(), UnsupportedOperationException);

  std::unique_ptr<CompoundCurve> closed_compound =
      MakeClosedCompoundCurve(*factory);
  EXPECT_THROW(closed_compound->isRing(), UnsupportedOperationException);
}

TEST(CurveTest, ApplyReadOnlyFilters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> line =
      factory->createLineString(MakeXySeq({{0.0, 0.0}, {1.0, 1.0}}));
  std::unique_ptr<CircularString> arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<CompoundCurve> compound = MakeOpenCompoundCurve(*factory);

  const std::vector<const Curve*> curves = {line.get(), arc.get(),
                                            compound.get()};
  for (const Curve* curve : curves) {
    const Geometry* expected_geom = curve;

    RecordingGeometryFilterRO geom_filter;
    curve->apply_ro(&geom_filter);
    EXPECT_THAT(geom_filter.visited, ElementsAre(expected_geom));

    RecordingComponentFilterRO comp_filter;
    curve->apply_ro(&comp_filter);
    EXPECT_THAT(comp_filter.visited, ElementsAre(expected_geom));
  }
}

TEST(CurveTest, ApplyReadWriteFilters) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> line =
      factory->createLineString(MakeXySeq({{0.0, 0.0}, {1.0, 1.0}}));
  std::unique_ptr<CircularString> arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<CompoundCurve> compound = MakeOpenCompoundCurve(*factory);

  const std::vector<Curve*> curves = {line.get(), arc.get(), compound.get()};
  for (Curve* curve : curves) {
    Geometry* expected_geom = curve;

    RecordingGeometryFilterRW geom_filter(4326);
    curve->apply_rw(&geom_filter);
    EXPECT_THAT(geom_filter.visited, ElementsAre(expected_geom));
    EXPECT_EQ(curve->getSRID(), 4326);

    RecordingComponentFilterRW comp_filter(3857);
    curve->apply_rw(&comp_filter);
    EXPECT_THAT(comp_filter.visited, ElementsAre(expected_geom));
    EXPECT_EQ(curve->getSRID(), 3857);
  }
}

TEST(CurveTest, GetNumCurvesAndGetCurveN) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<LineString> line =
      factory->createLineString(MakeXySeq({{0.0, 0.0}, {1.0, 0.0}}));
  const Curve& line_curve = *line;
  EXPECT_EQ(line_curve.getNumCurves(), 1);
  EXPECT_EQ(line_curve.getCurveN(0), line.get());

  std::unique_ptr<CircularString> arc = factory->createCircularString(
      MakeXySeq({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  const Curve& arc_curve = *arc;
  EXPECT_EQ(arc_curve.getNumCurves(), 1);
  EXPECT_EQ(arc_curve.getCurveN(0), arc.get());

  std::unique_ptr<CompoundCurve> compound = MakeOpenCompoundCurve(*factory);
  const Curve& compound_curve = *compound;
  ASSERT_EQ(compound_curve.getNumCurves(), 2);
  EXPECT_EQ(compound_curve.getCurveN(0)->getGeometryType(), "LineString");
  EXPECT_EQ(compound_curve.getCurveN(1)->getGeometryType(), "CircularString");
}

}  // namespace
}  // namespace geos_google
