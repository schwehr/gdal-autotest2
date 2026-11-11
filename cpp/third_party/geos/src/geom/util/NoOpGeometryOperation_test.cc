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

// Unit tests for third_party/geos/src/geom/util/NoOpGeometryOperation.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/NoOpGeometryOperation.cpp

#include "third_party/geos/include/geos/geom/util/NoOpGeometryOperation.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/util/GeometryEditor.h"
#include "third_party/geos/include/geos/geom/util/GeometryEditorOperation.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::Point;
using geos::geom::PrecisionModel;
using geos::geom::util::GeometryEditor;
using geos::geom::util::GeometryEditorOperation;
using geos::geom::util::NoOpGeometryOperation;
using geos::io::WKTReader;
using geos::util::UnsupportedOperationException;

struct GeometryCase {
  std::string_view name;
  std::string_view wkt;
  GeometryTypeId expected_type;
  bool expected_empty;
};

constexpr auto kGeometryCases = std::to_array<GeometryCase>({
    {.name = "point",
     .wkt = "POINT (1 2)",
     .expected_type = geos::geom::GEOS_POINT,
     .expected_empty = false},
    {.name = "point_empty",
     .wkt = "POINT EMPTY",
     .expected_type = geos::geom::GEOS_POINT,
     .expected_empty = true},
    {.name = "linestring",
     .wkt = "LINESTRING (0 0, 1 2, 3 4)",
     .expected_type = geos::geom::GEOS_LINESTRING,
     .expected_empty = false},
    {.name = "linestring_empty",
     .wkt = "LINESTRING EMPTY",
     .expected_type = geos::geom::GEOS_LINESTRING,
     .expected_empty = true},
    {.name = "linearring",
     .wkt = "LINEARRING (0 0, 2 0, 2 2, 0 0)",
     .expected_type = geos::geom::GEOS_LINEARRING,
     .expected_empty = false},
    {.name = "linearring_empty",
     .wkt = "LINEARRING EMPTY",
     .expected_type = geos::geom::GEOS_LINEARRING,
     .expected_empty = true},
    {.name = "polygon_with_hole",
     .wkt =
         "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))",
     .expected_type = geos::geom::GEOS_POLYGON,
     .expected_empty = false},
    {.name = "polygon_empty",
     .wkt = "POLYGON EMPTY",
     .expected_type = geos::geom::GEOS_POLYGON,
     .expected_empty = true},
    {.name = "multipoint",
     .wkt = "MULTIPOINT ((1 2), (3 4))",
     .expected_type = geos::geom::GEOS_MULTIPOINT,
     .expected_empty = false},
    {.name = "multipoint_empty",
     .wkt = "MULTIPOINT EMPTY",
     .expected_type = geos::geom::GEOS_MULTIPOINT,
     .expected_empty = true},
    {.name = "multilinestring",
     .wkt = "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
     .expected_type = geos::geom::GEOS_MULTILINESTRING,
     .expected_empty = false},
    {.name = "multilinestring_empty",
     .wkt = "MULTILINESTRING EMPTY",
     .expected_type = geos::geom::GEOS_MULTILINESTRING,
     .expected_empty = true},
    {.name = "multipolygon",
     .wkt = "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), ((5 5, 7 5, 7 7, 5 7, "
            "5 5)))",
     .expected_type = geos::geom::GEOS_MULTIPOLYGON,
     .expected_empty = false},
    {.name = "multipolygon_empty",
     .wkt = "MULTIPOLYGON EMPTY",
     .expected_type = geos::geom::GEOS_MULTIPOLYGON,
     .expected_empty = true},
    {.name = "geometrycollection",
     .wkt = "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 3 3), POLYGON "
            "((0 0, 1 0, 1 1, 0 1, 0 0)))",
     .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
     .expected_empty = false},
    {.name = "geometrycollection_empty",
     .wkt = "GEOMETRYCOLLECTION EMPTY",
     .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
     .expected_empty = true},
});

TEST(NoOpGeometryOperationTest, ClonesAllStandardGeometryTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  NoOpGeometryOperation op;

  for (const auto& tc : kGeometryCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr input = reader.read(std::string(tc.wkt));
    ASSERT_NE(input, nullptr);

    Geometry::Ptr cloned = op.edit(input.get(), factory.get());
    ASSERT_NE(cloned, nullptr);
    EXPECT_NE(cloned.get(), input.get());
    EXPECT_EQ(cloned->getFactory(), factory.get());
    EXPECT_EQ(cloned->getGeometryTypeId(), tc.expected_type);
    EXPECT_EQ(cloned->isEmpty(), tc.expected_empty);
    EXPECT_TRUE(cloned->equalsIdentical(input.get()));
  }
}

TEST(NoOpGeometryOperationTest, PreservesHigherDimensionalCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  NoOpGeometryOperation op;

  Geometry::Ptr pt_z = reader.read("POINT Z (1 2 3)");
  Geometry::Ptr cloned_z = op.edit(pt_z.get(), factory.get());
  ASSERT_NE(cloned_z, nullptr);
  const auto* out_z = dynamic_cast<const Point*>(cloned_z.get());
  ASSERT_NE(out_z, nullptr);
  EXPECT_TRUE(out_z->hasZ());
  EXPECT_FALSE(out_z->hasM());
  EXPECT_DOUBLE_EQ(out_z->getZ(), 3.0);
  EXPECT_TRUE(std::isnan(out_z->getM()));
  EXPECT_TRUE(cloned_z->equalsIdentical(pt_z.get()));

  Geometry::Ptr pt_m = reader.read("POINT M (1 2 4)");
  Geometry::Ptr cloned_m = op.edit(pt_m.get(), factory.get());
  ASSERT_NE(cloned_m, nullptr);
  const auto* out_m = dynamic_cast<const Point*>(cloned_m.get());
  ASSERT_NE(out_m, nullptr);
  EXPECT_FALSE(out_m->hasZ());
  EXPECT_TRUE(out_m->hasM());
  EXPECT_TRUE(std::isnan(out_m->getZ()));
  EXPECT_DOUBLE_EQ(out_m->getM(), 4.0);
  EXPECT_TRUE(cloned_m->equalsIdentical(pt_m.get()));

  Geometry::Ptr line_zm = reader.read("LINESTRING ZM (0 0 1 2, 3 4 5 6)");
  Geometry::Ptr cloned_zm = op.edit(line_zm.get(), factory.get());
  ASSERT_NE(cloned_zm, nullptr);
  EXPECT_EQ(cloned_zm->getCoordinateDimension(), 4);
  EXPECT_TRUE(cloned_zm->hasZ());
  EXPECT_TRUE(cloned_zm->hasM());
  EXPECT_TRUE(cloned_zm->equalsIdentical(line_zm.get()));
}

TEST(NoOpGeometryOperationTest, RebindsGeometryToTargetFactory) {
  PrecisionModel src_pm(PrecisionModel::FLOATING);
  PrecisionModel dst_pm(100.0);
  GeometryFactory::Ptr src_factory = GeometryFactory::create(&src_pm, 4326);
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&dst_pm, 3857);

  WKTReader reader(src_factory.get());
  Geometry::Ptr input = reader.read(
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1))");
  ASSERT_EQ(input->getSRID(), 4326);

  NoOpGeometryOperation op;
  Geometry::Ptr result = op.edit(input.get(), dst_factory.get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getFactory(), dst_factory.get());
  EXPECT_EQ(result->getSRID(), 3857);
  EXPECT_EQ(result->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(result->getPrecisionModel()->getScale(), 100.0);
  EXPECT_TRUE(result->equalsIdentical(input.get()));
  EXPECT_EQ(input->getFactory(), src_factory.get());
  EXPECT_EQ(input->getSRID(), 4326);
}

TEST(NoOpGeometryOperationTest, WorksThroughBaseOperationAndGeometryEditor) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel dst_pm(10.0);
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&dst_pm, 32610);

  WKTReader reader(src_factory.get());
  Geometry::Ptr collection = reader.read(
      "GEOMETRYCOLLECTION (POINT (1 2), POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)))");

  std::unique_ptr<GeometryEditorOperation> base_op =
      std::make_unique<NoOpGeometryOperation>();
  GeometryEditor editor(dst_factory.get());
  Geometry::Ptr edited = editor.edit(collection.get(), base_op.get());

  ASSERT_NE(edited, nullptr);
  EXPECT_NE(edited.get(), collection.get());
  EXPECT_EQ(edited->getFactory(), dst_factory.get());
  EXPECT_EQ(edited->getSRID(), 32610);
  EXPECT_TRUE(edited->equalsIdentical(collection.get()));
}

TEST(NoOpGeometryOperationTest, ThrowsOnCurvedGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr curve = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");

  NoOpGeometryOperation op;
  EXPECT_THROW(op.edit(curve.get(), factory.get()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
