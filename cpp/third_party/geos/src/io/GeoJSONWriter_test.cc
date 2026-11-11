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

// Unit tests for third_party/geos/src/io/GeoJSONWriter.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Uses an isolated GeometryFactory::create() per test fixture instance.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/GeoJSONWriter.cpp

#include "third_party/geos/include/geos/io/GeoJSONWriter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/GeoJSON.h"
#include "third_party/geos/include/geos/io/GeoJSONReader.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"
#include "third_party/geos/include/geos/vend/include_nlohmann_json.hpp"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::GeoJSONFeature;
using geos::io::GeoJSONFeatureCollection;
using geos::io::GeoJSONReader;
using geos::io::GeoJSONType;
using geos::io::GeoJSONValue;
using geos::io::GeoJSONWriter;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;
using OrderedJson = geos_nlohmann::ordered_json;

template <typename ExceptionT, typename Fn>
void ExpectThrowsWithMessage(Fn&& fn, std::string_view expected_substr) {
  try {
    fn();
    FAIL() << "Expected exception";
  } catch (const ExceptionT& e) {
    EXPECT_NE(std::string_view(e.what()).find(expected_substr),
              std::string_view::npos)
        << "Unexpected exception message: " << e.what();
  }
}

std::vector<std::string> ObjectKeys(const OrderedJson& obj) {
  std::vector<std::string> keys;
  for (const auto& item : obj.items()) {
    keys.push_back(item.key());
  }
  return keys;
}

class GeoJSONWriterTest : public ::testing::Test {
 protected:
  Geometry::Ptr ReadWkt(const std::string& wkt) const {
    return wkt_reader_.read(wkt);
  }

  Geometry::Ptr ReadGeoJson(const std::string& json_text) const {
    return geojson_reader_.read(json_text);
  }

  GeoJSONFeatureCollection ReadGeoJsonFeatures(
      const std::string& json_text) const {
    return geojson_reader_.readFeatures(json_text);
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader wkt_reader_{*factory_};
  GeoJSONReader geojson_reader_{*factory_};
  GeoJSONWriter writer_;
};

TEST_F(GeoJSONWriterTest, OutputDimensionDefaultsValidationAndToggling) {
  EXPECT_EQ(writer_.getOutputDimension(), 3);

  writer_.setOutputDimension(2);
  EXPECT_EQ(writer_.getOutputDimension(), 2);

  writer_.setOutputDimension(3);
  EXPECT_EQ(writer_.getOutputDimension(), 3);

  constexpr auto kInvalidDims = std::to_array<std::uint8_t>({0, 1, 4, 255});
  for (const std::uint8_t dims : kInvalidDims) {
    SCOPED_TRACE(static_cast<int>(dims));
    ExpectThrowsWithMessage<IllegalArgumentException>(
        [&]() { writer_.setOutputDimension(dims); },
        "GeoJSON output dimension must be 2 or 3");
    EXPECT_EQ(writer_.getOutputDimension(), 3);
  }

  writer_.setOutputDimension(2);
  EXPECT_THROW(writer_.setOutputDimension(4), IllegalArgumentException);
  EXPECT_EQ(writer_.getOutputDimension(), 2);
}

TEST_F(GeoJSONWriterTest, Points2D3DMAndEmpty) {
  Geometry::Ptr pt_2d = ReadWkt("POINT (1.25 -2.5)");
  const std::string json_2d = writer_.write(pt_2d.get());
  EXPECT_EQ(json_2d, R"({"type":"Point","coordinates":[1.25,-2.5]})");
  Geometry::Ptr parsed_2d = ReadGeoJson(json_2d);
  ASSERT_NE(dynamic_cast<const Point*>(parsed_2d.get()), nullptr);
  EXPECT_TRUE(parsed_2d->equalsIdentical(pt_2d.get()));

  Geometry::Ptr pt_3d = ReadWkt("POINT Z (1 2 3.5)");
  const std::string json_3d = writer_.write(pt_3d.get());
  EXPECT_EQ(json_3d, R"({"type":"Point","coordinates":[1.0,2.0,3.5]})");
  Geometry::Ptr parsed_3d = ReadGeoJson(json_3d);
  EXPECT_TRUE(parsed_3d->equalsIdentical(pt_3d.get()));

  writer_.setOutputDimension(2);
  const std::string json_3d_as_2d = writer_.write(pt_3d.get());
  EXPECT_EQ(json_3d_as_2d, R"({"type":"Point","coordinates":[1.0,2.0]})");
  Geometry::Ptr parsed_3d_as_2d = ReadGeoJson(json_3d_as_2d);
  EXPECT_FALSE(parsed_3d_as_2d->hasZ());
  EXPECT_TRUE(parsed_3d_as_2d->equalsExact(pt_3d.get()));

  writer_.setOutputDimension(3);
  EXPECT_EQ(writer_.write(pt_3d.get()),
            R"({"type":"Point","coordinates":[1.0,2.0,3.5]})");

  // Measure-only coordinates have NaN Z, so GeoJSON omits the third ordinate.
  Geometry::Ptr pt_m = ReadWkt("POINT M (4 5 9)");
  EXPECT_EQ(writer_.write(pt_m.get()),
            R"({"type":"Point","coordinates":[4.0,5.0]})");

  Geometry::Ptr pt_zm = ReadWkt("POINT ZM (4 5 6.5 9)");
  EXPECT_EQ(writer_.write(pt_zm.get()),
            R"({"type":"Point","coordinates":[4.0,5.0,6.5]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(writer_.write(pt_zm.get()),
            R"({"type":"Point","coordinates":[4.0,5.0]})");
  writer_.setOutputDimension(3);

  std::unique_ptr<Point> pt_nan_z =
      factory_->createPoint(Coordinate(7.0, 8.0, geos::DoubleNotANumber));
  EXPECT_EQ(writer_.write(pt_nan_z.get()),
            R"({"type":"Point","coordinates":[7.0,8.0]})");

  Geometry::Ptr pt_empty = ReadWkt("POINT EMPTY");
  const std::string json_empty = writer_.write(pt_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"Point","coordinates":[]})");
  Geometry::Ptr parsed_empty = ReadGeoJson(json_empty);
  EXPECT_TRUE(parsed_empty->isEmpty());
  EXPECT_EQ(parsed_empty->getGeometryTypeId(), geos::geom::GEOS_POINT);

  Geometry::Ptr pt_z_empty = ReadWkt("POINT Z EMPTY");
  EXPECT_EQ(writer_.write(pt_z_empty.get()),
            R"({"type":"Point","coordinates":[]})");
}

TEST_F(GeoJSONWriterTest, LineStringsAndLinearRings) {
  Geometry::Ptr line_2d = ReadWkt("LINESTRING (0 0, 1.5 2.5, 3 -4)");
  const std::string json_2d = writer_.write(line_2d.get());
  EXPECT_EQ(
      json_2d,
      R"({"type":"LineString","coordinates":[[0.0,0.0],[1.5,2.5],[3.0,-4.0]]})");
  Geometry::Ptr parsed_2d = ReadGeoJson(json_2d);
  ASSERT_NE(dynamic_cast<const LineString*>(parsed_2d.get()), nullptr);
  EXPECT_TRUE(parsed_2d->equalsIdentical(line_2d.get()));

  Geometry::Ptr line_3d = ReadWkt("LINESTRING Z (0 0 10, 1 2 20)");
  EXPECT_EQ(
      writer_.write(line_3d.get()),
      R"({"type":"LineString","coordinates":[[0.0,0.0,10.0],[1.0,2.0,20.0]]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(writer_.write(line_3d.get()),
            R"({"type":"LineString","coordinates":[[0.0,0.0],[1.0,2.0]]})");
  writer_.setOutputDimension(3);

  Geometry::Ptr line_m = ReadWkt("LINESTRING M (0 0 10, 1 2 20)");
  EXPECT_EQ(writer_.write(line_m.get()),
            R"({"type":"LineString","coordinates":[[0.0,0.0],[1.0,2.0]]})");

  Geometry::Ptr line_zm = ReadWkt("LINESTRING ZM (0 0 10 100, 1 2 20 200)");
  EXPECT_EQ(
      writer_.write(line_zm.get()),
      R"({"type":"LineString","coordinates":[[0.0,0.0,10.0],[1.0,2.0,20.0]]})");

  auto mixed_z_seq = std::make_unique<CoordinateSequence>(0u, true, false);
  mixed_z_seq->add(Coordinate(0.0, 0.0, geos::DoubleNotANumber));
  mixed_z_seq->add(Coordinate(1.0, 2.0, 5.0));
  std::unique_ptr<LineString> mixed_z_line =
      factory_->createLineString(std::move(mixed_z_seq));
  EXPECT_EQ(writer_.write(mixed_z_line.get()),
            R"({"type":"LineString","coordinates":[[0.0,0.0],[1.0,2.0,5.0]]})");

  Geometry::Ptr line_empty = ReadWkt("LINESTRING EMPTY");
  const std::string json_empty = writer_.write(line_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"LineString","coordinates":[]})");
  Geometry::Ptr parsed_empty = ReadGeoJson(json_empty);
  EXPECT_TRUE(parsed_empty->isEmpty());
  EXPECT_EQ(parsed_empty->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  // LinearRing is encoded as a GeoJSON LineString.
  Geometry::Ptr ring = ReadWkt("LINEARRING (0 0, 1 0, 1 1, 0 0)");
  ASSERT_EQ(ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  ASSERT_NE(dynamic_cast<const LinearRing*>(ring.get()), nullptr);
  const std::string json_ring = writer_.write(ring.get());
  EXPECT_EQ(
      json_ring,
      R"({"type":"LineString","coordinates":[[0.0,0.0],[1.0,0.0],[1.0,1.0],[0.0,0.0]]})");
  Geometry::Ptr parsed_ring = ReadGeoJson(json_ring);
  EXPECT_EQ(parsed_ring->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(
      parsed_ring->getCoordinates()->equalsIdentical(*ring->getCoordinates()));

  Geometry::Ptr ring_3d = ReadWkt("LINEARRING Z (0 0 1, 1 0 2, 1 1 3, 0 0 1)");
  EXPECT_EQ(
      writer_.write(ring_3d.get()),
      R"({"type":"LineString","coordinates":[[0.0,0.0,1.0],[1.0,0.0,2.0],[1.0,1.0,3.0],[0.0,0.0,1.0]]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(
      writer_.write(ring_3d.get()),
      R"({"type":"LineString","coordinates":[[0.0,0.0],[1.0,0.0],[1.0,1.0],[0.0,0.0]]})");
  writer_.setOutputDimension(3);

  Geometry::Ptr ring_empty = ReadWkt("LINEARRING EMPTY");
  ASSERT_EQ(ring_empty->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(writer_.write(ring_empty.get()),
            R"({"type":"LineString","coordinates":[]})");
}

TEST_F(GeoJSONWriterTest, PolygonsWithAndWithoutHolesAndEmpty) {
  Geometry::Ptr poly_simple =
      ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  const std::string json_simple = writer_.write(poly_simple.get());
  EXPECT_EQ(
      json_simple,
      R"({"type":"Polygon","coordinates":[[[0.0,0.0],[10.0,0.0],[10.0,10.0],[0.0,10.0],[0.0,0.0]]]})");
  Geometry::Ptr parsed_simple = ReadGeoJson(json_simple);
  ASSERT_NE(dynamic_cast<const Polygon*>(parsed_simple.get()), nullptr);
  EXPECT_TRUE(parsed_simple->equalsIdentical(poly_simple.get()));

  Geometry::Ptr poly_holes = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 2 4, 4 4, 4 2, 2 2), "
      "(10 10, 10 12, 12 12, 12 10, 10 10))");
  const std::string json_holes = writer_.write(poly_holes.get());
  EXPECT_EQ(
      json_holes,
      R"({"type":"Polygon","coordinates":[[[0.0,0.0],[20.0,0.0],[20.0,20.0],[0.0,20.0],[0.0,0.0]],[[2.0,2.0],[2.0,4.0],[4.0,4.0],[4.0,2.0],[2.0,2.0]],[[10.0,10.0],[10.0,12.0],[12.0,12.0],[12.0,10.0],[10.0,10.0]]]})");
  Geometry::Ptr parsed_holes = ReadGeoJson(json_holes);
  EXPECT_TRUE(parsed_holes->equalsIdentical(poly_holes.get()));

  Geometry::Ptr poly_3d = ReadWkt(
      "POLYGON Z ((0 0 5, 10 0 5, 10 10 5, 0 10 5, 0 0 5), "
      "(2 2 6, 2 4 6, 4 4 6, 4 2 6, 2 2 6))");
  const std::string json_3d = writer_.write(poly_3d.get());
  EXPECT_EQ(
      json_3d,
      R"({"type":"Polygon","coordinates":[[[0.0,0.0,5.0],[10.0,0.0,5.0],[10.0,10.0,5.0],[0.0,10.0,5.0],[0.0,0.0,5.0]],[[2.0,2.0,6.0],[2.0,4.0,6.0],[4.0,4.0,6.0],[4.0,2.0,6.0],[2.0,2.0,6.0]]]})");
  EXPECT_TRUE(ReadGeoJson(json_3d)->equalsIdentical(poly_3d.get()));

  writer_.setOutputDimension(2);
  EXPECT_EQ(
      writer_.write(poly_3d.get()),
      R"({"type":"Polygon","coordinates":[[[0.0,0.0],[10.0,0.0],[10.0,10.0],[0.0,10.0],[0.0,0.0]],[[2.0,2.0],[2.0,4.0],[4.0,4.0],[4.0,2.0],[2.0,2.0]]]})");
  writer_.setOutputDimension(3);

  Geometry::Ptr poly_empty = ReadWkt("POLYGON EMPTY");
  const std::string json_empty = writer_.write(poly_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"Polygon","coordinates":[[]]})");
  Geometry::Ptr parsed_empty = ReadGeoJson(json_empty);
  EXPECT_TRUE(parsed_empty->isEmpty());
  EXPECT_EQ(parsed_empty->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
}

TEST_F(GeoJSONWriterTest, PolygonForceCCWPreservesInputAndOrientsRings) {
  // CW exterior, hole 0 already CW, hole 1 CCW.
  Geometry::Ptr geom = ReadWkt(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(2 2, 2 4, 4 4, 4 2, 2 2), "
      "(6 6, 8 6, 8 8, 6 8, 6 6))");
  const auto* orig_poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(orig_poly, nullptr);
  ASSERT_FALSE(
      Orientation::isCCW(orig_poly->getExteriorRing()->getCoordinatesRO()));
  ASSERT_FALSE(
      Orientation::isCCW(orig_poly->getInteriorRingN(0)->getCoordinatesRO()));
  ASSERT_TRUE(
      Orientation::isCCW(orig_poly->getInteriorRingN(1)->getCoordinatesRO()));

  const std::string unforced_json = writer_.write(geom.get());
  EXPECT_EQ(
      unforced_json,
      R"({"type":"Polygon","coordinates":[[[0.0,0.0],[0.0,10.0],[10.0,10.0],[10.0,0.0],[0.0,0.0]],[[2.0,2.0],[2.0,4.0],[4.0,4.0],[4.0,2.0],[2.0,2.0]],[[6.0,6.0],[8.0,6.0],[8.0,8.0],[6.0,8.0],[6.0,6.0]]]})");

  writer_.setForceCCW(true);
  const std::string forced_json = writer_.write(geom.get());
  EXPECT_EQ(
      forced_json,
      R"({"type":"Polygon","coordinates":[[[0.0,0.0],[10.0,0.0],[10.0,10.0],[0.0,10.0],[0.0,0.0]],[[2.0,2.0],[2.0,4.0],[4.0,4.0],[4.0,2.0],[2.0,2.0]],[[6.0,6.0],[6.0,8.0],[8.0,8.0],[8.0,6.0],[6.0,6.0]]]})");

  // Input polygon must remain unmutated.
  EXPECT_FALSE(
      Orientation::isCCW(orig_poly->getExteriorRing()->getCoordinatesRO()));
  EXPECT_FALSE(
      Orientation::isCCW(orig_poly->getInteriorRingN(0)->getCoordinatesRO()));
  EXPECT_TRUE(
      Orientation::isCCW(orig_poly->getInteriorRingN(1)->getCoordinatesRO()));

  // Parsed output obeys RFC 7946 right-hand rule (CCW shell, CW holes).
  Geometry::Ptr parsed_forced = ReadGeoJson(forced_json);
  const auto* parsed_poly = dynamic_cast<const Polygon*>(parsed_forced.get());
  ASSERT_NE(parsed_poly, nullptr);
  EXPECT_TRUE(
      Orientation::isCCW(parsed_poly->getExteriorRing()->getCoordinatesRO()));
  ASSERT_EQ(parsed_poly->getNumInteriorRing(), std::size_t{2});
  EXPECT_FALSE(
      Orientation::isCCW(parsed_poly->getInteriorRingN(0)->getCoordinatesRO()));
  EXPECT_FALSE(
      Orientation::isCCW(parsed_poly->getInteriorRingN(1)->getCoordinatesRO()));
  EXPECT_TRUE(parsed_poly->equals(orig_poly));

  // Toggling setForceCCW(false) restores original ring orientation output.
  writer_.setForceCCW(false);
  EXPECT_EQ(writer_.write(geom.get()), unforced_json);

  // Empty polygon with setForceCCW(true) remains safe.
  writer_.setForceCCW(true);
  Geometry::Ptr empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_EQ(writer_.write(empty_poly.get()),
            R"({"type":"Polygon","coordinates":[[]]})");
}

TEST_F(GeoJSONWriterTest, MultiPoints2D3DMixedAndEmpty) {
  Geometry::Ptr mp_2d = ReadWkt("MULTIPOINT ((1 2), (3 4))");
  const std::string json_2d = writer_.write(mp_2d.get());
  EXPECT_EQ(json_2d,
            R"({"type":"MultiPoint","coordinates":[[1.0,2.0],[3.0,4.0]]})");
  Geometry::Ptr parsed_2d = ReadGeoJson(json_2d);
  ASSERT_NE(dynamic_cast<const MultiPoint*>(parsed_2d.get()), nullptr);
  EXPECT_TRUE(parsed_2d->equalsIdentical(mp_2d.get()));

  Geometry::Ptr mp_3d = ReadWkt("MULTIPOINT Z ((1 2 3), (4 5 6))");
  EXPECT_EQ(
      writer_.write(mp_3d.get()),
      R"({"type":"MultiPoint","coordinates":[[1.0,2.0,3.0],[4.0,5.0,6.0]]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(writer_.write(mp_3d.get()),
            R"({"type":"MultiPoint","coordinates":[[1.0,2.0],[4.0,5.0]]})");
  writer_.setOutputDimension(3);

  Geometry::Ptr mp_empty = ReadWkt("MULTIPOINT EMPTY");
  const std::string json_empty = writer_.write(mp_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"MultiPoint","coordinates":[]})");
  EXPECT_TRUE(ReadGeoJson(json_empty)->isEmpty());

  Geometry::Ptr mp_with_empty = ReadWkt("MULTIPOINT ((1 2), EMPTY, (3 4))");
  EXPECT_EQ(writer_.write(mp_with_empty.get()),
            R"({"type":"MultiPoint","coordinates":[[1.0,2.0],[3.0,4.0]]})");

  std::vector<std::unique_ptr<Point>> mixed_pts;
  mixed_pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  mixed_pts.push_back(factory_->createPoint(Coordinate(3.0, 4.0, 5.0)));
  std::unique_ptr<MultiPoint> mixed_mp =
      factory_->createMultiPoint(std::move(mixed_pts));
  EXPECT_EQ(writer_.write(mixed_mp.get()),
            R"({"type":"MultiPoint","coordinates":[[1.0,2.0],[3.0,4.0,5.0]]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(writer_.write(mixed_mp.get()),
            R"({"type":"MultiPoint","coordinates":[[1.0,2.0],[3.0,4.0]]})");
}

TEST_F(GeoJSONWriterTest, MultiLineStrings2D3DAndEmpty) {
  Geometry::Ptr mls_2d = ReadWkt("MULTILINESTRING ((0 0, 1 1), (2 2, 3 4))");
  const std::string json_2d = writer_.write(mls_2d.get());
  EXPECT_EQ(
      json_2d,
      R"({"type":"MultiLineString","coordinates":[[[0.0,0.0],[1.0,1.0]],[[2.0,2.0],[3.0,4.0]]]})");
  Geometry::Ptr parsed_2d = ReadGeoJson(json_2d);
  ASSERT_NE(dynamic_cast<const MultiLineString*>(parsed_2d.get()), nullptr);
  EXPECT_TRUE(parsed_2d->equalsIdentical(mls_2d.get()));

  Geometry::Ptr mls_3d =
      ReadWkt("MULTILINESTRING Z ((0 0 1, 1 1 2), (2 2 3, 3 4 5))");
  EXPECT_EQ(
      writer_.write(mls_3d.get()),
      R"({"type":"MultiLineString","coordinates":[[[0.0,0.0,1.0],[1.0,1.0,2.0]],[[2.0,2.0,3.0],[3.0,4.0,5.0]]]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(
      writer_.write(mls_3d.get()),
      R"({"type":"MultiLineString","coordinates":[[[0.0,0.0],[1.0,1.0]],[[2.0,2.0],[3.0,4.0]]]})");
  writer_.setOutputDimension(3);

  Geometry::Ptr mls_empty = ReadWkt("MULTILINESTRING EMPTY");
  const std::string json_empty = writer_.write(mls_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"MultiLineString","coordinates":[]})");
  EXPECT_TRUE(ReadGeoJson(json_empty)->isEmpty());

  Geometry::Ptr mls_with_empty =
      ReadWkt("MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3))");
  EXPECT_EQ(
      writer_.write(mls_with_empty.get()),
      R"({"type":"MultiLineString","coordinates":[[[0.0,0.0],[1.0,1.0]],[],[[2.0,2.0],[3.0,3.0]]]})");
}

TEST_F(GeoJSONWriterTest, MultiPolygonsForceCCWAndEmpty) {
  Geometry::Ptr mpoly_empty = ReadWkt("MULTIPOLYGON EMPTY");
  const std::string json_empty = writer_.write(mpoly_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"MultiPolygon","coordinates":[]})");
  Geometry::Ptr parsed_empty = ReadGeoJson(json_empty);
  ASSERT_NE(dynamic_cast<const MultiPolygon*>(parsed_empty.get()), nullptr);
  EXPECT_TRUE(parsed_empty->isEmpty());

  Geometry::Ptr mpoly_empty_member = ReadWkt("MULTIPOLYGON (EMPTY)");
  EXPECT_EQ(writer_.write(mpoly_empty_member.get()),
            R"({"type":"MultiPolygon","coordinates":[[[]]]})");

  // Polygon 0: CW shell, CCW hole. Polygon 1: CCW shell, CW hole.
  Geometry::Ptr mpoly = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 0 10, 10 10, 10 0, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2)), "
      "((20 20, 30 20, 30 30, 20 30, 20 20), (22 22, 22 24, 24 24, 24 22, 22 "
      "22))"
      ")");
  const auto* orig_mp = dynamic_cast<const MultiPolygon*>(mpoly.get());
  ASSERT_NE(orig_mp, nullptr);

  const std::string unforced_json = writer_.write(mpoly.get());
  EXPECT_EQ(
      unforced_json,
      R"({"type":"MultiPolygon","coordinates":[[[[0.0,0.0],[0.0,10.0],[10.0,10.0],[10.0,0.0],[0.0,0.0]],[[2.0,2.0],[4.0,2.0],[4.0,4.0],[2.0,4.0],[2.0,2.0]]],[[[20.0,20.0],[30.0,20.0],[30.0,30.0],[20.0,30.0],[20.0,20.0]],[[22.0,22.0],[22.0,24.0],[24.0,24.0],[24.0,22.0],[22.0,22.0]]]]})");
  EXPECT_TRUE(ReadGeoJson(unforced_json)->equalsIdentical(mpoly.get()));

  writer_.setForceCCW(true);
  const std::string forced_json = writer_.write(mpoly.get());
  EXPECT_EQ(
      forced_json,
      R"({"type":"MultiPolygon","coordinates":[[[[0.0,0.0],[10.0,0.0],[10.0,10.0],[0.0,10.0],[0.0,0.0]],[[2.0,2.0],[2.0,4.0],[4.0,4.0],[4.0,2.0],[2.0,2.0]]],[[[20.0,20.0],[30.0,20.0],[30.0,30.0],[20.0,30.0],[20.0,20.0]],[[22.0,22.0],[22.0,24.0],[24.0,24.0],[24.0,22.0],[22.0,22.0]]]]})");

  // Input MultiPolygon must remain unmutated.
  EXPECT_FALSE(Orientation::isCCW(
      orig_mp->getGeometryN(0)->getExteriorRing()->getCoordinatesRO()));
  EXPECT_TRUE(Orientation::isCCW(
      orig_mp->getGeometryN(0)->getInteriorRingN(0)->getCoordinatesRO()));

  Geometry::Ptr parsed_forced = ReadGeoJson(forced_json);
  const auto* parsed_mp =
      dynamic_cast<const MultiPolygon*>(parsed_forced.get());
  ASSERT_NE(parsed_mp, nullptr);
  ASSERT_EQ(parsed_mp->getNumGeometries(), std::size_t{2});
  for (std::size_t i = 0; i < parsed_mp->getNumGeometries(); ++i) {
    const Polygon* poly = parsed_mp->getGeometryN(i);
    EXPECT_TRUE(
        Orientation::isCCW(poly->getExteriorRing()->getCoordinatesRO()));
    ASSERT_EQ(poly->getNumInteriorRing(), std::size_t{1});
    EXPECT_FALSE(
        Orientation::isCCW(poly->getInteriorRingN(0)->getCoordinatesRO()));
  }
  EXPECT_TRUE(parsed_mp->equals(orig_mp));

  // Verify Z coordinates and output dimension=2 on MultiPolygon Z.
  writer_.setForceCCW(false);
  Geometry::Ptr mpoly_3d =
      ReadWkt("MULTIPOLYGON Z (((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1)))");
  EXPECT_EQ(
      writer_.write(mpoly_3d.get()),
      R"({"type":"MultiPolygon","coordinates":[[[[0.0,0.0,1.0],[1.0,0.0,1.0],[1.0,1.0,1.0],[0.0,1.0,1.0],[0.0,0.0,1.0]]]]})");
  writer_.setOutputDimension(2);
  EXPECT_EQ(
      writer_.write(mpoly_3d.get()),
      R"({"type":"MultiPolygon","coordinates":[[[[0.0,0.0],[1.0,0.0],[1.0,1.0],[0.0,1.0],[0.0,0.0]]]]})");
}

TEST_F(GeoJSONWriterTest, GeometryCollectionsNestedAndForceCCW) {
  Geometry::Ptr gc_empty = ReadWkt("GEOMETRYCOLLECTION EMPTY");
  const std::string json_empty = writer_.write(gc_empty.get());
  EXPECT_EQ(json_empty, R"({"type":"GeometryCollection","geometries":[]})");
  Geometry::Ptr parsed_empty = ReadGeoJson(json_empty);
  ASSERT_NE(dynamic_cast<const GeometryCollection*>(parsed_empty.get()),
            nullptr);
  EXPECT_TRUE(parsed_empty->isEmpty());

  Geometry::Ptr gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT Z (1 2 3), "
      "LINESTRING (0 0, 2 2), "
      "POLYGON ((0 0, 0 5, 5 5, 5 0, 0 0)), "
      "MULTIPOINT ((7 8)), "
      "MULTILINESTRING ((1 1, 2 2)), "
      "MULTIPOLYGON (((10 10, 10 12, 12 12, 12 10, 10 10))), "
      "GEOMETRYCOLLECTION (POINT (9 9))"
      ")");

  const std::string json_gc = writer_.write(gc.get());
  const OrderedJson parsed_json = OrderedJson::parse(json_gc);
  EXPECT_EQ(ObjectKeys(parsed_json),
            (std::vector<std::string>{"type", "geometries"}));
  EXPECT_EQ(parsed_json.at("type"), "GeometryCollection");
  ASSERT_TRUE(parsed_json.at("geometries").is_array());
  ASSERT_EQ(parsed_json.at("geometries").size(), std::size_t{7});
  EXPECT_EQ(parsed_json.at("geometries")[0].at("type"), "Point");
  EXPECT_EQ(parsed_json.at("geometries")[0].at("coordinates"),
            OrderedJson::array({1.0, 2.0, 3.0}));
  EXPECT_EQ(parsed_json.at("geometries")[1].at("type"), "LineString");
  EXPECT_EQ(parsed_json.at("geometries")[2].at("type"), "Polygon");
  EXPECT_EQ(parsed_json.at("geometries")[3].at("type"), "MultiPoint");
  EXPECT_EQ(parsed_json.at("geometries")[4].at("type"), "MultiLineString");
  EXPECT_EQ(parsed_json.at("geometries")[5].at("type"), "MultiPolygon");
  EXPECT_EQ(parsed_json.at("geometries")[6].at("type"), "GeometryCollection");
  EXPECT_EQ(parsed_json.at("geometries")[6].at("geometries").size(),
            std::size_t{1});
  EXPECT_TRUE(ReadGeoJson(json_gc)->equalsIdentical(gc.get()));

  writer_.setOutputDimension(2);
  writer_.setForceCCW(true);
  const OrderedJson forced_2d_json =
      OrderedJson::parse(writer_.write(gc.get()));
  EXPECT_EQ(forced_2d_json.at("geometries")[0].at("coordinates"),
            OrderedJson::array({1.0, 2.0}));
  EXPECT_EQ(forced_2d_json.at("geometries")[2].at("coordinates")[0][1],
            OrderedJson::array({5.0, 0.0}));
  EXPECT_EQ(forced_2d_json.at("geometries")[5].at("coordinates")[0][0][1],
            OrderedJson::array({12.0, 10.0}));
}

TEST_F(GeoJSONWriterTest, GeoJSONTypeModesAndWriteFormatted) {
  Geometry::Ptr pt = ReadWkt("POINT (1 2)");

  EXPECT_EQ(writer_.write(pt.get()),
            writer_.write(pt.get(), GeoJSONType::GEOMETRY));
  EXPECT_EQ(
      writer_.write(pt.get(), GeoJSONType::FEATURE),
      R"({"type":"Feature","geometry":{"type":"Point","coordinates":[1.0,2.0]}})");
  EXPECT_EQ(
      writer_.write(pt.get(), GeoJSONType::FEATURE_COLLECTION),
      R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[1.0,2.0]}}]})");

  EXPECT_EQ(writer_.writeFormatted(pt.get()),
            "{\n"
            "    \"type\": \"Point\",\n"
            "    \"coordinates\": [\n"
            "        1.0,\n"
            "        2.0\n"
            "    ]\n"
            "}");

  EXPECT_EQ(writer_.writeFormatted(pt.get(), GeoJSONType::FEATURE, 2),
            "{\n"
            "  \"type\": \"Feature\",\n"
            "  \"geometry\": {\n"
            "    \"type\": \"Point\",\n"
            "    \"coordinates\": [\n"
            "      1.0,\n"
            "      2.0\n"
            "    ]\n"
            "  }\n"
            "}");

  EXPECT_EQ(
      writer_.writeFormatted(pt.get(), GeoJSONType::FEATURE_COLLECTION, 2),
      "{\n"
      "  \"type\": \"FeatureCollection\",\n"
      "  \"features\": [\n"
      "    {\n"
      "      \"type\": \"Feature\",\n"
      "      \"geometry\": {\n"
      "        \"type\": \"Point\",\n"
      "        \"coordinates\": [\n"
      "          1.0,\n"
      "          2.0\n"
      "        ]\n"
      "      }\n"
      "    }\n"
      "  ]\n"
      "}");

  EXPECT_EQ(writer_.writeFormatted(pt.get(), GeoJSONType::GEOMETRY, 0),
            "{\n"
            "\"type\": \"Point\",\n"
            "\"coordinates\": [\n"
            "1.0,\n"
            "2.0\n"
            "]\n"
            "}");

  EXPECT_EQ(writer_.writeFormatted(pt.get(), GeoJSONType::GEOMETRY, -1),
            writer_.write(pt.get()));

  // writeFormatted honors output dimension and forceCCW settings.
  Geometry::Ptr cw_poly_3d =
      ReadWkt("POLYGON Z ((0 0 9, 0 3 9, 3 3 9, 3 0 9, 0 0 9))");
  writer_.setOutputDimension(2);
  writer_.setForceCCW(true);
  const OrderedJson formatted_parsed = OrderedJson::parse(
      writer_.writeFormatted(cw_poly_3d.get(), GeoJSONType::FEATURE, 2));
  EXPECT_EQ(formatted_parsed.at("type"), "Feature");
  EXPECT_EQ(formatted_parsed.at("geometry").at("coordinates")[0][1],
            OrderedJson::array({3.0, 0.0}));
}

TEST_F(GeoJSONWriterTest, FeatureWithAllPropertyTypesAndId) {
  std::map<std::string, GeoJSONValue> empty_props;
  GeoJSONFeature minimal_feature(ReadWkt("POINT (1 2)"),
                                 std::move(empty_props));
  EXPECT_EQ(
      writer_.write(minimal_feature),
      R"({"type":"Feature","geometry":{"type":"Point","coordinates":[1.0,2.0]},"properties":{}})");

  std::vector<GeoJSONValue> nested_array;
  nested_array.push_back(GeoJSONValue(2.0));
  nested_array.push_back(GeoJSONValue(std::string("nested")));

  std::map<std::string, GeoJSONValue> obj_in_array;
  obj_in_array.emplace("k", GeoJSONValue(std::string("v")));

  // Array containing all six GeoJSONValue kinds exercises array push_back
  // paths.
  std::vector<GeoJSONValue> mixed_array;
  mixed_array.push_back(GeoJSONValue(1.5));
  mixed_array.push_back(GeoJSONValue(std::string("item")));
  mixed_array.push_back(GeoJSONValue(true));
  mixed_array.push_back(GeoJSONValue(false));
  mixed_array.push_back(GeoJSONValue());
  mixed_array.push_back(GeoJSONValue(nested_array));
  mixed_array.push_back(GeoJSONValue(obj_in_array));

  std::map<std::string, GeoJSONValue> deep_obj;
  deep_obj.emplace("deep", GeoJSONValue(true));

  std::map<std::string, GeoJSONValue> nested_obj;
  nested_obj.emplace(
      "inner_arr", GeoJSONValue(std::vector<GeoJSONValue>{GeoJSONValue(7.0)}));
  nested_obj.emplace("inner_bool", GeoJSONValue(false));
  nested_obj.emplace("inner_null", GeoJSONValue());
  nested_obj.emplace("inner_num", GeoJSONValue(9.5));
  nested_obj.emplace("inner_obj", GeoJSONValue(deep_obj));
  nested_obj.emplace("inner_str", GeoJSONValue(std::string("ok")));

  std::map<std::string, GeoJSONValue> props;
  props.emplace("a_bool_false", GeoJSONValue(false));
  props.emplace("b_bool_true", GeoJSONValue(true));
  props.emplace("c_null", GeoJSONValue());
  props.emplace("d_num_int", GeoJSONValue(42.0));
  props.emplace("e_num_neg", GeoJSONValue(-3.25));
  props.emplace("f_str", GeoJSONValue(std::string(R"(hello "world")")));
  props.emplace("g_empty_arr", GeoJSONValue(std::vector<GeoJSONValue>{}));
  props.emplace("h_empty_obj",
                GeoJSONValue(std::map<std::string, GeoJSONValue>{}));
  props.emplace("i_mixed_arr", GeoJSONValue(mixed_array));
  props.emplace("j_nested_obj", GeoJSONValue(nested_obj));

  GeoJSONFeature full_feature(ReadWkt("POINT Z (10 20 30)"), std::move(props),
                              std::string("feat-42"));
  const std::string feature_json = writer_.write(full_feature);
  const OrderedJson parsed = OrderedJson::parse(feature_json);
  EXPECT_EQ(ObjectKeys(parsed),
            (std::vector<std::string>{"type", "id", "geometry", "properties"}));
  EXPECT_EQ(parsed.at("type"), "Feature");
  EXPECT_EQ(parsed.at("id"), "feat-42");
  EXPECT_EQ(parsed.at("geometry").at("coordinates"),
            OrderedJson::array({10.0, 20.0, 30.0}));

  const OrderedJson& jprops = parsed.at("properties");
  EXPECT_EQ(jprops.at("a_bool_false"), false);
  EXPECT_EQ(jprops.at("b_bool_true"), true);
  EXPECT_TRUE(jprops.at("c_null").is_null());
  EXPECT_DOUBLE_EQ(jprops.at("d_num_int").get<double>(), 42.0);
  EXPECT_DOUBLE_EQ(jprops.at("e_num_neg").get<double>(), -3.25);
  EXPECT_EQ(jprops.at("f_str"), R"(hello "world")");
  EXPECT_TRUE(jprops.at("g_empty_arr").is_array());
  EXPECT_TRUE(jprops.at("g_empty_arr").empty());
  EXPECT_TRUE(jprops.at("h_empty_obj").is_object());
  EXPECT_TRUE(jprops.at("h_empty_obj").empty());

  const OrderedJson& jarr = jprops.at("i_mixed_arr");
  ASSERT_EQ(jarr.size(), std::size_t{7});
  EXPECT_DOUBLE_EQ(jarr[0].get<double>(), 1.5);
  EXPECT_EQ(jarr[1], "item");
  EXPECT_EQ(jarr[2], true);
  EXPECT_EQ(jarr[3], false);
  EXPECT_TRUE(jarr[4].is_null());
  EXPECT_DOUBLE_EQ(jarr[5][0].get<double>(), 2.0);
  EXPECT_EQ(jarr[5][1], "nested");
  EXPECT_EQ(jarr[6].at("k"), "v");

  const OrderedJson& jobj = jprops.at("j_nested_obj");
  EXPECT_DOUBLE_EQ(jobj.at("inner_arr")[0].get<double>(), 7.0);
  EXPECT_EQ(jobj.at("inner_bool"), false);
  EXPECT_TRUE(jobj.at("inner_null").is_null());
  EXPECT_DOUBLE_EQ(jobj.at("inner_num").get<double>(), 9.5);
  EXPECT_EQ(jobj.at("inner_obj").at("deep"), true);
  EXPECT_EQ(jobj.at("inner_str"), "ok");

  GeoJSONFeatureCollection round_trip = ReadGeoJsonFeatures(feature_json);
  ASSERT_EQ(round_trip.getFeatures().size(), std::size_t{1});
  const GeoJSONFeature& decoded = round_trip.getFeatures()[0];
  EXPECT_EQ(decoded.getId(), "feat-42");
  EXPECT_TRUE(
      decoded.getGeometry()->equalsIdentical(full_feature.getGeometry()));
  EXPECT_DOUBLE_EQ(decoded.getProperties().at("d_num_int").getNumber(), 42.0);
  EXPECT_EQ(decoded.getProperties().at("f_str").getString(),
            R"(hello "world")");
  EXPECT_TRUE(decoded.getProperties().at("c_null").isNull());
  EXPECT_TRUE(decoded.getProperties().at("b_bool_true").getBoolean());
}

TEST_F(GeoJSONWriterTest, FeatureCollectionEmptyAndMultipleFeatures) {
  GeoJSONFeatureCollection empty_fc(std::vector<GeoJSONFeature>{});
  const std::string empty_json = writer_.write(empty_fc);
  EXPECT_EQ(empty_json, R"({"type":"FeatureCollection","features":[]})");
  EXPECT_TRUE(ReadGeoJsonFeatures(empty_json).getFeatures().empty());

  std::vector<GeoJSONFeature> features;
  {
    std::map<std::string, GeoJSONValue> p0;
    p0.emplace("label", GeoJSONValue(std::string("alpha")));
    features.emplace_back(ReadWkt("POINT Z (1 2 3)"), std::move(p0),
                          std::string("f0"));
  }
  {
    std::map<std::string, GeoJSONValue> p1;
    p1.emplace("weight", GeoJSONValue(12.5));
    features.emplace_back(ReadWkt("LINESTRING (0 0, 5 5)"), std::move(p1));
  }
  {
    std::map<std::string, GeoJSONValue> p2;
    features.emplace_back(ReadWkt("POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0))"),
                          std::move(p2), std::string("f2"));
  }
  GeoJSONFeatureCollection fc(std::move(features));

  writer_.setOutputDimension(2);
  writer_.setForceCCW(true);
  const std::string fc_json = writer_.write(fc);
  const OrderedJson parsed = OrderedJson::parse(fc_json);
  EXPECT_EQ(ObjectKeys(parsed), (std::vector<std::string>{"type", "features"}));
  EXPECT_EQ(parsed.at("type"), "FeatureCollection");
  ASSERT_EQ(parsed.at("features").size(), std::size_t{3});

  EXPECT_EQ(parsed.at("features")[0].at("id"), "f0");
  EXPECT_EQ(parsed.at("features")[0].at("geometry").at("coordinates"),
            OrderedJson::array({1.0, 2.0}));
  EXPECT_EQ(parsed.at("features")[0].at("properties").at("label"), "alpha");

  EXPECT_FALSE(parsed.at("features")[1].contains("id"));
  EXPECT_DOUBLE_EQ(
      parsed.at("features")[1].at("properties").at("weight").get<double>(),
      12.5);

  EXPECT_EQ(parsed.at("features")[2].at("id"), "f2");
  EXPECT_EQ(parsed.at("features")[2].at("geometry").at("coordinates")[0][1],
            OrderedJson::array({4.0, 0.0}));
  EXPECT_TRUE(parsed.at("features")[2].at("properties").empty());

  GeoJSONFeatureCollection round_trip = ReadGeoJsonFeatures(fc_json);
  ASSERT_EQ(round_trip.getFeatures().size(), std::size_t{3});
  EXPECT_EQ(round_trip.getFeatures()[0].getId(), "f0");
  EXPECT_EQ(round_trip.getFeatures()[1].getId(), "");
  EXPECT_EQ(round_trip.getFeatures()[2].getId(), "f2");
}

TEST_F(GeoJSONWriterTest, CurvedGeometriesThrowUnsupportedOperation) {
  struct CurvedCase {
    std::string_view name;
    std::string_view wkt;
  };
  constexpr auto kCurvedCases = std::to_array<CurvedCase>({
      {
          .name = "circular_string",
          .wkt = "CIRCULARSTRING (0 0, 1 1, 2 0)",
      },
      {
          .name = "compound_curve",
          .wkt = "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))",
      },
      {
          .name = "curve_polygon",
          .wkt = "CURVEPOLYGON (CIRCULARSTRING (0 0, 2 0, 2 2, 0 2, 0 0))",
      },
      {
          .name = "multi_curve",
          .wkt = "MULTICURVE (CIRCULARSTRING (0 0, 1 1, 2 0))",
      },
      {
          .name = "multi_surface",
          .wkt = "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 2 0, 2 2, 0 "
                 "2, 0 0)))",
      },
      {
          .name = "geometry_collection_with_curve",
          .wkt = "GEOMETRYCOLLECTION (POINT (1 2), CIRCULARSTRING (0 0, 1 1, 2 "
                 "0))",
      },
  });

  for (const auto& test_case : kCurvedCases) {
    SCOPED_TRACE(test_case.name);
    Geometry::Ptr geom = ReadWkt(std::string(test_case.wkt));
    ASSERT_NE(geom, nullptr);
    ExpectThrowsWithMessage<UnsupportedOperationException>(
        [&]() { static_cast<void>(writer_.write(geom.get())); },
        "Curved geometry types are not supported");
    EXPECT_THROW(
        static_cast<void>(writer_.write(geom.get(), GeoJSONType::FEATURE)),
        UnsupportedOperationException);
    EXPECT_THROW(static_cast<void>(writer_.write(
                     geom.get(), GeoJSONType::FEATURE_COLLECTION)),
                 UnsupportedOperationException);
    EXPECT_THROW(static_cast<void>(writer_.writeFormatted(geom.get())),
                 UnsupportedOperationException);

    GeoJSONFeature feature(std::move(geom),
                           std::map<std::string, GeoJSONValue>{});
    EXPECT_THROW(static_cast<void>(writer_.write(feature)),
                 UnsupportedOperationException);

    std::vector<GeoJSONFeature> vec;
    vec.push_back(std::move(feature));
    GeoJSONFeatureCollection fc(std::move(vec));
    EXPECT_THROW(static_cast<void>(writer_.write(fc)),
                 UnsupportedOperationException);
  }
}

}  // namespace
}  // namespace geos_google
