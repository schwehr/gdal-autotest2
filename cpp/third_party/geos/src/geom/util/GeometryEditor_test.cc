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

#include "third_party/geos/include/geos/geom/util/GeometryEditor.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/util/CoordinateOperation.h"
#include "third_party/geos/include/geos/geom/util/GeometryEditorOperation.h"
#include "third_party/geos/include/geos/geom/util/NoOpGeometryOperation.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::Point;
using ::geos::geom::Polygon;
using ::geos::geom::PrecisionModel;
using ::geos::geom::util::CoordinateOperation;
using ::geos::geom::util::GeometryEditor;
using ::geos::geom::util::GeometryEditorOperation;
using ::geos::geom::util::NoOpGeometryOperation;
using ::geos::io::WKTReader;

std::unique_ptr<Geometry> ReadWkt(const GeometryFactory* factory,
                                  const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read(wkt);
}

class ShiftCoordinatesOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  ShiftCoordinatesOp(double dx, double dy) : dx_(dx), dy_(dy) {}

  std::unique_ptr<CoordinateSequence> edit(const CoordinateSequence* coords,
                                           const Geometry*) override {
    auto out = std::make_unique<CoordinateSequence>(*coords);
    for (std::size_t i = 0; i < out->size(); ++i) {
      out->setOrdinate(i, CoordinateSequence::X, out->getX(i) + dx_);
      out->setOrdinate(i, CoordinateSequence::Y, out->getY(i) + dy_);
    }
    return out;
  }

 private:
  double dx_;
  double dy_;
};

class DropMatchingCoordOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  explicit DropMatchingCoordOp(std::function<bool(const Geometry*)> predicate)
      : predicate_(std::move(predicate)) {}

  std::unique_ptr<CoordinateSequence> edit(const CoordinateSequence* coords,
                                           const Geometry* geometry) override {
    if (predicate_(geometry)) {
      return std::make_unique<CoordinateSequence>(0u, coords->hasZ(),
                                                  coords->hasM());
    }
    return std::make_unique<CoordinateSequence>(*coords);
  }

 private:
  std::function<bool(const Geometry*)> predicate_;
};

class ReplacePointOp : public GeometryEditorOperation {
 public:
  std::unique_ptr<Geometry> edit(const Geometry* geometry,
                                 const GeometryFactory* factory) override {
    if (const auto* pt = dynamic_cast<const Point*>(geometry)) {
      if (!pt->isEmpty() && pt->getX() < 0.0) {
        return factory->createPoint();
      }
      if (!pt->isEmpty()) {
        return factory->createPoint(
            CoordinateXY(pt->getX() + 10.0, pt->getY() + 20.0));
      }
    }
    return geometry->clone();
  }
};

class EmptyPolygonOp : public GeometryEditorOperation {
 public:
  explicit EmptyPolygonOp(const GeometryFactory* override_factory)
      : override_factory_(override_factory) {}

  std::unique_ptr<Geometry> edit(const Geometry* geometry,
                                 const GeometryFactory* factory) override {
    if (dynamic_cast<const Polygon*>(geometry) != nullptr) {
      const GeometryFactory* target =
          override_factory_ != nullptr ? override_factory_ : factory;
      return target->createPolygon();
    }
    return geometry->clone();
  }

 private:
  const GeometryFactory* override_factory_;
};

TEST(GeometryEditorTest, ThrowsUnsupportedOperationOnCurvedComponents) {
  auto factory = GeometryFactory::create();
  GeometryEditor editor;
  NoOpGeometryOperation noop;

  const std::vector<std::string> curved_wkts = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "CIRCULARSTRING EMPTY",
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "MULTICURVE (CIRCULARSTRING (0 0, 1 1, 2 0))",
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)))",
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))",
  };

  for (const std::string& wkt : curved_wkts) {
    auto geom = ReadWkt(factory.get(), wkt);
    EXPECT_THROW(editor.edit(geom.get(), &noop),
                 geos::util::UnsupportedOperationException)
        << wkt;
  }
}

TEST(GeometryEditorTest, NullFactoryConstructorUsesInputFactory) {
  PrecisionModel pm;
  auto factory = GeometryFactory::create(&pm, 4326);
  GeometryEditor editor(nullptr);
  ShiftCoordinatesOp shift(2.0, -3.0);

  auto pt = ReadWkt(factory.get(), "POINT (1 5)");
  auto res = editor.edit(pt.get(), &shift);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->getFactory(), factory.get());
  EXPECT_EQ(res->getSRID(), 4326);
  ASSERT_NE(res->getCoordinate(), nullptr);
  EXPECT_DOUBLE_EQ(res->getCoordinate()->x, 3.0);
  EXPECT_DOUBLE_EQ(res->getCoordinate()->y, 2.0);
}

TEST(GeometryEditorTest, DefaultConstructorCopiesAndEditsAllGeometryTypes) {
  PrecisionModel pm;
  auto factory = GeometryFactory::create(&pm, 3857);
  ShiftCoordinatesOp shift(1.0, 2.0);
  NoOpGeometryOperation noop;

  const std::vector<std::string> wkts = {
      "POINT (1 2)",
      "LINESTRING (0 0, 3 4)",
      "LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)",
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))",
      "MULTIPOINT ((1 1), (2 2))",
      "MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))",
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((5 5, 7 5, 7 7, 5 7, 5 5)))",
      "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 2 0), "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), GEOMETRYCOLLECTION (POINT (9 9)))",
  };

  for (const std::string& wkt : wkts) {
    auto input = ReadWkt(factory.get(), wkt);

    GeometryEditor noop_editor;
    auto copied = noop_editor.edit(input.get(), &noop);
    ASSERT_NE(copied, nullptr) << wkt;
    EXPECT_EQ(copied->getFactory(), factory.get()) << wkt;
    EXPECT_EQ(copied->getSRID(), 3857) << wkt;
    EXPECT_EQ(copied->getGeometryTypeId(), input->getGeometryTypeId()) << wkt;
    EXPECT_TRUE(copied->equalsExact(input.get(), 0.0)) << wkt;

    GeometryEditor shift_editor;
    auto shifted = shift_editor.edit(input.get(), &shift);
    ASSERT_NE(shifted, nullptr) << wkt;
    EXPECT_EQ(shifted->getFactory(), factory.get()) << wkt;
    EXPECT_EQ(shifted->getGeometryTypeId(), input->getGeometryTypeId()) << wkt;
    const CoordinateXY* orig_c = input->getCoordinate();
    const CoordinateXY* shift_c = shifted->getCoordinate();
    ASSERT_NE(orig_c, nullptr);
    ASSERT_NE(shift_c, nullptr);
    EXPECT_DOUBLE_EQ(shift_c->x, orig_c->x + 1.0);
    EXPECT_DOUBLE_EQ(shift_c->y, orig_c->y + 2.0);
  }
}

TEST(GeometryEditorTest, EmptyAtomicGeometriesPreserveTypeAndTargetFactory) {
  auto src_factory = GeometryFactory::create();
  PrecisionModel dst_pm;
  auto dst_factory = GeometryFactory::create(&dst_pm, 2154);
  GeometryEditor editor(dst_factory.get());
  ShiftCoordinatesOp shift(1.0, 2.0);
  NoOpGeometryOperation noop;

  const std::vector<std::string> empty_atomic_wkts = {
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "LINEARRING EMPTY",
  };

  for (const std::string& wkt : empty_atomic_wkts) {
    auto input = ReadWkt(src_factory.get(), wkt);

    auto shifted = editor.edit(input.get(), &shift);
    ASSERT_NE(shifted, nullptr) << wkt;
    EXPECT_TRUE(shifted->isEmpty()) << wkt;
    EXPECT_EQ(shifted->getGeometryTypeId(), input->getGeometryTypeId()) << wkt;
    EXPECT_EQ(shifted->getFactory(), dst_factory.get()) << wkt;
    EXPECT_EQ(shifted->getSRID(), 2154) << wkt;

    auto copied = editor.edit(input.get(), &noop);
    ASSERT_NE(copied, nullptr) << wkt;
    EXPECT_TRUE(copied->isEmpty()) << wkt;
    EXPECT_EQ(copied->getGeometryTypeId(), input->getGeometryTypeId()) << wkt;
    EXPECT_EQ(copied->getFactory(), dst_factory.get()) << wkt;
    EXPECT_EQ(copied->getSRID(), 2154) << wkt;
  }
}

TEST(GeometryEditorTest, ExplicitFactoryTransfersGeometryToTargetFactory) {
  auto src_factory = GeometryFactory::create();
  PrecisionModel dst_pm(100.0);
  auto dst_factory = GeometryFactory::create(&dst_pm, 4326);

  GeometryEditor editor(dst_factory.get());
  ShiftCoordinatesOp shift(0.5, -0.5);

  auto pt = ReadWkt(src_factory.get(), "POINT (1 2)");
  auto pt_res = editor.edit(pt.get(), &shift);
  EXPECT_EQ(pt_res->getFactory(), dst_factory.get());
  EXPECT_EQ(pt_res->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(pt_res->getCoordinate()->x, 1.5);
  EXPECT_DOUBLE_EQ(pt_res->getCoordinate()->y, 1.5);

  auto line = ReadWkt(src_factory.get(), "LINESTRING (1 2, 3 4)");
  auto line_res = editor.edit(line.get(), &shift);
  EXPECT_EQ(line_res->getFactory(), dst_factory.get());
  EXPECT_EQ(line_res->getSRID(), 4326);
  EXPECT_DOUBLE_EQ(line_res->getCoordinate()->x, 1.5);
  EXPECT_DOUBLE_EQ(line_res->getCoordinate()->y, 1.5);

  auto poly = ReadWkt(
      src_factory.get(),
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))");
  auto poly_res = editor.edit(poly.get(), &shift);
  EXPECT_EQ(poly_res->getFactory(), dst_factory.get());
  EXPECT_EQ(poly_res->getSRID(), 4326);
  const auto* out_poly = dynamic_cast<const Polygon*>(poly_res.get());
  ASSERT_NE(out_poly, nullptr);
  EXPECT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_NEAR(out_poly->getArea(), 96.0, 1e-12);
}

TEST(GeometryEditorTest, EmptyPolygonPreservesTargetFactoryAndDimension) {
  auto src_factory = GeometryFactory::create();
  auto dst_factory = GeometryFactory::create();

  auto empty_poly_2d = ReadWkt(src_factory.get(), "POLYGON EMPTY");
  auto empty_poly_3d = ReadWkt(src_factory.get(), "POLYGON Z EMPTY");
  auto empty_poly_4d = ReadWkt(src_factory.get(), "POLYGON ZM EMPTY");

  ShiftCoordinatesOp coord_op(1.0, 1.0);

  // Same factory: returns newPolygon directly.
  GeometryEditor same_editor(src_factory.get());
  auto same_res = same_editor.edit(empty_poly_2d.get(), &coord_op);
  EXPECT_TRUE(same_res->isEmpty());
  EXPECT_EQ(same_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(same_res->getFactory(), src_factory.get());

  // Different factory: CoordinateOperation::edit returns a clone using
  // src_factory, so editPolygon creates an empty Polygon via dst_factory
  // matching the original coordinate dimension.
  GeometryEditor diff_editor(dst_factory.get());
  auto diff_res_2d = diff_editor.edit(empty_poly_2d.get(), &coord_op);
  EXPECT_TRUE(diff_res_2d->isEmpty());
  EXPECT_EQ(diff_res_2d->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(diff_res_2d->getFactory(), dst_factory.get());
  EXPECT_EQ(diff_res_2d->getCoordinateDimension(), 2u);

  auto diff_res_3d = diff_editor.edit(empty_poly_3d.get(), &coord_op);
  EXPECT_TRUE(diff_res_3d->isEmpty());
  EXPECT_EQ(diff_res_3d->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(diff_res_3d->getFactory(), dst_factory.get());
  EXPECT_EQ(diff_res_3d->getCoordinateDimension(), 3u);
  EXPECT_TRUE(diff_res_3d->hasZ());

  auto diff_res_4d = diff_editor.edit(empty_poly_4d.get(), &coord_op);
  EXPECT_TRUE(diff_res_4d->isEmpty());
  EXPECT_EQ(diff_res_4d->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(diff_res_4d->getFactory(), dst_factory.get());
  EXPECT_EQ(diff_res_4d->getCoordinateDimension(), 4u);
  EXPECT_TRUE(diff_res_4d->hasZ());
  EXPECT_TRUE(diff_res_4d->hasM());

  // Non-empty polygon emptied directly by a custom operation also preserves
  // dst_factory and coordinate dimension when op returns src_factory empty.
  auto poly_3d =
      ReadWkt(src_factory.get(), "POLYGON Z ((0 0 1, 2 0 1, 2 2 1, 0 0 1))");
  EmptyPolygonOp return_src_empty(src_factory.get());
  auto emptied_diff_factory =
      diff_editor.edit(poly_3d.get(), &return_src_empty);
  EXPECT_TRUE(emptied_diff_factory->isEmpty());
  EXPECT_EQ(emptied_diff_factory->getFactory(), dst_factory.get());
  EXPECT_EQ(emptied_diff_factory->getCoordinateDimension(), 3u);

  EmptyPolygonOp return_target_empty(nullptr);
  auto emptied_same_factory =
      diff_editor.edit(poly_3d.get(), &return_target_empty);
  EXPECT_TRUE(emptied_same_factory->isEmpty());
  EXPECT_EQ(emptied_same_factory->getFactory(), dst_factory.get());
}

TEST(GeometryEditorTest, PolygonWithCollapsedShellReturnsEmptyPolygon) {
  auto src_factory = GeometryFactory::create();
  auto dst_factory = GeometryFactory::create();

  auto poly = ReadWkt(
      src_factory.get(),
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))");
  const LinearRing* shell =
      dynamic_cast<const Polygon*>(poly.get())->getExteriorRing();

  // CoordinateOperation::edit(polygon, factory) clones the polygon before
  // editing its exterior ring, so match by coordinates rather than pointer.
  DropMatchingCoordOp drop_shell(
      [shell](const Geometry* g) { return g->equalsExact(shell, 0.0); });

  GeometryEditor editor(src_factory.get());
  auto res = editor.edit(poly.get(), &drop_shell);
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res->isEmpty());
  EXPECT_EQ(res->getFactory(), src_factory.get());
  EXPECT_EQ(res->getCoordinateDimension(), 2u);

  // Also verify coordinate dimension and target factory when collapsing a
  // 3D polygon shell across factories.
  auto poly_3d = ReadWkt(src_factory.get(),
                         "POLYGON Z ((0 0 5, 10 0 5, 10 10 5, 0 10 5, 0 0 5))");
  const LinearRing* shell_3d =
      dynamic_cast<const Polygon*>(poly_3d.get())->getExteriorRing();
  DropMatchingCoordOp drop_shell_3d(
      [shell_3d](const Geometry* g) { return g->equalsExact(shell_3d, 0.0); });

  GeometryEditor diff_editor(dst_factory.get());
  auto res_3d = diff_editor.edit(poly_3d.get(), &drop_shell_3d);
  EXPECT_EQ(res_3d->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_3d->isEmpty());
  EXPECT_EQ(res_3d->getFactory(), dst_factory.get());
  EXPECT_EQ(res_3d->getCoordinateDimension(), 3u);
  EXPECT_TRUE(res_3d->hasZ());
}

TEST(GeometryEditorTest, PolygonDeletesEmptiedHolesAndRetainsOthers) {
  auto factory = GeometryFactory::create();
  auto poly = ReadWkt(factory.get(),
                      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
                      "(2 2, 2 4, 4 4, 4 2, 2 2), "
                      "(10 10, 10 15, 15 15, 15 10, 10 10))");
  const auto* in_poly = dynamic_cast<const Polygon*>(poly.get());
  ASSERT_NE(in_poly, nullptr);
  const LinearRing* shell = in_poly->getExteriorRing();
  const LinearRing* first_hole = in_poly->getInteriorRingN(0);

  // CoordinateOperation::edit(polygon, factory) clones the polygon before
  // editing its interior rings, so match by coordinates rather than pointer.
  DropMatchingCoordOp drop_first_hole([first_hole](const Geometry* g) {
    return g->equalsExact(first_hole, 0.0);
  });

  GeometryEditor editor;
  auto res = editor.edit(poly.get(), &drop_first_hole);
  const auto* out_poly = dynamic_cast<const Polygon*>(res.get());
  ASSERT_NE(out_poly, nullptr);
  ASSERT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_TRUE(out_poly->getInteriorRingN(0)->equalsExact(
      in_poly->getInteriorRingN(1), 0.0));
  EXPECT_NEAR(out_poly->getArea(), 400.0 - 25.0, 1e-12);

  // Deleting all holes leaves only the exterior shell.
  DropMatchingCoordOp drop_all_holes(
      [shell](const Geometry* g) { return !g->equalsExact(shell, 0.0); });
  GeometryEditor drop_all_editor;
  auto no_holes_res = drop_all_editor.edit(poly.get(), &drop_all_holes);
  const auto* no_holes_poly = dynamic_cast<const Polygon*>(no_holes_res.get());
  ASSERT_NE(no_holes_poly, nullptr);
  EXPECT_EQ(no_holes_poly->getNumInteriorRing(), 0u);
  EXPECT_NEAR(no_holes_poly->getArea(), 400.0, 1e-12);
}

TEST(GeometryEditorTest, EmptyCollectionsPreserveTypeAndTargetFactory) {
  auto src_factory = GeometryFactory::create();
  PrecisionModel dst_pm;
  auto dst_factory = GeometryFactory::create(&dst_pm, 32633);
  GeometryEditor editor(dst_factory.get());
  NoOpGeometryOperation noop;

  const std::vector<std::string> empty_collections = {
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  };

  for (const std::string& wkt : empty_collections) {
    auto input = ReadWkt(src_factory.get(), wkt);
    auto res = editor.edit(input.get(), &noop);
    ASSERT_NE(res, nullptr) << wkt;
    EXPECT_TRUE(res->isEmpty()) << wkt;
    EXPECT_EQ(res->getGeometryTypeId(), input->getGeometryTypeId()) << wkt;
    EXPECT_EQ(res->getFactory(), dst_factory.get()) << wkt;
    EXPECT_EQ(res->getSRID(), 32633) << wkt;
  }
}

TEST(GeometryEditorTest, CollectionsDropEmptiedComponentsAcrossAllSubtypes) {
  auto factory = GeometryFactory::create();
  GeometryEditor editor;

  // Drop components whose X coordinate starts at >= 10.
  DropMatchingCoordOp drop_x_ge_10([](const Geometry* g) {
    const CoordinateXY* c = g->getCoordinate();
    return c != nullptr && c->x >= 10.0;
  });

  auto mpt = ReadWkt(factory.get(), "MULTIPOINT ((0 0), (10 10), (2 2))");
  auto mpt_res = editor.edit(mpt.get(), &drop_x_ge_10);
  EXPECT_EQ(mpt_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  ASSERT_EQ(mpt_res->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mpt_res->getGeometryN(0)->getCoordinate()->x, 0.0);
  EXPECT_DOUBLE_EQ(mpt_res->getGeometryN(1)->getCoordinate()->x, 2.0);

  // Dropping all components produces an empty MultiPoint.
  auto mpt_all_dropped_in = ReadWkt(factory.get(), "MULTIPOINT ((10 10))");
  auto mpt_all_dropped = editor.edit(mpt_all_dropped_in.get(), &drop_x_ge_10);
  EXPECT_EQ(mpt_all_dropped->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(mpt_all_dropped->isEmpty());

  auto mls =
      ReadWkt(factory.get(), "MULTILINESTRING ((10 0, 11 0), (0 0, 1 0))");
  auto mls_res = editor.edit(mls.get(), &drop_x_ge_10);
  EXPECT_EQ(mls_res->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(mls_res->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(mls_res->getGeometryN(0)->getCoordinate()->x, 0.0);

  auto mls_all_dropped_in =
      ReadWkt(factory.get(), "MULTILINESTRING ((10 0, 11 0))");
  auto mls_all_dropped = editor.edit(mls_all_dropped_in.get(), &drop_x_ge_10);
  EXPECT_EQ(mls_all_dropped->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(mls_all_dropped->isEmpty());

  auto mpoly = ReadWkt(factory.get(),
                       "MULTIPOLYGON (((10 0, 12 0, 12 2, 10 2, 10 0)), "
                       "((0 0, 2 0, 2 2, 0 2, 0 0)))");
  auto mpoly_res = editor.edit(mpoly.get(), &drop_x_ge_10);
  EXPECT_EQ(mpoly_res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  ASSERT_EQ(mpoly_res->getNumGeometries(), 1u);
  EXPECT_NEAR(mpoly_res->getArea(), 4.0, 1e-12);

  auto mpoly_all_dropped_in =
      ReadWkt(factory.get(), "MULTIPOLYGON (((10 0, 12 0, 12 2, 10 2, 10 0)))");
  auto mpoly_all_dropped =
      editor.edit(mpoly_all_dropped_in.get(), &drop_x_ge_10);
  EXPECT_EQ(mpoly_all_dropped->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(mpoly_all_dropped->isEmpty());

  auto gc =
      ReadWkt(factory.get(),
              "GEOMETRYCOLLECTION (POINT (1 1), POINT (12 12), "
              "LINESTRING (10 0, 15 0), POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0)), "
              "GEOMETRYCOLLECTION (POINT (15 15)))");
  auto gc_res = editor.edit(gc.get(), &drop_x_ge_10);
  EXPECT_EQ(gc_res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(gc_res->getNumGeometries(), 2u);
  EXPECT_EQ(gc_res->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(gc_res->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
}

TEST(GeometryEditorTest,
     CustomGeometryEditorOperationTransformsAndDropsPoints) {
  auto src_factory = GeometryFactory::create();
  auto dst_factory = GeometryFactory::create();
  GeometryEditor editor(dst_factory.get());
  ReplacePointOp op;

  auto gc = ReadWkt(
      src_factory.get(),
      "GEOMETRYCOLLECTION (POINT (-5 0), POINT (1 2), LINESTRING (0 0, 3 3))");
  auto res = editor.edit(gc.get(), &op);
  EXPECT_EQ(res->getFactory(), dst_factory.get());
  ASSERT_EQ(res->getNumGeometries(), 2u);
  EXPECT_EQ(res->getGeometryN(0)->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_DOUBLE_EQ(res->getGeometryN(0)->getCoordinate()->x, 11.0);
  EXPECT_DOUBLE_EQ(res->getGeometryN(0)->getCoordinate()->y, 22.0);
  EXPECT_EQ(res->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
}

}  // namespace
}  // namespace geos_google
