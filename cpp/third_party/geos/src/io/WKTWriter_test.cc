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

// Unit tests for third_party/geos/src/io/WKTWriter.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests run single-threaded because WKTWriter uses CLocalizer (LC_NUMERIC).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/WKTWriter.cpp

#include "third_party/geos/include/geos/io/WKTWriter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
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
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/io/OrdinateSet.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/Writer.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::DoubleNegInfinity;
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
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::MultiSurface;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::io::OrdinateSet;
using geos::io::WKTReader;
using geos::io::WKTWriter;
using geos::io::Writer;
using geos::util::IllegalArgumentException;

// Exposes protected WKTWriter methods to test protected branches directly.
class TestableWKTWriter : public WKTWriter {
 public:
  void CallAppendSequenceText(const CoordinateSequence& seq,
                              OrdinateSet output_ordinates, int level,
                              bool do_indent, Writer& writer) const {
    appendSequenceText(seq, output_ordinates, level, do_indent, writer);
  }

  void CallAppendCoordinate(const CoordinateXYZM& coord,
                            OrdinateSet output_ordinates,
                            Writer& writer) const {
    appendCoordinate(coord, output_ordinates, writer);
  }

  void CallAppendOrdinateText(OrdinateSet output_ordinates,
                              Writer& writer) const {
    appendOrdinateText(output_ordinates, writer);
  }

  void CallAppendTag(const Geometry& geometry, OrdinateSet output_ordinates,
                     Writer& writer) const {
    appendTag(geometry, output_ordinates, writer);
  }

  std::string CallWriteNumber(double d) const { return writeNumber(d); }
};

std::unique_ptr<Point> MakeXYZMPoint(const GeometryFactory& factory, bool has_z,
                                     bool has_m, double x, double y, double z,
                                     double m) {
  auto seq = std::make_unique<CoordinateSequence>(1u, has_z, has_m, false);
  seq->setAt(CoordinateXYZM(x, y, z, m), 0);
  return factory.createPoint(std::move(seq));
}

std::unique_ptr<LineString> MakeLineStringXY(
    const GeometryFactory& factory, std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<LinearRing> MakeLinearRingXY(
    const GeometryFactory& factory, std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<CircularString> MakeCircularStringXY(
    const GeometryFactory& factory, std::initializer_list<CoordinateXY> pts) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return factory.createCircularString(std::move(seq));
}

TEST(WKTWriterTest, DefaultConfiguration) {
  WKTWriter writer;
  EXPECT_EQ(writer.getOutputDimension(), 4);
}

TEST(WKTWriterTest, SetOutputDimensionValidAndInvalid) {
  WKTWriter writer;
  for (uint8_t dim : {uint8_t{2}, uint8_t{3}, uint8_t{4}}) {
    writer.setOutputDimension(dim);
    EXPECT_EQ(writer.getOutputDimension(), dim);
  }

  for (uint8_t invalid : {uint8_t{0}, uint8_t{1}, uint8_t{5}, uint8_t{255}}) {
    SCOPED_TRACE(static_cast<int>(invalid));
    EXPECT_THROW(writer.setOutputDimension(invalid), IllegalArgumentException);
  }
  EXPECT_EQ(writer.getOutputDimension(), 4);
}

TEST(WKTWriterTest, StaticToPoint) {
  EXPECT_EQ(WKTWriter::toPoint(Coordinate(1.0, 2.0)), "POINT (1 2 )");
  EXPECT_EQ(WKTWriter::toPoint(Coordinate(1.0, 2.0, 3.0)), "POINT (1 2 )");
  EXPECT_EQ(WKTWriter::toPoint(CoordinateXY(3.5, -4.25)), "POINT (3.5 -4.25 )");
}

TEST(WKTWriterTest, StaticToLineStringTwoPoints) {
  EXPECT_EQ(
      WKTWriter::toLineString(CoordinateXY(0.0, 0.0), CoordinateXY(3.5, -4.25)),
      "LINESTRING (0 0, 3.5 -4.25)");
}

TEST(WKTWriterTest, StaticToLineStringSequence) {
  CoordinateSequence empty = CoordinateSequence::XY(0);
  EXPECT_EQ(WKTWriter::toLineString(empty), "LINESTRING EMPTY");

  CoordinateSequence single({CoordinateXY(1.0, 2.0)});
  EXPECT_EQ(WKTWriter::toLineString(single), "LINESTRING (1 2)");

  CoordinateSequence multi({CoordinateXY(1.0, 2.0), CoordinateXY(3.5, 4.5),
                            CoordinateXY(-5.0, 6.25)});
  EXPECT_EQ(WKTWriter::toLineString(multi),
            "LINESTRING (1 2, 3.5 4.5, -5 6.25)");
}

TEST(WKTWriterTest, StaticWriteTrimmedNumberAndWriteNumber) {
  struct TrimmedCase {
    std::string_view name;
    double value;
    uint32_t precision;
    std::string_view expected;
  };

  const auto kCases = std::to_array<TrimmedCase>({
      {.name = "nan",
       .value = DoubleNotANumber,
       .precision = 6,
       .expected = "NaN"},
      {.name = "pos_inf",
       .value = DoubleInfinity,
       .precision = 6,
       .expected = "Infinity"},
      {.name = "neg_inf",
       .value = DoubleNegInfinity,
       .precision = 6,
       .expected = "-Infinity"},
      {.name = "pos_zero", .value = 0.0, .precision = 6, .expected = "0"},
      {.name = "neg_zero", .value = -0.0, .precision = 6, .expected = "0"},
      {.name = "large_threshold",
       .value = 1e17,
       .precision = 4,
       .expected = "1e+17"},
      {.name = "large_negative",
       .value = -1.25e17,
       .precision = 4,
       .expected = "-1.25e+17"},
      {.name = "small_positive",
       .value = 1e-5,
       .precision = 4,
       .expected = "1e-5"},
      {.name = "small_negative",
       .value = -2.5e-5,
       .precision = 4,
       .expected = "-2.5e-5"},
      {.name = "sub_one_low_prec_adjusted",
       .value = 0.005,
       .precision = 1,
       .expected = "0.005"},
      {.name = "sub_one_zero_prec_adjusted",
       .value = 0.005,
       .precision = 0,
       .expected = "0.005"},
      {.name = "sub_one_neg_low_prec_adjusted",
       .value = -0.005,
       .precision = 1,
       .expected = "-0.005"},
      {.name = "sub_one_prec_sufficient",
       .value = 0.5,
       .precision = 2,
       .expected = "0.5"},
      {.name = "sub_one_rounds_at_prec_2",
       .value = 0.12345,
       .precision = 2,
       .expected = "0.12"},
      {.name = "boundary_1e_minus_4",
       .value = 0.0001,
       .precision = 2,
       .expected = "0.0001"},
      {.name = "sub_one_prec_4",
       .value = 0.5,
       .precision = 4,
       .expected = "0.5"},
      {.name = "normal_rounded",
       .value = 123.456789,
       .precision = 3,
       .expected = "123.457"},
      {.name = "normal_integer_trimmed",
       .value = 123.0,
       .precision = 3,
       .expected = "123"},
      {.name = "round_half_to_even_even_pos",
       .value = 2.5,
       .precision = 0,
       .expected = "2"},
      {.name = "round_half_to_even_odd_pos",
       .value = 3.5,
       .precision = 0,
       .expected = "4"},
      {.name = "round_half_to_even_even_neg",
       .value = -2.5,
       .precision = 0,
       .expected = "-2"},
      {.name = "round_half_to_even_odd_neg",
       .value = -3.5,
       .precision = 0,
       .expected = "-4"},
      {.name = "boundary_neg_1e_minus_4",
       .value = -0.0001,
       .precision = 2,
       .expected = "-0.0001"},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    char buf[28] = {};
    const int len = WKTWriter::writeTrimmedNumber(tc.value, tc.precision, buf);
    ASSERT_GT(len, 0);
    ASSERT_LT(static_cast<std::size_t>(len), sizeof(buf));
    buf[len] = '\0';
    EXPECT_EQ(std::string_view(buf), tc.expected);
    EXPECT_EQ(WKTWriter::writeNumber(tc.value, /*trim=*/true, tc.precision),
              tc.expected);
  }

  EXPECT_EQ(WKTWriter::writeNumber(1.25, /*trim=*/false, 4), "1.2500");
  EXPECT_EQ(WKTWriter::writeNumber(1.2345, /*trim=*/false, 2), "1.23");
  EXPECT_EQ(WKTWriter::writeNumber(42.0, /*trim=*/false, 0), "42");
  EXPECT_EQ(WKTWriter::writeNumber(0.005, /*trim=*/false, 1), "0.0");
}

TEST(WKTWriterTest, WriteOverloadsAndWriterTarget) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.5, 2.25));

  WKTWriter writer;
  EXPECT_EQ(writer.write(pt.get()), "POINT (1.5 2.25)");
  EXPECT_EQ(writer.write(*pt), "POINT (1.5 2.25)");

  Writer raw_writer;
  writer.write(pt.get(), &raw_writer);
  EXPECT_EQ(raw_writer.toString(), "POINT (1.5 2.25)");

  std::unique_ptr<Polygon> poly = factory->createPolygon(
      MakeLinearRingXY(*factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(
      MakeLinearRingXY(*factory, {{1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}}));
  std::unique_ptr<Polygon> poly_with_hole = factory->createPolygon(
      MakeLinearRingXY(*factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}),
      std::move(holes));

  const std::string expected_formatted =
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), \n"
      "  (1 1, 1 2, 2 2, 2 1, 1 1))";
  EXPECT_EQ(writer.writeFormatted(poly_with_hole.get()), expected_formatted);

  Writer formatted_writer;
  writer.writeFormatted(poly_with_hole.get(), &formatted_writer);
  EXPECT_EQ(formatted_writer.toString(), expected_formatted);
  EXPECT_EQ(writer.write(poly.get()), "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
}

TEST(WKTWriterTest, RoundingPrecisionAndTrimWithPrecisionModels) {
  // Default FLOATING precision model has 16 significant digits.
  {
    GeometryFactory::Ptr factory = GeometryFactory::create();
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXY(1.123456789012345, 2.5));
    WKTWriter writer;
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.123456789012345 2.5)");

    writer.setRoundingPrecision(2);
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.12 2.5)");

    writer.setTrim(false);
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.12 2.50)");

    writer.setTrim(true);
    // Values below -1 clamp to -1 (PrecisionModel controlled).
    writer.setRoundingPrecision(-42);
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.123456789012345 2.5)");

    writer.setRoundingPrecision(2);
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.12 2.5)");
    writer.setRoundingPrecision(-1);
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.123456789012345 2.5)");

    // Ryu uses round-half-to-even (ties to even), so 2.5 rounds to 2.
    writer.setRoundingPrecision(0);
    EXPECT_EQ(writer.write(pt.get()), "POINT (1 2)");

    std::unique_ptr<Point> odd_tie_pt =
        factory->createPoint(CoordinateXY(1.7, 3.5));
    EXPECT_EQ(writer.write(odd_tie_pt.get()), "POINT (2 4)");

    std::unique_ptr<Point> neg_tie_pt =
        factory->createPoint(CoordinateXY(-2.5, -3.5));
    EXPECT_EQ(writer.write(neg_tie_pt.get()), "POINT (-2 -4)");

    writer.setTrim(false);
    std::unique_ptr<Point> fixed_zero_pt =
        factory->createPoint(CoordinateXY(1.25, 2.75));
    EXPECT_EQ(writer.write(fixed_zero_pt.get()), "POINT (1 3)");
  }

  // FLOATING_SINGLE uses 6 significant digits.
  {
    PrecisionModel single_pm(PrecisionModel::FLOATING_SINGLE);
    GeometryFactory::Ptr factory = GeometryFactory::create(&single_pm);
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXY(1.123456789, 2.5));
    WKTWriter writer;
    EXPECT_EQ(writer.write(pt.get()), "POINT (1.123457 2.5)");
  }

  // FIXED(1.0) uses 0 decimal places.
  {
    PrecisionModel fixed_unit_pm(PrecisionModel::FIXED);
    GeometryFactory::Ptr factory = GeometryFactory::create(&fixed_unit_pm);
    std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(12.7, 3.2));
    WKTWriter writer;
    EXPECT_EQ(writer.write(pt.get()), "POINT (13 3)");
  }

  // FIXED(0.01) yields negative getMaximumSignificantDigits() (-2), which must
  // clamp decimalPlaces to 0 inside writeNumber(double).
  {
    PrecisionModel coarse_pm(0.01);
    ASSERT_LT(coarse_pm.getMaximumSignificantDigits(), 0);
    GeometryFactory::Ptr factory = GeometryFactory::create(&coarse_pm);
    std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(12.7, 3.2));
    WKTWriter writer;
    EXPECT_EQ(writer.write(pt.get()), "POINT (13 3)");
  }

  // FIXED(1000.0) rounds to the PrecisionModel's maximum significant digits.
  {
    PrecisionModel fine_pm(1000.0);
    const int sig_digits = fine_pm.getMaximumSignificantDigits();
    ASSERT_GT(sig_digits, 0);
    GeometryFactory::Ptr factory = GeometryFactory::create(&fine_pm);
    std::unique_ptr<Point> pt =
        factory->createPoint(CoordinateXY(1.2345678, 9.8765432));
    WKTWriter writer;
    const std::string expected =
        "POINT (" +
        WKTWriter::writeNumber(1.2345678, true,
                               static_cast<uint32_t>(sig_digits)) +
        " " +
        WKTWriter::writeNumber(9.8765432, true,
                               static_cast<uint32_t>(sig_digits)) +
        ")";
    EXPECT_EQ(writer.write(pt.get()), expected);
  }
}

TEST(WKTWriterTest, AllGeometryTypesEmptyAndNonEmptyUnformatted) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTWriter writer;

  // Empty geometries of all 13 types.
  struct EmptyCase {
    GeometryTypeId type_id;
    std::string_view expected;
  };
  constexpr auto kEmptyCases = std::to_array<EmptyCase>({
      {.type_id = GeometryTypeId::GEOS_POINT, .expected = "POINT EMPTY"},
      {.type_id = GeometryTypeId::GEOS_LINESTRING,
       .expected = "LINESTRING EMPTY"},
      {.type_id = GeometryTypeId::GEOS_LINEARRING,
       .expected = "LINEARRING EMPTY"},
      {.type_id = GeometryTypeId::GEOS_CIRCULARSTRING,
       .expected = "CIRCULARSTRING EMPTY"},
      {.type_id = GeometryTypeId::GEOS_COMPOUNDCURVE,
       .expected = "COMPOUNDCURVE EMPTY"},
      {.type_id = GeometryTypeId::GEOS_POLYGON, .expected = "POLYGON EMPTY"},
      {.type_id = GeometryTypeId::GEOS_CURVEPOLYGON,
       .expected = "CURVEPOLYGON EMPTY"},
      {.type_id = GeometryTypeId::GEOS_MULTIPOINT,
       .expected = "MULTIPOINT EMPTY"},
      {.type_id = GeometryTypeId::GEOS_MULTILINESTRING,
       .expected = "MULTILINESTRING EMPTY"},
      {.type_id = GeometryTypeId::GEOS_MULTICURVE,
       .expected = "MULTICURVE EMPTY"},
      {.type_id = GeometryTypeId::GEOS_MULTIPOLYGON,
       .expected = "MULTIPOLYGON EMPTY"},
      {.type_id = GeometryTypeId::GEOS_MULTISURFACE,
       .expected = "MULTISURFACE EMPTY"},
      {.type_id = GeometryTypeId::GEOS_GEOMETRYCOLLECTION,
       .expected = "GEOMETRYCOLLECTION EMPTY"},
  });
  for (const auto& tc : kEmptyCases) {
    std::unique_ptr<Geometry> empty = factory->createEmptyGeometry(tc.type_id);
    EXPECT_EQ(writer.write(empty.get()), tc.expected);
  }

  // Non-empty Point (including non-finite ordinates).
  EXPECT_EQ(writer.write(factory->createPoint(CoordinateXY(1, 2)).get()),
            "POINT (1 2)");
  EXPECT_EQ(
      writer.write(
          factory->createPoint(CoordinateXY(DoubleInfinity, DoubleNegInfinity))
              .get()),
      "POINT (Infinity -Infinity)");
  EXPECT_EQ(
      writer.write(
          factory->createPoint(CoordinateXY(DoubleNotANumber, 1.0)).get()),
      "POINT (NaN 1)");

  // Non-empty LineString, LinearRing, CircularString.
  EXPECT_EQ(
      writer.write(MakeLineStringXY(*factory, {{0, 0}, {1, 1}, {2, 0}}).get()),
      "LINESTRING (0 0, 1 1, 2 0)");
  EXPECT_EQ(writer.write(MakeLinearRingXY(
                             *factory, {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}})
                             .get()),
            "LINEARRING (0 0, 1 0, 1 1, 0 1, 0 0)");
  EXPECT_EQ(writer.write(
                MakeCircularStringXY(*factory, {{0, 0}, {1, 1}, {2, 0}}).get()),
            "CIRCULARSTRING (0 0, 1 1, 2 0)");

  // Non-empty CompoundCurve (single and multi-segment).
  {
    std::vector<std::unique_ptr<SimpleCurve>> segs;
    segs.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    segs.push_back(MakeCircularStringXY(*factory, {{1, 1}, {2, 2}, {3, 1}}));
    segs.push_back(MakeLineStringXY(*factory, {{3, 1}, {4, 0}}));
    std::unique_ptr<CompoundCurve> cc =
        factory->createCompoundCurve(std::move(segs));
    EXPECT_EQ(writer.write(cc.get()),
              "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1), (3 "
              "1, 4 0))");
  }

  // Non-empty Polygon with multiple holes.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(
        MakeLinearRingXY(*factory, {{1, 1}, {1, 3}, {3, 3}, {3, 1}, {1, 1}}));
    holes.push_back(
        MakeLinearRingXY(*factory, {{5, 5}, {5, 7}, {7, 7}, {7, 5}, {5, 5}}));
    std::unique_ptr<Polygon> poly = factory->createPolygon(
        MakeLinearRingXY(*factory,
                         {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}),
        std::move(holes));
    EXPECT_EQ(writer.write(poly.get()),
              "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(1 1, 1 3, 3 3, 3 1, 1 1), "
              "(5 5, 5 7, 7 7, 7 5, 5 5))");
  }

  // Non-empty CurvePolygon with CompoundCurve shell + CircularString,
  // LinearRing, and CompoundCurve holes.
  {
    std::vector<std::unique_ptr<SimpleCurve>> shell_segs;
    shell_segs.push_back(MakeLineStringXY(*factory, {{0, 0}, {10, 0}}));
    shell_segs.push_back(
        MakeCircularStringXY(*factory, {{10, 0}, {15, 5}, {10, 10}}));
    shell_segs.push_back(
        MakeLineStringXY(*factory, {{10, 10}, {0, 10}, {0, 0}}));
    std::unique_ptr<Curve> shell =
        factory->createCompoundCurve(std::move(shell_segs));

    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(MakeCircularStringXY(
        *factory, {{2, 2}, {3, 3}, {4, 2}, {3, 1}, {2, 2}}));
    holes.push_back(
        MakeLinearRingXY(*factory, {{5, 5}, {5, 6}, {6, 6}, {6, 5}, {5, 5}}));

    std::vector<std::unique_ptr<SimpleCurve>> hole_cc_segs;
    hole_cc_segs.push_back(MakeLineStringXY(*factory, {{7, 2}, {8, 2}}));
    hole_cc_segs.push_back(
        MakeCircularStringXY(*factory, {{8, 2}, {9, 3}, {8, 4}}));
    hole_cc_segs.push_back(
        MakeLineStringXY(*factory, {{8, 4}, {7, 4}, {7, 2}}));
    holes.push_back(factory->createCompoundCurve(std::move(hole_cc_segs)));

    std::unique_ptr<CurvePolygon> cp =
        factory->createCurvePolygon(std::move(shell), std::move(holes));
    EXPECT_EQ(writer.write(cp.get()),
              "CURVEPOLYGON (COMPOUNDCURVE ((0 0, 10 0), "
              "CIRCULARSTRING (10 0, 15 5, 10 10), (10 10, 0 10, 0 0)), "
              "CIRCULARSTRING (2 2, 3 3, 4 2, 3 1, 2 2), "
              "(5 5, 5 6, 6 6, 6 5, 5 5), "
              "COMPOUNDCURVE ((7 2, 8 2), CIRCULARSTRING (8 2, 9 3, 8 4), "
              "(8 4, 7 4, 7 2)))");
  }

  // Non-empty MultiPoint including an empty Point member.
  {
    std::vector<std::unique_ptr<Point>> pts;
    pts.push_back(factory->createPoint());
    pts.push_back(factory->createPoint(CoordinateXY(1, 2)));
    pts.push_back(factory->createPoint(CoordinateXY(3, 4)));
    std::unique_ptr<MultiPoint> mp = factory->createMultiPoint(std::move(pts));
    EXPECT_EQ(writer.write(mp.get()), "MULTIPOINT (EMPTY, (1 2), (3 4))");
  }

  // Non-empty MultiLineString including an empty LineString member.
  {
    std::vector<std::unique_ptr<LineString>> lines;
    lines.push_back(factory->createLineString());
    lines.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(lines));
    EXPECT_EQ(writer.write(mls.get()), "MULTILINESTRING (EMPTY, (0 0, 1 1))");
  }

  // Non-empty MultiCurve containing LineString, CircularString, CompoundCurve.
  {
    std::vector<std::unique_ptr<Curve>> curves;
    curves.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    curves.push_back(MakeCircularStringXY(*factory, {{1, 1}, {2, 2}, {3, 1}}));
    std::vector<std::unique_ptr<SimpleCurve>> segs;
    segs.push_back(MakeLineStringXY(*factory, {{3, 1}, {4, 2}}));
    segs.push_back(MakeCircularStringXY(*factory, {{4, 2}, {5, 3}, {6, 2}}));
    curves.push_back(factory->createCompoundCurve(std::move(segs)));
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(curves));
    EXPECT_EQ(writer.write(mc.get()),
              "MULTICURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1), "
              "COMPOUNDCURVE ((3 1, 4 2), CIRCULARSTRING (4 2, 5 3, 6 2)))");
  }

  // Non-empty MultiCurve containing empty LineString, CircularString, and
  // CompoundCurve members.
  {
    std::vector<std::unique_ptr<Curve>> curves;
    curves.push_back(factory->createLineString());
    curves.push_back(factory->createCircularString(false, false));
    curves.push_back(factory->createCompoundCurve());
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(curves));
    EXPECT_EQ(writer.write(mc.get()),
              "MULTICURVE (EMPTY, CIRCULARSTRING EMPTY, COMPOUNDCURVE EMPTY)");
  }

  // Non-empty MultiPolygon including an empty Polygon member.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(factory->createPolygon());
    polys.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}})));
    std::unique_ptr<MultiPolygon> mpoly =
        factory->createMultiPolygon(std::move(polys));
    EXPECT_EQ(writer.write(mpoly.get()),
              "MULTIPOLYGON (EMPTY, ((0 0, 2 0, 2 2, 0 2, 0 0)))");
  }

  // Non-empty MultiSurface containing Polygon, CurvePolygon, and Polygon.
  {
    std::vector<std::unique_ptr<Surface>> surfaces;
    surfaces.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}})));
    surfaces.push_back(factory->createCurvePolygon(MakeCircularStringXY(
        *factory, {{3, 0}, {4, 1}, {5, 0}, {4, -1}, {3, 0}})));
    surfaces.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{6, 6}, {8, 6}, {8, 8}, {6, 8}, {6, 6}})));
    std::unique_ptr<MultiSurface> ms =
        factory->createMultiSurface(std::move(surfaces));
    EXPECT_EQ(writer.write(ms.get()),
              "MULTISURFACE (((0 0, 2 0, 2 2, 0 2, 0 0)), "
              "CURVEPOLYGON (CIRCULARSTRING (3 0, 4 1, 5 0, 4 -1, 3 0)), "
              "((6 6, 8 6, 8 8, 6 8, 6 6)))");
  }

  // MultiSurface containing empty Polygon and empty CurvePolygon members.
  {
    std::vector<std::unique_ptr<Surface>> surfaces;
    surfaces.push_back(factory->createPolygon());
    surfaces.push_back(factory->createCurvePolygon(false, false));
    std::unique_ptr<MultiSurface> ms =
        factory->createMultiSurface(std::move(surfaces));
    EXPECT_EQ(writer.write(ms.get()),
              "MULTISURFACE (EMPTY, CURVEPOLYGON EMPTY)");
  }

  // Non-empty nested GeometryCollection.
  {
    std::vector<std::unique_ptr<Geometry>> inner;
    inner.push_back(factory->createPoint(CoordinateXY(3, 4)));
    inner.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}})));

    std::vector<std::unique_ptr<Geometry>> outer;
    outer.push_back(factory->createPoint(CoordinateXY(1, 2)));
    outer.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    outer.push_back(factory->createGeometryCollection(std::move(inner)));
    std::unique_ptr<GeometryCollection> gc =
        factory->createGeometryCollection(std::move(outer));
    EXPECT_EQ(writer.write(gc.get()),
              "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 1 1), "
              "GEOMETRYCOLLECTION (POINT (3 4), POLYGON ((0 0, 1 0, 1 1, 0 1, "
              "0 0))))");
  }
}

TEST(WKTWriterTest, FormattedOutputAndIndentation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTWriter writer;

  // Polygon with two holes.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(
        MakeLinearRingXY(*factory, {{1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}}));
    holes.push_back(
        MakeLinearRingXY(*factory, {{5, 5}, {5, 6}, {6, 6}, {6, 5}, {5, 5}}));
    auto poly = factory->createPolygon(
        MakeLinearRingXY(*factory,
                         {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}),
        std::move(holes));
    EXPECT_EQ(writer.writeFormatted(poly.get()),
              "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), \n"
              "  (1 1, 1 2, 2 2, 2 1, 1 1), \n"
              "  (5 5, 5 6, 6 6, 6 5, 5 5))");
  }

  // CompoundCurve with LineString + CircularString + LineString.
  {
    std::vector<std::unique_ptr<SimpleCurve>> segs;
    segs.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    segs.push_back(MakeCircularStringXY(*factory, {{1, 1}, {2, 2}, {3, 1}}));
    segs.push_back(MakeLineStringXY(*factory, {{3, 1}, {4, 0}}));
    auto cc = factory->createCompoundCurve(std::move(segs));
    EXPECT_EQ(writer.writeFormatted(cc.get()),
              "COMPOUNDCURVE ((0 0, 1 1), \n"
              "  CIRCULARSTRING (1 1, 2 2, 3 1), \n"
              "  (3 1, 4 0))");
  }

  // CurvePolygon with CompoundCurve shell and CompoundCurve + CircularString
  // holes (exercises nested level 1 and level 2 indentation).
  {
    std::vector<std::unique_ptr<SimpleCurve>> shell_segs;
    shell_segs.push_back(MakeLineStringXY(*factory, {{0, 0}, {10, 0}}));
    shell_segs.push_back(
        MakeCircularStringXY(*factory, {{10, 0}, {15, 5}, {10, 10}}));
    shell_segs.push_back(
        MakeLineStringXY(*factory, {{10, 10}, {0, 10}, {0, 0}}));
    std::unique_ptr<Curve> shell =
        factory->createCompoundCurve(std::move(shell_segs));

    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(MakeCircularStringXY(
        *factory, {{2, 2}, {3, 3}, {4, 2}, {3, 1}, {2, 2}}));
    std::vector<std::unique_ptr<SimpleCurve>> hole_cc_segs;
    hole_cc_segs.push_back(MakeLineStringXY(*factory, {{7, 2}, {8, 2}}));
    hole_cc_segs.push_back(
        MakeCircularStringXY(*factory, {{8, 2}, {9, 3}, {8, 4}}));
    hole_cc_segs.push_back(
        MakeLineStringXY(*factory, {{8, 4}, {7, 4}, {7, 2}}));
    holes.push_back(factory->createCompoundCurve(std::move(hole_cc_segs)));

    auto cp = factory->createCurvePolygon(std::move(shell), std::move(holes));
    EXPECT_EQ(writer.writeFormatted(cp.get()),
              "CURVEPOLYGON (COMPOUNDCURVE ((0 0, 10 0), \n"
              "  CIRCULARSTRING (10 0, 15 5, 10 10), \n"
              "  (10 10, 0 10, 0 0)), \n"
              "  CIRCULARSTRING (2 2, 3 3, 4 2, 3 1, 2 2), \n"
              "  COMPOUNDCURVE ((7 2, 8 2), \n"
              "    CIRCULARSTRING (8 2, 9 3, 8 4), \n"
              "    (8 4, 7 4, 7 2)))");
  }

  // MultiCurve with LineString, CircularString, CompoundCurve.
  {
    std::vector<std::unique_ptr<Curve>> curves;
    curves.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    curves.push_back(MakeCircularStringXY(*factory, {{1, 1}, {2, 2}, {3, 1}}));
    std::vector<std::unique_ptr<SimpleCurve>> segs;
    segs.push_back(MakeLineStringXY(*factory, {{3, 1}, {4, 2}}));
    segs.push_back(MakeCircularStringXY(*factory, {{4, 2}, {5, 3}, {6, 2}}));
    curves.push_back(factory->createCompoundCurve(std::move(segs)));
    auto mc = factory->createMultiCurve(std::move(curves));
    EXPECT_EQ(writer.writeFormatted(mc.get()),
              "MULTICURVE ((0 0, 1 1), \n"
              "  CIRCULARSTRING (1 1, 2 2, 3 1), \n"
              "  COMPOUNDCURVE ((3 1, 4 2), \n"
              "    CIRCULARSTRING (4 2, 5 3, 6 2)))");
  }

  // MultiPolygon with hole in first polygon.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(
        MakeLinearRingXY(*factory, {{1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}}));
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}),
        std::move(holes)));
    polys.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{5, 5}, {7, 5}, {7, 7}, {5, 7}, {5, 5}})));
    auto mp = factory->createMultiPolygon(std::move(polys));
    EXPECT_EQ(writer.writeFormatted(mp.get()),
              "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0), \n"
              "  (1 1, 1 2, 2 2, 2 1, 1 1)), \n"
              "  ((5 5, 7 5, 7 7, 5 7, 5 5)))");
  }

  // MultiSurface with Polygon, CurvePolygon, Polygon.
  {
    std::vector<std::unique_ptr<Surface>> surfaces;
    surfaces.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}})));
    surfaces.push_back(factory->createCurvePolygon(MakeCircularStringXY(
        *factory, {{3, 0}, {4, 1}, {5, 0}, {4, -1}, {3, 0}})));
    surfaces.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{6, 6}, {8, 6}, {8, 8}, {6, 8}, {6, 6}})));
    auto ms = factory->createMultiSurface(std::move(surfaces));
    EXPECT_EQ(writer.writeFormatted(ms.get()),
              "MULTISURFACE (((0 0, 2 0, 2 2, 0 2, 0 0)), "
              "CURVEPOLYGON (CIRCULARSTRING (3 0, 4 1, 5 0, 4 -1, 3 0)), \n"
              "  ((6 6, 8 6, 8 8, 6 8, 6 6)))");
  }

  // MultiCurve, MultiPolygon, and MultiSurface with empty members formatted.
  {
    std::vector<std::unique_ptr<Curve>> curves;
    curves.push_back(factory->createLineString());
    curves.push_back(factory->createCircularString(false, false));
    curves.push_back(factory->createCompoundCurve());
    auto mc = factory->createMultiCurve(std::move(curves));
    EXPECT_EQ(writer.writeFormatted(mc.get()),
              "MULTICURVE (EMPTY, \n"
              "  CIRCULARSTRING EMPTY, \n"
              "  COMPOUNDCURVE EMPTY)");

    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}})));
    polys.push_back(factory->createPolygon());
    auto mpoly = factory->createMultiPolygon(std::move(polys));
    EXPECT_EQ(writer.writeFormatted(mpoly.get()),
              "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), EMPTY)");

    std::vector<std::unique_ptr<Surface>> surfaces;
    surfaces.push_back(factory->createCurvePolygon(false, false));
    surfaces.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}})));
    auto ms = factory->createMultiSurface(std::move(surfaces));
    EXPECT_EQ(writer.writeFormatted(ms.get()),
              "MULTISURFACE (CURVEPOLYGON EMPTY, \n"
              "  ((0 0, 2 0, 2 2, 0 2, 0 0)))");
  }

  // Nested GeometryCollection reaching indent levels 0, 1, and 2.
  {
    std::vector<std::unique_ptr<Geometry>> inner;
    inner.push_back(factory->createPoint(CoordinateXY(3, 4)));
    inner.push_back(factory->createPolygon(
        MakeLinearRingXY(*factory, {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}})));

    std::vector<std::unique_ptr<Geometry>> outer;
    outer.push_back(factory->createPoint(CoordinateXY(1, 2)));
    outer.push_back(MakeLineStringXY(*factory, {{0, 0}, {1, 1}}));
    outer.push_back(factory->createGeometryCollection(std::move(inner)));
    auto gc = factory->createGeometryCollection(std::move(outer));
    EXPECT_EQ(writer.writeFormatted(gc.get()),
              "GEOMETRYCOLLECTION (POINT (1 2), \n"
              "  LINESTRING (0 0, 1 1), \n"
              "  GEOMETRYCOLLECTION (\n"
              "  POINT (3 4), \n"
              "    POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))))");
  }
}

TEST(WKTWriterTest, FormattedLongSequenceWrapsEveryTenCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTWriter writer;

  auto coords = std::make_unique<CoordinateSequence>(0u, false, false);
  for (int i = 0; i < 12; ++i) {
    coords->add(static_cast<double>(i), static_cast<double>(i));
  }
  auto ls = factory->createLineString(std::move(coords));

  EXPECT_EQ(writer.write(ls.get()),
            "LINESTRING (0 0, 1 1, 2 2, 3 3, 4 4, 5 5, 6 6, 7 7, 8 8, 9 9, "
            "10 10, 11 11)");
  EXPECT_EQ(writer.writeFormatted(ls.get()),
            "LINESTRING (0 0, 1 1, 2 2, 3 3, 4 4, 5 5, 6 6, 7 7, 8 8, 9 9, \n"
            "    10 10, 11 11)");

  // Same sequence as second child of a GeometryCollection (level = 1 ->
  // coordinate wrap indents at level + 2 = 3, i.e., 6 spaces).
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(CoordinateXY(0, 0)));
  geoms.push_back(std::move(ls));
  auto gc = factory->createGeometryCollection(std::move(geoms));
  EXPECT_EQ(writer.writeFormatted(gc.get()),
            "GEOMETRYCOLLECTION (POINT (0 0), \n"
            "  LINESTRING (0 0, 1 1, 2 2, 3 3, 4 4, 5 5, 6 6, 7 7, 8 8, 9 9, \n"
            "      10 10, 11 11))");

  // Exact boundary: 10 coordinates fit on one line without wrapping.
  auto coords10 = std::make_unique<CoordinateSequence>(0u, false, false);
  for (int i = 0; i < 10; ++i) {
    coords10->add(static_cast<double>(i), static_cast<double>(i));
  }
  auto ls10 = factory->createLineString(std::move(coords10));
  EXPECT_EQ(writer.writeFormatted(ls10.get()),
            "LINESTRING (0 0, 1 1, 2 2, 3 3, 4 4, 5 5, 6 6, 7 7, 8 8, 9 9)");

  // 21 coordinates wrap at both index 10 and index 20.
  auto coords21 = std::make_unique<CoordinateSequence>(0u, false, false);
  for (int i = 0; i < 21; ++i) {
    coords21->add(static_cast<double>(i), static_cast<double>(i));
  }
  auto ls21 = factory->createLineString(std::move(coords21));
  EXPECT_EQ(writer.writeFormatted(ls21.get()),
            "LINESTRING (0 0, 1 1, 2 2, 3 3, 4 4, 5 5, 6 6, 7 7, 8 8, 9 9, \n"
            "    10 10, 11 11, 12 12, 13 13, 14 14, 15 15, 16 16, 17 17, "
            "18 18, 19 19, \n"
            "    20 20)");
}

TEST(WKTWriterTest, EmptyGeometriesPreserveDeclaredDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  struct EmptyDimCase {
    GeometryTypeId type_id;
    std::string_view tag;
  };
  constexpr auto kTypedEmpty = std::to_array<EmptyDimCase>({
      {.type_id = GeometryTypeId::GEOS_POINT, .tag = "POINT"},
      {.type_id = GeometryTypeId::GEOS_LINESTRING, .tag = "LINESTRING"},
      {.type_id = GeometryTypeId::GEOS_LINEARRING, .tag = "LINEARRING"},
      {.type_id = GeometryTypeId::GEOS_CIRCULARSTRING, .tag = "CIRCULARSTRING"},
      {.type_id = GeometryTypeId::GEOS_POLYGON, .tag = "POLYGON"},
      {.type_id = GeometryTypeId::GEOS_CURVEPOLYGON, .tag = "CURVEPOLYGON"},
  });

  struct DimMode {
    bool has_z;
    bool has_m;
    std::string_view suffix;
  };
  constexpr auto kModes = std::to_array<DimMode>({
      {.has_z = true, .has_m = false, .suffix = "Z"},
      {.has_z = false, .has_m = true, .suffix = "M"},
      {.has_z = true, .has_m = true, .suffix = "ZM"},
  });

  WKTWriter default_writer;
  WKTWriter strip_writer;
  strip_writer.setRemoveEmptyDimensions(true);
  WKTWriter dim3_writer;
  dim3_writer.setOutputDimension(3);
  WKTWriter dim2_writer;
  dim2_writer.setOutputDimension(2);
  WKTWriter old3d_writer;
  old3d_writer.setOld3D(true);

  for (const auto& geom_case : kTypedEmpty) {
    for (const auto& mode : kModes) {
      std::unique_ptr<Geometry> empty = factory->createEmptyGeometry(
          geom_case.type_id, mode.has_z, mode.has_m);
      const std::string tag(geom_case.tag);
      const std::string suffix(mode.suffix);
      const std::string full_expected = tag + " " + suffix + " EMPTY";
      EXPECT_EQ(default_writer.write(empty.get()), full_expected);
      // Empty geometries retain their declared dimensionality even with
      // removeEmptyDimensions = true.
      EXPECT_EQ(strip_writer.write(empty.get()), full_expected);

      const std::string dim3_expected =
          (mode.has_z && mode.has_m) ? (tag + " Z EMPTY") : full_expected;
      EXPECT_EQ(dim3_writer.write(empty.get()), dim3_expected);
      EXPECT_EQ(dim2_writer.write(empty.get()), tag + " EMPTY");

      const std::string old3d_expected =
          (!mode.has_z && mode.has_m) ? (tag + " M EMPTY") : (tag + " EMPTY");
      EXPECT_EQ(old3d_writer.write(empty.get()), old3d_expected);
    }
  }
}

TEST(WKTWriterTest, NonEmptyDimensionsOutputDimensionAndOld3D) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTWriter writer;
  WKTWriter dim3_writer;
  dim3_writer.setOutputDimension(3);
  WKTWriter dim2_writer;
  dim2_writer.setOutputDimension(2);
  WKTWriter old3d_writer;
  old3d_writer.setOld3D(true);

  // XY Point.
  auto pt_xy = factory->createPoint(CoordinateXY(1, 2));
  EXPECT_EQ(old3d_writer.write(pt_xy.get()), "POINT (1 2)");

  // XYZ Point.
  auto pt_z = factory->createPoint(Coordinate(1, 2, 3));
  EXPECT_EQ(writer.write(pt_z.get()), "POINT Z (1 2 3)");
  EXPECT_EQ(dim3_writer.write(pt_z.get()), "POINT Z (1 2 3)");
  EXPECT_EQ(dim2_writer.write(pt_z.get()), "POINT (1 2)");
  EXPECT_EQ(old3d_writer.write(pt_z.get()), "POINT (1 2 3)");

  // XYM Point.
  auto pt_m = factory->createPoint(CoordinateXYM(1, 2, 4));
  EXPECT_EQ(writer.write(pt_m.get()), "POINT M (1 2 4)");
  EXPECT_EQ(dim3_writer.write(pt_m.get()), "POINT M (1 2 4)");
  EXPECT_EQ(dim2_writer.write(pt_m.get()), "POINT (1 2)");
  EXPECT_EQ(old3d_writer.write(pt_m.get()), "POINT M (1 2 4)");

  // XYZM Point.
  auto pt_zm = factory->createPoint(CoordinateXYZM(1, 2, 3, 4));
  EXPECT_EQ(writer.write(pt_zm.get()), "POINT ZM (1 2 3 4)");
  EXPECT_EQ(dim3_writer.write(pt_zm.get()), "POINT Z (1 2 3)");
  EXPECT_EQ(dim2_writer.write(pt_zm.get()), "POINT (1 2)");
  EXPECT_EQ(old3d_writer.write(pt_zm.get()), "POINT (1 2 3 4)");

  // XYM LineString, CircularString, MultiPoint, Polygon, GeometryCollection.
  auto ls_m = factory->createLineString(
      CoordinateSequence({CoordinateXYM(0, 0, 10), CoordinateXYM(1, 1, 20)}));
  EXPECT_EQ(writer.write(ls_m.get()), "LINESTRING M (0 0 10, 1 1 20)");
  EXPECT_EQ(old3d_writer.write(ls_m.get()), "LINESTRING M (0 0 10, 1 1 20)");

  auto cs_z = factory->createCircularString(CoordinateSequence(
      {Coordinate(0, 0, 1), Coordinate(1, 1, 2), Coordinate(2, 0, 3)}));
  EXPECT_EQ(writer.write(cs_z.get()), "CIRCULARSTRING Z (0 0 1, 1 1 2, 2 0 3)");
  EXPECT_EQ(old3d_writer.write(cs_z.get()),
            "CIRCULARSTRING (0 0 1, 1 1 2, 2 0 3)");

  auto mp_zm = factory->createMultiPoint(CoordinateSequence(
      {CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(5, 6, 7, 8)}));
  EXPECT_EQ(writer.write(mp_zm.get()), "MULTIPOINT ZM ((1 2 3 4), (5 6 7 8))");
  EXPECT_EQ(dim3_writer.write(mp_zm.get()), "MULTIPOINT Z ((1 2 3), (5 6 7))");
  EXPECT_EQ(dim2_writer.write(mp_zm.get()), "MULTIPOINT ((1 2), (5 6))");
  EXPECT_EQ(old3d_writer.write(mp_zm.get()),
            "MULTIPOINT ((1 2 3 4), (5 6 7 8))");

  auto ring_zm = factory->createLinearRing(CoordinateSequence(
      {CoordinateXYZM(0, 0, 1, 10), CoordinateXYZM(2, 0, 2, 20),
       CoordinateXYZM(2, 2, 3, 30), CoordinateXYZM(0, 2, 4, 40),
       CoordinateXYZM(0, 0, 1, 10)}));
  auto poly_zm = factory->createPolygon(std::move(ring_zm));
  EXPECT_EQ(writer.write(poly_zm.get()),
            "POLYGON ZM ((0 0 1 10, 2 0 2 20, 2 2 3 30, 0 2 4 40, 0 0 1 10))");
  EXPECT_EQ(dim2_writer.write(poly_zm.get()),
            "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
}

TEST(WKTWriterTest, RemoveEmptyDimensionsStripsAllNaNOrdinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTWriter keep_writer;
  WKTWriter strip_writer;
  strip_writer.setRemoveEmptyDimensions(true);

  // XYZ Point with NaN Z.
  auto pt_z_nan = MakeXYZMPoint(*factory, true, false, 1, 2, DoubleNotANumber,
                                DoubleNotANumber);
  EXPECT_EQ(keep_writer.write(pt_z_nan.get()), "POINT Z (1 2 NaN)");
  EXPECT_EQ(strip_writer.write(pt_z_nan.get()), "POINT (1 2)");

  // XYZM Point with valid Z and NaN M.
  auto pt_z_only =
      MakeXYZMPoint(*factory, true, true, 1, 2, 3, DoubleNotANumber);
  EXPECT_EQ(keep_writer.write(pt_z_only.get()), "POINT ZM (1 2 3 NaN)");
  EXPECT_EQ(strip_writer.write(pt_z_only.get()), "POINT Z (1 2 3)");

  // XYZM Point with NaN Z and valid M.
  auto pt_m_only =
      MakeXYZMPoint(*factory, true, true, 1, 2, DoubleNotANumber, 4);
  EXPECT_EQ(keep_writer.write(pt_m_only.get()), "POINT ZM (1 2 NaN 4)");
  EXPECT_EQ(strip_writer.write(pt_m_only.get()), "POINT M (1 2 4)");

  // XYZM Point with both Z and M NaN.
  auto pt_xy_only = MakeXYZMPoint(*factory, true, true, 1, 2, DoubleNotANumber,
                                  DoubleNotANumber);
  EXPECT_EQ(strip_writer.write(pt_xy_only.get()), "POINT (1 2)");

  // XYZM LineString where vertex 0 supplies Z and vertex 1 supplies M keeps ZM.
  {
    auto seq = std::make_unique<CoordinateSequence>(0u, true, true);
    seq->add(CoordinateXYZM(1, 2, 3, DoubleNotANumber));
    seq->add(CoordinateXYZM(4, 5, DoubleNotANumber, 6));
    auto ls = factory->createLineString(std::move(seq));
    EXPECT_EQ(strip_writer.write(ls.get()),
              "LINESTRING ZM (1 2 3 NaN, 4 5 NaN 6)");
  }

  // XYZM LineString where all Z values are NaN strips Z and keeps M.
  {
    auto seq = std::make_unique<CoordinateSequence>(0u, true, true);
    seq->add(CoordinateXYZM(1, 2, DoubleNotANumber, 5));
    seq->add(CoordinateXYZM(4, 5, DoubleNotANumber, 6));
    auto ls = factory->createLineString(std::move(seq));
    EXPECT_EQ(strip_writer.write(ls.get()), "LINESTRING M (1 2 5, 4 5 6)");
  }

  // XYZ Polygon with all-NaN Z strips Z while keeping XY ring structure.
  {
    auto shell = std::make_unique<CoordinateSequence>(0u, true, false);
    shell->add(Coordinate(0, 0, DoubleNotANumber));
    shell->add(Coordinate(2, 0, DoubleNotANumber));
    shell->add(Coordinate(2, 2, DoubleNotANumber));
    shell->add(Coordinate(0, 2, DoubleNotANumber));
    shell->add(Coordinate(0, 0, DoubleNotANumber));
    auto poly =
        factory->createPolygon(factory->createLinearRing(std::move(shell)));
    EXPECT_EQ(keep_writer.write(poly.get()),
              "POLYGON Z ((0 0 NaN, 2 0 NaN, 2 2 NaN, 0 2 NaN, 0 0 NaN))");
    EXPECT_EQ(strip_writer.write(poly.get()),
              "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  }

  // GeometryCollection mixing Point Z (NaN Z) and Point XY strips Z at both
  // collection and element level.
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(MakeXYZMPoint(*factory, true, false, 1, 2, DoubleNotANumber,
                                  DoubleNotANumber));
    geoms.push_back(factory->createPoint(CoordinateXY(3, 4)));
    auto gc = factory->createGeometryCollection(std::move(geoms));
    EXPECT_EQ(strip_writer.write(gc.get()),
              "GEOMETRYCOLLECTION (POINT (1 2), POINT (3 4))");
  }

  // GeometryCollection mixing Point ZM and Point M with outputDimension = 3.
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(factory->createPoint(CoordinateXYZM(1, 2, 3, 4)));
    geoms.push_back(factory->createPoint(CoordinateXYM(5, 6, 7)));
    auto gc = factory->createGeometryCollection(std::move(geoms));

    WKTWriter dim3_keep;
    dim3_keep.setOutputDimension(3);
    EXPECT_EQ(dim3_keep.write(gc.get()),
              "GEOMETRYCOLLECTION Z (POINT Z (1 2 3), POINT M (5 6 7))");

    WKTWriter dim3_strip;
    dim3_strip.setOutputDimension(3);
    dim3_strip.setRemoveEmptyDimensions(true);
    EXPECT_EQ(dim3_strip.write(gc.get()),
              "GEOMETRYCOLLECTION Z (POINT Z (1 2 3), POINT (5 6))");
  }

  // GeometryCollection containing only empty XYZ Point preserves declared Z
  // even when removeEmptyDimensions = true; mixing an empty XYZ Point with an
  // all-NaN-Z Point strips Z at collection and non-empty element levels while
  // preserving declared Z on the empty element.
  {
    std::vector<std::unique_ptr<Geometry>> empty_only;
    empty_only.push_back(factory->createPoint(true, false));
    auto empty_gc = factory->createGeometryCollection(std::move(empty_only));
    EXPECT_EQ(strip_writer.write(empty_gc.get()),
              "GEOMETRYCOLLECTION Z (POINT Z EMPTY)");

    std::vector<std::unique_ptr<Geometry>> mixed;
    mixed.push_back(factory->createPoint(true, false));
    mixed.push_back(MakeXYZMPoint(*factory, true, false, 1, 2, DoubleNotANumber,
                                  DoubleNotANumber));
    auto mixed_gc = factory->createGeometryCollection(std::move(mixed));
    EXPECT_EQ(strip_writer.write(mixed_gc.get()),
              "GEOMETRYCOLLECTION (POINT Z EMPTY, POINT (1 2))");
  }
}

TEST(WKTWriterTest, ProtectedMethodsAndAppendSequenceIndent) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto pt = factory->createPoint(CoordinateXY(1, 2));
  CoordinateSequence seq({CoordinateXY(1, 2), CoordinateXY(3, 4)});
  CoordinateSequence empty_seq = CoordinateSequence::XY(0);

  TestableWKTWriter writer;
  // Set internal isFormatted = true via writeFormatted().
  EXPECT_EQ(writer.writeFormatted(pt.get()), "POINT (1 2)");
  {
    Writer out;
    writer.CallAppendSequenceText(seq, OrdinateSet::createXY(), /*level=*/1,
                                  /*do_indent=*/true, out);
    EXPECT_EQ(out.toString(), "\n  (1 2, 3 4)");
  }
  {
    Writer out;
    writer.CallAppendSequenceText(empty_seq, OrdinateSet::createXY(),
                                  /*level=*/1, /*do_indent=*/true, out);
    EXPECT_EQ(out.toString(), "EMPTY");
  }

  // Set internal isFormatted = false via write().
  EXPECT_EQ(writer.write(pt.get()), "POINT (1 2)");
  {
    Writer out;
    writer.CallAppendSequenceText(seq, OrdinateSet::createXY(), /*level=*/1,
                                  /*do_indent=*/true, out);
    EXPECT_EQ(out.toString(), "(1 2, 3 4)");
  }

  {
    Writer out;
    writer.CallAppendTag(*pt, OrdinateSet::createXYZ(), out);
    writer.CallAppendCoordinate(CoordinateXYZM(1, 2, 3, 4),
                                OrdinateSet::createXYZ(), out);
    EXPECT_EQ(out.toString(), "POINT Z 1 2 3");
  }
  {
    Writer out;
    writer.CallAppendOrdinateText(OrdinateSet::createXYZM(), out);
    writer.CallAppendCoordinate(CoordinateXYZM(1, 2, 3, 4),
                                OrdinateSet::createXYZM(), out);
    EXPECT_EQ(out.toString(), "ZM 1 2 3 4");
  }

  writer.setRoundingPrecision(2);
  EXPECT_EQ(writer.write(pt.get()), "POINT (1 2)");
  EXPECT_EQ(writer.CallWriteNumber(3.14159), "3.14");
}

TEST(WKTWriterTest, RoundTripWithWKTReader) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  WKTWriter writer;

  constexpr auto kRoundTripWkts = std::to_array<std::string_view>({
      "POINT (1.25 -2.5)",
      "POINT Z (1 2 3)",
      "POINT M (1 2 4)",
      "POINT ZM (1 2 3 4)",
      "POINT Z EMPTY",
      "LINESTRING (0 0, 1 2, 3 4)",
      "LINESTRING M EMPTY",
      "LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)",
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "COMPOUNDCURVE Z ((0 0 1, 1 1 2), CIRCULARSTRING Z (1 1 2, 2 2 3, 3 1 "
      "4))",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))",
      "CURVEPOLYGON (COMPOUNDCURVE ((0 0, 4 0), CIRCULARSTRING (4 0, 6 2, 4 "
      "4), "
      "(4 4, 0 4, 0 0)), CIRCULARSTRING (1 1, 2 2, 3 1, 2 0, 1 1))",
      "CURVEPOLYGON ZM EMPTY",
      "MULTIPOINT ((1 2), (3 4))",
      "MULTIPOINT (EMPTY, (1 2))",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
      "MULTICURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((3 3, 5 3, 5 5, 3 5, 3 3)))",
      "MULTISURFACE (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "CURVEPOLYGON (CIRCULARSTRING (3 0, 4 1, 5 0, 4 -1, 3 0)))",
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 3 4))",
      "GEOMETRYCOLLECTION Z (POINT Z (1 2 3), LINESTRING Z (0 0 1, 1 1 2))",
  });

  for (std::string_view wkt : kRoundTripWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> original = reader.read(std::string(wkt));
    ASSERT_NE(original, nullptr);

    const std::string written = writer.write(original.get());
    EXPECT_EQ(written, wkt);
    std::unique_ptr<Geometry> reparsed = reader.read(written);
    EXPECT_TRUE(original->equalsIdentical(reparsed.get()));

    const std::string formatted = writer.writeFormatted(original.get());
    std::unique_ptr<Geometry> reparsed_formatted = reader.read(formatted);
    EXPECT_TRUE(original->equalsIdentical(reparsed_formatted.get()));
  }
}

}  // namespace
}  // namespace geos_google
