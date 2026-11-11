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

// Unit tests for third_party/geos/src/geomgraph/EdgeList.cpp.

#include "third_party/geos/include/geos/geomgraph/EdgeList.h"

#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Location;
using geos::geomgraph::Edge;
using geos::geomgraph::EdgeList;
using geos::geomgraph::Label;

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords,
                               const Label& label = Label(Location::BOUNDARY)) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release(), label);
}

TEST(EdgeListTest, DefaultConstructionIsEmpty) {
  auto probe = MakeEdge({{0.0, 0.0}, {1.0, 1.0}});
  EdgeList list;

  EXPECT_TRUE(list.getEdges().empty());
  EXPECT_EQ(list.findEqualEdge(probe.get()), nullptr);
  EXPECT_EQ(list.findEdgeIndex(probe.get()), -1);
  EXPECT_EQ(list.print(), "EdgeList: \n");
}

TEST(EdgeListTest, AddAndGetPreserveInsertionOrder) {
  auto e0 = MakeEdge({{0.0, 0.0}, {1.0, 0.0}});
  auto e1 = MakeEdge({{1.0, 0.0}, {1.0, 2.0}});
  auto e2 = MakeEdge({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}});
  auto absent = MakeEdge({{9.0, 9.0}, {10.0, 10.0}});

  EdgeList list;
  list.add(e0.get());
  list.add(e1.get());
  list.add(e2.get());

  ASSERT_EQ(list.getEdges().size(), 3u);
  EXPECT_EQ(list.get(0), e0.get());
  EXPECT_EQ(list.get(1), e1.get());
  EXPECT_EQ(list.get(2), e2.get());

  EXPECT_EQ(list.findEdgeIndex(e0.get()), 0);
  EXPECT_EQ(list.findEdgeIndex(e1.get()), 1);
  EXPECT_EQ(list.findEdgeIndex(e2.get()), 2);
  EXPECT_EQ(list.findEdgeIndex(absent.get()), -1);
}

TEST(EdgeListTest, AddAllInsertsCollectionAndHandlesEmptyInput) {
  auto e0 = MakeEdge({{0.0, 0.0}, {2.0, 0.0}});
  auto e1 = MakeEdge({{2.0, 0.0}, {2.0, 2.0}});
  auto e2 = MakeEdge({{2.0, 2.0}, {0.0, 2.0}});

  EdgeList list;
  const std::vector<Edge*> empty_edges;
  list.addAll(empty_edges);
  EXPECT_TRUE(list.getEdges().empty());

  const std::vector<Edge*> batch = {e0.get(), e1.get(), e2.get()};
  list.addAll(batch);

  ASSERT_EQ(list.getEdges().size(), 3u);
  EXPECT_EQ(list.get(0), e0.get());
  EXPECT_EQ(list.get(1), e1.get());
  EXPECT_EQ(list.get(2), e2.get());
  EXPECT_EQ(list.findEqualEdge(e1.get()), e1.get());
}

TEST(EdgeListTest, FindEqualEdgeMatchesForwardReversedAndPalindrome) {
  auto e0 = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});
  auto e_pal = MakeEdge({{1.0, 0.0}, {2.0, 0.0}, {1.0, 0.0}});
  auto e0_dup = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});

  auto q_forward = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}});
  auto q_reversed = MakeEdge({{3.0, 4.0}, {1.0, 2.0}, {0.0, 0.0}});
  auto q_pal = MakeEdge({{1.0, 0.0}, {2.0, 0.0}, {1.0, 0.0}});
  auto q_short = MakeEdge({{0.0, 0.0}, {3.0, 4.0}});
  auto q_other = MakeEdge({{0.0, 0.0}, {1.0, 2.0}, {3.0, 5.0}});

  EdgeList list;
  list.add(e0.get());
  list.add(e_pal.get());

  EXPECT_EQ(list.findEqualEdge(q_forward.get()), e0.get());
  EXPECT_EQ(list.findEqualEdge(q_reversed.get()), e0.get());
  EXPECT_EQ(list.findEqualEdge(q_pal.get()), e_pal.get());
  EXPECT_EQ(list.findEqualEdge(q_short.get()), nullptr);
  EXPECT_EQ(list.findEqualEdge(q_other.get()), nullptr);

  // Adding a duplicate edge appends to the vector and updates the lookup map.
  list.add(e0_dup.get());
  EXPECT_EQ(list.getEdges().size(), 3u);
  EXPECT_EQ(list.findEqualEdge(q_forward.get()), e0_dup.get());
  EXPECT_EQ(list.findEdgeIndex(q_forward.get()), 0);
}

TEST(EdgeListTest, FindEdgeIndexMatchesForwardAndReversedIgnoringZ) {
  auto e0 = MakeEdge({{0.0, 0.0, 1.0}, {2.0, 0.0, 2.0}, {2.0, 3.0, 3.0}});
  auto e1 = MakeEdge({{5.0, 5.0, 0.0}, {6.0, 6.0, 0.0}});

  auto q0_rev = MakeEdge({{2.0, 3.0, 9.0}, {2.0, 0.0, 8.0}, {0.0, 0.0, 7.0}});
  auto q1_fwd = MakeEdge({{5.0, 5.0, 4.0}, {6.0, 6.0, 5.0}});
  auto q_miss = MakeEdge({{0.0, 0.0}, {2.0, 3.0}});

  EdgeList list;
  list.add(e0.get());
  list.add(e1.get());

  EXPECT_EQ(list.findEdgeIndex(q0_rev.get()), 0);
  EXPECT_EQ(list.findEdgeIndex(q1_fwd.get()), 1);
  EXPECT_EQ(list.findEdgeIndex(q_miss.get()), -1);
}

TEST(EdgeListTest, PrintAndStreamOperatorFormatAllEdges) {
  auto e0 = MakeEdge({{0.0, 0.0}, {1.0, 0.0}});
  auto e1 = MakeEdge({{1.0, 0.0}, {1.0, 2.0}});

  EdgeList list;
  list.add(e0.get());
  list.add(e1.get());

  const std::string printed = list.print();
  std::ostringstream os;
  os << list;
  EXPECT_EQ(os.str(), printed);

  EXPECT_EQ(printed.rfind("EdgeList: \n", 0), 0u);
  EXPECT_NE(printed.find(e0->print()), std::string::npos);
  EXPECT_NE(printed.find(e1->print()), std::string::npos);
}

TEST(EdgeListTest, ClearListDeletesOwnedEdgesAndEmptiesVector) {
  EdgeList list;
  list.clearList();
  EXPECT_TRUE(list.getEdges().empty());

  auto* raw0 = MakeEdge({{0.0, 0.0}, {1.0, 1.0}}).release();
  auto* raw1 = MakeEdge({{1.0, 1.0}, {2.0, 0.0}}).release();
  list.add(raw0);
  list.add(raw1);
  ASSERT_EQ(list.getEdges().size(), 2u);

  list.clearList();
  EXPECT_TRUE(list.getEdges().empty());
}

}  // namespace
}  // namespace geos_google
