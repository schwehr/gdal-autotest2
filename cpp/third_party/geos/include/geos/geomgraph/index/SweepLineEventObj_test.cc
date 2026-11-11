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

// Unit tests for
// third_party/geos/include/geos/geomgraph/index/SweepLineEventObj.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geomgraph/index/SweepLineEventObj.h

#include "third_party/geos/include/geos/geomgraph/index/SweepLineEventObj.h"

#include <initializer_list>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/index/MonotoneChain.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEvent.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineSegment.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::Edge;
using geos::geomgraph::index::MonotoneChain;
using geos::geomgraph::index::SweepLineEvent;
using geos::geomgraph::index::SweepLineEventOBJ;
using geos::geomgraph::index::SweepLineSegment;

static_assert(std::is_polymorphic_v<SweepLineEventOBJ>);
static_assert(std::has_virtual_destructor_v<SweepLineEventOBJ>);
static_assert(!std::is_abstract_v<SweepLineEventOBJ>);
static_assert(std::is_default_constructible_v<SweepLineEventOBJ>);
static_assert(std::is_copy_constructible_v<SweepLineEventOBJ>);
static_assert(std::is_copy_assignable_v<SweepLineEventOBJ>);
static_assert(std::is_base_of_v<SweepLineEventOBJ, SweepLineSegment>);
static_assert(std::is_base_of_v<SweepLineEventOBJ, MonotoneChain>);

class RecordingSweepLineObj : public SweepLineEventOBJ {
 public:
  explicit RecordingSweepLineObj(int id, bool* destroyed)
      : id_(id), destroyed_(destroyed) {}

  ~RecordingSweepLineObj() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  int id() const { return id_; }

 private:
  int id_;
  bool* destroyed_;
};

class DestructionNotifier {
 public:
  explicit DestructionNotifier(bool* destroyed) : destroyed_(destroyed) {}
  ~DestructionNotifier() {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

 private:
  bool* destroyed_;
};

// Relies on the compiler-generated virtual destructor to verify implicit
// override chaining back to ~SweepLineEventOBJ().
class ImplicitDtorSweepLineObj : public SweepLineEventOBJ {
 public:
  explicit ImplicitDtorSweepLineObj(bool* destroyed) : notifier_(destroyed) {}

 private:
  DestructionNotifier notifier_;
};

TEST(SweepLineEventObjTest, DirectConstructionCopyAndEventAttachment) {
  SweepLineEventOBJ base_obj;
  SweepLineEventOBJ copied_obj(base_obj);
  SweepLineEventOBJ assigned_obj;
  assigned_obj = copied_obj;

  SweepLineEvent event_with_obj(nullptr, 2.5, nullptr, &base_obj);
  SweepLineEvent event_with_copy(nullptr, 3.0, nullptr, &assigned_obj);
  SweepLineEvent event_without_obj(nullptr, 4.0, nullptr, nullptr);

  EXPECT_EQ(event_with_obj.getObject(), &base_obj);
  EXPECT_EQ(event_with_copy.getObject(), &assigned_obj);
  EXPECT_EQ(event_without_obj.getObject(), nullptr);
}

TEST(SweepLineEventObjTest, PolymorphicDestructionThroughBasePointer) {
  bool stack_destroyed = false;
  {
    RecordingSweepLineObj stack_obj(7, &stack_destroyed);
    EXPECT_EQ(stack_obj.id(), 7);
  }
  EXPECT_TRUE(stack_destroyed);

  bool explicit_dtor_destroyed = false;
  bool implicit_dtor_destroyed = false;
  std::vector<std::unique_ptr<SweepLineEventOBJ>> objects;
  objects.push_back(std::make_unique<SweepLineEventOBJ>());
  objects.push_back(
      std::make_unique<RecordingSweepLineObj>(42, &explicit_dtor_destroyed));
  objects.push_back(
      std::make_unique<ImplicitDtorSweepLineObj>(&implicit_dtor_destroyed));

  EXPECT_FALSE(explicit_dtor_destroyed);
  EXPECT_FALSE(implicit_dtor_destroyed);

  const auto* recorded =
      dynamic_cast<const RecordingSweepLineObj*>(objects[1].get());
  ASSERT_NE(recorded, nullptr);
  EXPECT_EQ(recorded->id(), 42);
  EXPECT_EQ(dynamic_cast<const RecordingSweepLineObj*>(objects[0].get()),
            nullptr);

  objects.clear();
  EXPECT_TRUE(explicit_dtor_destroyed);
  EXPECT_TRUE(implicit_dtor_destroyed);
}

TEST(SweepLineEventObjTest, GeosSubclassesViaBasePointersAndSweepLineEvent) {
  auto coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(1.0, 0.0),
          Coordinate(5.0, 4.0),
          Coordinate(2.0, 6.0),
      });
  // Edge takes ownership of the CoordinateSequence and must outlive the
  // SweepLineSegment and MonotoneChain objects below.
  Edge edge(coords.release());

  std::vector<std::unique_ptr<SweepLineEventOBJ>> objs;
  objs.push_back(std::make_unique<SweepLineSegment>(&edge, 0));
  objs.push_back(
      std::make_unique<MonotoneChain>(edge.getMonotoneChainEdge(), 0));

  SweepLineEvent insert_seg(nullptr, 1.0, nullptr, objs[0].get());
  SweepLineEvent delete_seg(nullptr, 5.0, &insert_seg, objs[0].get());
  SweepLineEvent insert_chain(nullptr, 1.0, nullptr, objs[1].get());

  EXPECT_EQ(insert_seg.getObject(), objs[0].get());
  EXPECT_EQ(delete_seg.getObject(), objs[0].get());
  EXPECT_EQ(delete_seg.getInsertEvent(), &insert_seg);
  EXPECT_EQ(insert_chain.getObject(), objs[1].get());

  auto* seg = dynamic_cast<SweepLineSegment*>(insert_seg.getObject());
  ASSERT_NE(seg, nullptr);
  EXPECT_DOUBLE_EQ(seg->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(seg->getMaxX(), 5.0);
  EXPECT_EQ(dynamic_cast<MonotoneChain*>(insert_seg.getObject()), nullptr);

  auto* chain = dynamic_cast<MonotoneChain*>(insert_chain.getObject());
  ASSERT_NE(chain, nullptr);
  EXPECT_EQ(dynamic_cast<SweepLineSegment*>(insert_chain.getObject()), nullptr);
}

}  // namespace
}  // namespace geos_google
