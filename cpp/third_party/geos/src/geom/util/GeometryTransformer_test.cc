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

// Unit tests for third_party/geos/src/geom/util/GeometryTransformer.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests GeometryTransformer base identity transforms, coordinate transforms,
//   structural pruning/collapse/promotion overrides, interior ring handling,
//   parent pointer propagation, and unsupported-subtype exception handling.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/GeometryTransformer.cpp

#include "third_party/geos/include/geos/geom/util/GeometryTransformer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
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
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::util::GeometryTransformer;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;

struct IdentityCase {
  std::string_view name;
  std::string_view wkt;
  GeometryTypeId expected_type;
  bool expected_empty;
};

constexpr auto kIdentityCases = std::to_array<IdentityCase>({
    {.name = "point",
     .wkt = "POINT (1 2)",
     .expected_type = geos::geom::GEOS_POINT,
     .expected_empty = false},
    {.name = "point_empty",
     .wkt = "POINT EMPTY",
     .expected_type = geos::geom::GEOS_POINT,
     .expected_empty = true},
    {.name = "multipoint",
     .wkt = "MULTIPOINT ((1 2), (3 4))",
     .expected_type = geos::geom::GEOS_MULTIPOINT,
     .expected_empty = false},
    {.name = "multipoint_empty",
     .wkt = "MULTIPOINT EMPTY",
     .expected_type = geos::geom::GEOS_MULTIPOINT,
     .expected_empty = true},
    {.name = "linearring",
     .wkt = "LINEARRING (0 0, 2 0, 2 2, 0 0)",
     .expected_type = geos::geom::GEOS_LINEARRING,
     .expected_empty = false},
    {.name = "linearring_empty",
     .wkt = "LINEARRING EMPTY",
     .expected_type = geos::geom::GEOS_LINEARRING,
     .expected_empty = true},
    {.name = "linestring",
     .wkt = "LINESTRING (0 0, 1 2, 3 4)",
     .expected_type = geos::geom::GEOS_LINESTRING,
     .expected_empty = false},
    {.name = "linestring_empty",
     .wkt = "LINESTRING EMPTY",
     .expected_type = geos::geom::GEOS_LINESTRING,
     .expected_empty = true},
    {.name = "multilinestring",
     .wkt = "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
     .expected_type = geos::geom::GEOS_MULTILINESTRING,
     .expected_empty = false},
    {.name = "multilinestring_empty",
     .wkt = "MULTILINESTRING EMPTY",
     .expected_type = geos::geom::GEOS_MULTILINESTRING,
     .expected_empty = true},
    {.name = "polygon_simple",
     .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
     .expected_type = geos::geom::GEOS_POLYGON,
     .expected_empty = false},
    {.name = "polygon_with_holes",
     .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
            "(1 1, 1 3, 3 3, 3 1, 1 1), (5 5, 5 7, 7 7, 7 5, 5 5))",
     .expected_type = geos::geom::GEOS_POLYGON,
     .expected_empty = false},
    {.name = "multipolygon",
     .wkt = "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
            "((5 5, 8 5, 8 8, 5 8, 5 5), (6 6, 6 7, 7 7, 7 6, 6 6)))",
     .expected_type = geos::geom::GEOS_MULTIPOLYGON,
     .expected_empty = false},
    {.name = "multipolygon_empty",
     .wkt = "MULTIPOLYGON EMPTY",
     .expected_type = geos::geom::GEOS_MULTIPOLYGON,
     .expected_empty = true},
    {.name = "geometrycollection",
     .wkt = "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 3 3), "
            "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))",
     .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
     .expected_empty = false},
    {.name = "geometrycollection_empty",
     .wkt = "GEOMETRYCOLLECTION EMPTY",
     .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
     .expected_empty = true},
    {.name = "geometrycollection_nested",
     .wkt = "GEOMETRYCOLLECTION (GEOMETRYCOLLECTION (POINT (1 2)), "
            "MULTIPOINT ((3 4), (5 6)))",
     .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
     .expected_empty = false},
});

TEST(GeometryTransformerTest, DefaultIdentityTransformClonesAllSupportedTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  GeometryTransformer transformer;

  for (const auto& tc : kIdentityCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = reader.read(std::string(tc.wkt));
    ASSERT_NE(input, nullptr);

    Geometry::Ptr output = transformer.transform(input.get());
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output.get(), input.get());
    EXPECT_EQ(output->getFactory(), factory.get());
    EXPECT_EQ(output->getGeometryTypeId(), tc.expected_type);
    EXPECT_EQ(output->isEmpty(), tc.expected_empty);
    EXPECT_TRUE(output->equalsIdentical(input.get()));
  }
}

TEST(GeometryTransformerTest, DefaultTransformOnEmptyPolygonReturnsEmptyRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  GeometryTransformer transformer;

  Geometry::Ptr empty_poly = reader.read("POLYGON EMPTY");
  Geometry::Ptr output = transformer.transform(empty_poly.get());
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->isEmpty());
  // Empty polygon's transformed shell is empty, so transformPolygon falls back
  // to buildGeometry({empty_shell}) which unwraps the single LinearRing.
  EXPECT_EQ(output->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
}

TEST(GeometryTransformerTest,
     DefaultIdentityTransformPreservesHigherDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  GeometryTransformer transformer;

  Geometry::Ptr pt_z = reader.read("POINT Z (1 2 3)");
  Geometry::Ptr out_z = transformer.transform(pt_z.get());
  const auto* pz = dynamic_cast<const Point*>(out_z.get());
  ASSERT_NE(pz, nullptr);
  EXPECT_TRUE(pz->hasZ());
  EXPECT_FALSE(pz->hasM());
  EXPECT_DOUBLE_EQ(pz->getZ(), 3.0);
  EXPECT_TRUE(std::isnan(pz->getM()));

  Geometry::Ptr pt_m = reader.read("POINT M (1 2 4)");
  Geometry::Ptr out_m = transformer.transform(pt_m.get());
  const auto* pm = dynamic_cast<const Point*>(out_m.get());
  ASSERT_NE(pm, nullptr);
  EXPECT_FALSE(pm->hasZ());
  EXPECT_TRUE(pm->hasM());
  EXPECT_TRUE(std::isnan(pm->getZ()));
  EXPECT_DOUBLE_EQ(pm->getM(), 4.0);

  Geometry::Ptr line_zm = reader.read("LINESTRING ZM (0 0 1 2, 3 4 5 6)");
  Geometry::Ptr out_zm = transformer.transform(line_zm.get());
  ASSERT_NE(out_zm, nullptr);
  EXPECT_EQ(out_zm->getCoordinateDimension(), 4);
  EXPECT_TRUE(out_zm->equalsIdentical(line_zm.get()));
}

TEST(GeometryTransformerTest, ThrowsOnUnsupportedCurvedGeometrySubtypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  GeometryTransformer transformer;

  constexpr auto kUnsupportedWkts = std::to_array<std::string_view>({
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 2, 3 1))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "MULTICURVE (CIRCULARSTRING (0 0, 1 1, 2 0))",
  });

  for (std::string_view wkt : kUnsupportedWkts) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = reader.read(std::string(wkt));
    EXPECT_THROW(transformer.transform(geom.get()), IllegalArgumentException);
  }
}

class ShiftCoordinatesTransformer : public GeometryTransformer {
 public:
  ShiftCoordinatesTransformer(double dx, double dy) : dx_(dx), dy_(dy) {}

  const std::vector<const Geometry*>& seen_parents() const {
    return seen_parents_;
  }

 protected:
  CoordinateSequence::Ptr transformCoordinates(
      const CoordinateSequence* coords, const Geometry* parent) override {
    seen_parents_.push_back(parent);
    auto out = coords->clone();
    for (std::size_t i = 0; i < out->size(); ++i) {
      out->setOrdinate(i, CoordinateSequence::X, out->getX(i) + dx_);
      out->setOrdinate(i, CoordinateSequence::Y, out->getY(i) + dy_);
    }
    return out;
  }

 private:
  double dx_;
  double dy_;
  std::vector<const Geometry*> seen_parents_;
};

TEST(GeometryTransformerTest, TransformCoordinatesShiftsAllVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  ShiftCoordinatesTransformer shifter(10.0, -5.0);

  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT ZM (1 2 3 4), "
      "LINESTRING (0 0, 2 3), "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1)))");
  Geometry::Ptr expected = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT ZM (11 -3 3 4), "
      "LINESTRING (10 -5, 12 -2), "
      "POLYGON ((10 -5, 14 -5, 14 -1, 10 -1, 10 -5), "
      "(11 -4, 11 -3, 12 -3, 12 -4, 11 -4)))");

  Geometry::Ptr output = shifter.transform(input.get());
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->equalsIdentical(expected.get()));
  ASSERT_EQ(shifter.seen_parents().size(), 4u);
  for (const Geometry* parent : shifter.seen_parents()) {
    EXPECT_NE(parent, nullptr);
  }
}

struct ParentCall {
  std::string method;
  const Geometry* geom;
  const Geometry* parent;
};

class ParentSpyTransformer : public GeometryTransformer {
 public:
  const std::vector<ParentCall>& calls() const { return calls_; }

 protected:
  Geometry::Ptr transformPoint(const Point* geom,
                               const Geometry* parent) override {
    calls_.push_back({"Point", geom, parent});
    return GeometryTransformer::transformPoint(geom, parent);
  }

  Geometry::Ptr transformMultiPoint(const MultiPoint* geom,
                                    const Geometry* parent) override {
    calls_.push_back({"MultiPoint", geom, parent});
    return GeometryTransformer::transformMultiPoint(geom, parent);
  }

  Geometry::Ptr transformLinearRing(const LinearRing* geom,
                                    const Geometry* parent) override {
    calls_.push_back({"LinearRing", geom, parent});
    return GeometryTransformer::transformLinearRing(geom, parent);
  }

  Geometry::Ptr transformLineString(const LineString* geom,
                                    const Geometry* parent) override {
    calls_.push_back({"LineString", geom, parent});
    return GeometryTransformer::transformLineString(geom, parent);
  }

  Geometry::Ptr transformMultiLineString(const MultiLineString* geom,
                                         const Geometry* parent) override {
    calls_.push_back({"MultiLineString", geom, parent});
    return GeometryTransformer::transformMultiLineString(geom, parent);
  }

  Geometry::Ptr transformPolygon(const Polygon* geom,
                                 const Geometry* parent) override {
    calls_.push_back({"Polygon", geom, parent});
    return GeometryTransformer::transformPolygon(geom, parent);
  }

  Geometry::Ptr transformMultiPolygon(const MultiPolygon* geom,
                                      const Geometry* parent) override {
    calls_.push_back({"MultiPolygon", geom, parent});
    return GeometryTransformer::transformMultiPolygon(geom, parent);
  }

  Geometry::Ptr transformGeometryCollection(const GeometryCollection* geom,
                                            const Geometry* parent) override {
    calls_.push_back({"GeometryCollection", geom, parent});
    return GeometryTransformer::transformGeometryCollection(geom, parent);
  }

 private:
  std::vector<ParentCall> calls_;
};

TEST(GeometryTransformerTest, PropagatesExpectedParentPointers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<MultiPoint> mp =
      reader.read<MultiPoint>("MULTIPOINT ((1 1), (2 2))");
  std::unique_ptr<MultiLineString> mls =
      reader.read<MultiLineString>("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))");
  std::unique_ptr<MultiPolygon> mpoly = reader.read<MultiPolygon>(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1)))");
  std::unique_ptr<GeometryCollection> gc =
      reader.read<GeometryCollection>("GEOMETRYCOLLECTION (POINT (9 9))");

  ParentSpyTransformer spy;
  (void)spy.transform(mp.get());
  (void)spy.transform(mls.get());
  (void)spy.transform(mpoly.get());
  (void)spy.transform(gc.get());

  const auto& calls = spy.calls();
  ASSERT_EQ(calls.size(), 12u);

  // MultiPoint -> Point (parent = mp)
  EXPECT_EQ(calls[0].method, "MultiPoint");
  EXPECT_EQ(calls[0].parent, nullptr);
  EXPECT_EQ(calls[1].method, "Point");
  EXPECT_EQ(calls[1].parent, mp.get());
  EXPECT_EQ(calls[2].method, "Point");
  EXPECT_EQ(calls[2].parent, mp.get());

  // MultiLineString -> LineString (parent = mls)
  EXPECT_EQ(calls[3].method, "MultiLineString");
  EXPECT_EQ(calls[3].parent, nullptr);
  EXPECT_EQ(calls[4].method, "LineString");
  EXPECT_EQ(calls[4].parent, mls.get());
  EXPECT_EQ(calls[5].method, "LineString");
  EXPECT_EQ(calls[5].parent, mls.get());

  // MultiPolygon -> Polygon (parent = mpoly) -> LinearRing (parent = poly)
  const Polygon* poly = mpoly->getGeometryN(0);
  EXPECT_EQ(calls[6].method, "MultiPolygon");
  EXPECT_EQ(calls[6].parent, nullptr);
  EXPECT_EQ(calls[7].method, "Polygon");
  EXPECT_EQ(calls[7].parent, mpoly.get());
  EXPECT_EQ(calls[8].method, "LinearRing");
  EXPECT_EQ(calls[8].parent, poly);
  EXPECT_EQ(calls[9].method, "LinearRing");
  EXPECT_EQ(calls[9].parent, poly);

  // GeometryCollection calls transform() on children, so child parent is null.
  EXPECT_EQ(calls[10].method, "GeometryCollection");
  EXPECT_EQ(calls[10].parent, nullptr);
  EXPECT_EQ(calls[11].method, "Point");
  EXPECT_EQ(calls[11].parent, nullptr);
}

class SequenceTruncatingTransformer : public GeometryTransformer {
 public:
  explicit SequenceTruncatingTransformer(std::optional<std::size_t> max_points)
      : max_points_(max_points) {}

 protected:
  CoordinateSequence::Ptr transformCoordinates(const CoordinateSequence* coords,
                                               const Geometry*) override {
    if (!max_points_.has_value()) {
      return nullptr;
    }
    auto out = std::make_unique<CoordinateSequence>(0u, coords->hasZ(),
                                                    coords->hasM());
    if (*max_points_ > 0 && !coords->isEmpty()) {
      const std::size_t count = std::min(*max_points_, coords->size());
      out->add(*coords, 0, count - 1);
    }
    return out;
  }

 private:
  std::optional<std::size_t> max_points_;
};

TEST(GeometryTransformerTest, TransformLinearRingDowngradesShortSequences) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr ring = reader.read("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 4 0, 4 4)");
  Geometry::Ptr point = reader.read("POINT (1 2)");

  // 2 points and 3 points both satisfy 0 < seqSize < 4, downgrading to
  // LineString.
  SequenceTruncatingTransformer trunc2(2);
  Geometry::Ptr out2 = trunc2.transform(ring.get());
  ASSERT_NE(out2, nullptr);
  EXPECT_EQ(out2->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(out2->getNumPoints(), 2u);

  SequenceTruncatingTransformer trunc3(3);
  Geometry::Ptr out3 = trunc3.transform(ring.get());
  ASSERT_NE(out3, nullptr);
  EXPECT_EQ(out3->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(out3->getNumPoints(), 3u);

  // 0 points stays an empty LinearRing.
  SequenceTruncatingTransformer trunc0(0);
  Geometry::Ptr out0 = trunc0.transform(ring.get());
  ASSERT_NE(out0, nullptr);
  EXPECT_EQ(out0->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(out0->isEmpty());

  // Null CoordinateSequence yields empty geometries without crashing.
  SequenceTruncatingTransformer null_seq(std::nullopt);
  Geometry::Ptr null_ring = null_seq.transform(ring.get());
  EXPECT_EQ(null_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(null_ring->isEmpty());

  Geometry::Ptr null_line = null_seq.transform(line.get());
  EXPECT_EQ(null_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(null_line->isEmpty());

  Geometry::Ptr null_point = null_seq.transform(point.get());
  EXPECT_EQ(null_point->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(null_point->isEmpty());
}

class FilteringPointTransformer : public GeometryTransformer {
 protected:
  Geometry::Ptr transformPoint(const Point* geom,
                               const Geometry* parent) override {
    if (!geom->isEmpty()) {
      const double x = geom->getX();
      if (x < 0.0) {
        return nullptr;
      }
      if (x == 0.0) {
        return factory->createPoint();
      }
      if (x >= 10.0) {
        CoordinateSequence seq{CoordinateXY(x, geom->getY()),
                               CoordinateXY(x + 1.0, geom->getY())};
        return factory->createLineString(seq);
      }
    }
    return GeometryTransformer::transformPoint(geom, parent);
  }
};

TEST(GeometryTransformerTest, TransformMultiPointPrunesNullEmptyAndPromotes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  FilteringPointTransformer transformer;

  // Drops x<0 (null) and x==0 (empty), keeping two points.
  Geometry::Ptr two_left = transformer.transform(
      reader.read("MULTIPOINT ((-1 0), (0 0), (1 2), (3 4))").get());
  EXPECT_EQ(two_left->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(two_left->equalsIdentical(
      reader.read("MULTIPOINT ((1 2), (3 4))").get()));

  // Only one point survives -> collapses to Point.
  Geometry::Ptr one_left = transformer.transform(
      reader.read("MULTIPOINT ((-1 0), (0 0), (2 5))").get());
  EXPECT_EQ(one_left->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(one_left->equalsIdentical(reader.read("POINT (2 5)").get()));

  // All points pruned -> returns MULTIPOINT EMPTY.
  Geometry::Ptr none_left =
      transformer.transform(reader.read("MULTIPOINT ((-1 0), (0 0))").get());
  EXPECT_EQ(none_left->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(none_left->isEmpty());

  // Mixed Point + LineString child outputs -> promotes to GeometryCollection.
  Geometry::Ptr mixed =
      transformer.transform(reader.read("MULTIPOINT ((1 2), (10 0))").get());
  EXPECT_EQ(mixed->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(mixed->equalsIdentical(
      reader.read("GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (10 0, 11 0))")
          .get()));

  // Default transformer also prunes empty Point members from a MultiPoint.
  GeometryTransformer identity;
  Geometry::Ptr with_empty = reader.read("MULTIPOINT (EMPTY, (4 7))");
  Geometry::Ptr pruned = identity.transform(with_empty.get());
  EXPECT_EQ(pruned->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(pruned->equalsIdentical(reader.read("POINT (4 7)").get()));
}

class FilteringLineTransformer : public GeometryTransformer {
 protected:
  Geometry::Ptr transformLineString(const LineString* geom,
                                    const Geometry* parent) override {
    if (!geom->isEmpty()) {
      const double x = geom->getCoordinate()->x;
      if (x < 0.0) {
        return nullptr;
      }
      if (x == 0.0) {
        return factory->createLineString();
      }
      if (x >= 10.0) {
        return factory->createPoint(*geom->getCoordinate());
      }
    }
    return GeometryTransformer::transformLineString(geom, parent);
  }
};

TEST(GeometryTransformerTest,
     TransformMultiLineStringPrunesNullEmptyAndPromotes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  FilteringLineTransformer transformer;

  Geometry::Ptr two_left =
      transformer.transform(reader
                                .read("MULTILINESTRING ((-5 0, -4 0), (0 0, 1 "
                                      "0), (2 0, 3 0), (4 0, 5 0))")
                                .get());
  EXPECT_EQ(two_left->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(two_left->equalsIdentical(
      reader.read("MULTILINESTRING ((2 0, 3 0), (4 0, 5 0))").get()));

  Geometry::Ptr one_left = transformer.transform(
      reader.read("MULTILINESTRING ((-5 0, -4 0), (0 0, 1 0), (2 0, 3 0))")
          .get());
  EXPECT_EQ(one_left->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(
      one_left->equalsIdentical(reader.read("LINESTRING (2 0, 3 0)").get()));

  Geometry::Ptr none_left = transformer.transform(
      reader.read("MULTILINESTRING ((-5 0, -4 0), (0 0, 1 0))").get());
  EXPECT_EQ(none_left->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(none_left->isEmpty());

  Geometry::Ptr mixed = transformer.transform(
      reader.read("MULTILINESTRING ((2 0, 3 0), (10 5, 11 5))").get());
  EXPECT_EQ(mixed->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(mixed->equalsIdentical(
      reader.read("GEOMETRYCOLLECTION (LINESTRING (2 0, 3 0), POINT (10 5))")
          .get()));
}

// Classifies rings by their first X coordinate to exercise every branch of
// transformPolygon:
//   x <= -10 : returns nullptr
//   x in [-5, 0) : returns empty LinearRing
//   x in [10, 20) : returns LineString (invalid ring type)
//   otherwise : delegates to GeometryTransformer::transformLinearRing
class RingActionTransformer : public GeometryTransformer {
 protected:
  Geometry::Ptr transformLinearRing(const LinearRing* geom,
                                    const Geometry* parent) override {
    if (!geom->isEmpty()) {
      const double x = geom->getCoordinate()->x;
      if (x <= -10.0) {
        return nullptr;
      }
      if (x >= -5.0 && x < 0.0) {
        return factory->createLinearRing();
      }
      if (x >= 10.0 && x < 20.0) {
        CoordinateSequence seq{CoordinateXY(x, 0.0),
                               CoordinateXY(x + 1.0, 0.0)};
        return factory->createLineString(seq);
      }
    }
    return GeometryTransformer::transformLinearRing(geom, parent);
  }
};

TEST(GeometryTransformerTest, TransformPolygonDropsNullAndEmptyHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  RingActionTransformer transformer;

  // Shell x=-9..30 kept; hole0 x=-12 (null), hole1 x=-3 (empty), hole2 x=22
  // (kept).
  Geometry::Ptr poly_with_holes = reader.read(
      "POLYGON ((-9 0, 30 0, 30 10, -9 10, -9 0), "
      "(-12 1, -12 2, -11 2, -11 1, -12 1), "
      "(-3 1, -3 2, -2 2, -2 1, -3 1), "
      "(22 1, 22 2, 23 2, 23 1, 22 1))");

  Geometry::Ptr out = transformer.transform(poly_with_holes.get());
  const auto* out_poly = dynamic_cast<const Polygon*>(out.get());
  ASSERT_NE(out_poly, nullptr);
  EXPECT_TRUE(out_poly->getExteriorRing()->equalsIdentical(
      reader.read("LINEARRING (-9 0, 30 0, 30 10, -9 10, -9 0)").get()));
  ASSERT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_TRUE(out_poly->getInteriorRingN(0)->equalsIdentical(
      reader.read("LINEARRING (22 1, 22 2, 23 2, 23 1, 22 1)").get()));
}

TEST(GeometryTransformerTest,
     TransformPolygonHandlesInvalidShellWithAndWithoutHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  RingActionTransformer transformer;

  // Shell starts at x=10 -> collapses to LineString; no holes -> LineString.
  Geometry::Ptr shell_to_line =
      reader.read("POLYGON ((10 0, 15 0, 15 5, 10 5, 10 0))");
  Geometry::Ptr out_line = transformer.transform(shell_to_line.get());
  EXPECT_EQ(out_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(
      out_line->equalsIdentical(reader.read("LINESTRING (10 0, 11 0)").get()));

  // Shell starts at x=10 -> LineString, plus valid hole LinearRing at x=22 ->
  // GeometryCollection (LineString and LinearRing have distinct
  // GeometryTypeIds).
  Geometry::Ptr shell_to_line_with_hole = reader.read(
      "POLYGON ((10 0, 30 0, 30 10, 10 10, 10 0), "
      "(22 1, 22 2, 23 2, 23 1, 22 1))");
  Geometry::Ptr out_gc = transformer.transform(shell_to_line_with_hole.get());
  EXPECT_EQ(out_gc->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(out_gc->getNumGeometries(), 2u);
  EXPECT_EQ(out_gc->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(out_gc->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);

  // Shell starts at x=-15 -> nullptr; no holes -> GEOMETRYCOLLECTION EMPTY.
  Geometry::Ptr shell_to_null =
      reader.read("POLYGON ((-15 0, -11 0, -11 5, -15 5, -15 0))");
  Geometry::Ptr out_empty_gc = transformer.transform(shell_to_null.get());
  EXPECT_EQ(out_empty_gc->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(out_empty_gc->isEmpty());

  // Shell starts at x=-15 -> nullptr, plus valid hole at x=22 -> LinearRing.
  Geometry::Ptr shell_to_null_with_hole = reader.read(
      "POLYGON ((-15 0, 30 0, 30 10, -15 10, -15 0), "
      "(22 1, 22 2, 23 2, 23 1, 22 1))");
  Geometry::Ptr out_hole_only =
      transformer.transform(shell_to_null_with_hole.get());
  EXPECT_EQ(out_hole_only->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(out_hole_only->equalsIdentical(
      reader.read("LINEARRING (22 1, 22 2, 23 2, 23 1, 22 1)").get()));
}

TEST(GeometryTransformerTest,
     SkipTransformedInvalidInteriorRingsControlsHoleHandling) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  RingActionTransformer transformer;

  // Shell x=0..30 (kept), hole0 x=12 (collapses to LineString), hole1 x=22
  // (kept).
  Geometry::Ptr poly = reader.read(
      "POLYGON ((0 0, 30 0, 30 10, 0 10, 0 0), "
      "(12 1, 12 2, 13 2, 13 1, 12 1), "
      "(22 1, 22 2, 23 2, 23 1, 22 1))");

  // Default (false): invalid collapsed hole invalidates polygon ->
  // MultiLineString of shell + valid hole.
  Geometry::Ptr out_default = transformer.transform(poly.get());
  EXPECT_EQ(out_default->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(out_default->getNumGeometries(), 2u);
  EXPECT_TRUE(out_default->getGeometryN(0)->equalsIdentical(
      reader.read("LINEARRING (0 0, 30 0, 30 10, 0 10, 0 0)").get()));
  EXPECT_TRUE(out_default->getGeometryN(1)->equalsIdentical(
      reader.read("LINEARRING (22 1, 22 2, 23 2, 23 1, 22 1)").get()));

  // Enabled (true): invalid collapsed hole is skipped -> Polygon with 1 hole.
  transformer.setSkipTransformedInvalidInteriorRings(true);
  Geometry::Ptr out_skipped = transformer.transform(poly.get());
  const auto* out_poly = dynamic_cast<const Polygon*>(out_skipped.get());
  ASSERT_NE(out_poly, nullptr);
  ASSERT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_TRUE(out_poly->getInteriorRingN(0)->equalsIdentical(
      reader.read("LINEARRING (22 1, 22 2, 23 2, 23 1, 22 1)").get()));

  // Disabling again restores default behavior.
  transformer.setSkipTransformedInvalidInteriorRings(false);
  Geometry::Ptr out_restored = transformer.transform(poly.get());
  EXPECT_EQ(out_restored->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
}

class FilteringPolygonTransformer : public GeometryTransformer {
 protected:
  Geometry::Ptr transformPolygon(const Polygon* geom,
                                 const Geometry* parent) override {
    if (!geom->isEmpty()) {
      const double x = geom->getCoordinate()->x;
      if (x < 0.0) {
        return nullptr;
      }
      if (x == 0.0) {
        return factory->createPolygon();
      }
      if (x >= 20.0) {
        CoordinateSequence seq{CoordinateXY(x, 0.0),
                               CoordinateXY(x + 2.0, 0.0)};
        return factory->createLineString(seq);
      }
    }
    return GeometryTransformer::transformPolygon(geom, parent);
  }
};

TEST(GeometryTransformerTest, TransformMultiPolygonPrunesNullEmptyAndPromotes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  FilteringPolygonTransformer transformer;

  Geometry::Ptr two_left =
      transformer.transform(reader
                                .read("MULTIPOLYGON ("
                                      "((-5 0, -4 0, -4 1, -5 1, -5 0)), "
                                      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
                                      "((5 0, 7 0, 7 2, 5 2, 5 0)), "
                                      "((10 0, 12 0, 12 2, 10 2, 10 0)))")
                                .get());
  EXPECT_EQ(two_left->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(two_left->getNumGeometries(), 2u);

  Geometry::Ptr one_left =
      transformer.transform(reader
                                .read("MULTIPOLYGON ("
                                      "((-5 0, -4 0, -4 1, -5 1, -5 0)), "
                                      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
                                      "((5 0, 7 0, 7 2, 5 2, 5 0)))")
                                .get());
  EXPECT_EQ(one_left->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(one_left->equalsIdentical(
      reader.read("POLYGON ((5 0, 7 0, 7 2, 5 2, 5 0))").get()));

  Geometry::Ptr none_left =
      transformer.transform(reader
                                .read("MULTIPOLYGON ("
                                      "((-5 0, -4 0, -4 1, -5 1, -5 0)), "
                                      "((0 0, 1 0, 1 1, 0 1, 0 0)))")
                                .get());
  EXPECT_EQ(none_left->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(none_left->isEmpty());

  Geometry::Ptr mixed =
      transformer.transform(reader
                                .read("MULTIPOLYGON ("
                                      "((5 0, 7 0, 7 2, 5 2, 5 0)), "
                                      "((20 0, 22 0, 22 2, 20 2, 20 0)))")
                                .get());
  EXPECT_EQ(mixed->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(
      mixed->equalsIdentical(reader
                                 .read("GEOMETRYCOLLECTION ("
                                       "POLYGON ((5 0, 7 0, 7 2, 5 2, 5 0)), "
                                       "LINESTRING (20 0, 22 0))")
                                 .get()));

  // Default transformer also prunes empty polygon members inside MultiPolygon.
  std::vector<std::unique_ptr<Polygon>> mp_parts;
  mp_parts.push_back(factory->createPolygon());
  mp_parts.push_back(
      reader.read<Polygon>("POLYGON ((5 0, 7 0, 7 2, 5 2, 5 0))"));
  std::unique_ptr<MultiPolygon> mp_with_empty =
      factory->createMultiPolygon(std::move(mp_parts));
  GeometryTransformer identity;
  Geometry::Ptr mp_pruned = identity.transform(mp_with_empty.get());
  EXPECT_EQ(mp_pruned->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(mp_pruned->equalsIdentical(
      reader.read("POLYGON ((5 0, 7 0, 7 2, 5 2, 5 0))").get()));
}

TEST(GeometryTransformerTest, TransformGeometryCollectionPrunesNullAndEmpty) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  FilteringPointTransformer transformer;

  // Drops POINT (-1 0) via nullptr, prunes POINT EMPTY / LINESTRING EMPTY /
  // nested GEOMETRYCOLLECTION (POINT EMPTY), and preserves GeometryCollection
  // wrapper around the single surviving POINT (3 4).
  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT (-1 0), "
      "POINT EMPTY, "
      "LINESTRING EMPTY, "
      "GEOMETRYCOLLECTION (POINT EMPTY), "
      "POINT (3 4))");
  Geometry::Ptr out = transformer.transform(input.get());
  EXPECT_EQ(out->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(out->getNumGeometries(), 1u);
  EXPECT_TRUE(out->equalsIdentical(
      reader.read("GEOMETRYCOLLECTION (POINT (3 4))").get()));

  Geometry::Ptr all_pruned = transformer.transform(
      reader.read("GEOMETRYCOLLECTION (POINT (-1 0), POINT EMPTY)").get());
  EXPECT_EQ(all_pruned->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(all_pruned->isEmpty());
}

TEST(GeometryTransformerTest, ReusesTransformerAcrossDifferentFactories) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr factory_a = GeometryFactory::create(&pm, 4326);
  GeometryFactory::Ptr factory_b = GeometryFactory::create(&pm, 3857);
  WKTReader reader_a(factory_a.get());
  WKTReader reader_b(factory_b.get());

  Geometry::Ptr geom_a = reader_a.read("POINT (1 2)");
  Geometry::Ptr geom_b = reader_b.read("LINESTRING (0 0, 10 10)");

  GeometryTransformer transformer;
  Geometry::Ptr out_a = transformer.transform(geom_a.get());
  Geometry::Ptr out_b = transformer.transform(geom_b.get());

  EXPECT_EQ(out_a->getFactory(), factory_a.get());
  EXPECT_EQ(out_a->getSRID(), 4326);
  EXPECT_EQ(out_b->getFactory(), factory_b.get());
  EXPECT_EQ(out_b->getSRID(), 3857);
}

}  // namespace
}  // namespace geos_google
