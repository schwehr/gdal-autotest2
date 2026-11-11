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

#include "third_party/geos/include/geos/geom/util/GeometryFixer.h"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::util::GeometryFixer;
using geos::io::WKTReader;
using geos::util::UnsupportedOperationException;

class GeometryFixerTest : public ::testing::Test {
 protected:
  GeometryFixerTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<CoordinateSequence> MakeXYSeq(
      std::initializer_list<CoordinateXY> coords) const {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    for (const CoordinateXY& c : coords) {
      seq->add(c);
    }
    return seq;
  }

  void ExpectTopoEquals(const Geometry& actual,
                        const std::string& expected_wkt) const {
    auto expected = ReadWkt(expected_wkt);
    EXPECT_TRUE(actual.isValid());
    EXPECT_TRUE(actual.equals(expected.get()))
        << "Actual: " << actual.toString() << "\nExpected: " << expected_wkt;
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(GeometryFixerTest, EmptyCollectionsReturnedAsClones) {
  for (const char* wkt : {"MULTIPOINT EMPTY", "MULTILINESTRING EMPTY",
                          "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY"}) {
    auto input = ReadWkt(wkt);
    auto fixed = GeometryFixer::fix(input.get());
    ASSERT_NE(fixed, nullptr);
    EXPECT_NE(fixed.get(), input.get());
    EXPECT_EQ(fixed->getGeometryTypeId(), input->getGeometryTypeId());
    EXPECT_TRUE(fixed->isEmpty());
    EXPECT_TRUE(fixed->isValid());

    GeometryFixer fixer(input.get());
    fixer.setKeepCollapsed(true);
    auto fixed_keep = fixer.getResult();
    EXPECT_EQ(fixed_keep->getGeometryTypeId(), input->getGeometryTypeId());
    EXPECT_TRUE(fixed_keep->isEmpty());
  }
}

TEST_F(GeometryFixerTest, UnsupportedCurvedGeometryThrows) {
  auto circ = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(GeometryFixer::fix(circ.get()), UnsupportedOperationException);

  auto comp = ReadWkt("COMPOUNDCURVE ((0 0, 1 1))");
  EXPECT_THROW(GeometryFixer::fix(comp.get()), UnsupportedOperationException);

  auto cpoly =
      ReadWkt("CURVEPOLYGON (CIRCULARSTRING (0 0, 4 0, 4 4, 0 4, 0 0))");
  EXPECT_THROW(GeometryFixer::fix(cpoly.get()), UnsupportedOperationException);

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(ReadWkt("POINT (1 2)"));
  geoms.push_back(std::move(circ));
  auto col = factory_->createGeometryCollection(std::move(geoms));
  EXPECT_THROW(GeometryFixer::fix(col.get()), UnsupportedOperationException);
}

TEST_F(GeometryFixerTest, FixPointValidEmptyAndNonFinite) {
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto fixed_empty = GeometryFixer::fix(empty_pt.get());
  EXPECT_EQ(fixed_empty->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(fixed_empty->isEmpty());
  EXPECT_TRUE(fixed_empty->isValid());

  auto valid_pt = ReadWkt("POINT (3 4)");
  auto fixed_valid = GeometryFixer::fix(valid_pt.get());
  EXPECT_NE(fixed_valid.get(), valid_pt.get());
  EXPECT_TRUE(fixed_valid->isValid());
  EXPECT_TRUE(fixed_valid->equalsIdentical(valid_pt.get()));

  auto pt_z = factory_->createPoint(Coordinate(1.0, 2.0, 3.0));
  auto fixed_z = GeometryFixer::fix(pt_z.get());
  EXPECT_TRUE(fixed_z->equalsIdentical(pt_z.get()));

  auto pt_m = factory_->createPoint(CoordinateXYM(1.0, 2.0, 4.0));
  auto fixed_m = GeometryFixer::fix(pt_m.get());
  EXPECT_TRUE(fixed_m->equalsIdentical(pt_m.get()));

  auto pt_zm = factory_->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0));
  auto fixed_zm = GeometryFixer::fix(pt_zm.get());
  EXPECT_TRUE(fixed_zm->equalsIdentical(pt_zm.get()));

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();
  for (const CoordinateXY& bad :
       {CoordinateXY(kNaN, 1.0), CoordinateXY(1.0, kNaN),
        CoordinateXY(kInf, 1.0), CoordinateXY(1.0, -kInf)}) {
    auto bad_pt = factory_->createPoint(bad);
    auto fixed_bad = GeometryFixer::fix(bad_pt.get());
    EXPECT_EQ(fixed_bad->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_TRUE(fixed_bad->isEmpty());
    EXPECT_TRUE(fixed_bad->isValid());
  }
}

TEST_F(GeometryFixerTest, FixMultiPointFiltersEmptyAndInvalid) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();

  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint());
  pts.push_back(factory_->createPoint(CoordinateXY(kNaN, 5.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(kInf, 0.0)));
  pts.push_back(factory_->createPoint(CoordinateXY(3.0, 4.0)));
  auto mp = factory_->createMultiPoint(std::move(pts));

  auto fixed = GeometryFixer::fix(mp.get());
  auto expected = ReadWkt("MULTIPOINT ((1 2), (3 4))");
  EXPECT_TRUE(fixed->isValid());
  EXPECT_TRUE(fixed->equalsExact(expected.get()));

  std::vector<std::unique_ptr<Point>> all_bad_pts;
  all_bad_pts.push_back(factory_->createPoint());
  all_bad_pts.push_back(factory_->createPoint(CoordinateXY(kNaN, kNaN)));
  auto mp_all_bad = factory_->createMultiPoint(std::move(all_bad_pts));
  auto fixed_all_bad = GeometryFixer::fix(mp_all_bad.get());
  EXPECT_EQ(fixed_all_bad->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(fixed_all_bad->isEmpty());
}

TEST_F(GeometryFixerTest, FixLineStringValidEmptyRepeatedAndInvalid) {
  auto empty_ls = ReadWkt("LINESTRING EMPTY");
  auto fixed_empty = GeometryFixer::fix(empty_ls.get());
  EXPECT_EQ(fixed_empty->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(fixed_empty->isEmpty());

  GeometryFixer empty_fixer(empty_ls.get());
  empty_fixer.setKeepCollapsed(true);
  auto fixed_empty_keep = empty_fixer.getResult();
  EXPECT_EQ(fixed_empty_keep->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(fixed_empty_keep->isEmpty());

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();
  auto messy = factory_->createLineString(MakeXYSeq({{kNaN, 0.0},
                                                     {0.0, 0.0},
                                                     {0.0, 0.0},
                                                     {1.0, kNaN},
                                                     {1.0, 1.0},
                                                     {1.0, 1.0},
                                                     {kInf, 2.0},
                                                     {2.0, 0.0}}));
  auto fixed_messy = GeometryFixer::fix(messy.get());
  auto expected_messy = ReadWkt("LINESTRING (0 0, 1 1, 2 0)");
  EXPECT_TRUE(fixed_messy->isValid());
  EXPECT_TRUE(fixed_messy->equalsExact(expected_messy.get()));

  auto seq_m = std::make_unique<CoordinateSequence>(0u, false, true);
  seq_m->add(CoordinateXYM(0.0, 0.0, 5.0));
  seq_m->add(CoordinateXYM(0.0, 0.0, 6.0));
  seq_m->add(CoordinateXYM(2.0, 3.0, 9.0));
  auto ls_m = factory_->createLineString(std::move(seq_m));
  auto fixed_m = GeometryFixer::fix(ls_m.get());
  EXPECT_TRUE(fixed_m->hasM());
  EXPECT_FALSE(fixed_m->hasZ());
  EXPECT_EQ(fixed_m->getNumPoints(), 2u);
  auto fixed_m_coords = fixed_m->getCoordinates();
  EXPECT_DOUBLE_EQ(fixed_m_coords->getOrdinate(0, CoordinateSequence::M), 5.0);
  EXPECT_DOUBLE_EQ(fixed_m_coords->getOrdinate(1, CoordinateSequence::M), 9.0);

  auto collapsed = factory_->createLineString(
      MakeXYSeq({{kNaN, 0.0}, {5.0, 6.0}, {5.0, 6.0}}));
  GeometryFixer fixer(collapsed.get());
  auto collapsed_empty = fixer.getResult();
  EXPECT_EQ(collapsed_empty->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(collapsed_empty->isEmpty());

  fixer.setKeepCollapsed(true);
  auto collapsed_kept = fixer.getResult();
  auto expected_pt = ReadWkt("POINT (5 6)");
  EXPECT_EQ(collapsed_kept->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(collapsed_kept->equalsExact(expected_pt.get()));

  fixer.setKeepCollapsed(false);
  auto toggled_back = fixer.getResult();
  EXPECT_EQ(toggled_back->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(toggled_back->isEmpty());

  auto all_invalid =
      factory_->createLineString(MakeXYSeq({{kNaN, 0.0}, {kInf, 1.0}}));
  GeometryFixer all_invalid_fixer(all_invalid.get());
  all_invalid_fixer.setKeepCollapsed(true);
  auto fixed_all_invalid = all_invalid_fixer.getResult();
  EXPECT_EQ(fixed_all_invalid->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(fixed_all_invalid->isEmpty());
}

TEST_F(GeometryFixerTest, FixLinearRingAllPaths) {
  auto empty_ring = factory_->createLinearRing();
  auto fixed_empty = GeometryFixer::fix(empty_ring.get());
  EXPECT_EQ(fixed_empty->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(fixed_empty->isEmpty());
  EXPECT_TRUE(fixed_empty->isValid());

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();

  auto valid_ring = factory_->createLinearRing(MakeXYSeq({{0.0, 0.0},
                                                          {0.0, 10.0},
                                                          {0.0, 10.0},
                                                          {kNaN, 5.0},
                                                          {10.0, 10.0},
                                                          {10.0, 0.0},
                                                          {0.0, 0.0}}));
  auto fixed_valid = GeometryFixer::fix(valid_ring.get());
  EXPECT_EQ(fixed_valid->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(fixed_valid->isValid());
  auto expected_ring = ReadWkt("LINEARRING (0 0, 0 10, 10 10, 10 0, 0 0)");
  EXPECT_TRUE(fixed_valid->equalsExact(expected_ring.get()));

  auto bowtie_ring = ReadWkt("LINEARRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  auto fixed_bowtie = GeometryFixer::fix(bowtie_ring.get());
  EXPECT_EQ(fixed_bowtie->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(fixed_bowtie->isValid());
  EXPECT_EQ(fixed_bowtie->getNumPoints(), 5u);

  auto three_pt_ring = factory_->createLinearRing(
      MakeXYSeq({{0.0, 0.0}, {kNaN, 0.0}, {1.0, 1.0}, {0.0, 0.0}}));
  GeometryFixer three_fixer(three_pt_ring.get());
  auto three_collapsed = three_fixer.getResult();
  EXPECT_EQ(three_collapsed->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(three_collapsed->isEmpty());

  three_fixer.setKeepCollapsed(true);
  auto three_kept = three_fixer.getResult();
  EXPECT_EQ(three_kept->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  auto expected_three_line = ReadWkt("LINESTRING (0 0, 1 1, 0 0)");
  EXPECT_TRUE(three_kept->equalsExact(expected_three_line.get()));

  auto one_pt_ring = factory_->createLinearRing(
      MakeXYSeq({{2.0, 3.0}, {2.0, 3.0}, {2.0, 3.0}, {2.0, 3.0}}));
  GeometryFixer one_fixer(one_pt_ring.get());
  EXPECT_TRUE(one_fixer.getResult()->isEmpty());
  one_fixer.setKeepCollapsed(true);
  auto one_kept = one_fixer.getResult();
  EXPECT_EQ(one_kept->getGeometryTypeId(), geos::geom::GEOS_POINT);
  auto expected_one_pt = ReadWkt("POINT (2 3)");
  EXPECT_TRUE(one_kept->equalsExact(expected_one_pt.get()));

  auto inf_ring = factory_->createLinearRing(
      MakeXYSeq({{kInf, kInf}, {kInf, kInf}, {kInf, kInf}}));
  GeometryFixer inf_fixer(inf_ring.get());
  inf_fixer.setKeepCollapsed(true);
  auto inf_result = inf_fixer.getResult();
  EXPECT_EQ(inf_result->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(inf_result->isEmpty());
}

TEST_F(GeometryFixerTest, FixMultiLineStringAllPaths) {
  auto mls_with_empty =
      ReadWkt("MULTILINESTRING (EMPTY, (0 0, 0 0, 1 1), (2 2, 3 3))");
  auto fixed_mls = GeometryFixer::fix(mls_with_empty.get());
  auto expected_mls = ReadWkt("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))");
  EXPECT_EQ(fixed_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(fixed_mls->equalsExact(expected_mls.get()));

  auto mixed_mls = ReadWkt("MULTILINESTRING ((0 0, 1 1), (5 5, 5 5))");
  GeometryFixer mixed_fixer(mixed_mls.get());
  auto single_line = mixed_fixer.getResult();
  EXPECT_EQ(single_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  auto expected_single = ReadWkt("LINESTRING (0 0, 1 1)");
  EXPECT_TRUE(single_line->equalsExact(expected_single.get()));

  mixed_fixer.setKeepCollapsed(true);
  auto mixed_kept = mixed_fixer.getResult();
  EXPECT_EQ(mixed_kept->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(mixed_kept->getNumGeometries(), 2u);
  EXPECT_EQ(mixed_kept->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(mixed_kept->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);

  auto two_collapsed = ReadWkt("MULTILINESTRING ((5 5, 5 5), (6 6, 6 6))");
  GeometryFixer two_fixer(two_collapsed.get());
  auto two_empty = two_fixer.getResult();
  EXPECT_EQ(two_empty->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(two_empty->isEmpty());

  two_fixer.setKeepCollapsed(true);
  auto two_kept = two_fixer.getResult();
  EXPECT_EQ(two_kept->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(two_kept->getNumGeometries(), 2u);
  EXPECT_EQ(two_kept->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(two_kept->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);

  auto single_collapsed = ReadWkt("MULTILINESTRING (EMPTY, (5 5, 5 5))");
  GeometryFixer single_fixer(single_collapsed.get());
  single_fixer.setKeepCollapsed(true);
  auto single_pt = single_fixer.getResult();
  EXPECT_EQ(single_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  auto expected_pt = ReadWkt("POINT (5 5)");
  EXPECT_TRUE(single_pt->equalsExact(expected_pt.get()));
}

TEST_F(GeometryFixerTest, FixPolygonSimpleAndCollapsed) {
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  GeometryFixer empty_fixer(empty_poly.get());
  auto fixed_empty = empty_fixer.getResult();
  EXPECT_EQ(fixed_empty->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_empty->isEmpty());

  empty_fixer.setKeepCollapsed(true);
  auto empty_keep = empty_fixer.getResult();
  EXPECT_EQ(empty_keep->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(empty_keep->isEmpty());

  auto line_collapsed = ReadWkt("POLYGON ((0 0, 1 0, 1 0, 0 0))");
  GeometryFixer line_fixer(line_collapsed.get());
  EXPECT_EQ(line_fixer.getResult()->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(line_fixer.getResult()->isEmpty());

  line_fixer.setKeepCollapsed(true);
  auto kept_line = line_fixer.getResult();
  EXPECT_EQ(kept_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  auto expected_line = ReadWkt("LINESTRING (0 0, 1 0, 0 0)");
  EXPECT_TRUE(kept_line->equalsExact(expected_line.get()));

  auto pt_collapsed = ReadWkt("POLYGON ((7 8, 7 8, 7 8, 7 8))");
  GeometryFixer pt_fixer(pt_collapsed.get());
  EXPECT_TRUE(pt_fixer.getResult()->isEmpty());
  pt_fixer.setKeepCollapsed(true);
  auto kept_pt = pt_fixer.getResult();
  EXPECT_EQ(kept_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  auto expected_pt = ReadWkt("POINT (7 8)");
  EXPECT_TRUE(kept_pt->equalsExact(expected_pt.get()));

  auto repeated_poly = ReadWkt("POLYGON ((0 0, 0 0, 0 10, 10 10, 10 0, 0 0))");
  auto fixed_repeated = GeometryFixer::fix(repeated_poly.get());
  EXPECT_EQ(fixed_repeated->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(fixed_repeated->getNumPoints(), 5u);
  ExpectTopoEquals(*fixed_repeated, "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");

  auto bowtie = ReadWkt("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  auto fixed_bowtie = GeometryFixer::fix(bowtie.get());
  EXPECT_EQ(fixed_bowtie->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(fixed_bowtie->getNumGeometries(), 2u);
  EXPECT_TRUE(fixed_bowtie->isValid());
  EXPECT_NEAR(fixed_bowtie->getArea(), 50.0, 1e-9);
}

TEST_F(GeometryFixerTest, FixPolygonWithHolesAllCases) {
  auto valid_with_hole = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(2 2, 2 6, 6 6, 6 2, 2 2))");
  auto fixed_valid = GeometryFixer::fix(valid_with_hole.get());
  EXPECT_EQ(fixed_valid->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_valid->isValid());
  EXPECT_NEAR(fixed_valid->getArea(), 84.0, 1e-9);

  auto bowtie_hole = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(2 2, 6 6, 6 2, 2 6, 2 2))");
  auto fixed_bowtie_hole = GeometryFixer::fix(bowtie_hole.get());
  EXPECT_EQ(fixed_bowtie_hole->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_bowtie_hole->isValid());
  EXPECT_NEAR(fixed_bowtie_hole->getArea(), 92.0, 1e-9);

  auto overlapping_holes = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(2 2, 2 6, 6 6, 6 2, 2 2), "
      "(4 4, 4 8, 8 8, 8 4, 4 4))");
  auto fixed_overlapping = GeometryFixer::fix(overlapping_holes.get());
  EXPECT_EQ(fixed_overlapping->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_overlapping->isValid());
  EXPECT_NEAR(fixed_overlapping->getArea(), 72.0, 1e-9);
  EXPECT_EQ(static_cast<const Polygon*>(fixed_overlapping.get())
                ->getNumInteriorRing(),
            1u);

  auto partial_outside_hole = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(5 2, 5 8, 12 8, 12 2, 5 2))");
  auto fixed_partial = GeometryFixer::fix(partial_outside_hole.get());
  EXPECT_EQ(fixed_partial->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_partial->isValid());
  EXPECT_NEAR(fixed_partial->getArea(), 70.0, 1e-9);

  auto splitting_hole = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(4 0, 4 10, 6 10, 6 0, 4 0))");
  auto fixed_split = GeometryFixer::fix(splitting_hole.get());
  EXPECT_EQ(fixed_split->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(fixed_split->getNumGeometries(), 2u);
  EXPECT_TRUE(fixed_split->isValid());
  EXPECT_NEAR(fixed_split->getArea(), 80.0, 1e-9);

  auto outside_hole = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(20 0, 20 5, 25 5, 25 0, 20 0))");
  auto fixed_outside = GeometryFixer::fix(outside_hole.get());
  EXPECT_EQ(fixed_outside->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(fixed_outside->getNumGeometries(), 2u);
  EXPECT_TRUE(fixed_outside->isValid());
  EXPECT_NEAR(fixed_outside->getArea(), 125.0, 1e-9);

  auto inside_and_outside_holes = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(2 2, 2 4, 4 4, 4 2, 2 2), "
      "(20 0, 20 5, 25 5, 25 0, 20 0))");
  auto fixed_both = GeometryFixer::fix(inside_and_outside_holes.get());
  EXPECT_EQ(fixed_both->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(fixed_both->getNumGeometries(), 2u);
  EXPECT_TRUE(fixed_both->isValid());
  EXPECT_NEAR(fixed_both->getArea(), 121.0, 1e-9);

  auto shell = factory_->createLinearRing(
      MakeXYSeq({{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}}));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing());
  holes.push_back(factory_->createLinearRing(
      MakeXYSeq({{2, 2}, {2, 4}, {4, 4}, {4, 2}, {2, 2}})));
  auto poly_with_empty_hole =
      factory_->createPolygon(std::move(shell), std::move(holes));
  auto fixed_empty_hole = GeometryFixer::fix(poly_with_empty_hole.get());
  EXPECT_EQ(fixed_empty_hole->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_empty_hole->isValid());
  EXPECT_NEAR(fixed_empty_hole->getArea(), 96.0, 1e-9);

  auto enclosing_hole = ReadWkt(
      "POLYGON ((2 2, 2 8, 8 8, 8 2, 2 2), "
      "(0 0, 0 10, 10 10, 10 0, 0 0))");
  auto fixed_enclosing = GeometryFixer::fix(enclosing_hole.get());
  EXPECT_TRUE(fixed_enclosing->isValid());
  EXPECT_TRUE(fixed_enclosing->isEmpty());
  EXPECT_DOUBLE_EQ(fixed_enclosing->getArea(), 0.0);
}

TEST_F(GeometryFixerTest, FixMultiPolygonAllPaths) {
  auto mp_empty_member = ReadWkt("MULTIPOLYGON (EMPTY)");
  auto fixed_empty_member = GeometryFixer::fix(mp_empty_member.get());
  EXPECT_EQ(fixed_empty_member->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(fixed_empty_member->isEmpty());

  auto mp_all_collapsed = ReadWkt("MULTIPOLYGON (((0 0, 1 0, 2 0, 0 0)))");
  auto fixed_all_collapsed = GeometryFixer::fix(mp_all_collapsed.get());
  EXPECT_EQ(fixed_all_collapsed->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(fixed_all_collapsed->isEmpty());

  auto mp_hole_eats_shell = ReadWkt(
      "MULTIPOLYGON ("
      "((2 2, 2 8, 8 8, 8 2, 2 2), (0 0, 0 10, 10 10, 10 0, 0 0)), "
      "((20 0, 20 5, 25 5, 25 0, 20 0)))");
  auto fixed_eaten = GeometryFixer::fix(mp_hole_eats_shell.get());
  EXPECT_EQ(fixed_eaten->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_eaten->isValid());
  EXPECT_NEAR(fixed_eaten->getArea(), 25.0, 1e-9);

  auto mp_overlap = ReadWkt(
      "MULTIPOLYGON ("
      "EMPTY, "
      "((0 0, 1 0, 2 0, 0 0)), "
      "((0 0, 0 10, 10 10, 10 0, 0 0)), "
      "((5 0, 5 10, 15 10, 15 0, 5 0)))");
  auto fixed_overlap = GeometryFixer::fix(mp_overlap.get());
  EXPECT_EQ(fixed_overlap->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_overlap->isValid());
  EXPECT_NEAR(fixed_overlap->getArea(), 150.0, 1e-9);

  auto mp_disjoint_bowtie = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 10, 10 0, 0 10, 0 0)), "
      "((20 0, 20 5, 25 5, 25 0, 20 0)))");
  auto fixed_disjoint = GeometryFixer::fix(mp_disjoint_bowtie.get());
  EXPECT_EQ(fixed_disjoint->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(fixed_disjoint->getNumGeometries(), 3u);
  EXPECT_TRUE(fixed_disjoint->isValid());
  EXPECT_NEAR(fixed_disjoint->getArea(), 75.0, 1e-9);

  auto mp_keep_collapsed = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 1 0, 1 0, 0 0)), "
      "((20 0, 20 5, 25 5, 25 0, 20 0)))");
  GeometryFixer mp_fixer(mp_keep_collapsed.get());
  mp_fixer.setKeepCollapsed(true);
  auto fixed_mp_kept = mp_fixer.getResult();
  EXPECT_TRUE(fixed_mp_kept->isValid());
  EXPECT_EQ(fixed_mp_kept->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(fixed_mp_kept->getNumGeometries(), 2u);
  EXPECT_NEAR(fixed_mp_kept->getArea(), 25.0, 1e-9);
}

TEST_F(GeometryFixerTest, FixGeometryCollectionRecursively) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();

  std::vector<std::unique_ptr<Geometry>> nested_geoms;
  nested_geoms.push_back(ReadWkt("POLYGON ((0 0, 1 0, 2 0, 0 0))"));
  nested_geoms.push_back(ReadWkt("POINT (9 9)"));
  auto nested_col = factory_->createGeometryCollection(std::move(nested_geoms));

  std::vector<std::unique_ptr<Geometry>> top_geoms;
  top_geoms.push_back(factory_->createPoint(CoordinateXY(kNaN, 1.0)));
  top_geoms.push_back(ReadWkt("POINT (2 3)"));
  top_geoms.push_back(factory_->createLineString(
      MakeXYSeq({{0.0, 0.0}, {0.0, 0.0}, {3.0, 4.0}})));
  top_geoms.push_back(ReadWkt("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))"));
  top_geoms.push_back(std::move(nested_col));
  auto col = factory_->createGeometryCollection(std::move(top_geoms));

  auto fixed = GeometryFixer::fix(col.get());
  ASSERT_EQ(fixed->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(fixed->getNumGeometries(), 5u);
  EXPECT_TRUE(fixed->isValid());

  EXPECT_EQ(fixed->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_TRUE(fixed->getGeometryN(0)->isEmpty());

  EXPECT_TRUE(
      fixed->getGeometryN(1)->equalsExact(ReadWkt("POINT (2 3)").get()));

  EXPECT_TRUE(fixed->getGeometryN(2)->equalsExact(
      ReadWkt("LINESTRING (0 0, 3 4)").get()));

  EXPECT_EQ(fixed->getGeometryN(3)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_NEAR(fixed->getGeometryN(3)->getArea(), 50.0, 1e-9);

  const Geometry* fixed_nested = fixed->getGeometryN(4);
  ASSERT_EQ(fixed_nested->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(fixed_nested->getNumGeometries(), 2u);
  EXPECT_EQ(fixed_nested->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fixed_nested->getGeometryN(0)->isEmpty());
  EXPECT_TRUE(
      fixed_nested->getGeometryN(1)->equalsExact(ReadWkt("POINT (9 9)").get()));
  EXPECT_EQ(fixed->getDimension(), Dimension::A);
}

TEST_F(GeometryFixerTest, PreservesCustomGeometryFactoryAndPrecisionModel) {
  PrecisionModel fixed_pm(100.0);
  GeometryFactory::Ptr custom_factory =
      GeometryFactory::create(&fixed_pm, 4326);
  WKTReader custom_reader(*custom_factory);

  auto bowtie = custom_reader.read("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  auto fixed = GeometryFixer::fix(bowtie.get());

  EXPECT_EQ(fixed->getFactory(), custom_factory.get());
  EXPECT_EQ(fixed->getSRID(), 4326);
  EXPECT_FALSE(fixed->getPrecisionModel()->isFloating());
  EXPECT_DOUBLE_EQ(fixed->getPrecisionModel()->getScale(), 100.0);
  EXPECT_TRUE(fixed->isValid());
  EXPECT_NEAR(fixed->getArea(), 50.0, 1e-9);
}

}  // namespace
}  // namespace geos_google
