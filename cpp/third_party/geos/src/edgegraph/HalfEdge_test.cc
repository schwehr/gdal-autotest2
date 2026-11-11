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

// Unit tests for third_party/geos/src/edgegraph/HalfEdge.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/edgegraph/HalfEdge.cpp

#include "third_party/geos/include/geos/edgegraph/HalfEdge.h"

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::edgegraph::HalfEdge;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::util::IllegalArgumentException;
using ::testing::HasSubstr;
using ::testing::StartsWith;

struct HalfEdgePairDeleter {
  void operator()(HalfEdge* e) const {
    if (e != nullptr) {
      delete e->sym();
      delete e;
    }
  }
};

using HalfEdgePairPtr = std::unique_ptr<HalfEdge, HalfEdgePairDeleter>;

HalfEdgePairPtr MakeEdgePair(double x0, double y0, double x1, double y1,
                             double z0 = DoubleNotANumber,
                             double m0 = DoubleNotANumber,
                             double z1 = DoubleNotANumber,
                             double m1 = DoubleNotANumber) {
  return HalfEdgePairPtr(HalfEdge::create(CoordinateXYZM(x0, y0, z0, m0),
                                          CoordinateXYZM(x1, y1, z1, m1)));
}

class CurvedHalfEdge : public HalfEdge {
 public:
  CurvedHalfEdge(const CoordinateXYZM& orig, const CoordinateXYZM& dir_pt)
      : HalfEdge(orig), dir_pt_(dir_pt) {}

 protected:
  const CoordinateXYZM& directionPt() const override { return dir_pt_; }

 private:
  CoordinateXYZM dir_pt_;
};

struct AngularComparisonCase {
  double dx1;
  double dy1;
  double dx2;
  double dy2;
  int expected;
};

constexpr auto kAngularComparisonCases = std::to_array<AngularComparisonCase>({
    // Equal vectors
    {.dx1 = 1.0, .dy1 = 1.0, .dx2 = 1.0, .dy2 = 1.0, .expected = 0},
    // Different quadrants (NE=0, NW=1, SW=2, SE=3)
    {.dx1 = 1.0, .dy1 = 1.0, .dx2 = -1.0, .dy2 = 1.0, .expected = -1},
    {.dx1 = -1.0, .dy1 = 1.0, .dx2 = 1.0, .dy2 = 1.0, .expected = 1},
    {.dx1 = -1.0, .dy1 = -1.0, .dx2 = 1.0, .dy2 = -1.0, .expected = -1},
    {.dx1 = 1.0, .dy1 = -1.0, .dx2 = 1.0, .dy2 = 1.0, .expected = 1},
    // Quadrant boundary axes
    {.dx1 = 1.0, .dy1 = 0.0, .dx2 = 0.0, .dy2 = 1.0, .expected = -1},
    {.dx1 = 0.0, .dy1 = 1.0, .dx2 = 1.0, .dy2 = 0.0, .expected = 1},
    {.dx1 = 0.0, .dy1 = 1.0, .dx2 = -1.0, .dy2 = 0.0, .expected = -1},
    {.dx1 = -1.0, .dy1 = 0.0, .dx2 = -1.0, .dy2 = -1.0, .expected = -1},
    {.dx1 = -1.0, .dy1 = -1.0, .dx2 = 0.0, .dy2 = -1.0, .expected = -1},
    {.dx1 = 0.0, .dy1 = -1.0, .dx2 = 1.0, .dy2 = 0.0, .expected = 1},
    // Same quadrant, distinct angles
    {.dx1 = 2.0, .dy1 = 1.0, .dx2 = 1.0, .dy2 = 2.0, .expected = -1},
    {.dx1 = 1.0, .dy1 = 2.0, .dx2 = 2.0, .dy2 = 1.0, .expected = 1},
    // Same quadrant, collinear different lengths
    {.dx1 = 1.0, .dy1 = 1.0, .dx2 = 3.0, .dy2 = 3.0, .expected = 0},
});

TEST(HalfEdgeTest, CreateAndLinkInitializeSymmetricPair) {
  HalfEdgePairPtr e = MakeEdgePair(1.0, 2.0, 4.0, 6.0, 10.0, 20.0, 30.0, 40.0);
  ASSERT_NE(e, nullptr);
  ASSERT_NE(e->sym(), nullptr);

  EXPECT_TRUE(e->orig().equals4D(CoordinateXYZM(1.0, 2.0, 10.0, 20.0)));
  EXPECT_TRUE(e->dest().equals4D(CoordinateXYZM(4.0, 6.0, 30.0, 40.0)));
  EXPECT_DOUBLE_EQ(e->directionX(), 3.0);
  EXPECT_DOUBLE_EQ(e->directionY(), 4.0);

  EXPECT_EQ(e->sym()->sym(), e.get());
  EXPECT_EQ(e->next(), e->sym());
  EXPECT_EQ(e->sym()->next(), e.get());
  EXPECT_EQ(e->oNext(), e.get());
  EXPECT_EQ(e->degree(), 1);
  EXPECT_EQ(e->prev(), e->sym());
  EXPECT_EQ(e->sym()->prev(), e.get());
  EXPECT_EQ(e->next()->prev(), e.get());

  // Direct link() on stack/deque-owned HalfEdge objects.
  std::deque<HalfEdge> pool;
  pool.emplace_back(CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  pool.emplace_back(CoordinateXYZM(2.0, -3.0, 0.0, 0.0));
  pool[0].link(&pool[1]);
  EXPECT_EQ(pool[0].sym(), &pool[1]);
  EXPECT_EQ(pool[1].sym(), &pool[0]);
  EXPECT_EQ(pool[0].next(), &pool[1]);
  EXPECT_EQ(pool[1].next(), &pool[0]);
}

TEST(HalfEdgeTest, EqualsChecks2DOriginAndDestinationOnly) {
  HalfEdgePairPtr e = MakeEdgePair(1.0, 2.0, 3.0, 4.0, 10.0, 20.0, 30.0, 40.0);

  EXPECT_TRUE(e->equals(CoordinateXY(1.0, 2.0), CoordinateXY(3.0, 4.0)));
  EXPECT_FALSE(e->equals(CoordinateXY(9.0, 2.0), CoordinateXY(3.0, 4.0)));
  EXPECT_FALSE(e->equals(CoordinateXY(1.0, 2.0), CoordinateXY(3.0, 9.0)));
  EXPECT_FALSE(e->equals(CoordinateXY(3.0, 4.0), CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(e->sym()->equals(CoordinateXY(3.0, 4.0), CoordinateXY(1.0, 2.0)));
}

TEST(HalfEdgeTest, FindLocatesEdgeAroundOriginAndHandlesNullNext) {
  HalfEdgePairPtr e0 = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  HalfEdgePairPtr e1 = MakeEdgePair(0.0, 0.0, 0.0, 1.0);
  HalfEdgePairPtr e2 = MakeEdgePair(0.0, 0.0, -1.0, 0.0);
  e0->insert(e1.get());
  e0->insert(e2.get());

  EXPECT_EQ(e0->find(CoordinateXY(1.0, 0.0)), e0.get());
  EXPECT_EQ(e0->find(CoordinateXY(0.0, 1.0)), e1.get());
  EXPECT_EQ(e0->find(CoordinateXY(-1.0, 0.0)), e2.get());
  EXPECT_EQ(e0->find(CoordinateXY(0.0, -1.0)), nullptr);

  HalfEdgePairPtr broken = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  broken->sym()->setNext(nullptr);
  EXPECT_EQ(broken->find(CoordinateXY(9.0, 9.0)), nullptr);
}

TEST(HalfEdgeTest, CompareAngularDirectionCoversAllQuadrantsAndCases) {
  for (const auto& tc : kAngularComparisonCases) {
    HalfEdgePairPtr a = MakeEdgePair(0.0, 0.0, tc.dx1, tc.dy1);
    HalfEdgePairPtr b = MakeEdgePair(0.0, 0.0, tc.dx2, tc.dy2);
    EXPECT_EQ(a->compareAngularDirection(b.get()), tc.expected);
    EXPECT_EQ(a->compareTo(b.get()), tc.expected);
  }

  // Parallel translated vectors match via identical (dx, dy).
  HalfEdgePairPtr p1 = MakeEdgePair(0.0, 0.0, 2.0, 3.0);
  HalfEdgePairPtr p2 = MakeEdgePair(5.0, -1.0, 7.0, 2.0);
  EXPECT_EQ(p1->compareAngularDirection(p2.get()), 0);

  // Zero-length vector compares equal to itself, throws against non-zero.
  HalfEdgePairPtr zero = MakeEdgePair(0.0, 0.0, 0.0, 0.0);
  HalfEdgePairPtr non_zero = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  EXPECT_EQ(zero->compareAngularDirection(zero.get()), 0);
  EXPECT_THROW(zero->compareAngularDirection(non_zero.get()),
               IllegalArgumentException);
  EXPECT_THROW(non_zero->compareAngularDirection(zero.get()),
               IllegalArgumentException);
}

TEST(HalfEdgeTest, InsertMaintainsCCWOrderAcrossAllInsertionCases) {
  // 45 deg (NE)
  HalfEdgePairPtr e_ne = MakeEdgePair(0.0, 0.0, 1.0, 1.0);
  // 225 deg (SW): oNext() == this branch
  HalfEdgePairPtr e_sw = MakeEdgePair(0.0, 0.0, -1.0, -1.0);
  e_ne->insert(e_sw.get());
  EXPECT_EQ(e_ne->degree(), 2);
  EXPECT_TRUE(e_ne->isEdgesSorted());

  // 135 deg (NW): Case 1 (45 < 135 < 225)
  HalfEdgePairPtr e_nw = MakeEdgePair(0.0, 0.0, -1.0, 1.0);
  e_ne->insert(e_nw.get());

  // 315 deg (SE): Case 2 (origin-crossing wrap 225 -> 45, 315 >= 225)
  HalfEdgePairPtr e_se = MakeEdgePair(0.0, 0.0, 1.0, -1.0);
  e_ne->insert(e_se.get());

  // 0 deg (E): Case 2 (origin-crossing wrap 315 -> 45, 0 <= 45)
  HalfEdgePairPtr e_e = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  e_nw->insert(e_e.get());

  // 90 deg (N): Case 1 reached after advancing around star from e_sw
  HalfEdgePairPtr e_n = MakeEdgePair(0.0, 0.0, 0.0, 1.0);
  e_sw->insert(e_n.get());

  EXPECT_EQ(e_e->degree(), 6);
  std::vector<const HalfEdge*> star = {
      e_e.get(), e_ne.get(), e_n.get(), e_nw.get(), e_sw.get(), e_se.get(),
  };
  for (std::size_t i = 0; i < star.size(); ++i) {
    EXPECT_EQ(star[i]->oNext(), star[(i + 1) % star.size()]);
    // Calling isEdgesSorted from every starting edge exercises findLowest().
    EXPECT_TRUE(star[i]->isEdgesSorted());
  }
}

TEST(HalfEdgeTest, IsEdgesSortedDetectsDegreeOneDuplicatesAndUnsortedRings) {
  HalfEdgePairPtr single = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  EXPECT_TRUE(single->isEdgesSorted());

  // Duplicate direction produces non-strict ordering -> false.
  HalfEdgePairPtr dup_a = MakeEdgePair(0.0, 0.0, 1.0, 1.0);
  HalfEdgePairPtr dup_b = MakeEdgePair(0.0, 0.0, 2.0, 2.0);
  HalfEdgePairPtr third = MakeEdgePair(0.0, 0.0, -1.0, 1.0);
  dup_a->insert(third.get());
  dup_a->insert(dup_b.get());
  EXPECT_FALSE(dup_a->isEdgesSorted());

  // Manually reversed (clockwise) ring -> false.
  HalfEdgePairPtr e_e = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  HalfEdgePairPtr e_n = MakeEdgePair(0.0, 0.0, 0.0, 1.0);
  HalfEdgePairPtr e_w = MakeEdgePair(0.0, 0.0, -1.0, 0.0);
  e_e->sym()->setNext(e_w.get());
  e_w->sym()->setNext(e_n.get());
  e_n->sym()->setNext(e_e.get());
  EXPECT_FALSE(e_e->isEdgesSorted());
  EXPECT_FALSE(e_n->isEdgesSorted());
  EXPECT_FALSE(e_w->isEdgesSorted());
}

TEST(HalfEdgeTest, NextAndPrevFormInverseEdgeAlgebraInTriangle) {
  HalfEdgePairPtr ab = MakeEdgePair(0.0, 0.0, 2.0, 0.0);
  HalfEdgePairPtr bc = MakeEdgePair(2.0, 0.0, 1.0, 2.0);
  HalfEdgePairPtr ca = MakeEdgePair(1.0, 2.0, 0.0, 0.0);

  ab->insert(ca->sym());
  bc->insert(ab->sym());
  ca->insert(bc->sym());

  EXPECT_EQ(ab->next(), bc.get());
  EXPECT_EQ(bc->next(), ca.get());
  EXPECT_EQ(ca->next(), ab.get());

  EXPECT_EQ(ab->prev(), ca.get());
  EXPECT_EQ(bc->prev(), ab.get());
  EXPECT_EQ(ca->prev(), bc.get());

  for (const HalfEdge* e :
       {ab.get(), bc.get(), ca.get(), ab->sym(), bc->sym(), ca->sym()}) {
    EXPECT_EQ(e->next()->prev(), e);
    EXPECT_EQ(e->prev()->next(), e);
    EXPECT_EQ(e->oNext(), e->sym()->next());
  }
}

TEST(HalfEdgeTest, PrevNodeWalksDegreeTwoChainsAndReturnsNullForRings) {
  // Closed triangle: all vertices have degree 2 -> prevNode() returns nullptr.
  HalfEdgePairPtr ab = MakeEdgePair(0.0, 0.0, 2.0, 0.0);
  HalfEdgePairPtr bc = MakeEdgePair(2.0, 0.0, 1.0, 2.0);
  HalfEdgePairPtr ca = MakeEdgePair(1.0, 2.0, 0.0, 0.0);
  ab->insert(ca->sym());
  bc->insert(ab->sym());
  ca->insert(bc->sym());
  EXPECT_EQ(ab->prevNode(), nullptr);
  EXPECT_EQ(bc->prevNode(), nullptr);
  EXPECT_EQ(ca->prevNode(), nullptr);

  // Open chain: A(0,0) - B(1,0) - C(2,0) - D(3,0).
  HalfEdgePairPtr p_ab = MakeEdgePair(0.0, 0.0, 1.0, 0.0);
  HalfEdgePairPtr p_bc = MakeEdgePair(1.0, 0.0, 2.0, 0.0);
  HalfEdgePairPtr p_cd = MakeEdgePair(2.0, 0.0, 3.0, 0.0);
  p_bc->insert(p_ab->sym());
  p_cd->insert(p_bc->sym());

  EXPECT_EQ(p_ab->degree(), 1);
  EXPECT_EQ(p_bc->degree(), 2);
  EXPECT_EQ(p_cd->degree(), 2);
  EXPECT_EQ(p_cd->sym()->degree(), 1);

  EXPECT_EQ(p_ab->prevNode(), p_ab.get());
  EXPECT_EQ(p_cd->prevNode(), p_ab.get());
  EXPECT_EQ(p_ab->sym()->prevNode(), p_cd->sym());

  // Make A a degree-3 junction: prevNode from C->D still terminates at A->B.
  HalfEdgePairPtr branch1 = MakeEdgePair(0.0, 0.0, 0.0, 1.0);
  HalfEdgePairPtr branch2 = MakeEdgePair(0.0, 0.0, 0.0, -1.0);
  p_ab->insert(branch1.get());
  p_ab->insert(branch2.get());
  EXPECT_EQ(p_ab->degree(), 3);
  EXPECT_EQ(p_cd->prevNode(), p_ab.get());
}

TEST(HalfEdgeTest, StreamOperatorAndToStringNodeFormatEdges) {
  HalfEdgePairPtr e0 = MakeEdgePair(1.5, 2.5, 3.5, 4.5);
  std::ostringstream edge_oss;
  edge_oss << *e0;
  EXPECT_EQ(edge_oss.str(), "HE(1.5 2.5, 3.5 4.5)");

  std::ostringstream single_node_oss;
  HalfEdge::toStringNode(e0.get(), single_node_oss);
  EXPECT_THAT(single_node_oss.str(), StartsWith("Node( "));
  EXPECT_THAT(single_node_oss.str(), HasSubstr("  -> "));

  HalfEdgePairPtr e1 = MakeEdgePair(1.5, 2.5, 1.5, 5.5);
  HalfEdgePairPtr e2 = MakeEdgePair(1.5, 2.5, -1.5, 2.5);
  e0->insert(e1.get());
  e0->insert(e2.get());

  std::ostringstream multi_node_oss;
  HalfEdge::toStringNode(e0.get(), multi_node_oss);
  std::istringstream iss(multi_node_oss.str());
  std::string line;
  ASSERT_TRUE(std::getline(iss, line));
  EXPECT_THAT(line, StartsWith("Node( "));
  int arrow_lines = 0;
  while (std::getline(iss, line)) {
    EXPECT_THAT(line, StartsWith("  -> "));
    ++arrow_lines;
  }
  EXPECT_EQ(arrow_lines, 3);
}

TEST(HalfEdgeTest, SubclassDirectionPtControlsDirectionAndComparison) {
  std::deque<CurvedHalfEdge> edges;
  // Endpoints both go to (1, 0), while directionPt diverges along +Y vs -Y.
  edges.emplace_back(CoordinateXYZM(0.0, 0.0, 0.0, 0.0),
                     CoordinateXYZM(0.0, 1.0, 0.0, 0.0));
  edges.emplace_back(CoordinateXYZM(1.0, 0.0, 0.0, 0.0),
                     CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  edges[0].link(&edges[1]);

  edges.emplace_back(CoordinateXYZM(0.0, 0.0, 0.0, 0.0),
                     CoordinateXYZM(1.0, -1.0, 0.0, 0.0));
  edges.emplace_back(CoordinateXYZM(1.0, 0.0, 0.0, 0.0),
                     CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  edges[2].link(&edges[3]);

  EXPECT_DOUBLE_EQ(edges[0].directionX(), 0.0);
  EXPECT_DOUBLE_EQ(edges[0].directionY(), 1.0);
  EXPECT_DOUBLE_EQ(edges[2].directionX(), 1.0);
  EXPECT_DOUBLE_EQ(edges[2].directionY(), -1.0);
  // NE (0) vs SE (3) -> -1
  EXPECT_EQ(edges[0].compareAngularDirection(&edges[2]), -1);
}

}  // namespace
}  // namespace geos_google
