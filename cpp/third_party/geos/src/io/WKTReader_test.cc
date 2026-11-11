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

// Unit tests for third_party/geos/src/io/WKTReader.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for ParseException and
//   IllegalArgumentException assertions.
// - Allocates isolated GeometryFactory instances via GeometryFactory::create()
//   for nearly all tests (one dedicated test exercises the default ctor).
// - Accesses XYM CoordinateSequence M ordinates via getOrdinate() / 2-arg
//   getAt() because GEOS_COORDSEQ_PADZ stores XYM with stride 4.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/WKTReader.cpp

#include "third_party/geos/include/geos/io/WKTReader.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
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
#include "third_party/geos/include/geos/io/ParseException.h"
#include "third_party/geos/include/geos/io/StringTokenizer.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXYM;
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
using geos::io::ParseException;
using geos::io::StringTokenizer;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;

struct EmptyGeometryCase {
  std::string_view wkt;
  GeometryTypeId expected_type_id;
  std::string_view expected_type_name;
  bool expected_has_z;
  bool expected_has_m;
};

constexpr auto kEmptyGeometryCases = std::to_array<EmptyGeometryCase>({
    {
        .wkt = "POINT EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "POINT Z EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = true,
        .expected_has_m = false,
    },
    {
        .wkt = "POINTZ EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = true,
        .expected_has_m = false,
    },
    {
        .wkt = "POINT M EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = false,
        .expected_has_m = true,
    },
    {
        .wkt = "POINTM EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = false,
        .expected_has_m = true,
    },
    {
        .wkt = "POINT ZM EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = true,
        .expected_has_m = true,
    },
    {
        .wkt = "POINTZM EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POINT,
        .expected_type_name = "Point",
        .expected_has_z = true,
        .expected_has_m = true,
    },
    {
        .wkt = "LINESTRING EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_LINESTRING,
        .expected_type_name = "LineString",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "LINESTRING Z EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_LINESTRING,
        .expected_type_name = "LineString",
        .expected_has_z = true,
        .expected_has_m = false,
    },
    {
        .wkt = "LINESTRING M EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_LINESTRING,
        .expected_type_name = "LineString",
        .expected_has_z = false,
        .expected_has_m = true,
    },
    {
        .wkt = "LINESTRING ZM EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_LINESTRING,
        .expected_type_name = "LineString",
        .expected_has_z = true,
        .expected_has_m = true,
    },
    {
        .wkt = "LINEARRING EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_LINEARRING,
        .expected_type_name = "LinearRing",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "LINEARRING Z EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_LINEARRING,
        .expected_type_name = "LinearRing",
        .expected_has_z = true,
        .expected_has_m = false,
    },
    {
        .wkt = "CIRCULARSTRING EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_CIRCULARSTRING,
        .expected_type_name = "CircularString",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "CIRCULARSTRING ZM EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_CIRCULARSTRING,
        .expected_type_name = "CircularString",
        .expected_has_z = true,
        .expected_has_m = true,
    },
    {
        .wkt = "COMPOUNDCURVE EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_COMPOUNDCURVE,
        .expected_type_name = "CompoundCurve",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "POLYGON EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POLYGON,
        .expected_type_name = "Polygon",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "POLYGON Z EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_POLYGON,
        .expected_type_name = "Polygon",
        .expected_has_z = true,
        .expected_has_m = false,
    },
    {
        .wkt = "CURVEPOLYGON EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_CURVEPOLYGON,
        .expected_type_name = "CurvePolygon",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "CURVEPOLYGON M EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_CURVEPOLYGON,
        .expected_type_name = "CurvePolygon",
        .expected_has_z = false,
        .expected_has_m = true,
    },
    {
        .wkt = "MULTIPOINT EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_MULTIPOINT,
        .expected_type_name = "MultiPoint",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "MULTILINESTRING EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_MULTILINESTRING,
        .expected_type_name = "MultiLineString",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "MULTICURVE EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_MULTICURVE,
        .expected_type_name = "MultiCurve",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "MULTIPOLYGON EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_MULTIPOLYGON,
        .expected_type_name = "MultiPolygon",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "MULTISURFACE EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_MULTISURFACE,
        .expected_type_name = "MultiSurface",
        .expected_has_z = false,
        .expected_has_m = false,
    },
    {
        .wkt = "GEOMETRYCOLLECTION EMPTY",
        .expected_type_id = GeometryTypeId::GEOS_GEOMETRYCOLLECTION,
        .expected_type_name = "GeometryCollection",
        .expected_has_z = false,
        .expected_has_m = false,
    },
});

struct PointDimCase {
  std::string_view name;
  std::string_view wkt;
  double x;
  double y;
  bool has_z;
  double z;
  bool has_m;
  double m;
};

constexpr auto kPointDimensionCases = std::to_array<PointDimCase>({
    {
        .name = "xy_basic",
        .wkt = "POINT (1.5 -2.25)",
        .x = 1.5,
        .y = -2.25,
        .has_z = false,
        .z = 0.0,
        .has_m = false,
        .m = 0.0,
    },
    {
        .name = "xyz_spaced",
        .wkt = "POINT Z (1 2 3)",
        .x = 1.0,
        .y = 2.0,
        .has_z = true,
        .z = 3.0,
        .has_m = false,
        .m = 0.0,
    },
    {
        .name = "xyz_suffixed",
        .wkt = "POINTZ (4 5 6)",
        .x = 4.0,
        .y = 5.0,
        .has_z = true,
        .z = 6.0,
        .has_m = false,
        .m = 0.0,
    },
    {
        .name = "xyz_undeclared",
        .wkt = "POINT (7 8 9)",
        .x = 7.0,
        .y = 8.0,
        .has_z = true,
        .z = 9.0,
        .has_m = false,
        .m = 0.0,
    },
    {
        .name = "xym_spaced",
        .wkt = "POINT M (1 2 30)",
        .x = 1.0,
        .y = 2.0,
        .has_z = false,
        .z = 0.0,
        .has_m = true,
        .m = 30.0,
    },
    {
        .name = "xym_suffixed",
        .wkt = "POINTM (4 5 60)",
        .x = 4.0,
        .y = 5.0,
        .has_z = false,
        .z = 0.0,
        .has_m = true,
        .m = 60.0,
    },
    {
        .name = "xyzm_spaced",
        .wkt = "POINT ZM (1 2 3 4)",
        .x = 1.0,
        .y = 2.0,
        .has_z = true,
        .z = 3.0,
        .has_m = true,
        .m = 4.0,
    },
    {
        .name = "xyzm_suffixed",
        .wkt = "POINTZM (5 6 7 8)",
        .x = 5.0,
        .y = 6.0,
        .has_z = true,
        .z = 7.0,
        .has_m = true,
        .m = 8.0,
    },
    {
        .name = "xyzm_undeclared",
        .wkt = "POINT (9 10 11 12)",
        .x = 9.0,
        .y = 10.0,
        .has_z = true,
        .z = 11.0,
        .has_m = true,
        .m = 12.0,
    },
});

constexpr auto kInvalidParseWktCases = std::to_array<std::string_view>({
    // Empty and truncated inputs (TT_EOF paths).
    "",
    "   \t\r\n  ",
    "POINT",
    "POINT (",
    "POINT (1",
    "POINT (1 2",
    "LINESTRING (0 0,",
    // Unexpected trailing tokens after geometry.
    "POINT (1 2) extra",
    "POINT (1 2) 3",
    "POINT (1 2) )",
    "POINT (1 2) ,",
    // Unknown or malformed geometry type names and non-word tokens.
    "123 (1 2)",
    "EMPTY",
    "UNKNOWN (1 2)",
    "ABCDE (1 2)",
    "POI (1 2)",
    "POINTA (1 2)",
    "POINTXY (1 2)",
    "POINTZMX (1 2)",
    "LINESTRINGAB (0 0, 1 1)",
    "GEOMETRY (1 2)",
    // Unexpected tokens in getNextEmptyOrOpener / getNextWord.
    "POINT FOO",
    "POINT )",
    "POINT ,",
    "POINT 1 2",
    // Unexpected tokens in getNextNumber.
    "POINT (abc 2)",
    "POINT (1 xyz)",
    "POINT ((1 2))",
    "POINT ()",
    "POINT (, 2)",
    "POINT (1)",
    // Unexpected tokens in getNextCloserOrComma.
    "LINESTRING (0 0 (1 1))",
    "LINESTRING (0 0 FOO)",
    "LINESTRING (0 0, )",
    "LINESTRING (0 0, 1 1 2)",
    // Duplicate or conflicting Z/M/ZM ordinate flags.
    "POINTZ Z (1 2 3)",
    "POINTM Z (1 2 3)",
    "POINTZM Z (1 2 3 4)",
    "POINTM M (1 2 3)",
    "POINTZ M (1 2 3)",
    "POINT Z M (1 2 3 4)",
    "POINTZ ZM (1 2 3 4)",
    "POINTZM ZM (1 2 3 4)",
    "MULTILINESTRING Z (Z (0 0 1, 1 1 2))",
    "MULTILINESTRING M (M (0 0 1, 1 1 2))",
    "MULTILINESTRING ZM (ZM (0 0 1 2, 3 4 5 6))",
    // Mixed coordinate counts and extra ordinates.
    "LINESTRING (0 0 1, 1 1)",
    "LINESTRING (0 0, 1 1 2)",
    "POINT Z (1 2 3 4)",
    "POINT M (1 2 3 4)",
    "POINT (1 2 3 4 5)",
    // Mixed dimensionality in nested geometries / collections.
    "GEOMETRYCOLLECTION Z (POINT (1 2))",
    "GEOMETRYCOLLECTION Z (POINT M (1 2 3))",
    "GEOMETRYCOLLECTION Z (POINT EMPTY)",
    "GEOMETRYCOLLECTION ZM (POINT Z (1 2 3))",
    "COMPOUNDCURVE Z (LINESTRING (0 0, 1 1))",
    "MULTICURVE Z (LINESTRING M (0 0 1, 1 1 2))",
    "MULTISURFACE Z (CURVEPOLYGON M ((0 0 1, 1 0 1, 1 1 1, 0 0 1)))",
    // Wrong component geometry types in curved/surface containers.
    "CURVEPOLYGON (POINT (0 0))",
    "CURVEPOLYGON (POLYGON ((0 0, 1 0, 1 1, 0 0)))",
    "CURVEPOLYGON ((0 0, 1 0, 1 1, 0 0), POINT (0.5 0.5))",
    "MULTICURVE (POINT (0 0))",
    "COMPOUNDCURVE (POINT (0 0))",
    "COMPOUNDCURVE ((0 0, 1 0), POINT (1 0))",
    "COMPOUNDCURVE (COMPOUNDCURVE ((0 0, 1 1)))",
    "MULTISURFACE (POINT (0 0))",
    "MULTISURFACE (LINESTRING (0 0, 1 1))",
    // MultiPoint unexpected tokens and invalid element transitions.
    "MULTIPOINT (",
    "MULTIPOINT ()",
    "MULTIPOINT (,)",
    "MULTIPOINT (FOO)",
    "MULTIPOINT (0 0, EMPTY)",
    "MULTIPOINT (1 2, )",
    "MULTIPOINT (1 2, 3 4 5)",
    "MULTIPOINT (1 2 3, 4 5)",
    "MULTIPOINT (1 2 3 4, 5 6)",
    "MULTIPOINT ((0 0), 1 1)",
    "MULTIPOINT ((1 2), )",
    "MULTIPOINT ((0 0 1), (1 1))",
    "MULTIPOINT ((0 0), Z (1 1 2))",
});

constexpr auto kInvalidGeometryWktCases = std::to_array<std::string_view>({
    "POINT (1 2, 3 4)",
    "LINESTRING (0 0)",
    "LINEARRING (0 0, 1 0, 1 1, 0 1)",
    "POLYGON ((0 0, 1 0, 1 1, 0 1))",
    "CIRCULARSTRING (0 0, 1 1)",
    "COMPOUNDCURVE (EMPTY)",
    "COMPOUNDCURVE ((0 0, 1 0), (2 0, 3 0))",
    "CURVEPOLYGON (EMPTY, (0 0, 1 0, 1 1, 0 0))",
});

class TestableWKTReader : public WKTReader {
 public:
  using WKTReader::getNextCloser;
  using WKTReader::WKTReader;
};

TEST(WKTReaderTest, ConstructorsAndPrecisionModel) {
  {
    WKTReader default_reader;
    Geometry::Ptr geom = default_reader.read("POINT (3 4)");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getFactory(), GeometryFactory::getDefaultInstance());
    const auto* pt = dynamic_cast<const Point*>(geom.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_DOUBLE_EQ(pt->getX(), 3.0);
    EXPECT_DOUBLE_EQ(pt->getY(), 4.0);
  }

  GeometryFactory::Ptr default_factory = GeometryFactory::create();
  {
    WKTReader ref_reader(*default_factory);
    Geometry::Ptr geom = ref_reader.read("POINT (5 6)");
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(geom->getFactory(), default_factory.get());
  }

  PrecisionModel fixed_pm(10.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm, 4326);
  WKTReader ptr_reader(fixed_factory.get());
  std::unique_ptr<Point> pt =
      ptr_reader.read<Point>("POINT Z (1.23 5.68 9.123)");
  ASSERT_NE(pt, nullptr);
  EXPECT_EQ(pt->getFactory(), fixed_factory.get());
  EXPECT_EQ(pt->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(pt->getX(), 1.2);
  EXPECT_DOUBLE_EQ(pt->getY(), 5.7);
  // PrecisionModel::makePrecise only snaps X and Y, leaving Z untouched.
  EXPECT_DOUBLE_EQ(pt->getZ(), 9.123);
}

TEST(WKTReaderTest, TypedReadSucceedsAndThrowsOnTypeMismatch) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Point> pt = reader.read<Point>("POINT (10 20)");
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), 10.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 20.0);

  std::unique_ptr<LineString> line =
      reader.read<LineString>("LINESTRING (0 0, 3 4)");
  ASSERT_NE(line, nullptr);
  EXPECT_DOUBLE_EQ(line->getLength(), 5.0);

  std::unique_ptr<Geometry> geom =
      reader.read<Geometry>("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  ASSERT_NE(geom, nullptr);
  EXPECT_DOUBLE_EQ(geom->getArea(), 4.0);

  EXPECT_THROW(reader.read<Polygon>("POINT (1 2)"), ParseException);
  EXPECT_THROW(reader.read<Point>("LINESTRING (0 0, 1 1)"), ParseException);
}

TEST(WKTReaderTest, EmptyGeometriesAcrossAllTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const EmptyGeometryCase& tc : kEmptyGeometryCases) {
    SCOPED_TRACE(std::string(tc.wkt));
    Geometry::Ptr geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_TRUE(geom->isEmpty());
    EXPECT_EQ(geom->getGeometryTypeId(), tc.expected_type_id);
    EXPECT_EQ(geom->getGeometryType(), tc.expected_type_name);
    EXPECT_EQ(geom->hasZ(), tc.expected_has_z);
    EXPECT_EQ(geom->hasM(), tc.expected_has_m);
  }
}

TEST(WKTReaderTest, CaseInsensitivityAndWhitespaceAndScientificNotation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Geometry::Ptr pt_geom =
      reader.read("  \t\r\n pOiNt \n ( \t -1.5e2 \r\n 2.5E-1 \t ) \r\n ");
  ASSERT_NE(pt_geom, nullptr);
  const auto* pt = dynamic_cast<const Point*>(pt_geom.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), -150.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 0.25);

  std::unique_ptr<LineString> line =
      reader.read<LineString>("lineString z (0 0 1, 3 4 1)");
  ASSERT_NE(line, nullptr);
  EXPECT_TRUE(line->hasZ());
  EXPECT_FALSE(line->hasM());
  EXPECT_EQ(line->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(line->getLength(), 5.0);

  Geometry::Ptr empty_m = reader.read("Point m Empty");
  ASSERT_NE(empty_m, nullptr);
  EXPECT_TRUE(empty_m->isEmpty());
  EXPECT_FALSE(empty_m->hasZ());
  EXPECT_TRUE(empty_m->hasM());
}

TEST(WKTReaderTest, PointDimensionsAndCoordinateOrdinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const PointDimCase& tc : kPointDimensionCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Point> pt = reader.read<Point>(std::string(tc.wkt));
    ASSERT_NE(pt, nullptr);
    EXPECT_FALSE(pt->isEmpty());
    EXPECT_EQ(pt->hasZ(), tc.has_z);
    EXPECT_EQ(pt->hasM(), tc.has_m);
    EXPECT_EQ(pt->getCoordinateDimension(),
              static_cast<std::uint8_t>(2 + tc.has_z + tc.has_m));
    EXPECT_DOUBLE_EQ(pt->getX(), tc.x);
    EXPECT_DOUBLE_EQ(pt->getY(), tc.y);

    if (tc.has_z) {
      EXPECT_DOUBLE_EQ(pt->getZ(), tc.z);
    } else {
      EXPECT_TRUE(std::isnan(pt->getZ()));
    }
    if (tc.has_m) {
      EXPECT_DOUBLE_EQ(pt->getM(), tc.m);
      const CoordinateSequence* seq = pt->getCoordinatesRO();
      EXPECT_DOUBLE_EQ(seq->getOrdinate(0, CoordinateSequence::M), tc.m);
      CoordinateXYM xym;
      seq->getAt(0, xym);
      EXPECT_DOUBLE_EQ(xym.x, tc.x);
      EXPECT_DOUBLE_EQ(xym.y, tc.y);
      EXPECT_DOUBLE_EQ(xym.m, tc.m);
    } else {
      EXPECT_TRUE(std::isnan(pt->getM()));
    }
  }
}

TEST(WKTReaderTest, LineStringAndLinearRingVariants) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<LineString> line_z =
      reader.read<LineString>("LINESTRING (0 0 10, 1 2 20, 3 4 30)");
  ASSERT_NE(line_z, nullptr);
  EXPECT_TRUE(line_z->hasZ());
  EXPECT_FALSE(line_z->hasM());
  ASSERT_EQ(line_z->getNumPoints(), 3u);
  const CoordinateSequence* line_coords = line_z->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(line_coords->getOrdinate(2, CoordinateSequence::X), 3.0);
  EXPECT_DOUBLE_EQ(line_coords->getOrdinate(2, CoordinateSequence::Y), 4.0);
  EXPECT_DOUBLE_EQ(line_coords->getOrdinate(2, CoordinateSequence::Z), 30.0);

  std::unique_ptr<LineString> line_zm =
      reader.read<LineString>("LINESTRING (0 0 1 100, 2 3 4 200)");
  ASSERT_NE(line_zm, nullptr);
  EXPECT_TRUE(line_zm->hasZ());
  EXPECT_TRUE(line_zm->hasM());
  EXPECT_DOUBLE_EQ(
      line_zm->getCoordinatesRO()->getOrdinate(1, CoordinateSequence::M),
      200.0);

  std::unique_ptr<LinearRing> ring_m =
      reader.read<LinearRing>("LINEARRING M (0 0 1, 2 0 2, 2 2 3, 0 0 4)");
  ASSERT_NE(ring_m, nullptr);
  EXPECT_FALSE(ring_m->hasZ());
  EXPECT_TRUE(ring_m->hasM());
  EXPECT_TRUE(ring_m->isClosed());
  EXPECT_EQ(ring_m->getNumPoints(), 4u);
  CoordinateXYM ring_pt;
  ring_m->getCoordinatesRO()->getAt(2, ring_pt);
  EXPECT_DOUBLE_EQ(ring_pt.x, 2.0);
  EXPECT_DOUBLE_EQ(ring_pt.y, 2.0);
  EXPECT_DOUBLE_EQ(ring_pt.m, 3.0);
}

TEST(WKTReaderTest, CircularStringAndCompoundCurve) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<CircularString> arc =
      reader.read<CircularString>("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ASSERT_NE(arc, nullptr);
  EXPECT_TRUE(arc->hasCurvedComponents());
  EXPECT_EQ(arc->getNumPoints(), 3u);
  EXPECT_NEAR(arc->getLength(), std::numbers::pi, 1e-9);

  std::unique_ptr<CircularString> arc_zm = reader.read<CircularString>(
      "CIRCULARSTRING ZM (0 0 1 10, 1 1 2 20, 2 0 3 30)");
  ASSERT_NE(arc_zm, nullptr);
  EXPECT_TRUE(arc_zm->hasZ());
  EXPECT_TRUE(arc_zm->hasM());
  EXPECT_DOUBLE_EQ(
      arc_zm->getCoordinatesRO()->getOrdinate(1, CoordinateSequence::Z), 2.0);
  EXPECT_DOUBLE_EQ(
      arc_zm->getCoordinatesRO()->getOrdinate(1, CoordinateSequence::M), 20.0);

  // Mix of untagged linear segment, tagged circular arc, and tagged linestring.
  std::unique_ptr<CompoundCurve> cc = reader.read<CompoundCurve>(
      "COMPOUNDCURVE ((0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 0), "
      "LINESTRING (3 0, 4 0))");
  ASSERT_NE(cc, nullptr);
  EXPECT_FALSE(cc->isEmpty());
  EXPECT_FALSE(cc->isClosed());
  EXPECT_TRUE(cc->hasCurvedComponents());
  ASSERT_EQ(cc->getNumCurves(), 3u);
  EXPECT_EQ(cc->getCurveN(0)->getGeometryTypeId(),
            GeometryTypeId::GEOS_LINESTRING);
  EXPECT_EQ(cc->getCurveN(1)->getGeometryTypeId(),
            GeometryTypeId::GEOS_CIRCULARSTRING);
  EXPECT_EQ(cc->getCurveN(2)->getGeometryTypeId(),
            GeometryTypeId::GEOS_LINESTRING);
  EXPECT_NEAR(cc->getLength(), 2.0 + std::numbers::pi, 1e-9);

  // Declared Z on CompoundCurve propagates to untagged sections and validates
  // tagged sections.
  std::unique_ptr<CompoundCurve> cc_z = reader.read<CompoundCurve>(
      "COMPOUNDCURVE Z ((0 0 1, 2 0 1), CIRCULARSTRING (2 0 1, 1 1 2, 0 0 1))");
  ASSERT_NE(cc_z, nullptr);
  EXPECT_TRUE(cc_z->hasZ());
  EXPECT_FALSE(cc_z->hasM());
  EXPECT_TRUE(cc_z->isClosed());
}

TEST(WKTReaderTest, PolygonAndCurvePolygon) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Polygon> poly = reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(1 1, 3 1, 3 3, 1 3, 1 1), "
      "(5 5, 7 5, 7 7, 5 7, 5 5))");
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 92.0);

  std::unique_ptr<Polygon> poly_z = reader.read<Polygon>(
      "POLYGON Z ((0 0 1, 4 0 1, 4 4 1, 0 4 1, 0 0 1), "
      "(1 1 2, 2 1 2, 2 2 2, 1 2 2, 1 1 2))");
  ASSERT_NE(poly_z, nullptr);
  EXPECT_TRUE(poly_z->hasZ());
  EXPECT_FALSE(poly_z->hasM());
  EXPECT_DOUBLE_EQ(poly_z->getArea(), 15.0);
  EXPECT_DOUBLE_EQ(poly_z->getInteriorRingN(0)->getCoordinatesRO()->getOrdinate(
                       0, CoordinateSequence::Z),
                   2.0);

  // CurvePolygon with untagged linear shell, untagged linear hole, and
  // circular hole.
  std::unique_ptr<CurvePolygon> cp = reader.read<CurvePolygon>(
      "CURVEPOLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(1 1, 3 1, 3 3, 1 3, 1 1), "
      "CIRCULARSTRING (10 10, 11 11, 12 10, 11 9, 10 10))");
  ASSERT_NE(cp, nullptr);
  EXPECT_TRUE(cp->hasCurvedComponents());
  EXPECT_EQ(cp->getNumInteriorRing(), 2u);
  EXPECT_NEAR(cp->getArea(), 400.0 - 4.0 - std::numbers::pi, 1e-9);

  // CurvePolygon with CircularString shell.
  std::unique_ptr<CurvePolygon> circle_cp = reader.read<CurvePolygon>(
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))");
  ASSERT_NE(circle_cp, nullptr);
  EXPECT_NEAR(circle_cp->getArea(), std::numbers::pi, 1e-9);

  // CurvePolygon with CompoundCurve shell (semicircle).
  std::unique_ptr<CurvePolygon> semi_cp = reader.read<CurvePolygon>(
      "CURVEPOLYGON (COMPOUNDCURVE ((0 0, 2 0), "
      "CIRCULARSTRING (2 0, 1 1, 0 0)))");
  ASSERT_NE(semi_cp, nullptr);
  EXPECT_NEAR(semi_cp->getArea(), std::numbers::pi / 2.0, 1e-9);

  // CurvePolygon with bare EMPTY shell maps EMPTY via defaultType = LINESTRING.
  std::unique_ptr<CurvePolygon> empty_shell_cp =
      reader.read<CurvePolygon>("CURVEPOLYGON (EMPTY)");
  ASSERT_NE(empty_shell_cp, nullptr);
  EXPECT_TRUE(empty_shell_cp->isEmpty());
  const Curve* empty_shell = empty_shell_cp->getExteriorRing();
  ASSERT_NE(empty_shell, nullptr);
  EXPECT_EQ(empty_shell->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);

  std::unique_ptr<CurvePolygon> empty_shell_cp_z =
      reader.read<CurvePolygon>("CURVEPOLYGON Z (EMPTY)");
  ASSERT_NE(empty_shell_cp_z, nullptr);
  EXPECT_TRUE(empty_shell_cp_z->isEmpty());
  EXPECT_TRUE(empty_shell_cp_z->hasZ());
}

TEST(WKTReaderTest, MultiPointFormatsAndEmptyElements) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Unparenthesized coordinates.
  std::unique_ptr<MultiPoint> mp_flat =
      reader.read<MultiPoint>("MULTIPOINT (0 0, 1 2, 3 4)");
  ASSERT_NE(mp_flat, nullptr);
  ASSERT_EQ(mp_flat->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(mp_flat->getGeometryN(0)->getX(), 0.0);
  EXPECT_DOUBLE_EQ(mp_flat->getGeometryN(1)->getX(), 1.0);
  EXPECT_DOUBLE_EQ(mp_flat->getGeometryN(1)->getY(), 2.0);
  EXPECT_DOUBLE_EQ(mp_flat->getGeometryN(2)->getX(), 3.0);
  EXPECT_DOUBLE_EQ(mp_flat->getGeometryN(2)->getY(), 4.0);

  // Unparenthesized with Z, M, and ZM.
  std::unique_ptr<MultiPoint> mp_z =
      reader.read<MultiPoint>("MULTIPOINT Z (1 2 3, 4 5 6)");
  ASSERT_NE(mp_z, nullptr);
  EXPECT_TRUE(mp_z->hasZ());
  EXPECT_FALSE(mp_z->hasM());
  EXPECT_DOUBLE_EQ(mp_z->getGeometryN(1)->getZ(), 6.0);

  std::unique_ptr<MultiPoint> mp_undeclared_z =
      reader.read<MultiPoint>("MULTIPOINT (1 2 3, 4 5 6)");
  ASSERT_NE(mp_undeclared_z, nullptr);
  EXPECT_TRUE(mp_undeclared_z->hasZ());
  EXPECT_FALSE(mp_undeclared_z->hasM());
  EXPECT_DOUBLE_EQ(mp_undeclared_z->getGeometryN(1)->getZ(), 6.0);

  std::unique_ptr<MultiPoint> mp_m =
      reader.read<MultiPoint>("MULTIPOINT M (0 0 10, 1 1 20)");
  ASSERT_NE(mp_m, nullptr);
  EXPECT_FALSE(mp_m->hasZ());
  EXPECT_TRUE(mp_m->hasM());
  EXPECT_DOUBLE_EQ(mp_m->getGeometryN(1)->getM(), 20.0);

  std::unique_ptr<MultiPoint> mp_zm =
      reader.read<MultiPoint>("MULTIPOINT ZM (1 2 3 4, 5 6 7 8)");
  ASSERT_NE(mp_zm, nullptr);
  EXPECT_TRUE(mp_zm->hasZ());
  EXPECT_TRUE(mp_zm->hasM());
  EXPECT_DOUBLE_EQ(mp_zm->getGeometryN(1)->getZ(), 7.0);
  EXPECT_DOUBLE_EQ(mp_zm->getGeometryN(1)->getM(), 8.0);

  // Unparenthesized MULTIPOINT pre-allocates CoordinateSequence from header
  // flags before inspecting the first coordinate, so undeclared 4D keeps Z
  // and drops M, whereas parenthesized elements preserve both Z and M.
  std::unique_ptr<MultiPoint> mp_undeclared_4d =
      reader.read<MultiPoint>("MULTIPOINT (1 2 3 4, 5 6 7 8)");
  ASSERT_NE(mp_undeclared_4d, nullptr);
  EXPECT_TRUE(mp_undeclared_4d->hasZ());
  EXPECT_FALSE(mp_undeclared_4d->hasM());
  EXPECT_DOUBLE_EQ(mp_undeclared_4d->getGeometryN(1)->getZ(), 7.0);
  EXPECT_TRUE(std::isnan(mp_undeclared_4d->getGeometryN(1)->getM()));

  std::unique_ptr<MultiPoint> mp_parens_zm =
      reader.read<MultiPoint>("MULTIPOINT ((1 2 3 4), (5 6 7 8))");
  ASSERT_NE(mp_parens_zm, nullptr);
  EXPECT_TRUE(mp_parens_zm->hasZ());
  EXPECT_TRUE(mp_parens_zm->hasM());
  EXPECT_DOUBLE_EQ(mp_parens_zm->getGeometryN(1)->getZ(), 7.0);
  EXPECT_DOUBLE_EQ(mp_parens_zm->getGeometryN(1)->getM(), 8.0);

  // Parenthesized coordinates with EMPTY elements (starts with '(' or WORD).
  std::unique_ptr<MultiPoint> mp_parens =
      reader.read<MultiPoint>("MULTIPOINT ((0 0), EMPTY, (3 4))");
  ASSERT_NE(mp_parens, nullptr);
  ASSERT_EQ(mp_parens->getNumGeometries(), 3u);
  EXPECT_DOUBLE_EQ(mp_parens->getGeometryN(0)->getX(), 0.0);
  EXPECT_TRUE(mp_parens->getGeometryN(1)->isEmpty());
  EXPECT_DOUBLE_EQ(mp_parens->getGeometryN(2)->getY(), 4.0);

  std::unique_ptr<MultiPoint> mp_leading_empty =
      reader.read<MultiPoint>("MULTIPOINT Z (EMPTY, (1 2 3))");
  ASSERT_NE(mp_leading_empty, nullptr);
  ASSERT_EQ(mp_leading_empty->getNumGeometries(), 2u);
  EXPECT_TRUE(mp_leading_empty->getGeometryN(0)->isEmpty());
  EXPECT_TRUE(mp_leading_empty->getGeometryN(0)->hasZ());
  EXPECT_DOUBLE_EQ(mp_leading_empty->getGeometryN(1)->getZ(), 3.0);

  // First point declares Z/M/ZM via SF1.2 modifier, subsequent points inherit.
  std::unique_ptr<MultiPoint> mp_inner_z =
      reader.read<MultiPoint>("MULTIPOINT (Z (1 2 3), (4 5 6))");
  ASSERT_NE(mp_inner_z, nullptr);
  ASSERT_EQ(mp_inner_z->getNumGeometries(), 2u);
  EXPECT_TRUE(mp_inner_z->hasZ());
  EXPECT_DOUBLE_EQ(mp_inner_z->getGeometryN(0)->getZ(), 3.0);
  EXPECT_DOUBLE_EQ(mp_inner_z->getGeometryN(1)->getZ(), 6.0);

  std::unique_ptr<MultiPoint> mp_inner_m =
      reader.read<MultiPoint>("MULTIPOINT (M (1 2 3), (4 5 6))");
  ASSERT_NE(mp_inner_m, nullptr);
  ASSERT_EQ(mp_inner_m->getNumGeometries(), 2u);
  EXPECT_FALSE(mp_inner_m->hasZ());
  EXPECT_TRUE(mp_inner_m->hasM());
  EXPECT_DOUBLE_EQ(mp_inner_m->getGeometryN(1)->getM(), 6.0);

  std::unique_ptr<MultiPoint> mp_inner_zm =
      reader.read<MultiPoint>("MULTIPOINT (ZM (1 2 3 4), (5 6 7 8))");
  ASSERT_NE(mp_inner_zm, nullptr);
  ASSERT_EQ(mp_inner_zm->getNumGeometries(), 2u);
  EXPECT_TRUE(mp_inner_zm->hasZ());
  EXPECT_TRUE(mp_inner_zm->hasM());
  EXPECT_DOUBLE_EQ(mp_inner_zm->getGeometryN(1)->getZ(), 7.0);
  EXPECT_DOUBLE_EQ(mp_inner_zm->getGeometryN(1)->getM(), 8.0);
}

TEST(WKTReaderTest, MultiLineStringMultiCurveMultiPolygonMultiSurface) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<MultiLineString> mls = reader.read<MultiLineString>(
      "MULTILINESTRING M ((0 0 1, 1 0 2), EMPTY, (2 2 3, 2 5 4))");
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 3u);
  EXPECT_TRUE(mls->hasM());
  EXPECT_TRUE(mls->getGeometryN(1)->isEmpty());
  EXPECT_DOUBLE_EQ(mls->getLength(), 4.0);

  std::unique_ptr<MultiCurve> mc = reader.read<MultiCurve>(
      "MULTICURVE Z ((0 0 1, 1 0 1), LINESTRING (1 0 1, 2 0 1), "
      "CIRCULARSTRING (0 0 1, 1 1 2, 2 0 1), "
      "COMPOUNDCURVE ((0 0 1, 1 0 1), (1 0 1, 2 1 2)), EMPTY)");
  ASSERT_NE(mc, nullptr);
  ASSERT_EQ(mc->getNumGeometries(), 5u);
  EXPECT_TRUE(mc->hasZ());
  EXPECT_EQ(mc->getGeometryN(0)->getGeometryTypeId(),
            GeometryTypeId::GEOS_LINESTRING);
  EXPECT_EQ(mc->getGeometryN(2)->getGeometryTypeId(),
            GeometryTypeId::GEOS_CIRCULARSTRING);
  EXPECT_EQ(mc->getGeometryN(3)->getGeometryTypeId(),
            GeometryTypeId::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(mc->getGeometryN(4)->getGeometryTypeId(),
            GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(mc->getGeometryN(4)->isEmpty());
  EXPECT_TRUE(mc->getGeometryN(4)->hasZ());

  std::unique_ptr<MultiPolygon> mp = reader.read<MultiPolygon>(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(1 1, 2 1, 2 2, 1 2, 1 1)), EMPTY, ((10 0, 12 0, 12 2, 10 0)))");
  ASSERT_NE(mp, nullptr);
  ASSERT_EQ(mp->getNumGeometries(), 3u);
  EXPECT_EQ(mp->getGeometryN(0)->getNumInteriorRing(), 1u);
  EXPECT_TRUE(mp->getGeometryN(1)->isEmpty());
  EXPECT_DOUBLE_EQ(mp->getArea(), 17.0);

  std::unique_ptr<MultiSurface> ms = reader.read<MultiSurface>(
      "MULTISURFACE ZM (((0 0 1 10, 2 0 1 10, 2 2 1 10, 0 0 1 10)), "
      "POLYGON ((3 0 1 10, 5 0 1 10, 5 2 1 10, 3 0 1 10)), "
      "CURVEPOLYGON ZM (CIRCULARSTRING (10 0 1 10, 11 1 1 10, 12 0 1 10, "
      "11 -1 1 10, 10 0 1 10)), EMPTY)");
  ASSERT_NE(ms, nullptr);
  ASSERT_EQ(ms->getNumGeometries(), 4u);
  EXPECT_TRUE(ms->hasZ());
  EXPECT_TRUE(ms->hasM());
  EXPECT_EQ(ms->getGeometryN(0)->getGeometryTypeId(),
            GeometryTypeId::GEOS_POLYGON);
  EXPECT_EQ(ms->getGeometryN(1)->getGeometryTypeId(),
            GeometryTypeId::GEOS_POLYGON);
  EXPECT_EQ(ms->getGeometryN(2)->getGeometryTypeId(),
            GeometryTypeId::GEOS_CURVEPOLYGON);
  EXPECT_EQ(ms->getGeometryN(3)->getGeometryTypeId(),
            GeometryTypeId::GEOS_POLYGON);
  EXPECT_TRUE(ms->getGeometryN(3)->isEmpty());
  EXPECT_TRUE(ms->getGeometryN(3)->hasZ());
  EXPECT_TRUE(ms->getGeometryN(3)->hasM());

  // CurvePolygon with an untagged linear shell infers Z before validating its
  // CircularString hole inside a MultiSurface Z.
  std::unique_ptr<MultiSurface> ms_z = reader.read<MultiSurface>(
      "MULTISURFACE Z (CURVEPOLYGON ((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1), "
      "CIRCULARSTRING (2 2 1, 3 3 1, 4 2 1, 3 1 1, 2 2 1)))");
  ASSERT_NE(ms_z, nullptr);
  ASSERT_EQ(ms_z->getNumGeometries(), 1u);
  EXPECT_TRUE(ms_z->hasZ());
  EXPECT_FALSE(ms_z->hasM());

  // Suffixed Z/M/ZM tags on curve and multi-geometry types.
  std::unique_ptr<MultiLineString> mls_z =
      reader.read<MultiLineString>("MULTILINESTRINGZ ((0 0 1, 1 1 2))");
  EXPECT_TRUE(mls_z->hasZ());
  std::unique_ptr<MultiPolygon> mp_m = reader.read<MultiPolygon>(
      "MULTIPOLYGONM (((0 0 1, 2 0 1, 2 2 1, 0 0 1)))");
  EXPECT_TRUE(mp_m->hasM());
  std::unique_ptr<CompoundCurve> cc_zm = reader.read<CompoundCurve>(
      "COMPOUNDCURVEZM (CIRCULARSTRING (0 0 1 2, 1 1 3 4, 2 0 5 6))");
  EXPECT_TRUE(cc_zm->hasZ());
  EXPECT_TRUE(cc_zm->hasM());
}

TEST(WKTReaderTest, GeometryCollectionNestedAndDimensionalityRules) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Untagged GeometryCollection permits heterogeneous element dimensions.
  std::unique_ptr<GeometryCollection> mixed = reader.read<GeometryCollection>(
      "GEOMETRYCOLLECTION (POINT Z (1 2 3), POINT M (4 5 6), "
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 1), CIRCULARSTRING (0 0, 1 1, 2 "
      "0)))");
  ASSERT_NE(mixed, nullptr);
  ASSERT_EQ(mixed->getNumGeometries(), 3u);
  EXPECT_TRUE(mixed->getGeometryN(0)->hasZ());
  EXPECT_FALSE(mixed->getGeometryN(0)->hasM());
  EXPECT_FALSE(mixed->getGeometryN(1)->hasZ());
  EXPECT_TRUE(mixed->getGeometryN(1)->hasM());
  const auto* nested =
      dynamic_cast<const GeometryCollection*>(mixed->getGeometryN(2));
  ASSERT_NE(nested, nullptr);
  ASSERT_EQ(nested->getNumGeometries(), 2u);
  EXPECT_EQ(nested->getGeometryN(1)->getGeometryTypeId(),
            GeometryTypeId::GEOS_CIRCULARSTRING);

  // Tagged Z GeometryCollection succeeds when children declare or infer Z.
  std::unique_ptr<GeometryCollection> gc_z = reader.read<GeometryCollection>(
      "GEOMETRYCOLLECTION Z (POINT Z (1 2 3), POINT (4 5 6), POINT Z EMPTY)");
  ASSERT_NE(gc_z, nullptr);
  ASSERT_EQ(gc_z->getNumGeometries(), 3u);
  EXPECT_TRUE(gc_z->hasZ());
  EXPECT_TRUE(gc_z->getGeometryN(2)->isEmpty());
  EXPECT_TRUE(gc_z->getGeometryN(2)->hasZ());
}

TEST(WKTReaderTest, SetFixStructureClosesUnclosedRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  EXPECT_THROW(reader.read("LINEARRING (0 0, 1 0, 1 1, 0 1)"),
               IllegalArgumentException);
  EXPECT_THROW(reader.read("POLYGON ((0 0, 1 0, 1 1, 0 1))"),
               IllegalArgumentException);

  reader.setFixStructure(true);

  std::unique_ptr<LinearRing> fixed_ring =
      reader.read<LinearRing>("LINEARRING Z (0 0 5, 1 0 6, 1 1 7, 0 1 8)");
  ASSERT_NE(fixed_ring, nullptr);
  EXPECT_TRUE(fixed_ring->isClosed());
  ASSERT_EQ(fixed_ring->getNumPoints(), 5u);
  const CoordinateSequence* ring_coords = fixed_ring->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(ring_coords->getOrdinate(4, CoordinateSequence::X), 0.0);
  EXPECT_DOUBLE_EQ(ring_coords->getOrdinate(4, CoordinateSequence::Y), 0.0);
  EXPECT_DOUBLE_EQ(ring_coords->getOrdinate(4, CoordinateSequence::Z), 5.0);

  std::unique_ptr<Polygon> fixed_poly = reader.read<Polygon>(
      "POLYGON ((0 0, 10 0, 10 10, 0 10), (2 2, 4 2, 4 4, 2 4))");
  ASSERT_NE(fixed_poly, nullptr);
  EXPECT_EQ(fixed_poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(fixed_poly->getArea(), 96.0);

  // Already-closed valid ring stays untouched when fixStructure is true.
  std::unique_ptr<LinearRing> already_closed =
      reader.read<LinearRing>("LINEARRING (0 0, 1 0, 1 1, 0 0)");
  ASSERT_NE(already_closed, nullptr);
  EXPECT_TRUE(already_closed->isClosed());
  EXPECT_EQ(already_closed->getNumPoints(), 4u);

  // Unclosed ZM ring preserves both Z and M on the appended closing vertex.
  std::unique_ptr<LinearRing> fixed_zm_ring = reader.read<LinearRing>(
      "LINEARRING ZM (0 0 5 10, 1 0 6 20, 1 1 7 30, 0 1 8 40)");
  ASSERT_NE(fixed_zm_ring, nullptr);
  EXPECT_TRUE(fixed_zm_ring->isClosed());
  ASSERT_EQ(fixed_zm_ring->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(
      fixed_zm_ring->getCoordinatesRO()->getOrdinate(4, CoordinateSequence::Z),
      5.0);
  EXPECT_DOUBLE_EQ(
      fixed_zm_ring->getCoordinatesRO()->getOrdinate(4, CoordinateSequence::M),
      10.0);

  // Empty rings remain empty; 2-point unclosed rings close to 3 points.
  std::unique_ptr<LinearRing> empty_ring =
      reader.read<LinearRing>("LINEARRING EMPTY");
  EXPECT_TRUE(empty_ring->isEmpty());
  std::unique_ptr<LinearRing> collapsed_ring =
      reader.read<LinearRing>("LINEARRING (0 0, 1 0)");
  EXPECT_EQ(collapsed_ring->getNumPoints(), 3u);
  EXPECT_TRUE(collapsed_ring->isClosed());

  // Already-closed 2-point ring stays length 2 and still fails validation.
  EXPECT_THROW(reader.read("LINEARRING (0 0, 0 0)"), IllegalArgumentException);

  // Resetting fixStructure restores strict validation.
  reader.setFixStructure(false);
  EXPECT_THROW(reader.read("LINEARRING (0 0, 1 0, 1 1, 0 1)"),
               IllegalArgumentException);
}

TEST(WKTReaderTest, ThrowsParseExceptionOnMalformedInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (std::string_view bad_wkt : kInvalidParseWktCases) {
    SCOPED_TRACE(std::string(bad_wkt));
    EXPECT_THROW(reader.read(std::string(bad_wkt)), ParseException);
  }
}

TEST(WKTReaderTest, ThrowsIllegalArgumentExceptionOnInvalidGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (std::string_view invalid_geom_wkt : kInvalidGeometryWktCases) {
    SCOPED_TRACE(std::string(invalid_geom_wkt));
    EXPECT_THROW(reader.read(std::string(invalid_geom_wkt)),
                 IllegalArgumentException);
  }
}

TEST(WKTReaderTest, ProtectedGetNextCloserHelper) {
  const std::string valid_closer = ")";
  StringTokenizer valid_tok(valid_closer);
  EXPECT_EQ(TestableWKTReader::getNextCloser(&valid_tok), ")");

  const std::string comma_token = ",";
  StringTokenizer comma_tok(comma_token);
  EXPECT_THROW(TestableWKTReader::getNextCloser(&comma_tok), ParseException);

  const std::string word_token = "EMPTY";
  StringTokenizer word_tok(word_token);
  EXPECT_THROW(TestableWKTReader::getNextCloser(&word_tok), ParseException);
}

}  // namespace
}  // namespace geos_google
