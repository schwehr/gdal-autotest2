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

#include "third_party/geos/include/geos/geom/util/GeometryEditorOperation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/util/CoordinateOperation.h"
#include "third_party/geos/include/geos/geom/util/GeometryEditor.h"
#include "third_party/geos/include/geos/geom/util/NoOpGeometryOperation.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

class ShiftPointOperation final
    : public geos::geom::util::GeometryEditorOperation {
 public:
  ShiftPointOperation(bool* destroyed, double dx, double dy)
      : destroyed_(destroyed), dx_(dx), dy_(dy) {}

  ~ShiftPointOperation() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  std::unique_ptr<geos::geom::Geometry> edit(
      const geos::geom::Geometry* geometry,
      const geos::geom::GeometryFactory* factory) override {
    if (const auto* pt = dynamic_cast<const geos::geom::Point*>(geometry)) {
      if (pt->isEmpty()) {
        return factory->createPoint(pt->getCoordinateDimension());
      }
      const geos::geom::CoordinateXY* coord = pt->getCoordinate();
      return factory->createPoint(
          geos::geom::CoordinateXY(coord->x + dx_, coord->y + dy_));
    }
    return factory->createGeometry(geometry);
  }

 private:
  bool* destroyed_;
  double dx_;
  double dy_;
};

class ImplicitDtorCloneOperation final
    : public geos::geom::util::GeometryEditorOperation {
 public:
  std::unique_ptr<geos::geom::Geometry> edit(
      const geos::geom::Geometry* geometry,
      const geos::geom::GeometryFactory* factory) override {
    return factory->createGeometry(geometry);
  }
};

struct EditInvocation {
  geos::geom::GeometryTypeId type_id;
  const geos::geom::GeometryFactory* factory;
};

class RecordingFactoryOperation final
    : public geos::geom::util::GeometryEditorOperation {
 public:
  std::unique_ptr<geos::geom::Geometry> edit(
      const geos::geom::Geometry* geometry,
      const geos::geom::GeometryFactory* factory) override {
    invocations_.push_back({geometry->getGeometryTypeId(), factory});
    if (const auto* pt = dynamic_cast<const geos::geom::Point*>(geometry)) {
      return factory->createPoint(*pt->getCoordinatesRO());
    }
    if (const auto* lr =
            dynamic_cast<const geos::geom::LinearRing*>(geometry)) {
      return factory->createLinearRing(*lr->getCoordinatesRO());
    }
    if (const auto* ls =
            dynamic_cast<const geos::geom::LineString*>(geometry)) {
      return factory->createLineString(*ls->getCoordinatesRO());
    }
    return geometry->clone();
  }

  const std::vector<EditInvocation>& invocations() const {
    return invocations_;
  }

 private:
  std::vector<EditInvocation> invocations_;
};

class SelectiveEmptyOperation final
    : public geos::geom::util::GeometryEditorOperation {
 public:
  explicit SelectiveEmptyOperation(
      const geos::geom::GeometryFactory* fallback_empty_factory = nullptr)
      : fallback_empty_factory_(fallback_empty_factory) {}

  std::unique_ptr<geos::geom::Geometry> edit(
      const geos::geom::Geometry* geometry,
      const geos::geom::GeometryFactory* factory) override {
    if (const auto* pt = dynamic_cast<const geos::geom::Point*>(geometry)) {
      if (!pt->isEmpty() && pt->getCoordinate()->x < 0.0) {
        return factory->createPoint();
      }
      return factory->createPoint(*pt->getCoordinatesRO());
    }
    if (const auto* lr =
            dynamic_cast<const geos::geom::LinearRing*>(geometry)) {
      if (!lr->isEmpty() && lr->getEnvelopeInternal()->getWidth() <= 2.0) {
        return factory->createLinearRing();
      }
      return factory->createLinearRing(*lr->getCoordinatesRO());
    }
    if (const auto* poly = dynamic_cast<const geos::geom::Polygon*>(geometry)) {
      if (fallback_empty_factory_ != nullptr && poly->getNumPoints() == 0) {
        return fallback_empty_factory_->createPolygon(
            poly->getCoordinateDimension());
      }
      return poly->clone();
    }
    if (const auto* ls =
            dynamic_cast<const geos::geom::LineString*>(geometry)) {
      return factory->createLineString(*ls->getCoordinatesRO());
    }
    return geometry->clone();
  }

 private:
  const geos::geom::GeometryFactory* fallback_empty_factory_;
};

class TranslateCoordinateOperation final
    : public geos::geom::util::CoordinateOperation {
 public:
  TranslateCoordinateOperation(double dx, double dy) : dx_(dx), dy_(dy) {}

  std::unique_ptr<geos::geom::CoordinateSequence> edit(
      const geos::geom::CoordinateSequence* coordinates,
      const geos::geom::Geometry* /*geometry*/) override {
    auto edited = coordinates->clone();
    for (std::size_t i = 0; i < edited->size(); ++i) {
      edited->setOrdinate(i, geos::geom::CoordinateSequence::X,
                          edited->getX(i) + dx_);
      edited->setOrdinate(i, geos::geom::CoordinateSequence::Y,
                          edited->getY(i) + dy_);
    }
    return edited;
  }

 private:
  double dx_;
  double dy_;
};

class GeometryEditorOperationTest : public ::testing::Test {
 protected:
  geos::geom::PrecisionModel pm_;
  geos::geom::GeometryFactory::Ptr factory_ =
      geos::geom::GeometryFactory::create(&pm_, 100);
  geos::io::WKTReader reader_{*factory_};
};

TEST_F(GeometryEditorOperationTest, DirectEditAndPolymorphicDestruction) {
  geos::geom::PrecisionModel other_pm(100.0);
  auto other_factory = geos::geom::GeometryFactory::create(&other_pm, 4326);

  auto pt = reader_.read("POINT (1.5 2.5)");
  auto empty_pt = reader_.read("POINT EMPTY");

  bool heap_destroyed = false;
  {
    std::unique_ptr<geos::geom::util::GeometryEditorOperation> op =
        std::make_unique<ShiftPointOperation>(&heap_destroyed, 10.0, 20.0);
    auto edited = op->edit(pt.get(), other_factory.get());
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->getFactory(), other_factory.get());
    EXPECT_EQ(edited->getSRID(), 4326);
    EXPECT_DOUBLE_EQ(edited->getCoordinate()->x, 11.5);
    EXPECT_DOUBLE_EQ(edited->getCoordinate()->y, 22.5);

    auto edited_empty = op->edit(empty_pt.get(), other_factory.get());
    ASSERT_NE(edited_empty, nullptr);
    EXPECT_TRUE(edited_empty->isEmpty());
    EXPECT_EQ(edited_empty->getFactory(), other_factory.get());
    EXPECT_FALSE(heap_destroyed);
  }
  EXPECT_TRUE(heap_destroyed);

  bool stack_destroyed = false;
  {
    ShiftPointOperation stack_op(&stack_destroyed, -0.5, 1.5);
    geos::geom::util::GeometryEditorOperation* base_op = &stack_op;
    auto edited = base_op->edit(pt.get(), factory_.get());
    ASSERT_NE(edited, nullptr);
    EXPECT_DOUBLE_EQ(edited->getCoordinate()->x, 1.0);
    EXPECT_DOUBLE_EQ(edited->getCoordinate()->y, 4.0);
    EXPECT_FALSE(stack_destroyed);
  }
  EXPECT_TRUE(stack_destroyed);

  std::unique_ptr<geos::geom::util::GeometryEditorOperation> implicit_heap_op =
      std::make_unique<ImplicitDtorCloneOperation>();
  auto cloned = implicit_heap_op->edit(pt.get(), other_factory.get());
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->getFactory(), other_factory.get());
  EXPECT_TRUE(cloned->equalsExact(pt.get()));
  implicit_heap_op.reset();

  ImplicitDtorCloneOperation implicit_stack_op;
  auto cloned_stack = implicit_stack_op.edit(pt.get(), factory_.get());
  ASSERT_NE(cloned_stack, nullptr);
  EXPECT_TRUE(cloned_stack->equalsExact(pt.get()));
}

TEST_F(GeometryEditorOperationTest, GeometryEditorPassesInputOrCustomFactory) {
  auto geom = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 2), "
      "LINESTRING (0 0, 3 4), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2)), "
      "MULTIPOINT ((5 6), (7 8)), "
      "MULTILINESTRING ((1 1, 2 2)), "
      "MULTIPOLYGON (((20 20, 22 20, 22 22, 20 22, 20 20))))");

  {
    geos::geom::util::GeometryEditor default_editor;
    RecordingFactoryOperation op;
    auto edited = default_editor.edit(geom.get(), &op);
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->getFactory(), factory_.get());
    EXPECT_EQ(edited->getSRID(), 100);
    EXPECT_TRUE(edited->equalsExact(geom.get()));
    ASSERT_FALSE(op.invocations().empty());
    for (const auto& call : op.invocations()) {
      EXPECT_EQ(call.factory, factory_.get());
    }
  }

  geos::geom::PrecisionModel target_pm;
  auto target_factory = geos::geom::GeometryFactory::create(&target_pm, 3857);
  {
    geos::geom::util::GeometryEditor custom_editor(target_factory.get());
    RecordingFactoryOperation op;
    auto edited = custom_editor.edit(geom.get(), &op);
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->getFactory(), target_factory.get());
    EXPECT_EQ(edited->getSRID(), 3857);
    EXPECT_TRUE(edited->equalsExact(geom.get()));

    const auto* gc =
        dynamic_cast<const geos::geom::GeometryCollection*>(edited.get());
    ASSERT_NE(gc, nullptr);
    ASSERT_EQ(gc->getNumGeometries(), 6u);
    for (std::size_t i = 0; i < gc->getNumGeometries(); ++i) {
      EXPECT_EQ(gc->getGeometryN(i)->getFactory(), target_factory.get());
      EXPECT_EQ(gc->getGeometryN(i)->getSRID(), 3857);
    }
    for (const auto& call : op.invocations()) {
      EXPECT_EQ(call.factory, target_factory.get());
    }
  }
}

TEST_F(GeometryEditorOperationTest, PropagatesEmptyComponentsAndFactories) {
  auto poly_with_holes = reader_.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(1 1, 2 1, 2 2, 1 2, 1 1), "
      "(4 4, 8 4, 8 8, 4 8, 4 4))");

  SelectiveEmptyOperation op;
  geos::geom::util::GeometryEditor editor(factory_.get());

  auto edited_poly_ptr = editor.edit(poly_with_holes.get(), &op);
  const auto* edited_poly =
      dynamic_cast<const geos::geom::Polygon*>(edited_poly_ptr.get());
  ASSERT_NE(edited_poly, nullptr);
  ASSERT_EQ(edited_poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(
      edited_poly->getInteriorRingN(0)->getEnvelopeInternal()->getWidth(), 4.0);

  auto small_poly = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto collapsed_poly = editor.edit(small_poly.get(), &op);
  EXPECT_TRUE(collapsed_poly->isEmpty());
  EXPECT_EQ(collapsed_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  geos::geom::PrecisionModel other_pm;
  auto other_factory = geos::geom::GeometryFactory::create(&other_pm, 999);
  SelectiveEmptyOperation cross_factory_op(other_factory.get());
  auto empty_poly = reader_.read("POLYGON EMPTY");
  auto edited_empty_poly = editor.edit(empty_poly.get(), &cross_factory_op);
  EXPECT_TRUE(edited_empty_poly->isEmpty());
  EXPECT_EQ(edited_empty_poly->getFactory(), factory_.get());
  EXPECT_EQ(edited_empty_poly->getSRID(), 100);

  auto mixed = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (-1 0), "
      "POINT (3 4), "
      "MULTIPOINT ((-5 1), (2 2)))");
  auto pruned = editor.edit(mixed.get(), &op);
  const auto* pruned_gc =
      dynamic_cast<const geos::geom::GeometryCollection*>(pruned.get());
  ASSERT_NE(pruned_gc, nullptr);
  ASSERT_EQ(pruned_gc->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(pruned_gc->getGeometryN(0)->getCoordinate()->x, 3.0);
  const auto* pruned_mp =
      dynamic_cast<const geos::geom::MultiPoint*>(pruned_gc->getGeometryN(1));
  ASSERT_NE(pruned_mp, nullptr);
  ASSERT_EQ(pruned_mp->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(pruned_mp->getGeometryN(0)->getCoordinate()->x, 2.0);
}

TEST_F(GeometryEditorOperationTest, CoordinateOperationImplementsInterface) {
  TranslateCoordinateOperation coord_op(10.0, -5.0);
  geos::geom::util::GeometryEditorOperation* op = &coord_op;
  geos::geom::util::GeometryEditor editor;

  auto geom = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 2), "
      "POINT EMPTY, "
      "LINESTRING (0 0, 3 4), "
      "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1)))");
  auto edited = editor.edit(geom.get(), op);
  // GeometryEditor::editGeometryCollection omits empty sub-geometries.
  auto expected = reader_.read(
      "GEOMETRYCOLLECTION ("
      "POINT (11 -3), "
      "LINESTRING (10 -5, 13 -1), "
      "POLYGON ((10 -5, 15 -5, 15 0, 10 0, 10 -5), "
      "(11 -4, 12 -4, 12 -3, 11 -3, 11 -4)))");

  ASSERT_NE(edited, nullptr);
  EXPECT_TRUE(edited->equalsExact(expected.get()));
  EXPECT_EQ(edited->getSRID(), 100);

  auto empty_pt = reader_.read("POINT EMPTY");
  auto edited_empty = editor.edit(empty_pt.get(), op);
  ASSERT_NE(edited_empty, nullptr);
  EXPECT_TRUE(edited_empty->isEmpty());
  EXPECT_EQ(edited_empty->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(edited_empty->getSRID(), 100);

  EXPECT_EQ(op->edit(nullptr, factory_.get()), nullptr);
}

TEST_F(GeometryEditorOperationTest, NoOpGeometryOperationImplementsInterface) {
  geos::geom::PrecisionModel target_pm;
  auto target_factory = geos::geom::GeometryFactory::create(&target_pm, 4326);

  auto poly = reader_.read(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))");

  std::unique_ptr<geos::geom::util::GeometryEditorOperation> op =
      std::make_unique<geos::geom::util::NoOpGeometryOperation>();
  auto direct_edited = op->edit(poly.get(), target_factory.get());
  ASSERT_NE(direct_edited, nullptr);
  EXPECT_TRUE(direct_edited->equalsExact(poly.get()));
  EXPECT_EQ(direct_edited->getFactory(), target_factory.get());
  EXPECT_EQ(direct_edited->getSRID(), 4326);

  geos::geom::util::GeometryEditor editor(target_factory.get());
  auto editor_edited = editor.edit(poly.get(), op.get());
  ASSERT_NE(editor_edited, nullptr);
  EXPECT_TRUE(editor_edited->equalsExact(poly.get()));
  EXPECT_EQ(editor_edited->getFactory(), target_factory.get());
  EXPECT_EQ(editor_edited->getSRID(), 4326);

  direct_edited.reset();
  editor_edited.reset();
  op.reset();
}

}  // namespace
}  // namespace geos_google
