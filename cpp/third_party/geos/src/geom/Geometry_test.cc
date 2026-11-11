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

#include "third_party/geos/include/geos/geom/Geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
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
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/GeometryFilter.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiSurface.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKBReader.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/buffer/BufferOp.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"
#include "third_party/geos/include/geos/version.h"

namespace geos_google {
namespace {

// Minimal Geometry subclass used to exercise base-class virtual defaults,
// nullptr factory fallback, and protected static/member helpers in Geometry.cpp
// that concrete GEOS subclasses override or do not expose publicly.
class TestGeometry final : public geos::geom::Geometry {
 public:
  using Geometry::apply_ro;
  using Geometry::apply_rw;

  explicit TestGeometry(
      const geos::geom::GeometryFactory* factory, bool empty = false,
      geos::geom::Dimension::DimensionType dim = geos::geom::Dimension::P,
      int sort_index = geos::geom::SORTINDEX_POINT)
      : Geometry(factory),
        empty_(empty),
        dim_(dim),
        sort_index_(sort_index),
        coord_(0.0, 0.0),
        env_(empty ? geos::geom::Envelope() : geos::geom::Envelope(coord_)) {}

  TestGeometry(const TestGeometry& other)
      : Geometry(other),
        empty_(other.empty_),
        dim_(other.dim_),
        sort_index_(other.sort_index_),
        coord_(other.coord_),
        env_(other.env_),
        changed_count_(other.changed_count_) {}

  static bool CallHasNullElements(const geos::geom::CoordinateSequence* seq) {
    return hasNullElements(seq);
  }

  static bool CallHasNonEmptyElements(
      const std::vector<const Geometry*>* geoms) {
    return hasNonEmptyElements(geoms);
  }

  static bool CallHasNullElementsVec(
      const std::vector<const Geometry*>* geoms) {
    return hasNullElements(geoms);
  }

  static int CallCompare(const std::vector<const Geometry*>& a,
                         const std::vector<const Geometry*>& b) {
    return compare(a, b);
  }

  static std::vector<std::unique_ptr<Geometry>> CallToGeometryArrayFromPoints(
      std::vector<std::unique_ptr<geos::geom::Point>>&& pts) {
    return toGeometryArray(std::move(pts));
  }

  static std::vector<std::unique_ptr<Geometry>> CallToGeometryArray(
      std::vector<std::unique_ptr<Geometry>>&& geoms) {
    return toGeometryArray(std::move(geoms));
  }

  static void CallCheckNotGeometryCollection(const Geometry* g) {
    checkNotGeometryCollection(g);
  }

  bool CallIsEquivalentClass(const Geometry* other) const {
    return isEquivalentClass(other);
  }

  bool CallEqual(const geos::geom::CoordinateXY& a,
                 const geos::geom::CoordinateXY& b, double tolerance) const {
    return equal(a, b, tolerance);
  }

  int changed_count() const { return changed_count_; }

  const geos::geom::CoordinateXY* getCoordinate() const override {
    return empty_ ? nullptr : &coord_;
  }

  std::unique_ptr<geos::geom::CoordinateSequence> getCoordinates()
      const override {
    auto seq = std::make_unique<geos::geom::CoordinateSequence>(std::size_t{0},
                                                                false, false);
    if (!empty_) {
      seq->add(coord_);
    }
    return seq;
  }

  std::size_t getNumPoints() const override { return empty_ ? 0 : 1; }
  std::string getGeometryType() const override { return "TestGeometry"; }
  geos::geom::GeometryTypeId getGeometryTypeId() const override {
    return geos::geom::GEOS_POINT;
  }
  bool isEmpty() const override { return empty_; }
  geos::geom::Dimension::DimensionType getDimension() const override {
    return dim_;
  }
  std::uint8_t getCoordinateDimension() const override { return 2; }
  bool hasZ() const override { return false; }
  bool hasM() const override { return false; }
  std::unique_ptr<Geometry> getBoundary() const override {
    return getFactory()->createGeometryCollection();
  }
  int getBoundaryDimension() const override {
    return geos::geom::Dimension::False;
  }
  const geos::geom::Envelope* getEnvelopeInternal() const override {
    return &env_;
  }
  bool equalsExact(const Geometry* other, double /*tolerance*/) const override {
    return isEquivalentClass(other);
  }
  bool equalsIdentical(const Geometry* other) const override {
    return isEquivalentClass(other);
  }
  void apply_rw(const geos::geom::CoordinateFilter* /*filter*/) override {}
  void apply_ro(geos::geom::CoordinateFilter* /*filter*/) const override {}
  void apply_rw(geos::geom::CoordinateSequenceFilter& /*filter*/) override {}
  void apply_ro(
      geos::geom::CoordinateSequenceFilter& /*filter*/) const override {}
  void normalize() override {}

 protected:
  void geometryChangedAction() override { ++changed_count_; }
  Geometry* cloneImpl() const override { return new TestGeometry(*this); }
  Geometry* reverseImpl() const override { return new TestGeometry(*this); }
  int compareToSameClass(const Geometry* /*geom*/) const override { return 0; }
  int getSortIndex() const override { return sort_index_; }

 private:
  bool empty_;
  geos::geom::Dimension::DimensionType dim_;
  int sort_index_;
  geos::geom::CoordinateXY coord_;
  geos::geom::Envelope env_;
  int changed_count_ = 0;
};

class RecordingGeometryFilter final : public geos::geom::GeometryFilter {
 public:
  void filter_ro(const geos::geom::Geometry* geom) override {
    ro_geoms_.push_back(geom);
  }
  void filter_rw(geos::geom::Geometry* geom) override {
    rw_geoms_.push_back(geom);
  }

  std::vector<const geos::geom::Geometry*> ro_geoms_;
  std::vector<geos::geom::Geometry*> rw_geoms_;
};

class RecordingComponentFilter final
    : public geos::geom::GeometryComponentFilter {
 public:
  void filter_ro(const geos::geom::Geometry* geom) override {
    ro_geoms_.push_back(geom);
  }
  void filter_rw(geos::geom::Geometry* geom) override {
    rw_geoms_.push_back(geom);
  }

  std::vector<const geos::geom::Geometry*> ro_geoms_;
  std::vector<geos::geom::Geometry*> rw_geoms_;
};

class ShiftXCoordinateFilter final : public geos::geom::CoordinateFilter {
 public:
  explicit ShiftXCoordinateFilter(double dx) : dx_(dx) {}

  void filter_rw(geos::geom::CoordinateXY* coord) const override {
    coord->x += dx_;
  }

 private:
  double dx_;
};

struct ComponentCounter {
  std::size_t count = 0;
  void filter(const geos::geom::Geometry* /*geom*/) { ++count; }
};

class GeometryTest : public ::testing::Test {
 protected:
  GeometryTest()
      : factory_(geos::geom::GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<geos::geom::Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  geos::geom::GeometryFactory::Ptr factory_;
  geos::io::WKTReader reader_;
};

TEST_F(GeometryTest, VersionAndJtsPort) {
  EXPECT_EQ(geos::geom::geosversion(), GEOS_VERSION);
  EXPECT_FALSE(geos::geom::geosversion().empty());
  EXPECT_EQ(geos::geom::jtsport(), GEOS_JTS_PORT);
  EXPECT_FALSE(geos::geom::jtsport().empty());
}

TEST_F(GeometryTest, ConstructionDefaultFactorySridUserDataAndClone) {
  TestGeometry default_geom(nullptr);
  EXPECT_EQ(default_geom.getFactory(),
            geos::geom::GeometryFactory::getDefaultInstance());
  EXPECT_EQ(default_geom.getSRID(), 0);
  EXPECT_EQ(
      default_geom.getPrecisionModel(),
      geos::geom::GeometryFactory::getDefaultInstance()->getPrecisionModel());

  geos::geom::PrecisionModel pm(100.0);
  auto custom_factory = geos::geom::GeometryFactory::create(&pm, 4326);
  auto point = custom_factory->createPoint(geos::geom::CoordinateXY(1.5, 2.5));
  int user_payload = 42;
  point->setUserData(&user_payload);

  EXPECT_EQ(point->getFactory(), custom_factory.get());
  EXPECT_EQ(point->getSRID(), 4326);
  EXPECT_EQ(point->getUserData(), &user_payload);
  EXPECT_FALSE(point->getPrecisionModel()->isFloating());
  EXPECT_DOUBLE_EQ(point->getPrecisionModel()->getScale(), 100.0);

  auto cloned = point->clone();
  EXPECT_EQ(cloned->getFactory(), custom_factory.get());
  EXPECT_EQ(cloned->getSRID(), 4326);
  EXPECT_EQ(cloned->getUserData(), nullptr);
  EXPECT_TRUE(cloned->equalsExact(point.get()));

  cloned->setSRID(3857);
  EXPECT_EQ(cloned->getSRID(), 3857);
  EXPECT_EQ(point->getSRID(), 4326);

  default_geom.setUserData(&user_payload);
  auto tg_cloned = default_geom.clone();
  EXPECT_EQ(tg_cloned->getFactory(),
            geos::geom::GeometryFactory::getDefaultInstance());
  EXPECT_EQ(tg_cloned->getUserData(), nullptr);
  EXPECT_TRUE(tg_cloned->equalsExact(&default_geom));

  // Verify Geometry keeps its GeometryFactory alive via reference counting.
  std::unique_ptr<geos::geom::Point> survivor;
  {
    auto transient_factory = geos::geom::GeometryFactory::create(&pm, 32632);
    survivor =
        transient_factory->createPoint(geos::geom::CoordinateXY(3.0, 4.0));
  }
  EXPECT_EQ(survivor->getSRID(), 32632);
  EXPECT_DOUBLE_EQ(survivor->getPrecisionModel()->getScale(), 100.0);
}

TEST_F(GeometryTest, ProtectedHelpersNullElementsEquivalentClassEqualAndCheck) {
  geos::geom::CoordinateSequence empty_seq(std::size_t{0}, std::size_t{3});
  EXPECT_FALSE(TestGeometry::CallHasNullElements(&empty_seq));

  geos::geom::CoordinateSequence valid_seq{
      geos::geom::Coordinate(1.0, 2.0, 3.0),
      geos::geom::Coordinate(4.0, 5.0, 6.0)};
  EXPECT_FALSE(TestGeometry::CallHasNullElements(&valid_seq));

  geos::geom::CoordinateSequence null_seq{geos::geom::Coordinate(1.0, 2.0, 3.0),
                                          geos::geom::Coordinate::getNull()};
  EXPECT_TRUE(TestGeometry::CallHasNullElements(&null_seq));

  TestGeometry test_a(factory_.get());
  TestGeometry test_b(factory_.get());
  auto pt = factory_->createPoint(geos::geom::CoordinateXY(0.0, 0.0));
  auto line = ReadWkt("LINESTRING (0 0, 1 1)");

  EXPECT_TRUE(test_a.CallIsEquivalentClass(&test_b));
  EXPECT_FALSE(test_a.CallIsEquivalentClass(pt.get()));
  EXPECT_FALSE(test_a.CallIsEquivalentClass(line.get()));

  const geos::geom::CoordinateXY c0(0.0, 0.0);
  const geos::geom::CoordinateXY c1(3.0, 4.0);
  EXPECT_TRUE(test_a.CallEqual(c0, c0, 0.0));
  EXPECT_FALSE(test_a.CallEqual(c0, c1, 0.0));
  EXPECT_TRUE(test_a.CallEqual(c0, c1, 5.0));
  EXPECT_TRUE(test_a.CallEqual(c0, c1, 5.5));
  EXPECT_FALSE(test_a.CallEqual(c0, c1, 4.99));

  auto pt_near = factory_->createPoint(geos::geom::CoordinateXY(3.0, 4.0));
  EXPECT_FALSE(pt->equalsExact(pt_near.get(), 0.0));
  EXPECT_TRUE(pt->equalsExact(pt_near.get(), 5.0));
  EXPECT_FALSE(pt->equalsExact(pt_near.get(), 4.9));

  EXPECT_NO_THROW(TestGeometry::CallCheckNotGeometryCollection(pt.get()));
  EXPECT_NO_THROW(TestGeometry::CallCheckNotGeometryCollection(line.get()));
  auto multi_pt = ReadWkt("MULTIPOINT ((0 0), (1 1))");
  EXPECT_NO_THROW(TestGeometry::CallCheckNotGeometryCollection(multi_pt.get()));

  auto gc = ReadWkt("GEOMETRYCOLLECTION (POINT (0 0))");
  EXPECT_THROW(TestGeometry::CallCheckNotGeometryCollection(gc.get()),
               geos::util::IllegalArgumentException);
}

TEST_F(GeometryTest, BaseVirtualDefaultsAndFilters) {
  TestGeometry tg(factory_.get());
  auto pt = factory_->createPoint(geos::geom::CoordinateXY(1.0, 2.0));
  auto line = ReadWkt("LINESTRING (0 0, 3 4)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  EXPECT_DOUBLE_EQ(tg.getArea(), 0.0);
  EXPECT_DOUBLE_EQ(tg.getLength(), 0.0);
  EXPECT_DOUBLE_EQ(pt->getArea(), 0.0);
  EXPECT_DOUBLE_EQ(pt->getLength(), 0.0);
  EXPECT_DOUBLE_EQ(line->getArea(), 0.0);

  EXPECT_FALSE(tg.hasCurvedComponents());
  EXPECT_FALSE(pt->hasCurvedComponents());
  EXPECT_FALSE(line->hasCurvedComponents());
  EXPECT_FALSE(poly->hasCurvedComponents());

  RecordingGeometryFilter geom_filter;
  const TestGeometry& const_tg = tg;
  const_tg.apply_ro(&geom_filter);
  tg.apply_rw(&geom_filter);
  EXPECT_THAT(geom_filter.ro_geoms_, ::testing::ElementsAre(&tg));
  EXPECT_THAT(geom_filter.rw_geoms_, ::testing::ElementsAre(&tg));

  RecordingComponentFilter comp_filter;
  const_tg.apply_ro(&comp_filter);
  tg.apply_rw(&comp_filter);
  EXPECT_THAT(comp_filter.ro_geoms_, ::testing::ElementsAre(&tg));
  EXPECT_THAT(comp_filter.rw_geoms_, ::testing::ElementsAre(&tg));

  EXPECT_EQ(tg.changed_count(), 0);
  tg.geometryChanged();
  EXPECT_EQ(tg.changed_count(), 1);

  ShiftXCoordinateFilter shift(10.0);
  line->apply_rw(&shift);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxX(), 3.0);
  line->geometryChanged();
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMinX(), 10.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxX(), 13.0);

  ComponentCounter counter;
  pt->applyComponentFilter(counter);
  EXPECT_EQ(counter.count, 1u);

  auto gc = ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (1 1, 2 2))");
  ComponentCounter gc_counter;
  gc->applyComponentFilter(gc_counter);
  EXPECT_EQ(gc_counter.count, 2u);
}

TEST_F(GeometryTest, MetadataCollectionsAndMixedDimension) {
  auto pt = ReadWkt("POINT (1 2)");
  auto line = ReadWkt("LINESTRING (0 0, 1 1)");
  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto mp = ReadWkt("MULTIPOINT ((0 0), (1 1))");
  auto ml = ReadWkt("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))");
  auto mpoly = ReadWkt("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)))");
  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");
  auto homo_gc = ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), POINT (1 1))");
  auto nested_homo_gc =
      ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), MULTIPOINT ((1 1), (2 2)))");
  auto mixed_gc =
      ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 0, 1 1))");
  auto nested_mixed_gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), GEOMETRYCOLLECTION (POLYGON ((0 0, 1 "
      "0, "
      "1 1, 0 1, 0 0))))");

  EXPECT_EQ(pt->getNumGeometries(), 1u);
  EXPECT_EQ(pt->getGeometryN(0), pt.get());
  EXPECT_EQ(mixed_gc->getNumGeometries(), 2u);
  EXPECT_NE(mixed_gc->getGeometryN(0), mixed_gc.get());

  EXPECT_FALSE(pt->isCollection());
  EXPECT_FALSE(line->isCollection());
  EXPECT_FALSE(poly->isCollection());
  EXPECT_TRUE(mp->isCollection());
  EXPECT_TRUE(ml->isCollection());
  EXPECT_TRUE(mpoly->isCollection());
  EXPECT_TRUE(empty_gc->isCollection());

  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_POINT),
            geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_LINESTRING),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_POLYGON),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_MULTIPOINT),
            geos::geom::GEOS_MULTIPOINT);

  EXPECT_TRUE(pt->isPuntal());
  EXPECT_FALSE(pt->isLineal());
  EXPECT_FALSE(pt->isPolygonal());
  EXPECT_TRUE(line->isLineal());
  EXPECT_TRUE(poly->isPolygonal());

  EXPECT_FALSE(pt->isMixedDimension());
  EXPECT_FALSE(line->isMixedDimension());
  EXPECT_FALSE(poly->isMixedDimension());
  EXPECT_FALSE(mp->isMixedDimension());
  EXPECT_FALSE(ml->isMixedDimension());
  EXPECT_FALSE(mpoly->isMixedDimension());
  EXPECT_FALSE(empty_gc->isMixedDimension());
  EXPECT_FALSE(homo_gc->isMixedDimension());
  EXPECT_FALSE(nested_homo_gc->isMixedDimension());
  EXPECT_TRUE(mixed_gc->isMixedDimension());
  EXPECT_TRUE(nested_mixed_gc->isMixedDimension());

  auto base_dim_same = geos::geom::Dimension::P;
  EXPECT_FALSE(pt->isMixedDimension(&base_dim_same));
  auto base_dim_diff = geos::geom::Dimension::L;
  EXPECT_TRUE(pt->isMixedDimension(&base_dim_diff));
}

TEST_F(GeometryTest, CentroidAndPrecisionModel) {
  auto empty_pt = factory_->createPoint();
  geos::geom::CoordinateXY cent;
  EXPECT_FALSE(empty_pt->getCentroid(cent));
  auto empty_cent = empty_pt->getCentroid();
  EXPECT_TRUE(empty_cent->isEmpty());
  EXPECT_EQ(empty_cent->getCoordinateDimension(), 2u);

  auto empty_pt_3d = factory_->createPoint(std::size_t{3});
  auto empty_cent_3d = empty_pt_3d->getCentroid();
  EXPECT_TRUE(empty_cent_3d->isEmpty());
  EXPECT_EQ(empty_cent_3d->getCoordinateDimension(), 3u);

  // Non-empty geometry whose Centroid::getCentroid returns false.
  TestGeometry non_empty_unknown(factory_.get(), false);
  EXPECT_FALSE(non_empty_unknown.getCentroid(cent));
  EXPECT_TRUE(non_empty_unknown.getCentroid()->isEmpty());

  auto pt = ReadWkt("POINT (3 4)");
  EXPECT_TRUE(pt->getCentroid(cent));
  EXPECT_DOUBLE_EQ(cent.x, 3.0);
  EXPECT_DOUBLE_EQ(cent.y, 4.0);
  auto pt_cent = pt->getCentroid();
  EXPECT_DOUBLE_EQ(pt_cent->getX(), 3.0);
  EXPECT_DOUBLE_EQ(pt_cent->getY(), 4.0);

  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto line_cent = line->getCentroid();
  EXPECT_DOUBLE_EQ(line_cent->getX(), 5.0);
  EXPECT_DOUBLE_EQ(line_cent->getY(), 0.0);

  auto poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto poly_cent = poly->getCentroid();
  EXPECT_DOUBLE_EQ(poly_cent->getX(), 2.0);
  EXPECT_DOUBLE_EQ(poly_cent->getY(), 2.0);

  // Fixed precision model rounds computed centroid coordinates.
  geos::geom::PrecisionModel fixed_pm(1.0);
  auto fixed_factory = geos::geom::GeometryFactory::create(&fixed_pm);
  geos::io::WKTReader fixed_reader(*fixed_factory);
  auto tri = fixed_reader.read("POLYGON ((0 0, 2 0, 0 2, 0 0))");
  EXPECT_TRUE(tri->getCentroid(cent));
  EXPECT_DOUBLE_EQ(cent.x, 1.0);
  EXPECT_DOUBLE_EQ(cent.y, 1.0);
}

TEST_F(GeometryTest, InteriorPointAllDimensionsAndCurvedException) {
  auto empty_pt = ReadWkt("POINT EMPTY");
  EXPECT_TRUE(empty_pt->getInteriorPoint()->isEmpty());

  auto pt = ReadWkt("POINT (3 4)");
  auto pt_int = pt->getInteriorPoint();
  EXPECT_FALSE(pt_int->isEmpty());
  EXPECT_DOUBLE_EQ(pt_int->getX(), 3.0);
  EXPECT_DOUBLE_EQ(pt_int->getY(), 4.0);

  auto mp = ReadWkt("MULTIPOINT ((0 0), (4 0), (10 0))");
  auto mp_int = mp->getInteriorPoint();
  EXPECT_TRUE(mp->covers(mp_int.get()));

  auto empty_line = ReadWkt("LINESTRING EMPTY");
  EXPECT_TRUE(empty_line->getInteriorPoint()->isEmpty());

  auto line = ReadWkt("LINESTRING (0 0, 5 0, 10 0)");
  auto line_int = line->getInteriorPoint();
  EXPECT_DOUBLE_EQ(line_int->getX(), 5.0);
  EXPECT_DOUBLE_EQ(line_int->getY(), 0.0);

  auto empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(empty_poly->getInteriorPoint()->isEmpty());

  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");
  EXPECT_TRUE(empty_gc->getInteriorPoint()->isEmpty());

  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto poly_int = poly->getInteriorPoint();
  EXPECT_FALSE(poly_int->isEmpty());
  EXPECT_TRUE(poly->contains(poly_int.get()));

  auto cs_coords = std::make_unique<geos::geom::CoordinateSequence>(
      std::size_t{0}, false, false);
  cs_coords->add(0.0, 0.0);
  cs_coords->add(1.0, 1.0);
  cs_coords->add(2.0, 0.0);
  auto curved = factory_->createCircularString(std::move(cs_coords));
  EXPECT_THROW(curved->getInteriorPoint(),
               geos::util::UnsupportedOperationException);
}

TEST_F(GeometryTest, ValiditySimplicityAndEnvelope) {
  auto valid_poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto bowtie_poly = ReadWkt("POLYGON ((0 0, 4 4, 4 0, 0 4, 0 0))");
  EXPECT_TRUE(valid_poly->isValid());
  EXPECT_FALSE(bowtie_poly->isValid());

  auto simple_line = ReadWkt("LINESTRING (0 0, 1 1, 2 0)");
  auto self_crossing_line = ReadWkt("LINESTRING (0 0, 2 2, 2 0, 0 2)");
  EXPECT_TRUE(simple_line->isSimple());
  EXPECT_FALSE(self_crossing_line->isSimple());

  auto empty_pt = ReadWkt("POINT EMPTY");
  EXPECT_TRUE(empty_pt->getEnvelope()->isEmpty());

  auto pt = ReadWkt("POINT (3 4)");
  auto pt_env = pt->getEnvelope();
  EXPECT_TRUE(pt_env->equalsExact(pt.get()));

  auto line = ReadWkt("LINESTRING (0 0, 3 4)");
  auto expected_env = ReadWkt("POLYGON ((0 0, 0 4, 3 4, 3 0, 0 0))");
  EXPECT_TRUE(line->getEnvelope()->equals(expected_env.get()));
}

TEST_F(GeometryTest, SpatialPredicatesAndRelate) {
  auto rect = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto far_rect = ReadWkt("POLYGON ((10 10, 14 10, 14 14, 10 14, 10 10))");
  auto inner_pt = ReadWkt("POINT (2 2)");
  auto boundary_pt = ReadWkt("POINT (0 2)");
  auto overlap_poly = ReadWkt("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");
  auto touching_poly = ReadWkt("POLYGON ((4 0, 8 0, 8 4, 4 4, 4 0))");
  auto tri_a = ReadWkt("POLYGON ((0 0, 4 0, 0 4, 0 0))");
  auto tri_b = ReadWkt("POLYGON ((3 3, 4 3, 3 4, 3 3))");
  auto diag_hit = ReadWkt("LINESTRING (1 1, 5 5)");
  auto diag_miss = ReadWkt("LINESTRING (0 10, 10 0)");
  auto cross_line = ReadWkt("LINESTRING (-1 2, 5 2)");

  EXPECT_TRUE(rect->disjoint(far_rect.get()));
  EXPECT_FALSE(rect->disjoint(inner_pt.get()));

  EXPECT_FALSE(rect->touches(far_rect.get()));
  EXPECT_FALSE(tri_a->touches(tri_b.get()));
  EXPECT_TRUE(rect->touches(touching_poly.get()));
  EXPECT_TRUE(rect->touches(boundary_pt.get()));
  EXPECT_FALSE(rect->touches(inner_pt.get()));

  // Rectangle optimization and envelope short-circuit branches in intersects().
  EXPECT_FALSE(rect->intersects(far_rect.get()));
  EXPECT_TRUE(rect->intersects(diag_hit.get()));
  EXPECT_FALSE(rect->intersects(diag_miss.get()));
  EXPECT_TRUE(diag_hit->intersects(rect.get()));
  EXPECT_FALSE(diag_miss->intersects(rect.get()));
  // Non-rectangle overlapping-envelope fallback branches in intersects().
  EXPECT_FALSE(tri_a->intersects(tri_b.get()));
  EXPECT_TRUE(tri_a->intersects(diag_hit.get()));

  // CurvePolygon / Point special cases in intersects().
  auto circle_coords = std::make_unique<geos::geom::CoordinateSequence>(
      std::size_t{0}, false, false);
  circle_coords->add(0.0, 0.0);
  circle_coords->add(2.0, 2.0);
  circle_coords->add(4.0, 0.0);
  circle_coords->add(2.0, -2.0);
  circle_coords->add(0.0, 0.0);
  auto circle = factory_->createCurvePolygon(
      factory_->createCircularString(std::move(circle_coords)));
  auto inside_circle =
      factory_->createPoint(geos::geom::CoordinateXY(2.0, 0.0));
  auto boundary_circle_pt =
      factory_->createPoint(geos::geom::CoordinateXY(0.0, 0.0));
  auto in_bbox_outside_circle =
      factory_->createPoint(geos::geom::CoordinateXY(0.1, 1.9));
  auto far_pt = factory_->createPoint(geos::geom::CoordinateXY(10.0, 10.0));
  auto empty_pt = ReadWkt("POINT EMPTY");

  EXPECT_TRUE(circle->intersects(inside_circle.get()));
  EXPECT_TRUE(circle->intersects(boundary_circle_pt.get()));
  EXPECT_FALSE(circle->intersects(in_bbox_outside_circle.get()));
  EXPECT_FALSE(circle->intersects(far_pt.get()));
  EXPECT_FALSE(circle->intersects(empty_pt.get()));
  EXPECT_TRUE(inside_circle->intersects(circle.get()));
  EXPECT_TRUE(boundary_circle_pt->intersects(circle.get()));
  EXPECT_FALSE(in_bbox_outside_circle->intersects(circle.get()));
  EXPECT_FALSE(far_pt->intersects(circle.get()));
  EXPECT_FALSE(empty_pt->intersects(circle.get()));

  EXPECT_TRUE(rect->covers(inner_pt.get()));
  EXPECT_TRUE(rect->covers(boundary_pt.get()));
  EXPECT_FALSE(rect->covers(far_rect.get()));

  EXPECT_TRUE(inner_pt->coveredBy(rect.get()));
  EXPECT_TRUE(boundary_pt->coveredBy(rect.get()));
  EXPECT_FALSE(far_rect->coveredBy(rect.get()));

  EXPECT_TRUE(rect->contains(inner_pt.get()));
  EXPECT_FALSE(rect->contains(boundary_pt.get()));
  EXPECT_FALSE(rect->contains(far_rect.get()));

  EXPECT_TRUE(inner_pt->within(rect.get()));
  EXPECT_FALSE(boundary_pt->within(rect.get()));
  EXPECT_FALSE(far_rect->within(rect.get()));

  EXPECT_TRUE(cross_line->crosses(rect.get()));
  EXPECT_FALSE(diag_miss->crosses(rect.get()));
  EXPECT_FALSE(rect->crosses(overlap_poly.get()));

  EXPECT_TRUE(rect->overlaps(overlap_poly.get()));
  EXPECT_FALSE(rect->overlaps(touching_poly.get()));
  EXPECT_FALSE(rect->overlaps(inner_pt.get()));

  auto rect_reordered = ReadWkt("POLYGON ((4 4, 0 4, 0 0, 4 0, 4 4))");
  EXPECT_TRUE(rect->equals(rect_reordered.get()));
  EXPECT_FALSE(rect->equalsExact(rect_reordered.get()));
  EXPECT_FALSE(rect->equals(overlap_poly.get()));
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(empty_pt->equals(empty_poly.get()));
  EXPECT_FALSE(empty_pt->equals(inner_pt.get()));

  auto im_ptr = rect->relate(overlap_poly.get());
  EXPECT_EQ(im_ptr->toString(), "212101212");
  auto im_ref = rect->relate(*inner_pt);
  EXPECT_TRUE(im_ref->isContains());

  EXPECT_TRUE(rect->relate(inner_pt.get(), "0F2FF1FF2"));
  EXPECT_FALSE(rect->relate(inner_pt.get(), "FF*FF****"));
  EXPECT_TRUE(rect->relate(*overlap_poly, "212101212"));
  EXPECT_FALSE(rect->relate(*far_rect, "T********"));
}

TEST_F(GeometryTest, TextAndStreamFormatting) {
  auto pt = ReadWkt("POINT (1 2)");
  EXPECT_EQ(pt->toText(), "POINT (1 2)");
  EXPECT_EQ(pt->toString(), pt->toText());

  auto empty_line = ReadWkt("LINESTRING EMPTY");
  EXPECT_EQ(empty_line->toText(), "LINESTRING EMPTY");
  EXPECT_EQ(empty_line->toString(), "LINESTRING EMPTY");

  auto poly = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  std::ostringstream oss;
  oss << *poly;
  ASSERT_FALSE(oss.str().empty());

  std::istringstream iss(oss.str());
  geos::io::WKBReader wkb_reader(*factory_);
  auto decoded = wkb_reader.readHEX(iss);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(poly->equalsExact(decoded.get()));
}

TEST_F(GeometryTest, BufferConvexHullAndSetOperations) {
  auto pt = ReadWkt("POINT (0 0)");
  auto buf_default = pt->buffer(2.0);
  EXPECT_GT(buf_default->getArea(), 11.0);
  EXPECT_TRUE(pt->buffer(0.0)->isEmpty());
  EXPECT_TRUE(pt->buffer(-1.0)->isEmpty());

  auto buf_low_seg = pt->buffer(2.0, 2);
  auto buf_high_seg = pt->buffer(2.0, 16);
  EXPECT_LT(buf_low_seg->getNumPoints(), buf_high_seg->getNumPoints());
  EXPECT_GT(buf_high_seg->getArea(), buf_low_seg->getArea());

  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto buf_butt =
      line->buffer(1.0, 8, geos::operation::buffer::BufferOp::CAP_BUTT);
  auto buf_square =
      line->buffer(1.0, 8, geos::operation::buffer::BufferOp::CAP_SQUARE);
  auto buf_round =
      line->buffer(1.0, 8, geos::operation::buffer::BufferOp::CAP_ROUND);
  EXPECT_NEAR(buf_butt->getArea(), 20.0, 1e-6);
  EXPECT_NEAR(buf_square->getArea(), 24.0, 1e-6);
  EXPECT_NEAR(buf_butt->getEnvelopeInternal()->getMinX(), 0.0, 1e-9);
  EXPECT_NEAR(buf_butt->getEnvelopeInternal()->getMaxX(), 10.0, 1e-9);
  EXPECT_NEAR(buf_butt->getEnvelopeInternal()->getMinY(), -1.0, 1e-9);
  EXPECT_NEAR(buf_butt->getEnvelopeInternal()->getMaxY(), 1.0, 1e-9);
  EXPECT_NEAR(buf_square->getEnvelopeInternal()->getMinX(), -1.0, 1e-9);
  EXPECT_NEAR(buf_square->getEnvelopeInternal()->getMaxX(), 11.0, 1e-9);
  EXPECT_NEAR(buf_square->getEnvelopeInternal()->getMinY(), -1.0, 1e-9);
  EXPECT_NEAR(buf_square->getEnvelopeInternal()->getMaxY(), 1.0, 1e-9);
  EXPECT_GT(buf_round->getArea(), 22.5);
  EXPECT_LT(buf_round->getArea(), 23.2);

  auto big_sq = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto eroded = big_sq->buffer(-0.5);
  EXPECT_NEAR(eroded->getArea(), 9.0, 1e-6);
  EXPECT_TRUE(ReadWkt("POINT EMPTY")->buffer(1.0)->isEmpty());

  auto mp = ReadWkt("MULTIPOINT ((0 0), (4 0), (1 1), (0 4))");
  auto hull = mp->convexHull();
  EXPECT_TRUE(hull->isPolygonal());
  EXPECT_NEAR(hull->getArea(), 8.0, 1e-9);
  EXPECT_TRUE(hull->covers(mp.get()));
  EXPECT_TRUE(ReadWkt("POINT EMPTY")->convexHull()->isEmpty());
  EXPECT_TRUE(ReadWkt("MULTIPOINT ((0 0), (1 0), (3 0))")
                  ->convexHull()
                  ->equals(ReadWkt("LINESTRING (0 0, 3 0)").get()));

  auto sq_a = ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto sq_b = ReadWkt("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");

  auto inter = sq_a->intersection(sq_b.get());
  EXPECT_NEAR(inter->getArea(), 2.0, 1e-9);

  auto uni = sq_a->Union(sq_b.get());
  EXPECT_NEAR(uni->getArea(), 6.0, 1e-9);

  auto diff = sq_a->difference(sq_b.get());
  EXPECT_NEAR(diff->getArea(), 2.0, 1e-9);

  auto sym_diff = sq_a->symDifference(sq_b.get());
  EXPECT_NEAR(sym_diff->getArea(), 4.0, 1e-9);

  auto overlapping_gc = ReadWkt(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), POLYGON ((1 0, "
      "3 0, 3 2, 1 2, 1 0)))");
  auto unary_union = overlapping_gc->Union();
  EXPECT_NEAR(unary_union->getArea(), 6.0, 1e-9);
  EXPECT_TRUE(ReadWkt("GEOMETRYCOLLECTION EMPTY")->Union()->isEmpty());
}

TEST_F(GeometryTest, DistanceAndWithinDistance) {
  auto pt0 = ReadWkt("POINT (0 0)");
  auto pt1 = ReadWkt("POINT (3 4)");
  auto empty_pt = ReadWkt("POINT EMPTY");

  EXPECT_DOUBLE_EQ(pt0->distance(pt1.get()), 5.0);
  EXPECT_DOUBLE_EQ(pt0->distance(pt0.get()), 0.0);
  EXPECT_DOUBLE_EQ(pt0->distance(empty_pt.get()), 0.0);

  EXPECT_TRUE(pt0->isWithinDistance(pt1.get(), 5.0));
  EXPECT_TRUE(pt0->isWithinDistance(pt1.get(), 6.0));
  EXPECT_FALSE(pt0->isWithinDistance(pt1.get(), 4.9));
  EXPECT_FALSE(pt0->isWithinDistance(empty_pt.get(), 1.0));
}

TEST_F(GeometryTest, CompareToAndGeometryGreaterThen) {
  auto pt_empty_a = ReadWkt("POINT EMPTY");
  auto pt_empty_b = ReadWkt("POINT EMPTY");
  auto pt_00 = ReadWkt("POINT (0 0)");
  auto pt_00_copy = ReadWkt("POINT (0 0)");
  auto pt_10 = ReadWkt("POINT (1 0)");
  auto line = ReadWkt("LINESTRING (0 0, 1 1)");
  auto poly = ReadWkt("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");

  EXPECT_EQ(pt_00->compareTo(pt_00.get()), 0);
  EXPECT_EQ(pt_00->compareTo(line.get()), -1);
  EXPECT_EQ(line->compareTo(pt_00.get()), 1);
  EXPECT_EQ(poly->compareTo(line.get()), 1);

  EXPECT_EQ(pt_empty_a->compareTo(pt_empty_b.get()), 0);
  EXPECT_EQ(pt_empty_a->compareTo(pt_00.get()), -1);
  EXPECT_EQ(pt_00->compareTo(pt_empty_a.get()), 1);

  EXPECT_EQ(pt_00->compareTo(pt_00_copy.get()), 0);
  EXPECT_LT(pt_00->compareTo(pt_10.get()), 0);
  EXPECT_GT(pt_10->compareTo(pt_00.get()), 0);

  geos::geom::GeometryGreaterThen greater;
  EXPECT_TRUE(greater(line.get(), pt_00.get()));
  EXPECT_FALSE(greater(pt_00.get(), line.get()));
  EXPECT_FALSE(greater(pt_00.get(), pt_00_copy.get()));

  std::vector<const geos::geom::Geometry*> geoms = {
      pt_empty_a.get(), pt_00.get(), line.get(), pt_10.get(), poly.get()};
  std::sort(geoms.begin(), geoms.end(), geos::geom::GeometryGreaterThen());
  EXPECT_THAT(geoms, ::testing::ElementsAre(poly.get(), line.get(), pt_10.get(),
                                            pt_00.get(), pt_empty_a.get()));
}

TEST_F(GeometryTest, ReverseDimensionHelpersAndAdditionalMetadata) {
  TestGeometry tg(factory_.get(), false, geos::geom::Dimension::L,
                  geos::geom::SORTINDEX_LINESTRING);
  EXPECT_TRUE(tg.hasDimension(geos::geom::Dimension::L));
  EXPECT_FALSE(tg.hasDimension(geos::geom::Dimension::P));
  EXPECT_TRUE(tg.isDimensionStrict(geos::geom::Dimension::L));
  EXPECT_FALSE(tg.isDimensionStrict(geos::geom::Dimension::A));
  auto tg_rev = tg.reverse();
  EXPECT_TRUE(tg_rev->equalsExact(&tg));

  auto line = ReadWkt("LINESTRING (0 0, 1 2, 3 4)");
  auto line_rev = line->reverse();
  EXPECT_TRUE(
      line_rev->equalsExact(ReadWkt("LINESTRING (3 4, 1 2, 0 0)").get()));

  auto empty_ring = factory_->createLinearRing();
  auto empty_cc = factory_->createCompoundCurve();
  auto empty_cp = factory_->createCurvePolygon(false, false);
  auto empty_mc = factory_->createMultiCurve();
  auto empty_ms = factory_->createMultiSurface();
  EXPECT_FALSE(empty_ring->isCollection());
  EXPECT_FALSE(empty_cc->isCollection());
  EXPECT_FALSE(empty_cp->isCollection());
  EXPECT_FALSE(empty_mc->isCollection());
  EXPECT_FALSE(empty_ms->isCollection());

  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_LINEARRING),
            geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_MULTILINESTRING),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_MULTIPOLYGON),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(
      geos::geom::Geometry::multiTypeId(geos::geom::GEOS_GEOMETRYCOLLECTION),
      geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_CIRCULARSTRING),
            geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_COMPOUNDCURVE),
            geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_CURVEPOLYGON),
            geos::geom::GEOS_CURVEPOLYGON);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_MULTICURVE),
            geos::geom::GEOS_MULTICURVE);
  EXPECT_EQ(geos::geom::Geometry::multiTypeId(geos::geom::GEOS_MULTISURFACE),
            geos::geom::GEOS_MULTISURFACE);

  EXPECT_TRUE(ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY)")
                  ->isMixedDimension());
  EXPECT_FALSE(ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, POINT (1 1))")
                   ->isMixedDimension());
  EXPECT_FALSE(
      ReadWkt("GEOMETRYCOLLECTION (GEOMETRYCOLLECTION EMPTY, POINT (0 0))")
          ->isMixedDimension());

  geos::geom::GeomPtrPair pair{ReadWkt("POINT (1 2)"), ReadWkt("POINT (3 4)")};
  EXPECT_DOUBLE_EQ(pair.first->distance(pair.second.get()),
                   std::hypot(2.0, 2.0));
}

TEST_F(GeometryTest, CurvePolygonHoleIntersectsAndCentroidInteriorPointDims) {
  auto shell_coords = std::make_unique<geos::geom::CoordinateSequence>(
      std::size_t{0}, false, false);
  shell_coords->add(0.0, 0.0);
  shell_coords->add(4.0, 4.0);
  shell_coords->add(8.0, 0.0);
  shell_coords->add(4.0, -4.0);
  shell_coords->add(0.0, 0.0);
  auto shell = factory_->createCircularString(std::move(shell_coords));

  auto hole_coords = std::make_unique<geos::geom::CoordinateSequence>(
      std::size_t{0}, false, false);
  hole_coords->add(3.0, -1.0);
  hole_coords->add(5.0, -1.0);
  hole_coords->add(5.0, 1.0);
  hole_coords->add(3.0, 1.0);
  hole_coords->add(3.0, -1.0);
  std::vector<std::unique_ptr<geos::geom::Curve>> holes;
  holes.push_back(factory_->createLinearRing(std::move(hole_coords)));

  auto cp_with_hole =
      factory_->createCurvePolygon(std::move(shell), std::move(holes));
  auto in_hole = factory_->createPoint(geos::geom::CoordinateXY(4.0, 0.0));
  auto on_hole = factory_->createPoint(geos::geom::CoordinateXY(3.0, 0.0));
  auto in_shell = factory_->createPoint(geos::geom::CoordinateXY(1.0, 0.0));
  auto empty_cp = factory_->createCurvePolygon(false, false);

  EXPECT_FALSE(cp_with_hole->intersects(in_hole.get()));
  EXPECT_FALSE(in_hole->intersects(cp_with_hole.get()));
  EXPECT_TRUE(cp_with_hole->intersects(on_hole.get()));
  EXPECT_TRUE(on_hole->intersects(cp_with_hole.get()));
  EXPECT_TRUE(cp_with_hole->intersects(in_shell.get()));
  EXPECT_TRUE(in_shell->intersects(cp_with_hole.get()));
  EXPECT_FALSE(empty_cp->intersects(in_hole.get()));
  EXPECT_FALSE(in_hole->intersects(empty_cp.get()));

  auto empty_pt_3d = factory_->createPoint(true, false);
  auto empty_pt_4d = factory_->createPoint(true, true);
  EXPECT_EQ(empty_pt_3d->getCentroid()->getCoordinateDimension(), 3u);
  EXPECT_EQ(empty_pt_3d->getInteriorPoint()->getCoordinateDimension(), 3u);
  EXPECT_EQ(empty_pt_4d->getCentroid()->getCoordinateDimension(), 4u);
  EXPECT_EQ(empty_pt_4d->getInteriorPoint()->getCoordinateDimension(), 4u);

  auto line_z = ReadWkt("LINESTRING Z (0 0 1, 5 0 2, 10 0 3)");
  auto line_z_int = line_z->getInteriorPoint();
  EXPECT_DOUBLE_EQ(line_z_int->getX(), 5.0);
  EXPECT_DOUBLE_EQ(line_z_int->getY(), 0.0);
  EXPECT_DOUBLE_EQ(line_z_int->getZ(), 2.0);
  EXPECT_TRUE(std::isnan(line_z_int->getM()));

  auto gc_pt = ReadWkt("GEOMETRYCOLLECTION (POINT (1 2))");
  auto gc_line = ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 5 0, 10 0))");
  auto gc_poly =
      ReadWkt("GEOMETRYCOLLECTION (POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))");
  EXPECT_DOUBLE_EQ(gc_pt->getInteriorPoint()->getX(), 1.0);
  EXPECT_DOUBLE_EQ(gc_line->getInteriorPoint()->getX(), 5.0);
  EXPECT_TRUE(gc_poly->contains(gc_poly->getInteriorPoint().get()));
}

TEST_F(GeometryTest, ProtectedVectorCompareAndToGeometryArrayHelpers) {
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto pt0 = ReadWkt("POINT (0 0)");
  auto pt1 = ReadWkt("POINT (1 0)");

  std::vector<const geos::geom::Geometry*> empty_vec;
  std::vector<const geos::geom::Geometry*> only_empty = {empty_pt.get()};
  std::vector<const geos::geom::Geometry*> with_non_empty = {empty_pt.get(),
                                                             pt0.get()};
  EXPECT_FALSE(TestGeometry::CallHasNonEmptyElements(&empty_vec));
  EXPECT_FALSE(TestGeometry::CallHasNonEmptyElements(&only_empty));
  EXPECT_TRUE(TestGeometry::CallHasNonEmptyElements(&with_non_empty));

  std::vector<const geos::geom::Geometry*> with_null = {pt0.get(), nullptr};
  EXPECT_FALSE(TestGeometry::CallHasNullElementsVec(&empty_vec));
  EXPECT_FALSE(TestGeometry::CallHasNullElementsVec(&with_non_empty));
  EXPECT_TRUE(TestGeometry::CallHasNullElementsVec(&with_null));

  std::vector<const geos::geom::Geometry*> seq_a = {pt0.get()};
  std::vector<const geos::geom::Geometry*> seq_a_copy = {pt0.get()};
  std::vector<const geos::geom::Geometry*> seq_b = {pt1.get()};
  std::vector<const geos::geom::Geometry*> seq_longer = {pt0.get(), pt1.get()};
  EXPECT_EQ(TestGeometry::CallCompare(seq_a, seq_a_copy), 0);
  EXPECT_LT(TestGeometry::CallCompare(seq_a, seq_b), 0);
  EXPECT_GT(TestGeometry::CallCompare(seq_b, seq_a), 0);
  EXPECT_LT(TestGeometry::CallCompare(seq_a, seq_longer), 0);
  EXPECT_GT(TestGeometry::CallCompare(seq_longer, seq_a), 0);

  std::vector<std::unique_ptr<geos::geom::Point>> pts;
  pts.push_back(factory_->createPoint(geos::geom::CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint(geos::geom::CoordinateXY(3.0, 4.0)));
  auto geom_vec = TestGeometry::CallToGeometryArrayFromPoints(std::move(pts));
  EXPECT_EQ(geom_vec.size(), 2u);
  auto moved_vec = TestGeometry::CallToGeometryArray(std::move(geom_vec));
  EXPECT_EQ(moved_vec.size(), 2u);
}

}  // namespace
}  // namespace geos_google
