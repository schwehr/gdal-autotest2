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

#include "third_party/geos/include/geos/noding/NodableSegmentString.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::NodableSegmentString;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentString;

static_assert(std::is_base_of_v<SegmentString, NodableSegmentString>);
static_assert(!std::is_abstract_v<NodableSegmentString>);
static_assert(std::has_virtual_destructor_v<NodableSegmentString>);
static_assert(!std::is_copy_constructible_v<NodableSegmentString>);
static_assert(!std::is_copy_assignable_v<NodableSegmentString>);

class RecordingNodableSegmentString final : public NodableSegmentString {
 public:
  RecordingNodableSegmentString(const void* context, CoordinateSequence* seq,
                                int* destroy_count)
      : NodableSegmentString(context, seq), destroy_count_(destroy_count) {}

  ~RecordingNodableSegmentString() override {
    if (destroy_count_ != nullptr) {
      ++(*destroy_count_);
    }
  }

  void addNode(const CoordinateXY& pt, std::size_t seg_index) {
    recorded_nodes_.emplace_back(seg_index, pt);
  }

  const std::vector<std::pair<std::size_t, CoordinateXY>>& recorded_nodes()
      const {
    return recorded_nodes_;
  }

 private:
  int* destroy_count_;
  std::vector<std::pair<std::size_t, CoordinateXY>> recorded_nodes_;
};

TEST(NodableSegmentStringTest, DirectConstructionContextAndCoordinates) {
  CoordinateSequence seq({
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 1.0),
      Coordinate(4.0, 5.0),
  });
  int context_tag = 42;
  NodableSegmentString ss(&context_tag, &seq);

  EXPECT_EQ(ss.getData(), &context_tag);
  int new_context_tag = 99;
  ss.setData(&new_context_tag);
  EXPECT_EQ(ss.getData(), &new_context_tag);

  EXPECT_EQ(ss.size(), 3u);
  EXPECT_FALSE(ss.isClosed());

  const NodableSegmentString& const_ss = ss;
  EXPECT_EQ(const_ss.getCoordinates(), &seq);
  EXPECT_EQ(ss.getCoordinates(), &seq);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate(0).x, 0.0);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate(0).y, 0.0);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate<CoordinateXY>(2).x, 4.0);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate<CoordinateXY>(2).y, 5.0);

  // SegmentString/NodableSegmentString leaves CoordinateSequence externally
  // owned.
  NodableSegmentString null_ctx_ss(nullptr, &seq);
  EXPECT_EQ(null_ctx_ss.getData(), nullptr);
  EXPECT_EQ(null_ctx_ss.size(), 3u);
}

TEST(NodableSegmentStringTest, SegmentOctantsAndClosedRingNavigation) {
  CoordinateSequence ring_seq({
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 1.0),   // dx=2, dy=1 -> octant 0
      Coordinate(3.0, 3.0),   // dx=1, dy=2 -> octant 1
      Coordinate(-1.0, 3.0),  // dx=-4, dy=0 -> octant 3
      Coordinate(0.0, 0.0),   // dx=1, dy=-3 -> octant 6
  });
  NodableSegmentString ring(nullptr, &ring_seq);

  EXPECT_TRUE(ring.isClosed());
  EXPECT_EQ(ring.getSegmentOctant(0), 0);
  EXPECT_EQ(ring.getSegmentOctant(1), 1);
  EXPECT_EQ(ring.getSegmentOctant(2), 3);
  EXPECT_EQ(ring.getSegmentOctant(3), 6);
  EXPECT_EQ(ring.getSegmentOctant(4), -1);
  EXPECT_EQ(SegmentString::getSegmentOctant(ring, 1), 1);

  EXPECT_TRUE(ring.nextInRing(0).equals2D(CoordinateXY(2.0, 1.0)));
  EXPECT_TRUE(ring.nextInRing(3).equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(ring.nextInRing(4).equals2D(CoordinateXY(2.0, 1.0)));

  EXPECT_TRUE(ring.prevInRing(0).equals2D(CoordinateXY(-1.0, 3.0)));
  EXPECT_TRUE(ring.prevInRing(1).equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(ring.prevInRing(4).equals2D(CoordinateXY(-1.0, 3.0)));

  // Zero-length consecutive segment yields octant 0 safely.
  CoordinateSequence deg_seq({
      Coordinate(5.0, 5.0),
      Coordinate(5.0, 5.0),
  });
  NodableSegmentString degenerate(nullptr, &deg_seq);
  EXPECT_EQ(degenerate.getSegmentOctant(0), 0);
  EXPECT_TRUE(degenerate.isClosed());
}

TEST(NodableSegmentStringTest, PolymorphicOwnershipAndPrinting) {
  CoordinateSequence seq({
      Coordinate(1.0, 2.0),
      Coordinate(3.0, 4.0),
  });
  int tag = 7;

  std::unique_ptr<NodableSegmentString> nodable =
      std::make_unique<NodableSegmentString>(&tag, &seq);
  std::ostringstream oss;
  oss << *nodable;
  EXPECT_EQ(oss.str(), "SegmentString\n");

  std::unique_ptr<SegmentString> as_base = std::move(nodable);
  EXPECT_EQ(as_base->getData(), &tag);
  EXPECT_EQ(as_base->size(), 2u);
  as_base.reset();

  // Externally owned CoordinateSequence remains intact after destruction.
  EXPECT_EQ(seq.size(), 2u);
  EXPECT_DOUBLE_EQ(seq[1].x, 3.0);
}

TEST(NodableSegmentStringTest, DerivedNodedSegmentStringThroughBaseInterface) {
  int tag = 123;
  auto seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<Coordinate>{{0.0, 0.0}, {10.0, 0.0}});
  std::unique_ptr<NodableSegmentString> nodable =
      std::make_unique<NodedSegmentString>(seq.release(), false, false, &tag);

  EXPECT_EQ(nodable->getData(), &tag);
  EXPECT_EQ(nodable->size(), 2u);

  auto* noded = dynamic_cast<NodedSegmentString*>(nodable.get());
  ASSERT_NE(noded, nullptr);
  noded->addIntersection(Coordinate(4.0, 0.0), 0);
  EXPECT_TRUE(noded->hasNodes());

  SegmentString::NonConstVect split_edges;
  absl::Cleanup cleanup_edges = [&split_edges] {
    for (SegmentString* edge : split_edges) {
      delete edge;
    }
  };
  noded->getNodeList().addSplitEdges(split_edges);
  ASSERT_EQ(split_edges.size(), 2u);
  EXPECT_EQ(split_edges[0]->getData(), &tag);
  EXPECT_EQ(split_edges[1]->getData(), &tag);
  EXPECT_TRUE(
      split_edges[0]->getCoordinate(1).equals2D(CoordinateXY(4.0, 0.0)));
  EXPECT_TRUE(
      split_edges[1]->getCoordinate(0).equals2D(CoordinateXY(4.0, 0.0)));
}

TEST(NodableSegmentStringTest, CustomNodableSubclassLifecycleAndForwarding) {
  CoordinateSequence seq({
      Coordinate(0.0, 0.0),
      Coordinate(6.0, 8.0),
  });
  int tag = 55;
  int destroy_count = 0;

  auto custom = std::make_unique<RecordingNodableSegmentString>(&tag, &seq,
                                                                &destroy_count);
  custom->addNode(CoordinateXY(3.0, 4.0), 0);
  ASSERT_EQ(custom->recorded_nodes().size(), 1u);
  EXPECT_EQ(custom->recorded_nodes()[0].first, 0u);
  EXPECT_TRUE(
      custom->recorded_nodes()[0].second.equals2D(CoordinateXY(3.0, 4.0)));

  std::unique_ptr<NodableSegmentString> base = std::move(custom);
  EXPECT_EQ(base->getData(), &tag);
  EXPECT_EQ(destroy_count, 0);
  base.reset();
  EXPECT_EQ(destroy_count, 1);
}

}  // namespace
}  // namespace geos_google
