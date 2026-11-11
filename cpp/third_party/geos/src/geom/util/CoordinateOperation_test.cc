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

// Unit tests for third_party/geos/src/geom/util/CoordinateOperation.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/CoordinateOperation.cpp

#include "third_party/geos/include/geos/geom/util/CoordinateOperation.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
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
#include "third_party/geos/include/geos/geom/util/GeometryEditor.h"
#include "third_party/geos/include/geos/geom/util/GeometryEditorOperation.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geom::util::CoordinateOperation;
using geos::geom::util::GeometryEditor;
using geos::geom::util::GeometryEditorOperation;
using geos::util::IllegalArgumentException;
using ::testing::ElementsAre;

static_assert(std::is_abstract_v<CoordinateOperation>);
static_assert(std::is_base_of_v<GeometryEditorOperation, CoordinateOperation>);

class ShiftCoordinatesOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  ShiftCoordinatesOp(double dx, double dy) : dx_(dx), dy_(dy) {}

  std::unique_ptr<CoordinateSequence> edit(const CoordinateSequence* coords,
                                           const Geometry* geometry) override {
    ++call_count;
    last_coords = coords;
    last_geometry = geometry;
    seen_types.push_back(geometry->getGeometryTypeId());

    auto out = coords->clone();
    for (std::size_t i = 0; i < out->size(); ++i) {
      out->setOrdinate(i, CoordinateSequence::X, out->getX(i) + dx_);
      out->setOrdinate(i, CoordinateSequence::Y, out->getY(i) + dy_);
    }
    return out;
  }

  std::size_t call_count = 0;
  const CoordinateSequence* last_coords = nullptr;
  const Geometry* last_geometry = nullptr;
  std::vector<GeometryTypeId> seen_types;

 private:
  double dx_;
  double dy_;
};

class NullSequenceOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  std::unique_ptr<CoordinateSequence> edit(
      const CoordinateSequence* /*coords*/,
      const Geometry* /*geometry*/) override {
    return nullptr;
  }
};

class InvalidRingSequenceOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  std::unique_ptr<CoordinateSequence> edit(
      const CoordinateSequence* /*coords*/,
      const Geometry* /*geometry*/) override {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    seq->add(0.0, 0.0);
    seq->add(1.0, 0.0);
    return seq;
  }
};

class SingleCoordinateSequenceOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  std::unique_ptr<CoordinateSequence> edit(
      const CoordinateSequence* /*coords*/,
      const Geometry* /*geometry*/) override {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    seq->add(2.0, 3.0);
    return seq;
  }
};

class ClearSpecificGeometryOp : public CoordinateOperation {
 public:
  using CoordinateOperation::edit;

  explicit ClearSpecificGeometryOp(const Geometry* target) : target_(target) {}

  std::unique_ptr<CoordinateSequence> edit(const CoordinateSequence* coords,
                                           const Geometry* geometry) override {
    if (geometry != nullptr && target_ != nullptr &&
        geometry->equalsExact(target_, 0.0)) {
      return std::make_unique<CoordinateSequence>(0u, coords->hasZ(),
                                                  coords->hasM());
    }
    return coords->clone();
  }

 private:
  const Geometry* target_;
};

std::unique_ptr<LineString> MakeLineString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createLinearRing(std::move(seq));
}

TEST(CoordinateOperationTest, PolymorphicDestructionWorks) {
  std::unique_ptr<GeometryEditorOperation> base =
      std::make_unique<ShiftCoordinatesOp>(1.0, 2.0);
  EXPECT_NE(base.get(), nullptr);
  base.reset();

  std::unique_ptr<CoordinateOperation> coord_op =
      std::make_unique<ShiftCoordinatesOp>(1.0, 2.0);
  EXPECT_NE(coord_op.get(), nullptr);
  coord_op.reset();
}

TEST(CoordinateOperationTest, NullGeometryReturnsNullptr) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  ShiftCoordinatesOp op(5.0, -3.0);

  EXPECT_EQ(op.edit(nullptr, factory.get()), nullptr);
  EXPECT_EQ(op.edit(static_cast<const Geometry*>(nullptr), nullptr), nullptr);
  EXPECT_EQ(op.call_count, 0u);
}

TEST(CoordinateOperationTest, EditsLinearRingAsLinearRingWithTargetFactory) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel pm;
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&pm, 4326);

  auto ring = MakeRing(*src_factory, {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}});
  ShiftCoordinatesOp op(10.0, 20.0);

  std::unique_ptr<Geometry> edited = op.edit(ring.get(), dst_factory.get());
  ASSERT_NE(edited, nullptr);
  EXPECT_EQ(edited->getGeometryTypeId(), GeometryTypeId::GEOS_LINEARRING);
  EXPECT_EQ(edited->getFactory(), dst_factory.get());
  EXPECT_EQ(edited->getSRID(), 4326);
  EXPECT_EQ(op.call_count, 1u);
  EXPECT_EQ(op.last_geometry, ring.get());
  EXPECT_EQ(op.last_coords, ring->getCoordinatesRO());

  auto expected_ring = MakeRing(
      *dst_factory, {{10, 20}, {14, 20}, {14, 24}, {10, 24}, {10, 20}});
  EXPECT_TRUE(edited->equalsExact(expected_ring.get(), 1e-12));

  // Empty LinearRing also takes the LinearRing branch.
  auto empty_ring = src_factory->createLinearRing();
  std::unique_ptr<Geometry> edited_empty =
      op.edit(empty_ring.get(), dst_factory.get());
  ASSERT_NE(edited_empty, nullptr);
  EXPECT_EQ(edited_empty->getGeometryTypeId(), GeometryTypeId::GEOS_LINEARRING);
  EXPECT_TRUE(edited_empty->isEmpty());
  EXPECT_EQ(edited_empty->getFactory(), dst_factory.get());
  EXPECT_EQ(op.call_count, 2u);

  // Invalid coordinate sequence returned for a LinearRing propagates exception
  // from GeometryFactory::createLinearRing.
  InvalidRingSequenceOp invalid_op;
  EXPECT_THROW(invalid_op.edit(ring.get(), dst_factory.get()),
               IllegalArgumentException);
}

TEST(CoordinateOperationTest, EditsLineStringPreservingZAndMWithTargetFactory) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel pm;
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&pm, 3857);

  auto line_zm = src_factory->createLineString(CoordinateSequence{
      CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
      CoordinateXYZM(5.0, 6.0, 7.0, 8.0),
  });
  ShiftCoordinatesOp op(-1.5, 2.5);

  std::unique_ptr<Geometry> edited = op.edit(line_zm.get(), dst_factory.get());
  ASSERT_NE(edited, nullptr);
  EXPECT_EQ(edited->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_EQ(edited->getFactory(), dst_factory.get());
  EXPECT_EQ(edited->getSRID(), 3857);
  EXPECT_EQ(op.call_count, 1u);
  EXPECT_EQ(op.last_geometry, line_zm.get());
  EXPECT_EQ(op.last_coords, line_zm->getCoordinatesRO());

  const auto* edited_line = dynamic_cast<const LineString*>(edited.get());
  ASSERT_NE(edited_line, nullptr);
  const CoordinateSequence* seq = edited_line->getCoordinatesRO();
  ASSERT_EQ(seq->size(), 2u);
  CoordinateXYZM c0;
  CoordinateXYZM c1;
  seq->getAt(0, c0);
  seq->getAt(1, c1);
  EXPECT_DOUBLE_EQ(c0.x, -0.5);
  EXPECT_DOUBLE_EQ(c0.y, 4.5);
  EXPECT_DOUBLE_EQ(c0.z, 3.0);
  EXPECT_DOUBLE_EQ(c0.m, 4.0);
  EXPECT_DOUBLE_EQ(c1.x, 3.5);
  EXPECT_DOUBLE_EQ(c1.y, 8.5);
  EXPECT_DOUBLE_EQ(c1.z, 7.0);
  EXPECT_DOUBLE_EQ(c1.m, 8.0);

  // Empty LineString also goes through createLineString on dst_factory.
  auto empty_line = src_factory->createLineString();
  std::unique_ptr<Geometry> edited_empty =
      op.edit(empty_line.get(), dst_factory.get());
  ASSERT_NE(edited_empty, nullptr);
  EXPECT_EQ(edited_empty->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(edited_empty->isEmpty());
  EXPECT_EQ(edited_empty->getFactory(), dst_factory.get());

  // Single-point sequence returned for a LineString propagates
  // IllegalArgumentException from GeometryFactory::createLineString.
  SingleCoordinateSequenceOp single_pt_op;
  EXPECT_THROW(single_pt_op.edit(line_zm.get(), dst_factory.get()),
               IllegalArgumentException);
}

TEST(CoordinateOperationTest, EditsPointIncludingEmptyAndXymWithTargetFactory) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel pm;
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&pm, 32610);

  auto pt_xy = src_factory->createPoint(CoordinateXY(2.0, 3.0));
  ShiftCoordinatesOp op(100.0, -50.0);

  std::unique_ptr<Geometry> edited_xy = op.edit(pt_xy.get(), dst_factory.get());
  ASSERT_NE(edited_xy, nullptr);
  EXPECT_EQ(edited_xy->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_EQ(edited_xy->getFactory(), dst_factory.get());
  EXPECT_EQ(edited_xy->getSRID(), 32610);
  EXPECT_EQ(op.call_count, 1u);
  EXPECT_EQ(op.last_geometry, pt_xy.get());
  EXPECT_EQ(op.last_coords, pt_xy->getCoordinatesRO());
  ASSERT_NE(edited_xy->getCoordinate(), nullptr);
  EXPECT_TRUE(edited_xy->getCoordinate()->equals2D(CoordinateXY(102.0, -47.0)));

  // XYM point preserves M and keeps Z as NaN.
  auto pt_xym = src_factory->createPoint(CoordinateXYM(4.0, 5.0, 9.5));
  std::unique_ptr<Geometry> edited_xym =
      op.edit(pt_xym.get(), dst_factory.get());
  ASSERT_NE(edited_xym, nullptr);
  const auto* xym_point = dynamic_cast<const Point*>(edited_xym.get());
  ASSERT_NE(xym_point, nullptr);
  const CoordinateSequence* xym_seq = xym_point->getCoordinatesRO();
  CoordinateXYM out_xym;
  xym_seq->getAt(0, out_xym);
  EXPECT_DOUBLE_EQ(out_xym.x, 104.0);
  EXPECT_DOUBLE_EQ(out_xym.y, -45.0);
  EXPECT_DOUBLE_EQ(out_xym.m, 9.5);
  EXPECT_TRUE(std::isnan(xym_seq->getOrdinate(0, CoordinateSequence::Z)));

  // Empty Point.
  auto empty_pt = src_factory->createPoint();
  std::unique_ptr<Geometry> edited_empty =
      op.edit(empty_pt.get(), dst_factory.get());
  ASSERT_NE(edited_empty, nullptr);
  EXPECT_EQ(edited_empty->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_TRUE(edited_empty->isEmpty());
  EXPECT_EQ(edited_empty->getFactory(), dst_factory.get());

  // Multi-point sequence returned for a Point propagates
  // IllegalArgumentException from GeometryFactory::createPoint.
  InvalidRingSequenceOp two_pt_op;
  EXPECT_THROW(two_pt_op.edit(pt_xy.get(), dst_factory.get()),
               IllegalArgumentException);
}

TEST(CoordinateOperationTest, ClonesNonPointNonLineNonRingGeometries) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel pm;
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&pm, 999);

  auto poly = src_factory->createPolygon(
      MakeRing(*src_factory, {{0, 0}, {5, 0}, {5, 5}, {0, 5}, {0, 0}}));
  std::vector<std::unique_ptr<Point>> mp_pts;
  mp_pts.push_back(src_factory->createPoint(CoordinateXY(1, 2)));
  auto multi_pt = src_factory->createMultiPoint(std::move(mp_pts));
  std::vector<std::unique_ptr<LineString>> mls_lines;
  mls_lines.push_back(MakeLineString(*src_factory, {{0, 0}, {2, 2}}));
  auto multi_line = src_factory->createMultiLineString(std::move(mls_lines));
  std::vector<std::unique_ptr<Polygon>> mpoly_vec;
  mpoly_vec.push_back(src_factory->createPolygon(
      MakeRing(*src_factory, {{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}})));
  auto multi_poly = src_factory->createMultiPolygon(std::move(mpoly_vec));
  std::vector<std::unique_ptr<Geometry>> gc_vec;
  gc_vec.push_back(src_factory->createPoint(CoordinateXY(7, 8)));
  auto gc = src_factory->createGeometryCollection(std::move(gc_vec));
  auto circ_seq = std::make_unique<CoordinateSequence>(0u, false, false);
  circ_seq->add(0.0, 0.0);
  circ_seq->add(1.0, 1.0);
  circ_seq->add(2.0, 0.0);
  auto circ = src_factory->createCircularString(std::move(circ_seq));

  ShiftCoordinatesOp op(10.0, 10.0);
  for (const Geometry* geom : std::initializer_list<const Geometry*>{
           poly.get(), multi_pt.get(), multi_line.get(), multi_poly.get(),
           gc.get(), circ.get()}) {
    std::unique_ptr<Geometry> cloned = op.edit(geom, dst_factory.get());
    ASSERT_NE(cloned, nullptr) << geom->getGeometryType();
    EXPECT_NE(cloned.get(), geom);
    EXPECT_TRUE(cloned->equalsIdentical(geom)) << geom->getGeometryType();
    EXPECT_EQ(cloned->getFactory(), src_factory.get());
  }

  // Fallback clone path does not dereference the target factory.
  std::unique_ptr<Geometry> cloned_null_factory = op.edit(poly.get(), nullptr);
  ASSERT_NE(cloned_null_factory, nullptr);
  EXPECT_TRUE(cloned_null_factory->equalsIdentical(poly.get()));
  EXPECT_EQ(op.call_count, 0u);
}

TEST(CoordinateOperationTest,
     NullReturnedSequenceProducesEmptyAtomicGeometries) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel pm;
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&pm, 7);
  NullSequenceOp null_op;

  auto pt = src_factory->createPoint(CoordinateXY(1.0, 2.0));
  auto line = MakeLineString(*src_factory, {{0.0, 0.0}, {1.0, 1.0}});
  auto ring = MakeRing(*src_factory, {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}});

  auto empty_pt = null_op.edit(pt.get(), dst_factory.get());
  auto empty_line = null_op.edit(line.get(), dst_factory.get());
  auto empty_ring = null_op.edit(ring.get(), dst_factory.get());
  ASSERT_NE(empty_pt, nullptr);
  ASSERT_NE(empty_line, nullptr);
  ASSERT_NE(empty_ring, nullptr);

  EXPECT_EQ(empty_pt->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_TRUE(empty_pt->isEmpty());
  EXPECT_EQ(empty_pt->getFactory(), dst_factory.get());

  EXPECT_EQ(empty_line->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(empty_line->isEmpty());
  EXPECT_EQ(empty_line->getFactory(), dst_factory.get());

  EXPECT_EQ(empty_ring->getGeometryTypeId(), GeometryTypeId::GEOS_LINEARRING);
  EXPECT_TRUE(empty_ring->isEmpty());
  EXPECT_EQ(empty_ring->getFactory(), dst_factory.get());
}

TEST(CoordinateOperationTest, DispatchesPolymorphicallyThroughBaseInterfaces) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  ShiftCoordinatesOp impl(2.0, 3.0);
  CoordinateOperation* as_coord_op = &impl;
  GeometryEditorOperation* as_editor_op = &impl;

  auto pt = factory->createPoint(CoordinateXY(1.0, 4.0));
  auto line = MakeLineString(*factory, {{0.0, 0.0}, {1.0, 2.0}});

  std::unique_ptr<Geometry> edited_pt =
      as_coord_op->edit(pt.get(), factory.get());
  ASSERT_NE(edited_pt, nullptr);
  ASSERT_NE(edited_pt->getCoordinate(), nullptr);
  EXPECT_TRUE(edited_pt->getCoordinate()->equals2D(CoordinateXY(3.0, 7.0)));

  std::unique_ptr<Geometry> edited_line =
      as_editor_op->edit(line.get(), factory.get());
  ASSERT_NE(edited_line, nullptr);
  EXPECT_TRUE(edited_line->equalsExact(
      MakeLineString(*factory, {{2.0, 3.0}, {3.0, 5.0}}).get(), 1e-12));
  EXPECT_EQ(impl.call_count, 2u);
}

TEST(CoordinateOperationTest, IntegratesWithGeometryEditorOnComplexGeometries) {
  GeometryFactory::Ptr src_factory = GeometryFactory::create();
  PrecisionModel pm;
  GeometryFactory::Ptr dst_factory = GeometryFactory::create(&pm, 4326);

  auto shell =
      MakeRing(*src_factory, {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(
      MakeRing(*src_factory, {{2, 2}, {4, 2}, {4, 4}, {2, 4}, {2, 2}}));
  auto poly = src_factory->createPolygon(std::move(shell), std::move(holes));

  std::vector<std::unique_ptr<Geometry>> gc_items;
  gc_items.push_back(src_factory->createPoint(CoordinateXY(1.0, 1.0)));
  gc_items.push_back(MakeLineString(*src_factory, {{0.0, 0.0}, {3.0, 3.0}}));
  gc_items.push_back(poly->clone());
  auto gc = src_factory->createGeometryCollection(std::move(gc_items));

  GeometryEditor editor(dst_factory.get());
  ShiftCoordinatesOp shift_op(5.0, -2.0);
  std::unique_ptr<Geometry> edited_gc = editor.edit(gc.get(), &shift_op);

  ASSERT_NE(edited_gc, nullptr);
  EXPECT_EQ(edited_gc->getFactory(), dst_factory.get());
  EXPECT_THAT(
      shift_op.seen_types,
      ElementsAre(GeometryTypeId::GEOS_POINT, GeometryTypeId::GEOS_LINESTRING,
                  GeometryTypeId::GEOS_LINEARRING,
                  GeometryTypeId::GEOS_LINEARRING));

  const auto* edited_coll =
      dynamic_cast<const GeometryCollection*>(edited_gc.get());
  ASSERT_NE(edited_coll, nullptr);
  ASSERT_EQ(edited_coll->getNumGeometries(), 3u);
  ASSERT_NE(edited_coll->getGeometryN(0)->getCoordinate(), nullptr);
  EXPECT_TRUE(
      edited_coll->getGeometryN(0)->getCoordinate()->equals2D({6.0, -1.0}));
  EXPECT_TRUE(edited_coll->getGeometryN(1)->equalsExact(
      MakeLineString(*dst_factory, {{5.0, -2.0}, {8.0, 1.0}}).get(), 1e-12));

  // Verify emptying a hole via CoordinateOperation drops the hole in
  // GeometryEditor.
  ClearSpecificGeometryOp drop_hole(poly->getInteriorRingN(0));
  std::unique_ptr<Geometry> poly_without_hole =
      editor.edit(poly.get(), &drop_hole);
  ASSERT_NE(poly_without_hole, nullptr);
  const auto* edited_poly =
      dynamic_cast<const Polygon*>(poly_without_hole.get());
  ASSERT_NE(edited_poly, nullptr);
  EXPECT_EQ(edited_poly->getNumInteriorRing(), 0u);
}

}  // namespace
}  // namespace geos_google
