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

#include "third_party/geos/include/geos/noding/NodedSegmentString.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentNode.h"
#include "third_party/geos/include/geos/noding/SegmentNodeList.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentNode;
using geos::noding::SegmentNodeList;
using geos::noding::SegmentString;

// RAII owner for SegmentString* elements placed into resultEdgeList.
class OwnedSplitEdges {
 public:
  OwnedSplitEdges() = default;
  OwnedSplitEdges(const OwnedSplitEdges&) = delete;
  OwnedSplitEdges& operator=(const OwnedSplitEdges&) = delete;

  ~OwnedSplitEdges() {
    for (SegmentString* ss : edges_) {
      delete ss;
    }
  }

  SegmentString::NonConstVect* raw() { return &edges_; }
  const SegmentString::NonConstVect& edges() const { return edges_; }

 private:
  SegmentString::NonConstVect edges_;
};

struct SplitEdgeVectorDeleter {
  void operator()(SegmentString::NonConstVect* vec) const {
    if (vec != nullptr) {
      for (SegmentString* ss : *vec) {
        delete ss;
      }
      delete vec;
    }
  }
};

using OwnedSplitEdgeVector =
    std::unique_ptr<SegmentString::NonConstVect, SplitEdgeVectorDeleter>;

TEST(NodedSegmentStringTest, ConstructFromSequenceAndAccessNodeList) {
  int context_tag = 42;
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {20.0, 5.0}});

  NodedSegmentString nss(seq.release(), /*constructZ=*/false,
                         /*constructM=*/false, &context_tag);

  EXPECT_EQ(nss.size(), 3u);
  EXPECT_EQ(nss.getData(), &context_tag);
  EXPECT_FALSE(nss.hasNodes());

  SegmentNodeList& mutable_list = nss.getNodeList();
  const NodedSegmentString& const_ref = nss;
  const SegmentNodeList& const_list = const_ref.getNodeList();
  EXPECT_EQ(&mutable_list, &const_list);
  EXPECT_EQ(const_list.size(), 0u);
  EXPECT_FALSE(const_list.getConstructZ());
  EXPECT_FALSE(const_list.getConstructM());

  int other_tag = 99;
  nss.setData(&other_tag);
  EXPECT_EQ(nss.getData(), &other_tag);
}

TEST(NodedSegmentStringTest, ConstructFromSegmentStringClonesSequenceAndData) {
  int context_tag = 7;
  CoordinateSequence src_seq({CoordinateXY(1.0, 2.0), CoordinateXY(3.0, 4.0)});
  BasicSegmentString src(&src_seq, &context_tag);

  NodedSegmentString nss(&src, /*constructZ=*/true, /*constructM=*/false);

  // Mutating the source sequence must not affect the cloned NodedSegmentString.
  src_seq.setAt(CoordinateXY(100.0, 200.0), 0);

  EXPECT_EQ(nss.getData(), &context_tag);
  EXPECT_EQ(nss.size(), 2u);
  EXPECT_TRUE(nss.getCoordinate<CoordinateXY>(0).equals2D({1.0, 2.0}));
  EXPECT_TRUE(nss.getCoordinate<CoordinateXY>(1).equals2D({3.0, 4.0}));
  EXPECT_TRUE(nss.getNodeList().getConstructZ());
  EXPECT_FALSE(nss.getNodeList().getConstructM());
}

TEST(NodedSegmentStringTest, ReleaseCoordinatesTransfersOwnershipSafely) {
  auto seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {5.0, 5.0}});
  CoordinateSequence* raw_ptr = seq.get();

  NodedSegmentString nss(seq.release(), false, false, nullptr);
  std::unique_ptr<CoordinateSequence> released = nss.releaseCoordinates();

  EXPECT_EQ(released.get(), raw_ptr);
  EXPECT_EQ(nss.getCoordinates(), nullptr);
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released->size(), 2u);
  EXPECT_TRUE(released->getAt<CoordinateXY>(1).equals2D({5.0, 5.0}));
}

TEST(NodedSegmentStringTest, AddIntersectionNormalizesVerticesAndDeduplicates) {
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}});
  NodedSegmentString nss(seq.release(), false, false, nullptr);

  // Interior point on segment 0 stays on segment 0.
  nss.addIntersection(CoordinateXY(4.0, 0.0), 0);
  EXPECT_TRUE(nss.hasNodes());

  // Point equal to vertex 1 passed with segmentIndex=0 normalizes to seg 1.
  nss.addIntersection(CoordinateXY(10.0, 0.0), 0);
  // Adding the exact same vertex on segment 1 deduplicates to the same node.
  nss.addIntersection(CoordinateXY(10.0, 0.0), 1);

  // Point equal to final vertex 2 passed with segmentIndex=1 normalizes to 2.
  nss.addIntersection(CoordinateXY(20.0, 0.0), 1);

  const SegmentNodeList& nodes =
      static_cast<const NodedSegmentString&>(nss).getNodeList();
  ASSERT_EQ(nodes.size(), 3u);

  auto it = nodes.begin();
  const SegmentNode& n0 = *it++;
  const SegmentNode& n1 = *it++;
  const SegmentNode& n2 = *it++;
  EXPECT_EQ(n0.segmentIndex, 0u);
  EXPECT_TRUE(n0.isInterior());
  EXPECT_TRUE(n0.coord.equals2D({4.0, 0.0}));

  EXPECT_EQ(n1.segmentIndex, 1u);
  EXPECT_FALSE(n1.isInterior());
  EXPECT_TRUE(n1.coord.equals2D({10.0, 0.0}));

  EXPECT_EQ(n2.segmentIndex, 2u);
  EXPECT_FALSE(n2.isInterior());
  EXPECT_TRUE(n2.coord.equals2D({20.0, 0.0}));

  std::unique_ptr<CoordinateSequence> noded_coords = nss.getNodedCoordinates();
  ASSERT_EQ(noded_coords->size(), 4u);
  EXPECT_TRUE(noded_coords->getAt<CoordinateXY>(0).equals2D({0.0, 0.0}));
  EXPECT_TRUE(noded_coords->getAt<CoordinateXY>(1).equals2D({4.0, 0.0}));
  EXPECT_TRUE(noded_coords->getAt<CoordinateXY>(2).equals2D({10.0, 0.0}));
  EXPECT_TRUE(noded_coords->getAt<CoordinateXY>(3).equals2D({20.0, 0.0}));
}

TEST(NodedSegmentStringTest, AddIntersectionOutOfRangeThrows) {
  auto seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}});
  NodedSegmentString nss(seq.release(), false, false, nullptr);

  // Valid segment indices for 3 vertices are 0 and 1; >= 2 throws.
  EXPECT_THROW(nss.addIntersection(CoordinateXY(5.0, 0.0), 2),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(nss.addIntersection(CoordinateXY(5.0, 0.0), 99),
               geos::util::IllegalArgumentException);
}

TEST(NodedSegmentStringTest,
     AddIntersectionsFromLineIntersectorHandlesZeroOneAndTwoPoints) {
  const CoordinateXY p0(0.0, 0.0);
  const CoordinateXY p1(10.0, 0.0);
  auto seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{p0, p1});
  NodedSegmentString nss(seq.release(), false, false, nullptr);

  LineIntersector li;

  // 0 intersections: disjoint segment.
  const CoordinateXY d0(0.0, 5.0);
  const CoordinateXY d1(10.0, 5.0);
  li.computeIntersection(p0, p1, d0, d1);
  ASSERT_EQ(li.getIntersectionNum(), 0u);
  nss.addIntersections(&li, 0, 0);
  EXPECT_FALSE(nss.hasNodes());

  // 1 intersection: perpendicular crossing at (5, 0).
  const CoordinateXY c0(5.0, -5.0);
  const CoordinateXY c1(5.0, 5.0);
  li.computeIntersection(p0, p1, c0, c1);
  ASSERT_EQ(li.getIntersectionNum(), 1u);
  nss.addIntersections(&li, 0, 0);
  EXPECT_EQ(nss.getNodeList().size(), 1u);

  // 2 intersections: collinear overlap [3, 7].
  const CoordinateXY o0(3.0, 0.0);
  const CoordinateXY o1(7.0, 0.0);
  li.computeIntersection(p0, p1, o0, o1);
  ASSERT_EQ(li.getIntersectionNum(), 2u);
  nss.addIntersections(&li, 0, 0);
  EXPECT_EQ(nss.getNodeList().size(), 3u);

  std::unique_ptr<CoordinateSequence> coords = nss.getNodedCoordinates();
  ASSERT_EQ(coords->size(), 5u);
  EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D({0.0, 0.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(1).equals2D({3.0, 0.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(2).equals2D({5.0, 0.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(3).equals2D({7.0, 0.0}));
  EXPECT_TRUE(coords->getAt<CoordinateXY>(4).equals2D({10.0, 0.0}));
}

TEST(NodedSegmentStringTest, GetNodedSubstringsAllOverloadsSplitCorrectly) {
  int tag1 = 1;
  int tag2 = 2;

  auto seq1 =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}});
  NodedSegmentString nss1(seq1.release(), false, false, &tag1);
  nss1.addIntersection(CoordinateXY(5.0, 0.0), 0);

  auto seq2 = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{20.0, 0.0}, {30.0, 0.0}});
  NodedSegmentString nss2(seq2.release(), false, false, &tag2);

  SegmentString::NonConstVect input{&nss1, &nss2};

  auto verify_split_edges = [&](const SegmentString::NonConstVect& edges) {
    ASSERT_EQ(edges.size(), 3u);

    EXPECT_EQ(edges[0]->getData(), &tag1);
    ASSERT_EQ(edges[0]->size(), 2u);
    EXPECT_TRUE(edges[0]->getCoordinate<CoordinateXY>(0).equals2D({0.0, 0.0}));
    EXPECT_TRUE(edges[0]->getCoordinate<CoordinateXY>(1).equals2D({5.0, 0.0}));

    EXPECT_EQ(edges[1]->getData(), &tag1);
    ASSERT_EQ(edges[1]->size(), 3u);
    EXPECT_TRUE(edges[1]->getCoordinate<CoordinateXY>(0).equals2D({5.0, 0.0}));
    EXPECT_TRUE(edges[1]->getCoordinate<CoordinateXY>(1).equals2D({10.0, 0.0}));
    EXPECT_TRUE(
        edges[1]->getCoordinate<CoordinateXY>(2).equals2D({10.0, 10.0}));

    EXPECT_EQ(edges[2]->getData(), &tag2);
    ASSERT_EQ(edges[2]->size(), 2u);
    EXPECT_TRUE(edges[2]->getCoordinate<CoordinateXY>(0).equals2D({20.0, 0.0}));
    EXPECT_TRUE(edges[2]->getCoordinate<CoordinateXY>(1).equals2D({30.0, 0.0}));
  };

  // Overload 1: (const NonConstVect&, NonConstVect*)
  {
    OwnedSplitEdges out;
    NodedSegmentString::getNodedSubstrings(input, out.raw());
    verify_split_edges(out.edges());
  }

  // Overload 2: (const NonConstVect&) returning newly allocated vector
  {
    OwnedSplitEdgeVector out(NodedSegmentString::getNodedSubstrings(input));
    ASSERT_NE(out, nullptr);
    verify_split_edges(*out);
  }

  // Overload 3: template iterator range (from, too_far, NonConstVect*)
  {
    OwnedSplitEdges out;
    NodedSegmentString::getNodedSubstrings(input.begin(), input.end(),
                                           out.raw());
    verify_split_edges(out.edges());
  }

  // Overload 4: template container pointer (C*, NonConstVect*)
  {
    std::vector<NodedSegmentString*> typed_input{&nss1, &nss2};
    OwnedSplitEdges out;
    NodedSegmentString::getNodedSubstrings(&typed_input, out.raw());
    verify_split_edges(out.edges());
  }
}

TEST(NodedSegmentStringTest, GetNodedCoordinatesAndSubstringsPreserveZM) {
  // XYZ only (constructZ=true, constructM=false).
  {
    auto seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<Coordinate>{{0.0, 0.0, 10.0}, {10.0, 0.0, 20.0}});
    NodedSegmentString nss(seq.release(), /*constructZ=*/true,
                           /*constructM=*/false, nullptr);
    nss.addIntersection(Coordinate(5.0, 0.0, 15.0), 0);

    std::unique_ptr<CoordinateSequence> coords = nss.getNodedCoordinates();
    EXPECT_TRUE(coords->hasZ());
    EXPECT_FALSE(coords->hasM());
    ASSERT_EQ(coords->size(), 3u);
    EXPECT_DOUBLE_EQ(coords->getAt<Coordinate>(1).z, 15.0);
    EXPECT_TRUE(std::isnan(coords->getOrdinate(1, CoordinateSequence::M)));

    OwnedSplitEdges split;
    NodedSegmentString::getNodedSubstrings(SegmentString::NonConstVect{&nss},
                                           split.raw());
    ASSERT_EQ(split.edges().size(), 2u);
    EXPECT_TRUE(split.edges()[0]->getCoordinates()->hasZ());
    EXPECT_DOUBLE_EQ(split.edges()[0]->getCoordinate<Coordinate>(1).z, 15.0);
  }

  // XYM only (constructZ=false, constructM=true).
  {
    auto seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYM>{{0.0, 0.0, 1.0},
                                             {10.0, 0.0, 3.0}});
    NodedSegmentString nss(seq.release(), /*constructZ=*/false,
                           /*constructM=*/true, nullptr);
    nss.addIntersection(CoordinateXYM(5.0, 0.0, 2.0), 0);

    std::unique_ptr<CoordinateSequence> coords = nss.getNodedCoordinates();
    EXPECT_FALSE(coords->hasZ());
    EXPECT_TRUE(coords->hasM());
    ASSERT_EQ(coords->size(), 3u);
    CoordinateXYM mid;
    coords->getAt(1, mid);
    EXPECT_DOUBLE_EQ(mid.x, 5.0);
    EXPECT_DOUBLE_EQ(mid.y, 0.0);
    EXPECT_DOUBLE_EQ(mid.m, 2.0);
    EXPECT_TRUE(std::isnan(coords->getOrdinate(1, CoordinateSequence::Z)));
    EXPECT_DOUBLE_EQ(coords->getOrdinate(1, CoordinateSequence::M), 2.0);
  }

  // XYZM (constructZ=true, constructM=true).
  {
    auto seq = std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXYZM>{{0.0, 0.0, 10.0, 100.0},
                                              {10.0, 0.0, 20.0, 200.0}});
    NodedSegmentString nss(seq.release(), /*constructZ=*/true,
                           /*constructM=*/true, nullptr);
    nss.addIntersection(CoordinateXYZM(5.0, 0.0, 15.0, 150.0), 0);

    std::unique_ptr<CoordinateSequence> coords = nss.getNodedCoordinates();
    EXPECT_TRUE(coords->hasZ());
    EXPECT_TRUE(coords->hasM());
    ASSERT_EQ(coords->size(), 3u);
    const auto& mid = coords->getAt<CoordinateXYZM>(1);
    EXPECT_DOUBLE_EQ(mid.z, 15.0);
    EXPECT_DOUBLE_EQ(mid.m, 150.0);
  }
}

TEST(NodedSegmentStringTest, PrintFormatsLineStringAndNodeCount) {
  auto seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{1.0, 2.0}, {3.0, 4.0}});
  NodedSegmentString nss(seq.release(), false, false, nullptr);
  nss.addIntersection(CoordinateXY(2.0, 3.0), 0);

  std::ostringstream direct_oss;
  std::ostream& ret = nss.print(direct_oss);
  EXPECT_EQ(&ret, &direct_oss);

  const std::string printed = direct_oss.str();
  EXPECT_NE(printed.find("NodedSegmentString:"), std::string::npos);
  EXPECT_NE(printed.find("LINESTRING"), std::string::npos);
  EXPECT_NE(printed.find("Nodes: 1"), std::string::npos);

  // Verify virtual dispatch through SegmentString::operator<<.
  std::ostringstream stream_oss;
  const SegmentString& base_ref = nss;
  stream_oss << base_ref;
  EXPECT_EQ(stream_oss.str(), printed);
}

}  // namespace
}  // namespace geos_google
